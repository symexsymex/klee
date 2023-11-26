//===-- KModule.cpp -------------------------------------------------------===//
//
//                     The KLEE Symbolic Virtual Machine
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#define DEBUG_TYPE "KModule"

#include "Passes.h"

#include "klee/Config/Version.h"
#include "klee/Core/Interpreter.h"
#include "klee/Support/OptionCategories.h"
#include "klee/Module/Cell.h"
#include "klee/Module/InstructionInfoTable.h"
#include "klee/Module/KInstruction.h"
#include "klee/Module/KType.h"
#include "klee/Module/KModule.h"
#include "klee/Support/Debug.h"
#include "klee/Support/ErrorHandling.h"
#include "klee/Support/ModuleUtil.h"
#include "llvm/IR/InstIterator.h"


#include "llvm/Bitcode/BitcodeWriter.h"
#if LLVM_VERSION_CODE < LLVM_VERSION(8, 0)
#include "llvm/IR/CallSite.h"
#endif
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/ValueSymbolTable.h"
#include "llvm/IR/Verifier.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/Linker/Linker.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/raw_os_ostream.h"
#include "llvm/Transforms/Scalar.h"
#if LLVM_VERSION_CODE >= LLVM_VERSION(8, 0)
#include "llvm/Transforms/Scalar/Scalarizer.h"
#endif
#include "llvm/Transforms/Utils/Cloning.h"
#if LLVM_VERSION_CODE >= LLVM_VERSION(7, 0)
#include "llvm/Transforms/Utils.h"
#endif

#include <sstream>
#include <unordered_map>
#include <unordered_set>

using namespace llvm;
using namespace klee;

namespace klee {
cl::OptionCategory
    ModuleCat("Module-related options",
              "These options affect the compile-time processing of the code.");
}

namespace {
  enum SwitchImplType {
    eSwitchTypeSimple,
    eSwitchTypeLLVM,
    eSwitchTypeInternal
  };

  cl::opt<bool>
  OutputSource("output-source",
               cl::desc("Write the assembly for the final transformed source (default=true)"),
               cl::init(true),
	       cl::cat(ModuleCat));

  cl::opt<bool>
  OutputModule("output-module",
               cl::desc("Write the bitcode for the final transformed module"),
               cl::init(false),
	       cl::cat(ModuleCat));

  cl::opt<SwitchImplType>
  SwitchType("switch-type", cl::desc("Select the implementation of switch (default=internal)"),
             cl::values(clEnumValN(eSwitchTypeSimple, "simple", 
                                   "lower to ordered branches"),
                        clEnumValN(eSwitchTypeLLVM, "llvm", 
                                   "lower using LLVM"),
                        clEnumValN(eSwitchTypeInternal, "internal", 
                                   "execute switch internally")),
             cl::init(eSwitchTypeInternal),
	     cl::cat(ModuleCat));
  
  cl::opt<bool>
  DebugPrintEscapingFunctions("debug-print-escaping-functions", 
                              cl::desc("Print functions whose address is taken (default=false)"),
			      cl::cat(ModuleCat));

  // Don't run VerifierPass when checking module
  cl::opt<bool>
  DontVerify("disable-verify",
             cl::desc("Do not verify the module integrity (default=false)"),
             cl::init(false), cl::cat(klee::ModuleCat));

  cl::opt<bool>
  OptimiseKLEECall("klee-call-optimisation",
                             cl::desc("Allow optimization of functions that "
                                      "contain KLEE calls (default=true)"),
                             cl::init(true), cl::cat(ModuleCat));
}

/***/

namespace llvm {
extern void Optimize(Module *, llvm::ArrayRef<const char *> preservedFunctions);
}

