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
#include "../Base.h"
#include "../utils.hpp"
#include "../thirdpart/json/json.hpp"
#include "../llm/HttpLlmClient.h"
#include <algorithm>
#include <ctime>
#include <iostream>
#include <map>
#if DYNAMIC_STATE_ABSTRACTION_ENABLED
#include <utility>
#include <sstream>

namespace {
    /// Convert WidgetKeyMask to human-readable dimension list for logging (e.g. "Clazz|ResourceID|ContentDesc").
    std::string maskToDimensionString(fastbotx::WidgetKeyMask m) {
        std::ostringstream os;
        const char *sep = "";
        if (m & static_cast<fastbotx::WidgetKeyMask>(fastbotx::WidgetKeyAttr::Clazz)) { os << sep << "Clazz"; sep = "|"; }
        if (m & static_cast<fastbotx::WidgetKeyMask>(fastbotx::WidgetKeyAttr::ResourceID)) { os << sep << "ResourceID"; sep = "|"; }
        if (m & static_cast<fastbotx::WidgetKeyMask>(fastbotx::WidgetKeyAttr::OperateMask)) { os << sep << "OperateMask"; sep = "|"; }
        if (m & static_cast<fastbotx::WidgetKeyMask>(fastbotx::WidgetKeyAttr::ScrollType)) { os << sep << "ScrollType"; sep = "|"; }
        if (m & static_cast<fastbotx::WidgetKeyMask>(fastbotx::WidgetKeyAttr::Text)) { os << sep << "Text"; sep = "|"; }
        if (m & static_cast<fastbotx::WidgetKeyMask>(fastbotx::WidgetKeyAttr::ContentDesc)) { os << sep << "ContentDesc"; sep = "|"; }
        if (m & static_cast<fastbotx::WidgetKeyMask>(fastbotx::WidgetKeyAttr::Index)) { os << sep << "Index"; sep = "|"; }
        return os.str().empty() ? "(none)" : os.str();
    }
}
#endif

namespace fastbotx {

    WidgetKeyMask Model::getActivityKeyMask(const std::string &activity) const {
        auto it = _activityKeyMask.find(activity);
        if (it != _activityKeyMask.end()) {
            return it->second;
        }
        return DefaultWidgetKeyMask;
    }

    void Model::setActivityKeyMask(const std::string &activity, WidgetKeyMask mask) {
        _activityKeyMask[activity] = mask;
    }

