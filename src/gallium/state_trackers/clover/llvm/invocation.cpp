//
// Copyright 2012-2016 Francisco Jerez
// Copyright 2012-2016 Advanced Micro Devices, Inc.
// Copyright 2014-2016 Jan Vesely
// Copyright 2014-2015 Serge Martin
// Copyright 2015 Zoltan Gilian
//
// Permission is hereby granted, free of charge, to any person obtaining a
// copy of this software and associated documentation files (the "Software"),
// to deal in the Software without restriction, including without limitation
// the rights to use, copy, modify, merge, publish, distribute, sublicense,
// and/or sell copies of the Software, and to permit persons to whom the
// Software is furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
// THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR
// OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
// ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
// OTHER DEALINGS IN THE SOFTWARE.
//

#include "llvm/compat.hpp"
#include "core/compiler.hpp"
#include "util/algorithm.hpp"

#include <clang/Frontend/CompilerInstance.h>
#include <clang/Frontend/TextDiagnosticBuffer.h>
#include <clang/Frontend/TextDiagnosticPrinter.h>
#include <clang/CodeGen/CodeGenAction.h>
#include <clang/Basic/TargetInfo.h>
#include <llvm/Bitcode/BitstreamWriter.h>
#include <llvm/Bitcode/ReaderWriter.h>
#include <llvm/Linker/Linker.h>
#include <llvm/IR/DiagnosticInfo.h>
#include <llvm/IR/DiagnosticPrinter.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/IRReader/IRReader.h>
#include <llvm/Support/CodeGen.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/FormattedStream.h>
#include <llvm/Support/TargetRegistry.h>
#include <llvm/Transforms/IPO.h>
#include <llvm/Transforms/IPO/PassManagerBuilder.h>
#include <llvm/Transforms/Utils/Cloning.h>


#include <llvm/IR/DataLayout.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/Target/TargetOptions.h>

#include <llvm-c/Target.h>
#include <llvm-c/TargetMachine.h>
#include <llvm-c/Core.h>

#include "pipe/p_state.h"
#include "util/u_memory.h"
#include "util/u_math.h"

#include <iostream>
#include <iomanip>
#include <fstream>
#include <cstdio>
#include <sstream>
#include <libelf.h>
#include <gelf.h>

using namespace clover;
using namespace clover::llvm;

using ::llvm::Function;
using ::llvm::LLVMContext;
using ::llvm::Module;
using ::llvm::raw_string_ostream;

namespace {
   // XXX - Temporary hack to avoid breaking the build for the moment, will
   //       get rid of this later.
   namespace llvm {
      using namespace ::llvm;
   }

   void debug_log(const std::string &msg, const std::string &suffix) {
      const char *dbg_file = debug_get_option("CLOVER_DEBUG_FILE", "stderr");
      if (!strcmp("stderr", dbg_file)) {
         std::cerr << msg;
       } else {
        std::ofstream file(dbg_file + suffix, std::ios::app);
        file << msg;
       }
   }

   inline std::vector<std::string>
   tokenize(const std::string &s) {
      std::vector<std::string> ss;
      std::istringstream iss(s);
      std::string t;

      while (getline(iss, t, ' '))
         ss.push_back(t);

      return ss;
   }

   struct target {
      target(const std::string &s) :
         cpu(s.begin(), s.begin() + s.find_first_of("-")),
         triple(s.begin() + s.find_first_of("-") + 1, s.end()) {}

      std::string cpu;
      std::string triple;
   };

   void
   init_targets() {
      static bool targets_initialized = false;
      if (!targets_initialized) {
         LLVMInitializeAllTargets();
         LLVMInitializeAllTargetInfos();
         LLVMInitializeAllTargetMCs();
         LLVMInitializeAllAsmPrinters();
         targets_initialized = true;
      }
   }

   void
   diagnostic_handler(const llvm::DiagnosticInfo &di, void *data) {
      if (di.getSeverity() == llvm::DS_Error) {
         std::string message = *(std::string*)data;
         llvm::raw_string_ostream stream(message);
         llvm::DiagnosticPrinterRawOStream dp(stream);
         di.print(dp);
         stream.flush();
         *(std::string*)data = message;

         throw compile_error();
      }
   }

