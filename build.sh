#!/bin/sh

# For more build options, visit
# https://klee.github.io/build-script/

# Base folder where dependencies and KLEE itself are installed
BASE=$HOME/klee_build

## KLEE Required options
# Build type for KLEE. The options are:
# Release
# Release+Debug
# Release+Asserts
# Release+Debug+Asserts
# Debug
# Debug+Asserts
# KLEE_RUNTIME_BUILD="Debug+Asserts"
KLEE_RUNTIME_BUILD="Release" # "Debug+Asserts"

COVERAGE=0
ENABLE_DOXYGEN=0
USE_TCMALLOC=0
TCMALLOC_VERSION=2.9.1
USE_LIBCXX=1
# Also required despite not being mentioned in the guide
SQLITE_VERSION="3400100"


## LLVM Required options
LLVM_VERSION=14
ENABLE_OPTIMIZED=1
ENABLE_DEBUG=1
DISABLE_ASSERTIONS=1
REQUIRES_RTTI=1

## Solvers Required options
# SOLVERS=STP
SOLVERS=STP:Z3

## Google Test Required options
GTEST_VERSION=1.11.0

## UClibC Required options
UCLIBC_VERSION=klee_uclibc_v1.3
# LLVM_VERSION is also required for UClibC

## Z3 Required options
Z3_VERSION=4.8.15
STP_VERSION=2.3.3
MINISAT_VERSION=master

BASE="$BASE" KLEE_RUNTIME_BUILD=$KLEE_RUNTIME_BUILD COVERAGE=$COVERAGE ENABLE_DOXYGEN=$ENABLE_DOXYGEN USE_TCMALLOC=$USE_TCMALLOC USE_LIBCXX=$USE_LIBCXX LLVM_VERSION=$LLVM_VERSION ENABLE_OPTIMIZED=$ENABLE_OPTIMIZED ENABLE_DEBUG=$ENABLE_DEBUG DISABLE_ASSERTIONS=$DISABLE_ASSERTIONS REQUIRES_RTTI=$REQUIRES_RTTI SOLVERS=$SOLVERS GTEST_VERSION=$GTEST_VERSION UCLIBC_VERSION=$UCLIBC_VERSION STP_VERSION=$STP_VERSION MINISAT_VERSION=$MINISAT_VERSION Z3_VERSION=$Z3_VERSION SQLITE_VERSION=$SQLITE_VERSION ./scripts/build/build.sh klee --install-system-deps
