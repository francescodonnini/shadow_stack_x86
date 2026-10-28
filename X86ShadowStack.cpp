#include "X86ShadowStack.h"
#include "MCTargetDesc/X86BaseInfo.h"
#include "MCTargetDesc/X86MCTargetDesc.h"
#include "llvm/CodeGen/MachineFrameInfo.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/TargetInstrInfo.h"
#include "llvm/CodeGen/TargetSubtargetInfo.h"
#include "llvm/IR/Module.h"
#include "llvm/MC/MCDwarf.h"

char X86ShadowStackPass::ID = 0;

static RegisterPass<X86ShadowStackPass>
    X("x86-shadow-stack", "X86 Shadow Stack Pass", false, false);

bool X86ShadowStackPass::doInitialization(Module &M) {
  OffsetGV = M.getGlobalVariable("__ss_shadow_offset");
  if (!OffsetGV) {
    OffsetGV = new GlobalVariable(M, Type::getInt64Ty(M.getContext()), false,
                                  GlobalValue::ExternalLinkage, nullptr,
                                  "__ss_shadow_offset");
  }
  return false;
}

bool X86ShadowStackPass::runOnMachineFunction(MachineFunction &MF) {
  bool IsLeaf = !MF.getFrameInfo().hasCalls();
  if (IsLeaf) {
    return false;
  }

  instrumentPreamble(MF);

  TrapBB = nullptr;
  RegScavenger RS;
  for (auto &MBB : MF) {
    RS.enterBasicBlockEnd(MBB);
    auto I = MBB.end();
    while (I != MBB.begin()) {
      --I;
      if (I->isReturn()) {
        instrumentRet(MF, MBB, RS, I);
      }
      RS.backward(I);
    }
  }

  return true;
}

void X86ShadowStackPass::emitLoadOffset(MachineBasicBlock &MBB,
                                        MachineBasicBlock::iterator MI,
                                        const DebugLoc &DL,
                                        const TargetInstrInfo *TII,
                                        Register Dst) {
  // Dst = &__ss_shadow_offset;
  BuildMI(MBB, MI, DL, TII->get(X86::MOV64rm), Dst)
      .addReg(X86::RIP)
      .addImm(1) // Scale = 1
      .addReg(0) // Index = None
      .addGlobalAddress(OffsetGV, 0, X86II::MO_GOTPCREL)
      .addReg(0); // Segment = None
  // Dst = *Dst
  BuildMI(MBB, MI, DL, TII->get(X86::MOV64rm), Dst)
      .addReg(Dst)
      .addImm(1)
      .addReg(0)
      .addImm(0)
      .addReg(0);
}

void X86ShadowStackPass::instrumentPreamble(MachineFunction &MF) {
  auto &MBB = MF.front();
  auto MI = MBB.begin();
  while (MI != MBB.end() && MI->isMetaInstruction()) {
    ++MI;
  }

  auto DL = MI != MBB.end() ? MI->getDebugLoc() : DebugLoc();
  const auto *TII = MF.getSubtarget().getInstrInfo();
  const auto *TRI = MF.getSubtarget().getRegisterInfo();

  auto IsRegOrAliasLiveIn = [&](Register Reg) {
    for (auto Alias = MCRegAliasIterator(Reg, TRI, true); Alias.isValid();
         ++Alias) {
      if (MBB.isLiveIn(*Alias)) {
        return true;
      }
    }

    return false;
  };

  auto AdjustCFI = [&](int Offset) {
    auto CFIIndex = MF.addFrameInst(
        MCCFIInstruction::createAdjustCfaOffset(nullptr, Offset));
    BuildMI(MBB, MI, DL, TII->get(TargetOpcode::CFI_INSTRUCTION))
        .addCFIIndex(CFIIndex);
  };

  auto RspOffset = (int64_t)0;
  auto SpillR11 = IsRegOrAliasLiveIn(X86::R11);
  if (SpillR11) {
    BuildMI(MBB, MI, DL, TII->get(X86::PUSH64r)).addReg(X86::R11);
    RspOffset += 8;
  }

  auto SpillR10 = IsRegOrAliasLiveIn(X86::R10);
  if (SpillR10) {
    BuildMI(MBB, MI, DL, TII->get(X86::PUSH64r)).addReg(X86::R10);
    RspOffset += 8;
  }

  // movq 16(%rsp), %r11       -> r11 = return address
  BuildMI(MBB, MI, DL, TII->get(X86::MOV64rm), X86::R11)
      .addReg(X86::RSP)
      .addImm(1)
      .addReg(0)
      .addImm(RspOffset)
      .addReg(0);

  // movq __ss_shadow_offset@GOTPCREL(%rip), %r10
  // movq (%r10), %r10         -> r10 = offset
  emitLoadOffset(MBB, MI, DL, TII, X86::R10);

  // movq %r11, 16(%rsp,%r10,1) -> *(ret_slot + offset) = return address
  BuildMI(MBB, MI, DL, TII->get(X86::MOV64mr))
      .addReg(X86::RSP)
      .addImm(1)
      .addReg(X86::R10)
      .addImm(RspOffset)
      .addReg(0)
      .addReg(X86::R11);

  // popq %r10 ; popq %r11
  if (SpillR10) {
    BuildMI(MBB, MI, DL, TII->get(X86::POP64r), X86::R10);
  }

  if (SpillR11) {
    BuildMI(MBB, MI, DL, TII->get(X86::POP64r), X86::R11);
  }
}

