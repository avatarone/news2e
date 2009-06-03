//===-- Expr.cpp ----------------------------------------------------------===//
//
//                     The KLEE Symbolic Virtual Machine
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "klee/Expr.h"


#include "klee/Machine.h"
// FIXME: This shouldn't be here.
//#include "klee/Memory.h"
#include "llvm/Type.h"
#include "llvm/DerivedTypes.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Streams.h"
// FIXME: We shouldn't need this once fast constant support moves into
// Core. If we need to do arithmetic, we probably want to use APInt.
#include "klee/Internal/Support/IntEvaluation.h"

#include "klee/util/ExprPPrinter.h"

using namespace klee;
using namespace llvm;

namespace {
  cl::opt<bool>
  ConstArrayOpt("const-array-opt",
	 cl::init(false),
	 cl::desc("Enable various optimizations involving all-constant arrays."));
}

/***/

unsigned Expr::count = 0;

ref<Expr> Expr::createTempRead(const Array *array, Expr::Width w) {
  UpdateList ul(array, true, 0);

  switch (w) {
  case Expr::Bool: 
    return ZExtExpr::create(ReadExpr::create(ul, 
                                             ConstantExpr::alloc(0,kMachinePointerType)),
                            Expr::Bool);
  case Expr::Int8: 
    return ReadExpr::create(ul, 
                            ConstantExpr::alloc(0,kMachinePointerType));
  case Expr::Int16: 
    return ConcatExpr::create(ReadExpr::create(ul, 
                                               ConstantExpr::alloc(1,kMachinePointerType)),
                              ReadExpr::create(ul, 
                                               ConstantExpr::alloc(0,kMachinePointerType)));
  case Expr::Int32: 
    return ConcatExpr::create4(ReadExpr::create(ul, 
                                                ConstantExpr::alloc(3,kMachinePointerType)),
                               ReadExpr::create(ul, 
                                                ConstantExpr::alloc(2,kMachinePointerType)),
                               ReadExpr::create(ul, 
                                                ConstantExpr::alloc(1,kMachinePointerType)),
                               ReadExpr::create(ul, 
                                                ConstantExpr::alloc(0,kMachinePointerType)));
  case Expr::Int64: 
    return ConcatExpr::create8(ReadExpr::create(ul, 
                                                ConstantExpr::alloc(7,kMachinePointerType)),
                               ReadExpr::create(ul, 
                                                ConstantExpr::alloc(6,kMachinePointerType)),
                               ReadExpr::create(ul, 
                                                ConstantExpr::alloc(5,kMachinePointerType)),
                               ReadExpr::create(ul, 
                                                ConstantExpr::alloc(4,kMachinePointerType)),
                               ReadExpr::create(ul, 
                                                ConstantExpr::alloc(3,kMachinePointerType)),
                               ReadExpr::create(ul, 
                                                ConstantExpr::alloc(2,kMachinePointerType)),
                               ReadExpr::create(ul, 
                                                ConstantExpr::alloc(1,kMachinePointerType)),
                               ReadExpr::create(ul, 
                                                ConstantExpr::alloc(0,kMachinePointerType)));
  default: assert(0 && "invalid width");
  }
}

// returns 0 if b is structurally equal to *this
int Expr::compare(const Expr &b) const {
  if (this == &b) return 0;

  Kind ak = getKind(), bk = b.getKind();
  if (ak!=bk)
    return (ak < bk) ? -1 : 1;

  if (hashValue != b.hashValue) 
    return (hashValue < b.hashValue) ? -1 : 1;

  if (int res = compareContents(b)) 
    return res;

  unsigned aN = getNumKids();
  for (unsigned i=0; i<aN; i++)
    if (int res = getKid(i).compare(b.getKid(i)))
      return res;

  return 0;
}

void Expr::printKind(std::ostream &os, Kind k) {
  switch(k) {
#define X(C) case C: os << #C; break
    X(Constant);
    X(NotOptimized);
    X(Read);
    X(Select);
    X(Concat);
    X(Extract);
    X(ZExt);
    X(SExt);
    X(Add);
    X(Sub);
    X(Mul);
    X(UDiv);
    X(SDiv);
    X(URem);
    X(SRem);
    X(And);
    X(Or);
    X(Xor);
    X(Shl);
    X(LShr);
    X(AShr);
    X(Eq);
    X(Ne);
    X(Ult);
    X(Ule);
    X(Ugt);
    X(Uge);
    X(Slt);
    X(Sle);
    X(Sgt);
    X(Sge);
#undef X
  default:
    assert(0 && "invalid kind");
    }
}

////////
//
// Simple hash functions for various kinds of Exprs
//
///////

unsigned Expr::computeHash() {
  unsigned res = getKind() * Expr::MAGIC_HASH_CONSTANT;

  int n = getNumKids();
  for (int i = 0; i < n; i++) {
    res <<= 1;
    res ^= getKid(i).hash() * Expr::MAGIC_HASH_CONSTANT;
  }
  
  hashValue = res;
  return hashValue;
}

unsigned ConstantExpr::computeHash() {
  return asUInt64 ^ (width * MAGIC_HASH_CONSTANT);
}

unsigned CastExpr::computeHash() {
  unsigned res = getWidth() * Expr::MAGIC_HASH_CONSTANT;
  hashValue = res ^ src.hash() * Expr::MAGIC_HASH_CONSTANT;
  return hashValue;
}

