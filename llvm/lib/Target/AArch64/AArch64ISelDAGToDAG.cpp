//===-- AArch64ISelDAGToDAG.cpp - A dag to dag inst selector for AArch64 --===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file defines an instruction selector for the AArch64 target.
//
//===----------------------------------------------------------------------===//

#define DEBUG_TYPE "aarch64-isel"
#include "AArch64.h"
#include "AArch64InstrInfo.h"
#include "AArch64Subtarget.h"
#include "AArch64TargetMachine.h"
#include "Utils/AArch64BaseInfo.h"
#include "llvm/ADT/APSInt.h"
#include "llvm/CodeGen/SelectionDAGISel.h"
#include "llvm/IR/GlobalValue.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;

//===--------------------------------------------------------------------===//
/// AArch64 specific code to select AArch64 machine instructions for
/// SelectionDAG operations.
///
namespace {

class AArch64DAGToDAGISel : public SelectionDAGISel {
  AArch64TargetMachine &TM;
  const AArch64InstrInfo *TII;

  /// Keep a pointer to the AArch64Subtarget around so that we can
  /// make the right decision when generating code for different targets.
  const AArch64Subtarget *Subtarget;

public:
  explicit AArch64DAGToDAGISel(AArch64TargetMachine &tm,
                               CodeGenOpt::Level OptLevel)
    : SelectionDAGISel(tm, OptLevel), TM(tm),
      TII(static_cast<const AArch64InstrInfo*>(TM.getInstrInfo())),
      Subtarget(&TM.getSubtarget<AArch64Subtarget>()) {
  }

  virtual const char *getPassName() const {
    return "AArch64 Instruction Selection";
  }

  // Include the pieces autogenerated from the target description.
#include "AArch64GenDAGISel.inc"

  template<unsigned MemSize>
  bool SelectOffsetUImm12(SDValue N, SDValue &UImm12) {
    const ConstantSDNode *CN = dyn_cast<ConstantSDNode>(N);
    if (!CN || CN->getZExtValue() % MemSize != 0
        || CN->getZExtValue() / MemSize > 0xfff)
      return false;

    UImm12 =  CurDAG->getTargetConstant(CN->getZExtValue() / MemSize, MVT::i64);
    return true;
  }

  template<unsigned RegWidth>
  bool SelectCVTFixedPosOperand(SDValue N, SDValue &FixedPos) {
    return SelectCVTFixedPosOperand(N, FixedPos, RegWidth);
  }

  /// Used for pre-lowered address-reference nodes, so we already know
  /// the fields match. This operand's job is simply to add an
  /// appropriate shift operand (i.e. 0) to the MOVZ/MOVK instruction.
  bool SelectMOVWAddressRef(SDValue N, SDValue &Imm, SDValue &Shift) {
    Imm = N;
    Shift = CurDAG->getTargetConstant(0, MVT::i32);
    return true;
  }

  bool SelectFPZeroOperand(SDValue N, SDValue &Dummy);

  bool SelectCVTFixedPosOperand(SDValue N, SDValue &FixedPos,
                                unsigned RegWidth);

  bool SelectInlineAsmMemoryOperand(const SDValue &Op,
                                    char ConstraintCode,
                                    std::vector<SDValue> &OutOps);

  bool SelectLogicalImm(SDValue N, SDValue &Imm);

  template<unsigned RegWidth>
  bool SelectTSTBOperand(SDValue N, SDValue &FixedPos) {
    return SelectTSTBOperand(N, FixedPos, RegWidth);
  }

  bool SelectTSTBOperand(SDValue N, SDValue &FixedPos, unsigned RegWidth);

  SDNode *SelectAtomic(SDNode *N, unsigned Op8, unsigned Op16, unsigned Op32, unsigned Op64);

  SDNode *TrySelectToMoveImm(SDNode *N);
  SDNode *LowerToFPLitPool(SDNode *Node);
  SDNode *SelectToLitPool(SDNode *N);

