//===-- Assignment.h --------------------------------------------*- C++ -*-===//
//
//                     The KLEE Symbolic Virtual Machine
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef KLEE_ASSIGNMENT_H
#define KLEE_ASSIGNMENT_H

#include "klee/ADT/SparseStorage.h"
#include "klee/Expr/ExprEvaluator.h"

#include <map>
#include <set>

namespace klee {
class Array;
class Symcrete;
struct SymcreteLess;
class ConstraintSet;

typedef std::set<ref<Symcrete>, SymcreteLess> SymcreteOrderedSet;
using symcretes_ty = SymcreteOrderedSet;

class Assignment {
public:
  typedef std::map<const Array *, SparseStorage<unsigned char>> bindings_ty;

  bool allowFreeValues;
  bindings_ty bindings;

  friend bool operator==(const Assignment &lhs, const Assignment &rhs) {
    return lhs.bindings == rhs.bindings;
  }

public:
  Assignment(bool _allowFreeValues = false)
      : allowFreeValues(_allowFreeValues) {}
  Assignment(const bindings_ty &_bindings, bool _allowFreeValues = false)
      : allowFreeValues(_allowFreeValues), bindings(_bindings) {}
  Assignment(const std::vector<const Array *> &objects,
             const std::vector<SparseStorage<unsigned char>> &values,
             bool _allowFreeValues = false)
      : allowFreeValues(_allowFreeValues) {
    assert(objects.size() == values.size());
    for (unsigned i = 0; i < values.size(); ++i) {
      const Array *os = objects.at(i);
      const SparseStorage<unsigned char> &arr = values.at(i);
      bindings.insert(std::make_pair(os, arr));
    }
  }

  ref<Expr> evaluate(const Array *mo, unsigned index) const;
  ref<Expr> evaluate(ref<Expr> e) const;
  ConstraintSet createConstraintsFromAssignment() const;

  template <typename InputIterator>
  bool satisfies(InputIterator begin, InputIterator end);
  void dump() const;

  Assignment diffWith(const Assignment &other) const;
  Assignment part(const SymcreteOrderedSet &symcretes) const;

  bindings_ty::const_iterator begin() const { return bindings.begin(); }
  bindings_ty::const_iterator end() const { return bindings.end(); }
  bool isEmpty() { return begin() == end(); }

  std::vector<const Array *> keys() const;
  std::vector<SparseStorage<unsigned char>> values() const;
};

class AssignmentEvaluator : public ExprEvaluator {
  const Assignment &a;

protected:
  ref<Expr> getInitialValue(const Array &mo, unsigned index) {
    return a.evaluate(&mo, index);
  }

public:
  AssignmentEvaluator(const Assignment &_a) : a(_a) {}
};

/***/

inline ref<Expr> Assignment::evaluate(const Array *array,
                                      unsigned index) const {
  assert(array);
  bindings_ty::const_iterator it = bindings.find(array);
  if (it != bindings.end() && index < it->second.size()) {
    return ConstantExpr::alloc(it->second.load(index), array->getRange());
  } else {
    if (allowFreeValues) {
      return ReadExpr::create(UpdateList(array, ref<UpdateNode>(nullptr)),
                              ConstantExpr::alloc(index, array->getDomain()));
    } else {
      return ConstantExpr::alloc(0, array->getRange());
    }
  }
}

inline ref<Expr> Assignment::evaluate(ref<Expr> e) const {
  AssignmentEvaluator v(*this);
  return v.visit(e);
}

template <typename InputIterator>
inline bool Assignment::satisfies(InputIterator begin, InputIterator end) {
  AssignmentEvaluator v(*this);
  for (; begin != end; ++begin)
    if (!v.visit(*begin)->isTrue())
      return false;
  return true;
}
} // namespace klee

#endif /* KLEE_ASSIGNMENT_H */
