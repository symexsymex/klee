//===-- TargetForest.cpp --------------------------------------------------===//
//
//                     The KLEE Symbolic Virtual Machine
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "klee/Module/TargetForest.h"

#include "klee/Core/TargetedExecutionReporter.h"
#include "klee/Expr/Expr.h"
#include "klee/Module/KInstruction.h"
#include "klee/Module/KModule.h"
#include "klee/Module/SarifReport.h"
#include "klee/Module/Target.h"
#include "klee/Module/TargetHash.h"

#include "klee/Support/ErrorHandling.h"

using namespace klee;
using namespace llvm;

TargetForest::UnorderedTargetsSet::UnorderedTargetsSetCacheSet
    TargetForest::UnorderedTargetsSet::cachedVectors;

TargetForest::UnorderedTargetsSet::UnorderedTargetsSet(
    const ref<Target> &target) {
  targetsVec.push_back(target);
  sortAndComputeHash();
}

TargetForest::UnorderedTargetsSet::UnorderedTargetsSet(
    const TargetHashSet &targets) {
  for (const auto &p : targets) {
    targetsVec.push_back(p);
  }
  sortAndComputeHash();
}

unsigned TargetForest::UnorderedTargetsSet::sortAndComputeHash() {
  std::sort(targetsVec.begin(), targetsVec.end(), TargetLess{});
  unsigned res = targetsVec.size();
  for (auto r : targetsVec) {
    res = (res * Expr::MAGIC_HASH_CONSTANT) + r->hash();
  }
  hashValue = res;
  return res;
}

int TargetForest::UnorderedTargetsSet::compare(
    const TargetForest::UnorderedTargetsSet &other) const {
  if (targetsVec != other.targetsVec) {
    return targetsVec < other.targetsVec ? -1 : 1;
  }
  return 0;
}

bool TargetForest::UnorderedTargetsSet::equals(
    const TargetForest::UnorderedTargetsSet &b) const {
  return (toBeCleared || b.toBeCleared) || (isCached && b.isCached)
             ? this == &b
             : compare(b) == 0;
}

bool TargetForest::UnorderedTargetsSet::operator<(
    const TargetForest::UnorderedTargetsSet &other) const {
  return compare(other) == -1;
}

bool TargetForest::UnorderedTargetsSet::operator==(
    const TargetForest::UnorderedTargetsSet &other) const {
  return equals(other);
}

ref<TargetForest::UnorderedTargetsSet>
TargetForest::UnorderedTargetsSet::create(const ref<Target> &target) {
  UnorderedTargetsSet *vec = new UnorderedTargetsSet(target);
  return create(vec);
}

ref<TargetForest::UnorderedTargetsSet>
TargetForest::UnorderedTargetsSet::create(const TargetHashSet &targets) {
  UnorderedTargetsSet *vec = new UnorderedTargetsSet(targets);
  return create(vec);
}

ref<TargetForest::UnorderedTargetsSet>
TargetForest::UnorderedTargetsSet::create(ref<UnorderedTargetsSet> vec) {
  std::pair<CacheType::const_iterator, bool> success =
      cachedVectors.cache.insert(vec.get());
  if (success.second) {
    // Cache miss
    vec->isCached = true;
    return vec;
  }
  // Cache hit
  return (ref<UnorderedTargetsSet>)*(success.first);
}

TargetForest::UnorderedTargetsSet::~UnorderedTargetsSet() {
  if (isCached) {
    toBeCleared = true;
    cachedVectors.cache.erase(this);
  }
}

