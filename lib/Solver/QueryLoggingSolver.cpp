//===-- QueryLoggingSolver.cpp --------------------------------------------===//
//
//                     The KLEE Symbolic Virtual Machine
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
#include "QueryLoggingSolver.h"

#include "klee/Config/config.h"
#include "klee/Expr/Constraints.h"
#include "klee/Expr/ExprUtil.h"
#include "klee/Statistics/Statistics.h"
#include "klee/Support/ErrorHandling.h"
#include "klee/Support/FileHandling.h"
#include "klee/Support/OptionCategories.h"
#include "klee/System/Time.h"

#include <utility>

namespace {
llvm::cl::opt<bool> DumpPartialQueryiesEarly(
    "log-partial-queries-early", llvm::cl::init(false),
    llvm::cl::desc("Log queries before calling the solver (default=false)"),
    llvm::cl::cat(klee::SolvingCat));

#ifdef HAVE_ZLIB_H
llvm::cl::opt<bool> CreateCompressedQueryLog(
    "compress-query-log", llvm::cl::init(false),
    llvm::cl::desc("Compress query log files (default=false)"),
    llvm::cl::cat(klee::SolvingCat));
#endif
} // namespace

QueryLoggingSolver::QueryLoggingSolver(std::unique_ptr<Solver> solver,
                                       std::string path,
                                       const std::string &commentSign,
                                       time::Span queryTimeToLog,
                                       bool logTimedOut)
    : solver(std::move(solver)), BufferString(""), logBuffer(BufferString),
      queryCount(0), minQueryTimeToLog(queryTimeToLog),
      logTimedOutQueries(logTimedOut), queryCommentSign(commentSign) {
  std::string error;
#ifdef HAVE_ZLIB_H
  if (!CreateCompressedQueryLog) {
#endif
    os = klee_open_output_file(path, error);
#ifdef HAVE_ZLIB_H
  } else {
    path.append(".gz");
    os = klee_open_compressed_output_file(path, error);
  }
#endif
  if (!os) {
    klee_error("Could not open file %s : %s", path.c_str(), error.c_str());
  }
  assert(this->solver);
}

void QueryLoggingSolver::flushBufferConditionally(bool writeToFile) {
  logBuffer.flush();
  if (writeToFile) {
    *os << logBuffer.str();
    os->flush();
  }
  // prepare the buffer for reuse
  BufferString = "";
}

void QueryLoggingSolver::startQuery(const Query &query, const char *typeName,
                                    const Query *falseQuery,
                                    const std::vector<const Array *> *objects) {
  Statistic *S = theStatisticManager->getStatisticByName("Instructions");
  uint64_t instructions = S ? S->getValue() : 0;

  logBuffer << queryCommentSign << " Query " << queryCount++ << " -- "
            << "Type: " << typeName << ", "
            << "Instructions: " << instructions << "\n";

  printQuery(query, falseQuery, objects);

  if (DumpPartialQueryiesEarly) {
    flushBufferConditionally(true);
  }
  startTime = time::getWallTime();
}

void QueryLoggingSolver::finishQuery(bool success) {
  lastQueryDuration = time::getWallTime() - startTime;
  logBuffer << queryCommentSign << "   " << (success ? "OK" : "FAIL") << " -- "
            << "Elapsed: " << lastQueryDuration << "\n";

  if (false == success) {
    logBuffer << queryCommentSign << "   Failure reason: "
              << SolverImpl::getOperationStatusString(
                     solver->impl->getOperationStatusCode())
              << "\n";
  }
}

void QueryLoggingSolver::flushBuffer() {
  // we either do not limit logging queries
  // or the query time is larger than threshold
  // or we log a timed out query
  bool writeToFile =
      (!minQueryTimeToLog) || (lastQueryDuration > minQueryTimeToLog) ||
      (logTimedOutQueries &&
       (SOLVER_RUN_STATUS_TIMEOUT == solver->impl->getOperationStatusCode()));

  flushBufferConditionally(writeToFile);
}

bool QueryLoggingSolver::computeTruth(const Query &query, bool &isValid) {
  startQuery(query, "Truth");

  bool success = solver->impl->computeTruth(query, isValid);

  finishQuery(success);

  if (success) {
    logBuffer << queryCommentSign
              << "   Is Valid: " << (isValid ? "true" : "false") << "\n";
  }
  logBuffer << "\n";

  flushBuffer();

  return success;
}

bool QueryLoggingSolver::computeValidity(const Query &query,
                                         PartialValidity &result) {
  startQuery(query, "Validity");

  bool success = solver->impl->computeValidity(query, result);

  finishQuery(success);

  if (success) {
    logBuffer << queryCommentSign << "   Validity: " << pv_to_str(result)
              << "\n";
  }
  logBuffer << "\n";

  flushBuffer();

  return success;
}

bool QueryLoggingSolver::computeValue(const Query &query, ref<Expr> &result) {
  Query withFalse = query.withFalse();
  startQuery(query, "Value", &withFalse);

  bool success = solver->impl->computeValue(query, result);

  finishQuery(success);

  if (success) {
    logBuffer << queryCommentSign << "   Result: " << result << "\n";
  }
  logBuffer << "\n";

  flushBuffer();

  return success;
}