unsigned ExtractExpr::computeHash() {
  unsigned res = offset * Expr::MAGIC_HASH_CONSTANT;
  res ^= getWidth() * Expr::MAGIC_HASH_CONSTANT;
  hashValue = res ^ expr.hash() * Expr::MAGIC_HASH_CONSTANT;
  return hashValue;
}

unsigned ReadExpr::computeHash() {
  unsigned res = index.hash() * Expr::MAGIC_HASH_CONSTANT;
  res ^= updates.hash();
  hashValue = res;
  return hashValue;
}

uint64_t Expr::getConstantValue() const {
  assert(getKind() == Constant);
  return static_cast<const ConstantExpr*>(this)->asUInt64;
}

ref<Expr> Expr::createFromKind(Kind k, std::vector<CreateArg> args) {
  unsigned numArgs = args.size();
  
  switch(k) {
    case NotOptimized:
      assert(numArgs == 1 && args[0].isExpr() &&
             "invalid args array for given opcode");
      return NotOptimizedExpr::create(args[0].expr);
      
    case Select:
      assert(numArgs == 3 && args[0].isExpr() &&
             args[1].isExpr() && args[2].isExpr() &&
             "invalid args array for Select opcode");
      return SelectExpr::create(args[0].expr,
                                args[1].expr,
                                args[2].expr);

    case Concat: {
      assert(numArgs == 2 && args[0].isExpr() && args[1].isExpr() && 
             "invalid args array for Concat opcode");
      
      return ConcatExpr::create(args[0].expr, args[1].expr);
    }
      
#define CAST_EXPR_CASE(T)                                    \
      case T:                                                \
        assert(numArgs == 2 &&				     \
               args[0].isExpr() && args[1].isWidth() &&      \
               "invalid args array for given opcode");       \
      return T ## Expr::create(args[0].expr, args[1].width); \
      
#define BINARY_EXPR_CASE(T)                                 \
      case T:                                               \
        assert(numArgs == 2 &&                              \
               args[0].isExpr() && args[1].isExpr() &&      \
               "invalid args array for given opcode");      \
      return T ## Expr::create(args[0].expr, args[1].expr); \

      CAST_EXPR_CASE(ZExt);
      CAST_EXPR_CASE(SExt);
      
      BINARY_EXPR_CASE(Add);
      BINARY_EXPR_CASE(Sub);
      BINARY_EXPR_CASE(Mul);
      BINARY_EXPR_CASE(UDiv);
      BINARY_EXPR_CASE(SDiv);
      BINARY_EXPR_CASE(URem);
      BINARY_EXPR_CASE(SRem);
      BINARY_EXPR_CASE(And);
      BINARY_EXPR_CASE(Or);
      BINARY_EXPR_CASE(Xor);
      BINARY_EXPR_CASE(Shl);
      BINARY_EXPR_CASE(LShr);
      BINARY_EXPR_CASE(AShr);
      
      BINARY_EXPR_CASE(Eq);
      BINARY_EXPR_CASE(Ne);
      BINARY_EXPR_CASE(Ult);
      BINARY_EXPR_CASE(Ule);
      BINARY_EXPR_CASE(Ugt);
      BINARY_EXPR_CASE(Uge);
      BINARY_EXPR_CASE(Slt);
      BINARY_EXPR_CASE(Sle);
      BINARY_EXPR_CASE(Sgt);
      BINARY_EXPR_CASE(Sge);

    case Constant:
    case Extract:
    case Read:
    default:
      assert(0 && "invalid kind");
  }

}


void Expr::printWidth(std::ostream &os, Width width) {
  switch(width) {
  case Expr::Bool: os << "Expr::Bool"; break;
  case Expr::Int8: os << "Expr::Int8"; break;
  case Expr::Int16: os << "Expr::Int16"; break;
  case Expr::Int32: os << "Expr::Int32"; break;
  case Expr::Int64: os << "Expr::Int64"; break;
  default: os << "<invalid type: " << (unsigned) width << ">";
  }
}

Expr::Width Expr::getWidthForLLVMType(const llvm::Type *t) {
  switch (t->getTypeID()) {
  case llvm::Type::IntegerTyID: {
    Width w = cast<IntegerType>(t)->getBitWidth();

    // should remove this limitation soon
    if (w == 1 || w == 8 || w == 16 || w == 32 || w == 64)
      return w;
    else {
      assert(0 && "XXX arbitrary bit widths unsupported");
      abort();
    }
  }
  case llvm::Type::FloatTyID: return Expr::Int32;
  case llvm::Type::DoubleTyID: return Expr::Int64;
  case llvm::Type::X86_FP80TyID: return Expr::Int64; // XXX: needs to be fixed
  case llvm::Type::PointerTyID: return kMachinePointerType;
  default:
    cerr << "non-primitive type argument to Expr::getTypeForLLVMType()\n";
    abort();
  }
}

ref<Expr> Expr::createImplies(ref<Expr> hyp, ref<Expr> conc) {
  return OrExpr::create(Expr::createNot(hyp), conc);
}

ref<Expr> Expr::createIsZero(ref<Expr> e) {
  return EqExpr::create(e, ConstantExpr::create(0, e.getWidth()));
}

