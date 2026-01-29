/*
 * This code is licensed under the Fastbot license. You may obtain a copy of this license in the LICENSE.txt file in the root directory of this source tree.
 */
/**
 * @authors Jianqiang Guo, Yuhui Su, Zhao Zhang
 */
#ifndef Model_CPP_
#define Model_CPP_

#include "Model.h"
#include "StateFactory.h"
#include "reuse/ReuseState.h"
#include "../agent/DoubleSarsaAgent.h"
#include "../utils.hpp"
#include <ctime>
#include <iostream>

namespace fastbotx {

    /**
     * @brief Log state information with each widget and action on a separate line
     * 
     * This helper function formats state information for debugging/logging purposes.
     * It prints the state hash, all widgets, and all actions in a readable format.
     * Long strings are split across multiple log lines.
     * 
     * @param state The state to log (nullptr is handled gracefully)
     */
    inline void logStatePerLine(const StatePtr &state) {
#if !_DEBUG_
        (void)state;
        return;
#else
        if (state == nullptr) {
            BDLOGE("State is null, cannot log state information");
            return;
        }
        
        // Print state header with hash code
        BDLOG("{state: %lu", static_cast<unsigned long>(state->hash()));
        
        // Print each widget on a separate line for better readability
        BDLOG("widgets:");
        const auto &widgets = state->getWidgets();
        for (const auto &widget : widgets) {
            if (!widget) {
                continue;
            }
            std::string widgetStr = widget->toString();
            // If widget string is too long, split it across multiple log lines
            if (widgetStr.length() > FASTBOT_MAX_LOG_LEN) {
                logLongStringInfo("   " + widgetStr);
            } else {
                BDLOG("   %s", widgetStr.c_str());
            }
        }
        
        // Print each action on a separate line for better readability
        BDLOG("action:");
        const auto &actions = state->getActions();
        for (const auto &action : actions) {
            if (!action) {
                continue;
            }
            std::string actionStr = action->toString();
            // If action string is too long, split it across multiple log lines
            if (actionStr.length() > FASTBOT_MAX_LOG_LEN) {
                logLongStringInfo("   " + actionStr);
            } else {
                BDLOG("   %s", actionStr.c_str());
            }
        }
        
        BDLOG("}");
#endif
    }

    /**
     * @brief Factory method to create a new Model instance
     * 
     * Uses new + shared_ptr instead of make_shared because the constructor is protected
     * and make_shared cannot access protected constructors from outside the class.
     * 
     * @return Shared pointer to a new Model instance
     */
    std::shared_ptr<Model> Model::create() {
        return std::shared_ptr<Model>(new Model());
    }

    /**
     * @brief Constructor for Model class
     * 
     * Initializes the model with:
     * - A new Graph instance for state management
     * - Preference singleton instance
     * - Network action parameters set to default values
     */
    Model::Model() {
#ifndef FASTBOT_VERSION
    // Use build timestamp if available, otherwise use compile-time date/time
    #ifdef FASTBOT_BUILD_TIMESTAMP
        #define FASTBOT_VERSION FASTBOT_BUILD_TIMESTAMP
    #else
        // Fallback to compiler's __DATE__ and __TIME__ macros
        #define FASTBOT_VERSION __DATE__ " " __TIME__
    #endif
#endif
        BLOG("----Fastbot native version " FASTBOT_VERSION "----\n");
        this->_graph = std::make_shared<Graph>();
        this->_namingManager = std::make_shared<NamingManager>(std::make_shared<NamingFactory>());
        this->_preference = Preference::inst();
        this->_netActionParam.netActionTaskid = 0;
    }


    /**
     * @brief General entry point for getting next operation step according to RL model
     * 
     * This is the main entry point that accepts XML content as a string.
     * It parses the XML string into an Element object and delegates to the
     * ElementPtr-based version of getOperate().
     * 
     * @param descContent XML content of the current page as a string
     * @param activity Activity name string
     * @param deviceID Device ID string (default: empty string uses default device)
     * @return Next operation step in JSON format, or empty string if parsing fails
     */
    std::string Model::getOperate(const std::string &descContent, const std::string &activity,
                                  const std::string &deviceID) {
        std::lock_guard<std::recursive_mutex> lock(_mutex);
        // Parse XML string into Element object using tinyxml2
        ElementPtr elem = Element::createFromXml(descContent);
        if (elem == nullptr) {
            return "";
        }
        // Delegate to ElementPtr-based version
        return this->getOperate(elem, activity, deviceID);
    }

    /**
     * @brief Create and add an agent to the model for a specific device
     * 
     * Creates a new agent using the AgentFactory, adds it to the device-agent map,
     * and registers it as a listener to the graph for state change notifications.
     * 
     * @param deviceIDString Device ID string (empty string uses default device ID)
     * @param agentType The type of algorithm/agent to create
     * @param deviceType The type of device (default: Normal)
     * @return Shared pointer to the newly created agent
     */
    AbstractAgentPtr Model::addAgent(const std::string &deviceIDString, AlgorithmType agentType,
                                     DeviceType deviceType) {
        std::lock_guard<std::recursive_mutex> lock(_mutex);
        // Create agent using factory pattern
        auto agent = AgentFactory::create(agentType, shared_from_this(), deviceType);
        
        // Use default device ID if empty string provided
        const std::string &deviceID = deviceIDString.empty() ? ModelConstants::DefaultDeviceID
                                                             : deviceIDString;
        
        // Add the device-agent pair to the map
        this->_deviceIDAgentMap.emplace(deviceID, agent);
        
        // Register agent as a listener to graph updates
        // This allows the agent to be notified when new states are added
        this->_graph->addListener(agent);
        
        return agent;
    }