bool QueryLoggingSolver::computeInitialValues(
    const Query &query, const std::vector<const Array *> &objects,
    std::vector<SparseStorage<unsigned char>> &values, bool &hasSolution) {
  startQuery(query, "InitialValues", 0, &objects);

  ExprHashSet expressions;
  expressions.insert(query.constraints.cs().begin(),
                     query.constraints.cs().end());
  expressions.insert(query.expr);

  std::vector<const Array *> allObjects;
  findSymbolicObjects(expressions.begin(), expressions.end(), allObjects);
  std::vector<SparseStorage<unsigned char>> allValues;

  bool success = solver->impl->computeInitialValues(query, allObjects,
                                                    allValues, hasSolution);

  finishQuery(success);

  if (success) {
    logBuffer << queryCommentSign
              << "   Solvable: " << (hasSolution ? "true" : "false") << "\n";
    if (hasSolution) {
      ref<InvalidResponse> invalidResponse =
          new InvalidResponse(allObjects, allValues);
      success = invalidResponse->tryGetInitialValuesFor(objects, values);
      assert(success);
      Assignment allSolutionAssignment(allObjects, allValues, true);
      std::vector<SparseStorage<unsigned char>>::iterator values_it =
          values.begin();

      Assignment solutionAssignment(objects, values, true);
      for (std::vector<const Array *>::const_iterator i = objects.begin(),
                                                      e = objects.end();
           i != e; ++i, ++values_it) {
        const Array *array = *i;
        SparseStorage<unsigned char> &data = *values_it;
        logBuffer << queryCommentSign << "     " << array->getIdentifier()
                  << " = [";
        ref<ConstantExpr> arrayConstantSize =
            dyn_cast<ConstantExpr>(allSolutionAssignment.evaluate(array->size));
        assert(arrayConstantSize &&
               "Array of symbolic size had not receive value for size!");

        for (unsigned j = 0; j < arrayConstantSize->getZExtValue(); j++) {
          logBuffer << (int)data.load(j);

          if (j + 1 < arrayConstantSize->getZExtValue()) {
            logBuffer << ",";
          }
        }
        logBuffer << "]\n";
      }
    }
  }
  logBuffer << "\n";

  flushBuffer();

  return success;
}

bool QueryLoggingSolver::check(const Query &query,
                               ref<SolverResponse> &result) {
  startQuery(query, "Check");

  bool success = solver->impl->check(query, result);

  finishQuery(success);

  if (success) {
    bool hasSolution = isa<InvalidResponse>(result);
    logBuffer << queryCommentSign
              << "   Solvable: " << (hasSolution ? "true" : "false") << "\n";
    if (hasSolution) {
      std::map<const Array *, SparseStorage<unsigned char>> initialValues;
      result->tryGetInitialValues(initialValues);
      Assignment solutionAssignment(initialValues, true);

      for (std::map<const Array *, SparseStorage<unsigned char>>::const_iterator
               i = initialValues.begin(),
               e = initialValues.end();
           i != e; ++i) {
        const Array *array = i->first;
        const SparseStorage<unsigned char> &data = i->second;
        logBuffer << queryCommentSign << "     " << array->getIdentifier()
                  << " = [";
        ref<ConstantExpr> arrayConstantSize =
            dyn_cast<ConstantExpr>(solutionAssignment.evaluate(array->size));
        assert(arrayConstantSize &&
               "Array of symbolic size had not receive value for size!");
        for (unsigned j = 0; j < arrayConstantSize->getZExtValue(); j++) {
          logBuffer << (int)data.load(j);

          if (j + 1 < arrayConstantSize->getZExtValue()) {
            logBuffer << ",";
          }
        }
        logBuffer << "]\n";
      }
    } else {
      ValidityCore validityCore;
      result->tryGetValidityCore(validityCore);
      logBuffer << queryCommentSign << "   ValidityCore:\n";

      printQuery(Query(ConstraintSet(validityCore.constraints, {}, {true}),
                       validityCore.expr));
    }
  }
  logBuffer << "\n";

  flushBuffer();

  return success;
}

bool QueryLoggingSolver::computeValidityCore(const Query &query,
                                             ValidityCore &validityCore,
                                             bool &isValid) {
  startQuery(query, "ValidityCore");

  bool success =
      solver->impl->computeValidityCore(query, validityCore, isValid);

  finishQuery(success);

  if (success) {
    logBuffer << queryCommentSign
              << "   Is Valid: " << (isValid ? "true" : "false") << "\n";
  }

  if (isValid) {
    logBuffer << queryCommentSign << "   ValidityCore:\n";

    printQuery(Query(ConstraintSet(validityCore.constraints, {}, {true}),
                     validityCore.expr));
  }

  logBuffer << "\n";

  flushBuffer();

  return success;
}

SolverImpl::SolverRunStatus QueryLoggingSolver::getOperationStatusCode() {
  return solver->impl->getOperationStatusCode();
}

char *QueryLoggingSolver::getConstraintLog(const Query &query) {
  return solver->impl->getConstraintLog(query);
}

void QueryLoggingSolver::setCoreSolverTimeout(time::Span timeout) {
  solver->impl->setCoreSolverTimeout(timeout);
}