  SDNode* Select(SDNode*);
private:
};
}

bool
AArch64DAGToDAGISel::SelectCVTFixedPosOperand(SDValue N, SDValue &FixedPos,
                                              unsigned RegWidth) {
  const ConstantFPSDNode *CN = dyn_cast<ConstantFPSDNode>(N);
  if (!CN) return false;

  // An FCVT[SU] instruction performs: convertToInt(Val * 2^fbits) where fbits
  // is between 1 and 32 for a destination w-register, or 1 and 64 for an
  // x-register.
  //
  // By this stage, we've detected (fp_to_[su]int (fmul Val, THIS_NODE)) so we
  // want THIS_NODE to be 2^fbits. This is much easier to deal with using
  // integers.
  bool IsExact;

  // fbits is between 1 and 64 in the worst-case, which means the fmul
  // could have 2^64 as an actual operand. Need 65 bits of precision.
  APSInt IntVal(65, true);
  CN->getValueAPF().convertToInteger(IntVal, APFloat::rmTowardZero, &IsExact);

  // N.b. isPowerOf2 also checks for > 0.
  if (!IsExact || !IntVal.isPowerOf2()) return false;
  unsigned FBits = IntVal.logBase2();

  // Checks above should have guaranteed that we haven't lost information in
  // finding FBits, but it must still be in range.
  if (FBits == 0 || FBits > RegWidth) return false;

  FixedPos = CurDAG->getTargetConstant(64 - FBits, MVT::i32);
  return true;
}

bool
AArch64DAGToDAGISel::SelectInlineAsmMemoryOperand(const SDValue &Op,
                                                 char ConstraintCode,
                                                 std::vector<SDValue> &OutOps) {
  switch (ConstraintCode) {
  default: llvm_unreachable("Unrecognised AArch64 memory constraint");
  case 'm':
    // FIXME: more freedom is actually permitted for 'm'. We can go
    // hunting for a base and an offset if we want. Of course, since
    // we don't really know how the operand is going to be used we're
    // probably restricted to the load/store pair's simm7 as an offset
    // range anyway.
  case 'Q':
    OutOps.push_back(Op);
  }

  return false;
}

bool
AArch64DAGToDAGISel::SelectFPZeroOperand(SDValue N, SDValue &Dummy) {
  ConstantFPSDNode *Imm = dyn_cast<ConstantFPSDNode>(N);
  if (!Imm || !Imm->getValueAPF().isPosZero())
    return false;

  // Doesn't actually carry any information, but keeps TableGen quiet.
  Dummy = CurDAG->getTargetConstant(0, MVT::i32);
  return true;
}

bool AArch64DAGToDAGISel::SelectLogicalImm(SDValue N, SDValue &Imm) {
  uint32_t Bits;
  uint32_t RegWidth = N.getValueType().getSizeInBits();

  ConstantSDNode *CN = dyn_cast<ConstantSDNode>(N);
  if (!CN) return false;

  if (!A64Imms::isLogicalImm(RegWidth, CN->getZExtValue(), Bits))
    return false;

  Imm = CurDAG->getTargetConstant(Bits, MVT::i32);
  return true;
}

