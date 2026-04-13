/*
 * This code is licensed under the Fastbot license. You may obtain a copy of this license in the LICENSE.txt file in the root directory of this source tree.
 */
/**
 * @authors Jianqiang Guo, Yuhui Su, Zhao Zhang, Tianming Liu, Chenxu Wang
 */

#ifndef AbstractAgent_H_
#define AbstractAgent_H_


#include "utils.hpp"
#include "Base.h"
#include "Action.h"
#include "State.h"
#include "ActionFilter.h"
#include "Graph.h"

#include <array>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace fastbotx {

    /**
     * @brief Action priority constants namespace
     * 
     * Defines constants used for calculating action priorities.
     * These values are used to adjust priorities during action selection.
     */
    namespace ActionPriorityConstants {
        /// Bonus value for unvisited actions, encourages exploration of unvisited actions
        constexpr int UnvisitedActionBonus = 20;
        /// Multiplier for new actions, significantly boosts priority of new actions
        constexpr int NewActionMultiplier = 5;
        /// Bonus value for no-target unvisited actions, encourages exploration of no-target actions
        constexpr int NoTargetUnvisitedBonus = 5;
    }

    class Model; // forward declaration
    typedef std::shared_ptr<Model> ModelPtr;
    class ReuseState; // LLMDroid B2: processState
    struct LlmdroidAgentOverlay;

    /**
     * @brief Abstract base class for all test agents
     * 
     * AbstractAgent is the base class for all test agents, implementing basic action
     * selection and state management functionality. It implements the GraphListener
     * interface to monitor changes in the state graph.
     * 
     * Main features:
     * 1. Manages current state, previous state, and new state
     * 2. Manages action history (last action, current action, new action)
     * 3. Adjusts action priorities
     * 4. Resolves and selects new actions
     * 5. Handles state blocking detection
     * 
     * Subclasses must implement:
     * - selectNewAction(): Concrete action selection strategy
     * - updateStrategy(): Strategy update (e.g., Q-value updates in reinforcement learning)
     */
    class AbstractAgent : public GraphListener {
    public:
        static constexpr int MAX_TEMPLATE_SEQUENCE_LEN = 5;

        struct GuidancePathTemplate {
            std::array<uint64_t, MAX_TEMPLATE_SEQUENCE_LEN> sequence{};
            std::array<double, MAX_TEMPLATE_SEQUENCE_LEN> reliability{};
            int length{0};
        };

        struct PreconditionInfo {
            double score{1.0};
            std::unordered_set<uint64_t> actionList;
            std::array<GuidancePathTemplate, MAX_TEMPLATE_SEQUENCE_LEN> templates{};
            int templateCount{0};
        };

        /**
         * @brief Get current state block count
         * 
         * When the same state is reached consecutively, the counter increments.
         * Used to detect if stuck in a loop state.
         * 
         * @return Number of times the current state has been blocked
         */
        virtual int getCurrentStateBlockTimes() const { return this->_currentStateBlockTimes; }

        /**
         * @brief Resolve and select a new action
         * 
         * Main entry point for action selection. Executes the following steps:
         * 1. Adjust priorities of all candidate actions
         * 2. Call subclass's selectNewAction() to select specific action
         * 3. Save selected action to _newAction
         * 
         * @return Pointer to selected action, or nullptr if selection fails
         */
        virtual ActionPtr resolveNewAction();

        /**
         * @brief Update strategy (pure virtual, implemented by subclasses)
         * 
         * Called after action execution to update internal strategy.
         * For example, update Q-values in reinforcement learning, update reuse model, etc.
         */
        virtual void updateStrategy() = 0;

        /**
         * @brief Move forward in state machine
         * 
         * Updates state and action history:
         * - _lastState = _currentState
         * - _currentState = _newState
         * - _newState = nextState
         * - Similarly updates corresponding action records
         * 
         * @param nextState Next state
         */
        virtual void moveForward(StatePtr nextState);

        /**
         * @brief Callback when a new node is added to the state graph
         * 
         * Implements GraphListener interface. Called when Graph adds a new state.
         * Used to update _newState and detect state blocking.
         * 
         * @param node Newly added state node
         */
        void onAddNode(StatePtr node) override;

        /**
         * @brief Constructor
         * 
         * @param model Model pointer, Agent needs access to model to get state graph info
         */
        explicit AbstractAgent(const ModelPtr &model);

        /**
         * @brief Virtual destructor
         * 
         * Cleans up all smart pointer resources
         */
        virtual ~AbstractAgent();

        /**
         * @brief Get algorithm type
         * 
         * @return Algorithm type used by this Agent
         */
        virtual AlgorithmType getAlgorithmType() { return this->_algorithmType; }

        /**
         * @brief Get current state (state before last moveForward).
         * Used by Model for transition logging (dynamic state abstraction).
         */
        StatePtr getCurrentState() const { return this->_currentState; }

        /**
         * @brief Get current action (action used to reach current state).
         * Used by Model for transition logging (dynamic state abstraction).
         */
        ActivityStateActionPtr getCurrentAction() const { return this->_currentAction; }

        /**
         * @brief Callback when state abstraction has changed (refine/coarsen batch finished).
         * Subclasses may override to clear hash-dependent caches when abstraction changes.
         * Default: no-op.
         */
        virtual void onStateAbstractionChanged() {}

        /**
         * @brief Optional: return LLM-generated input text for an action (e.g. content-aware input).
         * Used when the chosen action targets an editable widget; if non-empty, Model sets it on the Operate.
         * Default: returns empty string. LLMExplorerAgent overrides to call LLM (paper: Content-aware Input Text Generator).
         */
        virtual std::string getInputTextForAction(const StatePtr &state, const ActionPtr &action) const {
            (void) state;
            (void) action;
            return "";
        }

        /**
         * LLMDroid overlay: invoked from Model::getOperateOpt only when max.llm.llmdroid=true, after
         * addState/visit and recordTransition — canonical graph `state` identity matches the closed APE block.
         * Updates MergedStateGraph, queues STATE_OVERVIEW / REANALYSIS to GPTAgent worker (MIGRATION_LLMDROID_B2 §4.7 A).
         * HTTP calls need max.llm.enabled + URL/key so Model::getLlmClient() is non-null.
         * Hard rule (§3.3): do not call applyDynamicAbstractionIdentityHash, touch naming::StateKey for RL id,
         * or use getMergedState() inside resolveNewAction/selectAction for DoubleSarsa / curiosity features.
         */
        virtual void processState(const std::shared_ptr<ReuseState> &state);

        /** Event-driven hook: report current page as precondition page candidate. */
        virtual void addCurrentPageAsPrecondition(const StatePtr &state);

        /** Start a new episode and clear guide runtime counters. */
        virtual void beginNewEpisode();

    private:
        /** Lazy LLMDroid MergedStateGraph + GPT worker (max.llm.llmdroid only). */
        void ensureLlmdroidRuntime();

        std::unique_ptr<LlmdroidAgentOverlay> _llmdroid;

    protected:
        /** Delayed settlement of previous guide action; must run at selectNewAction() entry. */
        void checkPendingGuideResult();

        /** Try selecting one guide action from precondition templates, returns nullptr when no candidate. */
        ActionPtr trySelectGuideAction();

        /** Build/load/save .precond data (separate from .fbm). */
        bool loadPreconditionPagesFromFile(const std::string &filepath);
        bool savePreconditionPagesToFile(const std::string &filepath) const;

    protected:

        /**
         * @brief Handle null action situation
         * 
         * When no valid action can be selected, attempts to randomly select a valid
         * action from current state. If still fails, returns nullptr and logs error.
         * 
         * @return Pointer to handled action, or nullptr on failure
         */
        virtual ActivityStateActionPtr handleNullAction() const;

        /**
         * @brief Select new action (pure virtual, implemented by subclasses)
         * 
         * Subclasses must implement concrete action selection strategies, such as:
         * - Random selection
         * - Greedy selection based on Q-values
         * - Selection based on reuse model
         * 
         * @return Pointer to selected action
         */
        virtual ActionPtr selectNewAction() = 0;

        /**
         * @brief Adjust action priorities
         * 
         * Adjusts priority of each action based on visit status, type, etc.
         * Priority calculation rules:
         * 1. Base priority = action->getPriorityByActionType()
         * 2. If action has no target and is unvisited, add NoTargetUnvisitedBonus
         * 3. If action has target and is unvisited, add UnvisitedActionBonus
         * 4. If action is new (state not saturated), add NewActionMultiplier * base priority
         * 5. Final priority cannot be less than 0
         * 
         * Also calculates total state priority (sum of all action priorities)
         */
        virtual void adjustActions();

        /**
         * @brief Default constructor
         * 
         * Initializes all member variables to default values
         */
        AbstractAgent();

        /// Model weak pointer to avoid circular references
        std::weak_ptr<Model> _model;
        
        /// Previous state (state before last)
        StatePtr _lastState;
        
        /// Current state (actually the previous state, but will be updated by _newState)
        StatePtr _currentState;
        
        /// New state (just reached state)
        StatePtr _newState;
        
        /// Last executed action (action used to reach _lastState)
        ActivityStateActionPtr _lastAction;
        
        /// Current executed action (action used to reach _currentState)
        ActivityStateActionPtr _currentAction;
        
        /// Newly selected action (action used to reach _newState)
        ActivityStateActionPtr _newAction;

//    ActionRecordPtrVec  _actionHistory;

        /// Action validation filter for filtering invalid actions
        ActionFilterPtr _validateFilter;

        /// Graph stability counter (unused)
        long _graphStableCounter;
        
        /// State stability counter (unused)
        long _stateStableCounter;
        
        /// Activity stability counter (unused)
        long _activityStableCounter;

        /// Whether to disable fuzzing
        bool _disableFuzz;
        
        /// Whether to request restart
        bool _requestRestart;
        
        /// Whether app activity just started from clean state
        bool _appActivityJustStartedFromClean{};
        
        /// Whether app activity just started
        bool _appActivityJustStarted{};
        
        /// Whether current state has been recovered
        bool _currentStateRecovered{};
        
        /// Current state block count (consecutive times reaching same state)
        int _currentStateBlockTimes;

        /// Algorithm type
        AlgorithmType _algorithmType;

        // [GUIDE] Runtime state machine (non-persistent): delayed one-step settlement.
        bool _hasPendingGuideCheck{false};
        uint64_t _pendingGuideActionHash{0};
        uint64_t _pendingGuideTargetPage{0};
        bool _preconditionReachedSinceLastGuide{false};
        bool _pendingGuideRewardApplied{false};

        // [GUIDE] Keep matching context for reliability update and logs.
        int _pendingGuideTemplateIndex{-1};
        int _pendingGuidePosition{-1};

        // [GUIDE] Persistent precondition pages and templates.
        std::unordered_map<uint64_t, PreconditionInfo> _preconditionPages;

        // [GUIDE] Runtime episode-level stop-loss / counters.
        std::unordered_set<uint64_t> _coveredThisEpisode;
        std::unordered_map<uint64_t, std::unordered_map<uint64_t, int>> _consecutiveFails;
        std::unordered_map<uint64_t, int> _noProgressCount;
        std::unordered_map<uint64_t, int> _lastPosition;
        std::unordered_map<uint64_t, std::array<int, MAX_TEMPLATE_SEQUENCE_LEN>> _templateRewardCounts;
        std::unordered_map<uint64_t, int> _pageSelectionCounts;
        std::unordered_map<uint64_t, std::array<int, MAX_TEMPLATE_SEQUENCE_LEN>> _templateSelectionCounts;

        // [GUIDE] Recent executed action hashes (old -> new), for template generation.
        std::vector<uint64_t> _guideRecentActionHashes;

        int _guideSystemFailureCount{0};

    };


    typedef std::shared_ptr<AbstractAgent> AbstractAgentPtr;
    typedef std::vector<AbstractAgentPtr> AbstractAgentPtrVec;
    typedef std::map<std::string, AbstractAgentPtr> AbstractAgentPtrStrMap;
}


#endif //AbstractAgent_H_
