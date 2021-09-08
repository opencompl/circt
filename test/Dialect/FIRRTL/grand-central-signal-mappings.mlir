// RUN: circt-opt --pass-pipeline='firrtl.circuit(firrtl.module(firrtl-grand-central-signal-mappings))' %s | FileCheck %s

firrtl.circuit "SubCircuit" {
  firrtl.extmodule @FooExtern(in %clockIn: !firrtl.clock, out %clockOut: !firrtl.clock)
  firrtl.extmodule @BarExtern(in %someInput: !firrtl.uint<42>, out %someOutput: !firrtl.uint<42>)

  // CHECK-LABEL: firrtl.module @Foo_signal_mappings
  // CHECK-SAME:    out %clock_source: !firrtl.clock
  // CHECK-SAME:    in %clock_sink: !firrtl.clock
  // CHECK-SAME:  ) {
  // CHECK:         [[T1:%.+]] = firrtl.verbatim.wire "MainA.clock"
  // CHECK:         firrtl.connect %clock_source, [[T1]]
  // CHECK:         [[T2:%.+]] = firrtl.verbatim.wire "MainB.clock"
  // CHECK:         firrtl.force [[T2]], %clock_sink
  // CHECK:       }
  // CHECK-LABEL: firrtl.module @Foo
  firrtl.module @Foo() attributes {annotations = [{class = "sifive.enterprise.grandcentral.SignalDriverAnnotation", id = 0 : i64}]} {
    %clock_source = firrtl.wire {annotations = [{class = "sifive.enterprise.grandcentral.SignalDriverAnnotation", dir = "source", id = 0 : i64, peer = "~Main|MainA>clock", side = "local", targetId = 0 : i64}]} : !firrtl.clock
    %clock_sink = firrtl.wire {annotations = [{class = "sifive.enterprise.grandcentral.SignalDriverAnnotation", dir = "sink", id = 0 : i64, peer = "~Main|MainB>clock", side = "local", targetId = 1 : i64}]} : !firrtl.clock
    %ext_clockIn, %ext_clockOut = firrtl.instance @FooExtern {name = "ext"} : !firrtl.clock, !firrtl.clock
    firrtl.connect %ext_clockIn, %clock_source : !firrtl.clock, !firrtl.clock
    firrtl.connect %clock_sink, %ext_clockOut : !firrtl.clock, !firrtl.clock
    // CHECK: [[T1:%.+]], [[T2:%.+]] = firrtl.instance @Foo_signal_mappings
    // CHECK: firrtl.connect %clock_source, [[T1]] :
    // CHECK: firrtl.connect [[T2]], %clock_sink :
  }

  // CHECK-LABEL: firrtl.module @Bar_signal_mappings
  // CHECK-SAME:    out %data_source: !firrtl.uint<42>
  // CHECK-SAME:    in %data_sink: !firrtl.uint<42>
  // CHECK-SAME:  ) {
  // CHECK:         [[T1:%.+]] = firrtl.verbatim.wire "MainA.dataOut_x_y_z"
  // CHECK:         firrtl.connect %data_source, [[T1]]
  // CHECK:         [[T2:%.+]] = firrtl.verbatim.wire "MainA.dataIn_a_b_c"
  // CHECK:         firrtl.force [[T2]], %data_sink
  // CHECK:       }
  // CHECK-LABEL: firrtl.module @Bar
  firrtl.module @Bar() attributes {annotations = [{class = "sifive.enterprise.grandcentral.SignalDriverAnnotation", id = 1 : i64}]} {
    %data_source = firrtl.wire {annotations = [{class = "sifive.enterprise.grandcentral.SignalDriverAnnotation", dir = "source", id = 1 : i64, peer = "~Main|MainA>dataOut.x.y.z", side = "local", targetId = 0 : i64}]} : !firrtl.uint<42>
    %data_sink = firrtl.wire {annotations = [{class = "sifive.enterprise.grandcentral.SignalDriverAnnotation", dir = "sink", id = 1 : i64, peer = "~Main|MainA>dataIn.a.b.c", side = "local", targetId = 1 : i64}]} : !firrtl.uint<42>
    %ext_someInput, %ext_someOutput = firrtl.instance @BarExtern {name = "ext"} : !firrtl.uint<42>, !firrtl.uint<42>
    firrtl.connect %ext_someInput, %data_source : !firrtl.uint<42>, !firrtl.uint<42>
    firrtl.connect %data_sink, %ext_someOutput : !firrtl.uint<42>, !firrtl.uint<42>
    // CHECK: [[T1:%.+]], [[T2:%.+]] = firrtl.instance @Bar_signal_mappings
    // CHECK: firrtl.connect %data_source, [[T1]] :
    // CHECK: firrtl.connect [[T2]], %data_sink :
  }

  // CHECK-LABEL: firrtl.module @Baz_signal_mappings
  // CHECK-SAME:    out %data_source: !firrtl.uint<42>
  // CHECK-SAME:    in %data_sink: !firrtl.uint<42>
  // CHECK-SAME:  ) {
  // CHECK:         [[T1:%.+]] = firrtl.verbatim.wire "MainA.dataOut"
  // CHECK:         firrtl.connect %data_source, [[T1]]
  // CHECK:         [[T2:%.+]] = firrtl.verbatim.wire "MainA.dataIn"
  // CHECK:         firrtl.force [[T2]], %data_sink
  // CHECK:       }
  // CHECK-LABEL: firrtl.module @Baz
  firrtl.module @Baz(
    out %data_source: !firrtl.uint<42> {firrtl.annotations = [{class = "sifive.enterprise.grandcentral.SignalDriverAnnotation", dir = "source", id = 2 : i64, peer = "~Main|MainA>dataOut", side = "local", targetId = 0 : i64}]},
    in %data_sink: !firrtl.uint<42> {firrtl.annotations = [{class = "sifive.enterprise.grandcentral.SignalDriverAnnotation", dir = "sink", id = 2 : i64, peer = "~Main|MainA>dataIn", side = "local", targetId = 1 : i64}]}
  ) attributes {annotations = [{class = "sifive.enterprise.grandcentral.SignalDriverAnnotation", id = 2 : i64}]} {
    // CHECK: [[T1:%.+]], [[T2:%.+]] = firrtl.instance @Baz_signal_mappings
    // CHECK: firrtl.connect %data_source, [[T1]] :
    // CHECK: firrtl.connect [[T2]], %data_sink :
  }

  firrtl.module @SubCircuit() {
    firrtl.instance @Foo {name = "foo"}
    firrtl.instance @Bar {name = "bar"}
    %baz_data_source, %baz_data_sink = firrtl.instance @Baz {name = "baz"} : !firrtl.uint<42>, !firrtl.uint<42>
  }
}
