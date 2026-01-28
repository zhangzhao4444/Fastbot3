/*
 * This code is licensed under the Fastbot license. You may obtain a copy of this license in the LICENSE.txt file in the root directory of this source tree.
 */
/**
 * @authors Jianqiang Guo, Yuhui Su, Zhao Zhang
 */
#ifndef  Graph_CPP_
#define  Graph_CPP_


#include "Graph.h"
#include "../utils.hpp"
#include <vector>


namespace fastbotx {

    namespace {
        uintptr_t makeActionKey(ActionType type, uintptr_t targetHash) {
            return (static_cast<uintptr_t>(type) << 48) ^ targetHash;
        }
    }

    StateTransition::StateTransition(StatePtr sourceState,
                                     ActionType actionType,
                                     uintptr_t targetWidgetHash,
                                     StatePtr targetState,
                                     ElementPtr sourceTree,
                                     ElementPtr targetTree)
            : _source(std::move(sourceState)),
              _target(std::move(targetState)),
              _actionType(actionType),
              _targetWidgetHash(targetWidgetHash),
              _sourceTree(std::move(sourceTree)),
              _targetTree(std::move(targetTree)),
              _visitCount(1) {
        _hashcode = (reinterpret_cast<uintptr_t>(_source.get()) << 2)
                    ^ (reinterpret_cast<uintptr_t>(_target.get()) << 1)
                    ^ (static_cast<uintptr_t>(_actionType) << 6)
                    ^ _targetWidgetHash;
    }


    /**
     * @brief Constructor for Graph class
     * Initializes the graph with zero distribution count and zero timestamp
     */
    Graph::Graph()
            : _totalDistri(0), _timeStamp(0) {

    }

    /**
     * @brief Default distribution pair for activity statistics
     * First value: count of how many times the state is visited
     * Second value: percentage of times that this state been accessed over all states
     */
    const std::pair<int, double> Graph::_defaultDistri = std::make_pair(0, 0.0);

    /**
     * @brief Add a state to the graph, or return existing state if already present
     * 
     * This method performs the following operations:
     * 1. Checks if the state already exists in the graph (by hash comparison)
     * 2. If new: assigns an ID and adds to the state set
     * 3. If existing: fills details if the existing state has no details
     * 4. Notifies all listeners about the new/existing state
     * 5. Updates activity statistics (visit count and percentage)
     * 6. Processes and indexes all actions from this state
     * 
     * @param state The state to add to the graph
     * @return StatePtr The state that was added or the existing matching state
     * 
     * @note Performance: O(log n) for state lookup, O(m log k) for action processing
     *       where n is number of states, m is number of actions, k is number of unique actions
     */
    StatePtr Graph::addState(StatePtr state) {
        // Update graph timestamp (monotonic) when a new state snapshot is processed.
        // Used by Node::visit() (reserved) and GUITreeTransition timestamp ordering.
        _timeStamp.fetch_add(1);

        // Get the activity name (activity class name) of this new state
        auto activity = state->getActivityString();
        
        // Try to find state in state cache using hash-based comparison
        auto ifStateExists = this->_states.find(state);
        
        if (ifStateExists == this->_states.end()) {
            // This is a brand-new state, add it to the state cache
            state->setId(static_cast<int>(this->_states.size()));
            this->_states.emplace(state);
        } else {
            // State already exists, fill details if needed
            if ((*ifStateExists)->hasNoDetail()) {
                (*ifStateExists)->fillDetails(state);
            }
            // Use the existing state instead of the new one
            state = *ifStateExists;
        }

        // Notify all registered listeners about the new/existing state
        this->notifyNewStateEvents(state);

        // Add this activity name to the visited activities set (every name is unique)
        this->_visitedActivities.emplace(activity);
        
        // Update total distribution count
        this->_totalDistri++;
        
        // Update activity distribution statistics
        // Performance optimization: avoid creating string copy, use reference to shared_ptr's string
        const std::string& activityStr = *(activity.get());
        // Insert default if missing (single lookup).
        auto distriInsert = this->_activityDistri.emplace(activityStr, _defaultDistri);
        auto distriIt = distriInsert.first;
        
        // Update visit count and percentage
        distriIt->second.first++;
        distriIt->second.second = 1.0 * distriIt->second.first / this->_totalDistri;
        
        // Index state by naming for abstraction/refinement
        if (state && state->getCurrentNaming()) {
            this->_statesByNaming[state->getCurrentNaming()].emplace(state);
        }

        // Process and index all actions from this state
        addActionFromState(state);
        
        return state;
    }

