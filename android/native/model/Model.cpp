/*
 * This code is licensed under the Fastbot license. You may obtain a copy of this license in the LICENSE.txt file in the root directory of this source tree.
 */
/**
 * @authors Jianqiang Guo, Yuhui Su, Zhao Zhang
 */
#ifndef Model_CPP_
#define Model_CPP_

#include "Model.h"
#include "StateFactory.h"
#include "../desc/reuse/ReuseState.h"
#if DYNAMIC_STATE_ABSTRACTION_ENABLED
#include "../desc/gui_tree/GUITreeNode.h"
#include "../desc/reuse/ActivityNameAction.h"
#include "../desc/naming/NamerFactory.h"
#include "../desc/naming/NamerType.h"
#include "../desc/naming/ActionPatchNamer.h"
#endif
#ifndef NDEBUG
#include <cassert>
#include <thread>
#endif
#include <atomic>
#if defined(FASTBOT_HAS_PUGIXML) && FASTBOT_HAS_PUGIXML
#include "../desc/gui_tree/GUITreeFactory.h"
#include "../desc/gui_tree/GUITree.h"
#include "../desc/naming/NamingFactory.h"
#endif
#if DYNAMIC_STATE_ABSTRACTION_ENABLED
namespace {
using namespace fastbotx;

struct ApeNonDetPairStat {
    std::string sourceActivity;
    uintptr_t sourceKeyHash{0};
    bool hasSourceStateKey{false};
    naming::StateKey sourceStateKey = naming::StateKey::fromFallbackXmlStringHash("", 0);
    uintptr_t actionHash{0};
    std::unordered_set<uintptr_t> targetKeyHashes;
    size_t targetCount{0};
};

bool shouldLogApeDiagSample(const std::string &key, size_t everyN = 20) {
    static std::unordered_map<std::string, size_t> counters;
    size_t &c = counters[key];
    ++c;
    return c <= 3 || (everyN > 0 && (c % everyN) == 0);
}

void collectGUITreeNodesPreOrder(gui_tree::GUITreeNode *node, std::vector<gui_tree::GUITreeNode *> *out) {
    if (!node || !out) {
        return;
    }
    out->push_back(node);
    for (const auto &ch : node->getChildren()) {
        collectGUITreeNodesPreOrder(ch.get(), out);
    }
}

void applyApeDynamicActionHashesToReuseState(const StatePtr &state,
                                             const std::vector<gui_tree::GUITreeNode *> &nodesPreOrder,
                                             const naming::StateKey &apeKey) {
    if (!state) {
        return;
    }
    uint32_t fullMask = 0;
    for (auto t : naming::namerTypesUsed()) {
        fullMask |= (1u << static_cast<unsigned>(t));
    }
    naming::NamerPtr fullNamer = naming::NamerFactory::current().getByMask(fullMask);
    const uintptr_t activityH = fastStringHash(apeKey.activity());
    auto reuseState = std::dynamic_pointer_cast<ReuseState>(state);
    auto nodeForWidget = [&](const WidgetPtr &widget) -> gui_tree::GUITreeNode * {
        if (!reuseState || !widget || nodesPreOrder.empty()) {
            return nullptr;
        }
        const int stableId = reuseState->getStableElementIdForWidget(widget);
        if (stableId < 0) {
            return nullptr;
        }
        const size_t idx = static_cast<size_t>(stableId);
        if (idx >= nodesPreOrder.size()) {
            return nullptr;
        }
        return nodesPreOrder[idx];
    };
    auto stableTargetHashForWidget = [&](const WidgetPtr &widget) -> uintptr_t {
        if (!reuseState || !widget) {
            return 0x1;
        }
        const int stableId = reuseState->getStableElementIdForWidget(widget);
        if (stableId < 0) {
            return 0x1;
        }
        // Build deterministic per-element hash (stable across toolchains/runs).
        const uint64_t sid = static_cast<uint64_t>(static_cast<uint32_t>(stableId));
        const uint64_t mixed = sid * 11400714819323198485ull;
        return static_cast<uintptr_t>(mixed ^ (mixed >> 32));
    };
    const ActivityStateActionPtrVec &acts = state->getActions();
    const bool deriveActions = naming::useActionPatchDeriveActionsFromName();
    std::vector<uint8_t> keepMask;
    if (deriveActions) {
        keepMask.assign(acts.size(), 1);
    }
    size_t targetActions = 0;
    size_t noTargetActions = 0;
    size_t mappedXPath = 0;
    size_t mappedStableId = 0;
    size_t fallbackConst = 0;

    auto canonicalForCheck = [](ActionType t) -> ActionType {
        if (t == ActionType::SCROLL_BOTTOM_UP_N) {
            return ActionType::SCROLL_BOTTOM_UP;
        }
        return t;
    };

    auto kindBitForAction = [](ActionType t) -> uint32_t {
        switch (t) {
        case ActionType::CLICK:
            return naming::kDerivedKindClick;
        case ActionType::LONG_CLICK:
            return naming::kDerivedKindLongClick;
        case ActionType::SCROLL_TOP_DOWN:
        case ActionType::SCROLL_BOTTOM_UP:
        case ActionType::SCROLL_LEFT_RIGHT:
        case ActionType::SCROLL_RIGHT_LEFT:
        case ActionType::SCROLL_BOTTOM_UP_N:
            return naming::kDerivedKindScroll;
        default:
            return 0;
        }
    };

    for (size_t ai = 0; ai < acts.size(); ++ai) {
        const auto &a = acts[ai];
        auto ana = std::dynamic_pointer_cast<ActivityNameAction>(a);
        if (!ana) {
            continue;
        }
        naming::NamePtr nxp = nullptr;
        WidgetPtr w = ana->getTarget();
        if (!w) {
            noTargetActions++;
            ana->applyApeDynamicRlIdentity(activityH, 0x1);
            ana->setApeDynamicTargetFullPathHash(0);
            continue;
        }
        targetActions++;
        uintptr_t abstractTargetHash = stableTargetHashForWidget(w);
        uintptr_t fullTargetHash = 0;
        gui_tree::GUITreeNode *n = nodeForWidget(w);
        if (n) {
            nxp = n->getXPathName();
            if (nxp) {
                abstractTargetHash = fastStringHash(nxp->toXPath());
                mappedXPath++;
            } else if (abstractTargetHash != 0x1) {
                mappedStableId++;
            }
            if (fullNamer) {
                std::string fullKey = fullNamer->xpathKeyForNode(*n);
                if (fullKey.empty()) {
                    naming::NamePtr fullName = fullNamer->naming(*n);
                    if (fullName) {
                        fullKey = fullName->toXPath();
                    }
                }
                if (!fullKey.empty()) {
                    fullTargetHash = fastStringHash(fullKey);
                }
            }
        } else if (abstractTargetHash != 0x1) {
            mappedStableId++;
        }
        if (!nxp && abstractTargetHash == 0x1) {
            fallbackConst++;
        }
        ana->applyApeDynamicRlIdentity(activityH, abstractTargetHash);
        ana->setApeDynamicTargetFullPathHash(fullTargetHash);

        if (deriveActions && nxp && ai < keepMask.size()) {
            uint32_t allowedMask = 0;
            uint32_t knownKinds = 0;
            if (naming::decodeApeDerivedActionsFromName(nxp, &allowedMask, &knownKinds)) {
                const ActionType canonical = canonicalForCheck(ana->getActionType());
                const uint32_t kindBit = kindBitForAction(canonical);
                if (kindBit != 0 && (knownKinds & kindBit) != 0) {
                    const uint32_t actBit = 1u << static_cast<unsigned>(canonical);
                    if ((allowedMask & actBit) == 0) {
                        keepMask[ai] = 0;
                    }
                }
            }
        }
    }

    if (deriveActions && !keepMask.empty()) {
        state->filterActionsByKeepMask(keepMask);
    }
    if ((targetActions + noTargetActions) > 0) {
        BLOG("ape action hash mapping: activity=%s targetActions=%zu noTargetActions=%zu mappedXPath=%zu mappedStableId=%zu fallbackConst=%zu",
             apeKey.activity().c_str(), targetActions, noTargetActions, mappedXPath, mappedStableId,
             fallbackConst);
    }
}
} // namespace

#include "../desc/naming/NamingFactory.h"
#include "../desc/naming/NamerLattice.h"
#endif
#include "../Base.h"
#include "../utils.hpp"
#include "../thirdpart/json/json.hpp"
#include "../llm/HttpLlmClient.h"
#include <algorithm>
#include <ctime>
#include <inttypes.h>
#include <iostream>
#include <map>
#include <fstream>
#include <sstream>
#if DYNAMIC_STATE_ABSTRACTION_ENABLED
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace {
    /// Convert WidgetKeyMask to human-readable dimension list for logging (e.g. "Clazz|ResourceID|ContentDesc").
    std::string maskToDimensionString(fastbotx::WidgetKeyMask m) {
        std::ostringstream os;
        const char *sep = "";
        if (m & static_cast<fastbotx::WidgetKeyMask>(fastbotx::WidgetKeyAttr::Clazz)) { os << sep << "Clazz"; sep = "|"; }
        if (m & static_cast<fastbotx::WidgetKeyMask>(fastbotx::WidgetKeyAttr::ResourceID)) { os << sep << "ResourceID"; sep = "|"; }
        if (m & static_cast<fastbotx::WidgetKeyMask>(fastbotx::WidgetKeyAttr::OperateMask)) { os << sep << "OperateMask"; sep = "|"; }
        if (m & static_cast<fastbotx::WidgetKeyMask>(fastbotx::WidgetKeyAttr::ScrollType)) { os << sep << "ScrollType"; sep = "|"; }
        if (m & static_cast<fastbotx::WidgetKeyMask>(fastbotx::WidgetKeyAttr::Text)) { os << sep << "Text"; sep = "|"; }
        if (m & static_cast<fastbotx::WidgetKeyMask>(fastbotx::WidgetKeyAttr::ContentDesc)) { os << sep << "ContentDesc"; sep = "|"; }
        if (m & static_cast<fastbotx::WidgetKeyMask>(fastbotx::WidgetKeyAttr::Index)) { os << sep << "Index"; sep = "|"; }
        return os.str().empty() ? "(none)" : os.str();
    }

    // APE NamingFactory.resolveNonDeterminism: NDActionBlacklist when refine fails and
    // getOutStateTransitions(action).size() >= 3 (see ape/src/.../NamingFactory.java).
    constexpr int kApeNDActionBlacklistMinOutEdges = 3;

    /**
     * APE NamingFactory.sortRefinementResults / filterRefinementResult tie-break:
     * after primary keys (replay/score), prefer fewer induced partitions — proxy: smaller finenessGain;
     * then lexicographic namelets (expr, then compareNamer — Java NamerComparator + updated expr).
     */
    int compareNamingLexicographicForApeFilter(const fastbotx::naming::NamingPtr &a,
                                               const fastbotx::naming::NamingPtr &b) {
        using namespace fastbotx::naming;
        if (!a && !b) {
            return 0;
        }
        if (!a) {
            return 1;
        }
        if (!b) {
            return -1;
        }
        const auto &va = a->getNamelets();
        const auto &vb = b->getNamelets();
        if (va.size() != vb.size()) {
            return va.size() < vb.size() ? -1 : 1;
        }
        for (size_t i = 0; i < va.size(); ++i) {
            const auto &nla = va[i];
            const auto &nlb = vb[i];
            if (!nla && !nlb) {
                continue;
            }
            if (!nla) {
                return 1;
            }
            if (!nlb) {
                return -1;
            }
            int e = nla->getExprString().compare(nlb->getExprString());
            if (e != 0) {
                return e;
            }
            int n = compareNamer(nla->getNamer(), nlb->getNamer());
            if (n != 0) {
                return n;
            }
        }
        return 0;
    }
}
#endif

#if defined(FASTBOT_HAS_PUGIXML) && FASTBOT_HAS_PUGIXML && DYNAMIC_STATE_ABSTRACTION_ENABLED
namespace {
using ApeHashCache = std::unordered_map<uintptr_t, uintptr_t>;
bool apeStateHashFromXmlWithNaming(const std::string &activity, const std::string &xml,
                                    const fastbotx::naming::NamingPtr &naming,
                                    uintptr_t *outHash,
                                    uintptr_t cacheKey = 0,
                                    ApeHashCache *cache = nullptr) {
    using namespace fastbotx;
    if (!outHash || !naming || xml.empty()) {
        return false;
    }
    if (cache && cacheKey != 0) {
        auto itC = cache->find(cacheKey);
        if (itC != cache->end()) {
            *outHash = itC->second;
            return true;
        }
    }
    std::string pkg;
    std::string cls;
    naming::StateKey::splitActivityPackageClass(activity, &pkg, &cls);
    gui_tree::GUITreeBuildResult built = gui_tree::GUITreeFactory::buildFromXml(xml, pkg, cls);
    if (!built.tree || !built.dom) {
        return false;
    }
    if (!naming::NamingFactory::rebuildTree(naming, *built.tree, built.dom)) {
        return false;
    }
    *outHash = naming::StateKey::hashFromGUITree(*built.tree);
    if (cache && cacheKey != 0) {
        (*cache)[cacheKey] = *outHash;
    }
    return true;
}

bool apeStateHashFromXmlWithTwoNamings(const std::string &activity, const std::string &xml,
                                       const fastbotx::naming::NamingPtr &naming1, uintptr_t *outHash1,
                                       const fastbotx::naming::NamingPtr &naming2, uintptr_t *outHash2) {
    using namespace fastbotx;
    if (!naming1 || !naming2 || xml.empty()) {
        return false;
    }
    std::string pkg;
    std::string cls;
    naming::StateKey::splitActivityPackageClass(activity, &pkg, &cls);
    gui_tree::GUITreeBuildResult built = gui_tree::GUITreeFactory::buildFromXml(xml, pkg, cls);
    if (!built.tree || !built.dom) {
        return false;
    }
    if (!naming::NamingFactory::rebuildTree(naming1, *built.tree, built.dom)) {
        return false;
    }
    if (outHash1) {
        *outHash1 = naming::StateKey::hashFromGUITree(*built.tree);
    }
    if (!naming::NamingFactory::rebuildTree(naming2, *built.tree, built.dom)) {
        return false;
    }
    if (outHash2) {
        *outHash2 = naming::StateKey::hashFromGUITree(*built.tree);
    }
    return true;
}

/**
 * Coarsen / blacklist / prune paths historically used two independent apeStateHashFromXmlWithNaming calls.
 * Prefer one XML parse + two rebuilds; if that fails (e.g. second rebuildTree fails), fall back to two full
 * parses so partial success and triggerSource==0 mapping fallback (prevKeyHash -> storedKey) match legacy.
 */
void apeStateKeyPairFromXmlCoarsenPath(const std::string &activity, const std::string &xml,
                                       const fastbotx::naming::NamingPtr &namingCur, uintptr_t *tgtKeyHash,
                                       const fastbotx::naming::NamingPtr &namingPrev, uintptr_t *prevKeyHash) {
    if (!tgtKeyHash || !prevKeyHash || xml.empty() || !namingCur || !namingPrev) {
        return;
    }
    if (apeStateHashFromXmlWithTwoNamings(activity, xml, namingCur, tgtKeyHash, namingPrev, prevKeyHash)) {
        return;
    }
    *tgtKeyHash = 0;
    *prevKeyHash = 0;
    (void)apeStateHashFromXmlWithNaming(activity, xml, namingCur, tgtKeyHash);
    (void)apeStateHashFromXmlWithNaming(activity, xml, namingPrev, prevKeyHash);
}

bool evalApeSourcePartitionPredicateImpl(
    const std::unordered_map<uintptr_t, std::string> &xmlByHash, const std::string &predActivityKeyCanonical,
    const std::string &activity, const naming::NamingPtr &naming,
    const std::vector<std::vector<uintptr_t>> &partitions) {
    if (!naming || partitions.size() < 2) {
        return true;
    }
    if (predActivityKeyCanonical != naming::StateKey::canonicalActivityString(activity)) {
        return true;
    }
    ApeHashCache hashCache;
    std::unordered_set<uintptr_t> seen;
    std::vector<uintptr_t> computed;
    for (const auto &part : partitions) {
        computed.clear();
        computed.reserve(part.size());
        for (uintptr_t sh : part) {
            auto it = xmlByHash.find(sh);
            if (it == xmlByHash.end() || it->second.empty()) {
                continue;
            }
            uintptr_t h = 0;
            if (!apeStateHashFromXmlWithNaming(activity, it->second, naming, &h, sh, &hashCache)) {
                return false;
            }
            computed.push_back(h);
        }
        for (uintptr_t h : computed) {
            if (seen.count(h) != 0) {
                return false;
            }
        }
        for (uintptr_t h : computed) {
            seen.insert(h);
        }
    }
    return true;
}

bool evalApeSourcePartitionPredicateImplTwoNamings(
    const std::unordered_map<uintptr_t, std::string> &xmlByHash, const std::string &predActivityKeyCanonical,
    const std::string &activity, const naming::NamingPtr &namingPrev, const naming::NamingPtr &namingCur,
    const std::vector<std::vector<uintptr_t>> &partitions,
    const std::unordered_set<uintptr_t> &affectedStateHashes) {
    if (!namingPrev || !namingCur || partitions.size() < 2) {
        return true;
    }
    if (predActivityKeyCanonical != naming::StateKey::canonicalActivityString(activity)) {
        return true;
    }
    ApeHashCache prevCache, curCache;
    std::unordered_set<uintptr_t> seen;
    std::vector<uintptr_t> computed;
    for (const auto &part : partitions) {
        computed.clear();
        computed.reserve(part.size());
        for (uintptr_t sh : part) {
            const auto it = xmlByHash.find(sh);
            if (it == xmlByHash.end() || it->second.empty()) {
                continue;
            }
            const bool usePrev = affectedStateHashes.count(sh) != 0;
            const naming::NamingPtr &namingToUse = usePrev ? namingPrev : namingCur;
            ApeHashCache &cache = usePrev ? prevCache : curCache;
            uintptr_t h = 0;
            if (!apeStateHashFromXmlWithNaming(activity, it->second, namingToUse, &h, sh, &cache)) {
                return false;
            }
            computed.push_back(h);
        }
        for (uintptr_t h : computed) {
            if (seen.count(h) != 0) {
                return false;
            }
        }
        for (uintptr_t h : computed) {
            seen.insert(h);
        }
    }
    return true;
}

bool evalApeActionPartitionPredicateImpl(
    const std::unordered_map<uintptr_t, std::string> &xmlByHash, const std::string &predActivityKeyCanonical,
    const std::string &activity, const fastbotx::naming::NamingPtr &naming,
    const std::vector<std::vector<std::pair<uintptr_t, size_t>>> &partitions) {
    if (!naming || partitions.size() < 2) {
        return true;
    }
    if (predActivityKeyCanonical != naming::StateKey::canonicalActivityString(activity)) {
        return true;
    }
    std::string pkg;
    std::string cls;
    naming::StateKey::splitActivityPackageClass(activity, &pkg, &cls);
    std::unordered_set<std::string> actions;
    for (const auto &part : partitions) {
        std::unordered_set<std::string> temp;
        for (const auto &entry : part) {
            const uintptr_t sh = entry.first;
            const size_t preIdx = entry.second;
            auto itXml = xmlByHash.find(sh);
            if (itXml == xmlByHash.end() || itXml->second.empty()) {
                continue;
            }
            gui_tree::GUITreeBuildResult built = gui_tree::GUITreeFactory::buildFromXml(itXml->second, pkg, cls);
            if (!built.tree || !built.dom) {
                return false;
            }
            if (!naming::NamingFactory::rebuildTree(naming, *built.tree, built.dom)) {
                return false;
            }
            std::vector<gui_tree::GUITreeNode *> po;
            collectGUITreeNodesPreOrder(built.tree->getRootNode(), &po);
            if (preIdx >= po.size() || !po[preIdx]) {
                return false;
            }
            const naming::NamePtr nm = po[preIdx]->getXPathName();
            if (!nm) {
                return false;
            }
            const std::string x = nm->toXPath();
            const auto itT = temp.insert(x);
            if (itT.second) {
                const auto itA = actions.insert(x);
                if (!itA.second) {
                    return false;
                }
            }
        }
    }
    return true;
}

bool evalApeActionPartitionPredicateImplTwoNamings(
    const std::unordered_map<uintptr_t, std::string> &xmlByHash, const std::string &predActivityKeyCanonical,
    const std::string &activity, const fastbotx::naming::NamingPtr &namingPrev,
    const fastbotx::naming::NamingPtr &namingCur,
    const std::vector<std::vector<std::pair<uintptr_t, size_t>>> &partitions,
    const std::unordered_set<uintptr_t> &affectedStateHashes) {
    if (!namingPrev || !namingCur || partitions.size() < 2) {
        return true;
    }
    if (predActivityKeyCanonical != naming::StateKey::canonicalActivityString(activity)) {
        return true;
    }
    std::string pkg;
    std::string cls;
    naming::StateKey::splitActivityPackageClass(activity, &pkg, &cls);
    std::unordered_set<std::string> actions;
    for (const auto &part : partitions) {
        std::unordered_set<std::string> temp;
        for (const auto &entry : part) {
            const uintptr_t sh = entry.first;
            const size_t preIdx = entry.second;
            auto itXml = xmlByHash.find(sh);
            if (itXml == xmlByHash.end() || itXml->second.empty()) {
                continue;
            }
            const fastbotx::naming::NamingPtr &namingToUse =
                affectedStateHashes.count(sh) != 0 ? namingPrev : namingCur;
            gui_tree::GUITreeBuildResult built = gui_tree::GUITreeFactory::buildFromXml(itXml->second, pkg, cls);
            if (!built.tree || !built.dom) {
                return false;
            }
            if (!naming::NamingFactory::rebuildTree(namingToUse, *built.tree, built.dom)) {
                return false;
            }
            std::vector<gui_tree::GUITreeNode *> po;
            collectGUITreeNodesPreOrder(built.tree->getRootNode(), &po);
            if (preIdx >= po.size() || !po[preIdx]) {
                return false;
            }
            const naming::NamePtr nm = po[preIdx]->getXPathName();
            if (!nm) {
                return false;
            }
            const std::string x = nm->toXPath();
            const auto itT = temp.insert(x);
            if (itT.second) {
                const auto itA = actions.insert(x);
                if (!itA.second) {
                    return false;
                }
            }
        }
    }
    return true;
}

bool evalApeStatesFewerThanPredicateImpl(
    const std::unordered_map<uintptr_t, std::string> &xmlByHash, const std::string &predActivityKeyCanonical,
    const std::string &activity, const fastbotx::naming::NamingPtr &naming,
    const std::vector<uintptr_t> &stateHashes, int threshold) {
    if (!naming || threshold < 1) {
        return true;
    }
    if (predActivityKeyCanonical != naming::StateKey::canonicalActivityString(activity)) {
        return true;
    }
    ApeHashCache hashCache;
    std::unordered_set<uintptr_t> distinct;
    for (uintptr_t sh : stateHashes) {
        auto itXml = xmlByHash.find(sh);
        if (itXml == xmlByHash.end() || itXml->second.empty()) {
            continue;
        }
        uintptr_t h = 0;
        if (!apeStateHashFromXmlWithNaming(activity, itXml->second, naming, &h, sh, &hashCache)) {
            return false;
        }
        distinct.insert(h);
        if (static_cast<int>(distinct.size()) > threshold) {
            return false;
        }
    }
    return true;
}

bool evalApeStatesFewerThanPredicateImplTwoNamings(
    const std::unordered_map<uintptr_t, std::string> &xmlByHash, const std::string &predActivityKeyCanonical,
    const std::string &activity, const fastbotx::naming::NamingPtr &namingPrev,
    const fastbotx::naming::NamingPtr &namingCur, const std::vector<uintptr_t> &stateHashes,
    int threshold, const std::unordered_set<uintptr_t> &affectedStateHashes) {
    if (!namingPrev || !namingCur || threshold < 1) {
        return true;
    }
    if (predActivityKeyCanonical != naming::StateKey::canonicalActivityString(activity)) {
        return true;
    }
    ApeHashCache prevCache, curCache;
    std::unordered_set<uintptr_t> distinct;
    for (uintptr_t sh : stateHashes) {
        auto itXml = xmlByHash.find(sh);
        if (itXml == xmlByHash.end() || itXml->second.empty()) {
            continue;
        }
        const bool usePrev = affectedStateHashes.count(sh) != 0;
        const fastbotx::naming::NamingPtr &namingToUse = usePrev ? namingPrev : namingCur;
        ApeHashCache &cache = usePrev ? prevCache : curCache;
        uintptr_t h = 0;
        if (!apeStateHashFromXmlWithNaming(activity, itXml->second, namingToUse, &h, sh, &cache)) {
            return false;
        }
        distinct.insert(h);
        if (static_cast<int>(distinct.size()) > threshold) {
            return false;
        }
    }
    return true;
}

} // namespace
#endif

namespace fastbotx {

    WidgetKeyMask Model::getActivityKeyMask(const std::string &activity) const {
        auto it = _activityKeyMask.find(activity);
        if (it != _activityKeyMask.end()) {
            return it->second;
        }
        return DefaultWidgetKeyMask;
    }

    std::shared_ptr<LlmClient> Model::getLlmClient() const {
        return _llmTaskAgent ? _llmTaskAgent->getLlmClient() : nullptr;
    }

    void Model::setActivityKeyMask(const std::string &activity, WidgetKeyMask mask) {
        _activityKeyMask[activity] = mask;
    }

    /**
     * @brief Log state information with each widget and action on a separate line
     * 
     * This helper function formats state information for debugging/logging purposes.
     * It prints the state hash, all widgets, and all actions in a readable format.
     * Long strings (>3000 chars) are split across multiple log lines.
     * 
     * @param state The state to log (nullptr is handled gracefully)
     */
    inline void logStatePerLine(const StatePtr &state) {
        if (state == nullptr) {
            BDLOGE("State is null, cannot log state information");
            return;
        }
        
        // Print state header with hash code
        BDLOG("{state: %lu", static_cast<unsigned long>(state->hash()));
        
        // Print each widget on a separate line for better readability; skip empty (e.g. toXPath returns "" when details cleared)
        BDLOG("widgets:");
        const auto &widgets = state->getWidgets();
        for (const auto &widget : widgets) {
            std::string widgetStr = widget->toString();
            if (widgetStr.empty()) continue;
            // If widget string is too long, split it across multiple log lines
            if (widgetStr.length() > 3000) {
                logLongStringInfo("   " + widgetStr);
            } else {
                BDLOG("   %s", widgetStr.c_str());
            }
        }
        
        // Print each action on a separate line for better readability
        BDLOG("action:");
        const auto &actions = state->getActions();
        for (const auto &action : actions) {
            std::string actionStr = action->toString();
            // If action string is too long, split it across multiple log lines
            if (actionStr.length() > 3000) {
                logLongStringInfo("   " + actionStr);
            } else {
                BDLOG("   %s", actionStr.c_str());
            }
        }
        
        BDLOG("}");
    }

    /**
     * @brief Factory method to create a new Model instance
     * 
     * Uses new + shared_ptr instead of make_shared because the constructor is protected
     * and make_shared cannot access protected constructors from outside the class.
     * 
     * @return Shared pointer to a new Model instance
     */
    std::shared_ptr<Model> Model::create() {
        return std::shared_ptr<Model>(new Model());
    }

    /**
     * @brief Constructor for Model class
     * 
     * Initializes the model with:
     * - A new Graph instance for state management
     * - Preference singleton instance
     * - Network action parameters set to default values
     */
    Model::Model() {
#ifndef FASTBOT_VERSION
    // Use build timestamp if available, otherwise use compile-time date/time
    #ifdef FASTBOT_BUILD_TIMESTAMP
        #define FASTBOT_VERSION FASTBOT_BUILD_TIMESTAMP
    #else
        // Fallback to compiler's __DATE__ and __TIME__ macros
        #define FASTBOT_VERSION __DATE__ " " __TIME__
    #endif
#endif
        BLOG("----Fastbot native code verison: 4122012, build version: " FASTBOT_VERSION "----\n");
        this->_graph = std::make_shared<Graph>();
        this->_preference = Preference::inst();
        this->_netActionParam.netActionTaskid = 0;

        // Initialize LLMTaskAgent with HTTP LLM client if LLM is enabled in config.
        LlmRuntimeConfig llmCfg;
        if (this->_preference) {
            llmCfg = this->_preference->getLlmRuntimeConfig();
        }
        std::shared_ptr<LlmClient> client = nullptr;
        if (llmCfg.enabled) {
            client = std::make_shared<HttpLlmClient>(llmCfg);
            BLOG("LLMTaskAgent: HTTP LLM client initialized with model %s", llmCfg.model.c_str());
        } else {
            BLOG("LLMTaskAgent: LLM is disabled in config");
        }
        this->_llmTaskAgent = std::make_shared<LLMTaskAgent>(this->_preference, client);
        this->_apeStateNamingManager = std::make_shared<naming::StateNamingManager>(nullptr);
#if DYNAMIC_STATE_ABSTRACTION_ENABLED
        this->_apeTransitionLog.resize(MaxTransitionLogSize);
        // TransitionLog upper bound implies a natural upper bound for distinct (srcKey,action) pairs.
        // Reserve once to reduce rehash/malloc on the hot path.
        this->_apePairAgg.reserve(MaxTransitionLogSize);
        this->_apeEvidencePools.reserve(MaxTransitionLogSize);
        // Fixed-size clock for bounded LRU-like eviction.
        constexpr size_t kEvidencePoolClockSize = MaxTransitionLogSize*4;
        this->_apeEvidencePoolClock.resize(kEvidencePoolClockSize);
        this->_apeEvidencePoolClockWriteIndex = 0;
        this->_apeEvidencePoolClockEvictIndex = 0;
        this->_apeEvidenceEpoch = 0;
        BLOG("state abstraction: enabled (check interval=%d, batch every %d steps)",
             (int)RefinementCheckInterval, (int)RefinementCheckInterval);
#endif
    }


    /**
     * @brief General entry point for getting next operation step according to RL model
     * 
     * This is the main entry point that accepts XML content as a string.
     * It parses the XML string into an Element object and delegates to the
     * ElementPtr-based version of getOperate().
     * 
     * @param descContent XML content of the current page as a string
     * @param activity Activity name string
     * @param deviceID Device ID string (default: empty string uses default device)
     * @return Next operation step in JSON format, or empty string if parsing fails
     */
    std::string Model::getOperate(const std::string &descContent, const std::string &activity,
                                  const std::string &deviceID) {
        // Parse XML string into Element object using tinyxml2
        ElementPtr elem = Element::createFromXml(descContent);
        if (nullptr == elem) {
            return "";
        }
        // Delegate to ElementPtr-based version
        return this->getOperate(elem, activity, deviceID);
    }

