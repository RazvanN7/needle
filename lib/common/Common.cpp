#define DEBUG_TYPE "pasha_common"
#include "Common.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/Triple.h"
#include "llvm/Analysis/CFG.h"
#include "llvm/Analysis/CFGPrinter.h"
#include "llvm/Analysis/TargetLibraryInfo.h"
#include "llvm/AsmParser/Parser.h"
#include "llvm/Bitcode/ReaderWriter.h"
#include "llvm/CodeGen/CommandFlags.h"
#include "llvm/CodeGen/LinkAllAsmWriterComponents.h"
#include "llvm/CodeGen/LinkAllCodegenComponents.h"
#include "llvm/IR/Constant.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/IRPrintingPasses.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/PassManager.h"
#include "llvm/IR/Verifier.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/Linker/Linker.h"
#include "llvm/Linker/Linker.h"
#include "llvm/MC/SubtargetFeature.h"
#include "llvm/Pass.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/FileUtilities.h"
#include "llvm/Support/FormattedStream.h"
#include "llvm/Support/Host.h"
#include "llvm/Support/ManagedStatic.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/PrettyStackTrace.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/Signals.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/TargetRegistry.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/ToolOutputFile.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Transforms/IPO/PassManagerBuilder.h"
#include "llvm/Transforms/Scalar.h"

using namespace llvm;
using namespace std;

// GCC 4.8.3 does not have make_unique
template <typename T, typename... Args>
unique_ptr<T> make_unique(Args &&... args) {
    return unique_ptr<T>(new T(forward<Args>(args)...));
}

