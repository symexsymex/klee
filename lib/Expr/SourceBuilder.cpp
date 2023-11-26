#include "klee/Expr/SourceBuilder.h"
#include "klee/Expr/Expr.h"
#include "klee/Expr/SymbolicSource.h"
#include "klee/Module/KModule.h"

using namespace klee;

ref<SymbolicSource>
SourceBuilder::constant(const std::vector<ref<ConstantExpr>> &constantValues) {
  ref<SymbolicSource> r(new ConstantSource(constantValues));
  r->computeHash();
  return r;
}

ref<SymbolicSource> SourceBuilder::symbolicSizeConstant(unsigned defaultValue) {
  ref<SymbolicSource> r(new SymbolicSizeConstantSource(defaultValue));
  r->computeHash();
  return r;
}

ref<SymbolicSource>
SourceBuilder::symbolicSizeConstantAddress(unsigned defaultValue,
                                           unsigned version) {
  ref<SymbolicSource> r(
      new SymbolicSizeConstantAddressSource(defaultValue, version));
  r->computeHash();
  return r;
}

ref<SymbolicSource> SourceBuilder::makeSymbolic(const std::string &name,
                                                unsigned version) {
  ref<SymbolicSource> r(new MakeSymbolicSource(name, version));
  r->computeHash();
  return r;
}

ref<SymbolicSource>
SourceBuilder::lazyInitializationAddress(ref<Expr> pointer) {
  ref<SymbolicSource> r(new LazyInitializationAddressSource(pointer));
  r->computeHash();
  return r;
}

ref<SymbolicSource> SourceBuilder::lazyInitializationSize(ref<Expr> pointer) {
  ref<SymbolicSource> r(new LazyInitializationSizeSource(pointer));
  r->computeHash();
  return r;
}

ref<SymbolicSource>
SourceBuilder::lazyInitializationContent(ref<Expr> pointer) {
  ref<SymbolicSource> r(new LazyInitializationContentSource(pointer));
  r->computeHash();
  return r;
}

ref<SymbolicSource> SourceBuilder::argument(const llvm::Argument &_allocSite,
                                            int _index, KModule *km) {
  ref<SymbolicSource> r(new ArgumentSource(_allocSite, _index, km));
  r->computeHash();
  return r;
}

ref<SymbolicSource>
SourceBuilder::instruction(const llvm::Instruction &_allocSite, int _index,
                           KModule *km) {
  ref<SymbolicSource> r(new InstructionSource(_allocSite, _index, km));
  r->computeHash();
  return r;
}

ref<SymbolicSource> SourceBuilder::value(const llvm::Value &_allocSite,
                                         int _index, KModule *km) {
  if (const llvm::Argument *allocSite = dyn_cast<llvm::Argument>(&_allocSite)) {
    return argument(*allocSite, _index, km);
  }
  if (const llvm::Instruction *allocSite =
          dyn_cast<llvm::Instruction>(&_allocSite)) {
    return instruction(*allocSite, _index, km);
  }
  assert(0 && "unreachable");
}

ref<SymbolicSource> SourceBuilder::global(const llvm::GlobalVariable &gv) {
  ref<SymbolicSource> r(new GlobalSource(gv));
  r->computeHash();
  return r;
}

ref<SymbolicSource> SourceBuilder::irreproducible(const std::string &name,
                                                  unsigned version) {
  ref<SymbolicSource> r(new IrreproducibleSource(name, version));
  r->computeHash();
  return r;
}
