/*
 * This code is licensed under the Fastbot license. You may obtain a copy of this license in the LICENSE.txt file in the root directory of this source tree.
 */
/**
 * @authors Jianqiang Guo, Yuhui Su, Zhao Zhang
 */
#ifndef FASTBOTX_PREDICATE_CPP_
#define FASTBOTX_PREDICATE_CPP_

#include "Predicate.h"
#include "../reuse/ReuseState.h"
#include <unordered_map>

namespace fastbotx {

    namespace {
        // Reuse a single shared empty activity string to avoid repeated
        // heap allocations in hot paths (predicates are evaluated often).
        const stringPtr kEmptyActivity = std::make_shared<std::string>("");

        // Helper: build a StateKey for a single tree under a given Naming.
        StateKeyPtr buildStateKeyForTree(const ElementPtr &tree,
                                         const NamingPtr &naming) {
            if (!tree || !naming) {
                return nullptr;
            }
            ReuseStatePtr temp = ReuseState::create(tree, kEmptyActivity, naming);
            if (!temp) {
                return nullptr;
            }
            return temp->getStateKey();
        }

        // Helper: build the Name corresponding to a target widget (identified
        // by hash) within the given tree and Naming.
        NamePtr buildNameForTarget(const ElementPtr &tree,
                                   const NamingPtr &naming,
                                   uintptr_t widgetHash) {
            if (!tree || !naming) {
                return nullptr;
            }
            ReuseStatePtr temp = ReuseState::create(tree, kEmptyActivity, naming);
            if (!temp) {
                return nullptr;
            }
            return temp->getNameForWidgetHash(widgetHash);
        }
    }

    AssertStatesFewerThan::AssertStatesFewerThan(NamingPtr naming,
                                                 std::vector<ElementPtr> trees,
                                                 int threshold)
            : _naming(std::move(naming)),
              _trees(std::move(trees)),
              _threshold(threshold) {
    }

    bool AssertStatesFewerThan::eval(const NamingPtr &naming,
                                     const std::vector<ElementPtr> &trees) const {
        NamingPtr check = naming ? naming : _naming;
        const std::vector<ElementPtr> &useTrees = trees.empty() ? _trees : trees;
        std::set<uintptr_t> keys;
        // Cache StateKey results within this evaluation call to avoid repeated
        // ReuseState construction when the same tree appears multiple times.
        std::unordered_map<ElementPtr, StateKeyPtr> keyCache;
        keyCache.reserve(useTrees.size());
        for (const auto &tree : useTrees) {
            StateKeyPtr key;
            auto it = keyCache.find(tree);
            if (it != keyCache.end()) {
                key = it->second;
            } else {
                key = buildStateKeyForTree(tree, check);
                keyCache.emplace(tree, key);
            }
            if (key) {
                keys.insert(key->hash());
                if (static_cast<int>(keys.size()) > _threshold) {
                    return false;
                }
            }
        }
        return true;
    }

    AssertSourceDivergent::AssertSourceDivergent(NamingPtr naming,
                                                 std::vector<ElementPtr> group1,
                                                 std::vector<ElementPtr> group2)
            : _naming(std::move(naming)),
              _group1(std::move(group1)),
              _group2(std::move(group2)) {
    }

    bool AssertSourceDivergent::eval(const NamingPtr &naming,
                                     const std::vector<ElementPtr> &trees) const {
        (void)trees;
        NamingPtr check = naming ? naming : _naming;
        std::set<uintptr_t> left;
        std::unordered_map<ElementPtr, StateKeyPtr> keyCache;
        keyCache.reserve(_group1.size() + _group2.size());
        for (const auto &tree : _group1) {
            StateKeyPtr key;
            auto it = keyCache.find(tree);
            if (it != keyCache.end()) {
                key = it->second;
            } else {
                key = buildStateKeyForTree(tree, check);
                keyCache.emplace(tree, key);
            }
            if (key) {
                left.insert(key->hash());
            }
        }
        for (const auto &tree : _group2) {
            StateKeyPtr key;
            auto it = keyCache.find(tree);
            if (it != keyCache.end()) {
                key = it->second;
            } else {
                key = buildStateKeyForTree(tree, check);
                keyCache.emplace(tree, key);
            }
            if (key && left.count(key->hash()) > 0) {
                return false;
            }
        }
        return true;
    }

    AssertActionDivergent::AssertActionDivergent(NamingPtr naming,
                                                 std::vector<ElementPtr> trees,
                                                 uintptr_t targetWidgetHash)
            : _naming(std::move(naming)),
              _trees(std::move(trees)),
              _targetWidgetHash(targetWidgetHash) {
    }

    bool AssertActionDivergent::eval(const NamingPtr &naming,
                                     const std::vector<ElementPtr> &trees) const {
        NamingPtr check = naming ? naming : _naming;
        const std::vector<ElementPtr> &useTrees = trees.empty() ? _trees : trees;
        std::set<uintptr_t> names;
        // Cache Name results within this evaluation call to avoid repeated
        // ReuseState construction when the same tree is checked multiple times.
        std::unordered_map<ElementPtr, NamePtr> nameCache;
        nameCache.reserve(useTrees.size());
        for (const auto &tree : useTrees) {
            NamePtr name;
            auto it = nameCache.find(tree);
            if (it != nameCache.end()) {
                name = it->second;
            } else {
                name = buildNameForTarget(tree, check, _targetWidgetHash);
                nameCache.emplace(tree, name);
            }
            if (!name) {
                continue;
            }
            if (names.count(name->hash()) > 0) {
                return false;
            }
            names.insert(name->hash());
        }
        return true;
    }

} // namespace fastbotx

#endif // FASTBOTX_PREDICATE_CPP_
