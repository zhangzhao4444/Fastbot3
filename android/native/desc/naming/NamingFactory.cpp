/*
 * This code is licensed under the Fastbot license. You may obtain a copy of this license in the LICENSE.txt file in the root directory of this source tree.
 */
/**
 * @authors Jianqiang Guo, Yuhui Su, Zhao Zhang
 */
#ifndef FASTBOTX_NAMING_FACTORY_CPP_
#define FASTBOTX_NAMING_FACTORY_CPP_

#include "NamingFactory.h"
#include "../reuse/ReuseState.h"
#include "../../utils.hpp"
#include <unordered_map>

namespace fastbotx {

    NamingFactory::NamingFactory() {
        _base = createBaseNaming();
        _top = createTopNaming();
        _bottom = createBottomNaming();
    }

    bool NamingFactory::checkPredicate(const NamingPtr &naming,
                                       const std::vector<ElementPtr> &affected) const {
        // Reject any naming that has been explicitly blacklisted for one of
        // the affected trees or violates a previously added Predicate.
        for (const auto &tree : affected) {
            if (isBlacklisted(tree, naming)) {
                return false;
            }
        }
        for (const auto &predicate : _predicates) {
            if (!predicate->eval(naming, affected)) {
                return false;
            }
        }
        return true;
    }

    void NamingFactory::addPredicate(const PredicatePtr &predicate) {
        if (predicate) {
            _predicates.emplace_back(predicate);
        }
    }

    void NamingFactory::removeConflictingPredicates(const NamingPtr &naming,
                                                    const std::vector<ElementPtr> &affected) {
        std::vector<PredicatePtr> keep;
        keep.reserve(_predicates.size());
        for (const auto &predicate : _predicates) {
            if (predicate && predicate->eval(naming, affected)) {
                keep.emplace_back(predicate);
            }
        }
        _predicates.swap(keep);
    }

    void NamingFactory::blacklistNaming(const std::vector<ElementPtr> &trees,
                                        const NamingPtr &naming) {
        for (const auto &tree : trees) {
            _treeNamingBlacklist[tree].insert(naming);
        }
    }

    bool NamingFactory::isBlacklisted(const ElementPtr &tree, const NamingPtr &naming) const {
        auto iter = _treeNamingBlacklist.find(tree);
        if (iter == _treeNamingBlacklist.end()) {
            return false;
        }
        return iter->second.count(naming) > 0;
    }

    NamingPtr NamingFactory::createTopNaming() const {
        auto namelet = std::make_shared<Namelet>(Namelet::Type::BASE, "//*", NamerFactory::topNamer());
        namelet->setMatchRule(Namelet::MatchRule::ANY);
        std::vector<NameletPtr> namelets = {namelet};
        return std::make_shared<Naming>(namelets);
    }

    NamingPtr NamingFactory::createBottomNaming() const {
        auto namelet = std::make_shared<Namelet>(Namelet::Type::BASE, "//*", NamerFactory::bottomNamer());
        namelet->setMatchRule(Namelet::MatchRule::ANY);
        std::vector<NameletPtr> namelets = {namelet};
        return std::make_shared<Naming>(namelets);
    }

    NamingPtr NamingFactory::createBaseNaming() const {
        std::vector<NameletPtr> namelets;

        auto actionable = std::make_shared<Namelet>(Namelet::Type::BASE,
                                                    "//*[@clickable='true' or @long-clickable='true' or @checkable='true' or @scrollable='true']",
                                                    NamerFactory::getNamer({NamerType::TYPE}));
        actionable->setMatchRule(Namelet::MatchRule::ACTIONABLE);
        namelets.emplace_back(actionable);

        auto nonActionable = std::make_shared<Namelet>(Namelet::Type::BASE,
                                                       "//*[@clickable='false' and @long-clickable='false' and @checkable='false' and @scrollable='false']",
                                                       NamerFactory::bottomNamer());
        nonActionable->setMatchRule(Namelet::MatchRule::NON_ACTIONABLE);
        namelets.emplace_back(nonActionable);

        return std::make_shared<Naming>(namelets);
    }