void X86ShadowStackPass::instrumentRet(MachineFunction &MF,
                                       MachineBasicBlock &MBB, RegScavenger &RS,
                                       MachineBasicBlock::iterator MI) {
  const auto *TII = MF.getSubtarget().getInstrInfo();
  auto DL = MI->getDebugLoc();

  int64_t RspOffset = 0;
  const auto *TRI = MF.getSubtarget().getRegisterInfo();
  auto ScratchReg = RS.scavengeRegisterBackwards(
      *TRI->getRegClass(X86::GR64RegClassID), MI, false, 0, false);
  auto NeedManualSpilling = !ScratchReg.isValid();
  if (NeedManualSpilling) {
    ScratchReg = X86::R11;
    BuildMI(MBB, MI, DL, TII->get(X86::PUSH64r))
        .addReg(ScratchReg, RegState::Undef);
    RspOffset = 8;
  }

  // movq __ss_shadow_offset@GOTPCREL(%rip), %scratch
  // movq (%scratch), %scratch                 -> scratch = offset
  emitLoadOffset(MBB, MI, DL, TII, ScratchReg);

  // movq RspOffset(%rsp,%scratch,1), %scratch
  // scratch = *(ret_slot + offset)
  BuildMI(MBB, MI, DL, TII->get(X86::MOV64rm), ScratchReg)
      .addReg(X86::RSP)
      .addImm(1)
      .addReg(ScratchReg)
      .addImm(RspOffset)
      .addReg(0);

  switch (Strategy) {
  case ViolationStrategy::BlindWrite:
    // movq %scratch, RspOffset(%rsp)
    // overwrites normal return address
    BuildMI(MBB, MI, DL, TII->get(X86::MOV64mr))
        .addReg(X86::RSP)    // Base (Memory source)
        .addImm(1)           // Scale (Memory source))
        .addReg(0)           // Index (Memory source)
        .addImm(RspOffset)   // Displacement (Memory source)
        .addReg(0)           // Segment (Memory source)
        .addReg(ScratchReg); // Source
    break;
  case ViolationStrategy::Trap:
    // cmpq %scratch, RspOffset(%rsp)
    BuildMI(MBB, MI, DL, TII->get(X86::CMP64mr))
        .addReg(X86::RSP)
        .addImm(1)
        .addReg(0)
        .addImm(RspOffset)
        .addReg(0)
        .addReg(ScratchReg);
    break;
  }

  if (NeedManualSpilling) {
    // Put back to the scratch register its original value stored on the stack
    // It's a safe operation because POP doesn't change EFLAGS (might be used by
    // JCC_1)
    BuildMI(MBB, MI, DL, TII->get(X86::POP64r), ScratchReg);
  }

  if (Strategy == ViolationStrategy::Trap) {
    BuildMI(MBB, MI, DL, TII->get(X86::JCC_1))
        .addMBB(getOrCreateTrapBB(MF, DL))
        .addImm(X86::COND_NE);
    MBB.addSuccessor(TrapBB);
  }
}

MachineBasicBlock *X86ShadowStackPass::getOrCreateTrapBB(MachineFunction &MF,
                                                         DebugLoc DL) {
  if (!TrapBB) {
    TrapBB = MF.CreateMachineBasicBlock();
    MF.push_back(TrapBB);
    const auto *TII = MF.getSubtarget().getInstrInfo();
    BuildMI(TrapBB, DL, TII->get(X86::TRAP));
  }

  return TrapBB;
}