   std::unique_ptr<LLVMContext>
   create_context(std::string &r_log) {
      init_targets();
      std::unique_ptr<LLVMContext> ctx { new LLVMContext };
      ctx->setDiagnosticHandler(diagnostic_handler, &r_log);
      return ctx;
   }

   std::unique_ptr<clang::CompilerInstance>
   create_compiler_instance(const target &target,
                            const std::vector<std::string> &opts,
                            std::string &r_log) {
      std::unique_ptr<clang::CompilerInstance> c { new clang::CompilerInstance };
      clang::DiagnosticsEngine diag { new clang::DiagnosticIDs,
            new clang::DiagnosticOptions, new clang::TextDiagnosticBuffer };

      // Parse the compiler options.  A file name should be present at the end
      // and must have the .cl extension in order for the CompilerInvocation
      // class to recognize it as an OpenCL source file.
      const std::vector<const char *> copts =
         map(std::mem_fn(&std::string::c_str), opts);

      if (!clang::CompilerInvocation::CreateFromArgs(
             c->getInvocation(), copts.data(), copts.data() + copts.size(), diag))
         throw error(CL_INVALID_COMPILER_OPTIONS);

      c->getTargetOpts().CPU = target.cpu;
      c->getTargetOpts().Triple = target.triple;
      c->getLangOpts().NoBuiltin = true;

      // This is a workaround for a Clang bug which causes the number
      // of warnings and errors to be printed to stderr.
      // http://www.llvm.org/bugs/show_bug.cgi?id=19735
      c->getDiagnosticOpts().ShowCarets = false;

      compat::set_lang_defaults(c->getInvocation(), c->getLangOpts(),
                                clang::IK_OpenCL, ::llvm::Triple(target.triple),
                                c->getPreprocessorOpts(),
                                clang::LangStandard::lang_opencl11);

      c->createDiagnostics(new clang::TextDiagnosticPrinter(
                              *new raw_string_ostream(r_log),
                              &c->getDiagnosticOpts(), true));

      c->setTarget(clang::TargetInfo::CreateTargetInfo(
                           c->getDiagnostics(), c->getInvocation().TargetOpts));

      return c;
   }

   llvm::Module *
   compile_llvm(LLVMContext &ctx, clang::CompilerInstance &c,
                const std::string &source, const header_map &headers,
                const std::string &name, const std::string &target,
                const std::string &opts, std::string &r_log) {
      clang::EmitLLVMOnlyAction act(&ctx);

      c.getFrontendOpts().ProgramAction = clang::frontend::EmitLLVMOnly;
      c.getHeaderSearchOpts().UseBuiltinIncludes = true;
      c.getHeaderSearchOpts().UseStandardSystemIncludes = true;
      c.getHeaderSearchOpts().ResourceDir = CLANG_RESOURCE_DIR;

      // Add libclc generic search path
      c.getHeaderSearchOpts().AddPath(LIBCLC_INCLUDEDIR,
                                      clang::frontend::Angled,
                                      false, false
                                      );

      // Add libclc include
      c.getPreprocessorOpts().Includes.push_back("clc/clc.h");

      // clc.h requires that this macro be defined:
      c.getPreprocessorOpts().addMacroDef("cl_clang_storage_class_specifiers");

      c.getPreprocessorOpts().addRemappedFile(name,
                                              llvm::MemoryBuffer::getMemBuffer(source).release());

      if (headers.size()) {
         const std::string tmp_header_path = "/tmp/clover/";

         c.getHeaderSearchOpts().AddPath(tmp_header_path,
                                         clang::frontend::Angled,
                                         false, false
                                         );

         for (header_map::const_iterator it = headers.begin();
              it != headers.end(); ++it) {
            const std::string path = tmp_header_path + std::string(it->first);
            c.getPreprocessorOpts().addRemappedFile(path,
                    llvm::MemoryBuffer::getMemBuffer(it->second.c_str()).release());
         }
      }

      // Tell clang to link this file before performing any
      // optimizations.  This is required so that we can replace calls
      // to the OpenCL C barrier() builtin with calls to target
      // intrinsics that have the noduplicate attribute.  This
      // attribute will prevent Clang from creating illegal uses of
      // barrier() (e.g. Moving barrier() inside a conditional that is
      // no executed by all threads) during its optimizaton passes.
      compat::add_link_bitcode_file(c.getCodeGenOpts(),
                                    LIBCLC_LIBEXECDIR + target + ".bc");

      // Compile the code
      if (!c.ExecuteAction(act))
         throw compile_error();

      return act.takeModule().release();
   }

