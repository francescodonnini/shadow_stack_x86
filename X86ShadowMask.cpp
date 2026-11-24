#include "MCTargetDesc/X86BaseInfo.h"
#include "X86ShadowStack.h"

#include "MCTargetDesc/X86MCTargetDesc.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/CodeGen/LiveRegUnits.h"
#include <bitset>

char X86ShadowStackMaskPass::ID = 0;

static RegisterPass<X86ShadowStackMaskPass>
    X("x86-shadow-stack-mask", "X86 Shadow Stack Mask Pass", false, false);

namespace {
constexpr MCPhysReg GPR64[] = {X86::RAX, X86::RBX, X86::RCX, X86::RDX,
                               X86::RSI, X86::RDI, X86::RBP, X86::RSP,
                               X86::R8,  X86::R9,  X86::R10, X86::R11,
                               X86::R12, X86::R13, X86::R14, X86::R15};

using RegSet = std::bitset<16>;

}; // namespace

FunctionPass *llvm::createX86ShadowStackMaskPass() {
  return new X86ShadowStackMaskPass();
}

bool X86ShadowStackMaskPass::runOnMachineFunction(MachineFunction &MF) {
  const auto *TRI = MF.getSubtarget().getRegisterInfo();
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
      errs() << "base=" << printReg(Base, TRI)
             << " index=" << printReg(Index, TRI) << "  " << MI;
    }
  }
  return false;
}