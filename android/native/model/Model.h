/*
 * This code is licensed under the Fastbot license. You may obtain a copy of this license in the LICENSE.txt file in the root directory of this source tree.
 */
/**
 * @authors Jianqiang Guo, Yuhui Su, Zhao Zhang
 */
#ifndef  Model_H_
#define  Model_H_

#include <memory>
#include "Base.h"
#include "State.h"
#include "Element.h"
#include "Action.h"
#include "Graph.h"
#include "AbstractAgent.h"
#include "AgentFactory.h"
#include "Preference.h"
#include "agent/LLMTaskAgent.h"
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <set>

namespace fastbotx {

#if DYNAMIC_STATE_ABSTRACTION_ENABLED
    /// Single transition log entry for non-determinism detection
    struct TransitionEntry {
        uintptr_t sourceStateHash{0};
        uintptr_t actionHash{0};
        uintptr_t targetStateHash{0};
        std::string sourceActivity;
        bool valid{false};
    };

    /// Per-activity context for refinement/coarsening (previous mask and L′→L split tracking)
    struct ActivityAbstractionContext {
        WidgetKeyMask previousMask{DefaultWidgetKeyMask};
        size_t stateCountAtLastRefinement{0};
        /// APE coarsening: map old state hash (under L′) → set of new state hashes (under L)
        std::unordered_map<uintptr_t, std::unordered_set<uintptr_t>> oldStateToNewStates;
    };

    /// Per-activity last-seen state stats for "skip Text refinement" when text-heavy or would explode
    struct ActivityLastStateTextStats {
        size_t widgetsWithNonEmptyText{0};
        size_t totalWidgets{0};
        size_t uniqueWidgetsIfAddText{0};
    };
#endif

    /**
     * @brief Constants namespace for model-related constants
     */
    namespace ModelConstants {
        /// Default device ID used when no device ID is specified
        constexpr const char* DefaultDeviceID = "0000001";
    }

    /**
     * @brief Model class representing the core RL (Reinforcement Learning) model
     * 
     * The Model class is the central component that:
     * - Manages the state-action graph
     * - Coordinates agents for different devices
     * - Handles action selection and state management
     * - Provides the main interface for getting next operations
     * 
     * It uses shared_from_this to allow agents to hold references to the model.
     */
    class Model : public std::enable_shared_from_this<Model> {
    public:
        /**
         * @brief Factory method to create a new Model instance
         * 
         * @return Shared pointer to a new Model object
         */
        static std::shared_ptr<Model> create();

        /**
         * @brief Get the number of states in the graph
         * 
         * @return Number of unique states in the graph
         */
        inline size_t stateSize() const { return this->getGraph()->stateSize(); }

        /**
         * @brief Get the graph object
         * 
         * @return Const reference to the graph object
         */
        const GraphPtr &getGraph() const { return this->_graph; }

        /**
         * @brief Create and add an agent to the model for a specific device
         * 
         * Creates a new agent, adds it to the device-agent map, and registers it
         * as a listener to the graph for state change notifications.
         * 
         * @param deviceIDString Device ID string (empty string uses default device ID)
         * @param agentType The type of algorithm/agent to create
         * @param deviceType The type of device (default: Normal)
         * @return Shared pointer to the newly created agent
         */
        AbstractAgentPtr addAgent(const std::string &deviceIDString, AlgorithmType agentType,
                                  DeviceType deviceType = DeviceType::Normal);

        /**
         * @brief Get the agent for a specific device
         * 
         * @param deviceID Device ID string (empty string uses default device ID)
         * @return Shared pointer to the agent, or nullptr if not found
         */
        AbstractAgentPtr getAgent(const std::string &deviceID) const;

        /**
         * @brief Get next operation step from XML string, returning JSON format
         * 
         * This is the main entry point that accepts XML content as a string.
         * Parses the XML and delegates to the ElementPtr-based version.
         * 
         * @param descContent XML content of the current page as a string
         * @param activity Activity name string
         * @param deviceID Device ID string (default: empty string uses default device)
         * @return Next operation step in JSON format
         */
        std::string getOperate(const std::string &descContent, const std::string &activity,
                               const std::string &deviceID = "");