void TargetForest::Layer::addTrace(
    const Result &result,
    KFunction *entryKF,
    const std::unordered_map<ref<Location>, std::unordered_set<KBlock *>,
                             RefLocationHash, RefLocationCmp> &locToBlocks,
    bool reversed) {
  auto forest = this;
  for (size_t count = 0; count < result.locations.size(); ++count) {
    size_t i = reversed ? result.locations.size() - count - 1 : count;
    const auto &loc = result.locations[i];
    auto it = locToBlocks.find(loc);
    assert(it != locToBlocks.end());
    TargetHashSet targets;
    for (auto block : it->second) {
      ref<Target> target = nullptr;
      if (i == result.locations.size() - 1) {
        target = ReproduceErrorTarget::create(
            result.errors, result.id,
            ErrorLocation{loc->startLine, loc->endLine, loc->startColumn,
                          loc->endColumn},
            block);
      } else {
        target = ReachBlockTarget::create(block);
      }
      targets.insert(target);
    }

    ref<UnorderedTargetsSet> targetsVec = UnorderedTargetsSet::create(targets);
    if (forest->forest.count(targetsVec) == 0) {
      ref<TargetForest::Layer> next = new TargetForest::Layer();
      forest->insert(targetsVec, next);
    }

    for (auto &target : targetsVec->getTargets()) {
      forest->insertTargetsToVec(target, targetsVec);
    }

    forest = forest->forest[targetsVec].get();
  }

  if (reversed) {
    TargetHashSet targets;
    targets.insert(ReachBlockTarget::create(entryKF->entryKBlock));
    ref<UnorderedTargetsSet> targetsVec = UnorderedTargetsSet::create(targets);
    if (forest->forest.count(targetsVec) == 0) {
      ref<TargetForest::Layer> next = new TargetForest::Layer();
      forest->insert(targetsVec, next);
    }

    for (auto &target : targetsVec->getTargets()) {
      forest->insertTargetsToVec(target, targetsVec);
    }

    forest = forest->forest[targetsVec].get();
  }
}

void TargetForest::Layer::addTrace(const KBlockTrace &trace, bool reversed) {
  auto forest = this;
  auto entryKF = (*(trace.front().begin()))->parent;
  for (unsigned count = 0; count < trace.size(); count++) {
    unsigned i = reversed ? trace.size() - count - 1 : count;
    TargetHashSet targets;
    for (auto block : trace.at(i)) {
      if (i == trace.size() - 1) {
        targets.insert(ReproduceErrorTarget::create({ReachWithError::Reachable},
                                                    "", {}, block));
      } else {
        targets.insert(ReachBlockTarget::create(block));
      }
    }

    ref<UnorderedTargetsSet> targetsVec = UnorderedTargetsSet::create(targets);
    if (forest->forest.count(targetsVec) == 0) {
      ref<TargetForest::Layer> next = new TargetForest::Layer();
      forest->insert(targetsVec, next);
    }

    for (auto &target : targetsVec->getTargets()) {
      forest->insertTargetsToVec(target, targetsVec);
    }

    forest = forest->forest[targetsVec].get();
  }

  if (reversed) {
    TargetHashSet targets;
    targets.insert(ReachBlockTarget::create(entryKF->entryKBlock));
    ref<UnorderedTargetsSet> targetsVec = UnorderedTargetsSet::create(targets);
    if (forest->forest.count(targetsVec) == 0) {
      ref<TargetForest::Layer> next = new TargetForest::Layer();
      forest->insert(targetsVec, next);
    }

    for (auto &target : targetsVec->getTargets()) {
      forest->insertTargetsToVec(target, targetsVec);
    }

    forest = forest->forest[targetsVec].get();
  }
}

void TargetForest::Layer::propagateConfidenceToChildren() {
  auto parentConfidence = getConfidence();
  for (auto &kv : forest) {
    kv.second->confidence = kv.second->getConfidence(parentConfidence);
  }
}

void TargetForest::Layer::unionWith(TargetForest::Layer *other) {
  if (other->forest.empty())
    return;
  other->propagateConfidenceToChildren();
  for (const auto &kv : other->forest) {
    auto it = forest.find(kv.first);
    if (it == forest.end()) {
      forest.insert(std::make_pair(kv.first, kv.second));
      continue;
    }
    auto layer = new Layer(it->second);
    layer->unionWith(kv.second.get());
    it->second = layer;
  }

  for (const auto &kv : other->targetsToVector) {
    auto it = targetsToVector.find(kv.first);
    if (it == targetsToVector.end()) {
      targetsToVector.insert(std::make_pair(kv.first, kv.second));
      targets.insert(kv.first);
      continue;
    }
    it->second.insert(kv.second.begin(), kv.second.end());
  }
}