SDNode *AArch64DAGToDAGISel::TrySelectToMoveImm(SDNode *Node) {
  SDNode *ResNode;
  DebugLoc dl = Node->getDebugLoc();
  EVT DestType = Node->getValueType(0);
  unsigned DestWidth = DestType.getSizeInBits();

  unsigned MOVOpcode;
  EVT MOVType;
  int UImm16, Shift;
  uint32_t LogicalBits;

  uint64_t BitPat = cast<ConstantSDNode>(Node)->getZExtValue();
  if (A64Imms::isMOVZImm(DestWidth, BitPat, UImm16, Shift)) {
    MOVType = DestType;
    MOVOpcode = DestWidth == 64 ? AArch64::MOVZxii : AArch64::MOVZwii;
  } else if (A64Imms::isMOVNImm(DestWidth, BitPat, UImm16, Shift)) {
    MOVType = DestType;
    MOVOpcode = DestWidth == 64 ? AArch64::MOVNxii : AArch64::MOVNwii;
  } else if (DestWidth == 64 && A64Imms::isMOVNImm(32, BitPat, UImm16, Shift)) {
    // To get something like 0x0000_0000_ffff_1234 into a 64-bit register we can
    // use a 32-bit instruction: "movn w0, 0xedbc".
    MOVType = MVT::i32;
    MOVOpcode = AArch64::MOVNwii;
  } else if (A64Imms::isLogicalImm(DestWidth, BitPat, LogicalBits))  {
    MOVOpcode = DestWidth == 64 ? AArch64::ORRxxi : AArch64::ORRwwi;
    uint16_t ZR = DestWidth == 64 ? AArch64::XZR : AArch64::WZR;

    return CurDAG->getMachineNode(MOVOpcode, dl, DestType,
                              CurDAG->getRegister(ZR, DestType),
                              CurDAG->getTargetConstant(LogicalBits, MVT::i32));
  } else {
    // Can't handle it in one instruction. There's scope for permitting two (or
    // more) instructions, but that'll need more thought.
    return NULL;
  }

  ResNode = CurDAG->getMachineNode(MOVOpcode, dl, MOVType,
                                   CurDAG->getTargetConstant(UImm16, MVT::i32),
                                   CurDAG->getTargetConstant(Shift, MVT::i32));

  if (MOVType != DestType) {
    ResNode = CurDAG->getMachineNode(TargetOpcode::SUBREG_TO_REG, dl,
                          MVT::i64, MVT::i32, MVT::Other,
                          CurDAG->getTargetConstant(0, MVT::i64),
                          SDValue(ResNode, 0),
                          CurDAG->getTargetConstant(AArch64::sub_32, MVT::i32));
  }

  return ResNode;
}

SDNode *AArch64DAGToDAGISel::SelectToLitPool(SDNode *Node) {
  DebugLoc DL = Node->getDebugLoc();
  uint64_t UnsignedVal = cast<ConstantSDNode>(Node)->getZExtValue();
  int64_t SignedVal = cast<ConstantSDNode>(Node)->getSExtValue();
  EVT DestType = Node->getValueType(0);
  EVT PtrVT = TLI.getPointerTy();

  // Since we may end up loading a 64-bit constant from a 32-bit entry the
  // constant in the pool may have a different type to the eventual node.
  ISD::LoadExtType Extension;
  EVT MemType;

  assert((DestType == MVT::i64 || DestType == MVT::i32)
         && "Only expect integer constants at the moment");

  if (DestType == MVT::i32) {
    Extension = ISD::NON_EXTLOAD;
    MemType = MVT::i32;
  } else if (UnsignedVal <= UINT32_MAX) {
    Extension = ISD::ZEXTLOAD;
    MemType = MVT::i32;
  } else if (SignedVal >= INT32_MIN && SignedVal <= INT32_MAX) {
    Extension = ISD::SEXTLOAD;
    MemType = MVT::i32;
  } else {
    Extension = ISD::NON_EXTLOAD;
    MemType = MVT::i64;
  }

  Constant *CV = ConstantInt::get(Type::getIntNTy(*CurDAG->getContext(),
                                                  MemType.getSizeInBits()),
                                  UnsignedVal);
  SDValue PoolAddr;
  unsigned Alignment = TLI.getDataLayout()->getABITypeAlignment(CV->getType());
  PoolAddr = CurDAG->getNode(AArch64ISD::WrapperSmall, DL, PtrVT,
                             CurDAG->getTargetConstantPool(CV, PtrVT, 0, 0,
                                                         AArch64II::MO_NO_FLAG),
                             CurDAG->getTargetConstantPool(CV, PtrVT, 0, 0,
                                                           AArch64II::MO_LO12),
                             CurDAG->getConstant(Alignment, MVT::i32));

  return CurDAG->getExtLoad(Extension, DL, DestType, CurDAG->getEntryNode(),
                            PoolAddr,
                            MachinePointerInfo::getConstantPool(), MemType,
                            /* isVolatile = */ false,
                            /* isNonTemporal = */ false,
                            Alignment).getNode();
}