ref<Expr> Expr::createCoerceToPointerType(ref<Expr> e) {
  return ZExtExpr::create(e, kMachinePointerType);
}

ref<Expr> Expr::createNot(ref<Expr> e) {
  return createIsZero(e);
}

ref<Expr> Expr::createPointer(uint64_t v) {
  return ConstantExpr::create(v, kMachinePointerType);
}

Expr* Expr::createConstant(uint64_t val, Width w) {
  Expr *r = new ConstantExpr(val, w);
  r->computeHash();
  return r;
}

void Expr::print(std::ostream &os) const {
  const ref<Expr> tmp((Expr*)this);
  ExprPPrinter::printOne(os, "", tmp);
}

/***/

ref<ConstantExpr> ConstantExpr::fromMemory(void *address, Width width) {
  switch (width) {
  case  Expr::Bool: return ConstantExpr::create(*(( uint8_t*) address), width);
  case  Expr::Int8: return ConstantExpr::create(*(( uint8_t*) address), width);
  case Expr::Int16: return ConstantExpr::create(*((uint16_t*) address), width);
  case Expr::Int32: return ConstantExpr::create(*((uint32_t*) address), width);
  case Expr::Int64: return ConstantExpr::create(*((uint64_t*) address), width);
  default: assert(0 && "invalid type");
  }
}

void ConstantExpr::toMemory(void *address) {
  switch (width) {
  case  Expr::Bool: *(( uint8_t*) address) = asUInt64; break;
  case  Expr::Int8: *(( uint8_t*) address) = asUInt64; break;
  case Expr::Int16: *((uint16_t*) address) = asUInt64; break;
  case Expr::Int32: *((uint32_t*) address) = asUInt64; break;
  case Expr::Int64: *((uint64_t*) address) = asUInt64; break;
  default: assert(0 && "invalid type");
  }
}

/***/

ref<Expr>  NotOptimizedExpr::create(ref<Expr> src) {
  return NotOptimizedExpr::alloc(src);
}

ref<Expr> ReadExpr::create(const UpdateList &ul, ref<Expr> index) {
  // rollback index when possible... 

  // XXX this doesn't really belong here... there are basically two
  // cases, one is rebuild, where we want to optimistically try various
  // optimizations when the index has changed, and the other is 
  // initial creation, where we expect the ObjectState to have constructed
  // a smart UpdateList so it is not worth rescanning.

  const UpdateNode *un = ul.head;
  for (; un; un=un->next) {
    ref<Expr> cond = EqExpr::create(index, un->index);
    
    if (cond.isConstant()) {
      if (cond.getConstantValue())
        return un->value;
    } else {
      break;
    }
  }

  return ReadExpr::alloc(ul, index);
}

int ReadExpr::compareContents(const Expr &b) const { 
  return updates.compare(static_cast<const ReadExpr&>(b).updates);
}

ref<Expr> SelectExpr::create(ref<Expr> c, ref<Expr> t, ref<Expr> f) {
  Expr::Width kt = t.getWidth();

  assert(c.getWidth()==Bool && "type mismatch");
  assert(kt==f.getWidth() && "type mismatch");

  if (c.isConstant()) {
    return c.getConstantValue() ? t : f;
  } else if (t==f) {
    return t;
  } else if (kt==Expr::Bool) { // c ? t : f  <=> (c and t) or (not c and f)
    if (t.isConstant()) {      
      if (t.getConstantValue()) {
        return OrExpr::create(c, f);
      } else {
        return AndExpr::create(Expr::createNot(c), f);
      }
    } else if (f.isConstant()) {
      if (f.getConstantValue()) {
        return OrExpr::create(Expr::createNot(c), t);
      } else {
        return AndExpr::create(c, t);
      }
    }
  }
  
  return SelectExpr::alloc(c, t, f);
}

/***/


ref<Expr> ConcatExpr::create(const ref<Expr> &l, const ref<Expr> &r) {
  Expr::Width w = l.getWidth() + r.getWidth();
  
  /* Constant folding */
  if (l.getKind() == Expr::Constant && r.getKind() == Expr::Constant) {
    // XXX: should fix this constant limitation soon
    assert(w <= 64 && "ConcatExpr::create(): don't support concats describing constants greater than 64 bits yet");
    
    uint64_t res = (l.getConstantValue() << r.getWidth()) + r.getConstantValue();
    return ConstantExpr::create(res, w);
  }

  // Merge contiguous Extracts
  if (l.getKind() == Expr::Extract && r.getKind() == Expr::Extract) {
    const ExtractExpr* ee_left = static_ref_cast<ExtractExpr>(l);
    const ExtractExpr* ee_right = static_ref_cast<ExtractExpr>(r);
    if (ee_left->expr == ee_right->expr &&
	ee_right->offset + ee_right->width == ee_left->offset) {
      return ExtractExpr::create(ee_left->expr, ee_right->offset, w);
    }
  }

  return ConcatExpr::alloc(l, r);
}

/// Shortcut to concat N kids.  The chain returned is unbalanced to the right
ref<Expr> ConcatExpr::createN(unsigned n_kids, const ref<Expr> kids[]) {
  assert(n_kids > 0);
  if (n_kids == 1)
    return kids[0];
  
  ref<Expr> r = ConcatExpr::create(kids[n_kids-2], kids[n_kids-1]);
  for (int i=n_kids-3; i>=0; i--)
    r = ConcatExpr::create(kids[i], r);
  return r;
}

