//===-- toobj.cpp ---------------------------------------------------------===//
//
//                         LDC – the LLVM D compiler
//
// This file is distributed under the BSD-style LDC license. See the LICENSE
// file for details.
//
//===----------------------------------------------------------------------===//

#include "driver/toobj.h"

#include "driver/cl_options.h"
#include "driver/ir2obj_cache.h"
#include "driver/targetmachine.h"
#include "driver/tool.h"
#include "gen/irstate.h"
#include "gen/logger.h"
#include "gen/optimizer.h"
#include "gen/programs.h"
#include "llvm/IR/AssemblyAnnotationWriter.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Bitcode/ReaderWriter.h"
#if LDC_LLVM_VER >= 307
#include "llvm/IR/LegacyPassManager.h"
#else
#include "llvm/PassManager.h"
#endif
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/FormattedStream.h"
#include "llvm/Support/Program.h"
#if LDC_LLVM_VER >= 307
#include "llvm/Support/Path.h"
#endif
#include "llvm/Target/TargetMachine.h"
#if LDC_LLVM_VER >= 307
#include "llvm/Analysis/TargetTransformInfo.h"
#endif
#if LDC_LLVM_VER >= 306
#include "llvm/Target/TargetSubtargetInfo.h"
#endif
#include "llvm/IR/Module.h"
#include <cstddef>
#include <fstream>

#if LDC_LLVM_VER >= 306
using LLErrorInfo = std::error_code;
#define ERRORINFO_STRING(errinfo) errinfo.message().c_str()
#else
using LLErrorInfo = std::string;
#define ERRORINFO_STRING(errinfo) errinfo.c_str()
#endif

static llvm::cl::opt<bool>
    NoIntegratedAssembler("no-integrated-as", llvm::cl::Hidden,
                          llvm::cl::desc("Disable integrated assembler"));

// based on llc code, University of Illinois Open Source License
static void codegenModule(llvm::TargetMachine &Target, llvm::Module &m,
                          llvm::raw_fd_ostream &out,
                          llvm::TargetMachine::CodeGenFileType fileType) {
  using namespace llvm;

// Create a PassManager to hold and optimize the collection of passes we are
// about to build.
#if LDC_LLVM_VER >= 307
  legacy::
#endif
      PassManager Passes;

#if LDC_LLVM_VER >= 307
// The DataLayout is already set at the module (in module.cpp,
// method Module::genLLVMModule())
// FIXME: Introduce new command line switch default-data-layout to
// override the module data layout
#elif LDC_LLVM_VER == 306
  Passes.add(new DataLayoutPass());
#else
  if (const DataLayout *DL = Target.getDataLayout())
    Passes.add(new DataLayoutPass(*DL));
  else
    Passes.add(new DataLayoutPass(&m));
#endif

#if LDC_LLVM_VER >= 307
  // Add internal analysis passes from the target machine.
  Passes.add(
      createTargetTransformInfoWrapperPass(Target.getTargetIRAnalysis()));
#else
  Target.addAnalysisPasses(Passes);
#endif

#if LDC_LLVM_VER < 307
  llvm::formatted_raw_ostream fout(out);
#endif
  if (Target.addPassesToEmitFile(Passes,
#if LDC_LLVM_VER >= 307
                                 out,
#else
                                 fout,
#endif
                                 fileType, codeGenOptLevel())) {
    llvm_unreachable("no support for asm output");
  }

  Passes.run(m);
}

