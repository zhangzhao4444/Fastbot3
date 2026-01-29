/*
 * This code is licensed under the Fastbot license. You may obtain a copy of this license in the LICENSE.txt file in the root directory of this source tree.
 */
/**
 * @authors Jianqiang Guo, Yuhui Su, Zhao Zhang
 */
#ifndef  Model_H_
#define  Model_H_

#include <memory>
#include <mutex>
#include "Base.h"
#include "State.h"
#include "Element.h"
#include "Action.h"
#include "Graph.h"
#include "AbstractAgent.h"
#include "AgentFactory.h"
#include "Preference.h"
#include "naming/NamingManager.h"

namespace fastbotx {

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
         * 
         * @param element XML Element object of the current page
         * @param activity Activity name string
         * @param deviceID Device ID string (default: empty string uses default device)
         * @return Next operation step in JSON format
         */
        std::string getOperate(const ElementPtr &element, const std::string &activity,
                               const std::string &deviceID = "");

        /**
         * @brief Core method for getting next operation and updating RL model
         * 
         * This is the main orchestration method that creates states, selects actions,
         * and updates the reinforcement learning model.
         * 
         * @param element XML Element object of the current page
         * @param activity Activity name string
         * @param deviceID Device ID string (default: empty string uses default device)
         * @return DeviceOperateWrapper object containing the next operation to perform
         */
        OperatePtr getOperateOpt(const ElementPtr &element, const std::string &activity,
                                 const std::string &deviceID = "");

        /**
         * @brief Get the preference object
         * 
         * @return Shared pointer to the preference object
         */
        PreferencePtr getPreference() const { return this->_preference; }

        NamingManagerPtr getNamingManager() const { return this->_namingManager; }

        /**
         * @brief Rebuild the model by removing states with mismatched naming and re-adding transitions
         * 
         * This method aligns with APE's rebuild functionality:
         * 1. Identifies states whose naming no longer matches the current naming manager
         * 2. Removes these states and collects their transitions
         * 3. Sorts transitions by timestamp (if available)
         * 4. Rebuilds affected states
         * 5. Re-adds transitions in timestamp order
         * 
         * @return Reference to this model for method chaining
         */
        Model& rebuild();

        /**
         * @brief Resolve non-deterministic transitions (APE alignment)
         * 
         * Checks if a state transition is non-deterministic (NEW_ACTION_TARGET type)
         * and triggers refinement if needed.
         * 
         * @param edge The state transition to check
         * @return true if refinement occurred (naming changed)
         */
        bool resolveNonDeterministicTransitions(const StateTransitionPtr &edge);

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

        virtual ~Model();

    protected:
        Model();

    private:
        // Thread-safety: Model and its Graph are mutated across getOperate/addAgent/rebuild paths.
        // Use recursive mutex because public entry points call each other (getOperate(string)->getOperate(Element)->getOperateOpt).
        mutable std::recursive_mutex _mutex;

        /**
         * @brief Get custom action from preference if one exists for this page
         * 
         * @param activity Activity name string
         * @param element XML Element object of the current page
         * @return Custom action if exists, nullptr otherwise
         */
        ActionPtr getCustomActionIfExists(const std::string &activity, const ElementPtr &element) const;
        
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

        void rebuildModel();

        void addStateTransitionIfPossible(const AbstractAgentPtr &agent,
                                          const StatePtr &newState,
                                          const ElementPtr &newTree);

        /**
         * @brief Pre-evolve model: check under/over abstracted states before adding to graph
         * 
         * Implements APE's preEvolveModel flow:
         * 1. checkUnderAbstractedState() (aggregation)
         * 2. checkOverAbstractedState() (refinement)
         * 3. checkUnderAbstractedState() (aggregation again)
         * 
         * @param state The state to evolve
         * @param agent The agent
         */
        void preEvolveModel(StatePtr &state, const AbstractAgentPtr &agent);

        /**
         * @brief Check and refine over-abstracted state
         * 
         * @param state The state to check
         * @param agent The agent
         */
        void checkOverAbstractedState(StatePtr &state, const AbstractAgentPtr &agent);

        /**
         * @brief Check and aggregate under-abstracted state
         * 
         * @param state The state to check
         * @param agent The agent
         */
        void checkUnderAbstractedState(StatePtr &state, const AbstractAgentPtr &agent);

        /**
         * @brief State abstraction: aggregate over-fine states (APE alignment)
         * 
         * This is the intermediate layer method that matches APE's Model.stateAbstraction.
         * Delegates to NamingManager.batchAbstract and triggers rebuild if naming changes.
         * 
         * @param naming Current naming (may be too fine)
         * @param targetState The target state to check
         * @param parentNaming Parent naming (coarser)
         * @param states Set of states to consider for abstraction
         * @return true if abstraction occurred (naming changed)
         */
        bool stateAbstraction(const NamingPtr &naming,
                             const StatePtr &targetState,
                             const NamingPtr &parentNaming,
                             const StatePtrSet &states);

        /**
         * @brief Pre-check trivial new state and refresh if needed
         * 
         * If state is trivial, resample multiple times and check top naming equivalence.
         * 
         * @param state The state to check
         * @param element The GUI tree element
         * @param activityPtr Activity name
         * @param agent The agent
         */
        void preCheckTrivialNewState(StatePtr &state, const ElementPtr &element,
                                     const stringPtr &activityPtr, const AbstractAgentPtr &agent);

        /**
         * @brief Validate all actions in the new state
         * 
         * Ensures only executable actions participate in priority calculation.
         * 
         * @param state The state whose actions to validate
         */
        void validateAllNewActions(const StatePtr &state);

        /**
         * @brief Restore Q-values after rebuild
         * 
         * After rebuild, newly created Action objects have Q-values initialized to 0.
         * This method restores Q-values from Agent's _reuseQValue mappings to Action objects.
         * 
         * Process:
         * 1. Iterate through all agents
         * 2. For each agent, iterate through all states in graph
         * 3. For each state, iterate through all actions
         * 4. Look up Q-value from agent's Q-value mapping by action hash
         * 5. Restore Q-value to Action object
         * 
         * Supports both ModelReusableAgent (single Q-value) and DoubleSarsaAgent (Q1 and Q2).
         */
        void restoreQValuesAfterRebuild();
        
        /// Smart pointer to the graph object managing all states and actions
        GraphPtr _graph;

        NamingManagerPtr _namingManager;
        
        /// Map from device ID to agent object
        /// Allows multiple devices to have different agents with different strategies
        AbstractAgentPtrStrMap _deviceIDAgentMap;
        
        /// User-specified preferences for customizing behavior
        PreferencePtr _preference;

        /// Parameters for communicating with network-based action models
        NetActionParam _netActionParam;

    };

    typedef std::shared_ptr<Model> ModelPtr;
}

#endif  // Model_H_
