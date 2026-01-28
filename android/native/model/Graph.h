/*
 * This code is licensed under the Fastbot license. You may obtain a copy of this license in the LICENSE.txt file in the root directory of this source tree.
 */
/**
 * @authors Jianqiang Guo, Yuhui Su, Zhao Zhang
 */
#ifndef  Graph_H_
#define  Graph_H_

#include "State.h"
#include "Base.h"
#include "Action.h"
#include "Element.h"
#include "naming/Naming.h"
#include "GUITreeTransition.h"
#include <map>
#include <atomic>

namespace fastbotx {

    /**
     * @brief Map from widget to set of actions that can be performed on that widget
     * Used for quick lookup of available actions for a specific widget
     */
    typedef std::map<WidgetPtr, ActivityStateActionPtrSet, Comparator<Widget>> ModelActionPtrWidgetMap;
    
    /**
     * @brief Map from activity name string to set of states in that activity
     * Used for organizing states by their activity context
     */
    typedef std::map<std::string, StatePtrSet> StatePtrStrMap;

    class StateTransition : public HashNode {
    public:
        StateTransition(StatePtr sourceState,
                        ActionType actionType,
                        uintptr_t targetWidgetHash,
                        StatePtr targetState,
                        ElementPtr sourceTree,
                        ElementPtr targetTree);

        StatePtr getSource() const { return _source; }
        StatePtr getTarget() const { return _target; }
        ActionType getActionType() const { return _actionType; }
        uintptr_t getTargetWidgetHash() const { return _targetWidgetHash; }
        ElementPtr getSourceTree() const { return _sourceTree; }
        ElementPtr getTargetTree() const { return _targetTree; }

        /**
         * @brief Append a GUITreeTransition to this StateTransition (APE alignment)
         * 
         * @param treeTransition The GUITreeTransition to append
         */
        void appendGUITreeTransition(const GUITreeTransitionPtr &treeTransition);

        /**
         * @brief Get all GUITreeTransitions (APE alignment)
         * 
         * @return Const reference to GUITreeTransition vector
         */
        const GUITreeTransitionPtrVec &getGUITreeTransitions() const { return _treeTransitions; }

        /**
         * @brief Get the last GUITreeTransition (APE alignment)
         * 
         * @return Last GUITreeTransition, or nullptr if empty
         */
        GUITreeTransitionPtr getLastGUITreeTransition() const;

        /**
         * @brief Get timestamp from first GUITreeTransition (APE alignment)
         * 
         * @return Timestamp, or 0 if no transitions
         */
        int getTimestamp() const;

        void incrementVisit() { _visitCount++; }
        int getVisitCount() const { return _visitCount; }

        bool isStrong() const { return _visitCount > 0; }

        bool isSameActivity() const;

        uintptr_t hash() const override { return _hashcode; }

    private:
        StatePtr _source;
        StatePtr _target;
        ActionType _actionType;
        uintptr_t _targetWidgetHash;
        ElementPtr _sourceTree;
        ElementPtr _targetTree;
        int _visitCount;
        uintptr_t _hashcode;
        
        /// APE alignment: List of GUITreeTransitions
        GUITreeTransitionPtrVec _treeTransitions;
    };

    typedef std::shared_ptr<StateTransition> StateTransitionPtr;

    /**
     * @brief Counter for tracking action statistics by action type
     * 
     * Maintains counts for each action type and total action count.
     * Used for generating unique action IDs and tracking action distribution.
     */
    struct ActionCounter {
    private:
        /// Array storing count for each action type (indexed by ActionType enum)
        long actCount[ActionType::ActTypeSize];
        
        /// Total count of all actions processed
        long total;

    public:
        /**
         * @brief Constructor initializes all counters to zero
         */
        ActionCounter()
                : actCount{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}, total(0) {
        }

        /**
         * @brief Increment counter for a specific action type
         * 
         * @param action The action to count
         */
        void countAction(const ActivityStateActionPtr &action) {
            actCount[action->getActionType()]++;
            total++;
        }

        /**
         * @brief Get total count of all actions processed
         * 
         * @return Total action count
         */
        long getTotal() const { return total; }
    };