SDNode *AArch64DAGToDAGISel::LowerToFPLitPool(SDNode *Node) {
  DebugLoc DL = Node->getDebugLoc();
  const ConstantFP *FV = cast<ConstantFPSDNode>(Node)->getConstantFPValue();
  EVT PtrVT = TLI.getPointerTy();
  EVT DestType = Node->getValueType(0);

  unsigned Alignment = TLI.getDataLayout()->getABITypeAlignment(FV->getType());
  SDValue PoolAddr;

  assert(TM.getCodeModel() == CodeModel::Small &&
         "Only small code model supported");
  PoolAddr = CurDAG->getNode(AArch64ISD::WrapperSmall, DL, PtrVT,
                             CurDAG->getTargetConstantPool(FV, PtrVT, 0, 0,
                                                         AArch64II::MO_NO_FLAG),
                             CurDAG->getTargetConstantPool(FV, PtrVT, 0, 0,
                                                           AArch64II::MO_LO12),
                             CurDAG->getConstant(Alignment, MVT::i32));

  return CurDAG->getLoad(DestType, DL, CurDAG->getEntryNode(), PoolAddr,
                         MachinePointerInfo::getConstantPool(),
                         /* isVolatile = */ false,
                         /* isNonTemporal = */ false,
                         /* isInvariant = */ true,
                         Alignment).getNode();
}

bool
AArch64DAGToDAGISel::SelectTSTBOperand(SDValue N, SDValue &FixedPos,
                                       unsigned RegWidth) {
  const ConstantSDNode *CN = dyn_cast<ConstantSDNode>(N);
  if (!CN) return false;

  uint64_t Val = CN->getZExtValue();

  if (!isPowerOf2_64(Val)) return false;

  unsigned TestedBit = Log2_64(Val);
  // Checks above should have guaranteed that we haven't lost information in
  // finding TestedBit, but it must still be in range.
  if (TestedBit >= RegWidth) return false;

  FixedPos = CurDAG->getTargetConstant(TestedBit, MVT::i64);
  return true;
}

SDNode *AArch64DAGToDAGISel::SelectAtomic(SDNode *Node, unsigned Op8,
                                          unsigned Op16,unsigned Op32,
                                          unsigned Op64) {
  // Mostly direct translation to the given operations, except that we preserve
  // the AtomicOrdering for use later on.
  AtomicSDNode *AN = cast<AtomicSDNode>(Node);
  EVT VT = AN->getMemoryVT();

  unsigned Op;
  if (VT == MVT::i8)
    Op = Op8;
  else if (VT == MVT::i16)
    Op = Op16;
  else if (VT == MVT::i32)
    Op = Op32;
  else if (VT == MVT::i64)
    Op = Op64;
  else
    llvm_unreachable("Unexpected atomic operation");

  SmallVector<SDValue, 4> Ops;
  for (unsigned i = 1; i < AN->getNumOperands(); ++i)
      Ops.push_back(AN->getOperand(i));

  Ops.push_back(CurDAG->getTargetConstant(AN->getOrdering(), MVT::i32));
  Ops.push_back(AN->getOperand(0)); // Chain moves to the end

  return CurDAG->SelectNodeTo(Node, Op,
                              AN->getValueType(0), MVT::Other,
                              &Ops[0], Ops.size());
}