/// Shortcut to concat 4 kids.  The chain returned is unbalanced to the right
ref<Expr> ConcatExpr::create4(const ref<Expr> &kid1, const ref<Expr> &kid2,
				     const ref<Expr> &kid3, const ref<Expr> &kid4) {
  return ConcatExpr::create(kid1, ConcatExpr::create(kid2, ConcatExpr::create(kid3, kid4)));
}

/// Shortcut to concat 8 kids.  The chain returned is unbalanced to the right
ref<Expr> ConcatExpr::create8(const ref<Expr> &kid1, const ref<Expr> &kid2,
			      const ref<Expr> &kid3, const ref<Expr> &kid4,
			      const ref<Expr> &kid5, const ref<Expr> &kid6,
			      const ref<Expr> &kid7, const ref<Expr> &kid8) {
  return ConcatExpr::create(kid1, ConcatExpr::create(kid2, ConcatExpr::create(kid3, 
			      ConcatExpr::create(kid4, ConcatExpr::create4(kid5, kid6, kid7, kid8)))));
}

/***/

ref<Expr> ExtractExpr::create(ref<Expr> expr, unsigned off, Width w) {
  unsigned kw = expr.getWidth();
  assert(w > 0 && off + w <= kw && "invalid extract");
  
  if (w == kw)
    return expr;
  else if (expr.isConstant()) {
    return ConstantExpr::create(ints::trunc(expr.getConstantValue() >> off, w, kw), w);
  } 
  else 
    // Extract(Concat)
    if (ConcatExpr *ce = dyn_ref_cast<ConcatExpr>(expr)) {
      // if the extract skips the right side of the concat
      if (off >= ce->getRight().getWidth())
	return ExtractExpr::create(ce->getLeft(), off - ce->getRight().getWidth(), w);
      
      // if the extract skips the left side of the concat
      if (off + w <= ce->getRight().getWidth())
	return ExtractExpr::create(ce->getRight(), off, w);

      // E(C(x,y)) = C(E(x), E(y))
      return ConcatExpr::create(ExtractExpr::create(ce->getKid(0), 0, w - ce->getKid(1).getWidth() + off),
				ExtractExpr::create(ce->getKid(1), off, ce->getKid(1).getWidth() - off));
    }
  
  return ExtractExpr::alloc(expr, off, w);
}


ref<Expr> ExtractExpr::createByteOff(ref<Expr> expr, unsigned offset, Width bits) {
  return ExtractExpr::create(expr, 8*offset, bits);
}

/***/

ref<Expr> ZExtExpr::create(const ref<Expr> &e, Width w) {
  unsigned kBits = e.getWidth();
  if (w == kBits) {
    return e;
  } else if (w < kBits) { // trunc
    return ExtractExpr::createByteOff(e, 0, w);
  } else {
    if (e.isConstant()) {
      return ConstantExpr::create(ints::zext(e.getConstantValue(), w, kBits),
                                  w);
    }
    
    return ZExtExpr::alloc(e, w);
  }
}

ref<Expr> SExtExpr::create(const ref<Expr> &e, Width w) {
  unsigned kBits = e.getWidth();
  if (w == kBits) {
    return e;
  } else if (w < kBits) { // trunc
    return ExtractExpr::createByteOff(e, 0, w);
  } else {
    if (e.isConstant()) {
      return ConstantExpr::create(ints::sext(e.getConstantValue(), w, kBits),
                                  w);
    }
    
    return SExtExpr::alloc(e, w);
  }
}

/***/

static ref<Expr> AndExpr_create(Expr *l, Expr *r);
static ref<Expr> XorExpr_create(Expr *l, Expr *r);

static ref<Expr> EqExpr_createPartial(Expr *l, const ref<Expr> &cr);
static ref<Expr> AndExpr_createPartialR(const ref<Expr> &cl, Expr *r);
static ref<Expr> SubExpr_createPartialR(const ref<Expr> &cl, Expr *r);
static ref<Expr> XorExpr_createPartialR(const ref<Expr> &cl, Expr *r);

