//===- Algorithms.h - Library of scheduling algorithms ----------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file defines a library of scheduling algorithms.
//
//===----------------------------------------------------------------------===//

#ifndef CIRCT_SCHEDULING_ALGORITHMS_H
#define CIRCT_SCHEDULING_ALGORITHMS_H

#include "circt/Scheduling/Problems.h"

namespace circt {
namespace scheduling {

/// This is a simple list scheduler for solving the basic scheduling problem.
/// Its objective is to assign each operation its earliest possible start time,
/// or in other words, to schedule each operation as soon as possible (hence the
/// name). Fails if the dependence graph contains cycles.
LogicalResult scheduleASAP(Problem &prob);

/// Solve the basic problem using linear programming and a handwritten
/// implementation of the simplex algorithm. The objective is to minimize the
/// start time of the given \p lastOp. Fails if the dependence graph contains
/// cycles.
LogicalResult scheduleSimplex(Problem &prob, Operation *lastOp);

/// Solve the resource-free cyclic problem using linear programming and a
/// handwritten implementation of the simplex algorithm. The objectives are to
/// determine the smallest feasible initiation interval, and to minimize the
/// start time of the given \p lastOp. Fails if the dependence graph contains
/// cycles that do not include at least one edge with a non-zero distance.
LogicalResult scheduleSimplex(CyclicProblem &prob, Operation *lastOp);

/// Solve the acyclic problem with shared pipelined operators using a linear
/// programming-based heuristic. The approach tries to minimize the start time
/// of the given \p lastOp, but optimality is not guaranteed. Fails if the
/// dependence graph contains cycles.
LogicalResult scheduleSimplex(SharedPipelinedOperatorsProblem &prob,
                              Operation *lastOp);

} // namespace scheduling
} // namespace circt

#endif // CIRCT_SCHEDULING_ALGORITHMS_H