    TransitionVisitType Graph::addTransition(const StatePtr &source,
                                             const ActivityStateActionPtr &action,
                                             const StatePtr &target,
                                             const ElementPtr &sourceTree,
                                             const ElementPtr &targetTree,
                                             StateTransitionPtr &existingTransition,
                                             StateTransitionPtr &newTransition) {
        existingTransition = nullptr;
        newTransition = nullptr;
        if (!source || !action || !target) {
            return TransitionVisitType::EXISTING;
        }
        uintptr_t targetHash = 0;
        if (action->requireTarget() && action->getTarget()) {
            targetHash = action->getTarget()->hash();
        }
        uintptr_t actionKey = makeActionKey(action->getActionType(), targetHash);
        std::pair<uintptr_t, uintptr_t> key = {source->hash(), actionKey};
        auto &bucket = _transitionsByAction[key];
        for (const auto &transition : bucket) {
            if (transition && transition->getTarget() == target) {
                transition->incrementVisit();
                existingTransition = transition;
                // APE alignment: Append GUITreeTransition to existing transition
                if (sourceTree && targetTree) {
                    GUITreeTransitionPtr treeTransition = std::make_shared<GUITreeTransition>(
                        sourceTree, action, targetTree);
                    transition->appendGUITreeTransition(treeTransition);
                    treeTransition->setCurrentStateTransition(transition);
                }
                return TransitionVisitType::EXISTING;
            }
        }
        newTransition = std::make_shared<StateTransition>(source, action->getActionType(), targetHash,
                                                          target, sourceTree, targetTree);
        TransitionVisitType visitType = TransitionVisitType::NEW_ACTION;
        if (!bucket.empty()) {
            existingTransition = bucket.front();
            visitType = TransitionVisitType::NEW_ACTION_TARGET;
        }
        bucket.emplace_back(newTransition);
        _transitions.emplace_back(newTransition);
        
        // APE alignment: Create and append GUITreeTransition
        if (newTransition && sourceTree && targetTree) {
            GUITreeTransitionPtr treeTransition = std::make_shared<GUITreeTransition>(
                sourceTree, action, targetTree);
            newTransition->appendGUITreeTransition(treeTransition);
            treeTransition->setCurrentStateTransition(newTransition);
            
            // Add to tree transition history (APE alignment)
            _treeTransitionHistory.push_back(newTransition);
        }
        
        // Check if source tree is an entry tree
        if (sourceTree && _entryGUITrees.find(sourceTree) != _entryGUITrees.end()) {
            _entryStates.insert(source);
        }
        if (sourceTree && _cleanEntryGUITrees.find(sourceTree) != _cleanEntryGUITrees.end()) {
            _cleanEntryStates.insert(source);
        }
        
        return visitType;
    }

    const StatePtrSet &Graph::getStatesByNaming(const NamingPtr &naming) const {
        static StatePtrSet empty;
        auto iter = this->_statesByNaming.find(naming);
        if (iter != this->_statesByNaming.end()) {
            return iter->second;
        }
        return empty;
    }

    StatePtrSet Graph::getStatesByActivity(const std::string &activity) const {
        StatePtrSet results;
        for (const auto &state : _states) {
            if (!state || !state->getActivityString()) {
                continue;
            }
            if (*(state->getActivityString().get()) == activity) {
                results.emplace(state);
            }
        }
        return results;
    }

    stringPtr Graph::findVisitedActivityPtr(const std::string &activity) const {
        for (const auto &p : _visitedActivities) {
            if (p && *p == activity) {
                return p;
            }
        }
        return nullptr;
    }

    std::vector<StateTransitionPtr> Graph::getOutStateTransitions(const ActivityStateActionPtr &action) const {
        std::vector<StateTransitionPtr> results;
        if (!action) {
            return results;
        }
        auto statePtr = action->getState().lock();
        if (!statePtr) {
            return results;
        }
        uintptr_t sourceHash = statePtr->hash();
        uintptr_t targetHash = 0;
        if (action->requireTarget() && action->getTarget()) {
            targetHash = action->getTarget()->hash();
        }
        uintptr_t actionKey = makeActionKey(action->getActionType(), targetHash);
        std::pair<uintptr_t, uintptr_t> key = {sourceHash, actionKey};
        auto it = _transitionsByAction.find(key);
        if (it != _transitionsByAction.end()) {
            results = it->second;
        }
        return results;
    }

