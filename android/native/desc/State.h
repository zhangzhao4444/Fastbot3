/*
 * Copyright (c) 2020 Bytedance Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
/**
 * @authors Jianqiang Guo, Yuhui Su
 */
#ifndef State_H_
#define State_H_

#include "Node.h"
#include "../Base.h"
#include "Action.h"
#include "Widget.h"
#include "Element.h"
#include "ActionFilter.h"
#include "Element.h"
#include "naming/StateKey.h"
#include <vector>


namespace fastbotx {


    /**
     * @brief State class representing a UI state/screen in the application
     * 
     * State represents a complete UI screen state with all widgets and available actions.
     * It inherits from Node (visit tracking), PriorityNode (priority), and HashNode (hashing).
     * 
     * Features:
     * - Widget collection representing UI elements
     * - Action collection representing available operations
     * - Hash-based state comparison
     * - Action selection algorithms (greedy, random, etc.)
     * - Memory optimization (can clear details when not needed)
     * 
     * States are used in the reinforcement learning model to track different
     * screens and their available actions.
     */
    class State : public Node, public PriorityNode, public HashNode {
    public:
        /**
         * @brief Get the back action for this state
         * 
         * @return Shared pointer to the back action
         */
        ActivityStateActionPtr getBackAction() const { return this->_backAction; }

        NamingPtr getCurrentNaming() const { return this->_currentNaming; }

        StateKeyPtr getStateKey() const { return this->_stateKey; }

        /**
         * @brief Get the activity name string pointer
         * 
         * @return Shared pointer to activity name string
         */
        stringPtr getActivityString() const { return this->_activity; }

        /**
         * @brief Convert state to string representation (implements Serializable)
         * 
         * @return String representation of the state
         */
        std::string toString() const override;

        /**
         * @brief Get hash code of this state
         * 
         * @return Hash code as uintptr_t
         */
        uintptr_t hash() const override;

        /**
         * @brief Destructor cleans up all resources
         */
        virtual ~State();

        /**
         * @brief Count actions matching filter with priority consideration
         * 
         * @param filter Action filter to apply
         * @param includeBack Whether to include back action
         * @return Count of matching actions
         */
        int countActionPriority(const ActionFilterPtr &filter, bool includeBack) const;

        /**
         * @brief Get all actions available in this state
         * 
         * @return Const reference to action vector
         */
        const ActivityStateActionPtrVec &getActions() const { return this->_actions; }

        /**
         * @brief Get all widgets in this state
         * 
         * @return Const reference to widget vector
         */
        const WidgetPtrVec &getWidgets() const { return this->_widgets; }

        /**
         * @brief Get actions that have target widgets
         * 
         * @return Vector of actions with targets
         */
        ActivityStateActionPtrVec targetActions() const;

        /**
         * @brief Greedily pick action with maximum Q-value
         * 
         * @param filter Action filter to apply
         * @return Action with highest Q-value, or nullptr if none found
         */
        ActivityStateActionPtr greedyPickMaxQValue(const ActionFilterPtr &filter) const;

        /**
         * @brief Randomly pick an unvisited action
         * 
         * @return Random unvisited action, or nullptr if all visited
         */
        ActivityStateActionPtr randomPickUnvisitedAction() const;

        /**
         * @brief Randomly pick an action matching the filter
         * 
         * @param filter Action filter to apply
         * @return Random matching action, or nullptr if none found
         */
        ActivityStateActionPtr randomPickAction(const ActionFilterPtr &filter) const;

        /**
         * @brief Resolve action at a specific timestamp
         * 
         * @param action The action to resolve
         * @param t Timestamp
         * @return Resolved action
         */
        ActivityStateActionPtr resolveAt(ActivityStateActionPtr action, time_t t);

        /**
         * @brief Check if state contains a specific widget
         * 
         * @param widget Widget to search for
         * @return true if widget is found in state
         */
        bool containsTarget(const WidgetPtr &widget) const;

        /**
         * @brief Check if an action is saturated (visited too many times)
         * 
         * For actions with targets, checks if visited count exceeds merged widget count.
         * For actions without targets, checks if visited at least once.
         * 
         * @param action Action to check
         * @return true if action is saturated
         */
        bool isSaturated(const ActivityStateActionPtr &action) const;

