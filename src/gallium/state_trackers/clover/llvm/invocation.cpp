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

#include "llvm/codegen.hpp"
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
                                           get_kernels(mod)));

      ::llvm::PassManagerBuilder pmb;
      pmb.OptLevel = optimization_level;
      pmb.LibraryInfo = new compat::target_library_info(
         ::llvm::Triple(mod.getTargetTriple()));
      pmb.populateModulePassManager(pm);
      pm.run(mod);
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

   if (has_flag(debug::llvm))
      debug::log(".ll", print_module_bitcode(*mod));

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
         m = build_module_bitcode(*mod, *c);
         break;
      case PIPE_SHADER_IR_NATIVE:
         if (has_flag(debug::native))
            debug::log(".asm", print_module_native(*mod, target));

         m = build_module_native(*mod, target, *c, r_log);
         break;
   }

   return m;
}