    /**
     * @brief Get the agent for a specific device ID
     * 
     * @param deviceID Device ID string (empty string uses default device ID)
     * @return Shared pointer to the agent, or nullptr if not found
     */
    AbstractAgentPtr Model::getAgent(const std::string &deviceID) const {
        std::lock_guard<std::recursive_mutex> lock(_mutex);
        const std::string &d = deviceID.empty() ? ModelConstants::DefaultDeviceID : deviceID;
        auto iter = this->_deviceIDAgentMap.find(d);
        if (iter != this->_deviceIDAgentMap.end()) {
            return iter->second;
        }
        return nullptr;
    }


    /**
     * @brief Get next operation step from Element object, returning JSON string
     * 
     * This method wraps the core getOperateOpt() method and converts the result
     * to a JSON string format.
     * 
     * @param element XML Element object of the current page
     * @param activity Activity name string
     * @param deviceID Device ID string (default: empty string uses default device)
     * @return Next operation step in JSON format
     */
    std::string Model::getOperate(const ElementPtr &element, const std::string &activity,
                                  const std::string &deviceID) {
        std::lock_guard<std::recursive_mutex> lock(_mutex);
        OperatePtr operate = getOperateOpt(element, activity, deviceID);
        if (!operate) {
            return "";
        }
        // Convert operation object to JSON string
        std::string operateString = operate->toString();
        return operateString;
    }


    /**
     * @brief Get custom action from preference if one exists for this page
     * 
     * Checks if the user has specified a custom action for this activity/page
     * in the preference settings. Returns nullptr if no custom action is defined.
     * 
     * @param activity Activity name string
     * @param element XML Element object of the current page
     * @return Custom action if exists, nullptr otherwise
     */
    ActionPtr Model::getCustomActionIfExists(const std::string &activity, const ElementPtr &element) const {
        if (this->_preference) {
            BLOG("try get custom action from preference");
            return this->_preference->resolvePageAndGetSpecifiedAction(activity, element);
        }
        return nullptr;
    }

    /**
     * @brief Get or create an activity string pointer
     * 
     * This method optimizes memory usage by reusing existing activity string pointers
     * from the graph's visited activities set. If the activity already exists,
     * returns the cached shared pointer. Otherwise, creates a new one.
     * 
     * Performance optimization:
     * - Reuses existing string pointers to avoid duplicate string storage
     * - Uses hash-based set lookup for O(log n) complexity
     * 
     * @param activity The activity name string
     * @return Shared pointer to the activity string (cached or newly created)
     * 
     * @note The returned pointer may be from the cache or newly created.
     *       Newly created pointers will be added to the graph's visited activities
     *       when the state is added via createAndAddState().
     */
    stringPtr Model::getOrCreateActivityPtr(const std::string &activity) {
        // Fast path (no allocation): scan visited set for an existing shared_ptr.
        // This avoids allocating a temporary shared_ptr<string> just to probe the set.
        if (this->_graph) {
            auto cached = this->_graph->findVisitedActivityPtr(activity);
            if (cached) {
                return cached;
            }
        }
        // Miss: allocate once and return (Graph will store it when state is added).
        return std::make_shared<std::string>(activity);
    }

    /**
     * @brief Get or create an agent for the given device ID
     * 
     * This method retrieves an agent for the specified device ID. If no agent exists
     * for the device ID, returns the default agent. If no agents exist at all,
     * creates a default reuse agent.
     * 
     * Performance optimization:
     * - Uses find() instead of [] operator to avoid creating unnecessary map entries
     * - Falls back to default device ID if device ID is not found
     * 
     * @param deviceID The device ID string (empty string uses default device ID)
     * @return Shared pointer to the agent for the device
     * 
     * @note If the device ID is not found, returns the default agent instead of
     *       creating a new one. This ensures all devices have an agent to use.
     */
    AbstractAgentPtr Model::getOrCreateAgent(const std::string &deviceID) {
        // Create a default agent if map is empty
        if (this->_deviceIDAgentMap.empty()) {
            BLOG("%s", "use DoubleSarsaAgent as the default agent");
            this->addAgent(ModelConstants::DefaultDeviceID, AlgorithmType::DoubleSarsa);
        }
        
        // Use find() instead of [] to avoid creating unnecessary map entries
        // Performance: O(log n) lookup without side effects
        auto agentIterator = this->_deviceIDAgentMap.find(deviceID);
        
        if (agentIterator == this->_deviceIDAgentMap.end()) {
            // Device ID not found, return the default agent
            // Use find() again to avoid [] operator side effects
            auto defaultIterator = this->_deviceIDAgentMap.find(ModelConstants::DefaultDeviceID);
            if (defaultIterator != this->_deviceIDAgentMap.end()) {
                return defaultIterator->second;
            }
            // Should not reach here if addAgent worked correctly, but handle gracefully
            return nullptr;
        } else {
            // Found the agent for this device ID
            return agentIterator->second;
        }
    }

