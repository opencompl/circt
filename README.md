# Verification Dialect and Tools
(branch: verification; check branch main for the actual project)

[![](https://github.com/llvm/circt/workflows/Build%20and%20Test/badge.svg?event=push)](https://github.com/llvm/circt/actions?query=workflow%3A%22Build+and+Test%22)
[![Nightly integration tests](https://github.com/llvm/circt/workflows/Nightly%20integration%20tests/badge.svg)](https://github.com/llvm/circt/actions?query=workflow%3A%22Nightly+integration+tests%22)

## Setting this up

Following instructions have been slightly modified to accommodate the verification dialect.

These commands can be used to setup CIRCT project:

1) **Install Dependencies** of LLVM/MLIR according to [the
  instructions](https://mlir.llvm.org/getting_started/), including cmake and 
  ninja.

2) **Check out LLVM and CIRCT repos.**  CIRCT contains LLVM as a git
submodule.  The LLVM repo here includes staged changes to MLIR which
may be necessary to support CIRCT.  It also represents the version of
LLVM that has been tested.  MLIR is still changing relatively rapidly,
so feel free to use the current version of LLVM, but APIs may have
changed.

```
$ git clone git@github.com:llvm/circt.git
$ cd circt
$ git submodule init
```

2.1) If you wish to build `llvm` from the submodule, do the following:
```
$ git submodule update
```

*Note:* The repository is set up so that `git submodule update` performs a 
shallow clone, meaning it downloads just enough of the LLVM repository to check 
out the currently specified commit. If you wish to work with the full history of
the LLVM repository, you can manually "unshallow" the the submodule:

```
$ cd llvm
$ git fetch --unshallow
```

**Build and test LLVM/MLIR:**

```
$ cd circt
$ mkdir llvm/build
$ cd llvm/build
$ cmake -G Ninja ../llvm \
    -DLLVM_ENABLE_PROJECTS="mlir" \
    -DLLVM_TARGETS_TO_BUILD="host" \
    -DLLVM_ENABLE_ASSERTIONS=ON \
    -DCMAKE_BUILD_TYPE=DEBUG \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
$ ninja
$ ninja check-mlir
```

3) **Build smt-dialect**
```
git submodule update smt-dialect
```

4) **Build and test CIRCT:**

```
$ cd circt
$ mkdir build
$ cd build
$ cmake -G Ninja .. \
    -DMLIR_DIR=$PWD/../llvm/build/lib/cmake/mlir \
    -DLLVM_DIR=$PWD/../llvm/build/lib/cmake/llvm \
    -DLLVM_ENABLE_ASSERTIONS=ON \
    -DCMAKE_BUILD_TYPE=DEBUG \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
$ ninja
$ ninja check-circt
$ ninja check-circt-integration # Run the integration tests.
```

The `-DCMAKE_BUILD_TYPE=DEBUG` flag enables debug information, which makes the
whole tree compile slower, but allows you to step through code into the LLVM
and MLIR frameworks. The `-DCMAKE_EXPOERT_COMPILE_COMMANDS=ON` flag generates
a `build/compile_commands.json` file, which can be used by editors (or plugins)
for autocomplete and/or IDE-like features.

To get something that runs fast, use `-DCMAKE_BUILD_TYPE=Release` or
`-DCMAKE_BUILD_TYPE=RelWithDebInfo` if you want to go fast and optionally if
you want debug info to go with it.  `Release` mode makes a very large difference
in performance.

Consult the [Getting Started](docs/GettingStarted.md) page for detailed 
information on configuring and compiling CIRCT.

Consult the [Python Bindings](docs/PythonBindings.md) page if you are mainly
interested in using CIRCT from a Python prompt or script.
