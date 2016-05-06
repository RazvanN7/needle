#ifndef EPPDECODE_H
#define EPPDECODE_H

#include "llvm/Analysis/LoopInfo.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/APInt.h"
#include "llvm/IR/Module.h"
#include "llvm/Pass.h"

#include "EPPEncode.h"
#include <map>
#include <vector>

namespace epp {
enum PathType { RIRO, FIRO, RIFO, FIFO };
struct EPPDecode : public llvm::ModulePass {
    static char ID;
    llvm::StringRef filename;
    size_t numberToReturn;
    //llvm::DenseMap<std::uint64_t, std::uint64_t> SelfProfileMap;

    std::map<llvm::BasicBlock *, std::vector<std::shared_ptr<Edge>>> ValBySrc;

    EPPDecode() : llvm::ModulePass(ID) {}

    virtual void getAnalysisUsage(llvm::AnalysisUsage &au) const override {
        au.addRequired<EPPEncode>();
    }

    virtual bool runOnModule(llvm::Module &m) override;
    virtual void releaseMemory() override;

    std::pair<PathType, std::vector<llvm::BasicBlock *>>
    decode(llvm::Function &f, llvm::APInt pathID, EPPEncode &E);
};
}

#endif