    /**
     * @brief Create and add an agent to the model for a specific device
     * 
     * Creates a new agent using the AgentFactory, adds it to the device-agent map,
     * and registers it as a listener to the graph for state change notifications.
     * 
     * @param deviceIDString Device ID string (empty string uses default device ID)
     * @param agentType The type of algorithm/agent to create
     * @param deviceType The type of device (default: Normal)
     * @return Shared pointer to the newly created agent
     */
    AbstractAgentPtr Model::addAgent(const std::string &deviceIDString, AlgorithmType agentType,
                                     DeviceType deviceType) {
        // Create agent using factory pattern
        auto agent = AgentFactory::create(agentType, shared_from_this(), deviceType);
        
        // Use default device ID if empty string provided
        const std::string &deviceID = deviceIDString.empty() ? ModelConstants::DefaultDeviceID
                                                             : deviceIDString;
        
        // Add the device-agent pair to the map
        this->_deviceIDAgentMap.emplace(deviceID, agent);
        
        // Register agent as a listener to graph updates
        // This allows the agent to be notified when new states are added
        this->_graph->addListener(agent);
        
        return agent;
    }

    AbstractAgentPtr Model::addAgent(const std::string &deviceIDString, AlgorithmType agentType,
                                     bool useCodeCoverage, DeviceType deviceType) {
        (void) useCodeCoverage;
        return addAgent(deviceIDString, agentType, deviceType);
    }

    /**
     * @brief Get the agent for a specific device ID
     * 
     * @param deviceID Device ID string (empty string uses default device ID)
     * @return Shared pointer to the agent, or nullptr if not found
     */
    AbstractAgentPtr Model::getAgent(const std::string &deviceID) const {
        const std::string &d = deviceID.empty() ? ModelConstants::DefaultDeviceID : deviceID;
        auto iter = this->_deviceIDAgentMap.find(d);
        if (iter != this->_deviceIDAgentMap.end()) {
            return iter->second;
        }
        return nullptr;
    }


    /**
     * @brief Get next operation step from Element object, returning JSON string
     * 
     * This method wraps the core getOperateOpt() method and converts the result
     * to a JSON string format.
     * 
     * @param element XML Element object of the current page
     * @param activity Activity name string
     * @param deviceID Device ID string (default: empty string uses default device)
     * @return Next operation step in JSON format
     */
    std::string Model::getOperate(const ElementPtr &element, const std::string &activity,
                                  const std::string &deviceID) {
        OperatePtr operate = getOperateOpt(element, activity, deviceID);
        std::string operateString = operate->toString();
        return operateString;
    }


    /**
     * @brief Get custom action from preference if one exists for this page
     * 
     * Checks if the user has specified a custom action for this activity/page
     * in the preference settings. Returns nullptr if no custom action is defined.
     * 
     * @param activity Activity name string
     * @param element XML Element object of the current page
     * @return Custom action if exists, nullptr otherwise
     */
    /**
     * @brief Get or create an activity string pointer
     * 
     * This method optimizes memory usage by reusing existing activity string pointers
     * from the graph's visited activities set. If the activity already exists,
     * returns the cached shared pointer. Otherwise, creates a new one.
     * 
     * Performance optimization:
     * - Reuses existing string pointers to avoid duplicate string storage
     * - Uses hash-based set lookup for O(log n) complexity
     * 
     * @param activity The activity name string
     * @return Shared pointer to the activity string (cached or newly created)
     * 
     * @note The returned pointer may be from the cache or newly created.
     *       Newly created pointers will be added to the graph's visited activities
     *       when the state is added via createAndAddState().
     */
    stringPtr Model::getOrCreateActivityPtr(const std::string &activity) {
        // Get the set of visited activities (returns by value, but set is typically small)
        const stringPtrSet& activityStringPtrSet = this->_graph->getVisitedActivities();
        
        // Create temporary shared_ptr for lookup
        // Note: This creates a temporary object for comparison only
        // If not found, we'll return this pointer; if found, we'll return the cached one
        stringPtr tempActivityPtr = std::make_shared<std::string>(activity);
        
        // Try to find existing activity pointer in the set
        auto founded = activityStringPtrSet.find(tempActivityPtr);
        
        if (founded == activityStringPtrSet.end()) {
            // This is a new activity, return the newly created pointer
            return tempActivityPtr;
        } else {
            // Activity already exists, return the cached pointer to avoid duplication
            return *founded;
        }
    }

    /**
     * @brief Get or create an agent for the given device ID
     * 
     * This method retrieves an agent for the specified device ID. If no agent exists
     * for the device ID, returns the default agent. If no agents exist at all,
     * creates a default reuse agent.
     * 
     * Performance optimization:
     * - Uses find() instead of [] operator to avoid creating unnecessary map entries
     * - Falls back to default device ID if device ID is not found
     * 
     * @param deviceID The device ID string (empty string uses default device ID)
     * @return Shared pointer to the agent for the device
     * 
     * @note If the device ID is not found, returns the default agent instead of
     *       creating a new one. This ensures all devices have an agent to use.
     */
        AbstractAgentPtr Model::getOrCreateAgent(const std::string &deviceID) {
        // Create a default agent if map is empty
        if (this->_deviceIDAgentMap.empty()) {
            BLOG("%s", "use DoubleSarsaAgent as the default agent");
            this->addAgent(ModelConstants::DefaultDeviceID, AlgorithmType::DoubleSarsa);
        }
        
        // Use find() instead of [] to avoid creating unnecessary map entries
        // Performance: O(log n) lookup without side effects
        auto agentIterator = this->_deviceIDAgentMap.find(deviceID);
        
        if (agentIterator == this->_deviceIDAgentMap.end()) {
            // Device ID not found, return the default agent
            // Use find() again to avoid [] operator side effects
            auto defaultIterator = this->_deviceIDAgentMap.find(ModelConstants::DefaultDeviceID);
            if (defaultIterator != this->_deviceIDAgentMap.end()) {
                return defaultIterator->second;
            }
            // Should not reach here if addAgent worked correctly, but handle gracefully
            return nullptr;
        } else {
            // Found the agent for this device ID
            return agentIterator->second;
        }
    }

    /**
     * @brief Create a new state from element and add it to the graph
     * 
     * Creates a state object based on the agent's algorithm type, then adds it
     * to the graph. The graph will deduplicate if a similar state already exists.
     * Marks the state as visited with the current graph timestamp.
     * 
     * @param element XML Element object of the current page (must not be nullptr)
     * @param agent The agent to use for state creation (determines state type)
     * @param activityPtr Shared pointer to activity name string
     * @return Shared pointer to the created/existing state, or nullptr if element is null
     */
    StatePtr Model::buildStateOnly(const ElementPtr &element, const AbstractAgentPtr &agent,
                                   const stringPtr &activityPtr) {
        if (nullptr == element) {
            return nullptr;
        }
        std::string activityStr = activityPtr ? *activityPtr : "";
        WidgetKeyMask mask = getActivityKeyMask(activityStr);
        StatePtr state = StateFactory::createState(agent->getAlgorithmType(), activityPtr, element, mask);
        return state;
    }

    StatePtr Model::createAndAddState(const ElementPtr &element, const AbstractAgentPtr &agent,
                                      const stringPtr &activityPtr) {
        StatePtr state = buildStateOnly(element, agent, activityPtr);
        if (!state) return nullptr;
        state = this->_graph->addState(state);
        state->visit(this->_graph->getTimestamp());
        return state;
    }

    void Model::addCurrentPageAsPrecondition(const ElementPtr &element, const std::string &activity,
                                             const std::string &deviceID) {
        if (!element) {
            BLOG("[GUIDE] Model::addCurrentPageAsPrecondition skip: null element");
            return;
        }
        AbstractAgentPtr agent = getOrCreateAgent(deviceID);
        if (!agent) {
            BLOG("[GUIDE] Model::addCurrentPageAsPrecondition skip: no agent");
            return;
        }
        stringPtr activityPtr = getOrCreateActivityPtr(activity);
        StatePtr state = buildStateOnly(element, agent, activityPtr);
        if (!state) {
            BLOG("[GUIDE] Model::addCurrentPageAsPrecondition skip: buildStateOnly failed activity=%s", activity.c_str());
            return;
        }
        BLOG("[GUIDE] Model::addCurrentPageAsPrecondition activity=%s stateHash=%lu actions=%zu",
             activity.c_str(), static_cast<unsigned long>(state->hash()), state->getActions().size());
        agent->addCurrentPageAsPrecondition(state);
    }

    /**
     * @brief Select an action based on state, agent, and custom preferences
     * 
     * This method implements the action selection logic:
     * 1. Uses custom action from preference if available
     * 2. Checks for blocked state and returns RESTART if needed
     * 3. Otherwise, asks the agent to resolve a new action
     * 4. Updates agent strategy and marks action as visited if it's a model action
     * 
     * @param state The current state (may be modified)
     * @param agent The agent to use for action selection (may be modified)
     * @param customAction Custom action from preference, if any
     * @param actionCost Output parameter: time cost for action generation in seconds
     * @return Selected action, or nullptr if selection failed
     */
    ActionPtr Model::selectAction(StatePtr &state, AbstractAgentPtr &agent, ActionPtr customAction, double &actionCost) {
        double startGeneratingActionTimestamp = currentStamp();
        actionCost = 0.0;
        ActionPtr action = customAction; // Use custom action if provided

        const bool shouldSkipActionsFromModel =
            this->_preference ? this->_preference->skipAllActionsFromModel() : false;
        const char *activityLabel = "?";
        unsigned long long stateHashU = 0;
        size_t stateActionCount = 0;
        if (state) {
            stateHashU = static_cast<unsigned long long>(state->hash());
            stateActionCount = state->getActions().size();
            if (state->getActivityString() && state->getActivityString().get()) {
                activityLabel = state->getActivityString()->c_str();
            }
        }
        const int agentAlgo = agent ? static_cast<int>(agent->getAlgorithmType()) : -1;
        const int blockTimes = agent ? agent->getCurrentStateBlockTimes() : 0;
        BDLOG("selectAction: enter activity=%s stateHash=%llu graphStateId=%s stateActions=%zu "
              "agentAlgo=%d customXPath=%s listenSkipModel=%s blockTimes=%d",
              activityLabel,
              stateHashU,
              state ? state->getId().c_str() : "-",
              stateActionCount,
              agentAlgo,
              customAction ? "yes" : "no",
              shouldSkipActionsFromModel ? "yes" : "no",
              blockTimes);

        // Log state information for debugging
        logStatePerLine(state);

        if (shouldSkipActionsFromModel) {
            LOGI("listen mode skip get action from model");
        }

        // If no custom action specified and not in listen mode, get action from agent
        if (nullptr == customAction && !shouldSkipActionsFromModel) {
            // Check if we're in a blocked state and should restart
            if (-1 != BLOCK_STATE_TIME_RESTART &&
                -1 != Preference::inst()->getForceMaxBlockStateTimes() &&
                agent->getCurrentStateBlockTimes() > BLOCK_STATE_TIME_RESTART) {
                // Force restart action when stuck in blocked state
                action = Action::RESTART;
                BLOG("Ran into a block state %s blockTimes=%d restartThreshold=%d",
                     state ? state->getId().c_str() : "",
                     agent->getCurrentStateBlockTimes(),
                     BLOCK_STATE_TIME_RESTART);
            } else {
                // Ask agent to resolve a new action (this is the main RL model entry point)
                BDLOG("selectAction: calling agent->resolveNewAction() algo=%d", agentAlgo);
                auto resolvedAction = agent->resolveNewAction();
                action = std::dynamic_pointer_cast<Action>(resolvedAction);

                BDLOG("selectAction: resolveNewAction raw ptr=%s",
                      action ? action->toString().c_str() : "(null)");

                // Update agent's strategy based on the new action
                agent->updateStrategy();

                if (nullptr == action) {
                    BDLOGE("get null action!!!!");
                    return nullptr; // Handle null action gracefully
                }
            }

            // Calculate action generation time cost
            double endGeneratingActionTimestamp = currentStamp();
            actionCost = endGeneratingActionTimestamp - startGeneratingActionTimestamp;
        } else if (customAction) {
            BDLOG("selectAction: branch customXPath — skipping agent resolve/updateStrategy");
        } else if (shouldSkipActionsFromModel) {
            BDLOG("selectAction: branch listenSkipModel — agent not consulted (action may stay null)");
        }

        const char *how;
        if (customAction) {
            how = "custom_xpath";
        } else if (shouldSkipActionsFromModel) {
            how = "listen_skip_model";
        } else if (action && action->getActionType() == ActionType::RESTART) {
            how = "block_restart";
        } else {
            how = "agent";
        }
        BDLOG("selectAction: exit how=%s action=%s isModelAct=%d costMs=%.3f",
              how,
              action ? action->toString().c_str() : "(null)",
              (action && action->isModelAct()) ? 1 : 0,
              actionCost);

        return action;
    }

    /**
     * @brief Convert an action to an operate object and apply patches
     * 
     * Converts an Action object to a DeviceOperateWrapper (OperatePtr) that can be
     * executed. If the action requires a target widget, extracts widget information.
     * Applies preference patches and optionally clears state details for memory optimization.
     * 
     * @param action The action to convert (nullptr returns NOP operation)
     * @param state The current state (used for detail clearing optimization)
     * @return OperatePtr The operation object ready for execution
     */
    OperatePtr Model::convertActionToOperate(ActionPtr action, StatePtr state) {
        if (action == nullptr) {
            // Return no-operation if action is null
            return DeviceOperateWrapper::OperateNop;
        }

        BLOG("selected action %s", action->toString().c_str());
        
        // Convert action to operation object
        OperatePtr opt = action->toOperate();

        // If action requires a target widget, extract widget information
        if (action->requireTarget()) {
            if (auto stateAction = std::dynamic_pointer_cast<fastbotx::ActivityStateAction>(action)) {
                std::shared_ptr<Widget> widget = stateAction->getTarget();
                if (widget) {
                    // Serialize widget to JSON and attach to operation
                    std::string widget_str = widget->toJson();
                    opt->widget = widget_str;
                    BLOG("stateAction Widget: %s", widget_str.c_str());
                }
            }
        }

        // Apply preference patches to the operation (e.g., custom modifications)
        if (this->_preference) {
            this->_preference->patchOperate(opt);
        }

        // Memory optimization: clear state details after use if enabled
        // This reduces memory usage for states that are no longer needed in detail
        if (DROP_DETAIL_AFTER_SATE && state && !state->hasNoDetail()) {
            state->clearDetails();
        }

        return opt;
    }

    /**
     * @brief Core method for getting next operation step and updating RL model
     * 
     * This is the main orchestration method that:
     * 1. Gets custom action from preference if available
     * 2. Gets or creates activity pointer (memory optimization)
     * 3. Gets or creates agent for the device
     * 4. Creates and adds state to the graph
     * 5. Selects an action using the agent or custom action
     * 6. Converts action to operation object
     * 7. Logs performance metrics
     * 
     * @param element XML Element object of the current page
     * @param activity Activity name string
     * @param deviceID Device ID string (default: empty string uses default device)
     * @return DeviceOperateWrapper object containing the next operation to perform
     * 
     * @note This method updates the RL model by adding states and actions to the graph
     */
    OperatePtr Model::getOperateOpt(const ElementPtr &element, const std::string &activity,
                                    const std::string &deviceID) {
        // Record method start time for performance tracking
        double methodStartTimestamp = currentStamp();
        
        // Step 0: Match LLM task on raw tree (before resolvePage) so checkpoint matches unmodified UI.
        LlmTaskConfigPtr preMatchedLlmTask = nullptr;
        if (this->_preference && element) {
            preMatchedLlmTask = this->_preference->matchLlmTask(activity, element);
        }
        
        // Step 1: Resolve page (black widgets, tree pruning, valid texts) before using element.
        if (this->_preference && element) {
            this->_preference->resolvePage(activity, element);
        }
        // Step 2: Custom action from max.xpath.actions (if any) for this activity and page.
        ActionPtr customAction = (this->_preference && element)
            ? this->_preference->getCustomActionFromXpath(activity, element)
            : nullptr;
        
        // Step 3: Get or create activity pointer (reuses existing pointers for memory efficiency)
        stringPtr activityPtr = getOrCreateActivityPtr(activity);
        
        // Step 4: Get or create agent for this device (creates default if needed)
        AbstractAgentPtr agent = getOrCreateAgent(deviceID);
        
        // Step 5: Build state, notify agent of transition (moveForward) before adding to graph, then add state
        // moveForward(currentState) must run before addState so agent still has previous _newState/_newAction
        // for (fromState, actionTaken, nextState) → updateKnowledge / AIG edges (see FIND_NAVIGATE_PATH_CODE_REVIEW §7).
        double buildStateStartTimestamp = currentStamp();
        StatePtr built = buildStateOnly(element, agent, activityPtr);
        StatePtr state = built;
        if (state) {
            // Cache XML string within this frame to avoid repeated Element::toXML() allocations.
            std::string xmlCache;
            auto getXml = [&]() -> const std::string & {
                if (xmlCache.empty()) {
                    xmlCache = element ? element->toXMLCached() : std::string();
                }
                return xmlCache;
            };
#if defined(FASTBOT_HAS_PUGIXML) && FASTBOT_HAS_PUGIXML
            naming::StateKey apeKey = naming::StateKey::fromParts(
                naming::StateKey::canonicalActivityString(activity), nullptr, {});
            bool haveApeKey = false;
#if DYNAMIC_STATE_ABSTRACTION_ENABLED
            const bool wantApeRlIdentity = !_preference || !_preference->useStaticReuseAbstraction();
            const bool wantApeGraphDedup =
                _preference && _preference->useApeGraphDedupByStateKey();
            const bool wantApeStateKey = wantApeRlIdentity || wantApeGraphDedup;
            ApeStateKeyBuildFailReason buildFailReason = ApeStateKeyBuildFailReason::None;
            if (wantApeStateKey) {
                haveApeKey = buildApeStateKeyFromElementTree(
                    element, activity, &apeKey, &buildFailReason, wantApeRlIdentity ? built : StatePtr(), &xmlCache);
            } else {
                haveApeKey = false;
            }
            if (wantApeRlIdentity) {
                if (haveApeKey) {
                    built->applyDynamicAbstractionIdentityHash(apeKey.hash());
                } else {
                    _ape_correctness_counters.statekey_fallback_used++;
                    const uintptr_t xmlH = fastStringHash(getXml());
                    apeKey = naming::StateKey::fromFallbackXmlStringHash(activity, xmlH);
                    built->applyDynamicAbstractionIdentityHash(apeKey.hash());
                    applyApeDynamicActionHashesToReuseState(built, {}, apeKey);
                    haveApeKey = true;

                    const uint64_t n = _ape_correctness_counters.statekey_fallback_used;
                    if (n <= 3 || (n % 100) == 0) {
                        BLOG("ape statekey: fallback activity=%s reason=%d xmlHash=%zu",
                             activity.c_str(), static_cast<int>(buildFailReason), static_cast<size_t>(xmlH));
                    }
                }
            }
#else
            ApeStateKeyBuildFailReason buildFailReason = ApeStateKeyBuildFailReason::None;
            haveApeKey = buildApeStateKeyFromElementTree(element, activity, &apeKey, &buildFailReason);
#endif
            if (haveApeKey && _preference && _preference->useApeGraphDedupByStateKey()) {
                const uintptr_t kh = apeKey.hash();
                bool deduped = false;
                auto &bucket = _ape_graph_state_by_key[kh];
                for (const auto &entry : bucket) {
                    if (entry.key == apeKey) {
                        _ape_correctness_counters.graph_dedup_exact_hit++;
                        agent->moveForward(entry.state);
                        _graph->recordStateVisit(entry.state, built);
                        state = entry.state;
                        deduped = true;
                        break;
                    }
                }
                if (!deduped) {
                    if (!bucket.empty()) {
                        _ape_correctness_counters.graph_dedup_hash_collision++;
                        const uint64_t n = _ape_correctness_counters.graph_dedup_hash_collision;
                        if (n <= 3 || (n % 100) == 0) {
                            BLOG("ape graph dedup: hash collision activity=%s keyHash=%zu bucket=%zu",
                                 activity.c_str(), static_cast<size_t>(kh), bucket.size());
                        }
                    }
                    agent->moveForward(built);
                    state = _graph->addState(built);
                    bucket.push_back(ApeGraphStateKeyDedupEntry{apeKey, state});
                } else {
                    _ape_correctness_counters.graph_dedup_hash_hit++;
                }
            } else {
                agent->moveForward(built);
                state = _graph->addState(built);
            }
            state->visit(this->_graph->getTimestamp());
            if (haveApeKey) {
                recordApeStateKey(state, apeKey);
                logApeStateKeySnapshot(activity, state, apeKey, _graph);
            }
#if DYNAMIC_STATE_ABSTRACTION_ENABLED
            if (element && _preference && !_preference->useStaticReuseAbstraction() &&
                _preference->useApeNamingCandidateTransitionReplay()) {
                const uintptr_t sh = state->hash();
                const std::string actKeyCanonical = naming::StateKey::canonicalActivityString(activity);
                _apeStateXmlByStateHash[sh] = getXml();
                apeMiniHistoryTouchState(actKeyCanonical, sh);
                constexpr size_t kMaxApeXmlCache = 384;
                if (_apeStateXmlByStateHash.size() > kMaxApeXmlCache) {
                    // Q8 (local rebuild): protect mini-history referenced XML from cache eviction.
                    std::unordered_set<uintptr_t> protectedHashes;
                    protectedHashes.reserve(128);
                    for (const auto &kv : _apeMiniHistoryByActivity) {
                        const ApeMiniHistory &h = kv.second;
                        for (uintptr_t hsh : h.stateHashes) {
                            if (hsh != 0) {
                                protectedHashes.insert(hsh);
                            }
                        }
                        for (const auto &t : h.transitions) {
                            if (!t.valid) {
                                continue;
                            }
                            if (t.sourceStateHash != 0) {
                                protectedHashes.insert(t.sourceStateHash);
                            }
                            if (t.targetStateHash != 0) {
                                protectedHashes.insert(t.targetStateHash);
                            }
                        }
                    }
                    while (_apeStateXmlByStateHash.size() > kMaxApeXmlCache) {
                        auto itEvict = _apeStateXmlByStateHash.begin();
                        constexpr size_t kMaxProbe = 32;
                        size_t probe = 0;
                        while (itEvict != _apeStateXmlByStateHash.end() &&
                               protectedHashes.count(itEvict->first) != 0 &&
                               probe < kMaxProbe) {
                            ++itEvict;
                            ++probe;
                        }
                        if (itEvict == _apeStateXmlByStateHash.end()) {
                            itEvict = _apeStateXmlByStateHash.begin();
                        }
                        _apeStateXmlByStateHash.erase(itEvict);
                    }
                }
            }
#endif
#elif DYNAMIC_STATE_ABSTRACTION_ENABLED
            // No pugixml in this build: XPath/GUITree unavailable — use XML-digest StateKey only (still APE-style id).
            naming::StateKey apeKey = naming::StateKey::fromParts(
                naming::StateKey::canonicalActivityString(activity), nullptr, {});
            bool haveApeKey = false;
            const bool wantApeRlIdentity = !_preference || !_preference->useStaticReuseAbstraction();
            std::vector<gui_tree::GUITreeNode *> guiPreOrder;
            if (wantApeRlIdentity) {
                _ape_correctness_counters.statekey_fallback_used++;
                const uintptr_t xmlH = fastStringHash(getXml());
                apeKey = naming::StateKey::fromFallbackXmlStringHash(activity, xmlH);
                built->applyDynamicAbstractionIdentityHash(apeKey.hash());
                applyApeDynamicActionHashesToReuseState(built, guiPreOrder, apeKey);
                haveApeKey = true;

                const uint64_t n = _ape_correctness_counters.statekey_fallback_used;
                if (n <= 3 || (n % 100) == 0) {
                    BLOG("ape statekey fallback: activity=%s xmlHash=%zu",
                         activity.c_str(), static_cast<size_t>(xmlH));
                }
            }
            if (haveApeKey && _preference && _preference->useApeGraphDedupByStateKey()) {
                const uintptr_t kh = apeKey.hash();
                bool deduped = false;
                auto &bucket = _ape_graph_state_by_key[kh];
                for (const auto &entry : bucket) {
                    if (entry.key == apeKey) {
                        _ape_correctness_counters.graph_dedup_exact_hit++;
                        agent->moveForward(entry.state);
                        _graph->recordStateVisit(entry.state, built);
                        state = entry.state;
                        deduped = true;
                        break;
                    }
                }
                if (!deduped) {
                    if (!bucket.empty()) {
                        _ape_correctness_counters.graph_dedup_hash_collision++;
                        const uint64_t n = _ape_correctness_counters.graph_dedup_hash_collision;
                        if (n <= 3 || (n % 100) == 0) {
                            BLOG("ape graph dedup: hash collision activity=%s keyHash=%zu bucket=%zu",
                                 activity.c_str(), static_cast<size_t>(kh), bucket.size());
                        }
                    }
                    agent->moveForward(built);
                    state = _graph->addState(built);
                    bucket.push_back(ApeGraphStateKeyDedupEntry{apeKey, state});
                } else {
                    _ape_correctness_counters.graph_dedup_hash_hit++;
                }
            } else {
                agent->moveForward(built);
                state = _graph->addState(built);
            }
            state->visit(this->_graph->getTimestamp());
            if (haveApeKey) {
                recordApeStateKey(state, apeKey);
                logApeStateKeySnapshot(activity, state, apeKey, _graph);
            }
#else
            agent->moveForward(state);
            state = this->_graph->addState(state);
            state->visit(this->_graph->getTimestamp());
#endif
        }
        // Count the previous step's model action only after we observe the next state (moveForward above).
        // Covers RL, xpath custom, and LLM-selected actions; keeps resolveAt's visitedCount % N aligned.
        if (state && agent) {
            const std::string devKey = deviceID.empty() ? ModelConstants::DefaultDeviceID : deviceID;
            auto pit = _pendingModelActionVisitByDevice.find(devKey);
            if (pit != _pendingModelActionVisitByDevice.end()) {
                ActionPtr pend = pit->second;
                if (pend && pend->isModelAct()) {
                    pend->visit(this->_graph->getTimestamp());
                }
                _pendingModelActionVisitByDevice.erase(pit);
            }
        }
        double buildStateEndTimestamp = currentStamp();
        bool fromLlm = (_llmTaskAgent && _llmTaskAgent->inSession());
#if DYNAMIC_STATE_ABSTRACTION_ENABLED
        if (!fromLlm) {
            recordTransition(agent, state);
        }
#endif
        // LLMDroid (after addState,
        // visit, recordTransition). APE/RL identity for `state` is already fixed above — processState must
        // not call applyDynamicAbstractionIdentityHash, mutate StateKey, or alter Graph dedup keys.
        // Gated by max.llm.llmdroid at Model layer so default runs skip dynamic_pointer_cast + virtual call.
        {
            const PreferencePtr pref = _preference ? _preference : Preference::inst();
            if (pref && pref->isLlmdroidEnabled() && state && agent) {
                if (ReuseStatePtr reuseState = std::dynamic_pointer_cast<ReuseState>(state)) {
                    agent->processState(reuseState);
                }
            }
        }
        // Step 5b: Removed — image now stays in Java (setLastScreenshotForLlm + doLlmHttpPostFromPrompt).
        // Native no longer returns NOP when screenshotBytes is empty; Java always has the image when needed.
        // Step 6: Optionally delegate to LLMTaskAgent before RL (pass pre-matched task from raw tree).
        ActionPtr llmAction = nullptr;
        if (this->_llmTaskAgent) {
            llmAction = this->_llmTaskAgent->selectNextAction(element, activity, deviceID, preMatchedLlmTask);
        }

        // Step 7: Select action (either LLM, custom, restart, or from agent)
        double actionCost = 0.0;
        ActionPtr action;
        if (llmAction) {
            // When LLMTaskAgent returns an action, we bypass RL for this step.
            action = llmAction;
        } else {
            action = selectAction(state, agent, customAction, actionCost);
        }
        
        // Handle null action gracefully
        if (nullptr == action) {
            return DeviceOperateWrapper::OperateNop;
        }

        // Resolve merged widgets: when multiple concrete nodes share the same abstract widget,
        // set action target to the next concrete node (visitCount % total) so each selection hits a different node (e.g. 特价→首页→秒送→新品).
        if (state && action && action->requireTarget()) {
            if (auto stateAction = std::dynamic_pointer_cast<ActivityStateAction>(action)) {
                state->resolveAt(stateAction, _graph->getTimestamp());
            }
        }

        // Step 8: Convert action to operation object and apply patches
        OperatePtr opt = convertActionToOperate(action, state);
        if (llmAction) {
            opt->allowFuzzing = false;
        }
        const PreferencePtr prefLlmdroid = _preference ? _preference : Preference::inst();
        if (prefLlmdroid && prefLlmdroid->isLlmdroidEnabled() && state && action) {
            if (ReuseStatePtr rs = std::dynamic_pointer_cast<ReuseState>(state)) {
                rs->_actionToPerform = action;
            }
        }
        // Optional: agent-provided LLM-generated input text (e.g. LLMExplorerAgent content-aware input)
        if (agent) {
            std::string agentInputText = agent->getInputTextForAction(state, action);
            if (!agentInputText.empty()) {
                opt->setText(agentInputText);
            }
        }
        if (auto asa = std::dynamic_pointer_cast<ActivityStateAction>(action)) {
            if (!asa->getInputText().empty()) {
                opt->setText(asa->getInputText());
            }
        }

        // Record end time and log performance metrics (currentStamp returns ms, keep ms for log)
        double methodEndTimestamp = currentStamp();
        double buildStateCostMs = buildStateEndTimestamp - buildStateStartTimestamp;
        double actionCostMs = actionCost;
        double totalCostMs = methodEndTimestamp - methodStartTimestamp;
#if DYNAMIC_STATE_ABSTRACTION_ENABLED
        if (state && state->usesDynamicAbstractionIdentityHash()) {
            BLOG("build state cost: %.3fms action cost: %.3fms total cost: %.3fms dims=[APE]",
                 buildStateCostMs,
                 actionCostMs,
                 totalCostMs);
        } else {
            BLOG("build state cost: %.3fms action cost: %.3fms total cost: %.3fms dims=[%s]",
                 buildStateCostMs,
                 actionCostMs,
                 totalCostMs,
                 maskToDimensionString(getActivityKeyMask(activity)).c_str());
        }
#else
        BLOG("build state cost: %.3fms action cost: %.3fms total cost: %.3fms",
             buildStateCostMs,
             actionCostMs,
             totalCostMs);
#endif
#if DYNAMIC_STATE_ABSTRACTION_ENABLED
        if (!fromLlm) {
            _stepCountSinceLastCheck++;
            if (_stepCountSinceLastCheck >= RefinementCheckInterval) {
                runRefinementAndCoarseningIfScheduled();
                _stepCountSinceLastCheck = 0;
            }
        }
#endif
        if (action && action->isModelAct()) {
            const std::string devKey = deviceID.empty() ? ModelConstants::DefaultDeviceID : deviceID;
            _pendingModelActionVisitByDevice[devKey] = action;
        }
        return opt;
    }

#if DYNAMIC_STATE_ABSTRACTION_ENABLED
#ifndef NDEBUG
    void Model::assertApeSingleThreaded() const {
        const std::thread::id cur = std::this_thread::get_id();
        if (!_apeOwnerThreadSet) {
            _apeOwnerThread = cur;
            _apeOwnerThreadSet = true;
            return;
        }
        if (_apeOwnerThread != cur) {
            // Dynamic abstraction assumes Model is driven from a single thread.
            assert(false);
        }
    }
#endif
    void Model::recordTransition(const AbstractAgentPtr &agent, const StatePtr &targetState) {
        if (!agent || !targetState) return;
        StatePtr srcState = agent->getCurrentState();
        ActivityStateActionPtr act = agent->getCurrentAction();
        if (!srcState || !act || !act->isModelAct() || !act->requireTarget()) return;
        recordApeTransitionForAbstraction(srcState, targetState, act);
    }

