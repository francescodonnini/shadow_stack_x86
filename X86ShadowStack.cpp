#include "X86ShadowStack.h"
#include "llvm/CodeGen/MachineFrameInfo.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/TargetInstrInfo.h"
#include "llvm/CodeGen/TargetSubtargetInfo.h"
#include "MCTargetDesc/X86MCTargetDesc.h"

char X86ShadowStackPass::ID = 0;
static RegisterPass<X86ShadowStackPass> X(
    "x86-shadow-stack",
    "X86 Shadow Stack Pass",
    false,
    false
);

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

        for (auto I = MBB.end(); I != MBB.begin();) {
            --I;
            if (I->isReturn()) {
                instrumentRet(MF, MBB, RS, I);
            }
            RS.backward(I);
        }
    }

    return true;
}

void X86ShadowStackPass::instrumentPreamble(MachineFunction& MF) {
    auto &MBB = MF.front();
    auto MI = MBB.begin();
    while (MI != MBB.end() && MI->isMetaInstruction()) {
        ++MI;
    }

    auto DL = MI != MBB.end() ? MI->getDebugLoc() : DebugLoc();
    const auto *TII = MF.getSubtarget().getInstrInfo();
    BuildMI(MBB, MI, DL, TII->get(X86::PUSH64r))
        .addReg(X86::R11, RegState::Undef);
    BuildMI(MBB, MI, DL, TII->get(X86::PUSH64r))
        .addReg(X86::R10, RegState::Undef);
    BuildMI(MBB, MI, DL, TII->get(X86::MOV64rm), X86::R11)
        .addReg(0)
        .addImm(1)
        .addReg(0)
        .addImm(8)
        .addReg(X86::GS);
    BuildMI(MBB, MI, DL, TII->get(X86::MOV64rm), X86::R10)
        .addReg(X86::RSP)
        .addImm(1)
        .addReg(0)
        .addImm(16)
        .addReg(0);
    BuildMI(MBB, MI, DL, TII->get(X86::MOV64mr))
        .addReg(X86::R11)
        .addImm(1)
        .addReg(0)
        .addImm(0)
        .addReg(0)
        .addReg(X86::R10);
    BuildMI(MBB, MI, DL, TII->get(X86::ADD64ri8), X86::R11)
        .addReg(X86::R11)
        .addImm(8);
    BuildMI(MBB, MI, DL, TII->get(X86::MOV64mr))
        .addReg(0)
        .addImm(1)
        .addReg(0)
        .addImm(8)
        .addReg(X86::GS)
        .addReg(X86::R11);
    BuildMI(MBB, MI, DL, TII->get(X86::POP64r), X86::R10);
    BuildMI(MBB, MI, DL, TII->get(X86::POP64r), X86::R11);
}

void X86ShadowStackPass::instrumentRet(
    MachineFunction &MF,
    MachineBasicBlock &MBB,
    RegScavenger &RS,
    MachineBasicBlock::iterator MI) {
    const auto *TII = MF.getSubtarget().getInstrInfo();
    auto DL = MI->getDebugLoc();

    int64_t RspOffset = 0UL;
    const auto *TRI = MF.getSubtarget().getRegisterInfo();
    auto ScratchReg = RS.scavengeRegisterBackwards(*TRI->getRegClass(X86::GR64RegClassID), MI, false, 0, false);
    auto NeedManualSpilling = !ScratchReg.isValid();
    if (NeedManualSpilling) {
        ScratchReg = X86::R11;
        BuildMI(MBB, MI, DL, TII->get(X86::PUSH64r))
            .addReg(ScratchReg, RegState::Undef);
        RspOffset = 8;
    }

    BuildMI(MBB, MI, DL, TII->get(X86::MOV64rm), ScratchReg)
        .addReg(0)
        .addImm(1)
        .addReg(0)
        .addImm(8)
        .addReg(X86::GS);
    
    BuildMI(MBB, MI, DL, TII->get(X86::SUB64ri8), ScratchReg)
        .addReg(ScratchReg)
        .addImm(8);

    BuildMI(MBB, MI, DL, TII->get(X86::MOV64mr))
        .addReg(0)
        .addImm(1)
        .addReg(0)
        .addImm(8)
        .addReg(X86::GS)
        .addReg(ScratchReg);
    
    BuildMI(MBB, MI, DL, TII->get(X86::MOV64rm), ScratchReg)
        .addReg(ScratchReg)
        .addImm(1)
        .addReg(0)
        .addImm(0)
        .addReg(0);
    
    switch (Strategy) {
        case ViolationStrategy::BlindWrite:
            BuildMI(MBB, MI, DL, TII->get(X86::MOV64mr))
                .addReg(X86::RSP)
                .addImm(1)
                .addReg(0)
                .addImm(RspOffset)
                .addReg(0)
                .addReg(ScratchReg);
            break;
        case ViolationStrategy::Trap:
            BuildMI(MBB, MI, DL, TII->get(X86::CMP64mr))
                .addReg(X86::RSP)
                .addImm(1)
                .addReg(0)
                .addImm(RspOffset)
                .addReg(0)
                .addReg(ScratchReg);
            BuildMI(MBB, MI, DL, TII->get(X86::JCC_1))
                .addMBB(getOrCreateTrapBB(MF, DL))
                .addImm(X86::COND_NE);
            MBB.addSuccessor(TrapBB);
            break;
    }

    if (NeedManualSpilling) {
        BuildMI(MBB, MI, DL, TII->get(X86::POP64r), ScratchReg);
    }
}

MachineBasicBlock* X86ShadowStackPass::getOrCreateTrapBB(MachineFunction &MF, DebugLoc DL) {
    if (!TrapBB) {
        TrapBB = MF.CreateMachineBasicBlock();
        MF.push_back(TrapBB);
        const auto *TII = MF.getSubtarget().getInstrInfo();
        BuildMI(TrapBB, DL, TII->get(X86::TRAP));
    }
    return TrapBB;
}