    /**
     * @brief Log state information with each widget and action on a separate line
     * 
     * This helper function formats state information for debugging/logging purposes.
     * It prints the state hash, all widgets, and all actions in a readable format.
     * Long strings (>3000 chars) are split across multiple log lines.
     * 
     * @param state The state to log (nullptr is handled gracefully)
     */
    inline void logStatePerLine(const StatePtr &state) {
        if (state == nullptr) {
            BDLOGE("State is null, cannot log state information");
            return;
        }
        
        // Print state header with hash code
        BDLOG("{state: %lu", static_cast<unsigned long>(state->hash()));
        
        // Print each widget on a separate line for better readability; skip empty (e.g. toXPath returns "" when details cleared)
        BDLOG("widgets:");
        const auto &widgets = state->getWidgets();
        for (const auto &widget : widgets) {
            std::string widgetStr = widget->toString();
            if (widgetStr.empty()) continue;
            // If widget string is too long, split it across multiple log lines
            if (widgetStr.length() > 3000) {
                logLongStringInfo("   " + widgetStr);
            } else {
                BDLOG("   %s", widgetStr.c_str());
            }
        }
        
        // Print each action on a separate line for better readability
        BDLOG("action:");
        const auto &actions = state->getActions();
        for (const auto &action : actions) {
            std::string actionStr = action->toString();
            // If action string is too long, split it across multiple log lines
            if (actionStr.length() > 3000) {
                logLongStringInfo("   " + actionStr);
            } else {
                BDLOG("   %s", actionStr.c_str());
            }
        }
        
        BDLOG("}");
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
        BLOG("----Fastbot native build version: " FASTBOT_VERSION "----\n");
        this->_graph = std::make_shared<Graph>();
        this->_preference = Preference::inst();
        this->_netActionParam.netActionTaskid = 0;

        // Initialize AutodevAgent with HTTP LLM client if LLM is enabled in config.
        LlmRuntimeConfig llmCfg;
        if (this->_preference) {
            llmCfg = this->_preference->getLlmRuntimeConfig();
        }
        std::shared_ptr<LlmClient> client = nullptr;
        if (llmCfg.enabled) {
            client = std::make_shared<HttpLlmClient>(llmCfg);
            BLOG("AutodevAgent: HTTP LLM client initialized with model %s", llmCfg.model.c_str());
        } else {
            BLOG("AutodevAgent: LLM is disabled in config");
        }
        this->_autodevAgent = std::make_shared<AutodevAgent>(this->_preference, client);
#if DYNAMIC_STATE_ABSTRACTION_ENABLED
        this->_transitionLog.resize(MaxTransitionLogSize);
        BLOG("state abstraction: enabled (check interval=%d, batch every %d steps)",
             (int)RefinementCheckInterval, (int)RefinementCheckInterval);
#endif
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
        // Parse XML string into Element object using tinyxml2
        ElementPtr elem = Element::createFromXml(descContent);
        if (nullptr == elem) {
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
        OperatePtr operate = getOperateOpt(element, activity, deviceID);
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
        // Get the set of visited activities (returns by value, but set is typically small)
        const stringPtrSet& activityStringPtrSet = this->_graph->getVisitedActivities();
        
        // Create temporary shared_ptr for lookup
        // Note: This creates a temporary object for comparison only
        // If not found, we'll return this pointer; if found, we'll return the cached one
        stringPtr tempActivityPtr = std::make_shared<std::string>(activity);
        
        // Try to find existing activity pointer in the set
        auto founded = activityStringPtrSet.find(tempActivityPtr);
        
        if (founded == activityStringPtrSet.end()) {
            // This is a new activity, return the newly created pointer
            return tempActivityPtr;
        } else {
            // Activity already exists, return the cached pointer to avoid duplication
            return *founded;
        }
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
        if (nullptr == element) {
            return nullptr;
        }
        
        // Create state according to the agent's algorithm type
        // The state includes all possible actions based on widgets in the element
        std::string activityStr = activityPtr ? *activityPtr : "";
        WidgetKeyMask mask = getActivityKeyMask(activityStr);
        StatePtr state = StateFactory::createState(agent->getAlgorithmType(), activityPtr, element, mask);

#if DYNAMIC_STATE_ABSTRACTION_ENABLED
        // Update text stats from newly built state (before addState) for accurate "skip Text" check (§22)
        if (state && !activityStr.empty()) {
            const auto textMask = static_cast<WidgetKeyMask>(WidgetKeyAttr::Text);
            ActivityLastStateTextStats &st = _activityLastStateTextStats[activityStr];
            st.widgetsWithNonEmptyText = state->getWidgetsWithNonEmptyTextCount();
            st.totalWidgets = state->getWidgets().size();
            if ((mask & textMask) == 0) {
                st.uniqueWidgetsIfAddText = state->getUniqueWidgetCountUnderMask(mask | textMask);
            } else {
                st.uniqueWidgetsIfAddText = 0;
            }
        }
#endif

        // Add state to graph (may return existing state if duplicate)
        // The graph handles deduplication based on state hash
        state = this->_graph->addState(state);

        // Mark state as visited with current graph timestamp
        state->visit(this->_graph->getTimestamp());

        return state;
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
        if (nullptr == customAction && !shouldSkipActionsFromModel) {
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
                
                if (nullptr == action) {
                    BDLOGE("get null action!!!!");
                    return nullptr; // Handle null action gracefully
                }
            }
            
            // Calculate action generation time cost
            double endGeneratingActionTimestamp = currentStamp();
            actionCost = endGeneratingActionTimestamp - startGeneratingActionTimestamp;
            
            // Update agent state so block counter and DFS stack stay in sync (including after FUZZ)
            if (state) {
                if (action->isModelAct()) {
                    action->visit(this->_graph->getTimestamp());
                    agent->moveForward(state);
                } else if (action->getActionType() == ActionType::FUZZ) {
                    // FUZZ is not isModelAct(); still call moveForward so DFSAgent sees same/different state
                    agent->moveForward(state);
                }
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
        // Record method start time for performance tracking
        double methodStartTimestamp = currentStamp();
        
        // Step 0: Match LLM task on raw tree (before resolvePage) so checkpoint matches unmodified UI.
        LlmTaskConfigPtr preMatchedLlmTask = nullptr;
        if (this->_preference && element) {
            preMatchedLlmTask = this->_preference->matchLlmTask(activity, element);
        }
        
        // Step 1: Resolve page (black widgets, tree pruning, valid texts) before using element.
        if (this->_preference && element) {
            this->_preference->resolvePage(activity, element);
        }
        // Step 2: Custom action from max.xpath.actions (if any) for this activity and page.
        ActionPtr customAction = (this->_preference && element)
            ? this->_preference->getCustomActionFromXpath(activity, element)
            : nullptr;
        
        // Step 3: Get or create activity pointer (reuses existing pointers for memory efficiency)
        stringPtr activityPtr = getOrCreateActivityPtr(activity);
        
        // Step 4: Get or create agent for this device (creates default if needed)
        AbstractAgentPtr agent = getOrCreateAgent(deviceID);
        
        // Step 5: Create state from element and add to graph
        // The graph handles deduplication if a similar state already exists
        // currentStamp() returns ms; record build-state-only duration for log
        double buildStateStartTimestamp = currentStamp();
        StatePtr state = createAndAddState(element, agent, activityPtr);
        double buildStateEndTimestamp = currentStamp();
        bool fromLlm = (_autodevAgent && _autodevAgent->inSession());
#if DYNAMIC_STATE_ABSTRACTION_ENABLED
        if (!fromLlm) {
            recordTransition(agent, state);
            recordStateSplitIfRefined(activityPtr ? *activityPtr : "", state);
            if (state && state->getActivityString() &&
                state->getMaxWidgetsPerModelAction() > static_cast<size_t>(AlphaMaxGuiActionsPerModelAction)) {
                _activitiesNeedingAlphaRefinement.insert(activityPtr ? *activityPtr : "");
            }
        }
#endif
        // Step 5b: Removed — image now stays in Java (setLastScreenshotForLlm + doLlmHttpPostFromPrompt).
        // Native no longer returns NOP when screenshotBytes is empty; Java always has the image when needed.
        // Step 6: Optionally delegate to AutodevAgent before RL (pass pre-matched task from raw tree).
        ActionPtr llmAction = nullptr;
        if (this->_autodevAgent) {
            llmAction = this->_autodevAgent->selectNextAction(element, activity, deviceID, preMatchedLlmTask);
        }

        // Step 7: Select action (either LLM, custom, restart, or from agent)
        double actionCost = 0.0;
        ActionPtr action;
        if (llmAction) {
            // When AutodevAgent returns an action, we bypass RL for this step.
            action = llmAction;
        } else {
            action = selectAction(state, agent, customAction, actionCost);
        }
        
        // Handle null action gracefully
        if (nullptr == action) {
            return DeviceOperateWrapper::OperateNop;
        }
        
        // Step 8: Convert action to operation object and apply patches
        OperatePtr opt = convertActionToOperate(action, state);
        if (llmAction) {
            opt->allowFuzzing = false;
        }
        
        // Record end time and log performance metrics (currentStamp returns ms, keep ms for log)
        double methodEndTimestamp = currentStamp();
        double buildStateCostMs = buildStateEndTimestamp - buildStateStartTimestamp;
        double actionCostMs = actionCost;
        double totalCostMs = methodEndTimestamp - methodStartTimestamp;
#if DYNAMIC_STATE_ABSTRACTION_ENABLED
        BLOG("build state cost: %.3fms action cost: %.3fms total cost: %.3fms dims=[%s]",
             buildStateCostMs,
             actionCostMs,
             totalCostMs,
             maskToDimensionString(getActivityKeyMask(activity)).c_str());
#else
        BLOG("build state cost: %.3fms action cost: %.3fms total cost: %.3fms",
             buildStateCostMs,
             actionCostMs,
             totalCostMs);
#endif
#if DYNAMIC_STATE_ABSTRACTION_ENABLED
        if (!fromLlm) {
            _stepCountSinceLastCheck++;
            if (_stepCountSinceLastCheck >= RefinementCheckInterval) {
                runRefinementAndCoarseningIfScheduled();
                _stepCountSinceLastCheck = 0;
            }
        }
#endif
        return opt;
    }

#if DYNAMIC_STATE_ABSTRACTION_ENABLED
    void Model::recordTransition(const AbstractAgentPtr &agent, const StatePtr &targetState) {
        if (!agent || !targetState) return;
        StatePtr srcState = agent->getCurrentState();
        ActivityStateActionPtr act = agent->getCurrentAction();
        if (!srcState || !act || !act->isModelAct() || !act->requireTarget()) return;
        TransitionEntry e;
        e.sourceStateHash = srcState->hash();
        e.actionHash = act->hash();
        e.targetStateHash = targetState->hash();
        {
            auto actPtr = srcState->getActivityString();
            e.sourceActivity = (actPtr && actPtr.get()) ? *actPtr : "";
        }
        e.valid = true;
        if (_transitionLog.empty()) return;
        BDLOG("state abstraction: transition src=%lu act=%lu tgt=%lu activity=%s",
              (unsigned long)e.sourceStateHash, (unsigned long)e.actionHash, (unsigned long)e.targetStateHash,
              e.sourceActivity.c_str());
        _transitionLog[_transitionLogWriteIndex] = std::move(e);
        _transitionLogWriteIndex = (_transitionLogWriteIndex + 1) % _transitionLog.size();
    }

    void Model::recordStateSplitIfRefined(const std::string &activity, const StatePtr &state) {
        if (!state) return;
        auto it = _activityAbstractionContext.find(activity);
        if (it == _activityAbstractionContext.end()) return;
        ActivityAbstractionContext &ctx = it->second;
        WidgetKeyMask cur = getActivityKeyMask(activity);
        if (ctx.previousMask == cur) return;  // not refined yet or already coarsened
        uintptr_t oldHash = state->getHashUnderMask(ctx.previousMask);
        uintptr_t newHash = state->hash();
        ctx.oldStateToNewStates[oldHash].insert(newHash);
        BDLOG("state abstraction: split activity=%s oldHash=%lu newHash=%lu setSize=%zu",
              activity.c_str(), (unsigned long)oldHash, (unsigned long)newHash,
              ctx.oldStateToNewStates[oldHash].size());
    }

    std::vector<std::string> Model::detectNonDeterminism() const {
        using Key = std::pair<uintptr_t, uintptr_t>;
        std::map<Key, std::pair<std::unordered_set<uintptr_t>, std::string>> saToTargets;
        for (const auto &e : _transitionLog) {
            if (!e.valid) continue;
            if (e.sourceStateHash == e.targetStateHash) continue;
            Key k(e.sourceStateHash, e.actionHash);
            saToTargets[k].first.insert(e.targetStateHash);
            saToTargets[k].second = e.sourceActivity;
        }
        std::set<std::string> activitiesSet;
        for (const auto &p : saToTargets) {
            if (p.second.first.size() >= static_cast<size_t>(MinNonDeterminismCount)) {
                activitiesSet.insert(p.second.second);
            }
        }
        return std::vector<std::string>(activitiesSet.begin(), activitiesSet.end());
    }

    bool Model::refineActivity(const std::string &activity) {
        WidgetKeyMask cur = getActivityKeyMask(activity);
        const auto tMask = static_cast<WidgetKeyMask>(WidgetKeyAttr::Text);
        const auto cMask = static_cast<WidgetKeyMask>(WidgetKeyAttr::ContentDesc);
        const auto iMask = static_cast<WidgetKeyMask>(WidgetKeyAttr::Index);
        WidgetKeyMask newMask = cur;
        const char *addedAttr = nullptr;
        if ((cur & cMask) == 0) {
            newMask = cur | cMask;
            addedAttr = "ContentDesc";
        } else if ((cur & iMask) == 0) {
            newMask = cur | iMask;
            addedAttr = "Index";
        } else if ((cur & tMask) == 0) {
            newMask = cur | tMask;
            addedAttr = "Text";
            // Skip adding Text when text-heavy or would explode (see §11.5)
            auto it = _activityLastStateTextStats.find(activity);
            if (it != _activityLastStateTextStats.end()) {
                const ActivityLastStateTextStats &st = it->second;
                if (st.widgetsWithNonEmptyText > static_cast<size_t>(MaxTextWidgetCount)) {
                    BDLOG("state abstraction: skip refine activity=%s (+Text) reason=textCount>%d",
                          activity.c_str(), (int)MaxTextWidgetCount);
                    return false;
                }
                if (st.totalWidgets > 0 &&
                    st.widgetsWithNonEmptyText * 100 > static_cast<size_t>(MaxTextWidgetRatioPercent) * st.totalWidgets) {
                    BDLOG("state abstraction: skip refine activity=%s (+Text) reason=textRatio>%d%%",
                          activity.c_str(), (int)MaxTextWidgetRatioPercent);
                    return false;
                }
                if (st.uniqueWidgetsIfAddText > static_cast<size_t>(MaxUniqueWidgetsAfterText)) {
                    BDLOG("state abstraction: skip refine activity=%s (+Text) reason=uniqueAfterText>%d",
                          activity.c_str(), (int)MaxUniqueWidgetsAfterText);
                    return false;
                }
            }
        } else {
            BDLOG("state abstraction: skip refine activity=%s reason=already finest mask", activity.c_str());
            return false;
        }
        if (_coarseningBlacklist.count(std::make_pair(activity, newMask)) != 0) {
            BDLOG("state abstraction: skip refine activity=%s newMask=%u reason=blacklisted", activity.c_str(), (unsigned)newMask);
            return false;
        }
        ActivityAbstractionContext &ctx = _activityAbstractionContext[activity];
        ctx.previousMask = cur;
        ctx.stateCountAtLastRefinement = getGraph()->getStateCountByActivity(activity);
        ctx.oldStateToNewStates.clear();
        setActivityKeyMask(activity, newMask);
        BLOG("state abstraction: refine activity=%s mask %u->%u (+%s) stateCount=%zu dims=[%s]->[%s]",
             activity.c_str(), (unsigned)cur, (unsigned)newMask, addedAttr, ctx.stateCountAtLastRefinement,
             maskToDimensionString(cur).c_str(), maskToDimensionString(newMask).c_str());
        return true;
    }

    void Model::coarsenActivityIfNeeded(const std::string &activity) {
        auto it = _activityAbstractionContext.find(activity);
        if (it == _activityAbstractionContext.end()) return;
        // APE coarsening: if any old state L′ splits into > β new states, roll back
        for (const auto &p : it->second.oldStateToNewStates) {
            if (p.second.size() > static_cast<size_t>(BetaMaxSplitCount)) {
                WidgetKeyMask cur = getActivityKeyMask(activity);
                WidgetKeyMask prev = it->second.previousMask;
                setActivityKeyMask(activity, prev);
                _coarseningBlacklist.insert(std::make_pair(activity, cur));
                it->second.oldStateToNewStates.clear();
                it->second.stateCountAtLastRefinement = getGraph()->getStateCountByActivity(activity);
                BLOG("state abstraction: coarsen activity=%s mask %u->%u (split %zu>%d) dims=[%s]->[%s]",
                     activity.c_str(), (unsigned)cur, (unsigned)prev, p.second.size(), (int)BetaMaxSplitCount,
                     maskToDimensionString(cur).c_str(), maskToDimensionString(prev).c_str());
                return;
            }
        }
        return;
    }

    void Model::runRefinementAndCoarseningIfScheduled() {
        if (_stepCountSinceLastCheck < static_cast<size_t>(RefinementCheckInterval)) return;
        BLOG("state abstraction: batch at step %zu (interval=%d)", _stepCountSinceLastCheck, (int)RefinementCheckInterval);
        // Coarsen check for activities refined in a previous batch (oldStateToNewStates accumulated over last K steps)
        for (const auto &kv : _activityAbstractionContext) {
            const std::string &activity = kv.first;
            const ActivityAbstractionContext &ctx = kv.second;
            if (ctx.previousMask != getActivityKeyMask(activity)) {
                coarsenActivityIfNeeded(activity);
            }
        }
        if (UsePaperRefinementOrder) {
            // Paper order: (1) ActionRefinement(α) + Coarsening(β), (2) StateRefinement (non-determinism) + Coarsening(β)
            std::vector<std::string> activitiesAlpha(_activitiesNeedingAlphaRefinement.begin(),
                                                     _activitiesNeedingAlphaRefinement.end());
            _activitiesNeedingAlphaRefinement.clear();
            BLOG("state abstraction: paper order alpha=%zu", activitiesAlpha.size());
            for (const auto &activity : activitiesAlpha) {
                if (refineActivity(activity)) {
                    coarsenActivityIfNeeded(activity);
                }
            }
            std::vector<std::string> activitiesNonDet = detectNonDeterminism();
            BLOG("state abstraction: paper order nonDet=%zu", activitiesNonDet.size());
            for (const auto &activity : activitiesNonDet) {
                if (refineActivity(activity)) {
                    coarsenActivityIfNeeded(activity);
                }
            }
        } else {
            // Per-K-step batch: merge α + non-determinism, refine all, then coarsen all refined
            std::vector<std::string> activitiesToRefine = detectNonDeterminism();
            size_t nonDetCount = activitiesToRefine.size();
            size_t alphaCount = _activitiesNeedingAlphaRefinement.size();
            for (const auto &a : _activitiesNeedingAlphaRefinement) {
                if (std::find(activitiesToRefine.begin(), activitiesToRefine.end(), a) == activitiesToRefine.end()) {
                    activitiesToRefine.push_back(a);
                }
            }
            _activitiesNeedingAlphaRefinement.clear();
            BLOG("state abstraction: batch nonDet=%zu alpha=%zu toRefine=%zu",
                 nonDetCount, alphaCount, activitiesToRefine.size());
            std::vector<std::string> activitiesJustRefined;
            for (const auto &activity : activitiesToRefine) {
                if (refineActivity(activity)) {
                    activitiesJustRefined.push_back(activity);
                }
            }
            for (const auto &activity : activitiesJustRefined) {
                coarsenActivityIfNeeded(activity);
            }
            if (!activitiesJustRefined.empty()) {
                BLOG("state abstraction: batch refined=%zu coarsenChecked=%zu",
                     activitiesJustRefined.size(), activitiesJustRefined.size());
            } else {
                BLOG("state abstraction: batch done refined=0 (all already finest or skipped)");
            }
        }
    }
#endif

    void Model::reportActivity(const std::string &activity) {
        if (activity.empty()) return;
        std::lock_guard<std::mutex> lock(_coverageMutex);
        _visitedActivities.insert(activity);
        _coverageStepCount++;
    }

    std::string Model::getCoverageJson() const {
        std::lock_guard<std::mutex> lock(_coverageMutex);
        nlohmann::json j;
        j["stepsCount"] = _coverageStepCount;
        nlohmann::json arr = nlohmann::json::array();
        for (const auto &a : _visitedActivities) {
            arr.push_back(a);
        }
        j["testedActivities"] = arr;
        return j.dump();
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

}  // namespace fastbotx

#endif  // Model_CPP_