static ref<Expr> AddExpr_createPartialR(const ref<Expr> &cl, Expr *r) {
  assert(cl.isConstant() && "non-constant passed in place of constant");
  uint64_t value = cl.getConstantValue();
  Expr::Width type = cl.getWidth();

  if (type==Expr::Bool) {
    return XorExpr_createPartialR(cl, r);
  } else if (!value) {
    return r;
  } else {
    Expr::Kind rk = r->getKind();
    if (rk==Expr::Add && r->getKid(0).isConstant()) { // A + (B+c) == (A+B) + c
      return AddExpr::create(AddExpr::create(cl, r->getKid(0)),
                             r->getKid(1));
    } else if (rk==Expr::Sub && r->getKid(0).isConstant()) { // A + (B-c) == (A+B) - c
      return SubExpr::create(AddExpr::create(cl, r->getKid(0)),
                             r->getKid(1));
    } else {
      return AddExpr::alloc(cl, r);
    }
  }
}
static ref<Expr> AddExpr_createPartial(Expr *l, const ref<Expr> &cr) {
  return AddExpr_createPartialR(cr, l);
}
static ref<Expr> AddExpr_create(Expr *l, Expr *r) {
  Expr::Width type = l->getWidth();

  if (type == Expr::Bool) {
    return XorExpr_create(l, r);
  } else {
    Expr::Kind lk = l->getKind(), rk = r->getKind();
    if (lk==Expr::Add && l->getKid(0).isConstant()) { // (k+a)+b = k+(a+b)
      return AddExpr::create(l->getKid(0),
                             AddExpr::create(l->getKid(1), r));
    } else if (lk==Expr::Sub && l->getKid(0).isConstant()) { // (k-a)+b = k+(b-a)
      return AddExpr::create(l->getKid(0),
                             SubExpr::create(r, l->getKid(1)));
    } else if (rk==Expr::Add && r->getKid(0).isConstant()) { // a + (k+b) = k+(a+b)
      return AddExpr::create(r->getKid(0),
                             AddExpr::create(l, r->getKid(1)));
    } else if (rk==Expr::Sub && r->getKid(0).isConstant()) { // a + (k-b) = k+(a-b)
      return AddExpr::create(r->getKid(0),
                             SubExpr::create(l, r->getKid(1)));
    } else {
      return AddExpr::alloc(l, r);
    }
  }  
}

static ref<Expr> SubExpr_createPartialR(const ref<Expr> &cl, Expr *r) {
  assert(cl.isConstant() && "non-constant passed in place of constant");
  Expr::Width type = cl.getWidth();

  if (type==Expr::Bool) {
    return XorExpr_createPartialR(cl, r);
  } else {
    Expr::Kind rk = r->getKind();
    if (rk==Expr::Add && r->getKid(0).isConstant()) { // A - (B+c) == (A-B) - c
      return SubExpr::create(SubExpr::create(cl, r->getKid(0)),
                             r->getKid(1));
    } else if (rk==Expr::Sub && r->getKid(0).isConstant()) { // A - (B-c) == (A-B) + c
      return AddExpr::create(SubExpr::create(cl, r->getKid(0)),
                             r->getKid(1));
    } else {
      return SubExpr::alloc(cl, r);
    }
  }
}
static ref<Expr> SubExpr_createPartial(Expr *l, const ref<Expr> &cr) {
  assert(cr.isConstant() && "non-constant passed in place of constant");
  uint64_t value = cr.getConstantValue();
  Expr::Width width = cr.getWidth();
  uint64_t nvalue = ints::sub(0, value, width);

  return AddExpr_createPartial(l, ConstantExpr::create(nvalue, width));
}
static ref<Expr> SubExpr_create(Expr *l, Expr *r) {
  Expr::Width type = l->getWidth();

  if (type == Expr::Bool) {
    return XorExpr_create(l, r);
  } else if (*l==*r) {
    return ConstantExpr::alloc(0, type);
  } else {
    Expr::Kind lk = l->getKind(), rk = r->getKind();
    if (lk==Expr::Add && l->getKid(0).isConstant()) { // (k+a)-b = k+(a-b)
      return AddExpr::create(l->getKid(0),
                             SubExpr::create(l->getKid(1), r));
    } else if (lk==Expr::Sub && l->getKid(0).isConstant()) { // (k-a)-b = k-(a+b)
      return SubExpr::create(l->getKid(0),
                             AddExpr::create(l->getKid(1), r));
    } else if (rk==Expr::Add && r->getKid(0).isConstant()) { // a - (k+b) = (a-c) - k
      return SubExpr::create(SubExpr::create(l, r->getKid(1)),
                             r->getKid(0));
    } else if (rk==Expr::Sub && r->getKid(0).isConstant()) { // a - (k-b) = (a+b) - k
      return SubExpr::create(AddExpr::create(l, r->getKid(1)),
                             r->getKid(0));
    } else {
      return SubExpr::alloc(l, r);
    }
  }  
}

static ref<Expr> MulExpr_createPartialR(const ref<Expr> &cl, Expr *r) {
  assert(cl.isConstant() && "non-constant passed in place of constant");
  uint64_t value = cl.getConstantValue();
  Expr::Width type = cl.getWidth();

  if (type == Expr::Bool) {
    return AndExpr_createPartialR(cl, r);
  } else if (value == 1) {
    return r;
  } else if (!value) {
    return cl;
  } else {
    return MulExpr::alloc(cl, r);
  }
}
static ref<Expr> MulExpr_createPartial(Expr *l, const ref<Expr> &cr) {
  return MulExpr_createPartialR(cr, l);
}
static ref<Expr> MulExpr_create(Expr *l, Expr *r) {
  Expr::Width type = l->getWidth();
  
  if (type == Expr::Bool) {
    return AndExpr::alloc(l, r);
  } else {
    return MulExpr::alloc(l, r);
  }
}

static ref<Expr> AndExpr_createPartial(Expr *l, const ref<Expr> &cr) {
  assert(cr.isConstant() && "non-constant passed in place of constant");
  uint64_t value = cr.getConstantValue();
  Expr::Width width = cr.getWidth();;

  if (value==ints::sext(1, width, 1)) {
    return l;
  } else if (!value) {
    return cr;
  } else {
    return AndExpr::alloc(l, cr);
  }
}
static ref<Expr> AndExpr_createPartialR(const ref<Expr> &cl, Expr *r) {
  return AndExpr_createPartial(r, cl);
}
static ref<Expr> AndExpr_create(Expr *l, Expr *r) {
  return AndExpr::alloc(l, r);
}