SDNode *AArch64DAGToDAGISel::Select(SDNode *Node) {
  // Dump information about the Node being selected
  DEBUG(dbgs() << "Selecting: "; Node->dump(CurDAG); dbgs() << "\n");

  if (Node->isMachineOpcode()) {
    DEBUG(dbgs() << "== "; Node->dump(CurDAG); dbgs() << "\n");
    return NULL;
  }

  switch (Node->getOpcode()) {
  case ISD::ATOMIC_LOAD_ADD:
    return SelectAtomic(Node,
                        AArch64::ATOMIC_LOAD_ADD_I8,
                        AArch64::ATOMIC_LOAD_ADD_I16,
                        AArch64::ATOMIC_LOAD_ADD_I32,
                        AArch64::ATOMIC_LOAD_ADD_I64);
  case ISD::ATOMIC_LOAD_SUB:
    return SelectAtomic(Node,
                        AArch64::ATOMIC_LOAD_SUB_I8,
                        AArch64::ATOMIC_LOAD_SUB_I16,
                        AArch64::ATOMIC_LOAD_SUB_I32,
                        AArch64::ATOMIC_LOAD_SUB_I64);
  case ISD::ATOMIC_LOAD_AND:
    return SelectAtomic(Node,
                        AArch64::ATOMIC_LOAD_AND_I8,
                        AArch64::ATOMIC_LOAD_AND_I16,
                        AArch64::ATOMIC_LOAD_AND_I32,
                        AArch64::ATOMIC_LOAD_AND_I64);
  case ISD::ATOMIC_LOAD_OR:
    return SelectAtomic(Node,
                        AArch64::ATOMIC_LOAD_OR_I8,
                        AArch64::ATOMIC_LOAD_OR_I16,
                        AArch64::ATOMIC_LOAD_OR_I32,
                        AArch64::ATOMIC_LOAD_OR_I64);
  case ISD::ATOMIC_LOAD_XOR:
    return SelectAtomic(Node,
                        AArch64::ATOMIC_LOAD_XOR_I8,
                        AArch64::ATOMIC_LOAD_XOR_I16,
                        AArch64::ATOMIC_LOAD_XOR_I32,
                        AArch64::ATOMIC_LOAD_XOR_I64);
  case ISD::ATOMIC_LOAD_NAND:
    return SelectAtomic(Node,
                        AArch64::ATOMIC_LOAD_NAND_I8,
                        AArch64::ATOMIC_LOAD_NAND_I16,
                        AArch64::ATOMIC_LOAD_NAND_I32,
                        AArch64::ATOMIC_LOAD_NAND_I64);
  case ISD::ATOMIC_LOAD_MIN:
    return SelectAtomic(Node,
                        AArch64::ATOMIC_LOAD_MIN_I8,
                        AArch64::ATOMIC_LOAD_MIN_I16,
                        AArch64::ATOMIC_LOAD_MIN_I32,
                        AArch64::ATOMIC_LOAD_MIN_I64);
  case ISD::ATOMIC_LOAD_MAX:
    return SelectAtomic(Node,
                        AArch64::ATOMIC_LOAD_MAX_I8,
                        AArch64::ATOMIC_LOAD_MAX_I16,
                        AArch64::ATOMIC_LOAD_MAX_I32,
                        AArch64::ATOMIC_LOAD_MAX_I64);
  case ISD::ATOMIC_LOAD_UMIN:
    return SelectAtomic(Node,
                        AArch64::ATOMIC_LOAD_UMIN_I8,
                        AArch64::ATOMIC_LOAD_UMIN_I16,
                        AArch64::ATOMIC_LOAD_UMIN_I32,
                        AArch64::ATOMIC_LOAD_UMIN_I64);
  case ISD::ATOMIC_LOAD_UMAX:
    return SelectAtomic(Node,
                        AArch64::ATOMIC_LOAD_UMAX_I8,
                        AArch64::ATOMIC_LOAD_UMAX_I16,
                        AArch64::ATOMIC_LOAD_UMAX_I32,
                        AArch64::ATOMIC_LOAD_UMAX_I64);
  case ISD::ATOMIC_SWAP:
    return SelectAtomic(Node,
                        AArch64::ATOMIC_SWAP_I8,
                        AArch64::ATOMIC_SWAP_I16,
                        AArch64::ATOMIC_SWAP_I32,
                        AArch64::ATOMIC_SWAP_I64);
  case ISD::ATOMIC_CMP_SWAP:
    return SelectAtomic(Node,
                        AArch64::ATOMIC_CMP_SWAP_I8,
                        AArch64::ATOMIC_CMP_SWAP_I16,
                        AArch64::ATOMIC_CMP_SWAP_I32,
                        AArch64::ATOMIC_CMP_SWAP_I64);
  case ISD::FrameIndex: {
    int FI = cast<FrameIndexSDNode>(Node)->getIndex();
    EVT PtrTy = TLI.getPointerTy();
    SDValue TFI = CurDAG->getTargetFrameIndex(FI, PtrTy);
    return CurDAG->SelectNodeTo(Node, AArch64::ADDxxi_lsl0_s, PtrTy,
                                TFI, CurDAG->getTargetConstant(0, PtrTy));
  }
  case ISD::ConstantPool: {
    // Constant pools are fine, just create a Target entry.
    ConstantPoolSDNode *CN = cast<ConstantPoolSDNode>(Node);
    const Constant *C = CN->getConstVal();
    SDValue CP = CurDAG->getTargetConstantPool(C, CN->getValueType(0));

    ReplaceUses(SDValue(Node, 0), CP);
    return NULL;
  }
  case ISD::Constant: {
    SDNode *ResNode = 0;
    if (cast<ConstantSDNode>(Node)->getZExtValue() == 0) {
      // XZR and WZR are probably even better than an actual move: most of the
      // time they can be folded into another instruction with *no* cost.

      EVT Ty = Node->getValueType(0);
      assert((Ty == MVT::i32 || Ty == MVT::i64) && "unexpected type");
      uint16_t Register = Ty == MVT::i32 ? AArch64::WZR : AArch64::XZR;
      ResNode = CurDAG->getCopyFromReg(CurDAG->getEntryNode(),
                                       Node->getDebugLoc(),
                                       Register, Ty).getNode();
    }

    // Next best option is a move-immediate, see if we can do that.
    if (!ResNode) {
      ResNode = TrySelectToMoveImm(Node);
    }

    if (ResNode)
      return ResNode;

    // If even that fails we fall back to a lit-pool entry at the moment. Future
    // tuning may change this to a sequence of MOVZ/MOVN/MOVK instructions.
    ResNode = SelectToLitPool(Node);
    assert(ResNode && "We need *some* way to materialise a constant");

    // We want to continue selection at this point since the litpool access
    // generated used generic nodes for simplicity.
    ReplaceUses(SDValue(Node, 0), SDValue(ResNode, 0));
    Node = ResNode;
    break;
  }
  case ISD::ConstantFP: {
    if (A64Imms::isFPImm(cast<ConstantFPSDNode>(Node)->getValueAPF())) {
      // FMOV will take care of it from TableGen
      break;
    }

    SDNode *ResNode = LowerToFPLitPool(Node);
    ReplaceUses(SDValue(Node, 0), SDValue(ResNode, 0));

    // We want to continue selection at this point since the litpool access
    // generated used generic nodes for simplicity.
    Node = ResNode;
    break;
  }
  default:
    break; // Let generic code handle it
  }

  SDNode *ResNode = SelectCode(Node);

  DEBUG(dbgs() << "=> ";
        if (ResNode == NULL || ResNode == Node)
          Node->dump(CurDAG);
        else
          ResNode->dump(CurDAG);
        dbgs() << "\n");

  return ResNode;
}

/// This pass converts a legalized DAG into a AArch64-specific DAG, ready for
/// instruction scheduling.
FunctionPass *llvm::createAArch64ISelDAG(AArch64TargetMachine &TM,
                                         CodeGenOpt::Level OptLevel) {
  return new AArch64DAGToDAGISel(TM, OptLevel);
}