    /**
     * @brief Create a new state from element and add it to the graph
     * 
     * Creates a state object based on the agent's algorithm type, then adds it
     * to the graph. The graph will deduplicate if a similar state already exists.
     * Marks the state as visited with the current graph timestamp.
     * 
     * @param element XML Element object of the current page (must not be nullptr)
     * @param agent The agent to use for state creation (determines state type)
     * @param activityPtr Shared pointer to activity name string
     * @return Shared pointer to the created/existing state, or nullptr if element is null
     */
    StatePtr Model::createAndAddState(const ElementPtr &element, const AbstractAgentPtr &agent,
                                      const stringPtr &activityPtr) {
        // Validate input
        if (element == nullptr) {
            return nullptr;
        }
        
        // Create state according to the agent's algorithm type
        // The state includes all possible actions based on widgets in the element
        NamingPtr naming = this->_namingManager
                            ? this->_namingManager->getNamingForTree(element, activityPtr)
                            : nullptr;
        StatePtr state = StateFactory::createState(agent->getAlgorithmType(), activityPtr, element, naming);
        
        // Add state to graph (may return existing state if duplicate)
        // The graph handles deduplication based on state hash
        state = this->_graph->addState(state);

        // APE alignment: stamp this GUI tree snapshot with graph timestamp
        // (used for rebuild transition sorting via GUITreeTransition::getTimestamp()).
        element->setTimestamp(static_cast<int>(this->_graph->getTimestamp()));
        
        // Mark state as visited with current graph timestamp
        state->visit(this->_graph->getTimestamp());

        // Record GUI tree history for rebuild/refinement logic
        state->appendTree(element);
        state->resolveActionTargets(element);

        // APE alignment: Handle entry state (when app activity just started)
        // Note: In native, we need to check agent's flags, but they're protected
        // For now, we'll handle this in addStateTransitionIfPossible when source is null
        
        // Pre-check trivial new state and refresh if needed (APE alignment)
        preCheckTrivialNewState(state, element, activityPtr, agent);

        // Validate all actions in the new state (APE alignment)
        validateAllNewActions(state);

        // Pre-evolve model: check under/over abstracted states before adding to graph (APE alignment)
        preEvolveModel(state, agent);

        // Record state transition (previous state/action -> current state)
        addStateTransitionIfPossible(agent, state, element);
        
        return state;
    }

    void Model::addStateTransitionIfPossible(const AbstractAgentPtr &agent,
                                             const StatePtr &newState,
                                             const ElementPtr &newTree) {
        if (!agent || !newState) {
            return;
        }
        StatePtr source = agent->getCurrentState();
        ActivityStateActionPtr action = agent->getCurrentAction();
        
        // APE alignment: Handle entry state (when app activity just started)
        if (!source || !action) {
            // Check if this is an entry state (app just started)
            // In native, we check agent's appActivityJustStarted flag
            // Note: This flag should be set by the caller when app activity starts
            if (newTree) {
                _graph->addEntryGUITree(newTree);
                // Check if it's a clean entry (appActivityJustStartedFromClean)
                // This would need to be passed as a parameter or checked elsewhere
            }
            return;
        }
        
        ElementPtr sourceTree = source->getLatestTree();
        StateTransitionPtr existingTransition;
        StateTransitionPtr newTransition;
        TransitionVisitType type = this->_graph->addTransition(source, action, newState,
                                                               sourceTree, newTree,
                                                               existingTransition,
                                                               newTransition);
        
        // APE alignment: Set currentStateTransition in agent for checkNonDeterministicTransitions
        StateTransitionPtr currentTransition = newTransition ? newTransition : existingTransition;
        if (currentTransition && agent) {
            agent->setCurrentStateTransition(currentTransition);
        }
        
        // APE alignment: Immediate check for non-deterministic transitions
        if (type == TransitionVisitType::NEW_ACTION_TARGET && this->_namingManager) {
            NamingPtr oldNaming = this->_namingManager->getNaming();
            NamingPtr newNaming = this->_namingManager->resolveNonDeterminism(oldNaming,
                                                                              existingTransition,
                                                                              newTransition,
                                                                              this->_graph);
            if (newNaming && oldNaming != newNaming) {
                rebuildModel();
            }
        }
    }

    void Model::rebuildModel() {
        std::lock_guard<std::recursive_mutex> lock(_mutex);
        GraphPtr oldGraph = this->_graph;
        GraphPtr newGraph = std::make_shared<Graph>();
        std::map<ElementPtr, StatePtr> treeToState;

        // Align rebuild state type with current runtime agent, instead of hard-coding Reuse.
        AlgorithmType rebuildAlgo = AlgorithmType::DoubleSarsa;
        if (!this->_deviceIDAgentMap.empty() && this->_deviceIDAgentMap.begin()->second) {
            rebuildAlgo = this->_deviceIDAgentMap.begin()->second->getAlgorithmType();
        }

        // Rebuild states from stored GUI trees
        for (const auto &state : oldGraph->getStates()) {
            if (!state) {
                continue;
            }
            for (const auto &tree : state->getTreeHistory()) {
                if (!tree) {
                    continue;
                }
                stringPtr activityPtr;
                if (state->getActivityString()) {
                    activityPtr = getOrCreateActivityPtr(*(state->getActivityString().get()));
                } else {
                    activityPtr = getOrCreateActivityPtr("");
                }
                NamingPtr naming = this->_namingManager ? this->_namingManager->getNaming() : nullptr;
                StatePtr rebuilt = StateFactory::createState(rebuildAlgo, activityPtr, tree, naming);
                rebuilt = newGraph->addState(rebuilt);
                rebuilt->appendTree(tree);
                rebuilt->resolveActionTargets(tree);
                treeToState[tree] = rebuilt;
            }
        }

        // Rebuild transitions using recorded GUI trees
        for (const auto &transition : oldGraph->getTransitions()) {
            if (!transition) {
                continue;
            }
            StatePtr source = treeToState[transition->getSourceTree()];
            StatePtr target = treeToState[transition->getTargetTree()];
            if (!source || !target) {
                continue;
            }
            ActivityStateActionPtr action;
            if (transition->getTargetWidgetHash() != 0) {
                action = source->getActionByTargetHash(transition->getActionType(),
                                                       transition->getTargetWidgetHash());
                if (!action) {
                    NamingPtr naming = this->_namingManager ? this->_namingManager->getNaming() : nullptr;
                    if (naming) {
                        ReuseStatePtr temp = ReuseState::create(transition->getSourceTree(),
                                                               source->getActivityString(),
                                                               naming);
                        if (temp) {
                            NamePtr targetName = temp->getNameForWidgetHash(transition->getTargetWidgetHash());
                            action = source->relocateAction(transition->getActionType(), targetName);
                        }
                    }
                }
            } else {
                action = source->getActionByType(transition->getActionType());
            }
            if (!action) {
                continue;
            }
            StateTransitionPtr unusedExisting;
            StateTransitionPtr unusedNew;
            newGraph->addTransition(source, action, target,
                                    transition->getSourceTree(),
                                    transition->getTargetTree(),
                                    unusedExisting, unusedNew);
        }

        // Replace graph and re-register listeners
        this->_graph = newGraph;
        for (const auto &pair : this->_deviceIDAgentMap) {
            newGraph->addListener(pair.second);
        }
        
        // Restore Q-values after rebuild
        restoreQValuesAfterRebuild();
    }