    /**
     * @brief Interface for objects that want to be notified when new states are added to the graph
     * 
     * Implement this interface to receive notifications about state additions.
     * Used by agents to update their internal state when the graph changes.
     */
    class GraphListener {
    public:
        /**
         * @brief Called when a new state node is added to the graph
         * 
         * @param node The state node that was added
         */
        virtual void onAddNode(StatePtr node) = 0;
    };

    /// Smart pointer type for GraphListener
    typedef std::shared_ptr<GraphListener> GraphListenerPtr;
    
    /// Vector of graph listeners
    typedef std::vector<GraphListenerPtr> GraphListenerPtrVec;

    /**
     * @brief Graph class representing the state-action graph for reinforcement learning
     * 
     * The Graph maintains:
     * - All visited states in the application
     * - All actions (visited and unvisited) that can be performed
     * - Activity distribution statistics
     * - Listeners that need to be notified of state changes
     * 
     * States are stored in a set and deduplicated by hash. Actions are indexed
     * by their visited status for efficient lookup during action selection.
     */
    class Graph : Node {
    public:
        /**
         * @brief Constructor initializes the graph with empty state
         */
        Graph();

        /**
         * @brief Get the number of unique states in the graph
         * 
         * @return Number of states
         */
        inline size_t stateSize() const { return this->_states.size(); }

        /**
         * @brief Get the current timestamp of the graph
         * 
         * @return Current timestamp
         */
        time_t getTimestamp() const { return _timeStamp.load(); }

        /**
         * @brief Add a listener to be notified when new states are added
         * 
         * @param listener The listener to register
         */
        void addListener(const GraphListenerPtr &listener);

        /**
         * @brief Add a state to the graph, or return existing state if already present
         * 
         * If the state already exists (by hash), returns the existing state.
         * Otherwise, adds the new state and updates all related statistics.
         * 
         * @param state The state to add
         * @return The state that was added or the existing matching state
         */
        StatePtr addState(StatePtr state);

        TransitionVisitType addTransition(const StatePtr &source,
                                          const ActivityStateActionPtr &action,
                                          const StatePtr &target,
                                          const ElementPtr &sourceTree,
                                          const ElementPtr &targetTree,
                                          StateTransitionPtr &existingTransition,
                                          StateTransitionPtr &newTransition);

        const std::vector<StateTransitionPtr> &getTransitions() const { return _transitions; }

        /**
         * @brief Get outgoing state transitions for a given action
         * 
         * Returns all transitions that start from the action's source state and use this action.
         * Used for priority adjustment based on target state saturation and activity transitions.
         * 
         * @param action The action to get transitions for
         * @return Vector of state transitions
         */
        std::vector<StateTransitionPtr> getOutStateTransitions(const ActivityStateActionPtr &action) const;

        /**
         * @brief Get total distribution count (total number of state accesses)
         * 
         * @return Total distribution count
         */
        long getTotalDistri() const { return this->_totalDistri; }

        const StatePtrSet &getStatesByNaming(const NamingPtr &naming) const;

        const StatePtrSet &getStates() const { return _states; }

        StatePtrSet getStatesByActivity(const std::string &activity) const;

        /**
         * @brief Remove a state from the graph and collect all related transitions
         * 
         * Removes the state and all transitions connected to it (both incoming and outgoing).
         * This is used during model rebuild when states need to be removed due to naming changes.
         * 
         * @param state The state to remove
         * @param removedTransitions Output parameter to collect removed transitions
         */
        void removeState(const StatePtr &state, std::vector<StateTransitionPtr> &removedTransitions);

        /**
         * @brief Get set of all visited activity names
         * 
         * @return Const reference to set of visited activity string pointers
         */
        const stringPtrSet& getVisitedActivities() const { return this->_visitedActivities; };

        /**
         * @brief Find a visited activity string pointer without allocating
         *
         * Avoids creating a temporary shared_ptr<string> (heap allocation) just to probe the set.
         * Uses a linear scan, which is acceptable because visited activities are typically small.
         *
         * @param activity Activity name string
         * @return Cached stringPtr if found, nullptr otherwise
         */
        stringPtr findVisitedActivityPtr(const std::string &activity) const;