    Namelet::AttributeFilter NamingFactory::buildFilterFromWidget(const WidgetPtr &widget) const {
        Namelet::AttributeFilter filter;
        if (!widget) {
            return filter;
        }
        filter.clazz = widget->getClassName();
        filter.resourceID = widget->getResourceID();
        if (!widget->getText().empty()) {
            filter.text = widget->getText();
        }
        if (!widget->getContentDesc().empty()) {
            filter.contentDesc = widget->getContentDesc();
        }
        return filter;
    }

    NamingPtr NamingFactory::refineForAliasedAction(const NamingPtr &current, const StatePtr &state) const {
        if (!current || !state) {
            return current;
        }
        std::vector<ElementPtr> affectedTrees = state->getTreeHistory();
        for (const auto &action : state->targetActions()) {
            auto target = action->getTarget();
            if (!target) {
                continue;
            }
            int aliasedCount = static_cast<int>(action->getResolvedNodes().size());
            if (aliasedCount == 0) {
                aliasedCount = state->getMergedWidgetCount(target->hash());
            }
            if (aliasedCount <= ACTION_REFINEMENT_THRESHOLD) {
                continue;
            }
            NameletPtr currentNamelet = current->resolveNamelet(target);
            if (!currentNamelet || !currentNamelet->getNamer()) {
                continue;
            }
            auto refinedNamers = NamerFactory::getSortedAbove(currentNamelet->getNamer());
            Namelet::AttributeFilter filter = buildFilterFromWidget(target);
            for (const auto &refined : refinedNamers) {
                if (!refined || currentNamelet->getNamer()->refinesTo(refined)) {
                    continue;
                }
                auto newNamelet = std::make_shared<Namelet>(currentNamelet->getExprString(), refined);
                newNamelet->setParent(currentNamelet);
                newNamelet->setMatchRule(Namelet::MatchRule::ATTRIBUTES);
                newNamelet->setAttributeFilter(filter);
                NamingPtr newNaming = current->extend(currentNamelet, newNamelet);
                if (!checkPredicate(newNaming, affectedTrees)) {
                    continue;
                }
                addPredicate(std::make_shared<AssertActionDivergent>(newNaming, affectedTrees,
                                                                     target->hash()));
                return newNaming;
            }
        }
        return current;
    }

    NamingPtr NamingFactory::abstractIfTooFine(const NamingPtr &current, size_t stateCount) const {
        if (!current) {
            return current;
        }
        auto parent = current->getParent();
        if (!parent) {
            return current;
        }
        int threshold = getMaxStatesForRefinementThreshold(current);
        if (stateCount > static_cast<size_t>(threshold)) {
            return parent;
        }
        return current;
    }

    NamingPtr NamingFactory::batchAbstract(const NamingPtr &targetNaming,
                                           const StatePtr &initialState,
                                           const NamingPtr &targetParentNaming,
                                           const StatePtrSet &targetStates,
                                           const GraphPtr &graph) const {
        // APE alignment: batchAbstract signature matches APE's implementation
        // Step 1: Validate inputs
        if (!targetNaming || !initialState || !targetParentNaming || !graph) {
            return targetNaming;
        }
        if (!initialState->getLatestTree()) {
            return targetNaming;
        }
        
        // Step 2: Filter targets - find states in the same parent abstract block
        // APE: filterTargets(initialState, targetNaming, targetStates)
        // In parent naming, compute originStateKey for initialState
        StateKeyPtr originStateKey = buildStateKeyForTree(initialState->getLatestTree(),
                                                         initialState->getActivityString(),
                                                         targetParentNaming);
        if (!originStateKey) {
            return targetNaming;
        }
        
        // Filter targetStates: collect states that have the same parent StateKey
        std::vector<StatePtr> affectedStates;
        std::set<uintptr_t> targetKeys;
        std::set<ElementPtr> affectedTrees;
        
        // Use provided targetStates if available, otherwise fall back to graph->getStatesByNaming
        StatePtrSet statesToCheck = targetStates.empty() 
            ? graph->getStatesByNaming(targetNaming) 
            : targetStates;
        
        for (const auto &state : statesToCheck) {
            if (!state || state->getTreeHistory().empty()) {
                continue;
            }
            // Check if state belongs to the same parent abstract block
            StateKeyPtr parentKey = buildStateKeyForTree(state->getLatestTree(),
                                                         state->getActivityString(),
                                                         targetParentNaming);
            if (!parentKey || parentKey->hash() != originStateKey->hash()) {
                continue;
            }
            affectedStates.emplace_back(state);
            // Collect all trees from affected states
            for (const auto &tree : state->getTreeHistory()) {
                affectedTrees.insert(tree);
                // Compute StateKey in targetNaming for this tree
                StateKeyPtr key = buildStateKeyForTree(tree, state->getActivityString(), targetNaming);
                if (key) {
                    targetKeys.insert(key->hash());
                }
            }
        }
        
        // Step 3: Check if abstraction is needed
        const int affectedThreshold = 8;
        int threshold = getMaxStatesForRefinementThreshold(targetNaming);
        if (static_cast<int>(affectedStates.size()) <= affectedThreshold &&
            static_cast<int>(targetKeys.size()) <= threshold) {
            // Current granularity is acceptable, no abstraction needed
            return targetNaming;
        }
        
        // Step 4: Execute abstraction (APE alignment)
        // - Blacklist the targetNaming for affected trees
        // - Remove conflicting predicates
        // - Add AssertStatesFewerThan predicate
        // - Return parent naming
        std::vector<ElementPtr> trees(affectedTrees.begin(), affectedTrees.end());
        blacklistNaming(trees, targetNaming);
        removeConflictingPredicates(targetParentNaming, trees);
        addPredicate(std::make_shared<AssertStatesFewerThan>(targetParentNaming, trees, threshold));
        
        return targetParentNaming;
    }