    /**
     * @brief Select an action based on state, agent, and custom preferences
     * 
     * This method implements the action selection logic:
     * 1. Uses custom action from preference if available
     * 2. Checks for blocked state and returns RESTART if needed
     * 3. Otherwise, asks the agent to resolve a new action
     * 4. Updates agent strategy and marks action as visited if it's a model action
     * 
     * @param state The current state (may be modified)
     * @param agent The agent to use for action selection (may be modified)
     * @param customAction Custom action from preference, if any
     * @param actionCost Output parameter: time cost for action generation in seconds
     * @return Selected action, or nullptr if selection failed
     */
    ActionPtr Model::selectAction(StatePtr &state, AbstractAgentPtr &agent, ActionPtr customAction, double &actionCost) {
        double startGeneratingActionTimestamp = currentStamp();
        actionCost = 0.0;
        ActionPtr action = customAction; // Use custom action if provided

        // Log state information for debugging
        logStatePerLine(state);

        // Check if preference indicates we should skip model actions (listen mode)
        bool shouldSkipActionsFromModel = this->_preference ? this->_preference->skipAllActionsFromModel() : false;
        if (shouldSkipActionsFromModel) {
            LOGI("listen mode skip get action from model");
        }

        // If no custom action specified and not in listen mode, get action from agent
        if (customAction == nullptr && !shouldSkipActionsFromModel) {
            // Check if we're in a blocked state and should restart
            if (-1 != BLOCK_STATE_TIME_RESTART &&
                -1 != Preference::inst()->getForceMaxBlockStateTimes() &&
                agent->getCurrentStateBlockTimes() > BLOCK_STATE_TIME_RESTART) {
                // Force restart action when stuck in blocked state
                action = Action::RESTART;
                BLOG("Ran into a block state %s", state ? state->getId().c_str() : "");
            } else {
                // Ask agent to resolve a new action (this is the main RL model entry point)
                auto resolvedAction = agent->resolveNewAction();
                action = std::dynamic_pointer_cast<Action>(resolvedAction);
                
                // Update agent's strategy based on the new action
                agent->updateStrategy();
                
                if (action == nullptr) {
                    BDLOGE("get null action!!!!");
                    return nullptr; // Handle null action gracefully
                }
            }
            
            // Calculate action generation time cost
            double endGeneratingActionTimestamp = currentStamp();
            actionCost = endGeneratingActionTimestamp - startGeneratingActionTimestamp;
            
            // If this is a model action and state exists, mark it as visited and update agent
            if (action->isModelAct() && state) {
                action->visit(this->_graph->getTimestamp());
                // Update agent's current state/action with new state/action
                agent->moveForward(state);
            }
        }
        
        return action;
    }

    /**
     * @brief Convert an action to an operate object and apply patches
     * 
     * Converts an Action object to a DeviceOperateWrapper (OperatePtr) that can be
     * executed. If the action requires a target widget, extracts widget information.
     * Applies preference patches and optionally clears state details for memory optimization.
     * 
     * @param action The action to convert (nullptr returns NOP operation)
     * @param state The current state (used for detail clearing optimization)
     * @return OperatePtr The operation object ready for execution
     */
    OperatePtr Model::convertActionToOperate(ActionPtr action, StatePtr state) {
        if (action == nullptr) {
            // Return no-operation if action is null
            return DeviceOperateWrapper::OperateNop;
        }

        BLOG("selected action %s", action->toString().c_str());
        
        // Convert action to operation object
        OperatePtr opt = action->toOperate();
        if (!opt) {
            return DeviceOperateWrapper::OperateNop;
        }

        // If action requires a target widget, extract widget information
        if (action->requireTarget()) {
            if (auto stateAction = std::dynamic_pointer_cast<fastbotx::ActivityStateAction>(action)) {
                std::shared_ptr<Widget> widget = stateAction->getTarget();
                if (widget) {
                    // Serialize widget to JSON and attach to operation
                    std::string widget_str = widget->toJson();
                    opt->widget = widget_str;
                    BLOG("stateAction Widget: %s", widget_str.c_str());
                }
            }
        }

        // Apply preference patches to the operation (e.g., custom modifications)
        if (this->_preference) {
            this->_preference->patchOperate(opt);
        }

        // Memory optimization: clear state details after use if enabled
        // This reduces memory usage for states that are no longer needed in detail
        if (DROP_DETAIL_AFTER_SATE && state && !state->hasNoDetail()) {
            state->clearDetails();
        }

        return opt;
    }

