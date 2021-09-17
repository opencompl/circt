// RUN: circt-opt %s | circt-opt
// RUN: circt-translate %s --mlir-to-smt

func @main() attributes {smt_main} {
  %e = smt.forall (%x : i32) {
    %2 = constant 2 : i32
    %e = constant 0 : i1
    // %a = comb.add %x, %x : i32
    // %b = comb.mul %x, %2 : i32
    // %e = comb.icmp eq %a, %b : i32
    smt.yield %e : i1
  }
  smt.assert %e
  return
}