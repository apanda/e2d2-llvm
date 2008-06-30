//===-- SparcISelDAGToDAG.cpp - A dag to dag inst selector for Sparc ------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file defines an instruction selector for the SPARC target.
//
//===----------------------------------------------------------------------===//

#include "SparcISelLowering.h"
#include "SparcTargetMachine.h"
#include "llvm/Intrinsics.h"
#include "llvm/CodeGen/SelectionDAGISel.h"
#include "llvm/Support/Compiler.h"
#include "llvm/Support/Debug.h"
using namespace llvm;

//===----------------------------------------------------------------------===//
// Instruction Selector Implementation
//===----------------------------------------------------------------------===//

//===--------------------------------------------------------------------===//
/// SparcDAGToDAGISel - SPARC specific code to select SPARC machine
/// instructions for SelectionDAG operations.
///
namespace {
class SparcDAGToDAGISel : public SelectionDAGISel {
  SparcTargetLowering Lowering;

  /// Subtarget - Keep a pointer to the Sparc Subtarget around so that we can
  /// make the right decision when generating code for different targets.
  const SparcSubtarget &Subtarget;
public:
  SparcDAGToDAGISel(TargetMachine &TM)
    : SelectionDAGISel(Lowering), Lowering(TM),
      Subtarget(TM.getSubtarget<SparcSubtarget>()) {
  }

  SDNode *Select(SDOperand Op);

  // Complex Pattern Selectors.
  bool SelectADDRrr(SDOperand Op, SDOperand N, SDOperand &R1, SDOperand &R2);
  bool SelectADDRri(SDOperand Op, SDOperand N, SDOperand &Base,
                    SDOperand &Offset);
  
  /// InstructionSelect - This callback is invoked by
  /// SelectionDAGISel when it has created a SelectionDAG for us to codegen.
  virtual void InstructionSelect(SelectionDAG &DAG);
  
  virtual const char *getPassName() const {
    return "SPARC DAG->DAG Pattern Instruction Selection";
  } 
  
  // Include the pieces autogenerated from the target description.
#include "SparcGenDAGISel.inc"
};
}  // end anonymous namespace

/// InstructionSelect - This callback is invoked by
/// SelectionDAGISel when it has created a SelectionDAG for us to codegen.
void SparcDAGToDAGISel::InstructionSelect(SelectionDAG &DAG) {
  DEBUG(BB->dump());
  
  // Select target instructions for the DAG.
  DAG.setRoot(SelectRoot(DAG.getRoot()));
  DAG.RemoveDeadNodes();
}

bool SparcDAGToDAGISel::SelectADDRri(SDOperand Op, SDOperand Addr,
                                     SDOperand &Base, SDOperand &Offset) {
  if (FrameIndexSDNode *FIN = dyn_cast<FrameIndexSDNode>(Addr)) {
    Base = CurDAG->getTargetFrameIndex(FIN->getIndex(), MVT::i32);
    Offset = CurDAG->getTargetConstant(0, MVT::i32);
    return true;
  }
  if (Addr.getOpcode() == ISD::TargetExternalSymbol ||
      Addr.getOpcode() == ISD::TargetGlobalAddress)
    return false;  // direct calls.
  
  if (Addr.getOpcode() == ISD::ADD) {
    if (ConstantSDNode *CN = dyn_cast<ConstantSDNode>(Addr.getOperand(1))) {
      if (Predicate_simm13(CN)) {
        if (FrameIndexSDNode *FIN = 
                dyn_cast<FrameIndexSDNode>(Addr.getOperand(0))) {
          // Constant offset from frame ref.
          Base = CurDAG->getTargetFrameIndex(FIN->getIndex(), MVT::i32);
        } else {
          Base = Addr.getOperand(0);
        }
        Offset = CurDAG->getTargetConstant(CN->getValue(), MVT::i32);
        return true;
      }
    }
    if (Addr.getOperand(0).getOpcode() == SPISD::Lo) {
      Base = Addr.getOperand(1);
      Offset = Addr.getOperand(0).getOperand(0);
      return true;
    }
    if (Addr.getOperand(1).getOpcode() == SPISD::Lo) {
      Base = Addr.getOperand(0);
      Offset = Addr.getOperand(1).getOperand(0);
      return true;
    }
  }
  Base = Addr;
  Offset = CurDAG->getTargetConstant(0, MVT::i32);
  return true;
}