static void assemble(const std::string &asmpath, const std::string &objpath) {
  std::vector<std::string> args;
  args.push_back("-O3");
  args.push_back("-c");
  args.push_back("-xassembler");
  args.push_back(asmpath);
  args.push_back("-o");
  args.push_back(objpath);

  // Only specify -m32/-m64 for architectures where the two variants actually
  // exist (as e.g. the GCC ARM toolchain doesn't recognize the switches).
  // MIPS does not have -m32/-m64 but requires -mabi=.
  if (global.params.targetTriple->get64BitArchVariant().getArch() !=
          llvm::Triple::UnknownArch &&
      global.params.targetTriple->get32BitArchVariant().getArch() !=
          llvm::Triple::UnknownArch) {
    if (global.params.targetTriple->get64BitArchVariant().getArch() ==
            llvm::Triple::mips64 ||
        global.params.targetTriple->get64BitArchVariant().getArch() ==
            llvm::Triple::mips64el) {
      switch (getMipsABI()) {
      case MipsABI::EABI:
        args.push_back("-mabi=eabi");
        args.push_back("-march=mips32r2");
        break;
      case MipsABI::O32:
        args.push_back("-mabi=32");
        args.push_back("-march=mips32r2");
        break;
      case MipsABI::N32:
        args.push_back("-mabi=n32");
        args.push_back("-march=mips64r2");
        break;
      case MipsABI::N64:
        args.push_back("-mabi=64");
        args.push_back("-march=mips64r2");
        break;
      case MipsABI::Unknown:
        break;
      }
    } else {
      if (global.params.is64bit) {
        args.push_back("-m64");
      } else {
        args.push_back("-m32");
      }
    }
  }

  // Run the compiler to assembly the program.
  std::string gcc(getGcc());
  int R = executeToolAndWait(gcc, args, global.params.verbose);
  if (R) {
    error(Loc(), "Error while invoking external assembler.");
    fatal();
  }
}

////////////////////////////////////////////////////////////////////////////////

namespace {
using namespace llvm;
static void printDebugLoc(const DebugLoc &debugLoc, formatted_raw_ostream &os) {
  os << debugLoc.getLine() << ":" << debugLoc.getCol();
#if LDC_LLVM_VER >= 307
  if (DILocation *IDL = debugLoc.getInlinedAt()) {
    os << "@";
    printDebugLoc(IDL, os);
  }
#else
  if (MDNode *N = debugLoc.getInlinedAt(getGlobalContext())) {
    DebugLoc IDL = DebugLoc::getFromDILocation(N);
    if (!IDL.isUnknown()) {
      os << "@";
      printDebugLoc(IDL, os);
    }
  }
#endif
}

class AssemblyAnnotator : public AssemblyAnnotationWriter {
// Find the MDNode which corresponds to the DISubprogram data that described F.
#if LDC_LLVM_VER >= 307
  static DISubprogram *FindSubprogram(const Function *F,
                                      DebugInfoFinder &Finder)
#else
  static MDNode *FindSubprogram(const Function *F, DebugInfoFinder &Finder)
#endif
  {
#if LDC_LLVM_VER >= 307
    for (DISubprogram *Subprogram : Finder.subprograms())
      if (Subprogram->describes(F))
        return Subprogram;
    return nullptr;
#else
    for (DISubprogram Subprogram : Finder.subprograms()) {
      if (Subprogram.describes(F)) {
        return Subprogram;
      }
    }
    return nullptr;
#endif
  }

  static llvm::StringRef GetDisplayName(const Function *F) {
    llvm::DebugInfoFinder Finder;
    Finder.processModule(*F->getParent());
#if LDC_LLVM_VER >= 307
    if (DISubprogram *N = FindSubprogram(F, Finder))
#else
    if (MDNode *N = FindSubprogram(F, Finder))
#endif
    {
#if LDC_LLVM_VER >= 307
      return N->getDisplayName();
#else
      llvm::DISubprogram sub(N);
      return sub.getDisplayName();
#endif
    }
    return "";
  }

public:
  void emitFunctionAnnot(const Function *F,
                         formatted_raw_ostream &os) LLVM_OVERRIDE {
    os << "; [#uses = " << F->getNumUses() << ']';

    // show demangled name
    llvm::StringRef funcName = GetDisplayName(F);
    if (!funcName.empty()) {
      os << " [display name = " << funcName << ']';
    }
    os << '\n';
  }