    /**
     * @brief Core method for getting next operation step and updating RL model
     * 
     * This is the main orchestration method that:
     * 1. Gets custom action from preference if available
     * 2. Gets or creates activity pointer (memory optimization)
     * 3. Gets or creates agent for the device
     * 4. Creates and adds state to the graph
     * 5. Selects an action using the agent or custom action
     * 6. Converts action to operation object
     * 7. Logs performance metrics
     * 
     * @param element XML Element object of the current page
     * @param activity Activity name string
     * @param deviceID Device ID string (default: empty string uses default device)
     * @return DeviceOperateWrapper object containing the next operation to perform
     * 
     * @note This method updates the RL model by adding states and actions to the graph
     */
    OperatePtr Model::getOperateOpt(const ElementPtr &element, const std::string &activity,
                                    const std::string &deviceID) {
        std::lock_guard<std::recursive_mutex> lock(_mutex);
        if (!element) {
            return DeviceOperateWrapper::OperateNop;
        }
        // Record method start time for performance tracking
        double methodStartTimestamp = currentStamp();
        
        // Step 1: Get custom action from preference if user specified one
        ActionPtr customAction = getCustomActionIfExists(activity, element);
        
        // Step 2: Get or create activity pointer (reuses existing pointers for memory efficiency)
        stringPtr activityPtr = getOrCreateActivityPtr(activity);
        
        // Step 3: Get or create agent for this device (creates default if needed)
        AbstractAgentPtr agent = getOrCreateAgent(deviceID);
        if (!agent) {
            return DeviceOperateWrapper::OperateNop;
        }
        
        // Step 4: Create state from element and add to graph
        // The graph handles deduplication if a similar state already exists
        StatePtr state = createAndAddState(element, agent, activityPtr);
        
        // Record state generation time for performance tracking
        double stateGeneratedTimestamp = currentStamp();
        
        // Step 5: Select action (either custom, restart, or from agent)
        double actionCost = 0.0;
        ActionPtr action = selectAction(state, agent, customAction, actionCost);
        
        // Handle null action gracefully
        if (action == nullptr) {
            return DeviceOperateWrapper::OperateNop;
        }
        
        // Step 6: Convert action to operation object and apply patches
        OperatePtr opt = convertActionToOperate(action, state);
        
        // Record end time and log performance metrics
        double methodEndTimestamp = currentStamp();
        BLOG("build state cost: %.3fs action cost: %.3fs total cost %.3fs",
             stateGeneratedTimestamp - methodStartTimestamp,
             actionCost,
             methodEndTimestamp - methodStartTimestamp);
        
        return opt;
    }

    /**
     * @brief Destructor for Model class
     * 
     * Clears the device-agent map to release all agent resources.
     * The graph and preference are shared pointers and will be automatically
     * cleaned up when the last reference is released.
     */
    Model::~Model() {
        this->_deviceIDAgentMap.clear();
    }

    void Model::preEvolveModel(StatePtr &state, const AbstractAgentPtr &agent) {
        if (!state || !agent || !this->_namingManager) {
            return;
        }
        // Step 1: Check under-abstracted state (aggregation)
        checkUnderAbstractedState(state, agent);
        // Step 2: Check over-abstracted state (refinement)
        checkOverAbstractedState(state, agent);
        // Step 3: Check under-abstracted state again (aggregation after refinement)
        checkUnderAbstractedState(state, agent);
    }

    void Model::checkOverAbstractedState(StatePtr &state, const AbstractAgentPtr &agent) {
        if (!state || !agent || !this->_namingManager) {
            return;
        }
        int iteration = 0;
        (void)iteration; // suppress unused warning in non-debug builds where BDLOG is a no-op
        while (true) {
            BDLOG("Check over-abstracted state %s: #%d", state->getId().c_str(), iteration++);
            StatePtr oldState = state;
            // Try action refinement for targeted actions
            ActivityStateActionPtrVec targetedActions = state->targetActions();
            std::sort(targetedActions.begin(), targetedActions.end(),
                      [](const ActivityStateActionPtr &a, const ActivityStateActionPtr &b) {
                          return a->hash() < b->hash();
                      });
            bool refined = false;
            for (const auto &action : targetedActions) {
                if (!action || !action->requireTarget()) {
                    continue;
                }
                NamingPtr oldNaming = this->_namingManager->getNaming();
                NamingPtr newNaming = this->_namingManager->refineForAliasedAction(action);
                if (newNaming && newNaming != oldNaming) {
                    this->_namingManager->setCurrentNaming(newNaming);
                    rebuildModel();
                    // Rebuild state with new naming
                    ElementPtr latestTree = state->getLatestTree();
                    if (latestTree) {
                        NamingPtr updatedNaming = this->_namingManager->getNamingForTree(latestTree, state->getActivityString());
                        state = StateFactory::createState(agent->getAlgorithmType(), state->getActivityString(), latestTree, updatedNaming);
                        state = this->_graph->addState(state);
                        state->visit(this->_graph->getTimestamp());
                        state->appendTree(latestTree);
                        state->resolveActionTargets(latestTree);
                    }
                    refined = true;
                    break;
                }
            }
            if (!refined || oldState == state) {
                break;
            }
        }
    }