// what a hack
static Function *getStubFunctionForCtorList(Module *m,
                                            GlobalVariable *gv, 
                                            std::string name) {
  assert(!gv->isDeclaration() && !gv->hasInternalLinkage() &&
         "do not support old LLVM style constructor/destructor lists");

  std::vector<Type *> nullary;

  Function *fn = Function::Create(FunctionType::get(Type::getVoidTy(m->getContext()),
						    nullary, false),
				  GlobalVariable::InternalLinkage, 
				  name,
                              m);
  BasicBlock *bb = BasicBlock::Create(m->getContext(), "entry", fn);
  llvm::IRBuilder<> Builder(bb);

  // From lli:
  // Should be an array of '{ int, void ()* }' structs.  The first value is
  // the init priority, which we ignore.
  auto arr = dyn_cast<ConstantArray>(gv->getInitializer());
  if (arr) {
    for (unsigned i=0; i<arr->getNumOperands(); i++) {
      auto cs = cast<ConstantStruct>(arr->getOperand(i));
      // There is a third element in global_ctor elements (``i8 @data``).
#if LLVM_VERSION_CODE >= LLVM_VERSION(9, 0)
      assert(cs->getNumOperands() == 3 &&
             "unexpected element in ctor initializer list");
#else
      // before LLVM 9.0, the third operand was optional
      assert((cs->getNumOperands() == 2 || cs->getNumOperands() == 3) &&
             "unexpected element in ctor initializer list");
#endif
      auto fp = cs->getOperand(1);
      if (!fp->isNullValue()) {
        if (auto ce = dyn_cast<llvm::ConstantExpr>(fp))
          fp = ce->getOperand(0);

        if (auto f = dyn_cast<Function>(fp)) {
          Builder.CreateCall(f);
        } else {
          assert(0 && "unable to get function pointer from ctor initializer list");
        }
      }
    }
  }

  Builder.CreateRetVoid();

  return fn;
}

static void
injectStaticConstructorsAndDestructors(Module *m,
                                       llvm::StringRef entryFunction) {
  GlobalVariable *ctors = m->getNamedGlobal("llvm.global_ctors");
  GlobalVariable *dtors = m->getNamedGlobal("llvm.global_dtors");

  if (!ctors && !dtors)
    return;

  Function *mainFn = m->getFunction(entryFunction);
  if (!mainFn)
    klee_error("Entry function '%s' not found in module.",
               entryFunction.str().c_str());

  if (ctors) {
    llvm::IRBuilder<> Builder(&*mainFn->begin()->begin());
    Builder.CreateCall(getStubFunctionForCtorList(m, ctors, "klee.ctor_stub"));
  }

  if (dtors) {
    Function *dtorStub = getStubFunctionForCtorList(m, dtors, "klee.dtor_stub");
    for (Function::iterator it = mainFn->begin(), ie = mainFn->end(); it != ie;
         ++it) {
      if (isa<ReturnInst>(it->getTerminator())) {
        llvm::IRBuilder<> Builder(it->getTerminator());
        Builder.CreateCall(dtorStub);
      }
    }
  }
}

void KModule::addInternalFunction(const char* functionName){
  Function* internalFunction = module->getFunction(functionName);
  if (!internalFunction) {
    KLEE_DEBUG(klee_warning(
        "Failed to add internal function %s. Not found.", functionName));
    return ;
  }
  KLEE_DEBUG(klee_message("Added function %s.",functionName));
  internalFunctions.insert(internalFunction);
}

void KModule::calculateBackwardDistance(KFunction *kf) {
  std::map<KFunction *, unsigned int> &bdist = backwardDistance[kf];
  std::deque<KFunction *> nodes;
  nodes.push_back(kf);
  bdist[kf] = 0;
  while (!nodes.empty()) {
    KFunction *currKF = nodes.front();
    for (auto &cf : callMap[currKF->function]) {
      if (cf->isDeclaration())
        continue;
      KFunction *callKF = functionMap[cf];
      if (bdist.find(callKF) == bdist.end()) {
        bdist[callKF] = bdist[callKF] + 1;
        nodes.push_back(callKF);
      }
    }
    nodes.pop_front();
  }
}

void KModule::calculateDistance(KFunction *kf) {
  std::map<KFunction *, unsigned int> &dist = distance[kf];
  std::deque<KFunction *> nodes;
  nodes.push_back(kf);
  dist[kf] = 0;
  while (!nodes.empty()) {
    KFunction *currKF = nodes.front();
    for (auto &callBlock : currKF->kCallBlocks) {
      if (!callBlock->calledFunction ||
          callBlock->calledFunction->isDeclaration())
        continue;
      KFunction *callKF = functionMap[callBlock->calledFunction];
      if (dist.find(callKF) == dist.end()) {
        dist[callKF] = dist[callKF] + 1;
        nodes.push_back(callKF);
      }
    }
    nodes.pop_front();
  }
}