void TargetForest::Layer::block(ref<Target> target) {
  if (empty())
    return;
  removeTarget(target);
  for (InternalLayer::iterator itf = forest.begin(); itf != forest.end();) {
    ref<Layer> layer = itf->second->blockLeaf(target);
    itf->second = layer;
    if (itf->second->empty()) {
      for (auto &itfTarget : itf->first->getTargets()) {
        auto it = targetsToVector.find(itfTarget);
        if (it != targetsToVector.end()) {
          it->second.erase(itf->first);
          if (it->second.empty()) {
            targetsToVector.erase(it);
            targets.erase(itfTarget);
          }
        }
      }
      itf = forest.erase(itf);
    } else {
      ++itf;
    }
  }
}

void TargetForest::Layer::removeTarget(ref<Target> target) {
  auto it = targetsToVector.find(target);

  if (it == targetsToVector.end()) {
    return;
  }

  auto targetsVectors = std::move(it->second);

  targetsToVector.erase(it);
  targets.erase(target);

  for (auto &targetsVec : targetsVectors) {
    bool shouldDelete = true;

    for (auto &localTarget : targetsVec->getTargets()) {
      if (targetsToVector.count(localTarget) != 0) {
        shouldDelete = false;
      }
    }

    if (shouldDelete) {
      forest.erase(targetsVec);
    }
  }
}

bool TargetForest::Layer::deepFind(ref<Target> target) const {
  if (empty())
    return false;
  if (targetsToVector.count(target) != 0) {
    return true;
  }
  for (auto &f : forest) {
    if (f.second->deepFind(target))
      return true;
  }
  return false;
}

bool TargetForest::Layer::deepFindIn(ref<Target> child,
                                     ref<Target> target) const {
  if (targetsToVector.count(child) == 0) {
    return false;
  }
  auto &res = targetsToVector.at(child);

  if (child == target) {
    return true;
  }
  for (auto &targetsVec : res) {
    auto &it = forest.at(targetsVec);
    if (it->deepFind(target)) {
      return true;
    }
  }

  return false;
}

TargetForest::Layer *TargetForest::Layer::removeChild(ref<Target> child) const {
  auto result = new Layer(this);
  result->removeTarget(child);
  return result;
}

TargetForest::Layer *
TargetForest::Layer::removeChild(ref<UnorderedTargetsSet> child) const {
  auto result = new Layer(this);
  result->forest.erase(child);
  for (auto &target : child->getTargets()) {
    auto it = result->targetsToVector.find(target);
    if (it == result->targetsToVector.end()) {
      continue;
    }

    it->second.erase(child);
    if (it->second.empty()) {
      result->targetsToVector.erase(it);
      result->targets.erase(target);
    }
  }
  return result;
}

TargetForest::Layer *TargetForest::Layer::addChild(ref<Target> child) const {
  auto targetsVec = UnorderedTargetsSet::create(child);
  auto result = new Layer(this);
  if (forest.count(targetsVec) != 0) {
    return result;
  }
  result->forest.insert({targetsVec, new Layer()});

  result->targetsToVector[child].insert(targetsVec);
  result->targets.insert(child);
  return result;
}

TargetForest::Layer *
TargetForest::Layer::addChild(ref<UnorderedTargetsSet> child) const {
  auto result = new Layer(this);
  if (forest.count(child) != 0) {
    return result;
  }
  result->forest.insert({child, new Layer()});

  for (auto &target : child->getTargets()) {
    result->insertTargetsToVec(target, child);
  }
  return result;
}

bool TargetForest::Layer::hasChild(ref<UnorderedTargetsSet> child) const {
  return forest.count(child) != 0;
}

TargetForest::Layer *
TargetForest::Layer::blockLeafInChild(ref<UnorderedTargetsSet> child,
                                      ref<Target> leaf) const {
  auto subtree = forest.find(child);
  assert(subtree != forest.end());
  if (subtree->second->forest.empty()) {
    auto targetsVectors = targetsToVector.find(leaf);
    if (targetsVectors == targetsToVector.end() ||
        targetsVectors->second.count(subtree->first) == 0) {
      return new Layer(this);
    } else {
      return removeChild(leaf);
    }
  }

  ref<Layer> sublayer = new Layer(subtree->second);
  sublayer->block(leaf);
  if (sublayer->empty()) {
    return removeChild(child);
  } else {
    InternalLayer subforest;
    subforest[child] = sublayer;
    TargetsToVector subTargetsToVector;
    for (auto &target : child->getTargets()) {
      subTargetsToVector[target].insert(child);
    }
    sublayer = new Layer(subforest, subTargetsToVector, confidence);
    auto result = replaceChildWith(child, sublayer.get());
    return result;
  }
}