   std::vector<llvm::Function *>
   find_kernels(llvm::Module *mod) {
      std::vector<llvm::Function *> kernels;
#if HAVE_LLVM >= 0x0309
      auto &list = mod->getFunctionList();
      for_each(list.begin(), list.end(), [&](llvm::Function &f){
         if (f.getMetadata("kernel_arg_type"))
           kernels.push_back(&f);
      });
      return kernels;
#endif
      const llvm::NamedMDNode *kernel_node =
                                 mod->getNamedMetadata("opencl.kernels");
      // This means there are no kernels in the program.  The spec does not
      // require that we return an error here, but there will be an error if
      // the user tries to pass this program to a clCreateKernel() call.
      if (!kernel_node) {
         return std::vector<llvm::Function *>();
      }

      kernels.reserve(kernel_node->getNumOperands());
      for (unsigned i = 0; i < kernel_node->getNumOperands(); ++i) {
         kernels.push_back(llvm::mdconst::dyn_extract<llvm::Function>(
                                    kernel_node->getOperand(i)->getOperand(0)));
      }
      return kernels;
   }

   void
   optimize(llvm::Module *mod, unsigned optimization_level) {
      compat::pass_manager PM;

      compat::add_data_layout_pass(PM);

      // By default, the function internalizer pass will look for a function
      // called "main" and then mark all other functions as internal.  Marking
      // functions as internal enables the optimizer to perform optimizations
      // like function inlining and global dead-code elimination.
      //
      // When there is no "main" function in a module, the internalize pass will
      // treat the module like a library, and it won't internalize any functions.
      // Since there is no "main" function in our kernels, we need to tell
      // the internalizer pass that this module is not a library by passing a
      // list of kernel functions to the internalizer.  The internalizer will
      // treat the functions in the list as "main" functions and internalize
      // all of the other functions.
      compat::add_internalize_pass(PM, map(std::mem_fn(&Function::getName),
                                           find_kernels(mod)));

      llvm::PassManagerBuilder PMB;
      PMB.OptLevel = optimization_level;
      PMB.LibraryInfo = new compat::target_library_info(
         llvm::Triple(mod->getTargetTriple()));
      PMB.populateModulePassManager(PM);
      PM.run(*mod);
   }

   // Kernel metadata

   struct kernel_arg_md {
      llvm::StringRef type_name;
      llvm::StringRef access_qual;
      kernel_arg_md(llvm::StringRef type_name_, llvm::StringRef access_qual_):
         type_name(type_name_), access_qual(access_qual_) {}
   };
#if HAVE_LLVM >= 0x0309
   std::vector<kernel_arg_md>
   get_kernel_arg_md(const llvm::Function *kernel_func) {

      size_t num_args = kernel_func->getArgumentList().size();

      auto aq = kernel_func->getMetadata("kernel_arg_access_qual");
      auto ty = kernel_func->getMetadata("kernel_arg_type");

      std::vector<kernel_arg_md> res;
      res.reserve(num_args);
      for (size_t i = 0; i < num_args; ++i) {
         res.push_back(kernel_arg_md(
            llvm::cast<llvm::MDString>(ty->getOperand(i))->getString(),
            llvm::cast<llvm::MDString>(aq->getOperand(i))->getString()));
      }

      return res;
   }

#else

   const llvm::MDNode *
   get_kernel_metadata(const llvm::Function *kernel_func) {
      auto mod = kernel_func->getParent();
      auto kernels_node = mod->getNamedMetadata("opencl.kernels");
      if (!kernels_node) {
         return nullptr;
      }

      const llvm::MDNode *kernel_node = nullptr;
      for (unsigned i = 0; i < kernels_node->getNumOperands(); ++i) {
         auto func = llvm::mdconst::dyn_extract<llvm::Function>(
               kernels_node->getOperand(i)->getOperand(0));
         if (func == kernel_func) {
            kernel_node = kernels_node->getOperand(i);
            break;
         }
      }

      return kernel_node;
   }