    void Model::recordApeTransitionForAbstraction(const StatePtr &src, const StatePtr &tgt,
                                                  const ActivityStateActionPtr &act) {
        if (!src || !tgt || !act || _apeTransitionLog.empty()) {
            return;
        }
#ifndef NDEBUG
        assertApeSingleThreaded();
#endif
        const int minTargets = (_preference ? _preference->getApeNamingMinNonDetTargets()
                                            : MinNonDeterminismCount);
        naming::StateKey srcKey = naming::StateKey::fromFallbackXmlStringHash("", 0);
        naming::StateKey tgtKey = naming::StateKey::fromFallbackXmlStringHash("", 0);
        if (!tryGetApeStateKey(src->hash(), &srcKey) || !tryGetApeStateKey(tgt->hash(), &tgtKey)) {
            return;
        }
        ApeTransitionEntry e;
        e.sourceKeyHash = srcKey.hash();
        e.hasSourceStateKey = true;
        e.sourceStateKey = srcKey;
        e.actionHash = act->hash();
        e.targetKeyHash = tgtKey.hash();
        e.hasTargetStateKey = true;
        e.targetStateKey = tgtKey;
        e.sourceStateHash = src->hash();
        e.targetStateHash = tgt->hash();
        e.actionType = act->getActionType();
        e.hasTargetBounds = false;
        if (auto tw = act->getTarget()) {
            if (auto b = tw->getBounds()) {
                e.hasTargetBounds = true;
                e.targetBounds = *b;
            }
        }
        e.hasTargetFullPath = false;
        e.targetFullPathHash = 0;
        if (auto ana = std::dynamic_pointer_cast<ActivityNameAction>(act)) {
            const uintptr_t h = ana->getApeDynamicTargetFullPathHash();
            if (h != 0) {
                e.hasTargetFullPath = true;
                e.targetFullPathHash = h;
            }
        }
        {
            auto actPtr = src->getActivityString();
            e.sourceActivity = naming::StateKey::canonicalActivityString(
                (actPtr && actPtr.get()) ? *actPtr : "");
        }
        e.valid = true;
        BDLOG("ape naming: transition srcKey=%lu act=%lu tgtKey=%lu activity=%s",
              (unsigned long)e.sourceKeyHash, (unsigned long)e.actionHash, (unsigned long)e.targetKeyHash,
              e.sourceActivity.c_str());
        const ApePairKey pairKey{e.sourceKeyHash, e.actionHash};
        size_t prevPairTargetCount = 0;
        auto itPairBefore = _apePairAgg.find(pairKey);
        if (itPairBefore != _apePairAgg.end()) {
            prevPairTargetCount = itPairBefore->second.targetCounts.size();
        }
        ApeTransitionEntry &aSlot = _apeTransitionLog[_apeTransitionLogWriteIndex];
        if (aSlot.valid) {
            apePairAggRemove(aSlot);
        }
        aSlot = std::move(e);
        apePairAggAdd(aSlot);
        apeEvidencePoolAdd(pairKey, aSlot);
        apeMiniHistoryRecordTransition(aSlot.sourceActivity, aSlot);
        size_t nowPairTargetCount = 0;
        std::unordered_set<uintptr_t> pairTargetHashes;
        auto itPairAfter = _apePairAgg.find(pairKey);
        if (itPairAfter != _apePairAgg.end()) {
            nowPairTargetCount = itPairAfter->second.targetCounts.size();
            itPairAfter->second.targetCounts.forEach([&](uintptr_t h, int /*count*/) {
                pairTargetHashes.insert(h);
            });
        }
        _apeTransitionLogWriteIndex = (_apeTransitionLogWriteIndex + 1) % _apeTransitionLog.size();

        // APE event-layer alignment: on NEW_ACTION_TARGET-like growth of a non-det pair,
        // attempt immediate pair-scoped refinement and then rollback check.
        const bool pairFanoutIncreased = nowPairTargetCount > prevPairTargetCount;
        const bool becameNonDet =
            prevPairTargetCount < static_cast<size_t>(minTargets) &&
            nowPairTargetCount >= static_cast<size_t>(minTargets);
        if (!pairFanoutIncreased && !becameNonDet) {
            return;
        }
        auto countNonDetPairsByActivity = [&](const std::string &canonicalActivity) -> int {
            int c = 0;
            for (const auto &kv : _apePairAgg) {
                if (kv.second.sourceActivity == canonicalActivity &&
                    kv.second.targetCounts.size() >= static_cast<size_t>(minTargets)) {
                    ++c;
                }
            }
            return c;
        };
        const int nonDetPairs = countNonDetPairsByActivity(aSlot.sourceActivity);
        if (nonDetPairs <= 0 || nowPairTargetCount < static_cast<size_t>(minTargets)) {
            return;
        }
        ApeRefinePair rp;
        rp.sourceKeyHash = pairKey.sourceKeyHash;
        rp.hasSourceStateKey = aSlot.hasSourceStateKey;
        rp.sourceStateKey = aSlot.sourceStateKey;
        rp.actionHash = pairKey.actionHash;
        rp.targetKeyHashes = std::move(pairTargetHashes);
        rp.targetCount = nowPairTargetCount;
        BDLOG("ape naming: event refine-attempt activity=%s srcKey=%lu act=%lu targets=%zu nonDetPairs=%d",
              aSlot.sourceActivity.c_str(), (unsigned long)rp.sourceKeyHash,
              (unsigned long)rp.actionHash, rp.targetCount, nonDetPairs);
        const std::string actKey = aSlot.sourceActivity;
        if (refineActivityApeNaming(aSlot.sourceActivity, &rp, nonDetPairs)) {
            _apeEventRefineSuccessCount++;
            BDLOG("ape naming: event refine ok activity=%s srcKey=%lu act=%lu targets=%zu",
                  aSlot.sourceActivity.c_str(), (unsigned long)rp.sourceKeyHash,
                  (unsigned long)rp.actionHash, nowPairTargetCount);
            if (coarsenActivityApeNamingIfNeeded(aSlot.sourceActivity)) {
                _apeEventCoarsenRollbackCount++;
            }
            notifyAgentsOfApeNamingChange();
        } else if (rp.actionHash != 0 && nowPairTargetCount >= static_cast<size_t>(kApeNDActionBlacklistMinOutEdges)) {
            auto &blk = _apeRefineActionBlacklist[actKey];
            const bool inserted = blk.insert(rp.actionHash).second;
            apeCapApeNamingCoarsenAndRefineBlacklists();
            BLOG("ape naming: NDActionBlacklist add (APE: out>=%d after failed resolve) activity=%s act=%lu targets=%zu",
                 kApeNDActionBlacklistMinOutEdges, aSlot.sourceActivity.c_str(),
                 (unsigned long)rp.actionHash, nowPairTargetCount);
            if (shouldLogApeDiagSample(std::string("nd_blacklist_event_add#") + actKey, 25)) {
                BLOG("ape naming: diag NDActionBlacklist[event] activity=%s act=%lu inserted=%d size=%zu targets=%zu nonDetPairs=%d",
                     aSlot.sourceActivity.c_str(), (unsigned long)rp.actionHash, inserted ? 1 : 0,
                     blk.size(), nowPairTargetCount, nonDetPairs);
            }
        } else {
            BDLOG("ape naming: event refine failed activity=%s srcKey=%lu act=%lu targets=%zu "
                  "(no ND blacklist: actZero=%d targetsBelowNdMin=%d)",
                  aSlot.sourceActivity.c_str(), (unsigned long)rp.sourceKeyHash,
                  (unsigned long)rp.actionHash, nowPairTargetCount,
                  (rp.actionHash == 0) ? 1 : 0,
                  (nowPairTargetCount < static_cast<size_t>(kApeNDActionBlacklistMinOutEdges)) ? 1 : 0);
        }
    }

    void Model::apePairAggRemove(const ApeTransitionEntry &e) {
        if (!e.valid || e.sourceKeyHash == e.targetKeyHash) {
            return;
        }
        ApePairKey pk{e.sourceKeyHash, e.actionHash};
        auto it = _apePairAgg.find(pk);
        if (it == _apePairAgg.end()) {
            return;
        }
        if (!it->second.targetCounts.decrement(e.targetKeyHash)) {
            return;
        }
        if (it->second.targetCounts.empty()) {
            _apePairAgg.erase(it);
        }
    }

    void Model::apePairAggAdd(const ApeTransitionEntry &e) {
        if (!e.valid || e.sourceKeyHash == e.targetKeyHash) {
            return;
        }
        ApePairKey pk{e.sourceKeyHash, e.actionHash};
        auto &slot = _apePairAgg[pk];
        slot.targetCounts.increment(e.targetKeyHash);
        slot.sourceActivity = e.sourceActivity;
        if (e.hasSourceStateKey) {
            slot.hasSourceStateKey = true;
            slot.sourceStateKey = e.sourceStateKey;
        }
    }

    void Model::apeEvidencePoolAdd(const ApePairKey &pairKey, const ApeTransitionEntry &e) {
        if (!e.valid || pairKey.sourceKeyHash == 0 || pairKey.actionHash == 0) {
            return;
        }
        _ape_correctness_counters.evidence_pool_sample_add++;

        ApeEvidenceSample s;
        s.sourceStateHash = e.sourceStateHash;
        s.targetStateHash = e.targetStateHash;
        s.targetKeyHash = e.targetKeyHash;
        s.actionType = e.actionType;
        s.hasTargetBounds = e.hasTargetBounds;
        s.targetBounds = e.targetBounds;
        s.hasTargetFullPath = e.hasTargetFullPath;
        s.targetFullPathHash = e.targetFullPathHash;

        uint64_t epoch = ++_apeEvidenceEpoch;
        if (epoch == 0) {
            epoch = ++_apeEvidenceEpoch;
        }

        auto it = _apeEvidencePools.find(pairKey);
        if (it == _apeEvidencePools.end()) {
            _ape_correctness_counters.evidence_pool_new_pair++;
            it = _apeEvidencePools.emplace(pairKey, ApeEvidencePool{}).first;
        }
        it->second.push(std::move(s), epoch);

        if (!_apeEvidencePoolClock.empty()) {
            ApeEvidencePoolClockEntry &ce = _apeEvidencePoolClock[_apeEvidencePoolClockWriteIndex];
            ce.key = pairKey;
            ce.epoch = epoch;
            _apeEvidencePoolClockWriteIndex =
                (_apeEvidencePoolClockWriteIndex + 1) % _apeEvidencePoolClock.size();
        }

        apeEvidencePoolClockEvict();
    }

    void Model::apeEvidencePoolClockEvict() {
        constexpr size_t kMaxPools = MaxTransitionLogSize;
        if (_apeEvidencePoolClock.empty()) {
            return;
        }
        if (_apeEvidencePools.size() <= kMaxPools) {
            return;
        }

        size_t scanned = 0;
        const size_t clockSize = _apeEvidencePoolClock.size();
        while (_apeEvidencePools.size() > kMaxPools && scanned < clockSize) {
            ApeEvidencePoolClockEntry &victim = _apeEvidencePoolClock[_apeEvidencePoolClockEvictIndex];
            _apeEvidencePoolClockEvictIndex =
                (_apeEvidencePoolClockEvictIndex + 1) % _apeEvidencePoolClock.size();
            ++scanned;

            if (victim.epoch == 0) {
                continue;
            }

            auto itV = _apeEvidencePools.find(victim.key);
            if (itV == _apeEvidencePools.end()) {
                victim.epoch = 0;
                continue;
            }

            if (itV->second.lastTouchEpoch != victim.epoch) {
                continue;
            }

            _ape_correctness_counters.evidence_pool_evict++;
            _apeEvidencePools.erase(itV);
            victim.epoch = 0;
        }
    }

    void Model::apeClearTransitionAggregationForActivity(const std::string &actKeyCanonical) {
        for (auto &slot : _apeTransitionLog) {
            if (slot.valid && slot.sourceActivity == actKeyCanonical) {
                _apeEvidencePools.erase(ApePairKey{slot.sourceKeyHash, slot.actionHash});
                apePairAggRemove(slot);
                slot.valid = false;
            }
        }
        for (auto it = _apePairAgg.begin(); it != _apePairAgg.end();) {
            if (it->second.sourceActivity == actKeyCanonical) {
                it = _apePairAgg.erase(it);
            } else {
                ++it;
            }
        }
    }

    void Model::apeMiniHistoryTouchState(const std::string &activityKeyCanonical, uintptr_t stateHash) {
        if (activityKeyCanonical.empty() || stateHash == 0) {
            return;
        }
        _apeMiniHistoryByActivity[activityKeyCanonical].touchState(stateHash);
    }

    void Model::apeMiniHistoryRecordTransition(const std::string &activityKeyCanonical,
                                               const ApeTransitionEntry &e) {
        if (activityKeyCanonical.empty() || !e.valid) {
            return;
        }
        ApeMiniHistory &h = _apeMiniHistoryByActivity[activityKeyCanonical];
        h.touchState(e.sourceStateHash);
        h.touchState(e.targetStateHash);
        ApeMiniHistoryTransition t;
        t.sourceStateHash = e.sourceStateHash;
        t.targetStateHash = e.targetStateHash;
        t.actionType = e.actionType;
        t.hasTargetBounds = e.hasTargetBounds;
        t.targetBounds = e.targetBounds;
        t.hasTargetFullPath = e.hasTargetFullPath;
        t.targetFullPathHash = e.targetFullPathHash;
        t.valid = true;
        h.pushTransition(t);
    }

    void Model::apeInsertTransitionEntryNoRefine(const ApeTransitionEntry &e) {
        if (!e.valid || _apeTransitionLog.empty()) {
            return;
        }
        ApeTransitionEntry &slot = _apeTransitionLog[_apeTransitionLogWriteIndex];
        if (slot.valid) {
            apePairAggRemove(slot);
        }
        slot = e;
        apePairAggAdd(slot);
        apeEvidencePoolAdd(ApePairKey{slot.sourceKeyHash, slot.actionHash}, slot);
        _apeTransitionLogWriteIndex = (_apeTransitionLogWriteIndex + 1) % _apeTransitionLog.size();
    }

    bool Model::apeLocalRebuildFromHistoryIfNeeded(const std::string &activityKeyCanonical,
                                                   const char *reason) {
        if (!_graph || activityKeyCanonical.empty()) {
            return false;
        }
        ApeActivityRebuildStats &st = _apeRebuildStatsByActivity[activityKeyCanonical];
        const uint64_t now = static_cast<uint64_t>(_graph->getTimestamp());
        constexpr uint64_t kMinInterval = 500;
        if (st.lastRebuildTimestamp != 0 && now > st.lastRebuildTimestamp &&
            (now - st.lastRebuildTimestamp) < kMinInterval) {
            return false;
        }
        const double ratio =
            st.actionBlacklistChecks > 0
                ? (static_cast<double>(st.actionBlacklistHits) /
                   static_cast<double>(st.actionBlacklistChecks))
                : 0.0;
        constexpr int kMinConsecutiveRollbacks = 2;
        constexpr int kMinActionChecks = 20;
        constexpr double kMinActionRatio = 0.7;
        const bool triggerByRollback = st.consecutiveRollbacks >= kMinConsecutiveRollbacks;
        const bool triggerByActionBlk =
            (st.actionBlacklistChecks >= kMinActionChecks && ratio >= kMinActionRatio);
        if (!triggerByRollback && !triggerByActionBlk) {
            return false;
        }
        if (!apeLocalRebuildFromHistory(activityKeyCanonical)) {
            return false;
        }
        st.lastRebuildTimestamp = now;
        st.consecutiveRollbacks = 0;
        st.actionBlacklistChecks = 0;
        st.actionBlacklistHits = 0;
        BLOG("ape naming: local rebuild activity=%s reason=%s", activityKeyCanonical.c_str(),
             reason ? reason : "(unknown)");
        return true;
    }

    bool Model::apeLocalRebuildFromHistory(const std::string &activityKeyCanonical) {
#if !defined(FASTBOT_HAS_PUGIXML) || !FASTBOT_HAS_PUGIXML
        (void)activityKeyCanonical;
        return false;
#else
        if (!_graph || activityKeyCanonical.empty()) {
            return false;
        }
        if (!_preference || !_preference->useApeNamingCandidateTransitionReplay()) {
            return false;
        }
        const bool wantApeRlIdentity = !_preference->useStaticReuseAbstraction();
        if (!wantApeRlIdentity) {
            return false;
        }
        auto itH = _apeMiniHistoryByActivity.find(activityKeyCanonical);
        if (itH == _apeMiniHistoryByActivity.end()) {
            return false;
        }
        const ApeMiniHistory &hist = itH->second;

        // Snapshot minimal XML history before clearing states.
        std::unordered_map<uintptr_t, std::string> xmlByOldStateHash;
        xmlByOldStateHash.reserve(64);
        auto snapXml = [&](uintptr_t sh) {
            if (sh == 0) {
                return;
            }
            if (xmlByOldStateHash.find(sh) != xmlByOldStateHash.end()) {
                return;
            }
            auto itX = _apeStateXmlByStateHash.find(sh);
            if (itX == _apeStateXmlByStateHash.end() || itX->second.empty()) {
                return;
            }
            xmlByOldStateHash.emplace(sh, itX->second);
        };
        for (uintptr_t sh : hist.stateHashes) {
            snapXml(sh);
        }
        for (const auto &t : hist.transitions) {
            if (!t.valid) {
                continue;
            }
            snapXml(t.sourceStateHash);
            snapXml(t.targetStateHash);
        }
        if (xmlByOldStateHash.empty()) {
            return false;
        }

        std::vector<std::string> xmlList;
        xmlList.reserve(32);
        std::unordered_set<uintptr_t> seenXmlHash;
        seenXmlHash.reserve(64);
        for (const auto &kv : xmlByOldStateHash) {
            const std::string &xml = kv.second;
            const uintptr_t xh = fastStringHash(xml);
            if (seenXmlHash.insert(xh).second) {
                xmlList.push_back(xml);
                if (xmlList.size() >= 24) {
                    break;
                }
            }
        }
        if (xmlList.empty()) {
            return false;
        }

        // Remove all states for this activity.
        std::unordered_set<uintptr_t> toRemove;
        toRemove.reserve(128);
        for (const auto &kv : _ape_state_keys_by_hash) {
            const uintptr_t sh = kv.first;
            for (const auto &k : kv.second) {
                if (k.activity() == activityKeyCanonical) {
                    toRemove.insert(sh);
                    break;
                }
            }
        }
        for (const auto &s : _graph->getStates()) {
            if (!s || toRemove.count(s->hash()) == 0) {
                continue;
            }
            for (const auto &a : s->getActions()) {
                if (a) {
                    _apeInvalidatedReuseActionHashes.insert(a->hash());
                }
            }
        }
        _graph->removeStatesByHash(toRemove);
        for (uintptr_t sh : toRemove) {
            _ape_state_keys_by_hash.erase(sh);
            _apeGuiTreeNamingBlacklist.erase(sh);
            _apeStateXmlByStateHash.erase(sh);
        }
        apeClearTransitionAggregationForActivity(activityKeyCanonical);

        // Reset history to track rebuilt state hashes.
        _apeMiniHistoryByActivity[activityKeyCanonical] = ApeMiniHistory{};
        ApeMiniHistory &newHist = _apeMiniHistoryByActivity[activityKeyCanonical];

        AbstractAgentPtr agent = getOrCreateAgent(ModelConstants::DefaultDeviceID);
        if (!agent) {
            return false;
        }
        stringPtr activityPtr = getOrCreateActivityPtr(activityKeyCanonical);

        std::unordered_map<uintptr_t, StatePtr> stateByXmlHash;
        stateByXmlHash.reserve(xmlList.size() * 2);
        for (const std::string &xml : xmlList) {
            ElementPtr elem = Element::createFromXml(xml);
            if (!elem) {
                continue;
            }
            StatePtr built = buildStateOnly(elem, agent, activityPtr);
            if (!built) {
                continue;
            }
            naming::StateKey apeKey = naming::StateKey::fromParts(activityKeyCanonical, nullptr, {});
            const bool haveKey =
                buildApeStateKeyFromElementTree(elem, activityKeyCanonical, &apeKey, nullptr, built);
            if (haveKey) {
                built->applyDynamicAbstractionIdentityHash(apeKey.hash());
            }
            StatePtr canonical = _graph->addState(built);
            if (haveKey) {
                recordApeStateKey(canonical, apeKey);
            }
            _apeStateXmlByStateHash[canonical->hash()] = xml;
            newHist.touchState(canonical->hash());
            stateByXmlHash[fastStringHash(xml)] = canonical;
        }

        for (const auto &t : hist.transitions) {
            if (!t.valid) {
                continue;
            }
            auto itSx = xmlByOldStateHash.find(t.sourceStateHash);
            auto itTx = xmlByOldStateHash.find(t.targetStateHash);
            if (itSx == xmlByOldStateHash.end() || itTx == xmlByOldStateHash.end()) {
                continue;
            }
            StatePtr srcState = nullptr;
            StatePtr tgtState = nullptr;
            {
                const uintptr_t hs = fastStringHash(itSx->second);
                auto it = stateByXmlHash.find(hs);
                if (it != stateByXmlHash.end()) {
                    srcState = it->second;
                }
            }
            {
                const uintptr_t ht = fastStringHash(itTx->second);
                auto it = stateByXmlHash.find(ht);
                if (it != stateByXmlHash.end()) {
                    tgtState = it->second;
                }
            }
            if (!srcState || !tgtState) {
                continue;
            }

            ActivityStateActionPtr matchedAction;
            for (const auto &a : srcState->getActions()) {
                auto asa = std::dynamic_pointer_cast<ActivityStateAction>(a);
                if (!asa) {
                    continue;
                }
                if (asa->getActionType() != t.actionType) {
                    continue;
                }
                if (t.hasTargetBounds) {
                    auto w = asa->getTarget();
                    if (!w || !w->getBounds() || !(*w->getBounds() == t.targetBounds)) {
                        continue;
                    }
                }
                if (t.hasTargetFullPath) {
                    auto ana = std::dynamic_pointer_cast<ActivityNameAction>(asa);
                    if (ana && ana->getApeDynamicTargetFullPathHash() != t.targetFullPathHash) {
                        continue;
                    }
                }
                matchedAction = asa;
                break;
            }
            if (!matchedAction) {
                continue;
            }

            naming::StateKey srcKey = naming::StateKey::fromFallbackXmlStringHash("", 0);
            naming::StateKey tgtKey = naming::StateKey::fromFallbackXmlStringHash("", 0);
            if (!tryGetApeStateKey(srcState->hash(), &srcKey) ||
                !tryGetApeStateKey(tgtState->hash(), &tgtKey)) {
                continue;
            }

            ApeTransitionEntry e;
            e.sourceKeyHash = srcKey.hash();
            e.hasSourceStateKey = true;
            e.sourceStateKey = srcKey;
            e.actionHash = matchedAction->hash();
            e.targetKeyHash = tgtKey.hash();
            e.hasTargetStateKey = true;
            e.targetStateKey = tgtKey;
            e.sourceStateHash = srcState->hash();
            e.targetStateHash = tgtState->hash();
            e.actionType = matchedAction->getActionType();
            e.hasTargetBounds = t.hasTargetBounds;
            e.targetBounds = t.targetBounds;
            e.hasTargetFullPath = t.hasTargetFullPath;
            e.targetFullPathHash = t.targetFullPathHash;
            e.sourceActivity = activityKeyCanonical;
            e.valid = true;
            apeInsertTransitionEntryNoRefine(e);
        }

        notifyAgentsOfApeNamingChange();
        return true;
#endif
    }

    void Model::notifyAgentsOfApeNamingChange() {
        for (const auto &kv : _deviceIDAgentMap) {
            if (kv.second) {
                kv.second->onStateAbstractionChanged();
            }
        }
        _apeInvalidatedReuseActionHashes.clear();
    }

    size_t Model::getApeStateCountByActivityAndNamingFingerprint(
        const std::string &activityKeyCanonical, const std::string &namingFingerprint) const {
        if (activityKeyCanonical.empty() || namingFingerprint.empty()) {
            return 0;
        }
        size_t count = 0;
        for (const auto &kv : _ape_state_keys_by_hash) {
            const auto &bucket = kv.second;
            bool match = false;
            for (const auto &k : bucket) {
                if (k.activity() == activityKeyCanonical && k.namingFingerprint() == namingFingerprint) {
                    match = true;
                    break;
                }
            }
            if (match) {
                ++count;
            }
        }
        return count;
    }

    void Model::pruneStaleApeStatesForActivity(const std::string &activityKeyCanonical,
                                               const std::string &staleNamingFingerprint,
                                               const std::unordered_set<uintptr_t> *affectedStateHashes) {
        if (!_graph || activityKeyCanonical.empty() || staleNamingFingerprint.empty()) {
            return;
        }
        std::unordered_set<uintptr_t> staleStateHashes;
        staleStateHashes.reserve(64);
        const bool filterActive = (affectedStateHashes && !affectedStateHashes->empty());
        for (const auto &kv : _ape_state_keys_by_hash) {
            const uintptr_t stateHash = kv.first;
            if (filterActive && affectedStateHashes->count(stateHash) == 0) {
                continue;
            }
            const auto &bucket = kv.second;
            bool match = false;
            for (const auto &k : bucket) {
                if (k.activity() == activityKeyCanonical && k.namingFingerprint() == staleNamingFingerprint) {
                    match = true;
                    break;
                }
            }
            if (match) {
                staleStateHashes.insert(stateHash);
            }
        }
        if (staleStateHashes.empty()) {
            return;
        }
        // Collect action hashes from stale states (pre-prune) so agents can invalidate
        // hash-keyed caches without globally clearing all learned experience.
        for (const auto &s : _graph->getStates()) {
            if (!s || staleStateHashes.count(s->hash()) == 0) {
                continue;
            }
            for (const auto &a : s->getActions()) {
                if (a) {
                    _apeInvalidatedReuseActionHashes.insert(a->hash());
                }
            }
        }
        const size_t removedFromGraph = _graph->removeStatesByHash(staleStateHashes);
        (void)removedFromGraph;
#if defined(FASTBOT_HAS_PUGIXML) && FASTBOT_HAS_PUGIXML
        // Q8 (local rebuild): keep cached XML for states referenced by the per-activity mini-history,
        // even if the corresponding Graph states are pruned, so that later local rebuild can
        // still snapshot and replay these recent trees/transitions.
        const ApeMiniHistory *histKeep = nullptr;
        auto itHistKeep = _apeMiniHistoryByActivity.find(activityKeyCanonical);
        if (itHistKeep != _apeMiniHistoryByActivity.end()) {
            histKeep = &itHistKeep->second;
        }
        auto shouldKeepXml = [&](uintptr_t sh) -> bool {
            if (!histKeep) {
                return false;
            }
            for (uintptr_t h : histKeep->stateHashes) {
                if (h == sh) {
                    return true;
                }
            }
            for (const auto &t : histKeep->transitions) {
                if (!t.valid) {
                    continue;
                }
                if (t.sourceStateHash == sh || t.targetStateHash == sh) {
                    return true;
                }
            }
            return false;
        };
#endif
        for (uintptr_t sh : staleStateHashes) {
            _ape_state_keys_by_hash.erase(sh);
#if defined(FASTBOT_HAS_PUGIXML) && FASTBOT_HAS_PUGIXML
            if (!shouldKeepXml(sh)) {
                _apeStateXmlByStateHash.erase(sh);
            }
#endif
            _apeGuiTreeNamingBlacklist.erase(sh);
        }
        auto pruneSourcePredicatesForActivity = [&]() {
            _apeSourcePartitionPredicates.erase(
                std::remove_if(_apeSourcePartitionPredicates.begin(), _apeSourcePartitionPredicates.end(),
                               [&](ApeSourcePartitionPredicate &pred) {
                                   if (pred.activityKey != activityKeyCanonical) {
                                       return false;
                                   }
                                   for (auto &part : pred.partitions) {
                                       part.erase(std::remove_if(part.begin(), part.end(),
                                                                 [&](uintptr_t h) {
                                                                     return staleStateHashes.count(h) != 0;
                                                                 }),
                                                  part.end());
                                   }
                                   pred.partitions.erase(
                                       std::remove_if(pred.partitions.begin(), pred.partitions.end(),
                                                      [](const std::vector<uintptr_t> &p) { return p.empty(); }),
                                       pred.partitions.end());
                                   return pred.partitions.size() < 2;
                               }),
                _apeSourcePartitionPredicates.end());
        };
        auto pruneActionPredicatesForActivity = [&]() {
            _apeActionPartitionPredicates.erase(
                std::remove_if(_apeActionPartitionPredicates.begin(), _apeActionPartitionPredicates.end(),
                               [&](ApeActionPartitionPredicate &pred) {
                                   if (pred.activityKey != activityKeyCanonical) {
                                       return false;
                                   }
                                   for (auto &part : pred.partitions) {
                                       part.erase(std::remove_if(
                                                      part.begin(), part.end(),
                                                      [&](const std::pair<uintptr_t, size_t> &e) {
                                                          return staleStateHashes.count(e.first) != 0;
                                                      }),
                                                  part.end());
                                   }
                                   pred.partitions.erase(
                                       std::remove_if(
                                           pred.partitions.begin(), pred.partitions.end(),
                                           [](const std::vector<std::pair<uintptr_t, size_t>> &p) {
                                               return p.empty();
                                           }),
                                       pred.partitions.end());
                                   return pred.partitions.size() < 2;
                               }),
                _apeActionPartitionPredicates.end());
        };
        auto pruneStatesFewerThanPredicatesForActivity = [&]() {
            _apeStatesFewerThanPredicates.erase(
                std::remove_if(_apeStatesFewerThanPredicates.begin(), _apeStatesFewerThanPredicates.end(),
                               [&](ApeStatesFewerThanPredicate &pred) {
                                   if (pred.activityKey != activityKeyCanonical) {
                                       return false;
                                   }
                                   pred.stateHashes.erase(
                                       std::remove_if(pred.stateHashes.begin(), pred.stateHashes.end(),
                                                      [&](uintptr_t h) {
                                                          return staleStateHashes.count(h) != 0;
                                                      }),
                                       pred.stateHashes.end());
                                   return pred.stateHashes.empty();
                               }),
                _apeStatesFewerThanPredicates.end());
        };
        pruneSourcePredicatesForActivity();
        pruneActionPredicatesForActivity();
        pruneStatesFewerThanPredicatesForActivity();
        BLOG("ape naming: pruned %zu stale states for activity=%s fp=%s", staleStateHashes.size(),
             activityKeyCanonical.c_str(), staleNamingFingerprint.c_str());
    }

