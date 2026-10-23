#ifndef X86_SHADOW_STACK_PASS_H
#define X86_SHADOW_STACK_PASS_H

#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/RegisterScavenging.h"
#include "llvm/Pass.h"

#define X86_SHADOW_STACK_PASS_NAME "x86-shadow-stack"

using namespace llvm;

enum class ViolationStrategy { BlindWrite, Trap };

class X86ShadowStackPass : public MachineFunctionPass {
public:
  static char ID;
  ViolationStrategy Strategy;
  GlobalVariable *OffsetGV = nullptr;

  X86ShadowStackPass(ViolationStrategy Strategy = ViolationStrategy::BlindWrite)
      : MachineFunctionPass(ID), Strategy(Strategy) {}

  bool doInitialization(Module &M) override;

  bool runOnMachineFunction(MachineFunction &MF) override;

  StringRef getPassName() const override { return X86_SHADOW_STACK_PASS_NAME; }

private:
  MachineBasicBlock *TrapBB = nullptr;

  void instrumentPreamble(MachineFunction &MF);

  void instrumentRet(MachineFunction &MF, MachineBasicBlock &MBB,
                     RegScavenger &RS, MachineBasicBlock::iterator I);

  void emitLoadOffset(MachineBasicBlock &MBB, MachineBasicBlock::iterator MI,
                      const DebugLoc &DL, const TargetInstrInfo *TII,
                      Register Dst);

  MachineBasicBlock *getOrCreateTrapBB(MachineFunction &MF, DebugLoc DL);
};
#endif