TargetForest::Layer *
TargetForest::Layer::blockLeafInChild(ref<Target> child,
                                      ref<Target> leaf) const {
  TargetForest::Layer *res = nullptr;
  auto it = targetsToVector.find(child);
  if (it == targetsToVector.end()) {
    return res;
  }
  for (auto &targetsVec : it->second) {
    if (res) {
      res = res->blockLeafInChild(targetsVec, leaf);
    } else {
      res = blockLeafInChild(targetsVec, leaf);
    }
  }
  return res;
}

TargetForest::Layer *TargetForest::Layer::blockLeaf(ref<Target> leaf) const {
  auto result = new Layer(this);
  if (forest.empty()) {
    return result;
  }

  for (auto &layer : forest) {
    result = ref<Layer>(result)->blockLeafInChild(layer.first, leaf);
  }
  return result;
}

TargetForest::Layer *TargetForest::Layer::replaceChildWith(
    ref<TargetForest::UnorderedTargetsSet> const child,
    TargetForest::Layer *other) const {
  auto result = removeChild(child);
  result->unionWith(other);
  return result;
}

TargetForest::Layer *TargetForest::Layer::replaceChildWith(
    ref<Target> child,
    const std::unordered_set<ref<UnorderedTargetsSet>, UnorderedTargetsSetHash,
                             UnorderedTargetsSetCmp> &other) const {
  std::vector<Layer *> layers;
  for (auto &targetsVec : other) {
    auto it = forest.find(targetsVec);
    assert(it != forest.end());
    layers.push_back(it->second.get());
  }
  auto result = new Layer(this);
  for (auto &targetsVec : other) {
    for (auto &target : targetsVec->getTargets()) {
      auto it = result->targetsToVector.find(target);
      if (it != result->targetsToVector.end()) {
        it->second.erase(targetsVec);
        if (it->second.empty()) {
          result->targetsToVector.erase(it);
          result->targets.erase(target);
        }
      }
    }
    result->forest.erase(targetsVec);
  }
  for (auto layer : layers) {
    result->unionWith(layer);
  }
  return result;
}

void TargetForest::Layer::dump(unsigned n) const {
  llvm::errs() << "THE " << n << " LAYER:\n";
  llvm::errs() << "Confidence: " << confidence << "\n";
  for (const auto &kv : targetsToVector) {
    llvm::errs() << kv.first->toString() << "\n";
  }
  llvm::errs() << "-----------------------\n";
  if (!forest.empty()) {
    for (const auto &kv : forest) {
      kv.second->dump(n + 1);
    }
    llvm::errs() << "++++++++++++++++++++++\n";
  }
}

TargetsHistory::TargetHistoryCacheSet TargetsHistory::cachedHistories;

ref<TargetsHistory>
TargetsHistory::create(ref<Target> _target,
                       ref<TargetsHistory> _visitedTargets) {
  ref<TargetsHistory> history = new TargetsHistory(_target, _visitedTargets);

  std::pair<CacheType::const_iterator, bool> success =
      cachedHistories.cache.insert(history.get());
  if (success.second) {
    // Cache miss
    history->isCached = true;
    return history;
  }
  // Cache hit
  return (ref<TargetsHistory>)*(success.first);
}

ref<TargetsHistory> TargetsHistory::create(ref<Target> _target) {
  return create(_target, nullptr);
}

ref<TargetsHistory> TargetsHistory::create() { return create(nullptr); }

int TargetsHistory::compare(const TargetsHistory &h) const {
  if (this == &h)
    return 0;

  if (size() != h.size()) {
    return (size() < h.size()) ? -1 : 1;
  }

  if (size() == 0) {
    return 0;
  }

  assert(target && h.target);
  if (target != h.target) {
    return (target < h.target) ? -1 : 1;
  }

  assert(next && h.next);
  if (next != h.next) {
    return (next < h.next) ? -1 : 1;
  }

  return 0;
}

bool TargetsHistory::equals(const TargetsHistory &b) const {
  return (toBeCleared || b.toBeCleared) || (isCached && b.isCached)
             ? this == &b
             : compare(b) == 0;
}

void TargetsHistory::dump() const {
  if (target) {
    llvm::errs() << target->toString() << "\n";
  } else {
    llvm::errs() << "end.\n";
    assert(!next);
    return;
  }
  if (next)
    next->dump();
}

