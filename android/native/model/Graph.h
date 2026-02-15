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
#include <map>
#include <unordered_map>

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
         * @brief Get the set of all states in the graph (read-only).
         * Used e.g. by FrontierAgent for global frontier candidate collection.
         * @return Const reference to the state set
         */
        const StatePtrSet &getStates() const { return this->_states; }

        /**
         * @brief Get the current timestamp of the graph
         * 
         * @return Current timestamp
         */
        time_t getTimestamp() const { return this->_timeStamp; }

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

        /**
         * @brief Get total distribution count (total number of state accesses)
         * 
         * @return Total distribution count
         */
        long getTotalDistri() const { return this->_totalDistri; }

        /**
         * @brief Get set of all visited activity names
         * 
         * @return Const reference to set of visited activity string pointers
         */
        const stringPtrSet& getVisitedActivities() const { return this->_visitedActivities; };

        /**
         * @brief Get number of states that belong to the given activity.
         * Used for dynamic state abstraction (coarsening threshold).
         */
        size_t getStateCountByActivity(const std::string &activity) const;

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
        
        /// Current timestamp of the graph (updated when states are added)
        time_t _timeStamp;

        /// Per-activity count of unique states (updated only when a new state is added)
        /// Used for dynamic state abstraction (coarsening threshold) - O(1) lookup
        std::unordered_map<std::string, size_t> _activityStateCount;

        /// Default distribution pair (0, 0.0) used for initializing new activities
        const static std::pair<int, double> _defaultDistri;
    };

    typedef std::shared_ptr<Graph> GraphPtr;

}

#endif  // Graph_H_
