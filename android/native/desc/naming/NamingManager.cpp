/*
 * This code is licensed under the Fastbot license. You may obtain a copy of this license in the LICENSE.txt file in the root directory of this source tree.
 */
/**
 * @authors Jianqiang Guo, Yuhui Su, Zhao Zhang
 */
#ifndef FASTBOTX_NAMING_MANAGER_CPP_
#define FASTBOTX_NAMING_MANAGER_CPP_

#include "NamingManager.h"
#include "../../utils.hpp"
#include "../reuse/ReuseState.h"

namespace fastbotx {

    NamingManager::NamingManager(std::shared_ptr<NamingFactory> factory)
            : _factory(std::move(factory)) {
        if (!_factory) {
            _factory = std::make_shared<NamingFactory>();
        }
        _currentNaming = _factory->getBaseNaming();
    }

    NamingPtr NamingManager::getNamingForTree(const ElementPtr &tree, const stringPtr &activity) const {
        NamingPtr current = _factory->getBaseNaming();
        // Loop protection: track visited Naming objects directly instead of
        // relying on hash values (hash collisions, while unlikely, would
        // prematurely terminate the traversal).
        std::set<NamingPtr, Comparator<Naming>> visited;
        // Follow recorded refinement / abstraction edges starting from the
        // base naming until no further transition applies for this tree.
        while (current && tree) {
            ReuseStatePtr temp = ReuseState::create(tree, activity, current);
            if (!temp || !temp->getStateKey()) {
                break;
            }
            StateKeyPtr key = temp->getStateKey();
            auto edgeIt = _namingEdges.find(current);
            if (edgeIt == _namingEdges.end()) {
                break;
            }
            auto stateIt = edgeIt->second.find(key);
            if (stateIt == edgeIt->second.end()) {
                break;
            }
            NamingPtr next = stateIt->second;
            if (!next || visited.count(next) > 0) {
                break;
            }
            visited.insert(next);
            current = next;
        }
        return current ? current : _factory->getBaseNaming();
    }

    NamingPtr NamingManager::evolveNaming(const StatePtr &state, const GraphPtr &graph) {
        if (!state || !graph) {
            return _currentNaming;
        }
        NamingPtr updated = _currentNaming;
        updated = _factory->refineForAliasedAction(updated, state);
        // APE alignment: batchAbstract needs targetParentNaming and targetStates
        // For evolveNaming, we use current naming's parent and collect states by naming
        if (updated && updated->getParent()) {
            StatePtrSet targetStates = graph->getStatesByNaming(updated);
            updated = _factory->batchAbstract(updated, state, updated->getParent(), targetStates, graph);
        }
        if (updated && updated != _currentNaming) {
            _currentNaming = updated;
            updateNamingForTrees(state->getTreeHistory(), state->getCurrentNaming(), updated,
                                 state->getActivityString());
            BLOG("Naming updated: fineness=%d", _currentNaming->getFineness());
            _version++;
        }
        return _currentNaming;
    }

    NamingPtr NamingManager::resolveNonDeterminism(const NamingPtr &current,
                                                   const StateTransitionPtr &st1,
                                                   const StateTransitionPtr &st2,
                                                   const GraphPtr &graph) {
        if (!current || !st1 || !st2 || !graph) {
            return _currentNaming;
        }
        auto leafChecker = [this](const NamingPtr &naming) {
            return this->isLeaf(naming);
        };
        NamingPtr updated = _factory->refineForNonDeterminism(current, st1, st2, graph, leafChecker);
        if (updated && updated != _currentNaming) {
            _currentNaming = updated;
            std::vector<ElementPtr> affected;
            if (st1->getSource()) {
                for (const auto &tree : st1->getSource()->getTreeHistory()) {
                    affected.emplace_back(tree);
                }
            }
            if (st2->getSource()) {
                for (const auto &tree : st2->getSource()->getTreeHistory()) {
                    affected.emplace_back(tree);
                }
            }
            updateNamingForTrees(affected, current, updated,
                                 st1->getSource() ? st1->getSource()->getActivityString() : nullptr);
            BLOG("Naming updated (nondet): fineness=%d", _currentNaming->getFineness());
            _version++;
        }
        return _currentNaming;
    }

    void NamingManager::updateNamingForTrees(const std::vector<ElementPtr> &trees,
                                             const NamingPtr &oldOne,
                                             const NamingPtr &newOne,
                                             const stringPtr &activity) {
        if (!oldOne || !newOne || oldOne == newOne) {
            return;
        }
        for (const auto &tree : trees) {
            if (!tree) {
                continue;
            }
            NamingPtr current = getNamingForTree(tree, activity);
            if (current == newOne) {
                continue;
            }
            if (oldOne == newOne->getParent()) { // state refinement
                ReuseStatePtr temp = ReuseState::create(tree, activity, oldOne);
                if (!temp || !temp->getStateKey()) {
                    continue;
                }
                StateKeyPtr key = temp->getStateKey();
                _namingEdges[oldOne][key] = newOne;
            } else if (newOne->isAncestor(oldOne)) { // state abstraction
                NamingPtr child = oldOne;
                NamingPtr parent = child->getParent();
                while (parent) {
                    ReuseStatePtr temp = ReuseState::create(tree, activity, parent);
                    if (!temp || !temp->getStateKey()) {
                        break;
                    }
                    StateKeyPtr key = temp->getStateKey();
                    auto edgeIt = _namingEdges.find(parent);
                    if (edgeIt != _namingEdges.end()) {
                        edgeIt->second.erase(key);
                    }
                    if (parent == newOne) {
                        break;
                    }
                    child = parent;
                    parent = child->getParent();
                }
            } else if (oldOne->getParent() == newOne->getParent()) { // replacement
                NamingPtr parent = newOne->getParent();
                if (!parent) {
                    continue;
                }
                ReuseStatePtr temp = ReuseState::create(tree, activity, parent);
                if (!temp || !temp->getStateKey()) {
                    continue;
                }
                StateKeyPtr key = temp->getStateKey();
                _namingEdges[parent][key] = newOne;
            }
        }
    }

    bool NamingManager::isLeaf(const NamingPtr &naming) const {
        auto it = _namingEdges.find(naming);
        return it == _namingEdges.end() || it->second.empty();
    }

    NamingPtr NamingManager::refineForAliasedAction(const ActivityStateActionPtr &action) {
        if (!action || !_currentNaming) {
            return _currentNaming;
        }
        StatePtr state = action->getState().lock();
        if (!state) {
            return _currentNaming;
        }
        NamingPtr updated = _factory->refineForAliasedAction(_currentNaming, state);
        if (updated && updated != _currentNaming) {
            _currentNaming = updated;
            _version++;
        }
        return _currentNaming;
    }

    NamingPtr NamingManager::batchAbstract(const NamingPtr &targetNaming,
                                          const StatePtr &initialState,
                                          const NamingPtr &targetParentNaming,
                                          const StatePtrSet &targetStates,
                                          const GraphPtr &graph) {
        if (!targetNaming || !initialState || !targetParentNaming || !graph) {
            return _currentNaming;
        }
        NamingPtr updated = _factory->batchAbstract(targetNaming, initialState, targetParentNaming, targetStates, graph);
        if (updated && updated != _currentNaming) {
            _currentNaming = updated;
            updateNamingForTrees(initialState->getTreeHistory(), targetNaming, updated,
                                initialState->getActivityString());
            _version++;
        }
        return _currentNaming;
    }

} // namespace fastbotx

#endif // FASTBOTX_NAMING_MANAGER_CPP_