   llvm::MDNode*
   node_from_op_checked(const llvm::MDOperand &md_operand,
                        llvm::StringRef expect_name,
                        unsigned expect_num_args)
   {
      auto node = llvm::cast<llvm::MDNode>(md_operand);
      assert(node->getNumOperands() == expect_num_args &&
             "Wrong number of operands.");

      auto str_node = llvm::cast<llvm::MDString>(node->getOperand(0));
      assert(str_node->getString() == expect_name &&
             "Wrong metadata node name.");

      return node;
   }

   std::vector<kernel_arg_md>
   get_kernel_arg_md(const llvm::Function *kernel_func) {
      auto num_args = kernel_func->getArgumentList().size();

      auto kernel_node = get_kernel_metadata(kernel_func);
      auto aq = node_from_op_checked(kernel_node->getOperand(2),
                                     "kernel_arg_access_qual", num_args + 1);
      auto ty = node_from_op_checked(kernel_node->getOperand(3),
                                     "kernel_arg_type", num_args + 1);

      std::vector<kernel_arg_md> res;
      res.reserve(num_args);
      for (unsigned i = 0; i < num_args; ++i) {
         res.push_back(kernel_arg_md(
            llvm::cast<llvm::MDString>(ty->getOperand(i+1))->getString(),
            llvm::cast<llvm::MDString>(aq->getOperand(i+1))->getString()));
      }

      return res;
   }
#endif