static ref<Expr> OrExpr_createPartial(Expr *l, const ref<Expr> &cr) {
  assert(cr.isConstant() && "non-constant passed in place of constant");
  uint64_t value = cr.getConstantValue();
  Expr::Width width = cr.getWidth();

  if (value == ints::sext(1, width, 1)) {
    return cr;
  } else if (!value) {
    return l;
  } else {
    return OrExpr::alloc(l, cr);
  }
}
static ref<Expr> OrExpr_createPartialR(const ref<Expr> &cl, Expr *r) {
  return OrExpr_createPartial(r, cl);
}
static ref<Expr> OrExpr_create(Expr *l, Expr *r) {
  return OrExpr::alloc(l, r);
}

static ref<Expr> XorExpr_createPartialR(const ref<Expr> &cl, Expr *r) {
  assert(cl.isConstant() && "non-constant passed in place of constant");
  uint64_t value = cl.getConstantValue();
  Expr::Width type = cl.getWidth();

  if (type==Expr::Bool) {
    if (value) {
      return EqExpr_createPartial(r, ConstantExpr::create(0, Expr::Bool));
    } else {
      return r;
    }
  } else if (!value) {
    return r;
  } else {
    return XorExpr::alloc(cl, r);
  }
}

static ref<Expr> XorExpr_createPartial(Expr *l, const ref<Expr> &cr) {
  return XorExpr_createPartialR(cr, l);
}
static ref<Expr> XorExpr_create(Expr *l, Expr *r) {
  return XorExpr::alloc(l, r);
}

static ref<Expr> UDivExpr_create(const ref<Expr> &l, const ref<Expr> &r) {
  if (l.getWidth() == Expr::Bool) { // r must be 1
    return l;
  } else{
    return UDivExpr::alloc(l, r);
  }
}

static ref<Expr> SDivExpr_create(const ref<Expr> &l, const ref<Expr> &r) {
  if (l.getWidth() == Expr::Bool) { // r must be 1
    return l;
  } else{
    return SDivExpr::alloc(l, r);
  }
}

static ref<Expr> URemExpr_create(const ref<Expr> &l, const ref<Expr> &r) {
  if (l.getWidth() == Expr::Bool) { // r must be 1
    return ConstantExpr::create(0, Expr::Bool);
  } else{
    return URemExpr::alloc(l, r);
  }
}

static ref<Expr> SRemExpr_create(const ref<Expr> &l, const ref<Expr> &r) {
  if (l.getWidth() == Expr::Bool) { // r must be 1
    return ConstantExpr::create(0, Expr::Bool);
  } else{
    return SRemExpr::alloc(l, r);
  }
}

static ref<Expr> ShlExpr_create(const ref<Expr> &l, const ref<Expr> &r) {
  if (l.getWidth() == Expr::Bool) { // l & !r
    return AndExpr::create(l, Expr::createNot(r));
  } else{
    return ShlExpr::alloc(l, r);
  }
}

static ref<Expr> LShrExpr_create(const ref<Expr> &l, const ref<Expr> &r) {
  if (l.getWidth() == Expr::Bool) { // l & !r
    return AndExpr::create(l, Expr::createNot(r));
  } else{
    return LShrExpr::alloc(l, r);
  }
}

static ref<Expr> AShrExpr_create(const ref<Expr> &l, const ref<Expr> &r) {
  if (l.getWidth() == Expr::Bool) { // l
    return l;
  } else{
    return AShrExpr::alloc(l, r);
  }
}

#define BCREATE_R(_e_op, _op, partialL, partialR) \
ref<Expr>  _e_op ::create(const ref<Expr> &l, const ref<Expr> &r) { \
  assert(l.getWidth()==r.getWidth() && "type mismatch"); \
  if (l.isConstant()) {                                \
    if (r.isConstant()) {                              \
      Expr::Width width = l.getWidth(); \
      uint64_t val = ints::_op(l.getConstantValue(),  \
                               r.getConstantValue(), width);  \
      return ConstantExpr::create(val, width); \
    } else { \
      return _e_op ## _createPartialR(l, r.get()); \
    } \
  } else if (r.isConstant()) {             \
    return _e_op ## _createPartial(l.get(), r); \
  } \
  return _e_op ## _create(l.get(), r.get()); \
}

#define BCREATE(_e_op, _op) \
ref<Expr>  _e_op ::create(const ref<Expr> &l, const ref<Expr> &r) { \
  assert(l.getWidth()==r.getWidth() && "type mismatch"); \
  if (l.isConstant()) {                                \
    if (r.isConstant()) {                              \
      Expr::Width width = l.getWidth(); \
      uint64_t val = ints::_op(l.getConstantValue(), \
                               r.getConstantValue(), width);  \
      return ConstantExpr::create(val, width); \
    } \
  } \
  return _e_op ## _create(l, r);                    \
}