  void printInfoComment(const Value &val,
                        formatted_raw_ostream &os) LLVM_OVERRIDE {
    bool padding = false;
    if (!val.getType()->isVoidTy()) {
      os.PadToColumn(50);
      padding = true;
      os << "; [#uses = " << val.getNumUses();
      if (isa<GetElementPtrInst>(&val) || isa<PHINode>(&val)) {
        // Only print type for instructions where it is not obvious
        // from being repeated in its parameters. Might need to be
        // extended, but GEPs/PHIs are the most common ones.
        os << ", type = " << *val.getType();
      } else if (isa<AllocaInst>(&val)) {
        os << ", size/byte = "
           << gDataLayout->getTypeAllocSize(val.getType()->getContainedType(0));
      }
      os << ']';
    }

    const Instruction *instr = dyn_cast<Instruction>(&val);
    if (!instr) {
      return;
    }

#if LDC_LLVM_VER >= 307
    if (const DebugLoc &debugLoc = instr->getDebugLoc())
#else
    const DebugLoc &debugLoc = instr->getDebugLoc();
    if (!debugLoc.isUnknown())
#endif
    {
      if (!padding) {
        os.PadToColumn(50);
        padding = true;
        os << ';';
      }
      os << " [debug line = ";
      printDebugLoc(debugLoc, os);
      os << ']';
    }
    if (const DbgDeclareInst *DDI = dyn_cast<DbgDeclareInst>(instr)) {
#if LDC_LLVM_VER >= 307
      DILocalVariable *Var(DDI->getVariable());
#else
      DIVariable Var(DDI->getVariable());
#endif
      if (!padding) {
        os.PadToColumn(50);
        os << ";";
      }
#if LDC_LLVM_VER >= 307
      os << " [debug variable = " << Var->getName() << ']';
#else
      os << " [debug variable = " << Var.getName() << ']';
#endif
    } else if (const DbgValueInst *DVI = dyn_cast<DbgValueInst>(instr)) {
#if LDC_LLVM_VER >= 307
      DILocalVariable *Var(DVI->getVariable());
#else
      DIVariable Var(DVI->getVariable());
#endif
      if (!padding) {
        os.PadToColumn(50);
        os << ";";
      }
#if LDC_LLVM_VER >= 307
      os << " [debug variable = " << Var->getName() << ']';
#else
      os << " [debug variable = " << Var.getName() << ']';
#endif
    } else if (const CallInst *callinstr = dyn_cast<CallInst>(instr)) {
      const Function *F = callinstr->getCalledFunction();
      if (!F) {
        return;
      }

      StringRef funcName = GetDisplayName(F);
      if (!funcName.empty()) {
        if (!padding) {
          os.PadToColumn(50);
          os << ";";
        }
        os << " [display name = " << funcName << ']';
      }
    } else if (const InvokeInst *invokeinstr = dyn_cast<InvokeInst>(instr)) {
      const Function *F = invokeinstr->getCalledFunction();
      if (!F) {
        return;
      }

      StringRef funcName = GetDisplayName(F);
      if (!funcName.empty()) {
        if (!padding) {
          os.PadToColumn(50);
          os << ";";
        }
        os << " [display name = " << funcName << ']';
      }
    }
  }
};

void writeObjectFile(llvm::Module *m, std::string &filename) {
  IF_LOG Logger::println("Writing object file to: %s", filename.c_str());
  LLErrorInfo errinfo;
  {
    llvm::raw_fd_ostream out(filename.c_str(), errinfo, llvm::sys::fs::F_None);
#if LDC_LLVM_VER >= 306
    if (!errinfo)
#else
    if (errinfo.empty())
#endif
    {
      codegenModule(*gTargetMachine, *m, out,
                    llvm::TargetMachine::CGFT_ObjectFile);
    } else {
      error(Loc(), "cannot write object file '%s': %s", filename.c_str(),
            ERRORINFO_STRING(errinfo));
      fatal();
    }
  }
}
} // end of anonymous namespace