   std::vector<module::argument>
   get_kernel_args(const llvm::Module *mod, const std::string &kernel_name,
                   const clang::CompilerInstance &c) {
      std::vector<module::argument> args;
      const auto address_spaces = c.getTarget().getAddressSpaceMap();
      llvm::Function *kernel_func = mod->getFunction(kernel_name);
      assert(kernel_func && "Kernel name not found in module.");
      auto arg_md = get_kernel_arg_md(kernel_func);

      llvm::DataLayout TD(mod);
      llvm::Type *size_type =
         TD.getSmallestLegalIntType(mod->getContext(), sizeof(cl_uint) * 8);

      for (const auto &arg: kernel_func->args()) {

         llvm::Type *arg_type = arg.getType();
         const unsigned arg_store_size = TD.getTypeStoreSize(arg_type);

         // OpenCL 1.2 specification, Ch. 6.1.5: "A built-in data
         // type that is not a power of two bytes in size must be
         // aligned to the next larger power of two".  We need this
         // alignment for three element vectors, which have
         // non-power-of-2 store size.
         const unsigned arg_api_size = util_next_power_of_two(arg_store_size);

         llvm::Type *target_type = arg_type->isIntegerTy() ?
               TD.getSmallestLegalIntType(mod->getContext(), arg_store_size * 8)
               : arg_type;
         unsigned target_size = TD.getTypeStoreSize(target_type);
         unsigned target_align = TD.getABITypeAlignment(target_type);

         llvm::StringRef type_name = arg_md[arg.getArgNo()].type_name;
         llvm::StringRef access_qual = arg_md[arg.getArgNo()].access_qual;

         // Image
         const bool is_image2d = type_name == "image2d_t";
         const bool is_image3d = type_name == "image3d_t";
         if (is_image2d || is_image3d) {
            const bool is_write_only = access_qual == "write_only";
            const bool is_read_only = access_qual == "read_only";

            enum module::argument::type marg_type;
            if (is_image2d && is_read_only) {
               marg_type = module::argument::image2d_rd;
            } else if (is_image2d && is_write_only) {
               marg_type = module::argument::image2d_wr;
            } else if (is_image3d && is_read_only) {
               marg_type = module::argument::image3d_rd;
            } else if (is_image3d && is_write_only) {
               marg_type = module::argument::image3d_wr;
            } else {
               assert(0 && "Wrong image access qualifier");
            }

            args.push_back(module::argument(marg_type,
                                            arg_store_size, target_size,
                                            target_align,
                                            module::argument::zero_ext));
            continue;
         }

         // Image size implicit argument
         if (type_name == "__llvm_image_size") {
            args.push_back(module::argument(module::argument::scalar,
                                            sizeof(cl_uint),
                                            TD.getTypeStoreSize(size_type),
                                            TD.getABITypeAlignment(size_type),
                                            module::argument::zero_ext,
                                            module::argument::image_size));
            continue;
         }

         // Image format implicit argument
         if (type_name == "__llvm_image_format") {
            args.push_back(module::argument(module::argument::scalar,
                                            sizeof(cl_uint),
                                            TD.getTypeStoreSize(size_type),
                                            TD.getABITypeAlignment(size_type),
                                            module::argument::zero_ext,
                                            module::argument::image_format));
            continue;
         }

         // Other types
         if (llvm::isa<llvm::PointerType>(arg_type) && arg.hasByValAttr()) {
            arg_type =
                  llvm::dyn_cast<llvm::PointerType>(arg_type)->getElementType();
         }

         if (arg_type->isPointerTy()) {
            unsigned address_space = llvm::cast<llvm::PointerType>(arg_type)->getAddressSpace();
            if (address_space == address_spaces[clang::LangAS::opencl_local
                                                     - clang::LangAS::Offset]) {
               args.push_back(module::argument(module::argument::local,
                                               arg_api_size, target_size,
                                               target_align,
                                               module::argument::zero_ext));
            } else {
               // XXX: Correctly handle constant address space.  There is no
               // way for r600g to pass a handle for constant buffers back
               // to clover like it can for global buffers, so
               // creating constant arguments will break r600g.  For now,
               // continue treating constant buffers as global buffers
               // until we can come up with a way to create handles for
               // constant buffers.
               args.push_back(module::argument(module::argument::global,
                                               arg_api_size, target_size,
                                               target_align,
                                               module::argument::zero_ext));
           }

         } else {
            llvm::AttributeSet attrs = kernel_func->getAttributes();
            enum module::argument::ext_type ext_type =
                  (attrs.hasAttribute(arg.getArgNo() + 1,
                                     llvm::Attribute::SExt) ?
                   module::argument::sign_ext :
                   module::argument::zero_ext);

            args.push_back(
               module::argument(module::argument::scalar, arg_api_size,
                                target_size, target_align, ext_type));
         }
      }

      // Append implicit arguments.  XXX - The types, ordering and
      // vector size of the implicit arguments should depend on the
      // target according to the selected calling convention.
      args.push_back(
         module::argument(module::argument::scalar, sizeof(cl_uint),
                          TD.getTypeStoreSize(size_type),
                          TD.getABITypeAlignment(size_type),
                          module::argument::zero_ext,
                          module::argument::grid_dimension));

      args.push_back(
         module::argument(module::argument::scalar, sizeof(cl_uint),
                          TD.getTypeStoreSize(size_type),
                          TD.getABITypeAlignment(size_type),
                          module::argument::zero_ext,
                          module::argument::grid_offset));

      return args;
   }

   module
   build_module_llvm(llvm::Module *mod,
                     const clang::CompilerInstance &c) {

      module m;
      struct pipe_llvm_program_header header;

      llvm::SmallVector<char, 1024> llvm_bitcode;
      llvm::raw_svector_ostream bitcode_ostream(llvm_bitcode);
      llvm::BitstreamWriter writer(llvm_bitcode);
      llvm::WriteBitcodeToFile(mod, bitcode_ostream);
#if HAVE_LLVM < 0x0308
      bitcode_ostream.flush();
#endif

      const std::vector<llvm::Function *> kernels = find_kernels(mod);
      for (unsigned i = 0; i < kernels.size(); ++i) {
         std::string kernel_name = kernels[i]->getName();
         std::vector<module::argument> args =
               get_kernel_args(mod, kernel_name, c);

         m.syms.push_back(module::symbol(kernel_name, 0, i, args ));
      }

      header.num_bytes = llvm_bitcode.size();
      std::vector<char> data;
      data.insert(data.end(), (char*)(&header),
                              (char*)(&header) + sizeof(header));
      data.insert(data.end(), llvm_bitcode.begin(),
                                  llvm_bitcode.end());
      m.secs.push_back(module::section(0, module::section::text,
                                       header.num_bytes, data));

      return m;
   }