BCREATE_R(AddExpr, add, AddExpr_createPartial, AddExpr_createPartialR)
BCREATE_R(SubExpr, sub, SubExpr_createPartial, SubExpr_createPartialR)
BCREATE_R(MulExpr, mul, MulExpr_createPartial, MulExpr_createPartialR)
BCREATE_R(AndExpr, land, AndExpr_createPartial, AndExpr_createPartialR)
BCREATE_R(OrExpr, lor, OrExpr_createPartial, OrExpr_createPartialR)
BCREATE_R(XorExpr, lxor, XorExpr_createPartial, XorExpr_createPartialR)
BCREATE(UDivExpr, udiv)
BCREATE(SDivExpr, sdiv)
BCREATE(URemExpr, urem)
BCREATE(SRemExpr, srem)
BCREATE(ShlExpr, shl)
BCREATE(LShrExpr, lshr)
BCREATE(AShrExpr, ashr)

#define CMPCREATE(_e_op, _op) \
ref<Expr>  _e_op ::create(const ref<Expr> &l, const ref<Expr> &r) { \
  assert(l.getWidth()==r.getWidth() && "type mismatch"); \
  if (l.isConstant()) {                                \
    if (r.isConstant()) {                              \
      Expr::Width width = l.getWidth(); \
      uint64_t val = ints::_op(l.getConstantValue(), \
                               r.getConstantValue(), width);  \
      return ConstantExpr::create(val, Expr::Bool); \
    } \
  } \
  return _e_op ## _create(l, r);                    \
}

#define CMPCREATE_T(_e_op, _op, _reflexive_e_op, partialL, partialR) \
ref<Expr>  _e_op ::create(const ref<Expr> &l, const ref<Expr> &r) { \
  assert(l.getWidth()==r.getWidth() && "type mismatch"); \
  if (l.isConstant()) {                                \
    if (r.isConstant()) {                              \
      Expr::Width width = l.getWidth(); \
      uint64_t val = ints::_op(l.getConstantValue(), \
                               r.getConstantValue(), width);  \
      return ConstantExpr::create(val, Expr::Bool); \
    } else { \
      return partialR(l, r.get()); \
    } \
  } else if (r.isConstant()) {                  \
    return partialL(l.get(), r); \
  } else { \
    return _e_op ## _create(l.get(), r.get()); \
  } \
}
  

static ref<Expr> EqExpr_create(const ref<Expr> &l, const ref<Expr> &r) {
  if (l == r) {
    return ConstantExpr::alloc(1, Expr::Bool);
  } else {
    return EqExpr::alloc(l, r);
  }
}


/// Tries to optimize EqExpr cl == rd, where cl is a ConstantExpr and
/// rd a ReadExpr.  If rd is a read into an all-constant array,
/// returns a disjunction of equalities on the index.  Otherwise,
/// returns the initial equality expression. 
static ref<Expr> TryConstArrayOpt(const ref<Expr> &cl, 
				  ReadExpr *rd) {
  assert(cl.isConstant() && "constant expression required");
  assert(rd->getKind() == Expr::Read && "read expression required");
  
  uint64_t ct = cl.getConstantValue();
  ref<Expr> first_idx_match;

  // number of positions in the array that contain value ct
  unsigned matches = 0;

  //llvm::cerr << "Size updates/root: " << rd->updates.getSize() << " / " << (rd->updates.root)->size << "\n";

  // for now, just assume standard "flushing" of a concrete array,
  // where the concrete array has one update for each index, in order
  bool all_const = true;
  if (rd->updates.getSize() == rd->updates.root->size) {
    unsigned k = rd->updates.getSize();
    for (const UpdateNode *un = rd->updates.head; un; un = un->next) {
      assert(k > 0);
      k--;

      ref<Expr> idx = un->index;
      ref<Expr> val = un->value;
      if (!idx.isConstant() || !val.isConstant()) {
	all_const = false;
	//llvm::cerr << "Idx or val not constant\n";
	break;
      }
      else {
	if (idx.getConstantValue() != k) {
	  all_const = false;
	  //llvm::cerr << "Wrong constant\n";
	  break;
	}
	if (val.getConstantValue() == ct) {
	  matches++;
	  if (matches == 1)
	    first_idx_match = un->index;
	}
      }
    }
  }
  else all_const = false;
  
  if (all_const && matches <= 100) {
    // apply optimization
    //llvm::cerr << "\n\n=== Applying const array optimization ===\n\n";

    if (matches == 0)
      return ConstantExpr::alloc(0, Expr::Bool);

    ref<Expr> res = EqExpr::create(first_idx_match, rd->index);
    if (matches == 1)
      return res;
    
    for (const UpdateNode *un = rd->updates.head; un; un = un->next) {
      if (un->index != first_idx_match && un->value.getConstantValue() == ct) {
	ref<Expr> curr_eq = EqExpr::create(un->index, rd->index);
	res = OrExpr::create(curr_eq, res);
      }
    }
    
    return res;
  }

  return EqExpr_create(cl, ref<Expr>(rd));
}