        /**
         * @brief Get next operation step from Element object, returning JSON format
         * 
         * This method wraps getOperateOpt() and converts the result to JSON string.
         */
        std::string getOperate(const ElementPtr &element, const std::string &activity,
                               const std::string &deviceID = "");

        /**
         * @brief Core method for getting next operation and updating RL model
         * 
         * Image for LLM is obtained in Java on demand when native triggers HTTP (no screenshot param).
         */
        OperatePtr getOperateOpt(const ElementPtr &element, const std::string &activity,
                                 const std::string &deviceID = "");

        /**
         * @brief Get the preference object
         * 
         * @return Shared pointer to the preference object
         */
        PreferencePtr getPreference() const { return this->_preference; }

        /**
         * @brief Get the shared LLM client (if any) used by LLMTaskAgent.
         * Other agents (e.g. LLMExplorerAgent) may use it for content-aware input or knowledge org.
         */
        std::shared_ptr<LlmClient> getLlmClient() const;

        /**
         * @brief Get widget key mask for an activity (dynamic state abstraction).
         * Returns DefaultWidgetKeyMask if activity not found.
         */
        WidgetKeyMask getActivityKeyMask(const std::string &activity) const;

        /**
         * @brief Set widget key mask for an activity (dynamic state abstraction).
         */
        void setActivityKeyMask(const std::string &activity, WidgetKeyMask mask);

        /**
         * @brief Set the package name for network action parameters
         * 
         * @param packageName The package name string
         */
        void setPackageName(const std::string &packageName) { 
            this->_netActionParam.packageName = packageName; 
        }

        /**
         * @brief Get the package name
         * 
         * @return Const reference to the package name string
         */
        const std::string &getPackageName() const { return this->_netActionParam.packageName; }

        /**
         * @brief Get the network action task ID
         * 
         * @return Network action task ID
         */
        int getNetActionTaskID() const { return this->_netActionParam.netActionTaskid; }

        /**
         * @brief Report current activity for coverage tracking (performance: coverage in C++, PERF §3.4)
         */
        void reportActivity(const std::string &activity);

        /**
         * @brief Get coverage summary as JSON: {"stepsCount":N,"testedActivities":["a1",...]}
         */
        std::string getCoverageJson() const;

        /**
         * @brief Load persisted dynamic state abstraction policy for the current package (if enabled).
         *
         * Policy file path (per package):
         *   /sdcard/fastbot_{packageName}.statekey.json
         *
         * This method is a thin I/O wrapper and does not change any refinement/coarsening logic.
         */
        void loadStateAbstractionPolicy();

        /**
         * @brief Save current dynamic state abstraction policy for the current package (if enabled).
         *
         * Writes the same format as loadStateAbstractionPolicy() reads.
         * Safe to call multiple times; errors are logged and otherwise ignored.
         */
        void saveStateAbstractionPolicy() const;

        virtual ~Model();

    protected:
        Model();

    private:
        /**
         * @brief Get custom action from preference if one exists for this page
         * 
         * @param activity Activity name string
         * @param element XML Element object of the current page
         * @return Custom action if exists, nullptr otherwise
         */
        /**
         * @brief Get or create an activity string pointer (memory optimization)
         * 
         * Reuses existing activity string pointers from the graph to avoid duplication.
         * 
         * @param activity The activity name string
         * @return Shared pointer to the activity string (cached or newly created)
         */
        stringPtr getOrCreateActivityPtr(const std::string &activity);
        
        /**
         * @brief Get or create an agent for the given device ID
         * 
         * Returns existing agent or default agent if device ID not found.
         * Creates default agent if no agents exist.
         * 
         * @param deviceID Device ID string (empty string uses default device ID)
         * @return Shared pointer to the agent
         */
        AbstractAgentPtr getOrCreateAgent(const std::string &deviceID);
        
        /**
         * @brief Build a state from element without adding to the graph (for moveForward-before-addState flow).
         * 
         * @param element XML Element object of the current page
         * @param agent The agent to use for state creation
         * @param activityPtr Shared pointer to activity name string
         * @return Shared pointer to the created state (not yet in graph)
         */
        StatePtr buildStateOnly(const ElementPtr &element, const AbstractAgentPtr &agent,
                               const stringPtr &activityPtr);