    void Model::checkUnderAbstractedState(StatePtr &state, const AbstractAgentPtr &agent) {
        if (!state || !agent || !this->_namingManager) {
            return;
        }
        NamingPtr naming = state->getCurrentNaming();
        if (!naming || !naming->getParent()) {
            return;
        }
        int iteration = 0;
        (void)iteration;
        while (true) {
            BDLOG("Check under-abstracted state %s: #%d", state->getId().c_str(), iteration++);
            StatePtr oldState = state;
            NamingPtr parentNaming = naming;
            while (parentNaming && parentNaming->getParent()) {
                // Collect all states with current naming or its descendants
                StatePtrSet states;
                for (const auto &s : this->_graph->getStates()) {
                    if (s && s->getCurrentNaming()) {
                        NamingPtr sNaming = s->getCurrentNaming();
                        // Check if sNaming is a descendant of naming
                        while (sNaming) {
                            if (sNaming == naming) {
                                states.insert(s);
                                break;
                            }
                            sNaming = sNaming->getParent();
                        }
                    }
                }
                BDLOG("Check under-abstracted states collected %zu targets for naming. The state is %s. #%d",
                      states.size(), state->getId().c_str(), iteration);
                // APE alignment: Use stateAbstraction intermediate layer
                bool abstracted = stateAbstraction(naming, state, parentNaming, states);
                if (abstracted) {
                    NamingPtr updated = this->_namingManager->getNaming();
                    rebuildModel();
                    // Rebuild state with new naming
                    ElementPtr latestTree = state->getLatestTree();
                    if (latestTree) {
                        NamingPtr updatedNaming = this->_namingManager->getNamingForTree(latestTree, state->getActivityString());
                        state = StateFactory::createState(agent->getAlgorithmType(), state->getActivityString(), latestTree, updatedNaming);
                        state = this->_graph->addState(state);
                        state->visit(this->_graph->getTimestamp());
                        state->appendTree(latestTree);
                        state->resolveActionTargets(latestTree);
                        naming = updated;
                    }
                    break;
                }
                parentNaming = parentNaming->getParent();
            }
            if (oldState == state) {
                break;
            }
        }
    }

    void Model::preCheckTrivialNewState(StatePtr &state, const ElementPtr &element,
                                        const stringPtr &activityPtr, const AbstractAgentPtr &agent) {
        if (!state || !state->isTrivialState() || !element || !agent) {
            return;
        }
        BDLOG("Pre-check trivial new state: %s", state->getId().c_str());
        // Note: In native, we don't have direct access to AccessibilityNodeInfo for resampling
        // This is a simplified version that checks top naming equivalence
        // Full APE implementation would resample multiple times via getRootInActiveWindowSlow()
        if (this->_namingManager) {
            NamingPtr topNaming = this->_namingManager->getTopNaming();
            if (topNaming) {
                // Check if state is equivalent under top naming
                ReuseStatePtr tempState = ReuseState::create(element, activityPtr, topNaming);
                if (tempState) {
                    StateKeyPtr topStateKey = tempState->getStateKey();
                    if (topStateKey && state->getStateKey()) {
                        // If equivalent, state is stable
                        BDLOG("Trivial state is top naming equivalent");
                    }
                }
            }
        }
    }

    void Model::validateAllNewActions(const StatePtr &state) {
        if (!state) {
            return;
        }
        for (const auto &action : state->getActions()) {
            if (!action) {
                continue;
            }
            // Validate action: check if it's executable
            // In APE, this calls validateNewAction which checks if action can be resolved
            // Native implementation: check if action is valid and can be resolved
            if (action->requireTarget()) {
                auto activityStateAction = std::dynamic_pointer_cast<ActivityStateAction>(action);
                if (activityStateAction) {
                    // Action is validated if it has valid target and can be resolved
                    // The actual resolution happens later in resolveAction
                    if (!activityStateAction->isValid()) {
                        BDLOG("Action %s is invalid", action->toString().c_str());
                    }
                }
            }
        }
    }

    bool Model::stateAbstraction(const NamingPtr &naming,
                                 const StatePtr &targetState,
                                 const NamingPtr &parentNaming,
                                 const StatePtrSet &states) {
        // APE alignment: Model.stateAbstraction intermediate layer
        if (!naming || !targetState || !parentNaming || !this->_namingManager) {
            return false;
        }
        int oldVersion = this->_namingManager->getVersion();
        NamingPtr updated = this->_namingManager->batchAbstract(naming, targetState, parentNaming, states, this->_graph);
        if (updated && updated != naming) {
            this->_namingManager->setCurrentNaming(updated);
            int newVersion = this->_namingManager->getVersion();
            if (newVersion != oldVersion) {
                BLOG("State abstraction: naming updated from %s to %s", 
                     naming ? std::to_string(naming->hash()).c_str() : "null",
                     updated ? std::to_string(updated->hash()).c_str() : "null");
                return true;
            }
        }
        return false;
    }

    bool Model::resolveNonDeterministicTransitions(const StateTransitionPtr &edge) {
        // APE alignment: Model.resolveNonDeterministicTransitions
        if (!edge || !this->_namingManager) {
            return false;
        }
        // Check if action is BACK (should be deterministic)
        if (edge->getActionType() == ActionType::BACK) {
            return false; // back should be deterministic
        }
        
        StatePtr source = edge->getSource();
        if (!source) {
            return false;
        }
        
        // Find action in source state
        ActivityStateActionPtr action = nullptr;
        if (edge->getTargetWidgetHash() != 0) {
            action = source->getActionByTargetHash(edge->getActionType(), edge->getTargetWidgetHash());
        } else {
            action = source->getActionByType(edge->getActionType());
        }
        if (!action) {
            return false;
        }
        
        // Check if there are multiple transitions with same source and action (non-deterministic)
        std::vector<StateTransitionPtr> outTransitions = _graph->getOutStateTransitions(action);
        if (outTransitions.size() <= 1) {
            return false; // Not non-deterministic
        }
        
        // This is a non-deterministic transition, trigger refinement
        int oldVersion = this->_namingManager->getVersion();
        // Find the existing transition (first one) and new transition (this one)
        StateTransitionPtr existingTransition = nullptr;
        for (const auto &t : outTransitions) {
            if (t != edge) {
                existingTransition = t;
                break;
            }
        }
        if (!existingTransition) {
            return false;
        }
        
        NamingPtr oldNaming = this->_namingManager->getNaming();
        NamingPtr newNaming = this->_namingManager->resolveNonDeterminism(oldNaming,
                                                                          existingTransition,
                                                                          edge,
                                                                          this->_graph);
        if (newNaming && oldNaming != newNaming) {
            this->_namingManager->setCurrentNaming(newNaming);
            int newVersion = this->_namingManager->getVersion();
            if (newVersion != oldVersion) {
                BLOG("Eliminating non-deterministic transitions: naming updated");
                rebuildModel();
                return true;
            }
        }
        return false;
    }