    bool Model::evalApeGuiTreeNamingBlacklist(const std::vector<uintptr_t> &stateHashes,
                                               const naming::NamingPtr &naming) const {
        if (!naming || stateHashes.empty()) {
            return true;
        }
        const std::string &fp = naming->fingerprintString();
        for (uintptr_t sh : stateHashes) {
            auto it = _apeGuiTreeNamingBlacklist.find(sh);
            if (it != _apeGuiTreeNamingBlacklist.end() && it->second.count(fp) != 0) {
                return false;
            }
        }
        return true;
    }

    void Model::apeBlacklistFinerNamingOnRollback(
        const std::string &activity, const naming::NamingPtr &finerNaming,
        const ApeNamingAbstractionContext &ctx, const std::unordered_set<uintptr_t> &affectedStateHashesForBlacklist) {
        (void)ctx;
        if (!finerNaming || affectedStateHashesForBlacklist.empty()) {
            return;
        }
        const std::string fp = finerNaming->fingerprintString();
        // Java blacklistRefinement blacklists exactly the affected GUI trees.
        // Native uses state-hash keyed cache for GUI-tree naming blacklists, so we blacklist
        // by those affected state hashes directly.
        for (uintptr_t sh : affectedStateHashesForBlacklist) {
            _apeGuiTreeNamingBlacklist[sh].insert(fp);
        }
        apeCapGuiTreeNamingBlacklist();
        apeCapApeNamingCoarsenAndRefineBlacklists();
    }

    void Model::apeCapApeNamingCoarsenAndRefineBlacklists() {
        // no-op: match Java unbounded blacklists (NDActionBlacklist/guiTreeNamingBlaclist).
    }

    std::vector<std::string> Model::detectNonDeterminismApe() const {
        const int minTargets = (_preference ? _preference->getApeNamingMinNonDetTargets()
                                            : MinNonDeterminismCount);
        std::set<std::string> activitiesSet;
        for (const auto &kv : _apePairAgg) {
            if (kv.second.targetCounts.size() >= static_cast<size_t>(minTargets)) {
                activitiesSet.insert(kv.second.sourceActivity);
            }
        }
        return std::vector<std::string>(activitiesSet.begin(), activitiesSet.end());
    }

#if defined(FASTBOT_HAS_PUGIXML) && FASTBOT_HAS_PUGIXML && DYNAMIC_STATE_ABSTRACTION_ENABLED
    bool Model::resolveApeWidgetExprAndParentNamelet(uintptr_t stateHash, const std::string &activityForSplit,
                                                     const naming::NamingPtr &cur, const WidgetPtr &targetWidget,
                                                     std::string *outExpr, naming::NameletPtr *outParent) const {
        if (!outExpr || !outParent) {
            return false;
        }
        outExpr->clear();
        *outParent = nullptr;
        if (!targetWidget || !targetWidget->getBounds() || !cur) {
            return false;
        }
        auto itXml = _apeStateXmlByStateHash.find(stateHash);
        if (itXml == _apeStateXmlByStateHash.end() || itXml->second.empty()) {
            return false;
        }
        std::string pkg;
        std::string cls;
        naming::StateKey::splitActivityPackageClass(activityForSplit, &pkg, &cls);
        gui_tree::GUITreeBuildResult built =
            gui_tree::GUITreeFactory::buildFromXml(itXml->second, pkg, cls);
        if (!built.tree || !built.dom) {
            return false;
        }
        if (!naming::NamingFactory::rebuildTree(cur, *built.tree, built.dom)) {
            return false;
        }
        std::vector<gui_tree::GUITreeNode *> po;
        collectGUITreeNodesPreOrder(built.tree->getRootNode(), &po);
        if (po.empty()) {
            return false;
        }
        const Rect targetRect = *targetWidget->getBounds();
        for (gui_tree::GUITreeNode *node : po) {
            if (!node) {
                continue;
            }
            if (!(node->getBounds() == targetRect)) {
                continue;
            }
            const naming::NamePtr nm = node->getXPathName();
            if (nm) {
                *outExpr = nm->toXPath();
                if (!outExpr->empty()) {
                    break;
                }
            }
        }
        if (outExpr->empty()) {
            return false;
        }
        for (gui_tree::GUITreeNode *node : po) {
            if (!node) {
                continue;
            }
            const naming::NamePtr nm = node->getXPathName();
            if (!nm || nm->toXPath() != *outExpr) {
                continue;
            }
            naming::NameletPtr nl = node->getCurrentNamelet();
            if (!nl) {
                continue;
            }
            if (!*outParent) {
                *outParent = nl;
            } else if ((*outParent).get() != nl.get()) {
                outParent->reset();
                return false;
            }
        }
        return static_cast<bool>(*outParent);
    }
#endif

namespace {
/**
 * StateNamingManager::updateNaming(Refine) requires new naming to be a direct refinement child of `cur`.
 * actionRefinementCandidatesWithOptions may return a multi-hop refinement; walk up to the node whose
 * parent is `cur` so structural update and state-key edges match the manager contract.
 */
naming::NamingPtr apeRefineTargetAsDirectChild(const naming::NamingPtr &cur, naming::NamingPtr next) {
    if (!cur || !next) {
        return next;
    }
    if (next->getParent() == cur) {
        return next;
    }
    naming::NamingPtr x = next;
    while (x && x->getParent() && x->getParent() != cur) {
        x = x->getParent();
    }
    if (x && x->getParent() == cur) {
        return x;
    }
    return next;
}
} // namespace

    bool Model::refineActivityApeNaming(const std::string &activity) {
        return refineActivityApeNaming(activity, nullptr, -1);
    }

