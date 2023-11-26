//===-- Parser.h ------------------------------------------------*- C++ -*-===//
//
//                     The KLEE Symbolic Virtual Machine
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef KLEE_PARSER_H
#define KLEE_PARSER_H

#include "klee/Expr/Expr.h"
#include "klee/Expr/ExprHashMap.h"
#include "klee/Expr/Path.h"
#include "klee/Module/KModule.h"

#include <string>
#include <vector>

namespace llvm {
class MemoryBuffer;
}

namespace klee {
class ExprBuilder;

namespace expr {
// These are the language types we manipulate.
typedef ref<Expr> ExprHandle;
typedef UpdateList VersionHandle;

/// Identifier - Wrapper for a uniqued string.
struct Identifier {
  const std::string Name;

public:
  Identifier(const std::string _Name) : Name(_Name) {}
};

// FIXME: Do we have a use for tracking source locations?

/// Decl - Base class for top level declarations.
class Decl {
public:
  enum DeclKind {
    ArrayDeclKind,
    PathDeclKind,
    ExprVarDeclKind,
    VersionVarDeclKind,
    QueryCommandDeclKind,
    LemmaCommandDeclKind,

    DeclKindLast = LemmaCommandDeclKind,
    VarDeclKindFirst = ExprVarDeclKind,
    VarDeclKindLast = VersionVarDeclKind,
    CommandDeclKindFirst = QueryCommandDeclKind,
    CommandDeclKindLast = LemmaCommandDeclKind
  };

private:
  DeclKind Kind;

public:
  Decl(DeclKind _Kind);
  virtual ~Decl() {}

  /// getKind - Get the decl kind.
  DeclKind getKind() const { return Kind; }

  /// dump - Dump the AST node to stderr.
  virtual void dump() = 0;

  static bool classof(const Decl *) { return true; }
};

/// ArrayDecl - Array declarations.
///
/// For example:
///   array obj[] : w32 -> w8 = symbolic
///   array obj[32] : w32 -> w8 = [ ... ]
class ArrayDecl : public Decl {
public:
  /// Root - The root array object defined by this decl.
  const Array *Root;

public:
  ArrayDecl(const Array *_Root) : Decl(ArrayDeclKind), Root(_Root) {}

  virtual void dump();

  static bool classof(const Decl *D) {
    return D->getKind() == Decl::ArrayDeclKind;
  }
  static bool classof(const ArrayDecl *) { return true; }
};

// PathDecl - Path declarations.
// Example: TODO
class PathDecl : public Decl {
public:
  // Path defined by this decl.
  Path path;

public:
  PathDecl(Path path) : Decl(PathDeclKind), path(path) {}

  virtual void dump();

  static bool classof(const Decl *D) {
    return D->getKind() == Decl::PathDeclKind;
  }
  static bool classof(const PathDecl *) { return true; }
};

/// VarDecl - Variable declarations, used to associate names to
/// expressions or array versions outside of expressions.
///
/// For example:
// FIXME: What syntax are we going to use for this? We need it.
class VarDecl : public Decl {
public:
  const Identifier *Name;

  static bool classof(const Decl *D) {
    return (Decl::VarDeclKindFirst <= D->getKind() &&
            D->getKind() <= Decl::VarDeclKindLast);
  }
  static bool classof(const VarDecl *) { return true; }
};

/// ExprVarDecl - Expression variable declarations.
class ExprVarDecl : public VarDecl {
public:
  ExprHandle Value;

  static bool classof(const Decl *D) {
    return D->getKind() == Decl::ExprVarDeclKind;
  }
  static bool classof(const ExprVarDecl *) { return true; }
};

/// VersionVarDecl - Array version variable declarations.
class VersionVarDecl : public VarDecl {
public:
  VersionHandle Value;

  static bool classof(const Decl *D) {
    return D->getKind() == Decl::VersionVarDeclKind;
  }
  static bool classof(const VersionVarDecl *) { return true; }
};

/// CommandDecl - Base class for language commands.
class CommandDecl : public Decl {
public:
  CommandDecl(DeclKind _Kind) : Decl(_Kind) {}

  static bool classof(const Decl *D) {
    return (Decl::CommandDeclKindFirst <= D->getKind() &&
            D->getKind() <= Decl::CommandDeclKindLast);
  }
  static bool classof(const CommandDecl *) { return true; }
};

/// QueryCommand - Query commands.
///
/// (query [ ... constraints ... ] expression)
/// (query [ ... constraints ... ] expression values)
/// (query [ ... constraints ... ] expression values objects)
class QueryCommand : public CommandDecl {
public:
  // FIXME: One issue with STP... should we require the FE to
  // guarantee that these are consistent? That is a cornerstone of
  // being able to do independence. We may want this as a flag, if
  // we are to interface with things like SMT.

  KModule *km;

  /// Constraints - The list of constraints to assume for this
  /// expression.
  const std::vector<ExprHandle> Constraints;

  /// Query - The expression being queried.
  ExprHandle Query;

  /// Values - The expressions for which counterexamples should be
  /// given if the query is invalid.
  const std::vector<ExprHandle> Values;

  /// Objects - Symbolic arrays whose initial contents should be
  /// given if the query is invalid.
  const std::vector<const Array *> Objects;

public:
  QueryCommand(const std::vector<ExprHandle> &_Constraints, KModule *km,
               ExprHandle _Query, const std::vector<ExprHandle> &_Values,
               const std::vector<const Array *> &_Objects)
      : CommandDecl(QueryCommandDeclKind), km(km), Constraints(_Constraints),
        Query(_Query), Values(_Values), Objects(_Objects) {}

  virtual void dump();

  static bool classof(const Decl *D) {
    return D->getKind() == QueryCommandDeclKind;
  }
  static bool classof(const QueryCommand *) { return true; }
};

class LemmaCommand : public CommandDecl {
public:
  ExprOrderedSet constraints;
  Path path;

  LemmaCommand(ExprOrderedSet constraints, Path path)
      : CommandDecl(LemmaCommandDeclKind), constraints(constraints),
        path(path) {}

  // TODO
  virtual void dump() {}

  static bool classof(const Decl *D) {
    return D->getKind() == LemmaCommandDeclKind;
  }
  static bool classof(const LemmaCommand *) { return true; }
};

/// Parser - Public interface for parsing a .kquery language file.
class Parser {
protected:
  Parser();

public:
  virtual ~Parser();

  /// SetMaxErrors - Suppress anything beyond the first N errors.
  virtual void SetMaxErrors(unsigned N) = 0;

  /// GetNumErrors - Return the number of encountered errors.
  virtual unsigned GetNumErrors() const = 0;

  /// ParseTopLevelDecl - Parse and return a top level declaration,
  /// which the caller assumes ownership of.
  ///
  /// \return NULL indicates the end of the file has been reached.
  virtual Decl *ParseTopLevelDecl() = 0;

  /// CreateParser - Create a parser implementation for the given
  /// MemoryBuffer.
  ///
  /// \arg Name - The name to use in diagnostic messages.
  /// \arg MB - The input data.
  /// \arg Builder - The expression builder to use for constructing
  /// expressions.
  static Parser *Create(const std::string Name, const llvm::MemoryBuffer *MB,
                        ExprBuilder *Builder, bool ClearArrayAfterQuery);

  static Parser *Create(const std::string Name, const llvm::MemoryBuffer *MB,
                        ExprBuilder *Builder, ArrayCache *TheArrayCache,
                        KModule *km, bool ClearArrayAfterQuery);
};
} // namespace expr
} // namespace klee

#endif /* KLEE_PARSER_H */