   void
   emit_code(LLVMTargetMachineRef tm, LLVMModuleRef mod,
             LLVMCodeGenFileType file_type,
             LLVMMemoryBufferRef *out_buffer,
             std::string &r_log) {
      LLVMBool err;
      char *err_message = NULL;

      err = LLVMTargetMachineEmitToMemoryBuffer(tm, mod, file_type,
                                                &err_message, out_buffer);

      if (err) {
         r_log = std::string(err_message);
      }

      LLVMDisposeMessage(err_message);

      if (err) {
         throw compile_error();
      }
   }

   std::vector<char>
   compile_native(const llvm::Module *mod, const target &t, bool dump_asm,
                  std::string &r_log) {

      std::string log;
      LLVMTargetRef target;
      char *error_message;
      LLVMMemoryBufferRef out_buffer;
      unsigned buffer_size;
      const char *buffer_data;
      LLVMModuleRef mod_ref = wrap(mod);

      if (LLVMGetTargetFromTriple(t.triple.c_str(), &target, &error_message)) {
         r_log = std::string(error_message);
         LLVMDisposeMessage(error_message);
         throw compile_error();
      }

      LLVMTargetMachineRef tm = LLVMCreateTargetMachine(
            target, t.triple.c_str(), t.cpu.c_str(), "",
            LLVMCodeGenLevelDefault, LLVMRelocDefault, LLVMCodeModelDefault);

      if (!tm) {
         r_log = "Could not create TargetMachine: " + t.triple;
         throw compile_error();
      }

      if (dump_asm) {
         LLVMSetTargetMachineAsmVerbosity(tm, true);
#if HAVE_LLVM >= 0x0308
         LLVMModuleRef debug_mod = wrap(llvm::CloneModule(mod).release());
#else
         LLVMModuleRef debug_mod = wrap(llvm::CloneModule(mod));
#endif
         emit_code(tm, debug_mod, LLVMAssemblyFile, &out_buffer, r_log);
         buffer_size = LLVMGetBufferSize(out_buffer);
         buffer_data = LLVMGetBufferStart(out_buffer);
         debug_log(std::string(buffer_data, buffer_size), ".asm");

         LLVMSetTargetMachineAsmVerbosity(tm, false);
         LLVMDisposeMemoryBuffer(out_buffer);
         LLVMDisposeModule(debug_mod);
      }

      emit_code(tm, mod_ref, LLVMObjectFile, &out_buffer, r_log);

      buffer_size = LLVMGetBufferSize(out_buffer);
      buffer_data = LLVMGetBufferStart(out_buffer);

      std::vector<char> code(buffer_data, buffer_data + buffer_size);

      LLVMDisposeMemoryBuffer(out_buffer);
      LLVMDisposeTargetMachine(tm);

      return code;
   }

   std::map<std::string, unsigned>
   get_kernel_offsets(std::vector<char> &code,
                      const std::vector<llvm::Function *> &kernels,
                      std::string &r_log) {

      // One of the libelf implementations
      // (http://www.mr511.de/software/english.htm) requires calling
      // elf_version() before elf_memory().
      //
      elf_version(EV_CURRENT);

      Elf *elf = elf_memory(&code[0], code.size());
      size_t section_str_index;
      elf_getshdrstrndx(elf, &section_str_index);
      Elf_Scn *section = NULL;
      Elf_Scn *symtab = NULL;
      GElf_Shdr symtab_header;

      // Find the symbol table
      try {
         while ((section = elf_nextscn(elf, section))) {
            const char *name;
            if (gelf_getshdr(section, &symtab_header) != &symtab_header) {
               r_log = "Failed to read ELF section header.";
               throw compile_error();
            }
            name = elf_strptr(elf, section_str_index, symtab_header.sh_name);
           if (!strcmp(name, ".symtab")) {
               symtab = section;
               break;
           }
         }
         if (!symtab) {
            r_log = "Unable to find symbol table.";
            throw compile_error();
         }
      } catch (compile_error &e) {
         elf_end(elf);
         throw e;
      }


      // Extract symbol information from the table
      Elf_Data *symtab_data = NULL;
      GElf_Sym *symbol;
      GElf_Sym s;

      std::map<std::string, unsigned> kernel_offsets;
      symtab_data = elf_getdata(symtab, symtab_data);

      // Determine the offsets for each kernel
      for (int i = 0; (symbol = gelf_getsym(symtab_data, i, &s)); i++) {
         char *name = elf_strptr(elf, symtab_header.sh_link, symbol->st_name);
         for (std::vector<llvm::Function*>::const_iterator it = kernels.begin(),
              e = kernels.end(); it != e; ++it) {
            llvm::Function *f = *it;
            if (f->getName() == std::string(name))
               kernel_offsets[f->getName()] = symbol->st_value;
         }
      }
      elf_end(elf);
      return kernel_offsets;
   }