bool KModule::link(std::vector<std::unique_ptr<llvm::Module>> &modules,
                   const std::string &entryPoint) {
  auto numRemainingModules = modules.size();
  // Add the currently active module to the list of linkables
  modules.push_back(std::move(module));
  std::string error;
  module = std::unique_ptr<llvm::Module>(
      klee::linkModules(modules, entryPoint, error));
  if (!module)
    klee_error("Could not link KLEE files %s", error.c_str());

  targetData = std::unique_ptr<llvm::DataLayout>(new DataLayout(module.get()));

  // Check if we linked anything
  return modules.size() != numRemainingModules;
}

void KModule::instrument(const Interpreter::ModuleOptions &opts) {
  // Inject checks prior to optimization... we also perform the
  // invariant transformations that we will end up doing later so that
  // optimize is seeing what is as close as possible to the final
  // module.
  legacy::PassManager pm;
  pm.add(new RaiseAsmPass());

  // This pass will scalarize as much code as possible so that the Executor
  // does not need to handle operands of vector type for most instructions
  // other than InsertElementInst and ExtractElementInst.
  //
  // NOTE: Must come before division/overshift checks because those passes
  // don't know how to handle vector instructions.
  pm.add(createScalarizerPass());

  // This pass will replace atomic instructions with non-atomic operations
  pm.add(createLowerAtomicPass());
  if (opts.CheckDivZero) pm.add(new DivCheckPass());
  if (opts.CheckOvershift) pm.add(new OvershiftCheckPass());

  pm.add(new IntrinsicCleanerPass(*targetData));
  pm.run(*module);
}

void KModule::optimiseAndPrepare(
    const Interpreter::ModuleOptions &opts,
    llvm::ArrayRef<const char *> preservedFunctions) {
  // Preserve all functions containing klee-related function calls from being
  // optimised around
  if (!OptimiseKLEECall) {
    legacy::PassManager pm;
    pm.add(new OptNonePass());
    pm.run(*module);
  }

  if (opts.Optimize)
    Optimize(module.get(), preservedFunctions);

  // Add internal functions which are not used to check if instructions
  // have been already visited
  if (opts.CheckDivZero)
    addInternalFunction("klee_div_zero_check");
  if (opts.CheckOvershift)
    addInternalFunction("klee_overshift_check");

  // Needs to happen after linking (since ctors/dtors can be modified)
  // and optimization (since global optimization can rewrite lists).
  injectStaticConstructorsAndDestructors(module.get(), opts.EntryPoint);

  // Finally, run the passes that maintain invariants we expect during
  // interpretation. We run the intrinsic cleaner just in case we
  // linked in something with intrinsics but any external calls are
  // going to be unresolved. We really need to handle the intrinsics
  // directly I think?
  legacy::PassManager pm3;
  pm3.add(createCFGSimplificationPass());
  switch(SwitchType) {
  case eSwitchTypeInternal: break;
  case eSwitchTypeSimple: pm3.add(new LowerSwitchPass()); break;
  case eSwitchTypeLLVM:  pm3.add(createLowerSwitchPass()); break;
  default: klee_error("invalid --switch-type");
  }
  pm3.add(new IntrinsicCleanerPass(*targetData));
  pm3.add(createScalarizerPass());
  pm3.add(new PhiCleanerPass());
  pm3.add(new FunctionAliasPass());
  pm3.run(*module);
}

static void splitByCall(Function *function) {
  unsigned n = function->getBasicBlockList().size();
  BasicBlock **blocks = new BasicBlock *[n];
  unsigned i = 0;
  for (llvm::Function::iterator bbit = function->begin(),
                                bbie = function->end();
       bbit != bbie; bbit++, i++) {
    blocks[i] = &*bbit;
  }

  for (unsigned j = 0; j < n; j++) {
    BasicBlock *fbb = blocks[j];
    llvm::BasicBlock::iterator it = fbb->begin();
    llvm::BasicBlock::iterator ie = fbb->end();
    Instruction *firstInst = &*it;
    while (it != ie) {
      if (isa<CallInst>(it)) {
        Instruction *callInst = &*it++;
        Instruction *afterCallInst = &*it;
        if (afterCallInst->isTerminator() && !isa<InvokeInst>(afterCallInst))
          continue;
        if (callInst != firstInst)
          fbb = fbb->splitBasicBlock(callInst);
        fbb = fbb->splitBasicBlock(afterCallInst);
        it = fbb->begin();
        ie = fbb->end();
        firstInst = &*it;
      } else if (isa<InvokeInst>(it)) {
        Instruction *invokeInst = &*it++;
        if (invokeInst != firstInst)
          fbb = fbb->splitBasicBlock(invokeInst);
      } else {
        it++;
      }
    }
  }

  delete[] blocks;
}

