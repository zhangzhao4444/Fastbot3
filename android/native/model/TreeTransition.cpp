/**
 * @authors Zhao Zhang
 *
 * @file TreeTransition.cpp
 * @brief Implements transition indexing, nondeterministic branch sampling/pairing, and refinement context helpers.
 */

#include "TreeTransition.h"
#include "Model.h"

#include <algorithm>

namespace fastbotx {

/** Fills `outIndex` with pointers into `transitionLog` keyed by `transitionSeq` for matching activity and action. */
void buildTreeTransitionIndex(const std::vector<TreeTransitionEntry> &transitionLog,
                              const std::string &sourceActivity,
                              uintptr_t actionHash,
                              TreeTransitionIndex *outIndex) {
    if (!outIndex) {
        return;
    }
    outIndex->clear();
    outIndex->reserve(transitionLog.size());
    for (const auto &te : transitionLog) {
        if (!te.valid || te.sourceActivity != sourceActivity || te.actionHash != actionHash) {
            continue;
        }
        (*outIndex)[te.transitionSeq] = &te;
    }
}

/** Linear scan for the first valid row with `transitionSeq` and populated target bounds. */
bool findTreeTransitionTargetBoundsBySeq(const std::vector<TreeTransitionEntry> &transitionLog,
                                         uint64_t transitionSeq,
                                         Rect *outBounds) {
    for (const auto &te : transitionLog) {
        if (!te.valid || te.transitionSeq != transitionSeq || !te.hasTargetBounds) {
            continue;
        }
        if (outBounds) {
            *outBounds = te.targetBounds;
        }
        return true;
    }
    return false;
}

/**
 * Rotates through `transitionLog` as a ring starting at `transitionLogWriteIndex`, keeps rows that match
 * activity/action/source-key/target-set filters and have XML + tree-resolution data, then assigns increasing `seq`.
 */
void collectOrderedNondetBranchSourceSamples(
    const std::vector<ApeTransitionEntry> &transitionLog,
    size_t transitionLogWriteIndex,
    const std::string &sourceActivity,
    uintptr_t sourceKeyHash,
    uintptr_t actionHash,
    const std::unordered_set<uintptr_t> &targetKeyHashes,
    const TreeTransitionIndex &treeBySeq,
    const std::function<bool(uintptr_t)> &acceptSourceStateHash,
    const TransitionXmlLoader &sourceXmlLoader,
    std::vector<NondetBranchSourceSample> *outOrdered,
    NondetBranchInputStats *outStats) {
    if (!outOrdered) {
        return;
    }
    outOrdered->clear();
    outOrdered->reserve(transitionLog.size());
    if (outStats) {
        *outStats = NondetBranchInputStats{};
        outStats->logN = transitionLog.size();
    }

    uint64_t seq = 0;
    for (size_t i = 0; i < transitionLog.size(); ++i) {
        const size_t idx = (transitionLogWriteIndex + i) % transitionLog.size();
        const ApeTransitionEntry &te = transitionLog[idx];
        if (!te.valid) {
            continue;
        }
        if (te.sourceActivity != sourceActivity || te.sourceKeyHash != sourceKeyHash ||
            te.actionHash != actionHash) {
            if (outStats) {
                ++outStats->filteredByActivityOrPair;
            }
            continue;
        }
        if (targetKeyHashes.count(te.targetKeyHash) == 0) {
            if (outStats) {
                ++outStats->filteredByTarget;
            }
            continue;
        }
        std::string sourceXml;
        if (!sourceXmlLoader || !sourceXmlLoader(te, &sourceXml) || sourceXml.empty()) {
            if (outStats) {
                ++outStats->filteredBySnapshot;
            }
            continue;
        }
        auto itTree = treeBySeq.find(te.transitionSeq);
        if (itTree == treeBySeq.end() || !itTree->second || itTree->second->resolvedNodeStableIds.empty()) {
            if (outStats) {
                ++outStats->filteredBySnapshot;
            }
            continue;
        }
        if (acceptSourceStateHash && !acceptSourceStateHash(te.sourceStateHash)) {
            if (outStats) {
                ++outStats->filteredBySourceStateKey;
            }
            continue;
        }
        ++seq;
        outOrdered->push_back(NondetBranchSourceSample{
            seq, te.transitionSeq, std::move(sourceXml), te.sourceStateHash, te.targetKeyHash,
            te.targetStateHash, itTree->second->resolvedNodeStableIds});
    }
    if (outStats) {
        outStats->orderedCount = outOrdered->size();
    }
}

/**
 * Picks the reference sample `nstTransitionSeq`, groups rows by `targetKeyHash`, then emits one pair per
 * alternate target key vs the NST’s target (sorted XML + transition metadata on both branches).
 */
void buildNondetTreeTransitionBranchPairsFromOrderedSamples(
    const std::vector<NondetBranchSourceSample> &ordered,
    uint64_t nstTransitionSeq,
    std::vector<NondetTreeTransitionBranchPair> *outPairs) {
    if (!outPairs) {
        return;
    }
    outPairs->clear();
    if (ordered.size() < 2 || nstTransitionSeq == 0) {
        return;
    }

    const NondetBranchSourceSample *nst = nullptr;
    for (const auto &sx : ordered) {
        if (sx.transitionSeq == nstTransitionSeq) {
            nst = &sx;
            break;
        }
    }
    if (!nst) {
        return;
    }

    std::unordered_map<uintptr_t, std::vector<NondetBranchSourceSample>> byTarget;
    byTarget.reserve(8);
    for (const auto &sx : ordered) {
        byTarget[sx.targetKeyHash].push_back(sx);
    }

    auto itNstTarget = byTarget.find(nst->targetKeyHash);
    if (itNstTarget == byTarget.end()) {
        return;
    }
    std::vector<NondetBranchSourceSample> vecB = itNstTarget->second;
    std::sort(vecB.begin(), vecB.end(),
              [](const NondetBranchSourceSample &a, const NondetBranchSourceSample &b) {
                  return a.seq < b.seq;
              });

    for (const auto &kv : byTarget) {
        if (kv.first == nst->targetKeyHash) {
            continue;
        }
        NondetTreeTransitionBranchPair bp;
        bp.sourceStateHash = nst->sourceStateHash;
        bp.targetKeyA = kv.first;
        bp.targetKeyB = nst->targetKeyHash;
        bp.nstTargetStateHash = nst->targetStateHash;
        uint64_t minSeq = 0;

        auto vecA = kv.second;
        std::sort(vecA.begin(), vecA.end(),
                  [](const NondetBranchSourceSample &a, const NondetBranchSourceSample &b) {
                      return a.seq < b.seq;
                  });

        for (const auto &sx : vecA) {
            if (sx.xml.empty()) {
                continue;
            }
            bp.branchA.push_back(sx.xml);
#if defined(FASTBOT_HAS_PUGIXML) && FASTBOT_HAS_PUGIXML
            bp.branchATransitions.push_back(NondetTreeTransitionBranchPair::SourceTransition{
                sx.transitionSeq, sx.sourceStateHash, sx.targetStateHash, sx.xml, sx.resolvedNodeStableIds});
#else
            bp.branchATransitions.push_back(NondetTreeTransitionBranchPair::SourceTransition{
                sx.transitionSeq, sx.sourceStateHash, sx.targetStateHash, sx.xml, sx.resolvedNodeStableIds});
#endif
            if (minSeq == 0 || sx.seq < minSeq) {
                minSeq = sx.seq;
            }
        }
        for (const auto &sx : vecB) {
            if (sx.xml.empty()) {
                continue;
            }
            bp.branchB.push_back(sx.xml);
#if defined(FASTBOT_HAS_PUGIXML) && FASTBOT_HAS_PUGIXML
            bp.branchBTransitions.push_back(NondetTreeTransitionBranchPair::SourceTransition{
                sx.transitionSeq, sx.sourceStateHash, sx.targetStateHash, sx.xml, sx.resolvedNodeStableIds});
#else
            bp.branchBTransitions.push_back(NondetTreeTransitionBranchPair::SourceTransition{
                sx.transitionSeq, sx.sourceStateHash, sx.targetStateHash, sx.xml, sx.resolvedNodeStableIds});
#endif
            if (minSeq == 0 || sx.seq < minSeq) {
                minSeq = sx.seq;
            }
        }
        bp.firstSeenSeq = minSeq;
        bp.sourceTransitionSeq = minSeq;
        if (!bp.branchA.empty() && !bp.branchB.empty()) {
            outPairs->push_back(std::move(bp));
        }
    }
}

/** Validates that `nstTransitionSeq` appears in `ordered`, then builds and sorts branch pairs. */
NondetBranchBuildResult buildNondetBranchPairs(const std::vector<NondetBranchSourceSample> &ordered,
                                               uint64_t nstTransitionSeq) {
    NondetBranchBuildResult r;
    if (nstTransitionSeq == 0 || ordered.empty()) {
        return r;
    }
    for (const auto &sx : ordered) {
        if (sx.transitionSeq == nstTransitionSeq) {
            r.nstSeqFound = true;
            break;
        }
    }
    if (!r.nstSeqFound) {
        return r;
    }
    buildNondetTreeTransitionBranchPairsFromOrderedSamples(ordered, nstTransitionSeq, &r.branchPairs);
    sortNondetTreeTransitionBranchPairs(&r.branchPairs);
    return r;
}

/** Builds the tree index from `treeTransitionLog`, collects ordered samples from the ring buffer, then branch pairs. */
NondetBranchCollectionResult collectNondetBranchPairsForRefine(
    const std::vector<ApeTransitionEntry> &transitionLog,
    size_t transitionLogWriteIndex,
    const std::vector<TreeTransitionEntry> &treeTransitionLog,
    const std::string &sourceActivity,
    uintptr_t sourceKeyHash,
    uintptr_t actionHash,
    const std::unordered_set<uintptr_t> &targetKeyHashes,
    const std::function<bool(uintptr_t)> &acceptSourceStateHash,
    const TransitionXmlLoader &sourceXmlLoader,
    uint64_t nstTransitionSeq) {
    NondetBranchCollectionResult result;
    TreeTransitionIndex treeBySeq;
    buildTreeTransitionIndex(treeTransitionLog, sourceActivity, actionHash, &treeBySeq);

    std::vector<NondetBranchSourceSample> ordered;
    collectOrderedNondetBranchSourceSamples(
        transitionLog, transitionLogWriteIndex, sourceActivity, sourceKeyHash, actionHash,
        targetKeyHashes, treeBySeq, acceptSourceStateHash, sourceXmlLoader, &ordered, &result.inputStats);
    result.orderedCount = ordered.size();

    NondetBranchBuildResult build = buildNondetBranchPairs(ordered, nstTransitionSeq);
    result.nstSeqFound = build.nstSeqFound;
    result.branchPairs = std::move(build.branchPairs);
    return result;
}

/** Prefers branch A’s first source hash for the predicate; pulls bounds from `treeTransitionLog` when present. */
NondetActionRefineTransitionContext buildNondetActionRefineTransitionContext(
    const std::vector<NondetTreeTransitionBranchPair::SourceTransition> &branchATransitions,
    const std::vector<NondetTreeTransitionBranchPair::SourceTransition> &branchBTransitions,
    const std::vector<TreeTransitionEntry> &treeTransitionLog) {
    NondetActionRefineTransitionContext ctx;
    if (!branchATransitions.empty()) {
        ctx.actionPredicateSourceStateHash = branchATransitions.front().sourceStateHash;
    } else if (!branchBTransitions.empty()) {
        ctx.actionPredicateSourceStateHash = branchBTransitions.front().sourceStateHash;
    }
    for (const auto &t : branchATransitions) {
        if (findTreeTransitionTargetBoundsBySeq(treeTransitionLog, t.transitionSeq, &ctx.selectedTargetBounds)) {
            ctx.selectedTargetBoundsFound = true;
            return ctx;
        }
    }
    for (const auto &t : branchBTransitions) {
        if (findTreeTransitionTargetBoundsBySeq(treeTransitionLog, t.transitionSeq, &ctx.selectedTargetBounds)) {
            ctx.selectedTargetBoundsFound = true;
            return ctx;
        }
    }
    return ctx;
}

/** Orders branch pairs for stable refinement: earliest `firstSeenSeq`, then state hash, then target keys. */
void sortNondetTreeTransitionBranchPairs(std::vector<NondetTreeTransitionBranchPair> *pairs) {
    if (!pairs) {
        return;
    }
    std::sort(pairs->begin(), pairs->end(),
              [](const NondetTreeTransitionBranchPair &a, const NondetTreeTransitionBranchPair &b) {
                  if (a.firstSeenSeq != b.firstSeenSeq) {
                      return a.firstSeenSeq < b.firstSeenSeq;
                  }
                  if (a.sourceStateHash != b.sourceStateHash) {
                      return a.sourceStateHash < b.sourceStateHash;
                  }
                  if (a.targetKeyA != b.targetKeyA) {
                      return a.targetKeyA < b.targetKeyA;
                  }
                  return a.targetKeyB < b.targetKeyB;
              });
}

} // namespace fastbotx