   module
   build_module_native(std::vector<char> &code,
                       llvm::Module *mod,
                       const clang::CompilerInstance &c,
                       std::string &r_log) {

      const std::vector<llvm::Function *> kernels = find_kernels(mod);

      std::map<std::string, unsigned> kernel_offsets =
            get_kernel_offsets(code, kernels, r_log);

      // Begin building the clover module
      module m;
      struct pipe_llvm_program_header header;

      // Store the generated ELF binary in the module's text section.
      header.num_bytes = code.size();
      std::vector<char> data;
      data.insert(data.end(), (char*)(&header),
                              (char*)(&header) + sizeof(header));
      data.insert(data.end(), code.begin(), code.end());
      m.secs.push_back(module::section(0, module::section::text,
                                       header.num_bytes, data));

      for (std::map<std::string, unsigned>::iterator i = kernel_offsets.begin(),
           e = kernel_offsets.end(); i != e; ++i) {
         std::vector<module::argument> args = get_kernel_args(mod, i->first, c);
         m.syms.push_back(module::symbol(i->first, 0, i->second, args ));
      }

      return m;
   }

#define DBG_CLC  (1 << 0)
#define DBG_LLVM (1 << 1)
#define DBG_ASM  (1 << 2)

   unsigned
   get_debug_flags() {
      static const struct debug_named_value debug_options[] = {
         {"clc", DBG_CLC, "Dump the OpenCL C code for all kernels."},
         {"llvm", DBG_LLVM, "Dump the generated LLVM IR for all kernels."},
         {"asm", DBG_ASM, "Dump kernel assembly code for targets specifying "
          "PIPE_SHADER_IR_NATIVE"},
         DEBUG_NAMED_VALUE_END // must be last
      };
      static const unsigned debug_flags =
         debug_get_flags_option("CLOVER_DEBUG", debug_options, 0);

      return debug_flags;
   }

} // End anonymous namespace

module
clover::compile_program_llvm(const std::string &source,
                             const header_map &headers,
                             enum pipe_shader_ir ir,
                             const std::string &target,
                             const std::string &opts,
                             std::string &r_log) {
   if (get_debug_flags() & DBG_CLC)
      debug_log("// Build options: " + opts + '\n' + source, ".cl");

   auto ctx = create_context(r_log);
   // The input file name must have the .cl extension in order for the
   // CompilerInvocation class to recognize it as an OpenCL source file.
   const auto c = create_compiler_instance(target, tokenize(opts + " input.cl"),
                                           r_log);
   Module *mod = compile_llvm(*ctx, *c, source, headers, "input.cl",
                              target, opts, r_log);

   optimize(mod, c->getCodeGenOpts().OptimizationLevel);

   if (get_debug_flags() & DBG_LLVM) {
      std::string log;
      raw_string_ostream s_log(log);
      mod->print(s_log, NULL);
      s_log.flush();
      debug_log(log, ".ll");
    }

   module m;
   // Build the clover::module
   switch (ir) {
      case PIPE_SHADER_IR_NIR:
      case PIPE_SHADER_IR_TGSI:
         //XXX: Handle TGSI, NIR
         assert(0);
         m = module();
         break;
      case PIPE_SHADER_IR_LLVM:
         m = build_module_llvm(mod, *c);
         break;
      case PIPE_SHADER_IR_NATIVE: {
         std::vector<char> code = compile_native(mod, target,
                                                 get_debug_flags() & DBG_ASM,
                                                 r_log);
         m = build_module_native(code, mod, *c, r_log);
         break;
      }
   }
   // The user takes ownership of the module.
   delete mod;
   return m;
}