    namespace {
        struct StateKeyCacheKey {
            const void *tree{};
            const void *naming{};
            std::string activity;

            bool operator==(const StateKeyCacheKey &o) const {
                return tree == o.tree && naming == o.naming && activity == o.activity;
            }
        };

        struct StateKeyCacheKeyHash {
            size_t operator()(const StateKeyCacheKey &k) const noexcept {
                size_t h = 0;
                h ^= std::hash<const void *>{}(k.tree) + 0x9e3779b9 + (h << 6) + (h >> 2);
                h ^= std::hash<const void *>{}(k.naming) + 0x9e3779b9 + (h << 6) + (h >> 2);
                h ^= std::hash<std::string>{}(k.activity) + 0x9e3779b9 + (h << 6) + (h >> 2);
                return h;
            }
        };

        struct NameCacheKey {
            const void *tree{};
            const void *naming{};
            std::string activity;
            uintptr_t widgetHash{};

            bool operator==(const NameCacheKey &o) const {
                return tree == o.tree && naming == o.naming && activity == o.activity && widgetHash == o.widgetHash;
            }
        };

        struct NameCacheKeyHash {
            size_t operator()(const NameCacheKey &k) const noexcept {
                size_t h = 0;
                h ^= std::hash<const void *>{}(k.tree) + 0x9e3779b9 + (h << 6) + (h >> 2);
                h ^= std::hash<const void *>{}(k.naming) + 0x9e3779b9 + (h << 6) + (h >> 2);
                h ^= std::hash<std::string>{}(k.activity) + 0x9e3779b9 + (h << 6) + (h >> 2);
                h ^= std::hash<uintptr_t>{}(k.widgetHash) + 0x9e3779b9 + (h << 6) + (h >> 2);
                return h;
            }
        };

        // Memoized helpers for building StateKey / Name. These are meant to be
        // used with small, function-scope caches to avoid repeated ReuseState
        // construction in hot loops.
        StateKeyPtr buildStateKeyForTree(const ElementPtr &tree,
                                         const stringPtr &activity,
                                         const NamingPtr &naming) {
            if (!tree || !activity || !naming) {
                return nullptr;
            }
            ReuseStatePtr temp = ReuseState::create(tree, activity, naming);
            if (!temp) {
                return nullptr;
            }
            return temp->getStateKey();
        }

        StateKeyPtr buildStateKeyForTreeCached(
                const ElementPtr &tree,
                const stringPtr &activity,
                const NamingPtr &naming,
                std::unordered_map<StateKeyCacheKey, StateKeyPtr, StateKeyCacheKeyHash> &cache) {
            if (!tree || !activity || !naming) {
                return nullptr;
            }
            StateKeyCacheKey key{tree.get(), naming.get(), *activity};
            auto it = cache.find(key);
            if (it != cache.end()) {
                return it->second;
            }
            StateKeyPtr created = buildStateKeyForTree(tree, activity, naming);
            cache.emplace(std::move(key), created);
            return created;
        }