    bool StateTransition::isSameActivity() const {
        if (!_source || !_target) {
            return false;
        }
        auto sourceActivity = _source->getActivityString();
        auto targetActivity = _target->getActivityString();
        if (!sourceActivity || !targetActivity) {
            return false;
        }
        return *sourceActivity == *targetActivity;
    }

    void StateTransition::appendGUITreeTransition(const GUITreeTransitionPtr &treeTransition) {
        if (treeTransition) {
            _treeTransitions.push_back(treeTransition);
            // Note: setCurrentStateTransition will be called from Graph::addTransition
            // where we have access to the StateTransitionPtr
        }
    }

    GUITreeTransitionPtr StateTransition::getLastGUITreeTransition() const {
        if (_treeTransitions.empty()) {
            return nullptr;
        }
        return _treeTransitions.back();
    }

    int StateTransition::getTimestamp() const {
        // APE alignment: Get timestamp from first GUITreeTransition
        if (!_treeTransitions.empty() && _treeTransitions.front()) {
            return _treeTransitions.front()->getTimestamp();
        }
        return 0;
    }

    void Graph::addEntryGUITree(const ElementPtr &tree) {
        if (tree) {
            _entryGUITrees.insert(tree);
        }
    }

    void Graph::addCleanEntryGUITree(const ElementPtr &tree) {
        if (tree) {
            _cleanEntryGUITrees.insert(tree);
        }
    }

    bool Graph::isEntryState(const StatePtr &state) const {
        if (!state) {
            return false;
        }
        return _entryStates.find(state) != _entryStates.end();
    }

    bool Graph::isCleanEntryState(const StatePtr &state) const {
        if (!state) {
            return false;
        }
        return _cleanEntryStates.find(state) != _cleanEntryStates.end();
    }

    void Graph::rebuildHistory() {
        // Clear existing state transition history
        _stateTransitionHistory.clear();
        
        // Rebuild from tree transition history
        // In APE, this updates timestamps and visit counts
        // Native implementation: simply copy valid transitions
        for (const auto &transition : _treeTransitionHistory) {
            if (!transition) {
                continue;
            }
            // Check if transition is still valid (source and target states exist)
            if (transition->getSource() && transition->getTarget()) {
                // Check if states are still in the graph
                if (_states.find(transition->getSource()) != _states.end() &&
                    _states.find(transition->getTarget()) != _states.end()) {
                    _stateTransitionHistory.push_back(transition);
                }
            }
        }
        
        BLOG("Rebuilt state transition history: %zu transitions", _stateTransitionHistory.size());
    }

    void Graph::removeState(const StatePtr &state, std::vector<StateTransitionPtr> &removedTransitions) {
        if (!state) {
            return;
        }
        uintptr_t stateHash = state->hash();
        
        // Collect all transitions connected to this state
        std::vector<StateTransitionPtr> transitionsToRemove;
        
        // Find all transitions where this state is source or target
        for (auto it = _transitions.begin(); it != _transitions.end();) {
            const auto &transition = *it;
            if (!transition) {
                ++it;
                continue;
            }
            bool shouldRemove = false;
            if (transition->getSource() && transition->getSource()->hash() == stateHash) {
                shouldRemove = true;
            } else if (transition->getTarget() && transition->getTarget()->hash() == stateHash) {
                shouldRemove = true;
            }
            
            if (shouldRemove) {
                transitionsToRemove.push_back(transition);
                removedTransitions.push_back(transition);
                // Remove from _transitionsByAction
                if (transition->getSource()) {
                    uintptr_t sourceHash = transition->getSource()->hash();
                    uintptr_t actionKey = makeActionKey(transition->getActionType(), transition->getTargetWidgetHash());
                    std::pair<uintptr_t, uintptr_t> key = {sourceHash, actionKey};
                    auto actionIt = _transitionsByAction.find(key);
                    if (actionIt != _transitionsByAction.end()) {
                        auto &bucket = actionIt->second;
                        bucket.erase(std::remove(bucket.begin(), bucket.end(), transition), bucket.end());
                        if (bucket.empty()) {
                            _transitionsByAction.erase(actionIt);
                        }
                    }
                }
                it = _transitions.erase(it);
            } else {
                ++it;
            }
        }
        
        // Remove actions from visited/unvisited sets
        const auto &actions = state->getActions();
        for (const auto &action : actions) {
            if (!action) {
                continue;
            }
            _visitedActions.erase(action);
            _unvisitedActions.erase(action);
        }
        
        // Remove from _statesByNaming
        if (state->getCurrentNaming()) {
            auto namingIt = _statesByNaming.find(state->getCurrentNaming());
            if (namingIt != _statesByNaming.end()) {
                namingIt->second.erase(state);
                if (namingIt->second.empty()) {
                    _statesByNaming.erase(namingIt);
                }
            }
        }
        
        // Remove from entry states
        _entryStates.erase(state);
        _cleanEntryStates.erase(state);
        
        // Remove from _states
        _states.erase(state);
        
        BLOG("Removed state %lu with %zu transitions", static_cast<unsigned long>(stateHash), transitionsToRemove.size());
    }

