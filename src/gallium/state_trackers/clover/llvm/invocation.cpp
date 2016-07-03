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
#include "llvm/metadata.hpp"
#include "llvm/util.hpp"
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

using ::llvm::cast;
using ::llvm::dyn_cast;
using ::llvm::Function;
using ::llvm::isa;
using ::llvm::LLVMContext;
using ::llvm::Module;
using ::llvm::raw_string_ostream;
using ::llvm::TargetMachine;

namespace {
   // XXX - Temporary hack to avoid breaking the build for the moment, will
   //       get rid of this later.
   namespace llvm {
      using namespace ::llvm;
   }

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
   diagnostic_handler(const ::llvm::DiagnosticInfo &di, void *data) {
      if (di.getSeverity() == ::llvm::DS_Error) {
         raw_string_ostream os { *reinterpret_cast<std::string *>(data) };
         ::llvm::DiagnosticPrinterRawOStream printer { os };
         di.print(printer);
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

   std::unique_ptr<Module>
   compile(LLVMContext &ctx, clang::CompilerInstance &c,
           const std::string &name, const std::string &source,
           const header_map &headers, const std::string &target,
           const std::string &opts, std::string &r_log) {
      c.getFrontendOpts().ProgramAction = clang::frontend::EmitLLVMOnly;
      c.getHeaderSearchOpts().UseBuiltinIncludes = true;
      c.getHeaderSearchOpts().UseStandardSystemIncludes = true;
      c.getHeaderSearchOpts().ResourceDir = CLANG_RESOURCE_DIR;

      // Add libclc generic search path
      c.getHeaderSearchOpts().AddPath(LIBCLC_INCLUDEDIR,
                                      clang::frontend::Angled,
                                      false, false);

      // Add libclc include
      c.getPreprocessorOpts().Includes.push_back("clc/clc.h");

      // clc.h requires that this macro be defined:
      c.getPreprocessorOpts().addMacroDef("cl_clang_storage_class_specifiers");
      c.getPreprocessorOpts().addRemappedFile(
              name, ::llvm::MemoryBuffer::getMemBuffer(source).release());

      if (headers.size()) {
         const std::string tmp_header_path = "/tmp/clover/";

         c.getHeaderSearchOpts().AddPath(tmp_header_path,
                                         clang::frontend::Angled,
                                         false, false);

         for (const auto &header : headers)
            c.getPreprocessorOpts().addRemappedFile(
               tmp_header_path + header.first,
               ::llvm::MemoryBuffer::getMemBuffer(header.second).release());
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
      clang::EmitLLVMOnlyAction act(&ctx);
      if (!c.ExecuteAction(act))
         throw compile_error();

      return act.takeModule();
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
   optimize(Module &mod, unsigned optimization_level) {
      compat::pass_manager pm;

      compat::add_data_layout_pass(pm);

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
      compat::add_internalize_pass(pm, map(std::mem_fn(&Function::getName),
                                           find_kernels(&mod)));

      ::llvm::PassManagerBuilder pmb;
      pmb.OptLevel = optimization_level;
      pmb.LibraryInfo = new compat::target_library_info(
         ::llvm::Triple(mod.getTargetTriple()));
      pmb.populateModulePassManager(pm);
      pm.run(mod);
   }

   enum module::argument::type
   get_image_type(const std::string &type,
                  const std::string &qual) {
      if (type == "image2d_t" && qual == "read_only")
         return module::argument::image2d_rd;
      else if (type == "image2d_t" && qual == "write_only")
         return module::argument::image2d_wr;
      else if (type == "image3d_t" && qual == "read_only")
         return module::argument::image3d_rd;
      else if (type == "image3d_t" && qual == "write_only")
         return module::argument::image3d_wr;
      else
         unreachable("Unknown image type");
   }

   std::vector<module::argument>
   make_kernel_args(const Module &mod, const std::string &kernel_name,
                    const clang::CompilerInstance &c) {
      std::vector<module::argument> args;
      const auto address_spaces = c.getTarget().getAddressSpaceMap();
      const Function &f = *mod.getFunction(kernel_name);
      ::llvm::DataLayout dl(&mod);
      const auto size_type =
         dl.getSmallestLegalIntType(mod.getContext(), sizeof(cl_uint) * 8);

      for (const auto &arg : f.args()) {
         const auto arg_type = arg.getType();

         // OpenCL 1.2 specification, Ch. 6.1.5: "A built-in data
         // type that is not a power of two bytes in size must be
         // aligned to the next larger power of two".  We need this
         // alignment for three element vectors, which have
         // non-power-of-2 store size.
         const unsigned arg_store_size = dl.getTypeStoreSize(arg_type);
         const unsigned arg_api_size = util_next_power_of_two(arg_store_size);

         const auto target_type = !arg_type->isIntegerTy() ? arg_type :
            dl.getSmallestLegalIntType(mod.getContext(), arg_store_size * 8);
         const unsigned target_size = dl.getTypeStoreSize(target_type);
         const unsigned target_align = dl.getABITypeAlignment(target_type);

         const auto type_name = get_argument_metadata(f, arg,
                                                      "kernel_arg_type");

         if (type_name == "image2d_t" || type_name == "image3d_t") {
            // Image.
            const auto access_qual = get_argument_metadata(
               f, arg, "kernel_arg_access_qual");
            args.emplace_back(get_image_type(type_name, access_qual),
                              arg_store_size, target_size,
                              target_align, module::argument::zero_ext);

         } else if (type_name == "__llvm_image_size") {
            // Image size implicit argument.
            args.emplace_back(module::argument::scalar, sizeof(cl_uint),
                              dl.getTypeStoreSize(size_type),
                              dl.getABITypeAlignment(size_type),
                              module::argument::zero_ext,
                              module::argument::image_size);

         } else if (type_name == "__llvm_image_format") {
            // Image format implicit argument.
            args.emplace_back(module::argument::scalar, sizeof(cl_uint),
                              dl.getTypeStoreSize(size_type),
                              dl.getABITypeAlignment(size_type),
                              module::argument::zero_ext,
                              module::argument::image_format);

         } else {
            // Other types.
            const auto actual_type =
               isa<::llvm::PointerType>(arg_type) && arg.hasByValAttr() ?
               cast<::llvm::PointerType>(arg_type)->getElementType() : arg_type;

            if (actual_type->isPointerTy()) {
               const unsigned address_space =
                  cast<::llvm::PointerType>(actual_type)->getAddressSpace();

               if (address_space == address_spaces[clang::LangAS::opencl_local
                                                   - clang::LangAS::Offset]) {
                  args.emplace_back(module::argument::local, arg_api_size,
                                    target_size, target_align,
                                    module::argument::zero_ext);
               } else {
                  // XXX: Correctly handle constant address space.  There is no
                  // way for r600g to pass a handle for constant buffers back
                  // to clover like it can for global buffers, so
                  // creating constant arguments will break r600g.  For now,
                  // continue treating constant buffers as global buffers
                  // until we can come up with a way to create handles for
                  // constant buffers.
                  args.emplace_back(module::argument::global, arg_api_size,
                                    target_size, target_align,
                                    module::argument::zero_ext);
               }

            } else {
               const bool needs_sign_ext = f.getAttributes().hasAttribute(
                  arg.getArgNo() + 1, ::llvm::Attribute::SExt);

               args.emplace_back(module::argument::scalar, arg_api_size,
                                 target_size, target_align,
                                 (needs_sign_ext ? module::argument::sign_ext :
                                  module::argument::zero_ext));
            }
         }
      }

      // Append implicit arguments.  XXX - The types, ordering and
      // vector size of the implicit arguments should depend on the
      // target according to the selected calling convention.
      args.emplace_back(module::argument::scalar, sizeof(cl_uint),
                        dl.getTypeStoreSize(size_type),
                        dl.getABITypeAlignment(size_type),
                        module::argument::zero_ext,
                        module::argument::grid_dimension);

      args.emplace_back(module::argument::scalar, sizeof(cl_uint),
                        dl.getTypeStoreSize(size_type),
                        dl.getABITypeAlignment(size_type),
                        module::argument::zero_ext,
                        module::argument::grid_offset);

      return args;
   }

   module::section
   make_text_section(const std::vector<char> &code) {
      const pipe_llvm_program_header header { uint32_t(code.size()) };
      module::section text { 0, module::section::text, header.num_bytes, {} };

      text.data.insert(text.data.end(), reinterpret_cast<const char *>(&header),
                       reinterpret_cast<const char *>(&header) + sizeof(header));
      text.data.insert(text.data.end(), code.begin(), code.end());

      return text;
   }

   module
   build_module_common(const Module &mod,
                       const std::vector<char> &code,
                       const std::map<std::string,
                                      unsigned> &offsets,
                       const clang::CompilerInstance &c) {
      module m;

      for (const auto &name : map(std::mem_fn(&Function::getName),
                                  find_kernels(const_cast<Module *>(&mod)))) {
         if (offsets.count(name))
            m.syms.emplace_back(name, 0, offsets.at(name),
                                make_kernel_args(mod, name, c));
      }

      m.secs.push_back(make_text_section(code));
      return m;
   }

   module
   build_module_llvm(llvm::Module *mod,
                     const clang::CompilerInstance &c) {
      llvm::SmallVector<char, 1024> llvm_bitcode;
      llvm::raw_svector_ostream bitcode_ostream(llvm_bitcode);
      llvm::BitstreamWriter writer(llvm_bitcode);
      llvm::WriteBitcodeToFile(mod, bitcode_ostream);
#if HAVE_LLVM < 0x0308
      bitcode_ostream.flush();
#endif

      std::map<std::string, unsigned> offsets;
      unsigned i = 0;

      for (const auto &name : map(std::mem_fn(&::llvm::Function::getName),
                                  find_kernels(mod)))
         offsets[name] = i++;

      return build_module_common(*mod, { llvm_bitcode.begin(),
                                         llvm_bitcode.end() },
                                 offsets, c);
   }

   std::vector<char>
   emit_code(::llvm::Module &mod, const target &target,
             TargetMachine::CodeGenFileType ft,
             std::string &r_log) {
      std::string err;
      auto t = ::llvm::TargetRegistry::lookupTarget(target.triple, err);
      if (!t)
         fail(r_log, compile_error(), err);

      std::unique_ptr<TargetMachine> tm {
         t->createTargetMachine(target.triple, target.cpu, "", {},
                                compat::default_reloc_model,
                                ::llvm::CodeModel::Default,
                                ::llvm::CodeGenOpt::Default) };
      if (!tm)
         fail(r_log, compile_error(),
              "Could not create TargetMachine: " + target.triple);

      ::llvm::SmallVector<char, 1024> data;

      {
         compat::pass_manager pm;
         ::llvm::raw_svector_ostream os { data };
         compat::raw_ostream_to_emit_file fos { os };

         mod.setDataLayout(compat::get_data_layout(*tm));
         tm->Options.MCOptions.AsmVerbose =
            (ft == TargetMachine::CGFT_AssemblyFile);

         if (tm->addPassesToEmitFile(pm, fos, ft))
            fail(r_log, compile_error(), "TargetMachine can't emit this file");

         pm.run(mod);
      }

      return { data.begin(), data.end() };
   }

   std::vector<char>
   compile_native(llvm::Module *mod, const target &target,
                  std::string &r_log) {
      if (has_flag(debug::native)) {
         std::unique_ptr<llvm::Module> cmod { CloneModule(mod) };
         debug::log(".asm", as_string(
                       emit_code(*cmod, target,
                                 TargetMachine::CGFT_AssemblyFile, r_log)));
      }

      return emit_code(*mod, target, TargetMachine::CGFT_ObjectFile, r_log);
   }

   namespace elf {
      std::unique_ptr<Elf, int (*)(Elf *)>
      get(const std::vector<char> &code) {
         // One of the libelf implementations
         // (http://www.mr511.de/software/english.htm) requires calling
         // elf_version() before elf_memory().
         elf_version(EV_CURRENT);
         return { elf_memory(const_cast<char *>(code.data()), code.size()),
                  elf_end };
      }

      Elf_Scn *
      get_symbol_table(Elf *elf) {
         size_t section_str_index;
         elf_getshdrstrndx(elf, &section_str_index);

         for (Elf_Scn *s = elf_nextscn(elf, NULL); s; s = elf_nextscn(elf, s)) {
            GElf_Shdr header;
            if (gelf_getshdr(s, &header) != &header)
               return nullptr;

            if (!std::strcmp(elf_strptr(elf, section_str_index, header.sh_name),
                             ".symtab"))
               return s;
         }

         return nullptr;
      }

      std::map<std::string, unsigned>
      get_symbol_offsets(Elf *elf, Elf_Scn *symtab) {
         Elf_Data *const symtab_data = elf_getdata(symtab, NULL);
         GElf_Shdr header;
         if (gelf_getshdr(symtab, &header) != &header)
            return {};

         std::map<std::string, unsigned> symbol_offsets;
         GElf_Sym symbol;
         unsigned i = 0;

         while (GElf_Sym *s = gelf_getsym(symtab_data, i++, &symbol)) {
            const char *name = elf_strptr(elf, header.sh_link, s->st_name);
            symbol_offsets[name] = s->st_value;
         }

         return symbol_offsets;
      }
   }

   std::map<std::string, unsigned>
   get_symbol_offsets(const std::vector<char> &code,
                      std::string &r_log) {
      const auto elf = elf::get(code);
      const auto symtab = elf::get_symbol_table(elf.get());
      if (!symtab)
         fail(r_log, compile_error(), "Unable to find symbol table.");

      return elf::get_symbol_offsets(elf.get(), symtab);
   }

   module
   build_module_native(llvm::Module *mod, const target &target,
                       const clang::CompilerInstance &c,
                       std::string &r_log) {
      const auto code = compile_native(mod, target, r_log);
      return build_module_common(*mod, code,
                                 get_symbol_offsets(code, r_log), c);
   }
} // End anonymous namespace

module
clover::compile_program_llvm(const std::string &source,
                             const header_map &headers,
                             enum pipe_shader_ir ir,
                             const std::string &target,
                             const std::string &opts,
                             std::string &r_log) {
   if (has_flag(debug::clc))
      debug::log(".cl", "// Build options: " + opts + '\n' + source);

   auto ctx = create_context(r_log);
   // The input file name must have the .cl extension in order for the
   // CompilerInvocation class to recognize it as an OpenCL source file.
   const auto c = create_compiler_instance(target, tokenize(opts + " input.cl"),
                                           r_log);
   auto mod = compile(*ctx, *c, "input.cl", source, headers, target, opts, r_log);

   optimize(*mod, c->getCodeGenOpts().OptimizationLevel);

   if (has_flag(debug::llvm)) {
      std::string log;
      raw_string_ostream s_log(log);
      mod->print(s_log, NULL);
      s_log.flush();
      debug::log(".ll", log);
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
         m = build_module_llvm(&*mod, *c);
         break;
      case PIPE_SHADER_IR_NATIVE:
         m = build_module_native(&*mod, target, *c, r_log);
         break;
   }

   return m;
}