    Model& Model::rebuild() {
        std::lock_guard<std::recursive_mutex> lock(_mutex);
        BLOG("Start rebuilding model...");
        
        // Step 1: Identify states to remove (states whose naming no longer matches)
        StatePtrSet statesToRemove;
        std::vector<StateTransitionPtr> removedTransitions;
        std::vector<ElementPtr> affectedTrees;
        
        const auto &allStates = _graph->getStates();
        for (const auto &state : allStates) {
            if (!state) {
                continue;
            }
            // Check if state's naming matches current naming manager
            const auto &treeHistory = state->getTreeHistory();
            bool shouldRemove = false;
            for (const auto &tree : treeHistory) {
                if (!tree) {
                    continue;
                }
                auto activityPtr = state->getActivityString();
                if (!activityPtr) {
                    continue;
                }
                NamingPtr currentNaming = state->getCurrentNaming();
                if (!currentNaming) {
                    continue;
                }
                // Get the naming that should be used for this tree
                NamingPtr expectedNaming = _namingManager->getNamingForTree(tree, activityPtr);
                if (expectedNaming != currentNaming) {
                    shouldRemove = true;
                    break;
                }
            }
            if (shouldRemove) {
                statesToRemove.insert(state);
                // Collect affected trees
                for (const auto &tree : state->getTreeHistory()) {
                    if (tree) {
                        affectedTrees.push_back(tree);
                    }
                }
            }
        }
        
        // Step 2: Remove states and collect transitions
        for (const auto &state : statesToRemove) {
            BLOG("> Removing state %s", state->getId().c_str());
            _graph->removeState(state, removedTransitions);
        }
        
        // APE alignment: Collect GUITreeTransitions from removed transitions
        GUITreeTransitionPtrVec treeTransitions;
        for (const auto &transition : removedTransitions) {
            if (!transition) {
                continue;
            }
            const auto &treeTransitionsList = transition->getGUITreeTransitions();
            for (const auto &treeTransition : treeTransitionsList) {
                if (treeTransition) {
                    treeTransitions.push_back(treeTransition);
                }
            }
        }
        
        BLOG("> Removing (%zu) old states and (%zu) transitions finished. Collected %zu tree transitions.", 
             statesToRemove.size(), removedTransitions.size(), treeTransitions.size());
        
        // Step 3: Sort tree transitions by timestamp (APE alignment)
        std::sort(treeTransitions.begin(), treeTransitions.end(),
                 [](const GUITreeTransitionPtr &a, const GUITreeTransitionPtr &b) {
                     if (!a || !b) {
                         return false;
                     }
                     return a->getTimestamp() < b->getTimestamp();
                 });
        
        // Step 4: Rebuild affected states
        // Collect activity info before removing states
        std::map<ElementPtr, stringPtr, Comparator<Element>> treeToActivity;
        for (const auto &state : statesToRemove) {
            if (!state) {
                continue;
            }
            auto activityPtr = state->getActivityString();
            if (!activityPtr) {
                continue;
            }
            for (const auto &tree : state->getTreeHistory()) {
                if (tree) {
                    treeToActivity[tree] = activityPtr;
                }
            }
        }
        
        // Sort affected trees (simplified - in full implementation would sort by timestamp)
        std::set<ElementPtr, Comparator<Element>> sortedTrees(affectedTrees.begin(), affectedTrees.end());
        
        // Step 4: Rebuild affected states and build tree-to-state mapping
        std::map<ElementPtr, StatePtr, Comparator<Element>> treeToState;
        for (const auto &tree : sortedTrees) {
            if (!tree) {
                continue;
            }
            // Get activity from collected map
            stringPtr activityPtr;
            auto it = treeToActivity.find(tree);
            if (it != treeToActivity.end()) {
                activityPtr = it->second;
            } else {
                // Fallback: use empty activity
                activityPtr = std::make_shared<std::string>("");
            }
            
            // Create a temporary agent to rebuild state
            AbstractAgentPtr tempAgent = getOrCreateAgent("");
            if (tempAgent) {
                StatePtr rebuiltState = createAndAddState(tree, tempAgent, activityPtr);
                if (rebuiltState) {
                    treeToState[tree] = rebuiltState;
                }
            }
        }
        
        // Step 5: Re-add transitions (APE alignment)
        // APE: For each GUITreeTransition, get source/target states from trees and rebuild action
        size_t reAddedCount = 0;
        for (const auto &treeTransition : treeTransitions) {
            if (!treeTransition) {
                continue;
            }
            ElementPtr sourceTree = treeTransition->getSource();
            ElementPtr targetTree = treeTransition->getTarget();
            ActivityStateActionPtr treeAction = treeTransition->getAction();
            if (!sourceTree || !targetTree) {
                continue;
            }
            
            // Find source and target states from rebuilt states
            auto sourceIt = treeToState.find(sourceTree);
            auto targetIt = treeToState.find(targetTree);
            
            // If not found in rebuilt states, try to find in existing graph
            StatePtr sourceState = (sourceIt != treeToState.end()) ? sourceIt->second : nullptr;
            StatePtr targetState = (targetIt != treeToState.end()) ? targetIt->second : nullptr;
            
            if (!sourceState) {
                // Try to find source state in graph by tree
                for (const auto &state : _graph->getStates()) {
                    if (!state) {
                        continue;
                    }
                    for (const auto &tree : state->getTreeHistory()) {
                        if (tree == sourceTree) {
                            sourceState = state;
                            break;
                        }
                    }
                    if (sourceState) {
                        break;
                    }
                }
            }
            
            if (!targetState) {
                // Try to find target state in graph by tree
                for (const auto &state : _graph->getStates()) {
                    if (!state) {
                        continue;
                    }
                    for (const auto &tree : state->getTreeHistory()) {
                        if (tree == targetTree) {
                            targetState = state;
                            break;
                        }
                    }
                    if (targetState) {
                        break;
                    }
                }
            }
            
            if (!sourceState || !targetState) {
                BDLOG("Cannot find source or target state for tree transition, skipping");
                continue;
            }
            
            // APE alignment: Rebuild action from treeTransition
            // If treeAction is available, use it; otherwise rebuild from state
            ActivityStateActionPtr action = treeAction;
            if (!action && treeTransition->getAction()) {
                // Try to find action in source state by matching action type and target
                ActionType actionType = treeTransition->getAction()->getActionType();
                if (treeTransition->getAction()->requireTarget() && treeTransition->getAction()->getTarget()) {
                    uintptr_t targetHash = treeTransition->getAction()->getTarget()->hash();
                    action = sourceState->getActionByTargetHash(actionType, targetHash);
                    if (!action) {
                        // Try to relocate action using target name
                        NamingPtr naming = _namingManager ? _namingManager->getNamingForTree(sourceTree, sourceState->getActivityString()) : nullptr;
                        if (naming) {
                            ReuseStatePtr temp = ReuseState::create(sourceTree, sourceState->getActivityString(), naming);
                            if (temp) {
                                NamePtr targetName = temp->getNameForWidgetHash(targetHash);
                                action = sourceState->relocateAction(actionType, targetName);
                            }
                        }
                    }
                } else {
                    // Action without target: get by type
                    action = sourceState->getActionByType(actionType);
                }
            }
            
            if (!action) {
                BDLOG("Cannot rebuild action for tree transition, skipping");
                continue;
            }
            
            // Re-add transition to graph (APE alignment)
            StateTransitionPtr unusedExisting;
            StateTransitionPtr unusedNew;
            _graph->addTransition(sourceState, action, targetState,
                                 sourceTree, targetTree,
                                 unusedExisting, unusedNew);
            reAddedCount++;
        }
        
        BLOG("> Readding transitions finished. Re-added %zu/%zu transitions.", 
             reAddedCount, removedTransitions.size());
        
        // Step 6: Rebuild history (APE alignment)
        _graph->rebuildHistory();
        
        BLOG("Rebuilding model finished, removed %zu states and %zu state transitions, re-added %zu transitions.", 
             statesToRemove.size(), removedTransitions.size(), reAddedCount);
        
        // Step 7: Restore Q-values after rebuild
        restoreQValuesAfterRebuild();
        
        return *this;
    }

