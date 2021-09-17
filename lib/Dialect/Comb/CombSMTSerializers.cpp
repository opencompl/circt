#include "SMT/SMTInterfaces.h"
#include "circt/Dialect/Comb/CombOps.h"
#include "circt/Dialect/HW/HWOps.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/PatternMatch.h"
#include "llvm/Support/FormatVariadic.h"

using namespace circt;
using namespace comb;
using namespace mlir;
using namespace smt;

LogicalResult AddOp::generateDefinitions(mlir::smt::SMTContext &ctx) {
  return success();
}
LogicalResult AddOp::serializeExpression(llvm::raw_ostream &os,
                                         mlir::smt::SMTContext &ctx) {
  auto funcTy = FunctionType::get(getContext(), getOperandTypes(), {getType()});
  std::string funcName = "comb.add_" + std::to_string(getType().getWidth());
  if (failed(ctx.addFunc(funcName, funcTy, "(+ arg0 arg1)")))
    return failure();
  os << "(" << funcName;
  for (unsigned i : {0, 1}) {
    os << " ";
    if (failed(ctx.serializeExpression(this->getOperand(i), os)))
      return failure();
  }
  os << ")";
  return success();
}

LogicalResult MulOp::generateDefinitions(mlir::smt::SMTContext &ctx) {
  return success();
}
LogicalResult MulOp::serializeExpression(llvm::raw_ostream &os,
                                         mlir::smt::SMTContext &ctx) {
  auto funcTy = FunctionType::get(getContext(), getOperandTypes(), {getType()});
  std::string funcName = "comb.mul_" + std::to_string(getType().getWidth());
  if (failed(ctx.addFunc(funcName, funcTy, "(* arg0 arg1)")))
    return failure();
  os << "(" << funcName;
  for (unsigned i : {0, 1}) {
    os << " ";
    if (failed(ctx.serializeExpression(this->getOperand(i), os)))
      return failure();
  }
  os << ")";
  return success();
}

LogicalResult ICmpOp::serializeExpression(llvm::raw_ostream &os,
                                          ::mlir::smt::SMTContext &ctx) {
  switch (predicate()) {
  case ICmpPredicate::eq:
    os << "(= ";
    break;
  case ICmpPredicate::ne:
    os << "(not (= ";
    break;
  case ICmpPredicate::sge:
    os << "(>= ";
    break;
  case ICmpPredicate::sgt:
    os << "(> ";
    break;
  case ICmpPredicate::sle:
    os << "(<= ";
    break;
  case ICmpPredicate::slt:
    os << "(< ";
    break;
  default:
    return emitError(
        "[mlir-to-smt] Unsigned comparisons not supported (because I haven't "
        "yet figured out how to compare bitvectors).");
  }
  if (failed(ctx.serializeExpression(lhs(), os)))
    return failure();
  os << " ";
  if (failed(ctx.serializeExpression(rhs(), os)))
    return failure();
  os << ")";
  if (predicate() == ICmpPredicate::ne)
    os << ")";
  return success();
}