void KModule::manifest(InterpreterHandler *ih, bool forceSourceOutput) {

  for (auto &Function : *module) {
    splitByCall(&Function);
  }

  if (OutputSource || forceSourceOutput) {
    std::unique_ptr<llvm::raw_fd_ostream> os(ih->openOutputFile("assembly.ll"));
    assert(os && !os->has_error() && "unable to open source output");
    *os << *module;
  }

  if (OutputModule) {
    std::unique_ptr<llvm::raw_fd_ostream> f(ih->openOutputFile("final.bc"));
#if LLVM_VERSION_CODE >= LLVM_VERSION(7, 0)
    WriteBitcodeToFile(*module, *f);
#else
    WriteBitcodeToFile(module.get(), *f);
#endif
  }

  /* Build shadow structures */
  infos = std::unique_ptr<InstructionInfoTable>(
      new InstructionInfoTable(*module.get()));

  std::vector<Function *> declarations;

  for (auto &Function : *module) {
    if (Function.isDeclaration()) {
      declarations.push_back(&Function);
      continue;
    }

    auto kf = std::unique_ptr<KFunction>(new KFunction(&Function, this));

    llvm::Function *function = &Function;
    for (auto &BasicBlock : *function) {
      unsigned numInstructions = kf->blockMap[&BasicBlock]->numInstructions;
      KBlock *kb = kf->blockMap[&BasicBlock];
      for (unsigned i = 0; i < numInstructions; ++i) {
        KInstruction *ki = kb->instructions[i];
        ki->info = &infos->getInfo(*ki->inst);
      }
    }

    functionMap.insert(std::make_pair(&Function, kf.get()));
    functions.push_back(std::move(kf));
  }

  /* Compute various interesting properties */

  for (auto &kf : functions) {
    if (functionEscapes(kf->function))
      escapingFunctions.insert(kf->function);
  }

  for (auto &declaration : declarations) {
    if (functionEscapes(declaration))
      escapingFunctions.insert(declaration);
  }

  for (auto &kfp : functions) {
    for (auto &kcb : kfp.get()->kCallBlocks) {
      callMap[kcb->calledFunction].insert(kfp.get()->function);
    }
  }

  if (DebugPrintEscapingFunctions && !escapingFunctions.empty()) {
    llvm::errs() << "KLEE: escaping functions: [";
    std::string delimiter = "";
    for (auto &Function : escapingFunctions) {
      llvm::errs() << delimiter << Function->getName();
      delimiter = ", ";
    }
    llvm::errs() << "]\n";
  }
}



void KModule::checkModule() {
  InstructionOperandTypeCheckPass *operandTypeCheckPass =
      new InstructionOperandTypeCheckPass();

  legacy::PassManager pm;
  if (!DontVerify)
    pm.add(createVerifierPass());
  pm.add(operandTypeCheckPass);
  pm.run(*module);

  // Enforce the operand type invariants that the Executor expects.  This
  // implicitly depends on the "Scalarizer" pass to be run in order to succeed
  // in the presence of vector instructions.
  if (!operandTypeCheckPass->checkPassed()) {
    klee_error("Unexpected instruction operand types detected");
  }
}

KBlock *KModule::getKBlock(llvm::BasicBlock *bb) {
  return functionMap[bb->getParent()]->blockMap[bb];
}

std::map<KFunction *, unsigned int> &
KModule::getBackwardDistance(KFunction *kf) {
  if (backwardDistance.find(kf) == backwardDistance.end())
    calculateBackwardDistance(kf);
  return backwardDistance[kf];
}