    void Model::restoreQValuesAfterRebuild() {
        BLOG("Restoring Q-values after rebuild...");
        size_t restoredCount = 0;
        
        // Iterate through all agents
        for (const auto &agentPair : _deviceIDAgentMap) {
            auto agent = agentPair.second;
            if (!agent) {
                continue;
            }
            
            // Handle DoubleSarsaAgent
            if (auto doubleSarsaAgent = std::dynamic_pointer_cast<DoubleSarsaAgent>(agent)) {
                // Iterate through all states in graph
                for (const auto &state : _graph->getStates()) {
                    if (!state) {
                        continue;
                    }
                    
                    // Iterate through all actions in state
                    for (const auto &action : state->getActions()) {
                        if (!action) {
                            continue;
                        }
                        
                        // Get Q1 and Q2 values from agent's mappings
                        // Note: getQ1Value/getQ2Value already read from _reuseQValue1/_reuseQValue2 mappings
                        double q1Value = doubleSarsaAgent->getQ1Value(action);
                        double q2Value = doubleSarsaAgent->getQ2Value(action);
                        
                        // If either Q1 or Q2 has a non-zero value, restore them to Action object
                        if (q1Value != 0.0 || q2Value != 0.0) {
                            // Restore Q1 and Q2 values to mappings (setQ1Value/setQ2Value update both mapping and ensure consistency)
                            if (q1Value != 0.0) {
                                doubleSarsaAgent->setQ1Value(action, q1Value);
                            }
                            if (q2Value != 0.0) {
                                doubleSarsaAgent->setQ2Value(action, q2Value);
                            }
                            
                            // Set Action object's Q-value to average of Q1 and Q2 (or use available value)
                            // This provides a single Q-value for Action object while maintaining Q1/Q2 separation in agent
                            double avgQ = (q1Value != 0.0 && q2Value != 0.0) 
                                        ? (q1Value + q2Value) / 2.0 
                                        : (q1Value != 0.0 ? q1Value : q2Value);
                            action->setQValue(avgQ);
                            
                            restoredCount++;
                        }
                    }
                }
            }
        }
        
        BLOG("Q-values restoration finished. Restored Q-values for %zu actions.", restoredCount);
    }

}
#endif //Model_CPP_
