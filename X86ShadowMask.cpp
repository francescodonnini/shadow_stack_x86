#include "MCTargetDesc/X86BaseInfo.h"
#include "X86ShadowStack.h"

#include "MCTargetDesc/X86MCTargetDesc.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/CodeGen/LiveRegUnits.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/TargetInstrInfo.h"
#include "llvm/CodeGen/TargetRegisterInfo.h"
#include "llvm/CodeGen/TargetSubtargetInfo.h"
#include "llvm/Support/raw_ostream.h"
#include <bitset>

char X86ShadowStackMaskPass::ID = 0;

static RegisterPass<X86ShadowStackMaskPass>
    X("x86-shadow-stack-mask", "X86 Shadow Stack Mask Pass", false, false);

FunctionPass *llvm::createX86ShadowStackMaskPass() {
  return new X86ShadowStackMaskPass();
}

bool X86ShadowStackMaskPass::runOnMachineFunction(MachineFunction &MF) {
  const auto *TII = MF.getSubtarget().getInstrInfo();
  const auto *TRI = MF.getSubtarget().getRegisterInfo();
  auto Changed = false;
  for (auto &MBB : MF) {
    for (auto &MI : MBB) {
      // Calls write into memory but they write wherever the stack pointer is
      // pointing to (we don't mask the stack pointer)
      if (!MI.mayStore() || MI.isCall()) {
        continue;
      }

      auto Mem = X86II::getMemoryOperandNo(MI.getDesc().TSFlags);
      if (Mem < 0) {
        errs() << "[implicit] " << MI; // e.g. push, stos
        continue;
      }

      Mem += X86II::getOperandBias(MI.getDesc());
      auto Base = MI.getOperand(Mem + X86::AddrBaseReg).getReg();
      auto Index = MI.getOperand(Mem + X86::AddrIndexReg).getReg();
      if (Index) {
        continue;
      }

      if (!Base || Base == X86::RIP || Base == X86::RSP ||
          TRI->getRegClass(X86::GR32RegClassID)->contains(Base)) {
        continue;
      }

      if (MBB.computeRegisterLiveness(TRI, X86::EFLAGS, MI) !=
          MachineBasicBlock::LQR_Dead) {
        errs() << "[flags live] " << MI;
        continue;
      }

      BuildMI(MBB, MI, MI.getDebugLoc(), TII->get(X86::BTR64ri8), Base)
          .addReg(Base)
          .addImm(63);
      Changed = true;
    }
  }

  return Changed;
}