TargetsHistory::~TargetsHistory() {
  if (isCached) {
    toBeCleared = true;
    cachedHistories.cache.erase(this);
  }
}

void TargetForest::stepTo(ref<Target> loc) {
  if (forest->empty())
    return;
  auto res = forest->find(loc);
  if (res == forest->end()) {
    return;
  }
  history = history->add(loc);
  forest = forest->replaceChildWith(loc, res->second);
}

void TargetForest::add(ref<Target> target) {
  if (forest->find(target) != forest->end()) {
    return;
  }
  forest = forest->addChild(target);
}

void TargetForest::add(ref<UnorderedTargetsSet> target) {
  if (forest->hasChild(target)) {
    return;
  }
  forest = forest->addChild(target);
}

void TargetForest::remove(ref<Target> target) {
  if (forest->find(target) == forest->end()) {
    return;
  }
  forest = forest->removeChild(target);
}

void TargetForest::blockIn(ref<Target> subtarget, ref<Target> target) {
  if (!forest->deepFindIn(subtarget, target)) {
    return;
  }
  forest = forest->blockLeafInChild(subtarget, target);
}

void TargetForest::block(const ref<Target> &target) {
  if (!forest->deepFind(target)) {
    return;
  }

  forest = forest->blockLeaf(target);
}

void TargetForest::dump() const {
  llvm::errs() << "TargetHistory:\n";
  history->dump();
  llvm::errs() << "Forest:\n";
  forest->dump(1);
}

void TargetForest::Layer::addLeafs(
    std::vector<std::pair<ref<UnorderedTargetsSet>, confidence::ty>> &leafs,
    confidence::ty parentConfidence) const {
  for (const auto &targetAndForest : forest) {
    auto targetsVec = targetAndForest.first;
    auto layer = targetAndForest.second;
    auto confidence = layer->getConfidence(parentConfidence);
    if (layer->empty()) {
      leafs.push_back(std::make_pair(targetsVec, confidence));
    } else {
      layer->addLeafs(leafs, confidence);
    }
  }
}

std::vector<std::pair<ref<TargetForest::UnorderedTargetsSet>, confidence::ty>>
TargetForest::leafs() const {
  std::vector<std::pair<ref<UnorderedTargetsSet>, confidence::ty>> leafs;
  forest->addLeafs(leafs, forest->getConfidence());
  return leafs;
}

ref<TargetForest> TargetForest::deepCopy() {
  return new TargetForest(forest->deepCopy(), entryFunction);
}

ref<TargetForest::Layer> TargetForest::Layer::deepCopy() {
  auto copyForest = new TargetForest::Layer(this);
  for (auto &targetAndForest : forest) {
    auto targetsVec = targetAndForest.first;
    auto layer = targetAndForest.second;
    copyForest->forest[targetsVec] = layer->deepCopy();
  }
  return copyForest;
}

void TargetForest::Layer::divideConfidenceBy(unsigned factor) {
  for (auto &targetAndForest : forest) {
    auto layer = targetAndForest.second;
    layer->confidence /= factor;
  }
}

TargetForest::Layer *TargetForest::Layer::divideConfidenceBy(
    const TargetForest::TargetToStateSetMap &reachableStatesOfTarget) {
  if (forest.empty() || reachableStatesOfTarget.empty())
    return this;
  auto result = new Layer(this);
  for (auto &targetAndForest : forest) {
    auto targetsVec = targetAndForest.first;
    auto layer = targetAndForest.second;
    std::unordered_set<ExecutionState *> reachableStatesForTargetsVec;
    for (const auto &target : targetsVec->getTargets()) {
      auto it = reachableStatesOfTarget.find(target);
      if (it == reachableStatesOfTarget.end()) {
        continue;
      }

      for (auto state : it->second) {
        reachableStatesForTargetsVec.insert(state);
      }
    }

    size_t count = reachableStatesForTargetsVec.size();
    if (count) {
      if (count == 1)
        continue;
      auto next = new Layer(layer);
      result->forest[targetsVec] = next;
      next->confidence /= count;
    } else
      result->forest[targetsVec] =
          layer->divideConfidenceBy(reachableStatesOfTarget);
  }
  return result;
}