std::map<KFunction *, unsigned int> &KModule::getDistance(KFunction *kf) {
  if (distance.find(kf) == distance.end())
    calculateDistance(kf);
  return distance[kf];
}

Function *llvm::getTargetFunction(Value *calledVal) {
  SmallPtrSet<const GlobalValue *, 3> Visited;

  Constant *c = dyn_cast<Constant>(calledVal);
  if (!c)
    return 0;

  while (true) {
    if (GlobalValue *gv = dyn_cast<GlobalValue>(c)) {
      if (!Visited.insert(gv).second)
        return 0;

      if (Function *f = dyn_cast<Function>(gv))
        return f;
      else if (GlobalAlias *ga = dyn_cast<GlobalAlias>(gv))
        c = ga->getAliasee();
      else
        return 0;
    } else if (llvm::ConstantExpr *ce = dyn_cast<llvm::ConstantExpr>(c)) {
      if (ce->getOpcode() == Instruction::BitCast)
        c = ce->getOperand(0);
      else
        return 0;
    } else
      return 0;
  }
}

KConstant* KModule::getKConstant(const Constant *c) {
  auto it = constantMap.find(c);
  if (it != constantMap.end())
    return it->second.get();
  return NULL;
}

unsigned KModule::getConstantID(Constant *c, KInstruction* ki) {
  if (KConstant *kc = getKConstant(c))
    return kc->id;  

  unsigned id = constants.size();
  auto kc = std::unique_ptr<KConstant>(new KConstant(c, id, ki));
  constantMap.insert(std::make_pair(c, std::move(kc)));
  constants.push_back(c);
  return id;
}

/***/

KConstant::KConstant(llvm::Constant* _ct, unsigned _id, KInstruction* _ki) {
  ct = _ct;
  id = _id;
  ki = _ki;
}

/***/

static int getOperandNum(Value *v,
                         std::map<Instruction*, unsigned> &registerMap,
                         KModule *km,
                         KInstruction *ki) {
  if (Instruction *inst = dyn_cast<Instruction>(v)) {
    return registerMap[inst];
  } else if (Argument *a = dyn_cast<Argument>(v)) {
    return a->getArgNo();
  } else if (isa<BasicBlock>(v) || isa<InlineAsm>(v) ||
             isa<MetadataAsValue>(v)) {
    return -1;
  } else {
    assert(isa<Constant>(v));
    Constant *c = cast<Constant>(v);
    return -(km->getConstantID(c, ki) + 2);
  }
}

void KBlock::handleKInstruction(std::map<Instruction *, unsigned> &registerMap,
                                llvm::Instruction *inst, KModule *km,
                                KInstruction *ki) {
  ki->parent = this;
  ki->inst = inst;
  ki->dest = registerMap[inst];
  if (isa<CallInst>(inst) || isa<InvokeInst>(inst)) {
#if LLVM_VERSION_CODE >= LLVM_VERSION(8, 0)
    const CallBase &cs = cast<CallBase>(*inst);
    Value *val = cs.getCalledOperand();
#else
    const CallSite cs(inst);
    Value *val = cs.getCalledValue();
#endif
    unsigned numArgs = cs.arg_size();
    ki->operands = new int[numArgs + 1];
    ki->operands[0] = getOperandNum(val, registerMap, km, ki);
    for (unsigned j = 0; j < numArgs; j++) {
      Value *v = cs.getArgOperand(j);
      ki->operands[j + 1] = getOperandNum(v, registerMap, km, ki);
    }
  } else {
    unsigned numOperands = inst->getNumOperands();
    ki->operands = new int[numOperands];
    for (unsigned j = 0; j < numOperands; j++) {
      Value *v = inst->getOperand(j);
      ki->operands[j] = getOperandNum(v, registerMap, km, ki);
    }
  }
}

