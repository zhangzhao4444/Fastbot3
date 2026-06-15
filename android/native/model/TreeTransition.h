/**
 * @authors Zhao Zhang
 *
 * @file TreeTransition.h
 * @brief Helpers for analyzing logged UI transitions: index by sequence, nondeterministic branch pairing,
 *        and refinement context for action predicates. Consumes `Model` transition logs (`ApeTransitionEntry`)
 *        and optional `GUITree` snapshots when pugixml is enabled.
 */
#ifndef FASTBOTX_MODEL_TREE_TRANSITION_H_
#define FASTBOTX_MODEL_TREE_TRANSITION_H_

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "Action.h"
#include "Base.h"

#if defined(FASTBOT_HAS_PUGIXML) && FASTBOT_HAS_PUGIXML
#include "../desc/gui_tree/GUITree.h"
#endif

namespace fastbotx {

/** Opaque transition row type defined in `Model` (ring buffer of executed transitions). */
struct ApeTransitionEntry;

/** One tree-backed transition record: hashes, action metadata, resolved widget ids, optional bounds / path. */
struct TreeTransitionEntry {
    uint64_t transitionSeq{0};
    uintptr_t sourceStateHash{0};
    uintptr_t targetStateHash{0};
    uintptr_t actionHash{0};
    ActionType actionType{ActionType::NOP};
    std::vector<int> resolvedNodeStableIds;
    bool hasTargetBounds{false};
    Rect targetBounds{};
    bool hasTargetFullPath{false};
    uintptr_t targetFullPathHash{0};
    std::string sourceActivity;
    bool valid{false};
};

/** Two-way comparison material for nondeterministic refinement: XML strings and optional GUI snapshots per branch. */
struct NondetTreeTransitionBranchPair {
    struct SourceTransition {
        uint64_t transitionSeq{0};
        uintptr_t sourceStateHash{0};
        uintptr_t targetStateHash{0};
        std::string sourceXml;
        std::vector<int> resolvedNodeStableIds;
    };

    uintptr_t sourceStateHash{0};
    uint64_t sourceTransitionSeq{0};
    uint64_t firstSeenSeq{0};
    uintptr_t targetKeyA{0};
    uintptr_t targetKeyB{0};
    uintptr_t nstTargetStateHash{0};
    std::vector<std::string> branchA;
    std::vector<std::string> branchB;
    std::vector<SourceTransition> branchATransitions;
    std::vector<SourceTransition> branchBTransitions;
};

using TreeTransitionIndex = std::unordered_map<uint64_t, const TreeTransitionEntry *>;

/** One filtered transition sample in chronological order for branch construction. */
struct NondetBranchSourceSample {
    uint64_t seq{0};
    uint64_t transitionSeq{0};
    std::string xml;
    uintptr_t sourceStateHash{0};
    uintptr_t targetKeyHash{0};
    uintptr_t targetStateHash{0};
    std::vector<int> resolvedNodeStableIds;
};

/** Counters explaining how many ring-buffer entries were skipped while collecting branch samples. */
struct NondetBranchInputStats {
    size_t logN{0};
    size_t orderedCount{0};
    size_t filteredByActivityOrPair{0};
    size_t filteredByTarget{0};
    size_t filteredBySnapshot{0};
    size_t filteredBySourceStateKey{0};
};

struct NondetBranchBuildResult {
    bool nstSeqFound{false};
    std::vector<NondetTreeTransitionBranchPair> branchPairs;
};

struct NondetBranchCollectionResult {
    NondetBranchInputStats inputStats;
    size_t orderedCount{0};
    bool nstSeqFound{false};
    std::vector<NondetTreeTransitionBranchPair> branchPairs;
};

using TransitionXmlLoader = std::function<bool(const ApeTransitionEntry &, std::string *)>;

/** Bounds picked from `TreeTransitionEntry` rows when refining an action predicate. */
struct NondetActionRefineTransitionContext {
    uintptr_t actionPredicateSourceStateHash{0};
    bool selectedTargetBoundsFound{false};
    Rect selectedTargetBounds{};
};

/** Build `transitionSeq` → `TreeTransitionEntry*` for one activity and action hash (only valid rows). */
void buildTreeTransitionIndex(const std::vector<TreeTransitionEntry> &transitionLog,
                              const std::string &sourceActivity,
                              uintptr_t actionHash,
                              TreeTransitionIndex *outIndex);

/** Look up target widget bounds by global transition sequence in the tree log. */
bool findTreeTransitionTargetBoundsBySeq(const std::vector<TreeTransitionEntry> &transitionLog,
                                         uint64_t transitionSeq,
                                         Rect *outBounds);

/**
 * Walks the circular `transitionLog` from `transitionLogWriteIndex`, filters by activity / keys / targets,
 * joins `treeBySeq` for resolved stable ids (and optional GUI trees), and outputs monotonic `seq` ordering.
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
    NondetBranchInputStats *outStats);

/** Build A/B branch pairs around the nondeterministic transition `nstTransitionSeq` within `ordered`. */
void buildNondetTreeTransitionBranchPairsFromOrderedSamples(
    const std::vector<NondetBranchSourceSample> &ordered,
    uint64_t nstTransitionSeq,
    std::vector<NondetTreeTransitionBranchPair> *outPairs);

/** Convenience wrapper: validates NST sequence presence, builds pairs, sorts them. */
NondetBranchBuildResult buildNondetBranchPairs(const std::vector<NondetBranchSourceSample> &ordered,
                                               uint64_t nstTransitionSeq);

/** Full pipeline: index tree log, collect ordered samples, build branch pairs for refinement. */
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
    uint64_t nstTransitionSeq);

/** Derives source state hash and first available target bounds from branch transition lists. */
NondetActionRefineTransitionContext buildNondetActionRefineTransitionContext(
    const std::vector<NondetTreeTransitionBranchPair::SourceTransition> &branchATransitions,
    const std::vector<NondetTreeTransitionBranchPair::SourceTransition> &branchBTransitions,
    const std::vector<TreeTransitionEntry> &treeTransitionLog);

/** Stable ordering for downstream deterministic refinement (sequence, hashes, keys). */
void sortNondetTreeTransitionBranchPairs(std::vector<NondetTreeTransitionBranchPair> *pairs);

} // namespace fastbotx

#endif // FASTBOTX_MODEL_TREE_TRANSITION_H_