    bool Model::refineActivityApeNaming(const std::string &activity, const ApeRefinePair *pair,
                                        int precomputedActivityNonDetPairCount) {
        const std::string actKey = naming::StateKey::canonicalActivityString(activity);
        const int minTargets = (_preference ? _preference->getApeNamingMinNonDetTargets()
                                            : MinNonDeterminismCount);
        int nonDetPairs = 0;
        size_t dominantPairTargets = 0;
        uintptr_t dominantSourceKeyHash = 0;
        uintptr_t dominantActionHash = 0;
        std::unordered_set<uintptr_t> dominantTargetKeyHashes;
        const bool useBatchNonDet =
            precomputedActivityNonDetPairCount >= 0 && pair && pair->sourceKeyHash != 0 &&
            pair->actionHash != 0 && pair->targetCount >= static_cast<size_t>(minTargets);
        if (useBatchNonDet) {
            nonDetPairs = precomputedActivityNonDetPairCount;
            dominantPairTargets = pair->targetCount;
            dominantSourceKeyHash = pair->sourceKeyHash;
            dominantActionHash = pair->actionHash;
            dominantTargetKeyHashes = pair->targetKeyHashes;
        } else {
            for (const auto &kv : _apePairAgg) {
                if (kv.second.sourceActivity != actKey) {
                    continue;
                }
                const auto &tm = kv.second.targetCounts;
                if (tm.size() < static_cast<size_t>(minTargets)) {
                    continue;
                }
                nonDetPairs++;
                if (tm.size() > dominantPairTargets) {
                    dominantPairTargets = tm.size();
                    dominantSourceKeyHash = kv.first.sourceKeyHash;
                    dominantActionHash = kv.first.actionHash;
                    dominantTargetKeyHashes.clear();
                    tm.forEach([&](uintptr_t h, int /*count*/) {
                        dominantTargetKeyHashes.insert(h);
                    });
                }
            }
            // Batch (`runApeNamingAbstractionBatch`) passes the exact non-det pair for this attempt; use it as
            // the trigger for blacklist/admissibility and `ctx.trigger*` so logs and behavior match `refine-attempt`.
            // With `pair == nullptr` (one-arg API), keep scan-only dominant = max target fan-out for this activity.
            if (pair && pair->sourceKeyHash != 0 && pair->actionHash != 0 &&
                pair->targetCount >= static_cast<size_t>(minTargets)) {
                dominantPairTargets = pair->targetCount;
                dominantSourceKeyHash = pair->sourceKeyHash;
                dominantActionHash = pair->actionHash;
                dominantTargetKeyHashes = pair->targetKeyHashes;
            }
        }
        if (dominantSourceKeyHash != 0 || dominantActionHash != 0) {
            ApePairKey pairKey{dominantSourceKeyHash, dominantActionHash};
            auto itBlk = _apeRefinePairBlacklist.find(actKey);
            if (itBlk != _apeRefinePairBlacklist.end() && itBlk->second.count(pairKey) != 0) {
                BDLOG("ape naming: skip refine activity=%s reason=trigger pair blacklisted srcKey=%lu act=%lu",
                      activity.c_str(), (unsigned long)dominantSourceKeyHash, (unsigned long)dominantActionHash);
                if (shouldLogApeDiagSample(std::string("skip_pair_blacklist#") + actKey, 20)) {
                    BLOG("ape naming: diag skip-summary activity=%s reason=pair_blacklisted srcKey=%lu act=%lu nonDetPairs=%d domTargets=%zu",
                         activity.c_str(), (unsigned long)dominantSourceKeyHash,
                         (unsigned long)dominantActionHash, nonDetPairs, dominantPairTargets);
                }
                return false;
            }
            auto itActionBlk = _apeRefineActionBlacklist.find(actKey);
            ApeActivityRebuildStats &st = _apeRebuildStatsByActivity[actKey];
            if (dominantActionHash != 0) {
                ++st.actionBlacklistChecks;
            }
            if (itActionBlk != _apeRefineActionBlacklist.end() &&
                itActionBlk->second.count(dominantActionHash) != 0) {
                if (dominantActionHash != 0) {
                    ++st.actionBlacklistHits;
                }
                (void)apeLocalRebuildFromHistoryIfNeeded(actKey, "action_blacklist");
                BDLOG("ape naming: skip refine activity=%s reason=trigger action blacklisted act=%lu",
                      activity.c_str(), (unsigned long)dominantActionHash);
                if (shouldLogApeDiagSample(std::string("skip_action_blacklist#") + actKey, 20)) {
                    const size_t blkSize = (itActionBlk == _apeRefineActionBlacklist.end()) ? 0 : itActionBlk->second.size();
                    BLOG("ape naming: diag skip-summary activity=%s reason=action_blacklisted act=%lu checks=%d hits=%d blacklistSize=%zu nonDetPairs=%d domTargets=%zu",
                         activity.c_str(), (unsigned long)dominantActionHash,
                         st.actionBlacklistChecks, st.actionBlacklistHits, blkSize,
                         nonDetPairs, dominantPairTargets);
                }
                return false;
            }
        }
        const int minNonDetPairs = (_preference ? _preference->getApeNamingActionRefineMinNonDetPairs() : 1);
        if (nonDetPairs < minNonDetPairs) {
            BDLOG("ape naming: skip refine activity=%s reason=nonDetPairs<%d (%d)",
                  activity.c_str(), minNonDetPairs, nonDetPairs);
            if (shouldLogApeDiagSample(std::string("skip_non_det_pairs#") + actKey, 20)) {
                BLOG("ape naming: diag skip-summary activity=%s reason=nonDetPairs now=%d min=%d",
                     activity.c_str(), nonDetPairs, minNonDetPairs);
            }
            return false;
        }
        const int minNonDetPairDelta =
            (_preference ? _preference->getApeNamingActionRefineMinNonDetPairDelta() : 0);
        auto itCtx = _apeNamingContext.find(actKey);
        if (itCtx != _apeNamingContext.end()) {
            const int lastPairs = itCtx->second.nonDetPairsAtLastNamingRefinement;
            if (nonDetPairs < lastPairs + minNonDetPairDelta) {
                BDLOG("ape naming: skip refine activity=%s reason=nonDetPairDelta<%d (now=%d,last=%d)",
                      activity.c_str(), minNonDetPairDelta, nonDetPairs, lastPairs);
                if (shouldLogApeDiagSample(std::string("skip_non_det_delta#") + actKey, 20)) {
                    BLOG("ape naming: diag skip-summary activity=%s reason=nonDetPairDelta now=%d last=%d minDelta=%d",
                         activity.c_str(), nonDetPairs, lastPairs, minNonDetPairDelta);
                }
                return false;
            }
        }
        naming::ActivityNamingManager &mgr = _apeStateNamingManager->activityManager();
        naming::NamingPtr cur = mgr.getNaming(actKey);
        if (!cur) {
            cur = naming::NamingFactory::defaultRootNaming();
            if (!cur) {
                BDLOG("ape naming: skip refine activity=%s reason=no default root naming", activity.c_str());
                return false;
            }
            _apeStateNamingManager->updateNaming(actKey, naming::NamingUpdateKind::Refine, cur);
        }
        const size_t activityStateCount = getApeStateCountByActivityAndNamingFingerprint(
            actKey, cur ? cur->fingerprintString() : std::string());
        const int minStates = (_preference ? _preference->getApeNamingActionRefineMinActivityStates() : 2);
        if (activityStateCount < static_cast<size_t>(minStates)) {
            BDLOG("ape naming: skip refine activity=%s reason=stateCount<%d (%zu)",
                  activity.c_str(), minStates, activityStateCount);
            if (shouldLogApeDiagSample(std::string("skip_state_count#") + actKey, 20)) {
                BLOG("ape naming: diag skip-summary activity=%s reason=stateCount now=%zu min=%d",
                     activity.c_str(), activityStateCount, minStates);
            }
            return false;
        }
        const int minStateDelta = (_preference ? _preference->getApeNamingActionRefineMinStateDelta() : 1);
        if (itCtx != _apeNamingContext.end()) {
            const size_t lastCount = itCtx->second.stateCountAtLastNamingRefinement;
            if (activityStateCount < lastCount + static_cast<size_t>(minStateDelta)) {
                BDLOG("ape naming: skip refine activity=%s reason=stateDelta<%d (now=%zu,last=%zu)",
                      activity.c_str(), minStateDelta, activityStateCount, lastCount);
                if (shouldLogApeDiagSample(std::string("skip_state_delta#") + actKey, 20)) {
                    BLOG("ape naming: diag skip-summary activity=%s reason=stateDelta now=%zu last=%zu minDelta=%d",
                         activity.c_str(), activityStateCount, lastCount, minStateDelta);
                }
                return false;
            }
        }
        naming::NamerLattice lat(naming::NamerFactory::current());
        std::set<std::string> blk;
        for (const auto &p : _apeNamingCoarseningBlacklist) {
            if (p.first == actKey) {
                blk.insert(p.second);
            }
        }
        naming::NamingFactory::ActionRefinementOptions userOpts;
        userOpts.max_steps = (_preference ? _preference->getApeNamingActionRefineHops() : 8);
        userOpts.blacklist = &blk;
        const std::string predicateMode =
            (_preference ? _preference->getApeNamingActionRefinePredicateMode() : "fingerprint_change");
        const std::string selectionMode =
            (_preference ? _preference->getApeNamingActionRefineSelectionMode() : "first_accept");
        const std::string ruleProfile =
            (_preference ? _preference->getApeNamingActionRefineRuleProfile() : "baseline");
        userOpts.choose_deepest_acceptable = (selectionMode == "deepest_accept");
        if (ruleProfile == "java_rule_01_preview") {
            userOpts.choose_deepest_acceptable = true;
            const std::string curFp = cur->fingerprintString();
            const int curFineness = cur->getFineness();
            userOpts.accept_predicate = [curFp, curFineness](const naming::NamingPtr &candidate) {
                return candidate && candidate->fingerprintString() != curFp &&
                       candidate->getFineness() > curFineness;
            };
        } else if (ruleProfile == "java_rule_03_preview") {
            userOpts.choose_deepest_acceptable = true;
            userOpts.evaluate_all_immediate_candidates = true;
            const std::string curFp = cur->fingerprintString();
            userOpts.accept_predicate = [curFp](const naming::NamingPtr &candidate) {
                return candidate && candidate->fingerprintString() != curFp;
            };
        } else if (ruleProfile == "strict_baseline") {
            const std::string curFp = cur->fingerprintString();
            const int curFineness = cur->getFineness();
            userOpts.accept_predicate = [curFp, curFineness](const naming::NamingPtr &candidate) {
                return candidate && candidate->fingerprintString() != curFp &&
                       candidate->getFineness() > curFineness;
            };
        } else if (predicateMode == "fingerprint_change") {
            const std::string curFp = cur->fingerprintString();
            userOpts.accept_predicate = [curFp](const naming::NamingPtr &candidate) {
                return candidate && candidate->fingerprintString() != curFp;
            };
        } else if (predicateMode == "fineness_increase") {
            const int curFineness = cur->getFineness();
            userOpts.accept_predicate = [curFineness](const naming::NamingPtr &candidate) {
                return candidate && candidate->getFineness() > curFineness;
            };
        } else {
            userOpts.accept_predicate = {};
        }
        // Java NamingFactory.refine: actionRefinementFirst tries action-style resolve before state-style (or vice versa).
        naming::NamingFactory::ActionRefinementOptions strictBranchOpts;
        strictBranchOpts.max_steps = userOpts.max_steps;
        strictBranchOpts.blacklist = &blk;
        strictBranchOpts.choose_deepest_acceptable = true;
        strictBranchOpts.evaluate_all_immediate_candidates = true;
        {
            const std::string curFpS = cur->fingerprintString();
            const int curFinenessS = cur->getFineness();
            strictBranchOpts.accept_predicate = [curFpS, curFinenessS](const naming::NamingPtr &candidate) {
                return candidate && candidate->fingerprintString() != curFpS &&
                       candidate->getFineness() > curFinenessS;
            };
        }
        const bool actionRefinementFirst =
            !_preference || _preference->useApeNamingActionRefinementFirst();
        // XML-space remapped trigger hashes (align Element->GUITree hash space to XML->GUITree space
        // so coarsen-gate hash comparisons are consistent with buildFromXml path).
        uintptr_t xmlSpaceTriggerSourceKeyHash = dominantSourceKeyHash;
        std::unordered_set<uintptr_t> xmlSpaceTriggerTargetKeyHashes = dominantTargetKeyHashes;
        std::vector<naming::NamingPtr> candidates;
        if (actionRefinementFirst) {
            candidates =
                naming::NamingFactory::actionRefinementCandidatesWithOptions(cur, lat, strictBranchOpts);
            if (candidates.empty()) {
                candidates =
                    naming::NamingFactory::actionRefinementCandidatesWithOptions(cur, lat, userOpts);
            }
        } else {
            candidates =
                naming::NamingFactory::actionRefinementCandidatesWithOptions(cur, lat, userOpts);
            if (candidates.empty()) {
                candidates =
                    naming::NamingFactory::actionRefinementCandidatesWithOptions(cur, lat, strictBranchOpts);
            }
        }
        {
            const std::string curFpGather = cur ? cur->fingerprintString() : std::string("-");
            BDLOG(
                "ape naming: refine gather-cands activity=%s pair=%p useBatchNonDet=%d nonDetPairs=%d "
                "states=%zu minStates=%d srcKey=%lu act=%lu domTargets=%zu curFin=%d rule=%s pred=%s sel=%s "
                "maxSteps=%d actionRefinementFirst=%d candCount=%zu cur_fp=%s",
                activity.c_str(), static_cast<const void *>(pair), useBatchNonDet ? 1 : 0, nonDetPairs,
                activityStateCount, minStates, (unsigned long)dominantSourceKeyHash,
                (unsigned long)dominantActionHash, dominantPairTargets, cur ? cur->getFineness() : -1,
                ruleProfile.c_str(), predicateMode.c_str(), selectionMode.c_str(), userOpts.max_steps,
                actionRefinementFirst ? 1 : 0, candidates.size(), curFpGather.c_str());
        }
        struct CandidateEval {
            naming::NamingPtr naming;
            int score{0};
            bool strictFiner{false};
            bool fingerprintChanged{false};
            int finenessGain{0};
            bool replayUsed{false};
            int replayDistinctTargets{0};
            int replaySourceChanged{0};
            /// APE RefinementResult comparator: |states1|+|states2| from checkStateRefinement (replay only); -1 = n/a.
            int apePartitionStateCost{-1};
        };
        std::vector<uintptr_t> guiTreeBlacklistCheckHashes;
#if defined(FASTBOT_HAS_PUGIXML) && FASTBOT_HAS_PUGIXML && DYNAMIC_STATE_ABSTRACTION_ENABLED
        // Build stateHash -> StatePtr and keyHash -> stateHash indices once
        // (avoids repeated O(N) graph scans throughout refine).
        std::unordered_map<uintptr_t, StatePtr> stateByHash;
        std::unordered_map<uintptr_t, std::vector<uintptr_t>> stateHashesByKeyHash;
        {
            const auto &allStates = getGraph()->getStates();
            stateByHash.reserve(allStates.size());
            stateHashesByKeyHash.reserve(64);
            for (const auto &sp : allStates) {
                if (!sp) {
                    continue;
                }
                stateByHash[sp->hash()] = sp;
                uintptr_t kH = 0;
                if (tryGetApeStateKeyHash(sp->hash(), &kH)) {
                    stateHashesByKeyHash[kH].push_back(sp->hash());
                }
            }
        }
        std::vector<uintptr_t> triggerSourceStateHashesForReplay;
        if (dominantSourceKeyHash != 0) {
            auto itSrc = stateHashesByKeyHash.find(dominantSourceKeyHash);
            if (itSrc != stateHashesByKeyHash.end()) {
                triggerSourceStateHashesForReplay = itSrc->second;
            }
        }
        guiTreeBlacklistCheckHashes = triggerSourceStateHashesForReplay;
        bool replayActive = false;
        std::string replaySrcXml;
        uintptr_t replaySrcStateHash = 0;
        std::vector<uintptr_t> replayTgtStateHashes;
        if (_preference && _preference->useApeNamingCandidateTransitionReplay() &&
            dominantPairTargets >= static_cast<size_t>(minTargets) && dominantSourceKeyHash != 0 &&
            !dominantTargetKeyHashes.empty()) {
            auto findRepresentativeStateHashForKey = [&](uintptr_t keyH) -> uintptr_t {
                auto it = stateHashesByKeyHash.find(keyH);
                if (it == stateHashesByKeyHash.end()) {
                    return static_cast<uintptr_t>(0);
                }
                // Prefer one with cached XML (replay requires XML).
                for (uintptr_t sh : it->second) {
                    auto itXml = _apeStateXmlByStateHash.find(sh);
                    if (itXml != _apeStateXmlByStateHash.end() && !itXml->second.empty()) {
                        return sh;
                    }
                }
                return it->second.empty() ? static_cast<uintptr_t>(0) : it->second.front();
            };
            replaySrcStateHash = findRepresentativeStateHashForKey(dominantSourceKeyHash);
            if (replaySrcStateHash != 0) {
                auto itS = _apeStateXmlByStateHash.find(replaySrcStateHash);
                if (itS != _apeStateXmlByStateHash.end() && !itS->second.empty()) {
                    replaySrcXml = itS->second;
                }
            }
            replayTgtStateHashes.reserve(dominantTargetKeyHashes.size());
            for (uintptr_t th : dominantTargetKeyHashes) {
                const uintptr_t tsh = findRepresentativeStateHashForKey(th);
                if (tsh == 0) {
                    continue;
                }
                auto itT = _apeStateXmlByStateHash.find(tsh);
                if (itT != _apeStateXmlByStateHash.end() && !itT->second.empty()) {
                    replayTgtStateHashes.push_back(tsh);
                }
            }
            replayActive =
                !replaySrcXml.empty() &&
                replayTgtStateHashes.size() >= static_cast<size_t>(minTargets);
            if (!replayActive && dominantPairTargets >= static_cast<size_t>(minTargets)) {
                BDLOG("ape naming: replay skipped activity=%s haveSrcXml=%d tgtXml=%zu needTgt=%zu",
                      activity.c_str(), replaySrcStateHash != 0 ? 1 : 0, replayTgtStateHashes.size(),
                      dominantTargetKeyHashes.size());
            }
        }
        // Remap dominantSourceKeyHash / dominantTargetKeyHashes from Element->GUITree hash space
        // to XML->GUITree hash space. The coarsen gate (apeStateHashFromXmlWithNaming) always
        // reconstructs hashes via buildFromXml, which can diverge from buildFromElement due to
        // subtle attribute-handling differences (scrollable int vs bool, isEditText, etc.).
        {
            auto remapKeyHashToXmlSpace = [&](uintptr_t elementKeyHash) -> uintptr_t {
                if (elementKeyHash == 0) {
                    return 0;
                }
                auto itBucket = stateHashesByKeyHash.find(elementKeyHash);
                if (itBucket == stateHashesByKeyHash.end()) {
                    return elementKeyHash;
                }
                for (uintptr_t sh : itBucket->second) {
                    auto itXml = _apeStateXmlByStateHash.find(sh);
                    if (itXml == _apeStateXmlByStateHash.end() || itXml->second.empty()) {
                        continue;
                    }
                    uintptr_t xmH = 0;
                    if (apeStateHashFromXmlWithNaming(activity, itXml->second, cur, &xmH) &&
                        xmH != 0) {
                        return xmH;
                    }
                }
                return elementKeyHash;
            };
            xmlSpaceTriggerSourceKeyHash = remapKeyHashToXmlSpace(dominantSourceKeyHash);
            if (xmlSpaceTriggerSourceKeyHash != dominantSourceKeyHash) {
                BDLOG("ape naming: remap triggerSource %lu -> %lu (xml-space)",
                      (unsigned long)dominantSourceKeyHash, (unsigned long)xmlSpaceTriggerSourceKeyHash);
            }
            xmlSpaceTriggerTargetKeyHashes.clear();
            for (uintptr_t tkh : dominantTargetKeyHashes) {
                xmlSpaceTriggerTargetKeyHashes.insert(remapKeyHashToXmlSpace(tkh));
            }
        }
#else
        const bool replayActive = false;
#endif
#if DYNAMIC_STATE_ABSTRACTION_ENABLED
        {
#if !defined(FASTBOT_HAS_PUGIXML) || !FASTBOT_HAS_PUGIXML
            if (guiTreeBlacklistCheckHashes.empty() && dominantSourceKeyHash != 0) {
                size_t srcReprAdded = 0;
                for (const auto &sp : getGraph()->getStates()) {
                    if (!sp) {
                        continue;
                    }
                    auto ap = sp->getActivityString();
                    const std::string a =
                        (ap && ap.get()) ? naming::StateKey::canonicalActivityString(*ap) : std::string();
                    if (a != actKey) {
                        continue;
                    }
                    uintptr_t kH = 0;
                    if (tryGetApeStateKeyHash(sp->hash(), &kH) && kH == dominantSourceKeyHash) {
                        guiTreeBlacklistCheckHashes.push_back(sp->hash());
                        ++srcReprAdded;
                    }
                }
            }
#endif
            std::unordered_set<uintptr_t> seenGtb(guiTreeBlacklistCheckHashes.begin(),
                                                  guiTreeBlacklistCheckHashes.end());
            for (uintptr_t tkh : dominantTargetKeyHashes) {
                auto it = stateHashesByKeyHash.find(tkh);
                if (it == stateHashesByKeyHash.end()) {
                    continue;
                }
                for (uintptr_t sh : it->second) {
                    if (seenGtb.insert(sh).second) {
                        guiTreeBlacklistCheckHashes.push_back(sh);
                    }
                }
            }
        }
#endif
#if defined(FASTBOT_HAS_PUGIXML) && FASTBOT_HAS_PUGIXML && DYNAMIC_STATE_ABSTRACTION_ENABLED
        if (pair && dominantPairTargets >= static_cast<size_t>(minTargets) && dominantActionHash != 0 &&
            !guiTreeBlacklistCheckHashes.empty()) {
            ActionType dominantActionType = ActionType::NOP;
            uintptr_t dominantTargetWidgetHash = 0;
            bool hasDominantActionIdentity = false;
            for (uintptr_t repSh : guiTreeBlacklistCheckHashes) {
                auto itRep = stateByHash.find(repSh);
                if (itRep == stateByHash.end() || !itRep->second) {
                    continue;
                }
                const StatePtr &repSp = itRep->second;
                for (const auto &a : repSp->getActions()) {
                    auto asa = std::dynamic_pointer_cast<ActivityStateAction>(a);
                    if (!asa || asa->hash() != dominantActionHash) {
                        continue;
                    }
                    dominantActionType = asa->getActionType();
                    if (asa->getTarget()) {
                        dominantTargetWidgetHash = asa->getTarget()->hash();
                        hasDominantActionIdentity = true;
                    }
                    break;
                }
                if (hasDominantActionIdentity) {
                    break;
                }
            }
            auto findTargetWidget = [&](uintptr_t sh) -> WidgetPtr {
                auto itSp = stateByHash.find(sh);
                const StatePtr &sp = (itSp != stateByHash.end()) ? itSp->second : StatePtr{};
                if (!sp) {
                    return nullptr;
                }
                for (const auto &a : sp->getActions()) {
                    auto asa = std::dynamic_pointer_cast<ActivityStateAction>(a);
                    if (!asa) {
                        continue;
                    }
                    bool match = false;
                    if (hasDominantActionIdentity && asa->getTarget()) {
                        match = (asa->getActionType() == dominantActionType &&
                                 asa->getTarget()->hash() == dominantTargetWidgetHash);
                    } else {
                        match = (asa->hash() == dominantActionHash);
                    }
                    if (!match) {
                        continue;
                    }
                    return asa->getTarget();
                }
                return nullptr;
            };

            std::string widgetExpr;
            naming::NameletPtr widgetParent;
            bool firstOk = false;
            for (uintptr_t h : guiTreeBlacklistCheckHashes) {
                WidgetPtr tw = findTargetWidget(h);
                std::string exprCand;
                naming::NameletPtr parentCand;
                if (!resolveApeWidgetExprAndParentNamelet(h, activity, cur, tw, &exprCand, &parentCand)) {
                    continue;
                }
                if (!firstOk) {
                    widgetExpr = std::move(exprCand);
                    widgetParent = std::move(parentCand);
                    firstOk = true;
                    continue;
                }
                if (exprCand != widgetExpr || parentCand.get() != widgetParent.get()) {
                    widgetExpr.clear();
                    widgetParent.reset();
                    break;
                }
            }
            if (widgetParent && !widgetExpr.empty()) {
                std::vector<naming::NamingPtr> widgetCands =
                    naming::NamingFactory::widgetXPathRefinementCandidatesWithOptions(
                        cur, lat, strictBranchOpts, widgetParent, widgetExpr);
                if (widgetCands.empty()) {
                    widgetCands = naming::NamingFactory::widgetXPathRefinementCandidatesWithOptions(
                        cur, lat, userOpts, widgetParent, widgetExpr);
                }
                if (!widgetCands.empty()) {
                    std::unordered_set<std::string> seenFp;
                    std::vector<naming::NamingPtr> merged;
                    merged.reserve(widgetCands.size() + candidates.size());
                    for (const auto &cand : widgetCands) {
                        if (!cand) {
                            continue;
                        }
                        if (seenFp.insert(cand->fingerprintString()).second) {
                            merged.push_back(cand);
                        }
                    }
                    for (const auto &cand : candidates) {
                        if (!cand) {
                            continue;
                        }
                        if (seenFp.insert(cand->fingerprintString()).second) {
                            merged.push_back(cand);
                        }
                    }
                    candidates.swap(merged);
                }
            }
        }
#endif
        if (candidates.empty()) {
            BDLOG("ape naming: skip refine activity=%s reason=no non-blacklisted finer namer", activity.c_str());
            return false;
        }
        std::vector<CandidateEval> accepted;
        accepted.reserve(candidates.size());
        const std::string curFp = cur->fingerprintString();
        const int curFine = cur->getFineness();
        for (const auto &cand : candidates) {
            if (!cand) {
                continue;
            }
            CandidateEval e;
            e.naming = cand;
            e.finenessGain = cand->getFineness() - curFine;
            e.strictFiner = e.finenessGain > 0;
            e.fingerprintChanged = cand->fingerprintString() != curFp;

            // Transition-level admissibility baseline:
            // on concrete non-deterministic (sourceKey, action) pairs, require strict refinement.
            if (dominantPairTargets >= static_cast<size_t>(minTargets) &&
                (!e.strictFiner || !e.fingerprintChanged)) {
                continue;
            }
            // Java resolve/filter spirit:
            // rank candidates by stronger separation power while keeping deterministic tie-breaks.
            if (e.strictFiner) {
                e.score += 100;
            }
            if (e.fingerprintChanged) {
                e.score += 80;
            }
            if (e.finenessGain > 0) {
                e.score += std::min(5, e.finenessGain) * 10;
            }
            // In non-det scenario prefer deeper split candidate.
            if (dominantPairTargets >= static_cast<size_t>(minTargets)) {
                e.score += std::min<int>(10, static_cast<int>(dominantPairTargets)) * 3;
            }
#if defined(FASTBOT_HAS_PUGIXML) && FASTBOT_HAS_PUGIXML && DYNAMIC_STATE_ABSTRACTION_ENABLED
            if (replayActive) {
                e.replayUsed = true;
                std::unordered_map<uintptr_t, uintptr_t> replayKeyCacheCur;
                // Local memoization: avoid repeated XML parse + rebuildTree for the same (stateHash, naming).
                std::unordered_map<uintptr_t, uintptr_t> keyCacheCand;
                keyCacheCand.reserve(replayTgtStateHashes.size() + triggerSourceStateHashesForReplay.size() + 8);
                auto getApeKeyHashForStateHash = [&](const naming::NamingPtr &nm,
                                                     std::unordered_map<uintptr_t, uintptr_t> &cache,
                                                     uintptr_t sh,
                                                     uintptr_t *out) -> bool {
                    if (!out || !nm || sh == 0) {
                        return false;
                    }
                    auto it = cache.find(sh);
                    if (it != cache.end()) {
                        *out = it->second;
                        return true;
                    }
                    auto itXml = _apeStateXmlByStateHash.find(sh);
                    if (itXml == _apeStateXmlByStateHash.end() || itXml->second.empty()) {
                        return false;
                    }
                    uintptr_t h = 0;
                    if (!apeStateHashFromXmlWithNaming(activity, itXml->second, nm, &h)) {
                        return false;
                    }
                    cache.emplace(sh, h);
                    *out = h;
                    return true;
                };
                uintptr_t srcKeyCandH = 0;
                uintptr_t srcKeyCurH = 0;
                if (getApeKeyHashForStateHash(cand, keyCacheCand, replaySrcStateHash, &srcKeyCandH) &&
                    getApeKeyHashForStateHash(cur, replayKeyCacheCur, replaySrcStateHash, &srcKeyCurH)) {
                    e.replaySourceChanged = (srcKeyCandH != srcKeyCurH) ? 1 : 0;
                }
                std::unordered_set<uintptr_t> uniqTgt;
                uniqTgt.reserve(replayTgtStateHashes.size() * 2 + 1);
                for (uintptr_t tsh : replayTgtStateHashes) {
                    uintptr_t tkH = 0;
                    if (getApeKeyHashForStateHash(cand, keyCacheCand, tsh, &tkH)) {
                        uniqTgt.insert(tkH);
                    }
                }
                e.replayDistinctTargets = static_cast<int>(uniqTgt.size());
                e.score += e.replayDistinctTargets * 500;
                e.score += e.replaySourceChanged * 150;

                // APE checkStateRefinement partition sizes: |states1| (source transition trees) + |states2| (targets).
                std::unordered_set<uintptr_t> srcKeysUnderCand;
                srcKeysUnderCand.reserve(triggerSourceStateHashesForReplay.size() * 2 + 1);
                for (uintptr_t sh : triggerSourceStateHashesForReplay) {
                    uintptr_t skH = 0;
                    if (getApeKeyHashForStateHash(cand, keyCacheCand, sh, &skH)) {
                        srcKeysUnderCand.insert(skH);
                    }
                }
                bool tgtIntersectsSrc = false;
                if (srcKeysUnderCand.empty() && srcKeyCandH != 0) {
                    srcKeysUnderCand.insert(srcKeyCandH);
                }
                for (uintptr_t th : uniqTgt) {
                    if (srcKeysUnderCand.count(th) != 0) {
                        tgtIntersectsSrc = true;
                        break;
                    }
                }
                if (tgtIntersectsSrc) {
                    continue;
                }
                e.apePartitionStateCost = static_cast<int>(srcKeysUnderCand.size() + uniqTgt.size());
            }
#endif
            // Match java.util.Predicate.Type order: STATE_ABSTRACTION before STATE_REFINEMENT.
            if (!evalApeGuiTreeNamingBlacklist(guiTreeBlacklistCheckHashes, cand) ||
                !evalApeStatesFewerThanPredicates(activity, cand) ||
                !evalApeSourcePartitionPredicates(activity, cand) ||
                !evalApeActionPartitionPredicates(activity, cand)) {
                continue;
            }
            accepted.push_back(std::move(e));
        }
        if (accepted.empty()) {
            BDLOG("ape naming: skip refine activity=%s reason=all candidates filtered "
                  "candidateCount=%zu dominantPairTargets=%zu",
                  activity.c_str(), candidates.size(), dominantPairTargets);
            return false;
        }
        std::sort(accepted.begin(), accepted.end(), [](const CandidateEval &a, const CandidateEval &b) {
            if (a.replayUsed && b.replayUsed) {
                if (a.replayDistinctTargets != b.replayDistinctTargets) {
                    return a.replayDistinctTargets > b.replayDistinctTargets;
                }
                if (a.replaySourceChanged != b.replaySourceChanged) {
                    return a.replaySourceChanged > b.replaySourceChanged;
                }
                if (a.apePartitionStateCost >= 0 && b.apePartitionStateCost >= 0 &&
                    a.apePartitionStateCost != b.apePartitionStateCost) {
                    return a.apePartitionStateCost < b.apePartitionStateCost;
                }
            }
            if (a.score != b.score) return a.score > b.score;
            // APE filterRefinementResult: prefer smaller |states1|+|states2| when replay did not supply cost.
            if (a.apePartitionStateCost >= 0 && b.apePartitionStateCost >= 0 &&
                a.apePartitionStateCost != b.apePartitionStateCost) {
                return a.apePartitionStateCost < b.apePartitionStateCost;
            }
            if (a.finenessGain != b.finenessGain) return a.finenessGain < b.finenessGain;
            const int lex = compareNamingLexicographicForApeFilter(a.naming, b.naming);
            if (lex != 0) return lex < 0;
            return a.naming->fingerprintString() < b.naming->fingerprintString();
        });
        naming::NamingPtr next = apeRefineTargetAsDirectChild(cur, accepted.front().naming);
        BLOG("ape naming: refine-candidates activity=%s total=%zu accepted=%zu bestScore=%d bestFineGain=%d "
             "replay=%s distinctTgt=%d srcChanged=%d partitionCost=%d",
             activity.c_str(), candidates.size(), accepted.size(), accepted.front().score,
             accepted.front().finenessGain, accepted.front().replayUsed ? "yes" : "no",
             accepted.front().replayDistinctTargets, accepted.front().replaySourceChanged,
             accepted.front().apePartitionStateCost);
        {
            static std::atomic<uint64_t> g_refine_pick_seq{0};
            const uint64_t pickN = ++g_refine_pick_seq;
            if (pickN <= 16 || (pickN % 512) == 0) {
                const naming::NamingPtr nextPar = next ? next->getParent() : nullptr;
                int top3Direct = 0;
                const size_t lim = std::min<size_t>(accepted.size(), 3);
                for (size_t i = 0; i < lim; ++i) {
                    const naming::NamingPtr c = accepted[i].naming;
                    const naming::NamingPtr p = c ? c->getParent() : nullptr;
                    if (p.get() == cur.get()) {
                        top3Direct++;
                    }
                }
                BDLOG(
                    "ape naming: chain picked Refine act=%s cur=%p next=%p next_par=%p par_eq_cur=%d "
                    "raw_cands=%zu accepted=%zu top3_direct_child_of_cur=%d curFin=%d nextFin=%d",
                    actKey.c_str(), static_cast<const void *>(cur.get()),
                    static_cast<const void *>(next.get()), static_cast<const void *>(nextPar.get()),
                    (nextPar.get() == cur.get()) ? 1 : 0, candidates.size(), accepted.size(), top3Direct,
                    cur ? cur->getFineness() : -1, next ? next->getFineness() : -1);
            }
        }
        ApeNamingAbstractionContext &ctx = _apeNamingContext[actKey];
        ctx.previousNamingBeforeRefine = cur;
        ctx.previousNamingFingerprintBeforeRefine = cur->fingerprintString();
        ctx.oldKeyHashToNewKeyHashes.clear();
        ctx.oldKeyHashToObservationCount.clear();
        ctx.stateCountAtLastNamingRefinement = activityStateCount;
        ctx.nonDetPairsAtLastNamingRefinement = nonDetPairs;
        ctx.triggerSourceKeyHash = xmlSpaceTriggerSourceKeyHash;
        ctx.triggerSourceKeyHashOriginal = dominantSourceKeyHash;
        ctx.triggerSourceKeyExact = (pair && pair->hasSourceStateKey);
        if (ctx.triggerSourceKeyExact) {
            ctx.triggerSourceKey = pair->sourceStateKey;
        } else {
            ctx.triggerSourceKey = naming::StateKey::fromFallbackXmlStringHash("", 0);
        }
        ctx.triggerActionHash = dominantActionHash;
        ctx.triggerTargetCountAtRefine = dominantPairTargets;
#if defined(FASTBOT_HAS_PUGIXML) && FASTBOT_HAS_PUGIXML
        {
            auto hasXml = [&](uintptr_t sh) -> bool {
                auto itXml = _apeStateXmlByStateHash.find(sh);
                return itXml != _apeStateXmlByStateHash.end() && !itXml->second.empty();
            };
            std::vector<std::vector<uintptr_t>> predParts;
            constexpr size_t kMaxSourcePartitionRepr = 2;
            constexpr size_t kMaxTargetPartitionParts = 4; // total target partitions (each by one tkh)
            constexpr size_t kMaxTargetPartitionReprPerKey = 2;
            bool builtFromEvidence = false;
            if (dominantSourceKeyHash != 0 && dominantActionHash != 0) {
                const ApePairKey triggerPairKey{dominantSourceKeyHash, dominantActionHash};
                auto itPool = _apeEvidencePools.find(triggerPairKey);
                if (itPool != _apeEvidencePools.end()) {
                    std::vector<uintptr_t> partA;
                    partA.reserve(kMaxSourcePartitionRepr);
                    std::unordered_set<uintptr_t> seenSrc;
                    seenSrc.reserve(8);
                    struct TargetBucket {
                        int count{0};
                        std::vector<uintptr_t> repr;
                    };
                    std::unordered_map<uintptr_t, TargetBucket> buckets;
                    buckets.reserve(8);

                    itPool->second.forEach([&](const ApeEvidenceSample &s) {
                        if (!s.valid) {
                            return;
                        }
                        if (partA.size() < kMaxSourcePartitionRepr && hasXml(s.sourceStateHash)) {
                            if (seenSrc.insert(s.sourceStateHash).second) {
                                partA.push_back(s.sourceStateHash);
                            }
                        }
                        if (dominantTargetKeyHashes.count(s.targetKeyHash) == 0) {
                            return;
                        }
                        auto &b = buckets[s.targetKeyHash];
                        b.count++;
                        if (b.repr.size() < kMaxTargetPartitionReprPerKey && hasXml(s.targetStateHash)) {
                            bool dup = false;
                            for (uintptr_t h : b.repr) {
                                if (h == s.targetStateHash) {
                                    dup = true;
                                    break;
                                }
                            }
                            if (!dup) {
                                b.repr.push_back(s.targetStateHash);
                            }
                        }
                    });

                    if (partA.size() >= 1) {
                        predParts.push_back(std::move(partA));
                    }

                    size_t targetPartitionsAdded = 0;
                    std::vector<std::pair<uintptr_t, int>> targetOrder;
                    targetOrder.reserve(buckets.size());
                    for (const auto &kv : buckets) {
                        if (!kv.second.repr.empty()) {
                            targetOrder.emplace_back(kv.first, kv.second.count);
                        }
                    }
                    std::sort(targetOrder.begin(), targetOrder.end(),
                              [](const auto &a, const auto &b) {
                                  return a.second > b.second;
                              });
                    for (const auto &tc : targetOrder) {
                        if (targetPartitionsAdded >= kMaxTargetPartitionParts) {
                            break;
                        }
                        auto itB = buckets.find(tc.first);
                        if (itB == buckets.end() || itB->second.repr.empty()) {
                            continue;
                        }
                        predParts.push_back(itB->second.repr);
                        ++targetPartitionsAdded;
                    }

                    builtFromEvidence = predParts.size() >= 2;
                }
            }

            if (!builtFromEvidence) {
                predParts.clear();
                auto stateHashesMatchActivity = [&](uintptr_t sh) -> bool {
                    auto itS = stateByHash.find(sh);
                    if (itS == stateByHash.end() || !itS->second) {
                        return false;
                    }
                    auto ap = itS->second->getActivityString();
                    const std::string a =
                        (ap && ap.get()) ? naming::StateKey::canonicalActivityString(*ap) : std::string();
                    return a == actKey;
                };
                std::vector<uintptr_t> partA;
                if (dominantSourceKeyHash != 0) {
                    auto itSrcPart = stateHashesByKeyHash.find(dominantSourceKeyHash);
                    if (itSrcPart != stateHashesByKeyHash.end()) {
                        for (uintptr_t sh : itSrcPart->second) {
                            if (!hasXml(sh) || !stateHashesMatchActivity(sh)) {
                                continue;
                            }
                            partA.push_back(sh);
                            if (partA.size() >= kMaxSourcePartitionRepr) break;
                        }
                    }
                }
                if (partA.size() >= 1) {
                    predParts.push_back(std::move(partA));
                }

                size_t targetPartitionsAdded = 0;
                for (uintptr_t tkh : dominantTargetKeyHashes) {
                    if (targetPartitionsAdded >= kMaxTargetPartitionParts) {
                        break;
                    }
                    std::vector<uintptr_t> partT;
                    auto itTgtPart = stateHashesByKeyHash.find(tkh);
                    if (itTgtPart != stateHashesByKeyHash.end()) {
                        for (uintptr_t sh : itTgtPart->second) {
                            if (!hasXml(sh) || !stateHashesMatchActivity(sh)) {
                                continue;
                            }
                            partT.push_back(sh);
                            if (partT.size() >= kMaxTargetPartitionReprPerKey) break;
                        }
                    }
                    if (!partT.empty()) {
                        predParts.push_back(std::move(partT));
                        ++targetPartitionsAdded;
                    }
                }
            }

            if (predParts.size() >= 2) {
                pushApeSourcePartitionPredicate(activity, next, std::move(predParts));
            }
            if (dominantActionHash != 0) {
                // Java AssertActionDivergent2 partitions "resolved nodes" by the Name generated from @next.
                // Native does not store ModelAction.resolvedNodes directly, so we approximate them by:
                // 1) find the action target widget (matching dominantActionHash) for each source state;
                // 2) in the rebuilt GUI tree, collect all nodes whose bounds match that widget bounds;
                // 3) group collected nodes by their node name under @next.
                //
                // This is a closer analog to Java GUITree.pickNodes(action) than using one node position
                // per source state.
                std::unordered_map<std::string, std::vector<std::pair<uintptr_t, size_t>>> partsByName;
                // Java AssertActionDivergent2 is built from a single GUI tree's action.getResolvedNodes().
                // For full semantic alignment, only use one representative source tree in this predicate.
                constexpr size_t kMaxSrcForActionPred = 1;
                // No resolved-nodes truncation: keep all nodes matching the action target Name.
                constexpr size_t kMaxResolvedNodesPerSource = 4096;

                // dominantActionHash in native may include source-state hash, so matching it verbatim
                // across other source states could under-collect resolved nodes.
                // Extract a more stable identity (action type + target widget hash) from the first
                // state where the full hash matches, then match by that identity for other states.
                // Prefer evidence-driven source trees: those that actually executed (sourceKeyHash, actionSig).
                std::vector<uintptr_t> actionPredSourceStateHashes;
                actionPredSourceStateHashes.reserve(4);
                {
                    const ApePairKey triggerPairKey{dominantSourceKeyHash, dominantActionHash};
                    auto itPool = _apeEvidencePools.find(triggerPairKey);
                    if (itPool != _apeEvidencePools.end()) {
                        std::unordered_set<uintptr_t> seen;
                        seen.reserve(8);
                        itPool->second.forEach([&](const ApeEvidenceSample &s) {
                            if (actionPredSourceStateHashes.size() >= 4) {
                                return;
                            }
                            if (!s.valid || !hasXml(s.sourceStateHash)) {
                                return;
                            }
                            if (seen.insert(s.sourceStateHash).second) {
                                actionPredSourceStateHashes.push_back(s.sourceStateHash);
                            }
                        });
                    }
                }
                if (actionPredSourceStateHashes.empty()) {
                    actionPredSourceStateHashes = triggerSourceStateHashesForReplay;
                }

                ActionType dominantActionType = ActionType::NOP;
                uintptr_t dominantTargetWidgetHash = 0;
                bool hasDominantActionIdentity = false;
                for (uintptr_t repSh : actionPredSourceStateHashes) {
                    auto itRepSh = stateByHash.find(repSh);
                    if (itRepSh == stateByHash.end() || !itRepSh->second) {
                        continue;
                    }
                    const StatePtr &repSp = itRepSh->second;
                    for (const auto &a : repSp->getActions()) {
                        auto asa = std::dynamic_pointer_cast<ActivityStateAction>(a);
                        if (!asa || asa->hash() != dominantActionHash) {
                            continue;
                        }
                        dominantActionType = asa->getActionType();
                        if (asa->getTarget()) {
                            dominantTargetWidgetHash = asa->getTarget()->hash();
                            hasDominantActionIdentity = true;
                        }
                        break;
                    }
                    if (hasDominantActionIdentity) {
                        break;
                    }
                }

                size_t actionPredAdded = 0;
                for (uintptr_t sh : actionPredSourceStateHashes) {
                    if (actionPredAdded >= kMaxSrcForActionPred) {
                        break;
                    }
                    auto itSpLookup = stateByHash.find(sh);
                    if (itSpLookup == stateByHash.end() || !itSpLookup->second) {
                        continue;
                    }
                    const StatePtr &sp = itSpLookup->second;

                    // Locate the specific action target widget for @dominantActionHash.
                    WidgetPtr targetWidget;
                    for (const auto &a : sp->getActions()) {
                        auto asa = std::dynamic_pointer_cast<ActivityStateAction>(a);
                        if (!asa) {
                            continue;
                        }
                        bool match = false;
                        if (hasDominantActionIdentity && asa->getTarget()) {
                            match = (asa->getActionType() == dominantActionType &&
                                     asa->getTarget()->hash() == dominantTargetWidgetHash);
                        } else {
                            match = (asa->hash() == dominantActionHash);
                        }
                        if (!match) {
                            continue;
                        }
                        targetWidget = asa->getTarget();
                        break;
                    }
                    if (!targetWidget || !targetWidget->getBounds()) {
                        continue;
                    }
                    const Rect targetRect = *targetWidget->getBounds();

                    auto itXml = _apeStateXmlByStateHash.find(sh);
                    if (itXml == _apeStateXmlByStateHash.end() || itXml->second.empty()) {
                        continue;
                    }

                    std::string pkg;
                    std::string cls;
                    naming::StateKey::splitActivityPackageClass(activity, &pkg, &cls);
                    gui_tree::GUITreeBuildResult built =
                        gui_tree::GUITreeFactory::buildFromXml(itXml->second, pkg, cls);
                    if (!built.tree || !built.dom) {
                        continue;
                    }
                    std::vector<gui_tree::GUITreeNode *> po;
                    collectGUITreeNodesPreOrder(built.tree->getRootNode(), &po);
                    if (po.empty()) {
                        continue;
                    }

                    // Java's resolvedNodes are selected by action target Name computed before refinement
                    // (i.e., under @cur). To align more closely, rebuild once under @cur to derive a stable
                    // targetNameCurXPath, pick resolved node indices by (bounds match AND targetNameCur),
                    // then rebuild under @next and group those same indices by their new Name.
                    if (!naming::NamingFactory::rebuildTree(cur, *built.tree, built.dom)) {
                        continue;
                    }

                    std::string targetNameCurXPath;
                    // More strict: derive the action target Name under @cur from the specific targetWidget
                    // (when widget<->preorder index mapping is aligned). Fallback to bounds-match.
                    {
                        const WidgetPtrVec &ws = sp->getWidgets();
                        if (ws.size() == po.size()) {
                            for (size_t i = 0; i < ws.size(); ++i) {
                                if (ws[i] == targetWidget) {
                                    if (i < po.size() && po[i]) {
                                        const naming::NamePtr nm = po[i]->getXPathName();
                                        if (nm) {
                                            targetNameCurXPath = nm->toXPath();
                                        }
                                    }
                                    break;
                                }
                            }
                        }
                        // Fallback: derive the target Name under @cur from a node whose bounds match.
                        // This still yields the correct Name for resolved-nodes selection (we only use Name
                        // equality afterwards, not bounds).
                        if (targetNameCurXPath.empty()) {
                            for (size_t i = 0; i < po.size(); ++i) {
                                const auto *node = po[i];
                                if (!node) continue;
                                if (!(node->getBounds() == targetRect)) continue;
                                const naming::NamePtr nm = node->getXPathName();
                                if (!nm) continue;
                                targetNameCurXPath = nm->toXPath();
                                if (!targetNameCurXPath.empty()) {
                                    break;
                                }
                            }
                        }
                    }
                    // Strict mode: if we cannot derive the target Name under @cur,
                    // skip this source state for action partitions.
                    if (targetNameCurXPath.empty()) {
                        continue;
                    }

                    std::vector<size_t> resolvedIdx;
                    resolvedIdx.reserve(kMaxResolvedNodesPerSource);
                    for (size_t i = 0; i < po.size(); ++i) {
                        const auto *node = po[i];
                        if (!node) continue;
                        const naming::NamePtr nm = node->getXPathName();
                        if (!nm || nm->toXPath() != targetNameCurXPath) {
                            continue;
                        }
                        // Java includes all nodes with the same Name; no extra bounds constraint.
                        resolvedIdx.push_back(i);
                    }

                    if (resolvedIdx.empty()) {
                        continue;
                    }

                    // Rebuild with @next for grouping into partitions.
                    if (!naming::NamingFactory::rebuildTree(next, *built.tree, built.dom)) {
                        continue;
                    }

                    size_t insertedAny = 0;
                    for (size_t idx : resolvedIdx) {
                        if (idx >= po.size() || !po[idx]) {
                            continue;
                        }
                        const naming::NamePtr nm = po[idx]->getXPathName();
                        if (!nm) {
                            continue;
                        }
                        const std::string nameXPath = nm->toXPath();
                        auto &vec = partsByName[nameXPath];
                        vec.push_back({sh, idx});
                        insertedAny++;
                    }
                    if (insertedAny > 0) {
                        ++actionPredAdded;
                    }
                }

                if (partsByName.size() >= 2) {
                    std::vector<std::vector<std::pair<uintptr_t, size_t>>> actionPredParts;
                    actionPredParts.reserve(partsByName.size());
                    for (auto &kv : partsByName) {
                        if (!kv.second.empty()) {
                            actionPredParts.push_back(std::move(kv.second));
                        }
                    }
                    if (actionPredParts.size() >= 2) {
                        pushApeActionPartitionPredicate(activity, next, std::move(actionPredParts));
                    }
                }
            }
        }
#endif
        ctx.triggerTargetKeyHashes = std::move(xmlSpaceTriggerTargetKeyHashes);
        {
            const naming::NamingPtr nextPar = next ? next->getParent() : nullptr;
            if (next && nextPar.get() != cur.get()) {
                auto apeSnipFp = [](const naming::NamingPtr &p) -> std::string {
                    if (!p) {
                        return std::string("-");
                    }
                    const std::string &s = p->fingerprintString();
                    return s.size() > 140 ? s.substr(0, 140) + std::string("...") : s;
                };
                const std::string sCur = apeSnipFp(cur);
                const std::string sNext = apeSnipFp(next);
                const std::string sPar = apeSnipFp(nextPar);
                int curStrictAncNext = 0;
                if (cur && next) {
                    for (naming::NamingPtr x = next->getParent(); x; x = x->getParent()) {
                        if (x == cur) {
                            curStrictAncNext = 1;
                            break;
                        }
                    }
                }
                BDLOG(
                    "ape naming: refine pre_update NOT direct child: act=%s cur=%p next=%p next_parent=%p "
                    "srcKeyH=%lu actH=%lu trigExact=%d curFin=%d nextFin=%d cur_strict_anc_next=%d "
                    "cur_fp=%s next_fp=%s par_fp=%s",
                    actKey.c_str(), static_cast<const void *>(cur.get()),
                    static_cast<const void *>(next.get()), static_cast<const void *>(nextPar.get()),
                    static_cast<unsigned long>(dominantSourceKeyHash),
                    static_cast<unsigned long>(dominantActionHash), ctx.triggerSourceKeyExact ? 1 : 0,
                    cur ? cur->getFineness() : -1, next ? next->getFineness() : -1, curStrictAncNext,
                    sCur.c_str(), sNext.c_str(), sPar.c_str());
            }
        }
        if (ctx.triggerSourceKeyExact) {
            _apeStateNamingManager->updateNamingWithStateKey(
                actKey, naming::NamingUpdateKind::Refine, cur, next, ctx.triggerSourceKey);
        } else {
            _ape_correctness_counters.naming_update_by_hash++;
            _apeStateNamingManager->updateNamingWithStateHash(
                actKey, naming::NamingUpdateKind::Refine, cur, next, ctx.triggerSourceKeyHash);
        }
        invalidateApeGraphStateKeyDedupMap();
        std::vector<uintptr_t> repKeyHashes;
        std::unordered_set<uintptr_t> focusOldKeyHashes;
        std::unordered_set<uintptr_t> affectedTrees;
#if defined(FASTBOT_HAS_PUGIXML) && FASTBOT_HAS_PUGIXML && DYNAMIC_STATE_ABSTRACTION_ENABLED
        if (ctx.triggerSourceKeyHash != 0) {
            // Q8/P0: build affected set from naming mismatch (oldH != newH) instead of only trigger bucket.
            for (const auto &kv : _apeStateXmlByStateHash) {
                const uintptr_t sh = kv.first;
                const std::string &xml = kv.second;
                if (xml.empty()) {
                    continue;
                }
                auto itBucket = _ape_state_keys_by_hash.find(sh);
                if (itBucket == _ape_state_keys_by_hash.end() || itBucket->second.empty()) {
                    continue;
                }
                bool activityMatch = false;
                for (const auto &k : itBucket->second) {
                    if (k.activity() == actKey) {
                        activityMatch = true;
                        break;
                    }
                }
                if (!activityMatch) {
                    continue;
                }
                uintptr_t oldH = 0;
                uintptr_t newH = 0;
                if (!apeStateHashFromXmlWithTwoNamings(activity, xml, cur, &oldH, next, &newH)) {
                    continue;
                }
                if (oldH != newH) {
                    ctx.oldKeyHashToNewKeyHashes[oldH].insert(newH);
                    ctx.oldKeyHashToObservationCount[oldH]++;
                    focusOldKeyHashes.insert(oldH);
                    affectedTrees.insert(sh);
                }
            }
        }
#endif
        repKeyHashes.reserve(8);
        if (!focusOldKeyHashes.empty()) {
            for (uintptr_t h : focusOldKeyHashes) {
                repKeyHashes.push_back(h);
                if (repKeyHashes.size() >= 8) {
                    break;
                }
            }
        } else if (ctx.triggerSourceKeyHash != 0) {
            repKeyHashes.push_back(ctx.triggerSourceKeyHash);
        }
        if (!repKeyHashes.empty()) {
            rebuildApeStateRepresentativesForKeyHashes(activity, cur, repKeyHashes, 1);
        }
        if (!focusOldKeyHashes.empty()) {
            remapApeTransitionAggregationForActivity(activity, cur, next, &focusOldKeyHashes);
        }
        pruneStaleApeStatesForActivity(actKey, ctx.previousNamingFingerprintBeforeRefine,
                                       affectedTrees.empty() ? nullptr : &affectedTrees);
        BLOG("ape naming: refine activity=%s", activity.c_str());
        {
            const std::string nextFpOk = next ? next->fingerprintString() : std::string("-");
            BDLOG("ape naming: refine success activity=%s nextFin=%d focusOldKeys=%zu affectedTrees=%zu "
                  "repKeys=%zu next_fp=%s",
                  activity.c_str(), next ? next->getFineness() : -1, focusOldKeyHashes.size(),
                  affectedTrees.size(), repKeyHashes.size(), nextFpOk.c_str());
        }
        return true;
    }