namespace common {

void compile(Module &m, string outputPath, char optLevel) {
    string err;

    Triple triple = Triple(m.getTargetTriple());
    const Target *target = TargetRegistry::lookupTarget(MArch, triple, err);
    if (!target) {
        report_fatal_error("Unable to find target:\n " + err);
    }

    CodeGenOpt::Level level = CodeGenOpt::Default;
    switch (optLevel) {
    default:
        report_fatal_error("Invalid optimization level.\n");
    // No fall through
    case '0':
        level = CodeGenOpt::None;
        break;
    case '1':
        level = CodeGenOpt::Less;
        break;
    case '2':
        level = CodeGenOpt::Default;
        break;
    case '3':
        level = CodeGenOpt::Aggressive;
        break;
    }

    string FeaturesStr;
    TargetOptions options = InitTargetOptionsFromCodeGenFlags();
    unique_ptr<TargetMachine> machine(
        target->createTargetMachine(triple.getTriple(), MCPU, FeaturesStr,
                                    options, RelocModel, CMModel, level));
    assert(machine.get() && "Could not allocate target machine!");

    if (FloatABIForCalls != FloatABI::Default) {
        options.FloatABIType = FloatABI::Soft;
    }

    error_code EC;
    auto Out = ::make_unique<tool_output_file>(outputPath.c_str(), EC,
                                               sys::fs::F_None);
    if (EC) {
        report_fatal_error("Unable to create file:\n " + EC.message());
    }

    // Build up all of the passes that we want to do to the module.
    legacy::PassManager pm;

    // Add target specific info and transforms
    pm.add(new TargetLibraryInfoWrapperPass(triple));
    // machine->addAnalysisPasses(pm);

    // if (const DataLayout *dl = machine->createDataLayout()) {
    //      m.setDataLayout(dl);
    // }
    m.setDataLayout(machine->createDataLayout());

    { // Bound this scope
        // formatted_raw_ostream fos(out->os());
        raw_pwrite_stream *OS = &Out->os();
        FileType = TargetMachine::CGFT_ObjectFile;
        // Ask the target to add backend passes as necessary.
        if (machine->addPassesToEmitFile(pm, *OS, FileType)) {
            report_fatal_error("target does not support generation "
                               "of this file type!\n");
        }

        // Before executing passes, print the final values of the LLVM options.
        cl::PrintOptionValues();
        pm.run(m);
    }

    // Keep the output binary if we've been successful to this point.
    Out->keep();
}

void link(const string &objectFile, const string &outputFile, char optLevel,
          cl::list<string> &libPaths, cl::list<string> &libraries) {
    auto clang = sys::findProgramByName("clang++");
    if (!clang) {
        report_fatal_error(
            "Unable to link output file. Clang not found in PATH.");
    }

    string opt("-O");
    opt += optLevel;

    vector<string> args{clang.get(), opt, "-o", outputFile, objectFile};

    for (auto &libPath : libPaths) {
        args.push_back("-L" + libPath);
    }

    for (auto &library : libraries) {
        args.push_back("-l" + library);
    }

    vector<const char *> charArgs;
    for (auto &arg : args) {
        charArgs.push_back(arg.c_str());
    }
    charArgs.push_back(0);

    for (auto &arg : args) {
        DEBUG(outs() << arg.c_str() << " ");
    }
    DEBUG(outs() << "\n");

    string err;
    if (-1 == sys::ExecuteAndWait(clang.get(), &charArgs[0], nullptr, 0, 0, 0,
                                  &err)) {
        report_fatal_error("Unable to link output file.");
    }
}

void generateBinary(Module &m, const string &outputFilename, char optLevel,
                    cl::list<string> &libPaths, cl::list<string> &libraries) {
    // Compiling to native should allow things to keep working even when the
    // version of clang on the system and the version of LLVM used to compile
    // the tool don't quite match up.
    string objectFile = outputFilename + ".o";
    compile(m, objectFile, optLevel);
    link(objectFile, outputFilename, optLevel, libPaths, libraries);
}

void saveModule(Module &m, StringRef filename) {
    error_code EC;
    raw_fd_ostream out(filename.data(), EC, sys::fs::F_None);

    if (EC) {
        report_fatal_error("error saving llvm module to '" + filename +
                           "': \n" + EC.message());
    }
    WriteBitcodeToFile(&m, out);
}

DenseSet<pair<const BasicBlock *, const BasicBlock *>>
getBackEdges(BasicBlock *StartBB) {
    SmallVector<std::pair<const BasicBlock *, const BasicBlock *>, 8>
        BackEdgesVec;
    FindFunctionBackedges(*StartBB->getParent(), BackEdgesVec);
    DenseSet<pair<const BasicBlock *, const BasicBlock *>> BackEdges;

    for (auto &BE : BackEdgesVec) {
        BackEdges.insert(BE);
    }
    return BackEdges;
}

DenseSet<pair<const BasicBlock *, const BasicBlock *>>
getBackEdges(Function &F) {
    return getBackEdges(&F.getEntryBlock());
}

void optimizeModule(Module *Mod) {
    PassManagerBuilder PMB;
    PMB.OptLevel = 2;
    PMB.SLPVectorize = false;
    PMB.BBVectorize = false;
    legacy::PassManager PM;
    PMB.populateModulePassManager(PM);
    PM.run(*Mod);
}

void lowerSwitch(Function &F) {
    legacy::FunctionPassManager FPM(F.getParent());
    FPM.add(createLowerSwitchPass());
    FPM.doInitialization();
    FPM.run(F);
    FPM.doFinalization();
}

void breakCritEdges(Function &F) {
    legacy::FunctionPassManager FPM(F.getParent());
    FPM.add(createBreakCriticalEdgesPass());
    FPM.doInitialization();
    FPM.run(F);
    FPM.doFinalization();
}

void printCFG(Function &F) {
    legacy::FunctionPassManager FPM(F.getParent());
    FPM.add(llvm::createCFGPrinterPass());
    FPM.doInitialization();
    FPM.run(F);
    FPM.doFinalization();
}

void lowerSwitch(Module &M, StringRef FunctionName) {
    for (auto &F : M) {
        if (F.getName() == FunctionName)
            lowerSwitch(F);
    }
}

void breakCritEdges(Module &M, StringRef FunctionName) {
    for (auto &F : M) {
        if (F.getName() == FunctionName)
            breakCritEdges(F);
    }
}

bool checkIntrinsic(CallSite &CS) {
    auto *F = CS.getCalledFunction();
    auto Name = F->getName();
    if (Name.startswith("llvm.memcpy") || Name.startswith("llvm.memmove") ||
        Name.startswith("llvm.memset")) {
        // If the mem intrinsic is a small constant, then
        // it's ok to keep. This will usually happen for a
        // struct.
        auto *LenArg = CS.getArgument(2);
        if (ConstantInt *CI = dyn_cast<ConstantInt>(LenArg)) {
            if (CI->getLimitedValue() < 16) {
                return false;
            }
        }
        return true;
    } else if (Name.startswith("llvm.dbg.") ||      // This will be stripped out
               Name.startswith("llvm.lifetime.") || // This will be stripped out
               Name.startswith("llvm.uadd.") || // Handled in the Verilog module
               Name.startswith("llvm.umul.") || // Handled in the Verilog module
               Name.startswith(
                   "llvm.bswap.") || // Handled in the Verilog module
               Name.startswith("llvm.fabs.")) {
        return false;
    }
    // else if(F->isIntrinsic()){
    // return false;
    //}
    return true;
}

bool isSelfLoop(const BasicBlock *BB) {
    for (auto S = succ_begin(BB), E = succ_end(BB); S != E; S++) {
        if (*S == BB) {
            return true;
        }
    }
    return false;
}

static void getLoopsHelper(SetVector<Loop *> &Loops, Loop *L) {
    Loops.insert(L);
    for (auto &SL : L->getSubLoops()) {
        getLoopsHelper(Loops, SL);
    }
}

SetVector<Loop *> getLoops(LoopInfo *LI) {
    SetVector<Loop *> Loops;
    for (auto &L : *LI) {
        getLoopsHelper(Loops, L);
    }
    return Loops;
}

void writeModule(Module *Mod, string Name) {
    error_code EC;
    raw_fd_ostream File(Name, EC, sys::fs::OpenFlags::F_RW);
    Mod->print(File, nullptr);
    File.close();
}
}