    /**
     * @brief Notify all registered listeners about a new state being added
     * 
     * @param node The state node that was added to the graph
     */
    void Graph::notifyNewStateEvents(const StatePtr &node) {
        for (const auto &listener: this->_listeners) {
            listener->onAddNode(node);
        }
    }

    /**
     * @brief Add a listener to be notified when new states are added to the graph
     * 
     * @param listener The listener to register
     */
    void Graph::addListener(const GraphListenerPtr &listener) {
        this->_listeners.emplace_back(listener);
    }

    /**
     * @brief Process and index all actions from a state
     * 
     * This method performs the following operations for each action in the state:
     * 1. Checks if the action already exists in visited actions set
     * 2. If not visited, checks if it exists in unvisited actions set
     * 3. If new action, assigns a new ID and updates action counter
     * 4. Updates the appropriate set (visited/unvisited) based on action status
     * 
     * Performance optimization:
     * - Checks visited set first (typically smaller and more frequently accessed)
     * - Only checks unvisited set if action is not found in visited set
     * - Uses hash-based set lookup for O(log n) complexity
     * 
     * @param node The state node containing actions to process
     * 
     * @note Time complexity: O(m log k) where m is number of actions, k is number of unique actions
     */
    void Graph::addActionFromState(const StatePtr &node) {
        auto nodeActions = node->getActions();
        
        for (const auto &action: nodeActions) {
            // Performance optimization: check visited set first (typically smaller)
            auto itervisted = this->_visitedActions.find(action);
            if (itervisted != this->_visitedActions.end()) {
                // Action already exists in visited set, reuse its ID
                action->setId((*itervisted)->getIdi());
                // No need to check unvisited set
            } else {
                // Action not in visited set, check unvisited set
                auto iterunvisited = this->_unvisitedActions.find(action);
                if (iterunvisited != this->_unvisitedActions.end()) {
                    // Action exists in unvisited set, reuse its ID
                    action->setId((*iterunvisited)->getIdi());
                } else {
                    // New action, not in either set
                    // Assign new ID based on total action count
                    action->setId(static_cast<int>(this->_actionCounter.getTotal()));
                    // Update action counter statistics
                    this->_actionCounter.countAction(action);
                }
                
                // Update sets based on visited status
                // Move action to appropriate set if its visited status changed
                if (action->isVisited()) {
                    this->_visitedActions.emplace(action);
                    // Remove from unvisited if it was there (shouldn't happen, but safe)
                    this->_unvisitedActions.erase(action);
                } else {
                    this->_unvisitedActions.emplace(action);
                }
            }
        }
        
        BDLOG("unvisited action: %zu, visited action %zu", this->_unvisitedActions.size(),
              this->_visitedActions.size());
    }

    /**
     * @brief Destructor for Graph class
     * Clears all internal data structures to free memory
     */
    Graph::~Graph() {
        this->_states.clear();
        this->_unvisitedActions.clear();
        this->_visitedActions.clear();
        this->_widgetActions.clear();
        this->_listeners.clear();
        this->_statesByNaming.clear();
        this->_transitions.clear();
        this->_transitionsByAction.clear();
    }

}

#endif //Graph_CPP_