    void Model::invalidateApeGraphStateKeyDedupMap() {
        _ape_graph_state_by_key.clear();
    }

#if DYNAMIC_STATE_ABSTRACTION_ENABLED
    void Model::rebuildApeStateRepresentativesForKeyHashes(
        const std::string &rawActivity,
        const naming::NamingPtr &oldNaming,
        const std::vector<uintptr_t> &oldKeyHashes,
        size_t maxStatesPerKeyHash) {
#if !defined(FASTBOT_HAS_PUGIXML) || !FASTBOT_HAS_PUGIXML
        (void)rawActivity;
        (void)oldNaming;
        (void)oldKeyHashes;
        (void)maxStatesPerKeyHash;
#else
        if (!_graph || !_preference || oldKeyHashes.empty() || !oldNaming || maxStatesPerKeyHash == 0) {
            return;
        }
        const std::string actKey = naming::StateKey::canonicalActivityString(rawActivity);
        const bool wantApeRlIdentity = !_preference->useStaticReuseAbstraction();
        if (!wantApeRlIdentity) {
            return;
        }
        AbstractAgentPtr agent = getOrCreateAgent(ModelConstants::DefaultDeviceID);
        if (!agent) {
            return;
        }
        stringPtr activityPtr = getOrCreateActivityPtr(rawActivity);

        auto rebuildOneXml = [&](const std::string &xml) {
            if (xml.empty()) {
                return;
            }
            ElementPtr elem = Element::createFromXml(xml);
            if (!elem) {
                return;
            }
            StatePtr built = buildStateOnly(elem, agent, activityPtr);
            if (!built) {
                return;
            }
            naming::StateKey apeKey = naming::StateKey::fromParts(
                naming::StateKey::canonicalActivityString(rawActivity), nullptr, {});
            ApeStateKeyBuildFailReason failReason = ApeStateKeyBuildFailReason::None;
            const bool haveApeKey = buildApeStateKeyFromElementTree(
                elem, rawActivity, &apeKey, &failReason, wantApeRlIdentity ? built : StatePtr());
            if (haveApeKey) {
                built->applyDynamicAbstractionIdentityHash(apeKey.hash());
            }
            StatePtr canonical = built;
            if (haveApeKey && _preference->useApeGraphDedupByStateKey()) {
                const uintptr_t kh = apeKey.hash();
                bool deduped = false;
                auto &bucket = _ape_graph_state_by_key[kh];
                for (const auto &entry : bucket) {
                    if (entry.key == apeKey) {
                        _ape_correctness_counters.graph_dedup_exact_hit++;
                        _graph->recordStateVisit(entry.state, built);
                        canonical = entry.state;
                        deduped = true;
                        break;
                    }
                }
                if (!deduped) {
                    if (!bucket.empty()) {
                        _ape_correctness_counters.graph_dedup_hash_collision++;
                    }
                    canonical = _graph->addState(built);
                    bucket.push_back(ApeGraphStateKeyDedupEntry{apeKey, canonical});
                } else {
                    _ape_correctness_counters.graph_dedup_hash_hit++;
                }
            } else {
                canonical = _graph->addState(built);
            }
            if (haveApeKey) {
                recordApeStateKey(canonical, apeKey);
            }
            if (_preference->useApeNamingCandidateTransitionReplay()) {
                _apeStateXmlByStateHash[canonical->hash()] = xml;
            }
        };

        auto keyHashSet = std::unordered_set<uintptr_t>(oldKeyHashes.begin(), oldKeyHashes.end());
        std::unordered_map<uintptr_t, size_t> rebuiltPerKey;
        std::unordered_set<uintptr_t> satisfiedKeys;
        std::vector<uintptr_t> stateHashesToRebuild;
        stateHashesToRebuild.reserve(16);
        for (const auto &kv : _apeStateXmlByStateHash) {
            if (satisfiedKeys.size() >= keyHashSet.size()) {
                break;
            }
            const uintptr_t stateHash = kv.first;
            const std::string &xml = kv.second;
            if (xml.empty()) {
                continue;
            }
            naming::StateKey storedKey = naming::StateKey::fromFallbackXmlStringHash("", 0);
            if (!tryGetApeStateKey(stateHash, &storedKey)) {
                continue;
            }
            if (storedKey.activity() != actKey) {
                continue;
            }
            uintptr_t oldH = 0;
            if (!apeStateHashFromXmlWithNaming(rawActivity, xml, oldNaming, &oldH)) {
                continue;
            }
            if (keyHashSet.count(oldH) == 0) {
                continue;
            }
            size_t &n = rebuiltPerKey[oldH];
            if (n >= maxStatesPerKeyHash) {
                satisfiedKeys.insert(oldH);
                continue;
            }
            stateHashesToRebuild.push_back(stateHash);
            ++n;
            if (n >= maxStatesPerKeyHash) {
                satisfiedKeys.insert(oldH);
            }
        }
        for (uintptr_t sh : stateHashesToRebuild) {
            auto it = _apeStateXmlByStateHash.find(sh);
            if (it == _apeStateXmlByStateHash.end() || it->second.empty()) {
                continue;
            }
            rebuildOneXml(it->second);
        }
#endif
    }

    void Model::remapApeTransitionAggregationForActivity(
        const std::string &rawActivity,
        const naming::NamingPtr &fromNaming,
        const naming::NamingPtr &toNaming,
        const std::unordered_set<uintptr_t> *focusOldKeyHashes) {
#if !defined(FASTBOT_HAS_PUGIXML) || !FASTBOT_HAS_PUGIXML
        (void)rawActivity;
        (void)fromNaming;
        (void)toNaming;
#else
        if (rawActivity.empty() || !fromNaming || !toNaming) {
            return;
        }
        const std::string actKey = naming::StateKey::canonicalActivityString(rawActivity);
        const std::string &toFp = toNaming->fingerprintString();
        const bool hasFocus = (focusOldKeyHashes && !focusOldKeyHashes->empty());

        // Evidence pools are keyed by current (sourceKeyHash, actionSignature).
        // Since remap mutates hashes in-place, drop old-space pools for this activity and rebuild
        // from the remapped transition log.
        if (!_apeEvidencePools.empty()) {
            for (const auto &slot : _apeTransitionLog) {
                if (slot.valid && slot.sourceActivity == actKey) {
                    _apeEvidencePools.erase(ApePairKey{slot.sourceKeyHash, slot.actionHash});
                }
            }
        }

        uint32_t fullMask = 0;
        for (auto t : naming::namerTypesUsed()) {
            fullMask |= (1u << static_cast<unsigned>(t));
        }
        naming::NamerPtr fullNamer = naming::NamerFactory::current().getByMask(fullMask);

        // Build keyHash -> representative stateHash map once (avoid O(|states|*|transitions|) scan).
        std::unordered_map<uintptr_t, uintptr_t> representativeStateHashByKeyHash;
        representativeStateHashByKeyHash.reserve(256);
        for (const auto &kv : _ape_state_keys_by_hash) {
            const uintptr_t sh = kv.first;
            for (const auto &k : kv.second) {
                if (k.activity() == actKey && k.namingFingerprint() == toFp) {
                    representativeStateHashByKeyHash.emplace(k.hash(), sh);
                }
            }
        }
        auto findRepresentativeStateHash = [&](uintptr_t keyHash) -> uintptr_t {
            auto it = representativeStateHashByKeyHash.find(keyHash);
            return it == representativeStateHashByKeyHash.end() ? 0 : it->second;
        };

        std::string pkg;
        std::string cls;
        naming::StateKey::splitActivityPackageClass(rawActivity, &pkg, &cls);

        struct RemapTreeCacheEntry {
            gui_tree::GUITreeBuildResult built;
            std::vector<gui_tree::GUITreeNode *> preorder;
            std::vector<uintptr_t> fullPathHashes;
            std::vector<uintptr_t> xpathHashes;
            std::vector<Rect> bounds;
            bool ready{false};
            bool hasStateKey{false};
            naming::StateKey stateKey = naming::StateKey::fromFallbackXmlStringHash("", 0);
        };

        std::unordered_map<uintptr_t, RemapTreeCacheEntry> treeCache;
        treeCache.reserve(64);
        std::unordered_map<uintptr_t, uintptr_t> keyHashCache;
        keyHashCache.reserve(128);

        auto getTreeEntry = [&](uintptr_t stateHash, const std::string &xml,
                                RemapTreeCacheEntry **out) -> bool {
            if (!out || stateHash == 0 || xml.empty()) {
                return false;
            }
            auto it = treeCache.find(stateHash);
            if (it != treeCache.end()) {
                *out = &it->second;
                return it->second.ready;
            }
            RemapTreeCacheEntry entry;
            entry.built = gui_tree::GUITreeFactory::buildFromXml(xml, pkg, cls);
            if (!entry.built.tree || !entry.built.dom) {
                treeCache.emplace(stateHash, std::move(entry));
                *out = &treeCache.find(stateHash)->second;
                return false;
            }
            if (!naming::NamingFactory::rebuildTree(toNaming, *entry.built.tree, entry.built.dom)) {
                treeCache.emplace(stateHash, std::move(entry));
                *out = &treeCache.find(stateHash)->second;
                return false;
            }
            collectGUITreeNodesPreOrder(entry.built.tree->getRootNode(), &entry.preorder);
            const size_t n = entry.preorder.size();
            entry.fullPathHashes.assign(n, 0);
            entry.xpathHashes.assign(n, 0);
            entry.bounds.resize(n);
            for (size_t i = 0; i < n; ++i) {
                auto *node = entry.preorder[i];
                if (!node) {
                    continue;
                }
                entry.bounds[i] = node->getBounds();
                if (auto nxp = node->getXPathName(); nxp) {
                    const std::string xp = nxp->toXPath();
                    if (!xp.empty()) {
                        entry.xpathHashes[i] = fastStringHash(xp);
                    }
                }
                if (fullNamer) {
                    std::string fullKey = fullNamer->xpathKeyForNode(*node);
                    if (fullKey.empty()) {
                        if (naming::NamePtr fullName = fullNamer->naming(*node); fullName) {
                            fullKey = fullName->toXPath();
                        }
                    }
                    if (!fullKey.empty()) {
                        entry.fullPathHashes[i] = fastStringHash(fullKey);
                    }
                }
            }
            entry.ready = true;
            treeCache.emplace(stateHash, std::move(entry));
            *out = &treeCache.find(stateHash)->second;
            return true;
        };

        auto getStateKeyHashUnderToNaming = [&](uintptr_t stateHash, const std::string &xml,
                                                uintptr_t *out) -> bool {
            if (!out || stateHash == 0) {
                return false;
            }
            auto it = keyHashCache.find(stateHash);
            if (it != keyHashCache.end()) {
                *out = it->second;
                return true;
            }
            RemapTreeCacheEntry *entry = nullptr;
            if (!getTreeEntry(stateHash, xml, &entry) || !entry || !entry->built.tree) {
                return false;
            }
            const uintptr_t h = naming::StateKey::hashFromGUITree(*entry->built.tree);
            keyHashCache.emplace(stateHash, h);
            *out = h;
            return true;
        };

        auto computeActionHash = [&](uintptr_t sourceStateHash, const std::string &xml,
                                     const ApeTransitionEntry &e, uintptr_t *out) -> bool {
            if (!out) {
                return false;
            }
            const uintptr_t activityH = fastStringHash(actKey);
            uintptr_t abstractTargetHash = 0x1;
            if (e.hasTargetFullPath || e.hasTargetBounds) {
                RemapTreeCacheEntry *entry = nullptr;
                if (getTreeEntry(sourceStateHash, xml, &entry) && entry && entry->ready) {
                    size_t matchedIdx = static_cast<size_t>(-1);
                    if (e.hasTargetFullPath && fullNamer) {
                        size_t bestIdx = static_cast<size_t>(-1);
                        long bestDist = 0;
                        bool haveBest = false;
                        for (size_t i = 0; i < entry->preorder.size(); ++i) {
                            if (entry->fullPathHashes[i] == 0 || entry->fullPathHashes[i] != e.targetFullPathHash) {
                                continue;
                            }
                            if (!e.hasTargetBounds) {
                                matchedIdx = i;
                                break;
                            }
                            const Rect &b = entry->bounds[i];
                            const long cx = static_cast<long>(b.left + (b.right - b.left) / 2);
                            const long cy = static_cast<long>(b.top + (b.bottom - b.top) / 2);
                            const long tx = static_cast<long>(
                                e.targetBounds.left + (e.targetBounds.right - e.targetBounds.left) / 2);
                            const long ty = static_cast<long>(
                                e.targetBounds.top + (e.targetBounds.bottom - e.targetBounds.top) / 2);
                            const long dx = (cx > tx) ? (cx - tx) : (tx - cx);
                            const long dy = (cy > ty) ? (cy - ty) : (ty - cy);
                            const long dist = dx + dy;
                            if (!haveBest || dist < bestDist) {
                                bestIdx = i;
                                bestDist = dist;
                                haveBest = true;
                            }
                        }
                        if (matchedIdx == static_cast<size_t>(-1) && haveBest) {
                            matchedIdx = bestIdx;
                        }
                    }
                    if (matchedIdx == static_cast<size_t>(-1) && e.hasTargetBounds) {
                        for (size_t i = 0; i < entry->preorder.size(); ++i) {
                            if (entry->bounds[i] == e.targetBounds) {
                                matchedIdx = i;
                                break;
                            }
                        }
                    }
                    if (matchedIdx != static_cast<size_t>(-1)) {
                        const uintptr_t xpH = entry->xpathHashes[matchedIdx];
                        if (xpH != 0) {
                            abstractTargetHash = xpH;
                        }
                    }
                }
            }
            const uintptr_t actionTypeH = std::hash<int>{}(static_cast<int>(e.actionType));
            *out = 0x9e3779b9 + (activityH << 2) ^ (((actionTypeH << 6) ^ (abstractTargetHash << 1)) << 1);
            return true;
        };

        // Pre-build stateHash -> xmlKeyHash(fromNaming) cache so the focus check can compare
        // XML-space hashes consistently, even when slot.sourceKeyHash is still Element-space.
        std::unordered_map<uintptr_t, uintptr_t> fromNamingKeyHashCache;
        if (hasFocus) {
            for (const auto &kv : _apeStateXmlByStateHash) {
                if (kv.second.empty()) {
                    continue;
                }
                uintptr_t h = 0;
                if (apeStateHashFromXmlWithNaming(rawActivity, kv.second, fromNaming, &h) && h != 0) {
                    fromNamingKeyHashCache.emplace(kv.first, h);
                }
            }
        }

        for (auto &slot : _apeTransitionLog) {
            if (!slot.valid || slot.sourceActivity != actKey) {
                continue;
            }
            // Q8 (affectedTrees): when focus is provided, only remap transition entries that
            // reference affected StateKey hashes. Keep other entries intact to avoid losing evidence.
            if (hasFocus) {
                auto xmlKeyForState = [&](uintptr_t stateHash, uintptr_t storedKeyHash) -> uintptr_t {
                    auto it = fromNamingKeyHashCache.find(stateHash);
                    return (it != fromNamingKeyHashCache.end()) ? it->second : storedKeyHash;
                };
                const uintptr_t srcXml = xmlKeyForState(slot.sourceStateHash, slot.sourceKeyHash);
                const uintptr_t tgtXml = xmlKeyForState(slot.targetStateHash, slot.targetKeyHash);
                const bool inFocus = (focusOldKeyHashes->count(srcXml) != 0 ||
                                      focusOldKeyHashes->count(tgtXml) != 0);
                if (!inFocus) {
                    apeEvidencePoolAdd(ApePairKey{slot.sourceKeyHash, slot.actionHash}, slot);
                    continue;
                }
            }
            // Remove old-space aggregation first so that any remap failure won't leave stale counts.
            // Also avoid clearing pairAgg for other activities (APE Model.rebuild keeps unaffected evidence).
            apePairAggRemove(slot);
            const uintptr_t oldSrcStateHash = slot.sourceStateHash;
            const uintptr_t oldTgtStateHash = slot.targetStateHash;
            auto itSx = _apeStateXmlByStateHash.find(oldSrcStateHash);
            auto itTx = _apeStateXmlByStateHash.find(oldTgtStateHash);
            if (itSx == _apeStateXmlByStateHash.end() || itTx == _apeStateXmlByStateHash.end() ||
                itSx->second.empty() || itTx->second.empty()) {
                slot.valid = false;
                continue;
            }
            uintptr_t newSrcKeyHash = 0;
            uintptr_t newTgtKeyHash = 0;
            if (!getStateKeyHashUnderToNaming(oldSrcStateHash, itSx->second, &newSrcKeyHash) ||
                !getStateKeyHashUnderToNaming(oldTgtStateHash, itTx->second, &newTgtKeyHash)) {
                slot.valid = false;
                continue;
            }
            uintptr_t newActionHash = 0;
            if (!computeActionHash(oldSrcStateHash, itSx->second, slot, &newActionHash)) {
                slot.valid = false;
                continue;
            }
            slot.sourceKeyHash = newSrcKeyHash;
            slot.targetKeyHash = newTgtKeyHash;
            slot.actionHash = newActionHash;
            // Try to preserve exact StateKey after naming remap (avoid hash-only refine/rollback).
            slot.hasSourceStateKey = false;
            slot.sourceStateKey = naming::StateKey::fromFallbackXmlStringHash("", 0);
            slot.hasTargetStateKey = false;
            slot.targetStateKey = naming::StateKey::fromFallbackXmlStringHash("", 0);
            {
                RemapTreeCacheEntry *entry = nullptr;
                if (getTreeEntry(oldSrcStateHash, itSx->second, &entry) && entry && entry->built.tree) {
                    if (!entry->hasStateKey) {
                        entry->stateKey = naming::StateKey::fromGUITree(*entry->built.tree);
                        entry->hasStateKey = true;
                    }
                    slot.sourceStateKey = entry->stateKey;
                    slot.hasSourceStateKey = true;
                }
            }
            {
                RemapTreeCacheEntry *entry = nullptr;
                if (getTreeEntry(oldTgtStateHash, itTx->second, &entry) && entry && entry->built.tree) {
                    if (!entry->hasStateKey) {
                        entry->stateKey = naming::StateKey::fromGUITree(*entry->built.tree);
                        entry->hasStateKey = true;
                    }
                    slot.targetStateKey = entry->stateKey;
                    slot.hasTargetStateKey = true;
                }
            }
            {
                const uintptr_t repSrc = findRepresentativeStateHash(newSrcKeyHash);
                slot.sourceStateHash = (repSrc != 0) ? repSrc : oldSrcStateHash;
            }
            {
                const uintptr_t repTgt = findRepresentativeStateHash(newTgtKeyHash);
                slot.targetStateHash = (repTgt != 0) ? repTgt : oldTgtStateHash;
            }
            // Re-add into aggregation under the new key space.
            apePairAggAdd(slot);
            if (slot.valid) {
                apeEvidencePoolAdd(ApePairKey{slot.sourceKeyHash, slot.actionHash}, slot);
            }
        }
#endif
    }

    bool Model::evalApeSourcePartitionPredicates(const std::string &activity,
                                                 const naming::NamingPtr &naming) const {
#if !defined(FASTBOT_HAS_PUGIXML) || !FASTBOT_HAS_PUGIXML
        (void)activity;
        (void)naming;
        return true;
#else
        if (!naming) {
            return true;
        }
        const std::string ak = naming::StateKey::canonicalActivityString(activity);
        for (const auto &p : _apeSourcePartitionPredicates) {
            if (p.activityKey != ak) {
                continue;
            }
            if (!evalApeSourcePartitionPredicateImpl(_apeStateXmlByStateHash, p.activityKey, activity,
                                                     naming, p.partitions)) {
                return false;
            }
        }
        return true;
#endif
    }

    void Model::pushApeSourcePartitionPredicate(const std::string &activity,
                                               const naming::NamingPtr &updatedNaming,
                                               std::vector<std::vector<uintptr_t>> partitions) {
#if !defined(FASTBOT_HAS_PUGIXML) || !FASTBOT_HAS_PUGIXML
        (void)activity;
        (void)updatedNaming;
        (void)partitions;
#else
        if (!updatedNaming || partitions.size() < 2) {
            return;
        }
        ApeSourcePartitionPredicate p;
        p.activityKey = naming::StateKey::canonicalActivityString(activity);
        p.updatedNamingFingerprint = updatedNaming->fingerprintString();
        p.partitions = std::move(partitions);
        _apeSourcePartitionPredicates.push_back(std::move(p));
#endif
    }

    void Model::pruneApeSourcePartitionPredicates(const std::string &activity,
                                                  const naming::NamingPtr &namingPrev,
                                                  const naming::NamingPtr &namingCur,
                                                  const std::unordered_set<uintptr_t> &affectedStateHashes) {
#if !defined(FASTBOT_HAS_PUGIXML) || !FASTBOT_HAS_PUGIXML
        (void)activity;
        (void)namingPrev;
        (void)namingCur;
        (void)affectedStateHashes;
#else
        if (!namingPrev || !namingCur) {
            return;
        }
        const std::string ak = naming::StateKey::canonicalActivityString(activity);
        _apeSourcePartitionPredicates.erase(
            std::remove_if(_apeSourcePartitionPredicates.begin(), _apeSourcePartitionPredicates.end(),
                           [&](const ApeSourcePartitionPredicate &pred) {
                               if (pred.activityKey != ak) {
                                   return false;
                               }
                               return !evalApeSourcePartitionPredicateImplTwoNamings(
                                   _apeStateXmlByStateHash, pred.activityKey, activity, namingPrev,
                                   namingCur, pred.partitions, affectedStateHashes);
                           }),
            _apeSourcePartitionPredicates.end());
#endif
    }

    bool Model::evalApeActionPartitionPredicates(const std::string &activity,
                                                 const naming::NamingPtr &naming) const {
#if !defined(FASTBOT_HAS_PUGIXML) || !FASTBOT_HAS_PUGIXML
        (void)activity;
        (void)naming;
        return true;
#else
        if (!naming) {
            return true;
        }
        const std::string ak = naming::StateKey::canonicalActivityString(activity);
        for (const auto &p : _apeActionPartitionPredicates) {
            if (p.activityKey != ak) {
                continue;
            }
            if (!evalApeActionPartitionPredicateImpl(_apeStateXmlByStateHash, p.activityKey, activity,
                                                     naming, p.partitions)) {
                return false;
            }
        }
        return true;
#endif
    }

    void Model::pushApeActionPartitionPredicate(const std::string &activity,
                                               const naming::NamingPtr &updatedNaming,
                                               std::vector<std::vector<std::pair<uintptr_t, size_t>>> partitions) {
#if !defined(FASTBOT_HAS_PUGIXML) || !FASTBOT_HAS_PUGIXML
        (void)activity;
        (void)updatedNaming;
        (void)partitions;
#else
        if (!updatedNaming || partitions.size() < 2) {
            return;
        }
        ApeActionPartitionPredicate p;
        p.activityKey = naming::StateKey::canonicalActivityString(activity);
        p.updatedNamingFingerprint = updatedNaming->fingerprintString();
        p.partitions = std::move(partitions);
        _apeActionPartitionPredicates.push_back(std::move(p));
#endif
    }

    void Model::pruneApeActionPartitionPredicates(const std::string &activity,
                                                  const naming::NamingPtr &namingPrev,
                                                  const naming::NamingPtr &namingCur,
                                                  const std::unordered_set<uintptr_t> &affectedStateHashes) {
#if !defined(FASTBOT_HAS_PUGIXML) || !FASTBOT_HAS_PUGIXML
        (void)activity;
        (void)namingPrev;
        (void)namingCur;
        (void)affectedStateHashes;
#else
        if (!namingPrev || !namingCur) {
            return;
        }
        const std::string ak = naming::StateKey::canonicalActivityString(activity);
        _apeActionPartitionPredicates.erase(
            std::remove_if(_apeActionPartitionPredicates.begin(), _apeActionPartitionPredicates.end(),
                           [&](const ApeActionPartitionPredicate &pred) {
                               if (pred.activityKey != ak) {
                                   return false;
                               }
                               return !evalApeActionPartitionPredicateImplTwoNamings(
                                   _apeStateXmlByStateHash, pred.activityKey, activity, namingPrev,
                                   namingCur, pred.partitions, affectedStateHashes);
                           }),
            _apeActionPartitionPredicates.end());
#endif
    }

    bool Model::evalApeStatesFewerThanPredicates(const std::string &activity,
                                                 const naming::NamingPtr &naming) const {
#if !defined(FASTBOT_HAS_PUGIXML) || !FASTBOT_HAS_PUGIXML
        (void)activity;
        (void)naming;
        return true;
#else
        if (!naming) {
            return true;
        }
        const std::string ak = naming::StateKey::canonicalActivityString(activity);
        for (const auto &p : _apeStatesFewerThanPredicates) {
            if (p.activityKey != ak) {
                continue;
            }
            if (!evalApeStatesFewerThanPredicateImpl(_apeStateXmlByStateHash, p.activityKey, activity,
                                                     naming, p.stateHashes, p.threshold)) {
                return false;
            }
        }
        return true;
#endif
    }

    void Model::pushApeStatesFewerThanPredicate(const std::string &activity,
                                               const naming::NamingPtr &updatedNaming, int threshold,
                                               std::vector<uintptr_t> stateHashes) {
#if !defined(FASTBOT_HAS_PUGIXML) || !FASTBOT_HAS_PUGIXML
        (void)activity;
        (void)updatedNaming;
        (void)threshold;
        (void)stateHashes;
#else
        if (!updatedNaming || stateHashes.empty() || threshold < 1) {
            return;
        }
        ApeStatesFewerThanPredicate p;
        p.activityKey = naming::StateKey::canonicalActivityString(activity);
        p.updatedNamingFingerprint = updatedNaming->fingerprintString();
        p.threshold = threshold;
        p.stateHashes = std::move(stateHashes);
        _apeStatesFewerThanPredicates.push_back(std::move(p));
#endif
    }

    void Model::pruneApeStatesFewerThanPredicates(const std::string &activity,
                                                 const naming::NamingPtr &namingPrev,
                                                 const naming::NamingPtr &namingCur,
                                                 const std::unordered_set<uintptr_t> &affectedStateHashes) {
#if !defined(FASTBOT_HAS_PUGIXML) || !FASTBOT_HAS_PUGIXML
        (void)activity;
        (void)namingPrev;
        (void)namingCur;
        (void)affectedStateHashes;
#else
        if (!namingPrev || !namingCur) {
            return;
        }
        const std::string ak = naming::StateKey::canonicalActivityString(activity);
        _apeStatesFewerThanPredicates.erase(
            std::remove_if(_apeStatesFewerThanPredicates.begin(), _apeStatesFewerThanPredicates.end(),
                           [&](const ApeStatesFewerThanPredicate &pred) {
                               if (pred.activityKey != ak) {
                                   return false;
                               }
                               return !evalApeStatesFewerThanPredicateImplTwoNamings(
                                   _apeStateXmlByStateHash, pred.activityKey, activity, namingPrev,
                                   namingCur, pred.stateHashes, pred.threshold, affectedStateHashes);
                           }),
            _apeStatesFewerThanPredicates.end());
#endif
    }

    void Model::apeCapGuiTreeNamingBlacklist() {
        // no-op: match Java unbounded guiTreeNamingBlaclist.
    }
#endif