bool SparcDAGToDAGISel::SelectADDRrr(SDOperand Op, SDOperand Addr,
                                     SDOperand &R1,  SDOperand &R2) {
  if (Addr.getOpcode() == ISD::FrameIndex) return false;
  if (Addr.getOpcode() == ISD::TargetExternalSymbol ||
      Addr.getOpcode() == ISD::TargetGlobalAddress)
    return false;  // direct calls.
  
  if (Addr.getOpcode() == ISD::ADD) {
    if (isa<ConstantSDNode>(Addr.getOperand(1)) &&
        Predicate_simm13(Addr.getOperand(1).Val))
      return false;  // Let the reg+imm pattern catch this!
    if (Addr.getOperand(0).getOpcode() == SPISD::Lo ||
        Addr.getOperand(1).getOpcode() == SPISD::Lo)
      return false;  // Let the reg+imm pattern catch this!
    R1 = Addr.getOperand(0);
    R2 = Addr.getOperand(1);
    return true;
  }

  R1 = Addr;
  R2 = CurDAG->getRegister(SP::G0, MVT::i32);
  return true;
}

SDNode *SparcDAGToDAGISel::Select(SDOperand Op) {
  SDNode *N = Op.Val;
  if (N->getOpcode() >= ISD::BUILTIN_OP_END &&
      N->getOpcode() < SPISD::FIRST_NUMBER)
    return NULL;   // Already selected.

  switch (N->getOpcode()) {
  default: break;
  case ISD::SDIV:
  case ISD::UDIV: {
    // FIXME: should use a custom expander to expose the SRA to the dag.
    SDOperand DivLHS = N->getOperand(0);
    SDOperand DivRHS = N->getOperand(1);
    AddToISelQueue(DivLHS);
    AddToISelQueue(DivRHS);
    
    // Set the Y register to the high-part.
    SDOperand TopPart;
    if (N->getOpcode() == ISD::SDIV) {
      TopPart = SDOperand(CurDAG->getTargetNode(SP::SRAri, MVT::i32, DivLHS,
                                   CurDAG->getTargetConstant(31, MVT::i32)), 0);
    } else {
      TopPart = CurDAG->getRegister(SP::G0, MVT::i32);
    }
    TopPart = SDOperand(CurDAG->getTargetNode(SP::WRYrr, MVT::Flag, TopPart,
                                     CurDAG->getRegister(SP::G0, MVT::i32)), 0);

    // FIXME: Handle div by immediate.
    unsigned Opcode = N->getOpcode() == ISD::SDIV ? SP::SDIVrr : SP::UDIVrr;
    return CurDAG->SelectNodeTo(N, Opcode, MVT::i32, DivLHS, DivRHS,
                                TopPart);
  }    
  case ISD::MULHU:
  case ISD::MULHS: {
    // FIXME: Handle mul by immediate.
    SDOperand MulLHS = N->getOperand(0);
    SDOperand MulRHS = N->getOperand(1);
    AddToISelQueue(MulLHS);
    AddToISelQueue(MulRHS);
    unsigned Opcode = N->getOpcode() == ISD::MULHU ? SP::UMULrr : SP::SMULrr;
    SDNode *Mul = CurDAG->getTargetNode(Opcode, MVT::i32, MVT::Flag,
                                        MulLHS, MulRHS);
    // The high part is in the Y register.
    return CurDAG->SelectNodeTo(N, SP::RDY, MVT::i32, SDOperand(Mul, 1));
    return NULL;
  }
  }
  
  return SelectCode(Op);
}


/// createSparcISelDag - This pass converts a legalized DAG into a 
/// SPARC-specific DAG, ready for instruction scheduling.
///
FunctionPass *llvm::createSparcISelDag(TargetMachine &TM) {
  return new SparcDAGToDAGISel(TM);
}