        /**
         * @brief Add an entry GUI tree (first state when app starts)
         * 
         * @param tree The entry GUI tree
         */
        void addEntryGUITree(const ElementPtr &tree);

        /**
         * @brief Add a clean entry GUI tree (first state from clean install)
         * 
         * @param tree The clean entry GUI tree
         */
        void addCleanEntryGUITree(const ElementPtr &tree);

        /**
         * @brief Check if a state is an entry state
         * 
         * @param state The state to check
         * @return true if state is an entry state
         */
        bool isEntryState(const StatePtr &state) const;

        /**
         * @brief Check if a state is a clean entry state
         * 
         * @param state The state to check
         * @return true if state is a clean entry state
         */
        bool isCleanEntryState(const StatePtr &state) const;

        /**
         * @brief Get tree transition history
         * 
         * @return Const reference to tree transition history
         */
        const std::vector<StateTransitionPtr> &getTreeTransitionHistory() const { return _treeTransitionHistory; }

        /**
         * @brief Rebuild state transition history from tree transition history
         * 
         * This method rebuilds the state transition history after model rebuild,
         * ensuring that transitions are properly tracked.
         */
        void rebuildHistory();

        /**
         * @brief Destructor clears all internal data structures
         */
        virtual ~Graph();

    protected:
        /**
         * @brief Notify all registered listeners about a new state
         * 
         * @param node The state node that was added
         */
        void notifyNewStateEvents(const StatePtr &node);

    private:
        /**
         * @brief Process and index all actions from a state
         * 
         * Updates action sets (visited/unvisited) and assigns IDs to new actions.
         * 
         * @param node The state node containing actions to process
         */
        void addActionFromState(const StatePtr &node);

        /// Set of all unique states in the graph (deduplicated by hash)
        StatePtrSet _states;
        
        /// Set of all visited activity names (shared pointers to strings for memory efficiency)
        stringPtrSet _visitedActivities;

        /// Map from naming to set of states (for abstraction/refinement)
        std::map<NamingPtr, StatePtrSet, Comparator<Naming>> _statesByNaming;
        
        /// Map from activity name to (visit_count, percentage) pair
        /// Used for tracking activity distribution statistics
        std::map<std::string, std::pair<int, double>> _activityDistri;
        
        /// Total count of state accesses (new states + revisits)
        /// Used for calculating activity visit percentages
        long _totalDistri;
        
        /// Map from widget to set of actions that can be performed on that widget
        /// Used for quick lookup of available actions for a specific widget
        ModelActionPtrWidgetMap _widgetActions;

        /// Set of actions that have not been visited yet
        ActivityStateActionPtrSet _unvisitedActions;
        
        /// Set of actions that have been visited at least once
        ActivityStateActionPtrSet _visitedActions;

        /// Counter for tracking action statistics by type
        ActionCounter _actionCounter;
        
        /// List of listeners to notify when new states are added
        GraphListenerPtrVec _listeners;

        std::vector<StateTransitionPtr> _transitions;
        std::map<std::pair<uintptr_t, uintptr_t>, std::vector<StateTransitionPtr>> _transitionsByAction;
        
        /// Entry GUI trees (first states when app starts)
        std::set<ElementPtr, Comparator<Element>> _entryGUITrees;
        
        /// Clean entry GUI trees (first states from clean install)
        std::set<ElementPtr, Comparator<Element>> _cleanEntryGUITrees;
        
        /// Entry states (states corresponding to entry GUI trees)
        StatePtrSet _entryStates;
        
        /// Clean entry states (states corresponding to clean entry GUI trees)
        StatePtrSet _cleanEntryStates;
        
        /// Tree transition history (all GUI tree transitions)
        std::vector<StateTransitionPtr> _treeTransitionHistory;
        
        /// State transition history (rebuilt from tree transition history)
        std::vector<StateTransitionPtr> _stateTransitionHistory;
        
        /// Current timestamp of the graph (updated when states are added)
        std::atomic<time_t> _timeStamp;

        /// Default distribution pair (0, 0.0) used for initializing new activities
        const static std::pair<int, double> _defaultDistri;
    };

    typedef std::shared_ptr<Graph> GraphPtr;

}

#endif  // Graph_H_
