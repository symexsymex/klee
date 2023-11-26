// -*- C++ -*-

#ifndef KLEE_TESTCASEUTILS_H
#define KLEE_TESTCASEUTILS_H

extern "C" {
#include "TestCase.h"
}

#include <vector>

Offset createOffset(unsigned offset, unsigned index);

ConcretizedObject createConcretizedObject(char *name, unsigned char *values,
                                          unsigned size, Offset *offsets,
                                          unsigned n_offsets, uint64_t address);

ConcretizedObject createConcretizedObject(const char *name,
                                          std::vector<unsigned char> &values,
                                          uint64_t address);

#endif /* KLEE_TESTCASEUTILS_H */
