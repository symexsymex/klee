//===-- KInstruction.h ------------------------------------------*- C++ -*-===//
//
//                     The KLEE Symbolic Virtual Machine
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef KLEE_KINSTRUCTION_H
#define KLEE_KINSTRUCTION_H

#include "klee/Config/Version.h"
#include "klee/Module/InstructionInfoTable.h"
#include "klee/Module/KInstIterator.h"
#include "klee/Module/KModule.h"

#include "klee/Support/CompilerWarning.h"
DISABLE_WARNING_PUSH
DISABLE_WARNING_DEPRECATED_DECLARATIONS
#include "llvm/Support/DataTypes.h"
#include "llvm/Support/raw_ostream.h"
DISABLE_WARNING_POP

#include <vector>

namespace llvm {
class Instruction;
}

namespace klee {
class Executor;
struct InstructionInfo;
class KModule;
struct KBlock;
struct KFunction;

/// KInstruction - Intermediate instruction representation used
/// during execution.
struct KInstruction {
  llvm::Instruction *inst;
  const InstructionInfo *info;

  /// Value numbers for each operand. -1 is an invalid value,
  /// otherwise negative numbers are indices (negated and offset by
  /// 2) into the module constant table and positive numbers are
  /// register indices.
  int *operands;
  /// Destination register index.
  unsigned dest;
  KBlock *parent;

  // Instruction index in the basic block
  unsigned index;

public:
  KInstruction() = default;
  explicit KInstruction(const KInstruction &ki);
  virtual ~KInstruction();
  std::string getSourceLocation() const;
  std::string toString() const;

  KInstIterator getIterator() const { return (parent->instructions + index); }
};

struct KGEPInstruction : KInstruction {
  /// indices - The list of variable sized adjustments to add to the pointer
  /// operand to execute the instruction. The first element is the operand
  /// index into the GetElementPtr instruction, and the second element is the
  /// element size to multiple that index by.
  std::vector<std::pair<unsigned, uint64_t>> indices;

  /// offset - A constant offset to add to the pointer operand to execute the
  /// instruction.
  uint64_t offset;

public:
  KGEPInstruction() = default;
  explicit KGEPInstruction(const KGEPInstruction &ki);
};

struct CallStackFrame {
  KInstruction *caller;
  KFunction *kf;

  CallStackFrame(KInstruction *caller_, KFunction *kf_)
      : caller(caller_), kf(kf_) {}
  ~CallStackFrame() = default;
  CallStackFrame(const CallStackFrame &s);

  bool equals(const CallStackFrame &other) const;

  bool operator==(const CallStackFrame &other) const { return equals(other); }

  bool operator!=(const CallStackFrame &other) const { return !equals(other); }

  static void subtractFrames(std::vector<CallStackFrame> &minued,
                             std::vector<CallStackFrame> subtrahend) {
    while (!subtrahend.empty() && !minued.empty()) {
      if (subtrahend.size() == 1) {
        assert(subtrahend.back().caller == nullptr);
        break;
      }
      auto forwardF = subtrahend.back();
      auto backwardF = minued.back();
      assert(forwardF == backwardF);
      minued.pop_back();
      subtrahend.pop_back();
    }
  }
};

} // namespace klee

#endif /* KLEE_KINSTRUCTION_H */
