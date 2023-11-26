// RUN: %clang %s -emit-llvm -g -c -o %t1.bc
// RUN: rm -rf %t.klee-out
// RUN: %klee --output-dir=%t.klee-out --use-guided-search=error --analysis-reproduce=%s.json %t1.bc
// RUN: FileCheck -input-file=%t.klee-out/info %s
// CHECK: KLEE: done: total queries = 1
#include "assert.h"
#include "klee/klee.h"

int main(int x) {
  if (x > 0) {
    return 0;
  }
  x = 42;
  return 0;
}