    bool Model::coarsenActivityApeNamingIfNeeded(const std::string &activity) {
        const std::string actKey = naming::StateKey::canonicalActivityString(activity);
        auto it = _apeNamingContext.find(actKey);
        if (it == _apeNamingContext.end()) {
            BDLOG("ape naming: coarsen skip activity=%s reason=no_ape_naming_context", activity.c_str());
            return false;
        }
        ApeNamingAbstractionContext &ctx = it->second;
        naming::ActivityNamingManager &mgr2 = _apeStateNamingManager->activityManager();
        naming::NamingPtr cur = mgr2.getNaming(actKey);
        naming::NamingPtr prev = ctx.previousNamingBeforeRefine;
        if (!cur || !prev) {
            BDLOG("ape naming: coarsen skip activity=%s reason=missing_naming cur=%p prev=%p",
                  activity.c_str(), static_cast<const void *>(cur.get()),
                  static_cast<const void *>(prev.get()));
            return false;
        }
        size_t affectedStateObservations = 0;
        for (const auto &p : ctx.oldKeyHashToObservationCount) {
            affectedStateObservations += p.second;
        }
        std::unordered_set<uintptr_t> totalNewKeys;
        bool overSplit = false;
        for (const auto &p : ctx.oldKeyHashToNewKeyHashes) {
            totalNewKeys.insert(p.second.begin(), p.second.end());
            if (p.second.size() > static_cast<size_t>(BetaMaxSplitCount)) {
                overSplit = true;
            }
        }
        // Java batchAbstract-inspired global rollback gates:
        // 1) affected old states threshold; 2) resulting refined targets threshold by fineness.
        const int affectedThreshold = 8;
        const int totalTypes = static_cast<int>(naming::namerTypesUsed().size());
        const int fineness = cur->getFineness();
        const int shift = std::max(0, totalTypes - fineness);
        const int targetThreshold = std::min(8, std::max(1, 2 << shift));
        const bool overAffected = affectedStateObservations > static_cast<size_t>(affectedThreshold);
        const bool overTargets = totalNewKeys.size() > static_cast<size_t>(targetThreshold);
        // Java batchAbstract filterTargets-like gate: focus on trigger source-key bucket.
        const uintptr_t triggerSource = ctx.triggerSourceKeyHash;
        size_t filteredAffected = 0;
        size_t filteredTargets = 0;
        // optimization 4 (align Java): recompute affectedStates/targets.size with the same
        // originState.equals(oldState) filtering semantics used by optimization 3.
#if defined(FASTBOT_HAS_PUGIXML) && FASTBOT_HAS_PUGIXML && DYNAMIC_STATE_ABSTRACTION_ENABLED
        {
            bool computed = false;
            if (triggerSource != 0) {
                std::unordered_set<uintptr_t> distinctTargetKeys;
                std::unordered_set<uintptr_t> affectedStateHashesForPrune;
                for (const auto &sp : getGraph()->getStates()) {
                    if (!sp) {
                        continue;
                    }
                    auto apBa = sp->getActivityString();
                    const std::string aBa = (apBa && apBa.get())
                                                ? naming::StateKey::canonicalActivityString(*apBa)
                                                : std::string();
                    if (aBa != actKey) {
                        continue;
                    }
                    const uintptr_t ghBa = sp->hash();
                    uintptr_t storedKeyH = 0;
                    const bool haveStoredApeKey = tryGetApeStateKeyHash(ghBa, &storedKeyH);
                    const uintptr_t khBa = haveStoredApeKey ? storedKeyH : ghBa;

                    // Approximate Java targetStates membership by checking whether this state belongs to
                    // targetNaming (`cur`) key space (i.e., its StateKey under `cur` is in `totalNewKeys`).
                    auto itXml = _apeStateXmlByStateHash.find(ghBa);
                    const bool haveXml = (itXml != _apeStateXmlByStateHash.end() && !itXml->second.empty());
                    // optimization 6: AssertStatesFewerThan.eval() skips missing XML.
                    // To keep gate/affected aligned with predicate inputs, exclude states without XML.
                    if (!haveXml) {
                        continue;
                    }
                    uintptr_t tgtKeyHash = 0;
                    uintptr_t oldH = 0;
                    apeStateKeyPairFromXmlCoarsenPath(activity, itXml->second, cur, &tgtKeyHash, prev, &oldH);

                    if (!totalNewKeys.empty()) {
                        if (tgtKeyHash == 0 || totalNewKeys.count(tgtKeyHash) == 0) {
                            continue;
                        }
                    }

                    const bool affectedBa = (oldH == triggerSource);

                    if (affectedBa) {
                        filteredAffected++;
                        affectedStateHashesForPrune.insert(ghBa);
                        // Java targets = GUITreeBuilder.getStateKey(targetNaming, tree) for all trees in affected states.
                        if (tgtKeyHash != 0) {
                            distinctTargetKeys.insert(tgtKeyHash);
                        } else if (haveStoredApeKey) {
                            distinctTargetKeys.insert(khBa);
                        }
                    }
                }
                filteredTargets = distinctTargetKeys.size();
                computed = true;
            }
            if (!computed) {
                auto itFiltered = ctx.oldKeyHashToNewKeyHashes.find(triggerSource);
                if (itFiltered != ctx.oldKeyHashToNewKeyHashes.end()) {
                    auto itCnt = ctx.oldKeyHashToObservationCount.find(triggerSource);
                    filteredAffected = (itCnt == ctx.oldKeyHashToObservationCount.end()) ? 0 : itCnt->second;
                    filteredTargets = itFiltered->second.size();
                }
            }
        }
#else
        {
            auto itFiltered = ctx.oldKeyHashToNewKeyHashes.find(triggerSource);
            if (itFiltered != ctx.oldKeyHashToNewKeyHashes.end()) {
                auto itCnt = ctx.oldKeyHashToObservationCount.find(triggerSource);
                filteredAffected = (itCnt == ctx.oldKeyHashToObservationCount.end()) ? 0 : itCnt->second;
                filteredTargets = itFiltered->second.size();
            }
        }
#endif

        const bool overFilteredAffected = filteredAffected > static_cast<size_t>(affectedThreshold);
        const bool overFilteredTargets = filteredTargets > static_cast<size_t>(targetThreshold);

        // Pair-driven effectiveness check: if trigger source is observed but still unsplit
        // and trigger targets remain divergent after refinement, rollback.
        bool unresolvedTriggerPair = false;
        size_t postRefineMaxFanoutForAction = 0;
        auto itFiltered = ctx.oldKeyHashToNewKeyHashes.find(triggerSource);
        if (triggerSource != 0 && itFiltered != ctx.oldKeyHashToNewKeyHashes.end()) {
            const bool sourceSplit = itFiltered->second.size() > 1;
            std::unordered_set<uintptr_t> mappedTargetUnion;
            size_t coveredOldTargets = 0;
            for (auto oldT : ctx.triggerTargetKeyHashes) {
                auto itOld = ctx.oldKeyHashToNewKeyHashes.find(oldT);
                if (itOld == ctx.oldKeyHashToNewKeyHashes.end()) {
                    continue;
                }
                coveredOldTargets++;
                mappedTargetUnion.insert(itOld->second.begin(), itOld->second.end());
            }
            const size_t minEvidence = std::min<size_t>(ctx.triggerTargetCountAtRefine, 2);
            if (!sourceSplit && coveredOldTargets >= minEvidence && mappedTargetUnion.size() >= minEvidence) {
                unresolvedTriggerPair = true;
            }
        }
        // Action-level transition-sample check (Java checkActionRefinement approximation):
        // estimate post-refine fan-out via old->new key mapping evidence.
        if (ctx.triggerActionHash != 0 && ctx.triggerSourceKeyHash != 0) {
            auto itSrcMap = ctx.oldKeyHashToNewKeyHashes.find(ctx.triggerSourceKeyHash);
            if (itSrcMap != ctx.oldKeyHashToNewKeyHashes.end() && !itSrcMap->second.empty()) {
                std::unordered_set<uintptr_t> mappedTargetUnion;
                size_t coveredOldTargets = 0;
                for (auto oldT : ctx.triggerTargetKeyHashes) {
                    auto itOld = ctx.oldKeyHashToNewKeyHashes.find(oldT);
                    if (itOld == ctx.oldKeyHashToNewKeyHashes.end()) {
                        continue;
                    }
                    coveredOldTargets++;
                    mappedTargetUnion.insert(itOld->second.begin(), itOld->second.end());
                }
                postRefineMaxFanoutForAction = mappedTargetUnion.size();
                const size_t minEvidence = std::min<size_t>(ctx.triggerTargetCountAtRefine, 2);
                // Evidence guard: only fail when source and targets both have enough remap evidence.
                if (ctx.triggerTargetCountAtRefine > 0 &&
                    itSrcMap->second.size() >= minEvidence &&
                    coveredOldTargets >= minEvidence &&
                    postRefineMaxFanoutForAction >= ctx.triggerTargetCountAtRefine) {
                    unresolvedTriggerPair = true;
                }
            }
        }
        const bool hasTriggerSource = (triggerSource != 0);
        // When originState is available, align batchAbstract rollback gate to Java:
        // rollback only if (affectedStates.size > affectedThreshold) OR (targets.size > threshold).
        bool shouldRollback = false;
        if (hasTriggerSource) {
            shouldRollback = overFilteredAffected || overFilteredTargets || unresolvedTriggerPair;
        } else {
            shouldRollback = overSplit || overAffected || overTargets;
        }
        if (!shouldRollback && shouldLogApeDiagSample(std::string("coarsen_gate_no_rollback#") + actKey, 10)) {
            BLOG("ape naming: coarsen-gate activity=%s rollback=0 hasTriggerSource=%d triggerSource=%lu "
                 "overSplit=%d overAffected=%d overTargets=%d overFilteredAffected=%d overFilteredTargets=%d "
                 "unresolvedTriggerPair=%d affected=%zu totalNew=%zu filteredAffected=%zu filteredTargets=%zu "
                 "triggerTargets=%zu postFanout=%zu targetThreshold=%d",
                 activity.c_str(), hasTriggerSource ? 1 : 0, (unsigned long)triggerSource,
                 overSplit ? 1 : 0, overAffected ? 1 : 0, overTargets ? 1 : 0,
                 overFilteredAffected ? 1 : 0, overFilteredTargets ? 1 : 0,
                 unresolvedTriggerPair ? 1 : 0, affectedStateObservations, totalNewKeys.size(),
                 filteredAffected, filteredTargets, ctx.triggerTargetCountAtRefine,
                 postRefineMaxFanoutForAction, targetThreshold);
        }
        if (shouldRollback) {
            std::string fpFiner = cur->fingerprintString();
            std::unordered_set<uintptr_t> affectedStateHashesForBlacklist;
#if defined(FASTBOT_HAS_PUGIXML) && FASTBOT_HAS_PUGIXML && DYNAMIC_STATE_ABSTRACTION_ENABLED
            // Compute affected state-hash set with the same filtering semantics as batchAbstract
            // (Java filterTargets(originState.equals(oldState))).
            for (const auto &kv : _apeStateXmlByStateHash) {
                const uintptr_t ghBa = kv.first;
                const std::string &xml = kv.second;
                if (xml.empty()) {
                    continue;
                }
                naming::StateKey storedKey = naming::StateKey::fromFallbackXmlStringHash("", 0);
                if (!tryGetApeStateKey(ghBa, &storedKey)) {
                    continue;
                }
                if (storedKey.activity() != actKey) {
                    continue;
                }
                uintptr_t tgtKeyHash = 0;
                uintptr_t prevKeyHash = 0;
                apeStateKeyPairFromXmlCoarsenPath(activity, xml, cur, &tgtKeyHash, prev, &prevKeyHash);
                if (!totalNewKeys.empty() && totalNewKeys.count(tgtKeyHash) == 0) {
                    continue;
                }

                bool affectedBa = false;
                if (triggerSource != 0) {
                    affectedBa = (prevKeyHash == triggerSource);
                } else {
                    // Fallback when triggerSource is missing: use old->new mapping evidence.
                    uintptr_t khBaXml = (prevKeyHash != 0) ? prevKeyHash : storedKey.hash();
                    for (const auto &p : ctx.oldKeyHashToNewKeyHashes) {
                        if (p.first == khBaXml) {
                            affectedBa = true;
                            break;
                        }
                        for (uintptr_t nh : p.second) {
                            if (nh == khBaXml) {
                                affectedBa = true;
                                break;
                            }
                        }
                        if (affectedBa) {
                            break;
                        }
                    }
                }
                if (affectedBa) {
                    affectedStateHashesForBlacklist.insert(ghBa);
                }
            }
#endif
            apeBlacklistFinerNamingOnRollback(activity, cur, ctx, affectedStateHashesForBlacklist);
            {
                static std::atomic<uint64_t> g_coarsen_chain{0};
                const uint64_t cn = ++g_coarsen_chain;
                if (cn <= 10 || (cn % 128) == 0) {
                    BDLOG(
                        "ape naming: chain coarsen rollback Abstract update act=%s cur=%p prev=%p "
                        "trigExact=%d trigSrcH=%lu",
                        actKey.c_str(), static_cast<const void *>(cur.get()),
                        static_cast<const void *>(prev.get()), ctx.triggerSourceKeyExact ? 1 : 0,
                        static_cast<unsigned long>(ctx.triggerSourceKeyHash));
                }
            }
            if (ctx.triggerSourceKeyExact) {
                _apeStateNamingManager->updateNamingWithStateKey(
                    actKey, naming::NamingUpdateKind::Abstract, cur, prev, ctx.triggerSourceKey);
            } else {
                _apeStateNamingManager->updateNamingWithStateHash(
                    actKey, naming::NamingUpdateKind::Abstract, cur, prev, ctx.triggerSourceKeyHash);
            }
            invalidateApeGraphStateKeyDedupMap();
            // removeConflictPredicates(aligned): only evaluate constraints whose predicates intersect affectedGUITrees.
            // For now we approximate affectedGUITrees as affected state-hashes (when available under pugixml build).
            std::unordered_set<uintptr_t> affectedStateHashesForPrune;
#if defined(FASTBOT_HAS_PUGIXML) && FASTBOT_HAS_PUGIXML && DYNAMIC_STATE_ABSTRACTION_ENABLED
            // We approximate affectedGUITrees using exactly the GUI-tree XML entries we cache for predicate eval.
            // This better matches Java's affectedGUITrees set fed into removeConflictPredicates.
            for (const auto &kv : _apeStateXmlByStateHash) {
                const uintptr_t ghBa = kv.first;
                const std::string &xml = kv.second;
                if (xml.empty()) {
                    continue;
                }
                naming::StateKey storedKey = naming::StateKey::fromFallbackXmlStringHash("", 0);
                if (!tryGetApeStateKey(ghBa, &storedKey)) {
                    continue;
                }
                if (storedKey.activity() != actKey) {
                    continue;
                }

                uintptr_t tgtKeyHash = 0;
                uintptr_t prevKeyHash = 0;
                apeStateKeyPairFromXmlCoarsenPath(activity, xml, cur, &tgtKeyHash, prev, &prevKeyHash);
                if (!totalNewKeys.empty() && totalNewKeys.count(tgtKeyHash) == 0) {
                    continue;
                }

                bool affectedBa = false;
                if (triggerSource != 0) {
                    affectedBa = (prevKeyHash == triggerSource);
                } else {
                    // Fallback when triggerSource is missing: match against old->new mapping evidence.
                    uintptr_t khBaXml = (prevKeyHash != 0) ? prevKeyHash : storedKey.hash();
                    for (const auto &p : ctx.oldKeyHashToNewKeyHashes) {
                        if (p.first == khBaXml) {
                            affectedBa = true;
                            break;
                        }
                        for (uintptr_t nh : p.second) {
                            if (nh == khBaXml) {
                                affectedBa = true;
                                break;
                            }
                        }
                        if (affectedBa) {
                            break;
                        }
                    }
                }

                if (affectedBa) {
                    affectedStateHashesForPrune.insert(ghBa);
                }
            }
#endif
            pruneApeSourcePartitionPredicates(activity, prev, cur, affectedStateHashesForPrune);
            pruneApeActionPartitionPredicates(activity, prev, cur, affectedStateHashesForPrune);
            pruneApeStatesFewerThanPredicates(activity, prev, cur, affectedStateHashesForPrune);
            {
                // NamingFactory.batchAbstract: after rollback to targetParentNaming, add AssertStatesFewerThan
                // (threshold from finer targetNaming fineness; trees = affected in rollback bucket).
                std::vector<uintptr_t> batchAbstractHashes;
                batchAbstractHashes.reserve(64);
                // Align Java "distinct StateKeys" nature: keep one representative state hash per APE key hash.
                std::unordered_set<uintptr_t> seenApeKeyHashes;
                const uintptr_t originOldKeyHash = ctx.triggerSourceKeyHash;
                // Java AssertStatesFewerThan stops once distinct StateKeys size > threshold.
                const int totalTypesBa = static_cast<int>(naming::namerTypesUsed().size());
                const int shiftBa = std::max(0, totalTypesBa - cur->getFineness());
                const int thrBa = std::min(8, std::max(1, 2 << shiftBa));
                // thrBa is also passed into AssertStatesFewerThan below.
                // Fallback（当我们无法基于 XML 计算 oldState 时）沿用优化 2：
                // keep only target (new) key hashes remapped from the trigger source bucket.
                std::unordered_set<uintptr_t> allowedNewKeyHashes;
                if (originOldKeyHash != 0) {
                    auto itNew = ctx.oldKeyHashToNewKeyHashes.find(originOldKeyHash);
                    if (itNew != ctx.oldKeyHashToNewKeyHashes.end()) {
                        // Strict originState semantics (Java filterTargets):
                        // if a new key hash remaps from multiple different old key hashes, we cannot
                        // guarantee it came from the originState bucket => drop it to avoid false affected.
                        std::unordered_map<uintptr_t, size_t> newKeyToOldCount;
                        for (const auto &p : ctx.oldKeyHashToNewKeyHashes) {
                            for (uintptr_t nh : p.second) {
                                newKeyToOldCount[nh]++;
                            }
                        }
                        for (uintptr_t nh : itNew->second) {
                            if (newKeyToOldCount[nh] == 1) {
                                allowedNewKeyHashes.insert(nh);
                            }
                        }
                    }
                }
                for (const auto &sp : getGraph()->getStates()) {
                    if (!sp) {
                        continue;
                    }
                    auto apBa = sp->getActivityString();
                    const std::string aBa = (apBa && apBa.get())
                                                ? naming::StateKey::canonicalActivityString(*apBa)
                                                : std::string();
                    if (aBa != actKey) {
                        continue;
                    }
                    const uintptr_t ghBa = sp->hash();
                    uintptr_t kBaH = 0;
                    const bool haveStoredApeKey = tryGetApeStateKeyHash(ghBa, &kBaH);
                    const uintptr_t khBa = haveStoredApeKey ? kBaH : ghBa;
                    // Optimization 5: AssertStatesFewerThan distinct-key must match Java,
                    // i.e. distinct StateKeys computed under `prev` (targetParentNaming).
                    // Default to stored APE key hash; if we can rebuild oldState under `prev`,
                    // we'll overwrite this with `oldState.hash()` in XML-space.
                    uintptr_t dedupKey = khBa;
                    // Keep batchAbstract `affectedStates` consistent with rollback gate (optimization 4):
                    // approximate Java `targetStates` membership by checking whether this concrete state belongs
                    // to `targetNaming` (`cur`) key space, i.e. StateKey under `cur` is within `totalNewKeys`.
                    uintptr_t tgtKeyHashForMembership = 0;
#if defined(FASTBOT_HAS_PUGIXML) && FASTBOT_HAS_PUGIXML && DYNAMIC_STATE_ABSTRACTION_ENABLED
                    {
                        auto itXml = _apeStateXmlByStateHash.find(ghBa);
                        const bool haveXml = (itXml != _apeStateXmlByStateHash.end() && !itXml->second.empty());
                        if (haveXml) {
                            uintptr_t tgtH = 0;
                            if (apeStateHashFromXmlWithNaming(activity, itXml->second, cur, &tgtH)) {
                                tgtKeyHashForMembership = tgtH;
                            }
                        }
                    }
#else
                    tgtKeyHashForMembership = dedupKey;
#endif
                    if (!totalNewKeys.empty() &&
                        (tgtKeyHashForMembership == 0 || totalNewKeys.count(tgtKeyHashForMembership) == 0)) {
                        continue;
                    }
                    bool affectedBa = false;

#if defined(FASTBOT_HAS_PUGIXML) && FASTBOT_HAS_PUGIXML && DYNAMIC_STATE_ABSTRACTION_ENABLED
                    if (originOldKeyHash != 0) {
                        // Java filterTargets(originState.equals(oldState)):
                        // recompute oldState under prev using cached XML.
                        auto itXml = _apeStateXmlByStateHash.find(ghBa);
                        if (itXml != _apeStateXmlByStateHash.end() && !itXml->second.empty()) {
                            uintptr_t oldH = 0;
                            if (apeStateHashFromXmlWithNaming(activity, itXml->second, prev, &oldH)) {
                                dedupKey = oldH;
                                affectedBa = (oldH == originOldKeyHash);
                            }
                        }
                    } else
#endif
                    {
                        // Fallback（当 originOldKeyHash 不可用 / 或无 pugixml 时）沿用优化 2：
                        // match against stored APE key hash using old->new mapping evidence.
                        if (!haveStoredApeKey) {
                            continue;
                        }
                        // Remap khBa to XML-space for consistent comparison with
                        // ctx.oldKeyHashToNewKeyHashes / allowedNewKeyHashes (both XML-space).
                        uintptr_t khBaXml = khBa;
#if defined(FASTBOT_HAS_PUGIXML) && FASTBOT_HAS_PUGIXML
                        {
                            auto itXml2 = _apeStateXmlByStateHash.find(ghBa);
                            if (itXml2 != _apeStateXmlByStateHash.end() && !itXml2->second.empty()) {
                                uintptr_t xmH = 0;
                                if (apeStateHashFromXmlWithNaming(activity, itXml2->second, prev, &xmH) &&
                                    xmH != 0) {
                                    khBaXml = xmH;
                                    dedupKey = xmH;
                                }
                            }
                        }
#endif
                        if (!allowedNewKeyHashes.empty()) {
                            // Prefer strict "new target key hashes only" semantics.
                            affectedBa = allowedNewKeyHashes.count(khBaXml) != 0;
                        } else {
                            for (const auto &p : ctx.oldKeyHashToNewKeyHashes) {
                                if (p.first == khBaXml) {
                                    affectedBa = true;
                                    break;
                                }
                                for (uintptr_t nh : p.second) {
                                    if (nh == khBaXml) {
                                        affectedBa = true;
                                        break;
                                    }
                                }
                                if (affectedBa) {
                                    break;
                                }
                            }
                        }
                    }
                    if (!affectedBa) continue;

                    if (seenApeKeyHashes.insert(dedupKey).second) {
                        batchAbstractHashes.push_back(ghBa);
                        // Early stop when distinct-count would already fail predicate.
                        if (seenApeKeyHashes.size() > static_cast<size_t>(thrBa)) {
                            break;
                        }
                    }
                }
                if (!batchAbstractHashes.empty()) {
                    pushApeStatesFewerThanPredicate(activity, prev, thrBa, std::move(batchAbstractHashes));
                }
            }
            std::vector<uintptr_t> repKeyHashes;
            std::unordered_set<uintptr_t> focusOldKeyHashes;
            // Q8 (local remap): cur is in refined key space; use observed old-new mismatch
            // to identify which refined key hashes should be remapped back.
#if defined(FASTBOT_HAS_PUGIXML) && FASTBOT_HAS_PUGIXML && DYNAMIC_STATE_ABSTRACTION_ENABLED
            if (!affectedStateHashesForPrune.empty()) {
                for (uintptr_t sh : affectedStateHashesForPrune) {
                    auto itXml = _apeStateXmlByStateHash.find(sh);
                    if (itXml == _apeStateXmlByStateHash.end() || itXml->second.empty()) {
                        continue;
                    }
                    uintptr_t oldH = 0;
                    uintptr_t newH = 0;
                    if (!apeStateHashFromXmlWithNaming(activity, itXml->second, cur, &oldH) ||
                        !apeStateHashFromXmlWithNaming(activity, itXml->second, prev, &newH)) {
                        continue;
                    }
                    if (oldH != newH) {
                        focusOldKeyHashes.insert(oldH);
                    }
                }
            }
#endif
            repKeyHashes.reserve(8);
            for (uintptr_t h : focusOldKeyHashes) {
                repKeyHashes.push_back(h);
                if (repKeyHashes.size() >= 8) {
                    break;
                }
            }
            if (!repKeyHashes.empty()) {
                rebuildApeStateRepresentativesForKeyHashes(activity, cur, repKeyHashes, 1);
            }
            if (!focusOldKeyHashes.empty()) {
                remapApeTransitionAggregationForActivity(activity, cur, prev, &focusOldKeyHashes);
            }
            pruneStaleApeStatesForActivity(actKey, fpFiner,
                                           affectedStateHashesForPrune.empty() ? nullptr
                                                                               : &affectedStateHashesForPrune);
            _apeNamingCoarseningBlacklist.insert(std::make_pair(actKey, fpFiner));
            if (ctx.triggerSourceKeyHashOriginal != 0 || ctx.triggerActionHash != 0) {
                _apeRefinePairBlacklist[actKey].insert(
                    ApePairKey{ctx.triggerSourceKeyHashOriginal, ctx.triggerActionHash});
            }
            apeCapApeNamingCoarsenAndRefineBlacklists();
            BLOG("ape naming: coarsen activity=%s rollback split=%d overAffected=%d overTargets=%d "
                "overFilteredAffected=%d overFilteredTargets=%d unresolvedTriggerPair=%d "
                "affectedStates=%zu totalNew=%zu filteredAffected=%zu filteredTargets=%zu triggerTargets=%zu postFanout=%zu "
                "targetThreshold=%d triggerSource=%lu fp=%s",
                activity.c_str(), overSplit ? 1 : 0, overAffected ? 1 : 0, overTargets ? 1 : 0,
                overFilteredAffected ? 1 : 0, overFilteredTargets ? 1 : 0, unresolvedTriggerPair ? 1 : 0,
                affectedStateObservations, totalNewKeys.size(), filteredAffected, filteredTargets,
                ctx.triggerTargetCountAtRefine,
                postRefineMaxFanoutForAction, targetThreshold, (unsigned long)triggerSource,
                fpFiner.c_str());
            ctx.oldKeyHashToNewKeyHashes.clear();
            ctx.oldKeyHashToObservationCount.clear();
            ctx.previousNamingBeforeRefine = nullptr;
            ctx.previousNamingFingerprintBeforeRefine.clear();
            ctx.triggerSourceKeyHash = 0;
            ctx.triggerSourceKeyHashOriginal = 0;
            ctx.triggerSourceKeyExact = false;
            ctx.triggerSourceKey = naming::StateKey::fromFallbackXmlStringHash("", 0);
            ctx.triggerActionHash = 0;
            ctx.triggerTargetKeyHashes.clear();
            ctx.triggerTargetCountAtRefine = 0;
            ctx.stateCountAtLastNamingRefinement = getApeStateCountByActivityAndNamingFingerprint(
                actKey, prev ? prev->fingerprintString() : std::string());
            ApeActivityRebuildStats &st = _apeRebuildStatsByActivity[actKey];
            ++st.consecutiveRollbacks;
            (void)apeLocalRebuildFromHistoryIfNeeded(actKey, "rollback");
            
            return true;
        }
        _apeRebuildStatsByActivity[actKey].consecutiveRollbacks = 0;
        BDLOG(
            "ape naming: coarsen keep refinement activity=%s rollback=0 triggerSource=%lu "
            "overSplit=%d overAffected=%d overTargets=%d overFilteredAffected=%d overFilteredTargets=%d "
            "unresolvedTriggerPair=%d affectedObs=%zu totalNewKeys=%zu filteredAffected=%zu filteredTargets=%zu "
            "triggerTargets=%zu postFanout=%zu targetThreshold=%d fineness=%d",
            activity.c_str(), (unsigned long)triggerSource, overSplit ? 1 : 0, overAffected ? 1 : 0,
            overTargets ? 1 : 0, overFilteredAffected ? 1 : 0, overFilteredTargets ? 1 : 0,
            unresolvedTriggerPair ? 1 : 0, affectedStateObservations, totalNewKeys.size(), filteredAffected,
            filteredTargets, ctx.triggerTargetCountAtRefine, postRefineMaxFanoutForAction, targetThreshold,
            fineness);
        return false;
    }