static ref<Expr> EqExpr_createPartialR(const ref<Expr> &cl, Expr *r) {  
  assert(cl.isConstant() && "non-constant passed in place of constant");
  uint64_t value = cl.getConstantValue();
  Expr::Width width = cl.getWidth();

  Expr::Kind rk = r->getKind();
  if (width == Expr::Bool) {
    if (value) {
      return r;
    } else {
      // 0 != ...
      
      if (rk == Expr::Eq) {
        const EqExpr *ree = static_ref_cast<EqExpr>(r);

        // eliminate double negation
        if (ree->left.isConstant() &&
            ree->left.getWidth()==Expr::Bool) {
          assert(!ree->left.getConstantValue());
          return ree->right;
        }
      } else if (rk == Expr::Or) {
        const OrExpr *roe = static_ref_cast<OrExpr>(r);

        // transform not(or(a,b)) to and(not a, not b)
        return AndExpr::create(Expr::createNot(roe->left),
                               Expr::createNot(roe->right));
      }
    }
  } else if (rk == Expr::SExt) {
    // (sext(a,T)==c) == (a==c)
    const SExtExpr *see = static_ref_cast<SExtExpr>(r);
    Expr::Width fromBits = see->src.getWidth();
    uint64_t trunc = bits64::truncateToNBits(value, fromBits);

    // pathological check, make sure it is possible to
    // sext to this value *from any value*
    if (value == ints::sext(trunc, width, fromBits)) {
      return EqExpr::create(see->src, ConstantExpr::create(trunc, fromBits));
    } else {
      return ConstantExpr::create(0, Expr::Bool);
    }
  } else if (rk == Expr::ZExt) {
    // (zext(a,T)==c) == (a==c)
    const ZExtExpr *zee = static_ref_cast<ZExtExpr>(r);
    Expr::Width fromBits = zee->src.getWidth();
    uint64_t trunc = bits64::truncateToNBits(value, fromBits);
    
    // pathological check, make sure it is possible to
    // zext to this value *from any value*
    if (value == ints::zext(trunc, width, fromBits)) {
      return EqExpr::create(zee->src, ConstantExpr::create(trunc, fromBits));
    } else {
      return ConstantExpr::create(0, Expr::Bool);
    }
  } else if (rk==Expr::Add) {
    const AddExpr *ae = static_ref_cast<AddExpr>(r);
    if (ae->left.isConstant()) {
      // c0 = c1 + b => c0 - c1 = b
      return EqExpr_createPartialR(SubExpr::create(cl, ae->left),
                                   ae->right.get());
    }
  } else if (rk==Expr::Sub) {
    const SubExpr *se = static_ref_cast<SubExpr>(r);
    if (se->left.isConstant()) {
      // c0 = c1 - b => c1 - c0 = b
      return EqExpr_createPartialR(SubExpr::create(se->left, cl),
                                   se->right.get());
    }
  } else if (rk == Expr::Read && ConstArrayOpt) {
    return TryConstArrayOpt(cl, static_cast<ReadExpr*>(r));
  }
    
  return EqExpr_create(cl, r);
}

static ref<Expr> EqExpr_createPartial(Expr *l, const ref<Expr> &cr) {  
  return EqExpr_createPartialR(cr, l);
}
  
ref<Expr> NeExpr::create(const ref<Expr> &l, const ref<Expr> &r) {
  return EqExpr::create(ConstantExpr::create(0, Expr::Bool),
                        EqExpr::create(l, r));
}

ref<Expr> UgtExpr::create(const ref<Expr> &l, const ref<Expr> &r) {
  return UltExpr::create(r, l);
}
ref<Expr> UgeExpr::create(const ref<Expr> &l, const ref<Expr> &r) {
  return UleExpr::create(r, l);
}

ref<Expr> SgtExpr::create(const ref<Expr> &l, const ref<Expr> &r) {
  return SltExpr::create(r, l);
}
ref<Expr> SgeExpr::create(const ref<Expr> &l, const ref<Expr> &r) {
  return SleExpr::create(r, l);
}

static ref<Expr> UltExpr_create(const ref<Expr> &l, const ref<Expr> &r) {
  Expr::Width t = l.getWidth();
  if (t == Expr::Bool) { // !l && r
    return AndExpr::create(Expr::createNot(l), r);
  } else {
    if (r.isConstant()) {      
      uint64_t value = r.getConstantValue();
      if (value <= 8) {
        ref<Expr> res = ConstantExpr::alloc(0, Expr::Bool);
        for (unsigned i=0; i<value; i++) {
          res = OrExpr::create(EqExpr::create(l, 
                                              ConstantExpr::alloc(i, t)), res);
        }
        return res;
      }
    }
    return UltExpr::alloc(l, r);
  }
}

static ref<Expr> UleExpr_create(const ref<Expr> &l, const ref<Expr> &r) {
  if (l.getWidth() == Expr::Bool) { // !(l && !r)
    return OrExpr::create(Expr::createNot(l), r);
  } else {
    return UleExpr::alloc(l, r);
  }
}

static ref<Expr> SltExpr_create(const ref<Expr> &l, const ref<Expr> &r) {
  if (l.getWidth() == Expr::Bool) { // l && !r
    return AndExpr::create(l, Expr::createNot(r));
  } else {
    return SltExpr::alloc(l, r);
  }
}

static ref<Expr> SleExpr_create(const ref<Expr> &l, const ref<Expr> &r) {
  if (l.getWidth() == Expr::Bool) { // !(!l && r)
    return OrExpr::create(l, Expr::createNot(r));
  } else {
    return SleExpr::alloc(l, r);
  }
}

CMPCREATE_T(EqExpr, eq, EqExpr, EqExpr_createPartial, EqExpr_createPartialR)
CMPCREATE(UltExpr, ult)
CMPCREATE(UleExpr, ule)
CMPCREATE(SltExpr, slt)
CMPCREATE(SleExpr, sle)