        /**
         * @brief Check if state is saturated (all actions are saturated)
         * 
         * A state is saturated if all its actions are saturated.
         * 
         * @return true if state is saturated
         */
        bool isSaturated() const;

        bool isTrivialState() const { return _stateKey ? _stateKey->isTrivialState() : false; }

        int getMergedWidgetCount(uintptr_t widgetHash) const;

        NamePtr getNameForWidgetHash(uintptr_t widgetHash) const;

        ActivityStateActionPtr getActionByTargetHash(ActionType type, uintptr_t widgetHash) const;

        ActivityStateActionPtr getActionByType(ActionType type) const;

        ActivityStateActionPtr relocateAction(ActionType type, const NamePtr &targetName) const;

        /**
         * @brief Set priority of this state
         * 
         * @param p Priority value
         */
        void setPriority(int p) { this->_priority = p; }

        /**
         * @brief Less-than comparison operator (for sorting)
         * 
         * @param state State to compare with
         * @return true if this state's hash is less than other's
         */
        bool operator<(const State &state) const;

        /**
         * @brief Equality comparison operator
         * 
         * @param state State to compare with
         * @return true if states are equal (same hash)
         */
        bool operator==(const State &state) const;

        /**
         * @brief Clear detailed information to save memory
         * 
         * Removes detailed widget information while keeping essential data
         * for state comparison. Used for memory optimization.
         */
        virtual void clearDetails();

        /**
         * @brief Fill details from another state
         * 
         * Copies detailed information from another state to this one.
         * Used when a state without details is matched to one with details.
         * 
         * @param copy State to copy details from
         */
        virtual void fillDetails(const std::shared_ptr<State> &copy);

        void buildStateKey(const NamingPtr &naming);

        void appendTree(const ElementPtr &tree);

        const std::vector<ElementPtr> &getTreeHistory() const { return _treeHistory; }

        ElementPtr getLatestTree() const;

        void resolveActionTargets(const ElementPtr &tree);

        /**
         * @brief Check if state has no detailed information
         * 
         * @return true if details have been cleared
         */
        bool hasNoDetail() const { return this->_hasNoDetail; }

        FuncGetID(State);

    protected:
        State();

        explicit State(stringPtr activityName);

        ///
        /// \param mergeWidgets
        /// \return
        int mergeWidgetAndStoreMergedOnes(WidgetPtrSet &mergeWidgets);

        ///
        /// \param parentWidget
        /// \param elem
        virtual void buildFromElement(WidgetPtr parentWidget, ElementPtr elem);

        ///
        /// \param filter
        /// \param includeBack
        /// \return
        ActivityStateActionPtr
        randomPickAction(const ActionFilterPtr &filter, bool includeBack) const;

        ///
        /// \param filter
        /// \param includeBack
        /// \param index
        /// \return
        ActivityStateActionPtr
        pickAction(const ActionFilterPtr &filter, bool includeBack, int index) const;


        /// Hash code for this state (computed from activity and widgets)
        uintptr_t _hashcode{};
        
        /// Activity name string pointer (shared for memory efficiency)
        stringPtr _activity;
        
        /// Bounds of the root element
        RectPtr _rootBounds;
        
        /// Vector of all available actions in this state
        ActivityStateActionPtrVec _actions;
        
        /// Vector of all widgets in this state
        WidgetPtrVec _widgets;
        
        /// Map from widget hash to vector of merged widgets (for widget deduplication)
        WidgetPtrVecMap _mergedWidgets;

        NamingPtr _currentNaming;
        StateKeyPtr _stateKey;
        NamePtrVec _nameWidgets;
        std::map<uintptr_t, NamePtr> _widgetNames;

        std::vector<ElementPtr> _treeHistory;

        /// Flag indicating if detailed information has been cleared
        bool _hasNoDetail;
        
        /// Static shared root bounds (used for comparison across states)
        static RectPtr _sameRootBounds;
        
        /// Back action for navigating away from this state
        ActivityStateActionPtr _backAction;
    private:
        static std::shared_ptr<State> create(ElementPtr elem, stringPtr activityName);

        PropertyIDPrefix(State);

    };

    typedef std::shared_ptr<State> StatePtr;
    typedef std::vector<StatePtr> StatePtrVec;
    typedef std::set<StatePtr, Comparator<State>> StatePtrSet;

}

#endif //State_H_