KFunction::KFunction(llvm::Function *_function,
                     KModule *_km)
  : parent(_km),
    function(_function),
    numArgs(function->arg_size()),
    numInstructions(0),
    trackCoverage(true) {
  for (auto &BasicBlock : *function) {
    numInstructions += BasicBlock.size();
    numBlocks++;
  }
  instructions = new KInstruction*[numInstructions];
  std::map<Instruction*, unsigned> registerMap;
  // Assign unique instruction IDs to each basic block
  unsigned n = 0;
  // The first arg_size() registers are reserved for formals.
  unsigned rnum = numArgs;

  std::map<llvm::Value *, llvm::Type *> bitcastToType;
  for (llvm::Function::iterator bbit = function->begin(),
         bbie = function->end(); bbit != bbie; ++bbit) {
    for (llvm::BasicBlock::iterator it = bbit->begin(), ie = bbit->end();
         it != ie; ++it) {
      if (it->getOpcode() == Instruction::BitCast && bitcastToType.count(it->getOperand(0)) == 0) {
        bitcastToType.emplace(it->getOperand(0), it->getType());
      }
      registerMap[&*it] = rnum++;
    }
  }
  numRegisters = rnum;
  
  for (llvm::Function::iterator bbit = function->begin(),
                                bbie = function->end();
       bbit != bbie; ++bbit) {
    KBlock *kb;
    Instruction *it = &*(*bbit).begin();
    if (it->getOpcode() == Instruction::Call ||
        it->getOpcode() == Instruction::Invoke) {
#if LLVM_VERSION_CODE >= LLVM_VERSION(8, 0)
      const CallBase &cs = cast<CallBase>(*it);
      Value *fp = cs.getCalledOperand();
#else
      CallSite cs(it);
      Value *fp = cs.getCalledValue();
#endif
      Function *f = getTargetFunction(fp);
      KCallBlock *ckb = KCallBlock::computeKCallBlock(
          this, &*bbit, parent, registerMap, regToInst, f, &instructions[n],
          bitcastToType);
      kCallBlocks.push_back(ckb);
      kb = ckb;
    } else
      kb = new KBlock(this, &*bbit, parent, registerMap, regToInst,
                      &instructions[n]);
    for (unsigned i = 0; i < kb->numInstructions; i++, n++) {
      instructionMap[instructions[n]->inst] = instructions[n];
    }
    blockMap[&*bbit] = kb;
    blocks.push_back(std::unique_ptr<KBlock>(kb));
    if (isa<ReturnInst>(kb->instructions[kb->numInstructions - 1]->inst))
      finalKBlocks.push_back(kb);
  }

  entryKBlock = blockMap[&*function->begin()];
}

KFunction::~KFunction() {
  for (unsigned i = 0; i < numInstructions; ++i)
    delete instructions[i];
  delete[] instructions;
}

void KFunction::calculateDistance(KBlock *bb) {
  std::map<KBlock *, unsigned int> &dist = distance[bb];
  std::deque<KBlock *> nodes;
  nodes.push_back(bb);
  dist[bb] = 0;
  while (!nodes.empty()) {
    KBlock *currBB = nodes.front();
    for (auto const &succ : successors(currBB->basicBlock)) {
      if (dist.find(blockMap[succ]) == dist.end()) {
        dist[blockMap[succ]] = dist[currBB] + 1;
        nodes.push_back(blockMap[succ]);
      }
    }
    nodes.pop_front();
  }
}

void KFunction::calculateBackwardDistance(KBlock *bb) {
  std::map<KBlock *, unsigned int> &bdist = backwardDistance[bb];
  std::deque<KBlock *> nodes;
  nodes.push_back(bb);
  bdist[bb] = 0;
  while (!nodes.empty()) {
    KBlock *currBB = nodes.front();
    for (auto const &pred : predecessors(currBB->basicBlock)) {
      if (bdist.find(blockMap[pred]) == bdist.end()) {
        bdist[blockMap[pred]] = bdist[currBB] + 1;
        nodes.push_back(blockMap[pred]);
      }
    }
    nodes.pop_front();
  }
}

std::map<KBlock *, unsigned int> &KFunction::getDistance(KBlock *kb) {
  if (distance.find(kb) == distance.end())
    calculateDistance(kb);
  return distance[kb];
}

std::map<KBlock *, unsigned int> &KFunction::getBackwardDistance(KBlock *kb) {
  if (backwardDistance.find(kb) == backwardDistance.end())
    calculateBackwardDistance(kb);
  return backwardDistance[kb];
}

