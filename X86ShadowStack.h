#ifndef X86_SHADOW_STACK_PASS_H
#define X86_SHADOW_STACK_PASS_H

#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/RegisterScavenging.h"
#include "llvm/Pass.h"

#define X86_SHADOW_STACK_PASS_NAME "x86-shadow-stack"

using namespace llvm;

enum class ViolationStrategy {
    BlindWrite,
    Trap
};

class X86ShadowStackPass : public MachineFunctionPass {
public:
    static char ID;
    ViolationStrategy Strategy;

    X86ShadowStackPass(ViolationStrategy Strategy = ViolationStrategy::BlindWrite) : MachineFunctionPass(ID), Strategy(Strategy) {}

    bool runOnMachineFunction(MachineFunction &MF) override;

    StringRef getPassName() const override {
        return X86_SHADOW_STACK_PASS_NAME;
    }

private:
    MachineBasicBlock *TrapBB = nullptr;

    void instrumentPreamble(MachineFunction& MF);

    void instrumentRet(MachineFunction& MF, MachineBasicBlock& MBB, RegScavenger& RS, MachineBasicBlock::iterator I);

    MachineBasicBlock *getOrCreateTrapBB(MachineFunction &MF, DebugLoc DL);
};
#endif