    bool Model::runApeNamingAbstractionBatch() {
        if (Preference::inst() && Preference::inst()->useStaticReuseAbstraction()) {
            BDLOG("ape naming: batch skip reason=static_reuse_abstraction");
            return false;
        }
        const size_t eventRefineBefore = _apeEventRefineSuccessCount;
        const size_t eventRollbackBefore = _apeEventCoarsenRollbackCount;
        const size_t batchRefineBefore = _apeBatchRefineSuccessCount;
        const size_t batchRollbackBefore = _apeBatchCoarsenRollbackCount;
        const int minTargets = (_preference ? _preference->getApeNamingMinNonDetTargets()
                                            : MinNonDeterminismCount);
        for (const auto &kv : _apeNamingContext) {
            const std::string &activity = kv.first;
            const ApeNamingAbstractionContext &ctx = kv.second;
            if (!ctx.previousNamingBeforeRefine) {
                continue;
            }
            naming::NamingPtr n = _apeStateNamingManager->activityManager().getNaming(activity);
            if (!n) {
                continue;
            }
            if (ctx.previousNamingFingerprintBeforeRefine != n->fingerprintString()) {
                if (coarsenActivityApeNamingIfNeeded(activity)) {
                    _apeBatchCoarsenRollbackCount++;
                }
            }
        }
        if (Preference::inst() && !Preference::inst()->useApeNamingPeriodicRefinement()) {
            const bool mutatedPrefetch =
                (_apeBatchRefineSuccessCount > batchRefineBefore ||
                 _apeBatchCoarsenRollbackCount > batchRollbackBefore);
            BDLOG("ape naming: batch prefetch-only path (periodic refinement off) mutated=%d",
                  mutatedPrefetch ? 1 : 0);
            if (mutatedPrefetch) {
                notifyAgentsOfApeNamingChange();
            }
            return mutatedPrefetch;
        }
        const std::string ruleProfile =
            (_preference ? _preference->getApeNamingActionRefineRuleProfile() : "baseline");
        auto collectNonDetPairs = [&]() -> std::vector<ApeNonDetPairStat> {
            std::vector<ApeNonDetPairStat> out;
            out.reserve(_apePairAgg.size());
            for (const auto &kv : _apePairAgg) {
                const auto &tm = kv.second.targetCounts;
                if (tm.size() < static_cast<size_t>(minTargets)) {
                    continue;
                }
                ApeNonDetPairStat s;
                s.sourceKeyHash = kv.first.sourceKeyHash;
                s.actionHash = kv.first.actionHash;
                s.sourceActivity = kv.second.sourceActivity;
                s.hasSourceStateKey = kv.second.hasSourceStateKey;
                if (s.hasSourceStateKey) {
                    s.sourceStateKey = kv.second.sourceStateKey;
                }
                tm.forEach([&](uintptr_t h, int /*count*/) {
                    s.targetKeyHashes.insert(h);
                });
                s.targetCount = s.targetKeyHashes.size();
                out.push_back(std::move(s));
            }
            std::sort(out.begin(), out.end(), [](const ApeNonDetPairStat &a, const ApeNonDetPairStat &b) {
                if (a.targetCount != b.targetCount) return a.targetCount > b.targetCount;
                if (a.sourceActivity != b.sourceActivity) return a.sourceActivity < b.sourceActivity;
                if (a.sourceKeyHash != b.sourceKeyHash) return a.sourceKeyHash < b.sourceKeyHash;
                return a.actionHash < b.actionHash;
            });
            return out;
        };
        auto toRefinePair = [](const ApeNonDetPairStat &p) -> ApeRefinePair {
            ApeRefinePair out;
            out.sourceKeyHash = p.sourceKeyHash;
            out.hasSourceStateKey = p.hasSourceStateKey;
            if (out.hasSourceStateKey) {
                out.sourceStateKey = p.sourceStateKey;
            }
            out.actionHash = p.actionHash;
            out.targetKeyHashes = p.targetKeyHashes;
            out.targetCount = p.targetCount;
            return out;
        };
        auto countNonDetPairsPerActivity = [](const std::vector<ApeNonDetPairStat> &v) {
            std::unordered_map<std::string, int> m;
            m.reserve(std::max<size_t>(v.size(), 8) * 2);
            for (const auto &p : v) {
                const std::string k = naming::StateKey::canonicalActivityString(p.sourceActivity);
                m[k]++;
            }
            return m;
        };
        if (UsePaperRefinementOrder) {
            std::vector<ApeNonDetPairStat> nonDetPairs = collectNonDetPairs();
            const std::unordered_map<std::string, int> nonDetCountByAct = countNonDetPairsPerActivity(nonDetPairs);
            if (ruleProfile == "java_rule_02_preview" && nonDetPairs.size() > 1) {
                nonDetPairs.resize(1);
            }
            BLOG("ape naming: paper order nonDetPairs=%zu", nonDetPairs.size());
            std::unordered_set<std::string> refinedActivities;
            for (const auto &p : nonDetPairs) {
                if (refinedActivities.count(p.sourceActivity) != 0) {
                    continue;
                }
                ApeRefinePair rp = toRefinePair(p);
                BLOG("ape naming: refine-attempt activity=%s srcKey=%lu act=%lu targets=%zu",
                     p.sourceActivity.c_str(), (unsigned long)p.sourceKeyHash,
                     (unsigned long)p.actionHash, p.targetCount);
                const int preN = nonDetCountByAct.at(
                    naming::StateKey::canonicalActivityString(p.sourceActivity));
                if (refineActivityApeNaming(p.sourceActivity, &rp, preN)) {
                    _apeBatchRefineSuccessCount++;
                    if (coarsenActivityApeNamingIfNeeded(p.sourceActivity)) {
                        _apeBatchCoarsenRollbackCount++;
                    }
                    refinedActivities.insert(p.sourceActivity);
                } else if (rp.actionHash != 0 &&
                           p.targetCount >= static_cast<size_t>(kApeNDActionBlacklistMinOutEdges)) {
                    const std::string actKey = naming::StateKey::canonicalActivityString(p.sourceActivity);
                    auto &blk = _apeRefineActionBlacklist[actKey];
                    const bool inserted = blk.insert(rp.actionHash).second;
                    apeCapApeNamingCoarsenAndRefineBlacklists();
                    BLOG("ape naming: NDActionBlacklist add (APE: out>=%d after failed resolve) activity=%s "
                         "act=%lu targets=%zu",
                         kApeNDActionBlacklistMinOutEdges, p.sourceActivity.c_str(),
                         (unsigned long)rp.actionHash, p.targetCount);
                    if (shouldLogApeDiagSample(std::string("nd_blacklist_batch_add#") + actKey, 25)) {
                        BLOG("ape naming: diag NDActionBlacklist[batch] activity=%s act=%lu inserted=%d size=%zu targets=%zu",
                             p.sourceActivity.c_str(), (unsigned long)rp.actionHash,
                             inserted ? 1 : 0, blk.size(), p.targetCount);
                    }
                }
            }
        } else {
            std::vector<ApeNonDetPairStat> nonDetPairs = collectNonDetPairs();
            const std::unordered_map<std::string, int> nonDetCountByAct = countNonDetPairsPerActivity(nonDetPairs);
            if (ruleProfile == "java_rule_02_preview" && nonDetPairs.size() > 1) {
                nonDetPairs.resize(1);
            }
            BLOG("ape naming: batch nonDetPairs=%zu", nonDetPairs.size());
            std::vector<std::string> refinedActs;
            std::unordered_set<std::string> refinedActivities;
            for (const auto &p : nonDetPairs) {
                if (refinedActivities.count(p.sourceActivity) != 0) {
                    continue;
                }
                ApeRefinePair rp = toRefinePair(p);
                BLOG("ape naming: refine-attempt activity=%s srcKey=%lu act=%lu targets=%zu",
                     p.sourceActivity.c_str(), (unsigned long)p.sourceKeyHash,
                     (unsigned long)p.actionHash, p.targetCount);
                const int preN = nonDetCountByAct.at(
                    naming::StateKey::canonicalActivityString(p.sourceActivity));
                if (refineActivityApeNaming(p.sourceActivity, &rp, preN)) {
                    _apeBatchRefineSuccessCount++;
                    refinedActs.push_back(p.sourceActivity);
                    refinedActivities.insert(p.sourceActivity);
                } else if (rp.actionHash != 0 &&
                           p.targetCount >= static_cast<size_t>(kApeNDActionBlacklistMinOutEdges)) {
                    const std::string actKey = naming::StateKey::canonicalActivityString(p.sourceActivity);
                    auto &blk = _apeRefineActionBlacklist[actKey];
                    const bool inserted = blk.insert(rp.actionHash).second;
                    apeCapApeNamingCoarsenAndRefineBlacklists();
                    BLOG("ape naming: NDActionBlacklist add (APE: out>=%d after failed resolve) activity=%s "
                         "act=%lu targets=%zu",
                         kApeNDActionBlacklistMinOutEdges, p.sourceActivity.c_str(),
                         (unsigned long)rp.actionHash, p.targetCount);
                    if (shouldLogApeDiagSample(std::string("nd_blacklist_batch_add#") + actKey, 25)) {
                        BLOG("ape naming: diag NDActionBlacklist[batch] activity=%s act=%lu inserted=%d size=%zu targets=%zu",
                             p.sourceActivity.c_str(), (unsigned long)rp.actionHash,
                             inserted ? 1 : 0, blk.size(), p.targetCount);
                    }
                }
            }
            for (const auto &a : refinedActs) {
                if (coarsenActivityApeNamingIfNeeded(a)) {
                    _apeBatchCoarsenRollbackCount++;
                }
            }
        }
        const size_t eventRefineDelta = _apeEventRefineSuccessCount - eventRefineBefore;
        const size_t eventRollbackDelta = _apeEventCoarsenRollbackCount - eventRollbackBefore;
        const size_t batchRefineDelta = _apeBatchRefineSuccessCount - batchRefineBefore;
        const size_t batchRollbackDelta = _apeBatchCoarsenRollbackCount - batchRollbackBefore;
        BLOG("ape naming: counters delta event(refine=%zu,rollback=%zu) "
             "batch(refine=%zu,rollback=%zu) total event(refine=%zu,rollback=%zu) "
             "batch(refine=%zu,rollback=%zu)",
             eventRefineDelta, eventRollbackDelta, batchRefineDelta, batchRollbackDelta,
             _apeEventRefineSuccessCount, _apeEventCoarsenRollbackCount,
             _apeBatchRefineSuccessCount, _apeBatchCoarsenRollbackCount);
        if (_apeStateNamingManager) {
            const auto edgeStats = _apeStateNamingManager->consumeEdgeLookupStats();
            const uint64_t total =
                edgeStats.exact_hit + edgeStats.hash_only_hit + edgeStats.miss;
            if (total > 0) {
                const double exactRate = (100.0 * static_cast<double>(edgeStats.exact_hit)) /
                                         static_cast<double>(total);
                const double fallbackRate = (100.0 * static_cast<double>(edgeStats.hash_only_hit)) /
                                            static_cast<double>(total);
                const double missRate = (100.0 * static_cast<double>(edgeStats.miss)) /
                                        static_cast<double>(total);
                BLOG("ape naming: edge lookup window total=%" PRIu64
                     " exact=%" PRIu64 " (%.2f%%) hashOnly=%" PRIu64 " (%.2f%%) miss=%" PRIu64 " (%.2f%%)",
                     total, edgeStats.exact_hit, exactRate,
                     edgeStats.hash_only_hit, fallbackRate, edgeStats.miss, missRate);
            }
#if defined(FASTBOT_HAS_PUGIXML) && FASTBOT_HAS_PUGIXML
            // Review 1.1–1.2: evaluateNaming / default ActionType XPath / rebuild failures (logs inside consume*).
            (void)naming::NamingFactory::consumeNamingEvaluateDiagStats();
            // Review 1.3–1.4: StateNamingManager silent rejects + treeToNaming / fixed-point (logs inside consume*).
            (void)naming::StateNamingManager::consumeUpdateRejectDiagStats();
            (void)naming::StateNamingManager::consumeTreeWalkDiagStats();
#endif
        }
        const ApeCorrectnessCounters counters = _ape_correctness_counters;
        const uint64_t stateKeyBuildTotal = counters.statekey_build_ok + counters.statekey_build_fail;
        if (stateKeyBuildTotal > 0 || counters.statekey_fallback_used > 0 ||
            counters.graph_dedup_hash_hit > 0 || counters.naming_update_by_hash > 0 ||
            counters.statekey_record_hash_collision > 0 || counters.evidence_pool_sample_add > 0 ||
            counters.evidence_pool_new_pair > 0 || counters.evidence_pool_evict > 0) {
            BLOG("ape correctness: stateKeyBuild=%" PRIu64 " ok=%" PRIu64 " fail=%" PRIu64
                 " failNull=%" PRIu64 " failTree=%" PRIu64 " failNaming=%" PRIu64 " failRebuild=%" PRIu64
                 " fallback=%" PRIu64 " stateKeyHashMulti=%" PRIu64
                 " graphDedupLookups=%" PRIu64 " graphDedupExact=%" PRIu64 " graphDedupCollision=%" PRIu64
                 " namingUpdateHash=%" PRIu64 " evidencePoolSampleAdd=%" PRIu64
                 " evidencePoolNewPair=%" PRIu64 " evidencePoolEvict=%" PRIu64,
                 stateKeyBuildTotal, counters.statekey_build_ok, counters.statekey_build_fail,
                 counters.statekey_fail_null_input, counters.statekey_fail_build_tree_dom,
                 counters.statekey_fail_no_naming, counters.statekey_fail_rebuild_tree,
                 counters.statekey_fallback_used, counters.statekey_record_hash_collision,
                 counters.graph_dedup_hash_hit, counters.graph_dedup_exact_hit,
                 counters.graph_dedup_hash_collision, counters.naming_update_by_hash,
                 counters.evidence_pool_sample_add, counters.evidence_pool_new_pair,
                 counters.evidence_pool_evict);
        }
        _ape_correctness_counters = ApeCorrectnessCounters{};
        const bool batchMutated = (batchRefineDelta > 0 || batchRollbackDelta > 0);
        BDLOG("ape naming: batch done batchMutated=%d eventD(ref=%zu,rb=%zu) batchD(ref=%zu,rb=%zu)",
              batchMutated ? 1 : 0, eventRefineDelta, eventRollbackDelta, batchRefineDelta,
              batchRollbackDelta);
        if (batchMutated) {
            notifyAgentsOfApeNamingChange();
        }
        return batchMutated;
    }

    void Model::runRefinementAndCoarseningIfScheduled() {
        if (Preference::inst() && Preference::inst()->useStaticReuseAbstraction()) {
            BDLOG("state abstraction: skip scheduled ape batch reason=static_reuse_abstraction");
            return;
        }
        if (_stepCountSinceLastCheck < static_cast<size_t>(RefinementCheckInterval)) {
            BDLOG("state abstraction: skip scheduled ape batch reason=interval stepsSince=%zu need=%d",
                  _stepCountSinceLastCheck, (int)RefinementCheckInterval);
            return;
        }
        BLOG("state abstraction: ape-only batch at step %zu (interval=%d)",
             _stepCountSinceLastCheck, (int)RefinementCheckInterval);
        (void)runApeNamingAbstractionBatch();
    }
#endif

    void Model::reportActivity(const std::string &activity) {
        if (activity.empty()) return;
        std::lock_guard<std::mutex> lock(_coverageMutex);
        _visitedActivities.insert(activity);
        _coverageStepCount++;
    }

    std::string Model::getCoverageJson() const {
        std::lock_guard<std::mutex> lock(_coverageMutex);
        nlohmann::json j;
        j["stepsCount"] = _coverageStepCount;
        nlohmann::json arr = nlohmann::json::array();
        for (const auto &a : _visitedActivities) {
            arr.push_back(a);
        }
        j["testedActivities"] = arr;
        return j.dump();
    }

    double Model::getLlmdroidStagnationMetric() const {
        std::lock_guard<std::mutex> lock(_coverageMutex);
        const size_t states = _graph ? _graph->stateSize() : 0;
        const size_t acts = _visitedActivities.size();
        const int steps = _coverageStepCount;
        return static_cast<double>(states) + 0.01 * static_cast<double>(acts) +
               1e-9 * static_cast<double>(steps);
    }

    void Model::loadStateAbstractionPolicy() {
#if DYNAMIC_STATE_ABSTRACTION_ENABLED
        auto pref = Preference::inst();
        // Only meaningful when dynamic abstraction is actually used and policy persistence is enabled.
        if (!pref || pref->useStaticReuseAbstraction() || !pref->isStateAbstractionPolicyEnabled()) {
            return;
        }

        const std::string &pkg = getPackageName();
        if (pkg.empty()) {
            return;
        }

        std::string path = "/sdcard/fastbot_" + pkg + ".statekey.json";
        BLOG("state abstraction: try load policy from %s", path.c_str());
        std::ifstream in(path);
        if (!in.is_open()) {
            return;
        }

        try {
            // Basic size guard to avoid attempting to parse extremely large / corrupted files.
            in.seekg(0, std::ios::end);
            std::streamoff sz = in.tellg();
            if (sz <= 0 || sz > static_cast<std::streamoff>(1024 * 1024)) { // 1MB hard upper bound
                BLOGE("state abstraction: skip loading %s (size=%lld bytes out of bounds)", path.c_str(),
                      static_cast<long long>(sz));
                return;
            }
            in.seekg(0, std::ios::beg);

            nlohmann::json j;
            in >> j;

            if (!j.is_object()) {
                BLOGE("state abstraction: policy file %s is not a JSON object", path.c_str());
                return;
            }

            // v1 files may contain widget-key masks and coarseningBlacklist (legacy); do not apply — dynamic
            // identity is APE StateKey-only; keeping old entries would confuse debugging.
            auto itActs = j.find("activities");
            if (itActs != j.end() && itActs->is_array() && !itActs->empty()) {
                BLOG("state abstraction: %s contains legacy activities[]; ignored", path.c_str());
            }
            auto itBlk = j.find("coarseningBlacklist");
            if (itBlk != j.end() && itBlk->is_array() && !itBlk->empty()) {
                BLOG("state abstraction: %s contains legacy coarseningBlacklist; ignored", path.c_str());
            }

            BLOG("state abstraction: loaded policy metadata from %s (no widget-mask state applied)", path.c_str());
        } catch (const std::exception &ex) {
            BLOGE("state abstraction: failed to load policy from %s: %s", path.c_str(), ex.what());
        }
#else
        (void)this;
#endif
    }

    void Model::saveStateAbstractionPolicy() const {
#if DYNAMIC_STATE_ABSTRACTION_ENABLED
        auto pref = Preference::inst();
        // Only meaningful when dynamic abstraction is actually used and policy persistence is enabled.
        if (!pref || pref->useStaticReuseAbstraction() || !pref->isStateAbstractionPolicyEnabled()) {
            return;
        }

        const std::string &pkg = getPackageName();
        if (pkg.empty()) {
            return;
        }

        std::string path = "/sdcard/fastbot_" + pkg + ".statekey.json";
        std::string tmpPath = path + ".tmp";

        nlohmann::json j;
        j["version"] = 2;
        j["activities"] = nlohmann::json::array();

        try {
            std::ofstream out(tmpPath, std::ios::trunc);
            if (!out.is_open()) {
                BLOGE("state abstraction: cannot open temp policy file %s for writing", tmpPath.c_str());
                return;
            }
            out << j.dump();
            out.close();
            if (std::rename(tmpPath.c_str(), path.c_str()) != 0) {
                BLOGE("state abstraction: failed to rename policy file %s -> %s", tmpPath.c_str(), path.c_str());
            } else {
                BLOG("state abstraction: policy saved to %s", path.c_str());
            }
        } catch (const std::exception &ex) {
            BLOGE("state abstraction: failed to save policy to %s: %s", path.c_str(), ex.what());
        }
#else
        (void)this;
#endif
    }

#if DYNAMIC_STATE_ABSTRACTION_ENABLED
    void Model::logApeStateKeySnapshot(const std::string &rawActivity, const StatePtr &state,
                                       const naming::StateKey &key, const GraphPtr &graph) {
        const auto &xs = key.sortedXPaths();
        std::ostringstream head;
        head << "state-key: activity=" << key.activity()
             << ";hash=" << static_cast<unsigned long>(key.hash())
             << ";naming=" << key.namingFingerprint()
             << ";widgets=" << xs.size();
        BDLOG("%s", head.str().c_str());
        for (size_t i = 0; i < xs.size(); ++i) {
            BDLOG("  %zu xpath=%s", static_cast<unsigned long>(i), xs[i].c_str());
        }
        (void)rawActivity;
        (void)state;
        (void)graph;
    }

#endif

#if defined(FASTBOT_HAS_PUGIXML) && FASTBOT_HAS_PUGIXML

    bool Model::buildApeStateKeyFromElementTree(const ElementPtr &element, const std::string &activity,
                                               naming::StateKey *outKey,
                                               ApeStateKeyBuildFailReason *outFailReason,
                                               const StatePtr &stateForDynamicApply,
                                               std::string *ioXmlCache) {
        auto fail = [&](ApeStateKeyBuildFailReason reason) -> bool {
            if (outFailReason != nullptr) {
                *outFailReason = reason;
            }
            _ape_correctness_counters.statekey_build_fail++;
            switch (reason) {
                case ApeStateKeyBuildFailReason::NullInput:
                    _ape_correctness_counters.statekey_fail_null_input++;
                    break;
                case ApeStateKeyBuildFailReason::BuildTreeOrDomFailed:
                    _ape_correctness_counters.statekey_fail_build_tree_dom++;
                    break;
                case ApeStateKeyBuildFailReason::NoNaming:
                    _ape_correctness_counters.statekey_fail_no_naming++;
                    break;
                case ApeStateKeyBuildFailReason::RebuildTreeFailed:
                    _ape_correctness_counters.statekey_fail_rebuild_tree++;
                    break;
                case ApeStateKeyBuildFailReason::None:
                default:
                    break;
            }
            return false;
        };

        if (!element || !outKey) {
            return fail(ApeStateKeyBuildFailReason::NullInput);
        }
        // When static reuse abstraction is enabled, we must not sync APE naming fixed-point
        // refinement or update dynamic naming bookkeeping; we only need enough naming to
        // compute the StateKey identity.
        const bool wantApeRlIdentity =
            !_preference || !_preference->useStaticReuseAbstraction();
        std::string pkg;
        std::string cls;
        naming::StateKey::splitActivityPackageClass(activity, &pkg, &cls);
        const std::string actKey = naming::StateKey::canonicalActivityString(activity);
        gui_tree::GUITreeBuildResult built = gui_tree::GUITreeFactory::buildFromElement(element, pkg, cls);
        if (!built.tree || !built.dom) {
            const std::string *xmlPtr = nullptr;
            std::string xmlLocal;
            if (ioXmlCache && !ioXmlCache->empty()) {
                xmlPtr = ioXmlCache;
            } else {
                xmlLocal = element->toXMLCached();
                if (ioXmlCache) {
                    *ioXmlCache = xmlLocal;
                }
                xmlPtr = &xmlLocal;
            }
            built = gui_tree::GUITreeFactory::buildFromXml(*xmlPtr, pkg, cls);
        }
        if (!built.tree || !built.dom) {
            return fail(ApeStateKeyBuildFailReason::BuildTreeOrDomFailed);
        }
        naming::ActivityNamingManager &mgr = _apeStateNamingManager->activityManager();
        const int fpSteps = (_preference && wantApeRlIdentity)
                                ? _preference->getApeNamingFixedPointMaxIter()
                                : 0;
        naming::NamingPtr naming;
        if (fpSteps > 0) {
            naming::NamingPtr beforeNaming = mgr.getNaming(actKey);
            std::string fpBefore;
            if (beforeNaming) {
                fpBefore = beforeNaming->fingerprintString();
            }
            naming = _apeStateNamingManager->getNamingFixedPoint(actKey, *built.tree, built.dom, fpSteps);
            if (!naming) {
                return fail(ApeStateKeyBuildFailReason::NoNaming);
            }
#if DYNAMIC_STATE_ABSTRACTION_ENABLED
            const std::string fpAfter = naming->fingerprintString();
            if (fpAfter != fpBefore) {
                // getNamingFixedPoint persists the new naming into ActivityNamingManager.
                // Align the rest of the runtime caches with the new key space.
                invalidateApeGraphStateKeyDedupMap();
                uintptr_t focusOldKeyHash = 0;
                uintptr_t focusOldKeyHashXml = 0;
                if (beforeNaming && built.tree && built.dom) {
                    if (naming::NamingFactory::rebuildTree(beforeNaming, *built.tree, built.dom)) {
                        focusOldKeyHash = naming::StateKey::hashFromGUITree(*built.tree);
                    }
                    if (!naming::NamingFactory::rebuildTree(naming, *built.tree, built.dom)) {
                        // Rebuild under current naming failed; tree may be stuck in beforeNaming state.
                        // Re-run getNamingFixedPoint to restore the tree to a consistent state.
                        naming = _apeStateNamingManager->getNamingFixedPoint(
                            actKey, *built.tree, built.dom, fpSteps);
                        if (!naming) {
                            return fail(ApeStateKeyBuildFailReason::NoNaming);
                        }
                    }
                }
                // Recompute focusOldKeyHash in XML-space for affectedTrees comparison
                // (apeStateHashFromXmlWithNaming uses buildFromXml which can differ from buildFromElement).
#if defined(FASTBOT_HAS_PUGIXML) && FASTBOT_HAS_PUGIXML
                if (focusOldKeyHash != 0 && beforeNaming) {
                    std::string xmlForRemap;
                    if (ioXmlCache && !ioXmlCache->empty()) {
                        xmlForRemap = *ioXmlCache;
                    } else if (element) {
                        xmlForRemap = element->toXMLCached();
                    }
                    if (!xmlForRemap.empty()) {
                        uintptr_t xmH = 0;
                        if (apeStateHashFromXmlWithNaming(activity, xmlForRemap, beforeNaming, &xmH) &&
                            xmH != 0) {
                            focusOldKeyHashXml = xmH;
                        }
                    }
                    if (focusOldKeyHashXml == 0) {
                        focusOldKeyHashXml = focusOldKeyHash;
                    }
                }
#else
                focusOldKeyHashXml = focusOldKeyHash;
#endif
                std::unordered_set<uintptr_t> focusOldKeyHashes;
                const uintptr_t focusForRemap = (focusOldKeyHashXml != 0) ? focusOldKeyHashXml : focusOldKeyHash;
                if (focusForRemap != 0) {
                    focusOldKeyHashes.insert(focusForRemap);
                }
                remapApeTransitionAggregationForActivity(
                    activity, beforeNaming, naming,
                    focusOldKeyHashes.empty() ? nullptr : &focusOldKeyHashes);
                std::unordered_set<uintptr_t> affectedTrees;
                if (focusOldKeyHashXml != 0 && beforeNaming) {
                    for (const auto &kv : _apeStateXmlByStateHash) {
                        const uintptr_t sh = kv.first;
                        const std::string &xml = kv.second;
                        if (xml.empty()) {
                            continue;
                        }
                        naming::StateKey storedKey = naming::StateKey::fromFallbackXmlStringHash("", 0);
                        if (!tryGetApeStateKey(sh, &storedKey) || storedKey.activity() != actKey) {
                            continue;
                        }
                        uintptr_t oldH = 0;
                        if (apeStateHashFromXmlWithNaming(activity, xml, beforeNaming, &oldH) &&
                            oldH == focusOldKeyHashXml) {
                            affectedTrees.insert(sh);
                        }
                    }
                }
                pruneStaleApeStatesForActivity(actKey, fpBefore,
                                               affectedTrees.empty() ? nullptr : &affectedTrees);
                notifyAgentsOfApeNamingChange();
            }
#endif
        } else {
            naming = mgr.getNaming(actKey);
            if (!naming) {
                naming = naming::NamingFactory::defaultRootNaming();
                // In static reuse abstraction mode, avoid syncing a newly created naming into the manager.
                if (naming && wantApeRlIdentity) {
                    _apeStateNamingManager->updateNaming(
                        actKey, naming::NamingUpdateKind::Refine, naming);
                }
            }
            if (!naming) {
                return fail(ApeStateKeyBuildFailReason::NoNaming);
            }
            if (!naming::NamingFactory::rebuildTree(naming, *built.tree, built.dom)) {
                return fail(ApeStateKeyBuildFailReason::RebuildTreeFailed);
            }
        }
#if DYNAMIC_STATE_ABSTRACTION_ENABLED
        if (wantApeRlIdentity) {
            auto itCtx = _apeNamingContext.find(actKey);
            if (itCtx != _apeNamingContext.end() && itCtx->second.previousNamingBeforeRefine) {
                naming::NamingPtr prevN = itCtx->second.previousNamingBeforeRefine;
                uintptr_t oldH = 0;
                uintptr_t newH = 0;
#if defined(FASTBOT_HAS_PUGIXML) && FASTBOT_HAS_PUGIXML
                {
                    std::string xmlForRemap;
                    if (ioXmlCache && !ioXmlCache->empty()) {
                        xmlForRemap = *ioXmlCache;
                    } else if (element) {
                        xmlForRemap = element->toXMLCached();
                        if (ioXmlCache) {
                            *ioXmlCache = xmlForRemap;
                        }
                    }
                    if (!xmlForRemap.empty()) {
                        uintptr_t xmlOldH = 0;
                        uintptr_t xmlNewH = 0;
                        if (apeStateHashFromXmlWithTwoNamings(activity, xmlForRemap, prevN, &xmlOldH, naming, &xmlNewH)) {
                            oldH = xmlOldH;
                            newH = xmlNewH;
                        }
                    }
                }
#endif
                // Fallback to Element-space if XML remap unavailable.
                if (oldH == 0 && newH == 0) {
                    if (naming::NamingFactory::rebuildTree(prevN, *built.tree, built.dom)) {
                        oldH = naming::StateKey::hashFromGUITree(*built.tree);
                    }
                    if (!naming::NamingFactory::rebuildTree(naming, *built.tree, built.dom)) {
                        oldH = 0;
                        newH = 0;
                    } else {
                        newH = naming::StateKey::hashFromGUITree(*built.tree);
                    }
                }
                if (oldH != 0 && newH != 0 && oldH != newH) {
                    itCtx->second.oldKeyHashToNewKeyHashes[oldH].insert(newH);
                    itCtx->second.oldKeyHashToObservationCount[oldH]++;
                }
            }
        }
#endif
        naming::StateKey kNew = naming::StateKey::fromGUITree(*built.tree);
#if DYNAMIC_STATE_ABSTRACTION_ENABLED
        if (stateForDynamicApply) {
            std::vector<gui_tree::GUITreeNode *> guiPreOrder;
            collectGUITreeNodesPreOrder(built.tree->getRootNode(), &guiPreOrder);
            applyApeDynamicActionHashesToReuseState(stateForDynamicApply, guiPreOrder, kNew);
        }
#endif
        *outKey = std::move(kNew);
        if (outFailReason != nullptr) {
            *outFailReason = ApeStateKeyBuildFailReason::None;
        }
        _ape_correctness_counters.statekey_build_ok++;
        return true;
    }
#endif

    void Model::recordApeStateKey(const StatePtr &state, const naming::StateKey &key) {
        if (!state) {
            return;
        }
        const uintptr_t stateHash = state->hash();
        auto &bucket = _ape_state_keys_by_hash[stateHash];

        // In dynamic APE identity mode, State::hash() is overridden to StateKey::hash().
        // Only in this mode does it make sense to treat multiple different keys under the
        // same hash as a potential hash collision.
        const bool inApeHashSpace = (stateHash == key.hash());
        if (!inApeHashSpace) {
            bucket.clear();
            bucket.push_back(key);
            return;
        }

        for (const auto &existing : bucket) {
            if (existing == key) {
                return;
            }
        }
        if (!bucket.empty()) {
            _ape_correctness_counters.statekey_record_hash_collision++;
        }
        bucket.push_back(key);
    }

    bool Model::tryGetApeStateKey(uintptr_t stateHash, naming::StateKey *out) const {
        auto it = _ape_state_keys_by_hash.find(stateHash);
        if (it == _ape_state_keys_by_hash.end() || it->second.empty()) {
            return false;
        }
        if (it->second.size() != 1) {
            return false;
        }
        if (out != nullptr) {
            *out = it->second.front();
        }
        return true;
    }

    bool Model::tryGetApeStateKeyHash(uintptr_t stateHash, uintptr_t *outKeyHash) const {
        naming::StateKey k = naming::StateKey::fromFallbackXmlStringHash("", 0);
        if (!tryGetApeStateKey(stateHash, &k)) {
            return false;
        }
        if (outKeyHash != nullptr) {
            *outKeyHash = k.hash();
        }
        return true;
    }

    /**
     * @brief Destructor for Model class
     * 
     * Clears the device-agent map to release all agent resources.
     * The graph and preference are shared pointers and will be automatically
     * cleaned up when the last reference is released.
     */
    Model::~Model() {
        this->_deviceIDAgentMap.clear();
    }

}  // namespace fastbotx

#endif  // Model_CPP_