KBlock::KBlock(KFunction *_kfunction, llvm::BasicBlock *block, KModule *km,
               std::map<Instruction *, unsigned> &registerMap,
               std::map<unsigned, KInstruction *> &regToInst,
               KInstruction **instructionsKF)
    : parent(_kfunction), basicBlock(block), numInstructions(0),
      trackCoverage(true) {
  numInstructions += block->size();
  instructions = instructionsKF;

  unsigned i = 0;
  for (llvm::BasicBlock::iterator it = block->begin(), ie = block->end();
       it != ie; ++it) {
    KInstruction *ki;

    switch (it->getOpcode()) {
    case Instruction::GetElementPtr:
    case Instruction::InsertValue:
    case Instruction::ExtractValue:
      ki = new KGEPInstruction();
      break;
    default:
      ki = new KInstruction();
      break;
    }

    Instruction *inst = &*it;
    handleKInstruction(registerMap, inst, km, ki);
    instructions[i++] = ki;
    regToInst[registerMap[&*it]] = ki;
  }
}

const llvm::StringRef KCallAllocBlock::allocNewU = "_Znwj";
const llvm::StringRef KCallAllocBlock::allocNewUArray = "_Znaj";
const llvm::StringRef KCallAllocBlock::allocNewL = "_Znwm";
const llvm::StringRef KCallAllocBlock::allocNewLArray = "_Znam";
const llvm::StringRef KCallAllocBlock::allocMalloc = "malloc";
const llvm::StringRef KCallAllocBlock::allocCalloc = "calloc";
const llvm::StringRef KCallAllocBlock::allocMemalign = "memalign";
const llvm::StringRef KCallAllocBlock::allocRealloc = "realloc";


KCallBlock::KCallBlock(KFunction *_kfunction, llvm::BasicBlock *block, KModule *km,
                    std::map<Instruction*, unsigned> &registerMap, std::map<unsigned, KInstruction*> &regToInst,
                    llvm::Function *_calledFunction, KInstruction **instructionsKF)
  : KBlock::KBlock(_kfunction, block, km, registerMap, regToInst, instructionsKF),
    kcallInstruction(this->instructions[0]),
    calledFunction(_calledFunction) {}


KCallAllocBlock::KCallAllocBlock(KFunction *_kfunction, llvm::BasicBlock *block, KModule *km,
                    std::map<Instruction*, unsigned> &registerMap, std::map<unsigned, KInstruction*> &regToInst,
                    llvm::Function *_calledFunction, KInstruction **instructionsKF, llvm::Type *allocationType)
  : KCallBlock::KCallBlock(_kfunction, block, km, registerMap, regToInst, _calledFunction, instructionsKF), 
    allocationType(allocationType) {}


KCallBlock *KCallBlock::computeKCallBlock(KFunction *_kfunction, llvm::BasicBlock *block, KModule *km,
                    std::map<Instruction*, unsigned> &registerMap, std::map<unsigned, KInstruction*> &regToInst,
                    llvm::Function *_calledFunction, KInstruction **instructionsKF,
                    std::map<llvm::Value *, llvm::Type *> &bitcastToType) {
  if (_calledFunction) {
    llvm::StringRef functionName = _calledFunction->getName();
    llvm::Instruction *it = &*(block->begin());
    if (functionName.compare(KCallAllocBlock::allocNewU) == 0 ||
        functionName.compare(KCallAllocBlock::allocNewL) == 0 ||
        functionName.compare(KCallAllocBlock::allocNewUArray) == 0 ||
        functionName.compare(KCallAllocBlock::allocNewLArray) == 0) {
      assert(bitcastToType.count(it) && "Can not find associated bitcast");
      return new KCallAllocBlock(_kfunction, block, km, registerMap, regToInst, _calledFunction, instructionsKF, bitcastToType[it]);
    }
    if (functionName.compare(KCallAllocBlock::allocMalloc) == 0 ||
        functionName.compare(KCallAllocBlock::allocCalloc) == 0 ||
        functionName.compare(KCallAllocBlock::allocMemalign) == 0 ||
        functionName.compare(KCallAllocBlock::allocRealloc) == 0) {
      return new KCallAllocBlock(_kfunction, block, km, registerMap, regToInst, _calledFunction, instructionsKF, nullptr);
    }
  }

  return new KCallBlock(_kfunction, block, km, registerMap, regToInst, _calledFunction, instructionsKF);
}