        NamePtr buildNameForTarget(const ElementPtr &tree,
                                   const stringPtr &activity,
                                   const NamingPtr &naming,
                                   uintptr_t widgetHash) {
            if (!tree || !activity || !naming) {
                return nullptr;
            }
            ReuseStatePtr temp = ReuseState::create(tree, activity, naming);
            if (!temp) {
                return nullptr;
            }
            return temp->getNameForWidgetHash(widgetHash);
        }

        NamePtr buildNameForTargetCached(
                const ElementPtr &tree,
                const stringPtr &activity,
                const NamingPtr &naming,
                uintptr_t widgetHash,
                std::unordered_map<NameCacheKey, NamePtr, NameCacheKeyHash> &cache) {
            if (!tree || !activity || !naming) {
                return nullptr;
            }
            NameCacheKey key{tree.get(), naming.get(), *activity, widgetHash};
            auto it = cache.find(key);
            if (it != cache.end()) {
                return it->second;
            }
            NamePtr created = buildNameForTarget(tree, activity, naming, widgetHash);
            cache.emplace(std::move(key), created);
            return created;
        }

        bool isIsomorphicNode(const ElementPtr &a, const ElementPtr &b) {
            if (!a || !b) {
                return false;
            }
            if (a->getIndex() != b->getIndex()) {
                return false;
            }
            if (a->getClassname() != b->getClassname()) {
                return false;
            }
            if (a->getPackageName() != b->getPackageName()) {
                return false;
            }
            if (a->getResourceID() != b->getResourceID()) {
                return false;
            }
            if (a->getEnable() != b->getEnable()) {
                return false;
            }
            if (a->getText() != b->getText()) {
                return false;
            }
            if (a->getContentDesc() != b->getContentDesc()) {
                return false;
            }
            if (a->getClickable() != b->getClickable()) {
                return false;
            }
            if (a->getCheckable() != b->getCheckable()) {
                return false;
            }
            if (a->getLongClickable() != b->getLongClickable()) {
                return false;
            }
            if (a->getScrollable() != b->getScrollable()) {
                return false;
            }
            const auto &childrenA = a->getChildren();
            const auto &childrenB = b->getChildren();
            if (childrenA.size() != childrenB.size()) {
                return false;
            }
            for (size_t i = 0; i < childrenA.size(); i++) {
                if (!isIsomorphicNode(childrenA[i], childrenB[i])) {
                    return false;
                }
            }
            return true;
        }

        bool isTopNamingEquivalent(const ElementPtr &t1,
                                   const ElementPtr &t2,
                                   const stringPtr &activity,
                                   const NamingPtr &topNaming) {
            if (!t1 || !t2 || !activity || !topNaming) {
                return false;
            }
            StateKeyPtr key1 = buildStateKeyForTree(t1, activity, topNaming);
            StateKeyPtr key2 = buildStateKeyForTree(t2, activity, topNaming);
            if (!key1 || !key2) {
                return false;
            }
            return key1->hash() == key2->hash();
        }
    }