void writeModule(llvm::Module *m, std::string filename) {
  // There is no integrated assembler on AIX because XCOFF is not supported.
  // Starting with LLVM 3.5 the integrated assembler can be used with MinGW.
  bool const assembleExternally =
      global.params.output_o &&
      (NoIntegratedAssembler ||
       global.params.targetTriple->getOS() == llvm::Triple::AIX);

  // Use cached object code if possible
  bool useIR2ObjCache = !opts::ir2objCacheDir.empty();
  llvm::SmallString<32> moduleHash;
  if (useIR2ObjCache && global.params.output_o && !assembleExternally) {
    llvm::SmallString<128> cacheDir(opts::ir2objCacheDir.c_str());
    llvm::sys::fs::make_absolute(cacheDir);
    opts::ir2objCacheDir = cacheDir.c_str();

    IF_LOG Logger::println("Use IR-to-Object cache in %s",
                           opts::ir2objCacheDir.c_str());
    LOG_SCOPE

    ir2obj::calculateModuleHash(m, moduleHash);
    std::string cacheFile = ir2obj::cacheLookup(moduleHash);
    if (!cacheFile.empty()) {
      ir2obj::recoverObjectFile(moduleHash, filename);
      return;
    }
  }

  // run optimizer
  ldc_optimize_module(m);

  // eventually do our own path stuff, dmd's is a bit strange.
  using LLPath = llvm::SmallString<128>;

  // make sure the output directory exists
  const auto directory = llvm::sys::path::parent_path(filename);
  if (!directory.empty()) {
    if (auto ec = llvm::sys::fs::create_directories(directory)) {
      error(Loc(), "failed to create output directory: %s\n%s",
            directory.data(), ec.message().c_str());
      fatal();
    }
  }

  // write LLVM bitcode
  if (global.params.output_bc) {
    LLPath bcpath(filename);
    llvm::sys::path::replace_extension(bcpath, global.bc_ext);
    Logger::println("Writing LLVM bitcode to: %s\n", bcpath.c_str());
    LLErrorInfo errinfo;
    llvm::raw_fd_ostream bos(bcpath.c_str(), errinfo, llvm::sys::fs::F_None);
    if (bos.has_error()) {
      error(Loc(), "cannot write LLVM bitcode file '%s': %s", bcpath.c_str(),
            ERRORINFO_STRING(errinfo));
      fatal();
    }
    llvm::WriteBitcodeToFile(m, bos);
  }

  // write LLVM IR
  if (global.params.output_ll) {
    LLPath llpath(filename);
    llvm::sys::path::replace_extension(llpath, global.ll_ext);
    Logger::println("Writing LLVM IR to: %s\n", llpath.c_str());
    LLErrorInfo errinfo;
    llvm::raw_fd_ostream aos(llpath.c_str(), errinfo, llvm::sys::fs::F_None);
    if (aos.has_error()) {
      error(Loc(), "cannot write LLVM IR file '%s': %s", llpath.c_str(),
            ERRORINFO_STRING(errinfo));
      fatal();
    }
    AssemblyAnnotator annotator;
    m->print(aos, &annotator);
  }

  // write native assembly
  if (global.params.output_s || assembleExternally) {
    LLPath spath(filename);
    llvm::sys::path::replace_extension(spath, global.s_ext);
    if (!global.params.output_s) {
      llvm::sys::fs::createUniqueFile("ldc-%%%%%%%.s", spath);
    }

    Logger::println("Writing asm to: %s\n", spath.c_str());
    LLErrorInfo errinfo;
    {
      llvm::raw_fd_ostream out(spath.c_str(), errinfo, llvm::sys::fs::F_None);
#if LDC_LLVM_VER >= 306
      if (!errinfo)
#else
      if (errinfo.empty())
#endif
      {
        codegenModule(*gTargetMachine, *m, out,
                      llvm::TargetMachine::CGFT_AssemblyFile);
      } else {
        error(Loc(), "cannot write asm: %s", ERRORINFO_STRING(errinfo));
        fatal();
      }
    }

    if (assembleExternally) {
      assemble(spath.str(), filename);
    }

    if (!global.params.output_s) {
      llvm::sys::fs::remove(spath.str());
    }
  }

  if (global.params.output_o && !assembleExternally) {
    writeObjectFile(m, filename);
    if (useIR2ObjCache) {
      ir2obj::cacheObjectFile(filename, moduleHash);
    }
  }
}

#undef ERRORINFO_STRING