        /**
         * @brief Create a new state from element and add it to the graph
         * 
         * @param element XML Element object of the current page
         * @param agent The agent to use for state creation
         * @param activityPtr Shared pointer to activity name string
         * @return Shared pointer to the created/existing state
         */
        StatePtr createAndAddState(const ElementPtr &element, const AbstractAgentPtr &agent, 
                                   const stringPtr &activityPtr);
        
        /**
         * @brief Select an action based on state, agent, and custom preferences
         * 
         * @param state The current state (may be modified)
         * @param agent The agent to use for action selection (may be modified)
         * @param customAction Custom action from preference, if any
         * @param actionCost Output parameter: time cost for action generation in seconds
         * @return Selected action, or nullptr if selection failed
         */
        ActionPtr selectAction(StatePtr &state, AbstractAgentPtr &agent, ActionPtr customAction, double &actionCost);
        
        /**
         * @brief Convert an action to an operate object and apply patches
         * 
         * @param action The action to convert
         * @param state The current state (used for detail clearing optimization)
         * @return OperatePtr The operation object ready for execution
         */
        OperatePtr convertActionToOperate(ActionPtr action, StatePtr state);

#if DYNAMIC_STATE_ABSTRACTION_ENABLED
        /// Record one transition (source, action, target) for non-determinism detection
        void recordTransition(const AbstractAgentPtr &agent, const StatePtr &targetState);
        /// Record state under previous mask for APE coarsening (one L′ state → > β new states)
        void recordStateSplitIfRefined(const std::string &activity, const StatePtr &state);
        /// Run refinement/coarsening batch if step count reached interval
        void runRefinementAndCoarseningIfScheduled();
        /// Detect (source, action) pairs that lead to multiple different targets; return activity names to refine
        std::vector<std::string> detectNonDeterminism() const;
        /// Refine activity mask (add Text/ContentDesc/Index); return true if refined
        bool refineActivity(const std::string &activity);
        /// Coarsen activity mask if state count exceeded threshold (after refinement)
        void coarsenActivityIfNeeded(const std::string &activity);
#endif
        
        /// Smart pointer to the graph object managing all states and actions
        GraphPtr _graph;
        
        /// Map from device ID to agent object
        /// Allows multiple devices to have different agents with different strategies
        AbstractAgentPtrStrMap _deviceIDAgentMap;
        
        /// User-specified preferences for customizing behavior
        PreferencePtr _preference;

        /// Parameters for communicating with network-based action models
        NetActionParam _netActionParam;

        /// Optional LLM-based GUI agent (LLMTaskAgent). When configured with a concrete
        /// LlmClient implementation, this agent can temporarily take over action
        /// selection for predefined tasks (e.g. login flows).
        std::shared_ptr<LLMTaskAgent> _llmTaskAgent;

        /// Coverage tracking: visited activities and step count (performance optimization)
        std::unordered_set<std::string> _visitedActivities;
        int _coverageStepCount{0};
        mutable std::mutex _coverageMutex;

        /// Per-activity widget key mask for dynamic state abstraction
        mutable std::unordered_map<std::string, WidgetKeyMask> _activityKeyMask;

#if DYNAMIC_STATE_ABSTRACTION_ENABLED
        std::vector<TransitionEntry> _transitionLog;
        size_t _transitionLogWriteIndex{0};
        size_t _stepCountSinceLastCheck{0};
        std::unordered_map<std::string, ActivityAbstractionContext> _activityAbstractionContext;
        std::set<std::pair<std::string, WidgetKeyMask>> _coarseningBlacklist;
        /// Activities that need refinement due to α (max widgets per model action > α)
        std::set<std::string> _activitiesNeedingAlphaRefinement;
        /// Last-seen state stats per activity for "skip Text" when text-heavy or unique-after-Text would explode
        std::unordered_map<std::string, ActivityLastStateTextStats> _activityLastStateTextStats;
#endif

    };

    typedef std::shared_ptr<Model> ModelPtr;
}

#endif  // Model_H_