    NamingPtr NamingFactory::refineForNonDeterminism(const NamingPtr &current,
                                                     const StateTransitionPtr &st1,
                                                     const StateTransitionPtr &st2,
                                                     const GraphPtr &graph,
                                                     const std::function<bool(const NamingPtr &)> &isLeaf) const {
        if (!current || !st1 || !st2 || !graph) {
            return current;
        }
        StatePtr source = st1->getSource();
        if (!source) {
            return current;
        }
        if (source->getStateKey() && source->getStateKey()->getWidgets().size() >
                                     static_cast<size_t>(MAX_INITIAL_NAMES_PER_STATE_THRESHOLD)) {
            return current;
        }
        if (source->getTreeHistory().size() > static_cast<size_t>(MAX_GUI_TREES_PER_STATE)) {
            return current;
        }
        if (source->getActivityString()) {
            auto statesInActivity = graph->getStatesByActivity(*(source->getActivityString().get()));
            if (statesInActivity.size() > static_cast<size_t>(MAX_STATES_PER_ACTIVITY)) {
                return current;
            }
        }
        stringPtr activity = source->getActivityString();
        ElementPtr tree1 = st1->getSourceTree();
        ElementPtr tree2 = st2->getSourceTree();
        if (!tree1 || !tree2) {
            return current;
        }
        if (isTopNamingEquivalent(tree1, tree2, activity, getTopNaming()) &&
            isIsomorphicNode(tree1, tree2)) {
            return current;
        }
        bool actionRefinementFirst = (ACTION_REFINEMENT_FIRST != 0);

        auto tryActionRefinement = [&]() -> NamingPtr {
            std::unordered_map<StateKeyCacheKey, StateKeyPtr, StateKeyCacheKeyHash> stateKeyCache;
            std::unordered_map<NameCacheKey, NamePtr, NameCacheKeyHash> nameCache;
            stateKeyCache.reserve(128);
            nameCache.reserve(128);

            uintptr_t targetHash = st1->getTargetWidgetHash();
            if (targetHash == 0) {
                return nullptr;
            }
            ActivityStateActionPtr sourceAction =
                    st1->getSource()->getActionByTargetHash(st1->getActionType(), targetHash);
            if (!sourceAction || !sourceAction->getTarget()) {
                return nullptr;
            }
            NameletPtr currentNamelet = current->resolveNamelet(sourceAction->getTarget());
            if (!currentNamelet || !currentNamelet->getNamer()) {
                return nullptr;
            }
            auto refinedNamers = NamerFactory::getSortedAbove(currentNamelet->getNamer());
            std::vector<NamerPtr> upperBounds;
            for (const auto &refined : refinedNamers) {
                bool skip = false;
                for (const auto &upper : upperBounds) {
                    if (refined && upper && refined->refinesTo(upper)) {
                        skip = true;
                        break;
                    }
                }
                if (skip) {
                    continue;
                }
                if (!refined || currentNamelet->getNamer()->refinesTo(refined)) {
                    continue;
                }
                NameletPtr newNamelet = std::make_shared<Namelet>(currentNamelet->getExprString(), refined);
                newNamelet->setParent(currentNamelet);
                newNamelet->setMatchRule(Namelet::MatchRule::ATTRIBUTES);
                newNamelet->setAttributeFilter(buildFilterFromWidget(sourceAction->getTarget()));
                NamingPtr newNaming = nullptr;
#if ENABLE_REPLACING_NAMELET
                if (current->isReplaceable(currentNamelet) &&
                    (!isLeaf || isLeaf(current))) {
                    newNaming = current->replaceLast(currentNamelet, newNamelet);
                }
#endif
                if (!newNaming) {
                    newNaming = current->extend(currentNamelet, newNamelet);
                }
                std::vector<ElementPtr> affectedTrees = source->getTreeHistory();
                if (!checkPredicate(newNaming, affectedTrees)) {
                    continue;
                }
                NamePtr name1 = buildNameForTargetCached(tree1, activity, newNaming, targetHash, nameCache);
                NamePtr name2 = buildNameForTargetCached(tree2, activity, newNaming, targetHash, nameCache);
                if (!name1 || !name2) {
                    continue;
                }
                if (name1->hash() == name2->hash()) {
                    continue;
                }
                int threshold = getMaxStatesForRefinementThreshold(newNaming);
                std::set<uintptr_t> states;
                for (const auto &tree : affectedTrees) {
                    StateKeyPtr key = buildStateKeyForTreeCached(tree, activity, newNaming, stateKeyCache);
                    if (key) {
                        states.insert(key->hash());
                        if (static_cast<int>(states.size()) > threshold) {
                            upperBounds.emplace_back(refined);
                            skip = true;
                            break;
                        }
                    }
                }
                if (skip) {
                    continue;
                }
                addPredicate(std::make_shared<AssertActionDivergent>(newNaming, affectedTrees, targetHash));
                return newNaming;
            }
            return nullptr;
        };

        auto tryStateRefinement = [&]() -> NamingPtr {
            std::unordered_map<StateKeyCacheKey, StateKeyPtr, StateKeyCacheKeyHash> stateKeyCache;
            stateKeyCache.reserve(256);

            std::set<uintptr_t> distinct;
            for (const auto &action : source->targetActions()) {
                if (!action || !action->getTarget()) {
                    continue;
                }
                uintptr_t widgetHash = action->getTarget()->hash();
                if (distinct.count(widgetHash)) {
                    continue;
                }
                distinct.insert(widgetHash);
                NameletPtr currentNamelet = current->resolveNamelet(action->getTarget());
                if (!currentNamelet || !currentNamelet->getNamer()) {
                    continue;
                }
                auto refinedNamers = NamerFactory::getSortedAbove(currentNamelet->getNamer());
                std::vector<NamerPtr> upperBounds;
                for (const auto &refined : refinedNamers) {
                    bool skip = false;
                    for (const auto &upper : upperBounds) {
                        if (refined && upper && refined->refinesTo(upper)) {
                            skip = true;
                            break;
                        }
                    }
                    if (skip) {
                        continue;
                    }
                    if (!refined || currentNamelet->getNamer()->refinesTo(refined)) {
                        continue;
                    }
                    NameletPtr newNamelet = std::make_shared<Namelet>(currentNamelet->getExprString(), refined);
                    newNamelet->setParent(currentNamelet);
                    newNamelet->setMatchRule(Namelet::MatchRule::ATTRIBUTES);
                    newNamelet->setAttributeFilter(buildFilterFromWidget(action->getTarget()));
                    NamingPtr newNaming = nullptr;
#if ENABLE_REPLACING_NAMELET
                    if (current->isReplaceable(currentNamelet) &&
                        (!isLeaf || isLeaf(current))) {
                        newNaming = current->replaceLast(currentNamelet, newNamelet);
                    }
#endif
                    if (!newNaming) {
                        newNaming = current->extend(currentNamelet, newNamelet);
                    }
                    std::vector<ElementPtr> affectedTrees = source->getTreeHistory();
                    if (!checkPredicate(newNaming, affectedTrees)) {
                        continue;
                    }
                    StateKeyPtr key1 = buildStateKeyForTreeCached(tree1, activity, newNaming, stateKeyCache);
                    StateKeyPtr key2 = buildStateKeyForTreeCached(tree2, activity, newNaming, stateKeyCache);
                    if (!key1 || !key2 || key1->hash() == key2->hash()) {
                        continue;
                    }
                    int threshold = getMaxStatesForRefinementThreshold(newNaming);
                    std::set<uintptr_t> keys;
                    for (const auto &s : graph->getStatesByNaming(current)) {
                        if (!s) {
                            continue;
                        }
                        for (const auto &tree : s->getTreeHistory()) {
                            StateKeyPtr key = buildStateKeyForTreeCached(tree, s->getActivityString(), newNaming,
                                                                         stateKeyCache);
                            if (key) {
                                keys.insert(key->hash());
                                if (static_cast<int>(keys.size()) > threshold) {
                                    key.reset();
                                    break;
                                }
                            }
                        }
                        if (static_cast<int>(keys.size()) > threshold) {
                            break;
                        }
                    }
                    if (static_cast<int>(keys.size()) > threshold) {
                        upperBounds.emplace_back(refined);
                        continue;
                    }
                    std::vector<ElementPtr> group1 = {tree1};
                    std::vector<ElementPtr> group2 = {tree2};
                    addPredicate(std::make_shared<AssertSourceDivergent>(newNaming, group1, group2));
                    return newNaming;
                }
            }
            return nullptr;
        };

        NamingPtr updated = nullptr;
        if (actionRefinementFirst) {
            updated = tryActionRefinement();
            if (!updated) {
                updated = tryStateRefinement();
            }
        } else {
            updated = tryStateRefinement();
            if (!updated) {
                updated = tryActionRefinement();
            }
        }
        return updated ? updated : current;
    }

    int NamingFactory::getMaxStatesForRefinementThreshold(const NamingPtr &naming) const {
        if (!naming) {
            return 1;
        }
        int fineness = naming->getFineness();
        int total = static_cast<int>(NamerFactory::usedTypes().size());
        int shift = total - fineness;
        int threshold = std::min(8, std::max(1, 2 << shift));
        return threshold;
    }

} // namespace fastbotx

#endif // FASTBOTX_NAMING_FACTORY_CPP_
