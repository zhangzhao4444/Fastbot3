/**
 * @authors Zhao Zhang
 */
 
#ifndef fastbotx_DoubleSarsaAgent_CPP_
#define fastbotx_DoubleSarsaAgent_CPP_

#include "DoubleSarsaAgent.h"
#include "Model.h"
#include <cmath>
#include "ActivityNameAction.h"
#include "ModelStorageConstants.h"
#include "../storage/ReuseModel_generated.h"
#include <iostream>
#include <fstream>
#include <limits>
#include <memory>
#include <mutex>
#include <utility>
#include <algorithm>
#include <thread>
#include <chrono>
#include <cstdio>
#include <cinttypes>

namespace fastbotx {

    /// Static default model save path
    std::string DoubleSarsaAgent::DefaultModelSavePath = "/sdcard/fastbot.model.fbm";

    /**
     * @brief Constructor
     * 
     * Initializes Double SARSA parameters and reuse model related variables.
     * 
     * @param model Model pointer for accessing state graph info
     */
    DoubleSarsaAgent::DoubleSarsaAgent(const ModelPtr &model)
            : AbstractAgent(model), 
              _alpha(DoubleSarsaRLConstants::DefaultAlpha),  // Initial learning rate 0.25
              _epsilon(DoubleSarsaRLConstants::DefaultEpsilon),  // Initial exploration rate 0.05
              _rng(std::random_device{}()),  // Initialize random number generator with random device
              _modelSavePath(DefaultModelSavePath),  // Set model save path
              _defaultModelSavePath(DefaultModelSavePath) {  // Set default save path
        this->_algorithmType = AlgorithmType::DoubleSarsa;  // Set algorithm type to DoubleSarsa
        BLOG("Double SARSA: Agent initialized with alpha=%.4f, epsilon=%.4f, gamma=%.4f, NStep=%d", 
             DoubleSarsaRLConstants::DefaultAlpha, 
             DoubleSarsaRLConstants::DefaultEpsilon,
             DoubleSarsaRLConstants::DefaultGamma,
             DoubleSarsaRLConstants::NStep);
    }

    /**
     * @brief Destructor
     * 
     * Saves reuse model and cleans up resources.
     */
    DoubleSarsaAgent::~DoubleSarsaAgent() {
        BLOG("Double SARSA: Destructor called, saving model (reuse entries=%zu, Q1 entries=%zu, Q2 entries=%zu)", 
             this->_reuseModel.size(), this->_reuseQValue1.size(), this->_reuseQValue2.size());
        this->saveReuseModel(this->_modelSavePath);
        this->_reuseModel.clear();
        this->_reuseQValue1.clear();
        this->_reuseQValue2.clear();
        BLOG("Double SARSA: Agent destructed, all resources cleaned up");
    }

    /**
     * @brief Compute alpha value
     * 
     * Dynamically adjusts learning rate alpha based on current state graph's visit count.
     * More visits result in smaller learning rate (learning rate decay strategy).
     */
    void DoubleSarsaAgent::computeAlphaValue() {
        if (nullptr != this->_newState) {
            // Get model pointer (use weak_ptr to avoid circular references)
            auto modelPtr = this->_model.lock();
            if (!modelPtr) {
                BLOGE("Double SARSA: Model has been destroyed, cannot compute alpha value");
                return;
            }
            
            // Get state graph and read total visit count
            const GraphPtr &graphRef = modelPtr->getGraph();
            long totalVisitCount = graphRef->getTotalDistri();
            
            // Calculate alpha value based on visit count
            this->_alpha = calculateAlphaByVisitCount(totalVisitCount);
        }
    }
    
    /**
     * @brief Calculate alpha value based on visit count
     * 
     * Implements learning rate decay strategy: learning rate gradually decreases as visit count increases.
     * 
     * Decay rules (from high to low):
     * - visitCount > 250000: alpha = 0.1
     * - visitCount > 100000: alpha = 0.2
     * - visitCount > 50000:  alpha = 0.3
     * - visitCount > 20000: alpha = 0.4
     * - Otherwise: alpha = 0.5
     * 
     * Minimum alpha value is DefaultAlpha (0.25), ensures it doesn't decay to 0.
     */
    double DoubleSarsaAgent::calculateAlphaByVisitCount(long visitCount) const {
        using namespace DoubleSarsaRLConstants;
        
        // Use static lookup table for better maintainability and performance
        static const std::vector<std::pair<long, double>> alphaThresholds = {
            {AlphaThreshold4, AlphaDecrement},  // 250000 -> -0.1
            {AlphaThreshold3, AlphaDecrement},  // 100000 -> -0.1
            {AlphaThreshold2, AlphaDecrement},  // 50000  -> -0.1
            {AlphaThreshold1, AlphaDecrement}  // 20000  -> -0.1
        };
        
        // Start from initial value
        double movingAlpha = InitialMovingAlpha;  // 0.5
        
        // Decrement alpha based on visit count
        for (const auto& pair : alphaThresholds) {
            long threshold = pair.first;
            double decrement = pair.second;
            if (visitCount > threshold) {
                movingAlpha -= decrement;
            }
        }
        
        // Ensure alpha is not less than minimum (DefaultAlpha = 0.25)
        return std::max(DefaultAlpha, movingAlpha);
    }

    /**
     * @brief Compute reward value of latest executed action
     * 
     * Computes reward for the most recently executed action (_previousActions.back()).
     * The reward is based on the probability that the action can reach unvisited activities
     * and the expectation value of the resulting state.
     * 
     * Reward calculation formula:
     * reward = [probabilityOfVisitingNewActivities / sqrt(visitedCount + 1)]
     *        + [getStateActionExpectationValue / sqrt(stateVisitedCount + 1)]
     * 
     * @return Reward value for the latest executed action
     */
    double DoubleSarsaAgent::computeRewardOfLatestAction() {
        double rewardValue = 0.0;
        
        if (nullptr != this->_newState) {
            // Update alpha value (dynamically adjusted based on visit count)
            this->computeAlphaValue();
            
            // Get model pointer
            auto modelPtr = this->_model.lock();
            if (!modelPtr) {
                BLOGE("Double SARSA: Model has been destroyed, cannot compute reward");
                return rewardValue;
            }
            
            // Get set of visited activities (after reaching _newState)
            const GraphPtr &graphRef = modelPtr->getGraph();
            auto visitedActivities = graphRef->getVisitedActivities();
            
            // Get latest executed action (last in action history)
            if (auto lastSelectedAction = std::dynamic_pointer_cast<ActivityStateAction>(
                    this->_previousActions.back())) {
                
                // Compute probability that action can reach unvisited activities
                double probValue = this->probabilityOfVisitingNewActivities(lastSelectedAction,
                                                                       visitedActivities);
                rewardValue = probValue;
                
                BDLOG("Double SARSA: Reward computation - action=%s, probOfNewActivities=%.4f, visitedCount=%d", 
                      lastSelectedAction->toString().c_str(), probValue, lastSelectedAction->getVisitedCount());
                
                // If action not in reuse model (new action), directly give reward
                if (std::abs(rewardValue - 0.0) < DoubleSarsaRLConstants::RewardEpsilon) {
                    rewardValue = DoubleSarsaRLConstants::NewActionReward;
                    BDLOG("Double SARSA: Action not in reuse model, using NewActionReward=%.4f", rewardValue);
                }
                
                // Normalize: divide by square root of visit count
                double normalizedReward = rewardValue / sqrt(lastSelectedAction->getVisitedCount() + 1.0);
                BDLOG("Double SARSA: Normalized reward (action): %.4f / sqrt(%d+1) = %.4f", 
                      rewardValue, lastSelectedAction->getVisitedCount(), normalizedReward);
                rewardValue = normalizedReward;
            }
            
            // Add state expectation value (normalized)
            double stateExpectation = this->getStateActionExpectationValue(this->_newState,
                                                                              visitedActivities);
            double stateVisitedCount = this->_newState->getVisitedCount();
            double normalizedStateValue = stateExpectation / sqrt(stateVisitedCount + 1.0);
            double rewardBeforeState = rewardValue;
            rewardValue = rewardValue + normalizedStateValue;
            
            BDLOG("Double SARSA: State expectation=%.4f, stateVisitedCount=%.0f, normalized=%.4f, total reward: %.4f + %.4f = %.4f", 
                  stateExpectation, stateVisitedCount, normalizedStateValue, rewardBeforeState, normalizedStateValue, rewardValue);
            
            BLOG("Double SARSA: total visited " ACTIVITY_VC_STR " count is %zu", visitedActivities.size());
        }
        
        BDLOG("Double SARSA: Final computed reward=%.4f", rewardValue);
        
        // Add reward value to cache
        this->_rewardCache.emplace_back(rewardValue);
        
        // Ensure cache size doesn't exceed NStep
        if (this->_rewardCache.size() > DoubleSarsaRLConstants::NStep) {
            this->_rewardCache.erase(this->_rewardCache.begin());
        }
        
        return rewardValue;
    }

    /**
     * @brief Get action's Q1-value
     * 
     * @param action Action pointer
     * @return Q1-value
     */
    double DoubleSarsaAgent::getQ1Value(const ActionPtr &action) {
        uintptr_t actionHash = action->hash();
        std::lock_guard<std::mutex> reuseGuard(this->_reuseModelLock);
        auto it = this->_reuseQValue1.find(actionHash);
        if (it != this->_reuseQValue1.end()) {
            return it->second;
        }
        return 0.0;  // Default Q-value
    }
    
    /**
     * @brief Get action's Q2-value
     * 
     * @param action Action pointer
     * @return Q2-value
     */
    double DoubleSarsaAgent::getQ2Value(const ActionPtr &action) {
        uintptr_t actionHash = action->hash();
        std::lock_guard<std::mutex> reuseGuard(this->_reuseModelLock);
        auto it = this->_reuseQValue2.find(actionHash);
        if (it != this->_reuseQValue2.end()) {
            return it->second;
        }
        return 0.0;  // Default Q-value
    }
    
    /**
     * @brief Get action's Q-value (from randomly chosen Q1 or Q2)
     * 
     * Randomly chooses Q1 or Q2 and returns the Q-value.
     * Used for action selection.
     * 
     * @param action Action pointer
     * @return Q-value from randomly chosen Q-function
     */
    double DoubleSarsaAgent::getQValue(const ActionPtr &action) {
        // Randomly choose Q1 or Q2
        int choice = _uniformIntDist(_rng);  // 0 or 1
        if (choice == 0) {
            return getQ1Value(action);
        } else {
            return getQ2Value(action);
        }
    }

    /**
     * @brief Set action's Q1-value
     * 
     * @param action Action pointer
     * @param qValue Q1-value
     */
    void DoubleSarsaAgent::setQ1Value(const ActionPtr &action, double qValue) {
        uintptr_t actionHash = action->hash();
        std::lock_guard<std::mutex> reuseGuard(this->_reuseModelLock);
        this->_reuseQValue1[actionHash] = qValue;
    }
    
    /**
     * @brief Set action's Q2-value
     * 
     * @param action Action pointer
     * @param qValue Q2-value
     */
    void DoubleSarsaAgent::setQ2Value(const ActionPtr &action, double qValue) {
        uintptr_t actionHash = action->hash();
        std::lock_guard<std::mutex> reuseGuard(this->_reuseModelLock);
        this->_reuseQValue2[actionHash] = qValue;
    }

    /**
     * @brief Update Q-values using N-step Double SARSA algorithm
     * 
     * Implements N-step Double SARSA algorithm following the standard Double SARSA principle:
     * - For EACH action in the window, independently randomly chooses to update Q1 or Q2
     * - Uses the OTHER Q-function for bootstrapping (Double SARSA key idea)
     * - Updates all actions in the N-step window
     * 
     * Standard Double SARSA update rule (for each action):
     * - Randomly choose Q1 or Q2 to update (independent for each action)
     * - If updating Q1: n-step return uses Q2 for bootstrapping
     * - If updating Q2: n-step return uses Q1 for bootstrapping
     * 
     * N-step return formula for action i:
     *   G_i^(n) = R_i + γR_{i+1} + ... + γ^(k-1)R_{i+k-1} + γ^k * Q_other(s_{i+k}, a_{i+k})
     *   where k is the number of steps from i+1 to the end of window
     *   and Q_other is the OTHER Q-function (Q2 if updating Q1, Q1 if updating Q2)
     * 
     * Key difference from standard SARSA:
     * - Uses the OTHER Q-function for bootstrapping to reduce overestimation bias
     * - Each action's update independently randomly selects Q1 or Q2
     * - This ensures Q1 and Q2 are updated in a balanced manner
     */
    void DoubleSarsaAgent::updateQValues() {
        using namespace DoubleSarsaRLConstants;
        
        // Validate array sizes match
        if (this->_previousActions.empty()) {
            BDLOG("Double SARSA: updateQValues called but action history is empty");
            return;
        }
        
        // Check if reward cache has enough entries
        if (this->_rewardCache.size() < this->_previousActions.size()) {
            BLOGE("Double SARSA: Reward cache size (%zu) is smaller than action history size (%zu)", 
                  this->_rewardCache.size(), this->_previousActions.size());
            return;
        }
        
        // Get window size
        int windowSize = static_cast<int>(this->_previousActions.size());
        BDLOG("Double SARSA: ===== Starting Q-value update =====");
        BDLOG("Double SARSA: Window size=%d, alpha=%.4f, gamma=%.4f, epsilon=%.4f", 
              windowSize, this->_alpha, DefaultGamma, this->_epsilon);
        
        // Log action history and rewards for debugging
        BDLOG("Double SARSA: Action history (from oldest to newest):");
        for (int idx = 0; idx < windowSize; idx++) {
            uintptr_t actionHash = this->_previousActions[idx]->hash();
            double reward = (idx < static_cast<int>(this->_rewardCache.size())) ? this->_rewardCache[idx] : 0.0;
            BDLOG("Double SARSA:   [%d] action_hash=0x%" PRIxPTR ", reward=%.4f", idx, actionHash, reward);
        }
        
        // Log _newAction (used for bootstrapping)
        if (_newAction != nullptr) {
            uintptr_t newActionHash = _newAction->hash();
            double q1New = getQ1Value(_newAction);
            double q2New = getQ2Value(_newAction);
            BDLOG("Double SARSA: Bootstrap action (_newAction): hash=0x%" PRIxPTR ", Q1=%.4f, Q2=%.4f", 
                  newActionHash, q1New, q2New);
        }
        
        // Counters for Q1 and Q2 updates
        int q1UpdateCount = 0;
        int q2UpdateCount = 0;
        
        // Traverse action history from back to front (from latest to oldest)
        // For EACH action, independently:
        //   1. Randomly choose to update Q1 or Q2
        //   2. Compute n-step return using the OTHER Q-function for bootstrapping
        //   3. Update the chosen Q-function
        // 
        // Key: Each action independently selects Q1 or Q2, ensuring balanced updates
        for (int i = windowSize - 1; i >= 0; i--) {
            // Safety check: ensure index is valid
            if (i >= static_cast<int>(this->_rewardCache.size())) {
                BLOGE("Double SARSA: Index %d out of bounds for reward cache (size %zu)", i, this->_rewardCache.size());
                break;
            }
            
            // For each action, independently randomly choose which Q-function to update
            // This is the core of Double SARSA: each (s,a) pair independently selects Q1 or Q2
            // This ensures Q1 and Q2 are updated in a balanced manner over time
            int updateQ1 = _uniformIntDist(_rng);  // 0 = update Q1, 1 = update Q2
            
            // Get action hash for logging
            uintptr_t actionHash = this->_previousActions[i]->hash();
            
            // Get current Q1 and Q2 values for this action (before update)
            double currentQ1Value = getQ1Value(this->_previousActions[i]);
            double currentQ2Value = getQ2Value(this->_previousActions[i]);
            
            // Compute n-step return for this action
            // Start from _newAction's Q-value using the OTHER Q-function for bootstrapping
            // This represents Q(s_{i+k}, a_{i+k}) in the n-step return formula
            // where k is the number of steps from i+1 to the end of window
            // 
            // We need to compute: R_i + γR_{i+1} + ... + γ^(k-1)R_{i+k-1} + γ^k * Q_other(s_{i+k}, a_{i+k})
            // Standard N-step: accumulate from back to front
            // But since each action uses a different Q-function for bootstrapping, we compute independently
            double nStepReturn;
            double bootstrapQValue;
            if (updateQ1 == 0) {
                // Updating Q1, use Q2 for bootstrapping (Double SARSA key idea)
                bootstrapQValue = getQ2Value(_newAction);
                nStepReturn = bootstrapQValue;
                q1UpdateCount++;
            } else {
                // Updating Q2, use Q1 for bootstrapping (Double SARSA key idea)
                bootstrapQValue = getQ1Value(_newAction);
                nStepReturn = bootstrapQValue;
                q2UpdateCount++;
            }
            
            BDLOG("Double SARSA: Action[%d] hash=0x%" PRIxPTR ": updating %s (Q1=%.4f, Q2=%.4f), bootstrap using Q_%s=%.4f", 
                  i, actionHash, (updateQ1 == 0 ? "Q1" : "Q2"), currentQ1Value, currentQ2Value, 
                  (updateQ1 == 0 ? "2" : "1"), bootstrapQValue);
            
            // Accumulate n-step return from action i to end of window
            // Build: R_i + γR_{i+1} + ... + γ^(k-1)R_{i+k-1} + γ^k * Q_other(s_{i+k}, a_{i+k})
            // We traverse from the end (windowSize-1) back to i, accumulating rewards
            // This matches the standard N-step SARSA accumulation pattern
            // For action i, we need rewards from i to windowSize-1, then bootstrapping value
            for (int j = windowSize - 1; j >= i; j--) {
                if (j < static_cast<int>(this->_rewardCache.size())) {
                    // Accumulate: R_j + γ * (future return)
                    // At each step j (from back to front), we add reward R_j and multiply by gamma
                    // This builds: R_j + γ * (R_{j+1} + γ * (R_{j+2} + ... + γ^k * Q_other))
                    double prevReturn = nStepReturn;
                    nStepReturn = this->_rewardCache[j] + DefaultGamma * nStepReturn;
                    BDLOG("Double SARSA: Action[%d] step[%d] reward=%.4f, return: %.4f -> %.4f", 
                          i, j, this->_rewardCache[j], prevReturn, nStepReturn);
                }
            }
            
            // Get current Q-value (from the Q-function being updated)
            double currentQValue = (updateQ1 == 0) ? currentQ1Value : currentQ2Value;
            
            // Update Q-value for this action using n-step return
            // Update the chosen Q-function (Q1 or Q2)
            // The bootstrapping used the OTHER Q-function (Double SARSA key idea)
            // This reduces overestimation bias by decorrelating action selection from value estimation
            double qUpdate = this->_alpha * (nStepReturn - currentQValue);
            double newQValue = currentQValue + qUpdate;
            if (updateQ1 == 0) {
                setQ1Value(this->_previousActions[i], newQValue);
            } else {
                setQ2Value(this->_previousActions[i], newQValue);
            }
            
            // Get updated Q values for logging
            double updatedQ1Value = getQ1Value(this->_previousActions[i]);
            double updatedQ2Value = getQ2Value(this->_previousActions[i]);
            
            BDLOG("Double SARSA: Action[%d] hash=0x%" PRIxPTR " %s updated: Q_old=%.4f, nStepReturn=%.4f, alpha=%.4f, delta=%.4f, Q_new=%.4f", 
                  i, actionHash, (updateQ1 == 0 ? "Q1" : "Q2"), currentQValue, nStepReturn, this->_alpha, qUpdate, newQValue);
            BDLOG("Double SARSA: Action[%d] hash=0x%" PRIxPTR " after update: Q1=%.4f, Q2=%.4f", 
                  i, actionHash, updatedQ1Value, updatedQ2Value);
        }
        
        // Log update statistics
        BDLOG("Double SARSA: ===== Q-value update completed =====");
        BDLOG("Double SARSA: Update statistics: Q1 updates=%d, Q2 updates=%d, total=%d", 
              q1UpdateCount, q2UpdateCount, q1UpdateCount + q2UpdateCount);
        BDLOG("Double SARSA: Q1 update ratio=%.2f%%, Q2 update ratio=%.2f%%", 
              (windowSize > 0 ? 100.0 * q1UpdateCount / windowSize : 0.0),
              (windowSize > 0 ? 100.0 * q2UpdateCount / windowSize : 0.0));
    }

    /**
     * @brief Update strategy
     * 
     * Implements AbstractAgent's pure virtual function.
     * Called after action execution, performs the following operations:
     * 1. Compute reward value of latest executed action
     * 2. Update reuse model (record action-to-activity mapping)
     * 3. Update Q-values using Double SARSA algorithm
     * 4. Add new action to action history cache
     */
    void DoubleSarsaAgent::updateStrategy() {
        // If no new action, return directly
        if (nullptr == this->_newAction) {
            BDLOG("Double SARSA: updateStrategy called but _newAction is null");
            return;
        }
        
        BDLOG("Double SARSA: updateStrategy called, action history size=%zu, reward cache size=%zu", 
              this->_previousActions.size(), this->_rewardCache.size());
        
        // If action history is not empty, execute update operations
        if (!this->_previousActions.empty()) {
            // 1. Compute reward value of latest executed action
            BDLOG("Double SARSA: Step 1 - Computing reward for latest action");
            this->computeRewardOfLatestAction();
            
            // 2. Update reuse model
            BDLOG("Double SARSA: Step 2 - Updating reuse model");
            this->updateReuseModel();
            
            // 3. Update Q-values using Double SARSA algorithm
            BDLOG("Double SARSA: Step 3 - Updating Q-values using N-step Double SARSA");
            this->updateQValues();
        } else {
            BDLOG("Double SARSA: Action history is empty, skipping Q-value update");
        }
        
        // 4. Add new action to end of action history cache
        this->_previousActions.emplace_back(this->_newAction);
        BDLOG("Double SARSA: Added new action to history, history size=%zu", this->_previousActions.size());
        
        // Ensure cache size doesn't exceed NStep
        if (this->_previousActions.size() > DoubleSarsaRLConstants::NStep) {
            BDLOG("Double SARSA: Action history exceeds NStep=%d, removing oldest action", 
                  DoubleSarsaRLConstants::NStep);
            this->_previousActions.erase(this->_previousActions.begin());
        }
    }

    /**
     * @brief Compute probability that action can reach unvisited activities
     * 
     * Based on reuse model, computes the ratio of unvisited activity visit counts
     * to total visit counts among activities that this action can reach.
     */
    double DoubleSarsaAgent::probabilityOfVisitingNewActivities(const ActivityStateActionPtr &action,
                                                                 const stringPtrSet &visitedActivities) const {
        double value = 0.0;
        int total = 0;
        int unvisited = 0;
        
        // Lock to protect concurrent access to reuse model
        std::lock_guard<std::mutex> reuseGuard(this->_reuseModelLock);
        
        // Find action in reuse model (by hash value)
        auto actionMapIterator = this->_reuseModel.find(action->hash());
        
        if (actionMapIterator != this->_reuseModel.end()) {
            // Iterate through all activities this action can reach and their visit counts
            for (const auto &activityCountMapIterator: actionMapIterator->second) {
                total += activityCountMapIterator.second;
                stringPtr activity = activityCountMapIterator.first;
                
                // Check if this activity is unvisited
                if (visitedActivities.count(activity) == 0) {
                    unvisited += activityCountMapIterator.second;
                }
            }
            
            // Calculate probability: unvisited activity visit counts / total visit counts
            if (total > 0 && unvisited > 0) {
                value = static_cast<double>(unvisited) / total;
            }
        }
        
        return value;
    }

    /**
     * @brief Check if action is in reuse model
     */
    bool DoubleSarsaAgent::isActionInReuseModel(uintptr_t actionHash) const {
        std::lock_guard<std::mutex> reuseGuard(this->_reuseModelLock);
        return this->_reuseModel.count(actionHash) > 0;
    }

    /**
     * @brief Compute state action expectation value
     * 
     * Evaluates expected value of reaching unvisited activities after executing actions from this state.
     */
    double DoubleSarsaAgent::getStateActionExpectationValue(const StatePtr &state,
                                                            const stringPtrSet &visitedActivities) const {
        double value = 0.0;
        
        // Iterate through all actions in state
        for (const auto &action: state->getActions()) {
            uintptr_t actionHash = action->hash();
            
            // If action not in reuse model (new action), add reward
            if (!this->isActionInReuseModel(actionHash)) {
                value += DoubleSarsaRLConstants::NewActionInStateReward;  // +1.0
            }
            // If action already visited (executed in current test), add smaller reward
            else if (action->getVisitedCount() >= 1) {
                value += DoubleSarsaRLConstants::VisitedActionReward;  // +0.5
            }

            // For target actions, add probability of reaching unvisited activities
            if (action->getTarget() != nullptr) {
                value += probabilityOfVisitingNewActivities(action, visitedActivities);
            }
        }
        
        return value;
    }

    /**
     * @brief Update reuse model
     * 
     * Records latest executed action and the activity it reached.
     */
    void DoubleSarsaAgent::updateReuseModel() {
        // If action history is empty, return directly
        if (this->_previousActions.empty()) {
            return;
        }
        
        // Get latest executed action
        ActionPtr lastAction = this->_previousActions.back();
        
        // Only process ActivityNameAction type actions
        if (auto modelAction = std::dynamic_pointer_cast<ActivityNameAction>(lastAction)) {
            // If new state is null, cannot get activity info
            if (nullptr == this->_newState) {
                return;
            }
            
            // Get action hash and reached activity
            auto hash = static_cast<uint64_t>(modelAction->hash());
            stringPtr activity = this->_newState->getActivityString();
            
            if (activity == nullptr) {
                return;
            }
            
            // Lock to protect concurrent access to reuse model
            {
                std::lock_guard<std::mutex> reuseGuard(this->_reuseModelLock);
                
                // Find action in reuse model
                auto iter = this->_reuseModel.find(hash);
                
                if (iter == this->_reuseModel.end()) {
                    // Action not in reuse model, create new entry
                    BDLOG("Double SARSA: Adding new action %s (hash=%" PRIu64 ") to reuse model, activity=%s", 
                          modelAction->getId().c_str(), hash, activity->c_str());
                    ReuseEntryM entryMap;
                    entryMap.emplace(activity, 1);
                    this->_reuseModel[hash] = entryMap;
                } else {
                    // Action in reuse model, increment this activity's visit count
                    int oldCount = iter->second[activity];
                    iter->second[activity] += 1;
                    BDLOG("Double SARSA: Updating reuse model - action %s (hash=%" PRIu64 "), activity=%s, count: %d -> %d", 
                          modelAction->getId().c_str(), hash, activity->c_str(), oldCount, iter->second[activity]);
                }
            }
        }
    }

    /**
     * @brief Select action using epsilon-greedy strategy with Double SARSA
     * 
     * With (1-epsilon) probability selects action with highest Q-value from randomly chosen Q1 or Q2,
     * with epsilon probability randomly selects action (exploration).
     */
    ActivityStateActionPtr DoubleSarsaAgent::selectNewActionEpsilonGreedyRandomly() const {
        if (this->eGreedy()) {
            // Use greedy strategy: select action with highest Q-value from randomly chosen Q1 or Q2
            BDLOG("Double SARSA: Try to select the max value action");
            
            // Randomly choose Q1 or Q2
            int choice = _uniformIntDist(_rng);  // 0 or 1
            BDLOG("Double SARSA: Epsilon-greedy greedy selection using %s", (choice == 0 ? "Q1" : "Q2"));
            
            // Find action with maximum Q-value from chosen Q-function
            ActivityStateActionPtr bestAction = nullptr;
            double maxQ = -std::numeric_limits<double>::max();
            
            // Create non-const pointer for calling non-const methods
            DoubleSarsaAgent* nonConstThis = const_cast<DoubleSarsaAgent*>(this);
            
            int actionCount = 0;
            for (const auto &action : this->_newState->getActions()) {
                if (!action->isValid()) {
                    continue;
                }
                
                // Get both Q1 and Q2 values for comparison
                double q1Value = nonConstThis->getQ1Value(action);
                double q2Value = nonConstThis->getQ2Value(action);
                double qValue = (choice == 0) ? q1Value : q2Value;
                
                actionCount++;
                uintptr_t actionHash = action->hash();
                BDLOG("Double SARSA: Action[%d] hash=0x%" PRIxPTR " %s: Q1=%.4f, Q2=%.4f, using %s=%.4f (current max=%.4f)", 
                      actionCount, actionHash, action->toString().c_str(), q1Value, q2Value, 
                      (choice == 0 ? "Q1" : "Q2"), qValue, maxQ);
                
                if (qValue > maxQ) {
                    maxQ = qValue;
                    bestAction = std::dynamic_pointer_cast<ActivityStateAction>(action);
                    BDLOG("Double SARSA: New best action selected: hash=0x%" PRIxPTR " %s with %s=%.4f", 
                          actionHash, action->toString().c_str(), (choice == 0 ? "Q1" : "Q2"), qValue);
                }
            }
            
            if (bestAction != nullptr) {
                uintptr_t bestActionHash = bestAction->hash();
                double bestQ1 = nonConstThis->getQ1Value(bestAction);
                double bestQ2 = nonConstThis->getQ2Value(bestAction);
                BDLOG("Double SARSA: Epsilon-greedy selected action: hash=0x%" PRIxPTR " %s with max %s=%.4f (Q1=%.4f, Q2=%.4f)", 
                      bestActionHash, bestAction->toString().c_str(), (choice == 0 ? "Q1" : "Q2"), maxQ, bestQ1, bestQ2);
            }
            
            if (bestAction != nullptr) {
                return bestAction;
            }
        }
        
        // Use random strategy: randomly select action
        BDLOG("Double SARSA: Try to randomly select a value action");
        return this->_newState->randomPickAction(enableValidValuePriorityFilter);
    }

    /**
     * @brief Determine whether to use greedy strategy
     */
    bool DoubleSarsaAgent::eGreedy() const {
        double randomValue = _uniformDist(_rng);
        bool useGreedy = randomValue >= this->_epsilon;
        BDLOG("Double SARSA: eGreedy decision - random=%.4f, epsilon=%.4f, useGreedy=%s", 
              randomValue, this->_epsilon, (useGreedy ? "true" : "false"));
        return useGreedy;
    }

    /**
     * @brief Select unexecuted action not in reuse model
     */
    ActionPtr DoubleSarsaAgent::selectUnperformedActionNotInReuseModel() const {
        std::vector<ActionPtr> actionsNotInModel;
        int totalActions = 0;
        int modelActions = 0;
        int inReuseModel = 0;
        int visitedActions = 0;
        
        for (const auto &action: this->_newState->getActions()) {
            totalActions++;
            
            if (!action->isModelAct()) {
                continue;  // Skip non-model actions
            }
            
            modelActions++;
            uintptr_t actionHash = action->hash();
            bool inModel = this->isActionInReuseModel(actionHash);
            bool visited = action->getVisitedCount() > 0;
            
            if (inModel) {
                inReuseModel++;
            }
            if (visited) {
                visitedActions++;
            }
            
            // Match condition: model action, not in reuse model, and not visited
            bool matched = !inModel && !visited;
            
            if (matched) {
                actionsNotInModel.emplace_back(action);
            }
        }
        
        if (actionsNotInModel.empty()) {
            BDLOG("Double SARSA: Cannot find unexecuted action not in reuse model - total actions=%d, model actions=%d, in reuse model=%d, visited=%d (this is normal, will try next strategy)", 
                   totalActions, modelActions, inReuseModel, visitedActions);
            return nullptr;
        }
        
        // Build cumulative weight array
        std::vector<int> cumulativeWeights;
        int totalWeight = 0;
        for (const auto &action: actionsNotInModel) {
            totalWeight += action->getPriority();
            cumulativeWeights.push_back(totalWeight);
        }
        
        if (totalWeight <= 0) {
            BDLOGE("Double SARSA: total weights is 0");
            return nullptr;
        }
        
        // Use binary search to select action
        int randI = randomInt(0, totalWeight);
        auto it = std::lower_bound(cumulativeWeights.begin(), 
                                  cumulativeWeights.end(), randI);
        size_t index = std::distance(cumulativeWeights.begin(), it);
        
        if (index < actionsNotInModel.size()) {
            return actionsNotInModel[index];
        }
        
        BDLOGE("Double SARSA: rand a null action");
        return nullptr;
    }

    /**
     * @brief Select unvisited unexecuted action in reuse model
     */
    ActionPtr DoubleSarsaAgent::selectUnperformedActionInReuseModel() const {
        float maxValue = -MAXFLOAT;
        ActionPtr nextAction = nullptr;
        
        // Cache visitedActivities
        auto modelPointer = this->_model.lock();
        stringPtrSet visitedActivities;
        if (modelPointer) {
            const GraphPtr &graphRef = modelPointer->getGraph();
            visitedActivities = graphRef->getVisitedActivities();
        } else {
            return nullptr;
        }
        
        // Use humble-gumbel distribution to influence sampling
        for (const auto &action: this->_newState->targetActions()) {
            uintptr_t actionHash = action->hash();
            
            if (this->isActionInReuseModel(actionHash)) {
                if (action->getVisitedCount() > 0) {
                    BDLOG("Double SARSA: action has been visited - %s, visitedCount=%d", 
                          action->toString().c_str(), action->getVisitedCount());
                    continue;
                }
                
                auto qualityValue = static_cast<float>(this->probabilityOfVisitingNewActivities(
                        action,
                        visitedActivities));
                
                if (qualityValue > DoubleSarsaRLConstants::QualityValueThreshold) {
                    qualityValue = DoubleSarsaRLConstants::QualityValueMultiplier * qualityValue;
                    
                    auto uniform = _uniformFloatDist(_rng);
                    
                    if (uniform < std::numeric_limits<float>::min()) {
                        uniform = std::numeric_limits<float>::min();
                    }
                    
                    qualityValue -= log(-log(uniform));

                    if (qualityValue > maxValue) {
                        maxValue = qualityValue;
                        nextAction = action;
                    }
                }
            }
        }
        
        return nextAction;
    }

    /**
     * @brief Select action based on Q-value (using randomly chosen Q1 or Q2)
     */
    ActionPtr DoubleSarsaAgent::selectActionByQValue() {
        ActionPtr returnAction = nullptr;
        float maxQ = -MAXFLOAT;
        
        // Get model pointer and set of visited activities
        auto modelPtr = this->_model.lock();
        if (!modelPtr) {
            BLOGE("Double SARSA: Model has been destroyed, cannot select action by Q value");
            return nullptr;
        }
        
        const GraphPtr &graphRef = modelPtr->getGraph();
        auto visitedActivities = graphRef->getVisitedActivities();
        
        // Randomly choose Q1 or Q2 for this selection
        int choice = _uniformIntDist(_rng);  // 0 or 1
        BDLOG("Double SARSA: selectActionByQValue using %s", (choice == 0 ? "Q1" : "Q2"));
        
        // Iterate through all actions, select action with maximum adjusted Q-value
        int actionIndex = 0;
        for (const auto &action: this->_newState->getActions()) {
            double qv = 0.0;
            uintptr_t actionHash = action->hash();
            
            // If action unvisited
            if (action->getVisitedCount() <= 0) {
                if (this->isActionInReuseModel(actionHash)) {
                    double prob = this->probabilityOfVisitingNewActivities(action, visitedActivities);
                    qv += prob;
                    BDLOG("Double SARSA: Action[%d] %s unvisited, prob=%.4f, qv=%.4f", 
                          actionIndex, action->toString().c_str(), prob, qv);
                } else {
                    // Not in reuse model (new action), directly return
                    BDLOG("Double SARSA: selectActionByQValue returning new action: %s", action->toString().c_str());
                    return action;
                }
            }
            
            // Add action's Q-value (from chosen Q-function)
            // Get both Q1 and Q2 for comparison
            double q1Value = getQ1Value(action);
            double q2Value = getQ2Value(action);
            double baseQValue = (choice == 0) ? q1Value : q2Value;
            qv += baseQValue;
            
            // Divide by EntropyAlpha for normalization
            double normalizedQv = qv / DoubleSarsaRLConstants::EntropyAlpha;
            
            // Use member random number generator
            float uniform = _uniformFloatDist(_rng);
            
            if (uniform < std::numeric_limits<float>::min()) {
                uniform = std::numeric_limits<float>::min();
            }
            
            // Add humble-gumbel distribution random term
            double gumbelTerm = log(-log(uniform));
            double adjustedQv = normalizedQv - gumbelTerm;
            
            // actionHash already defined above, reuse it
            // Suppress format warning for PRIxPTR macro - the macro expands correctly at runtime
            #pragma GCC diagnostic push
            #pragma GCC diagnostic ignored "-Wformat"
            BDLOG("Double SARSA: Action[%d] hash=0x%" PRIxPTR " %s: Q1=%.4f, Q2=%.4f, using %s=%.4f, Q_base=%.4f, Q_norm=%.4f, gumbel=%.4f, Q_adj=%.4f (max=%.4f)", 
                  actionIndex, actionHash, action->toString().c_str(), q1Value, q2Value, 
                  (choice == 0 ? "Q1" : "Q2"), baseQValue, normalizedQv, gumbelTerm, adjustedQv, static_cast<double>(maxQ));
            #pragma GCC diagnostic pop
            
            // Select action with maximum adjusted Q-value
            if (adjustedQv > maxQ) {
                maxQ = static_cast<float>(adjustedQv);
                returnAction = action;
                BDLOG("Double SARSA: New best action: %s with adjusted Q=%.4f", 
                      action->toString().c_str(), adjustedQv);
            }
            
            actionIndex++;
        }
        
        if (returnAction != nullptr) {
            uintptr_t returnActionHash = returnAction->hash();
            double returnQ1 = getQ1Value(returnAction);
            double returnQ2 = getQ2Value(returnAction);
            BDLOG("Double SARSA: selectActionByQValue selected: hash=0x%" PRIxPTR " %s with max adjusted Q=%.4f from %s (Q1=%.4f, Q2=%.4f)", 
                  returnActionHash, returnAction->toString().c_str(), maxQ, (choice == 0 ? "Q1" : "Q2"), returnQ1, returnQ2);
        }
        
        return returnAction;
    }

    /**
     * @brief Select new action (implements AbstractAgent's pure virtual function)
     */
    ActionPtr DoubleSarsaAgent::selectNewAction() {
        ActionPtr action = nullptr;
        
        // Strategy 1: Select unexecuted actions not in reuse model
        action = this->selectUnperformedActionNotInReuseModel();
        if (nullptr != action) {
            BLOG("Double SARSA: select action not in reuse model - %s", action->toString().c_str());
            return action;
        }

        // Strategy 2: Select unvisited unexecuted actions in reuse model
        action = this->selectUnperformedActionInReuseModel();
        if (nullptr != action) {
            BLOG("Double SARSA: select action in reuse model - %s", action->toString().c_str());
            return action;
        }

        // Strategy 3: Select unvisited actions
        action = this->_newState->randomPickUnvisitedAction();
        if (nullptr != action) {
            BLOG("Double SARSA: select action in unvisited action - %s", action->toString().c_str());
            return action;
        }

        // Strategy 4: If all actions are visited, select actions based on Q-values
        action = this->selectActionByQValue();
        if (nullptr != action) {
            BLOG("Double SARSA: select action by qvalue - %s", action->toString().c_str());
            return action;
        }

        // Strategy 5: Use traditional epsilon-greedy strategy
        action = this->selectNewActionEpsilonGreedyRandomly();
        if (nullptr != action) {
            BLOG("Double SARSA: select action by EpsilonGreedyRandom - %s", action->toString().c_str());
            return action;
        }
        
        // Strategy 6: All methods failed, attempt to handle null action
        BLOGE("Double SARSA: null action happened, handle null action");
        return handleNullAction();
    }

    /**
     * @brief Adjust action priorities (overrides parent class method)
     */
    void DoubleSarsaAgent::adjustActions() {
        AbstractAgent::adjustActions();
    }

    /**
     * @brief Load reuse model
     * 
     * Loads previously saved reuse model from file system.
     * Note: Q-values (Q1 and Q2) are not loaded from file, they start from 0.
     */
    void DoubleSarsaAgent::loadReuseModel(const std::string &packageName) {
        // Build model file path (dynamic vs static reuse abstraction share same schema but use different files)
        const bool useStatic = Preference::inst() && Preference::inst()->useStaticReuseAbstraction();
        std::string basePath = std::string(ModelStorageConstants::StoragePrefix) + packageName;
        std::string modelFilePath = useStatic
                                    ? (basePath + ".static" + ModelStorageConstants::ModelFileExtension)
                                    : (basePath + ModelStorageConstants::ModelFileExtension);

        // Set model save path
        this->_modelSavePath = modelFilePath;
        
        // Set default save path
        if (!this->_modelSavePath.empty()) {
            this->_defaultModelSavePath = useStatic
                                          ? (basePath + ".static" + ModelStorageConstants::TempModelFileExtension)
                                          : (basePath + ModelStorageConstants::TempModelFileExtension);
        }
        
        BLOG("Double SARSA: begin load model: %s", this->_modelSavePath.c_str());

        // Open model file
        std::ifstream modelFile(modelFilePath, std::ios::binary | std::ios::in);
        if (!modelFile.is_open()) {
            BLOGE("Double SARSA: Failed to open model file: %s", modelFilePath.c_str());
            return;
        }

        // Get file size
        std::filebuf *fileBuffer = modelFile.rdbuf();
        std::size_t filesize = fileBuffer->pubseekoff(0, modelFile.end, modelFile.in);
        fileBuffer->pubseekpos(0, modelFile.in);
        
        // Check if file size is valid
        if (filesize <= 0 || filesize > DoubleSarsaRLConstants::MaxModelFileSize) {
            BLOGE("Double SARSA: Invalid model file size: %zu", filesize);
            return;
        }
        
        // Read file content
        std::unique_ptr<char[]> modelFileData(new char[filesize]);
        std::streamsize bytesRead = fileBuffer->sgetn(modelFileData.get(), static_cast<int>(filesize));
        
        if (bytesRead != static_cast<std::streamsize>(filesize)) {
            BLOGE("Double SARSA: Failed to read complete model file: read %lld bytes, expected %zu bytes", 
                  static_cast<long long>(bytesRead), filesize);
            return;
        }
        
        // Verify buffer before deserializing (security: prevent OOB/malformed data)
        flatbuffers::Verifier verifier(reinterpret_cast<const uint8_t*>(modelFileData.get()), filesize);
        if (!VerifyReuseModelBuffer(verifier)) {
            BLOGE("Double SARSA: Invalid or corrupted model buffer");
            return;
        }
        auto reuseFBModel = GetReuseModel(modelFileData.get());
        if (!reuseFBModel) {
            BLOGE("Double SARSA: GetReuseModel returned null");
            return;
        }

        // Clear existing reuse model
        {
            std::lock_guard<std::mutex> reuseGuard(this->_reuseModelLock);
            this->_reuseModel.clear();
            this->_reuseQValue1.clear();
            this->_reuseQValue2.clear();
        }
        
        // Get model data pointer
        auto reusedModelDataPtr = reuseFBModel->model();
        if (!reusedModelDataPtr) {
            BLOG("Double SARSA: model data is null");
            return;
        }
        
        // Iterate through all reuse entries, load to memory
        for (flatbuffers::uoffset_t entryIndex = 0; entryIndex < reusedModelDataPtr->size(); entryIndex++) {
            auto reuseEntryInReuseModel = reusedModelDataPtr->Get(entryIndex);
            uint64_t actionHash = reuseEntryInReuseModel->action();
            auto activityEntry = reuseEntryInReuseModel->targets();
            
            // Build activity mapping
            ReuseEntryM entryPtr;
            for (flatbuffers::uoffset_t targetIndex = 0; targetIndex < activityEntry->size(); targetIndex++) {
                auto targetEntry = activityEntry->Get(targetIndex);
                BDLOG("Double SARSA: load model hash: %" PRIu64 " %s %d", actionHash,
                      targetEntry->activity()->str().c_str(), static_cast<int>(targetEntry->times()));
                
                entryPtr.emplace(
                        std::make_shared<std::string>(targetEntry->activity()->str()),
                        static_cast<int>(targetEntry->times()));
            }
            
            // If entry not empty, add to reuse model
            if (!entryPtr.empty()) {
                std::lock_guard<std::mutex> reuseGuard(this->_reuseModelLock);
                this->_reuseModel.emplace(actionHash, entryPtr);
                // Note: Q-values start from 0, not loaded from file
            }
        }
        
        BLOG("Double SARSA: loaded model contains %zu actions, Q1 entries=%zu, Q2 entries=%zu", 
             this->_reuseModel.size(), this->_reuseQValue1.size(), this->_reuseQValue2.size());
        BDLOG("Double SARSA: Note - Q-values (Q1 and Q2) are not loaded from file, starting from 0");
    }

    /**
     * @brief Save reuse model
     * 
     * Serializes and saves current reuse model to file system.
     * Note: Q-values (Q1 and Q2) are not saved to file.
     */
    void DoubleSarsaAgent::saveReuseModel(const std::string &modelFilepath) {
        // Create FlatBuffers builder
        flatbuffers::FlatBufferBuilder builder;
        std::vector<flatbuffers::Offset<fastbotx::ReuseEntry>> actionActivityVector;
        
        // Lock to protect concurrent access to reuse model
        {
            std::lock_guard<std::mutex> reuseGuard(this->_reuseModelLock);
            
            // Iterate through reuse model, build FlatBuffers data structure
            for (const auto &actionIterator: this->_reuseModel) {
                uint64_t actionHash = actionIterator.first;
                ReuseEntryM activityCountEntryMap = actionIterator.second;
                
                // FlatBuffers needs vector instead of map
                std::vector<flatbuffers::Offset<fastbotx::ActivityTimes>> activityCountEntryVector;
                
                // Iterate through activity mapping
                for (const auto &activityCountEntry: activityCountEntryMap) {
                    const std::string &actName = *(activityCountEntry.first);
                    size_t actLen = std::min(actName.size(), ModelStorageConstants::MaxActivityNameLength);
                    auto sentryActT = CreateActivityTimes(
                            builder,
                            builder.CreateString(actName.c_str(), actLen),  // Activity name (length capped)
                            activityCountEntry.second);
                    activityCountEntryVector.push_back(sentryActT);
                }
                
                // Create ReuseEntry object
                auto savedActivityCountEntries = CreateReuseEntry(
                        builder, 
                        actionHash,
                        builder.CreateVector(activityCountEntryVector.data(),
                                          activityCountEntryVector.size()));
                actionActivityVector.push_back(savedActivityCountEntries);
            }
        }
        
        // Create ReuseModel root object and complete serialization
        auto savedActionActivityEntries = CreateReuseModel(
                builder, 
                builder.CreateVector(actionActivityVector.data(), actionActivityVector.size()));
        builder.Finish(savedActionActivityEntries);

        // Determine output file path
        std::string outputFilePath = modelFilepath;
        if (outputFilePath.empty()) {
            outputFilePath = this->_defaultModelSavePath;
        }
        
        if (outputFilePath.empty()) {
            BLOGE("Double SARSA: Cannot save model: output file path is empty");
            return;
        }
        
        // First write to temporary file
        std::string tempFilePath = outputFilePath + ".tmp";
        BLOG("Double SARSA: save model to temporary path: %s", tempFilePath.c_str());
        
        std::ofstream outputFile(tempFilePath, std::ios::binary);
        if (!outputFile.is_open()) {
            BLOGE("Double SARSA: Failed to open temporary file for writing: %s", tempFilePath.c_str());
            return;
        }
        
        // Write serialized data
        outputFile.write(reinterpret_cast<const char*>(builder.GetBufferPointer()), 
                        static_cast<int>(builder.GetSize()));
        outputFile.close();
        
        // Check if write was successful
        if (outputFile.fail()) {
            BLOGE("Double SARSA: Failed to write model to temporary file: %s", tempFilePath.c_str());
            std::remove(tempFilePath.c_str());
            return;
        }
        
        // Atomically replace original file
        if (std::rename(tempFilePath.c_str(), outputFilePath.c_str()) != 0) {
            BLOGE("Double SARSA: Failed to rename temporary file to final file: %s -> %s", 
                  tempFilePath.c_str(), outputFilePath.c_str());
            std::remove(tempFilePath.c_str());
            return;
        }
        
        BLOG("Double SARSA: Model saved successfully to: %s (reuse entries=%zu, Q1 entries=%zu, Q2 entries=%zu)", 
             outputFilePath.c_str(), this->_reuseModel.size(), this->_reuseQValue1.size(), this->_reuseQValue2.size());
        BDLOG("Double SARSA: Note - Q-values (Q1 and Q2) are not saved to file, only reuse model is persisted");
    }

    /**
     * @brief Background thread function for periodic model saving
     * 
     * Periodically saves the reuse model to disk every 10 minutes (ModelSaveIntervalMs).
     * Runs in a background thread until the agent is destructed.
     * 
     * Implementation details:
     * 1. Saves model every 10 minutes (ModelSaveIntervalMs)
     * 2. Uses weak_ptr to avoid circular references
     * 3. Thread automatically exits when agent is destructed (weak_ptr becomes invalid)
     * 4. Briefly locks agent to get save path, then releases lock before IO operations
     * 
     * @param agent Weak pointer to DoubleSarsaAgent instance
     */
    void DoubleSarsaAgent::threadModelStorage(const std::weak_ptr<DoubleSarsaAgent> &agent) {
        constexpr int saveInterval = DoubleSarsaRLConstants::ModelSaveIntervalMs;  // 10 minutes
        constexpr auto interval = std::chrono::milliseconds(saveInterval);
        
        // Loop to save until Agent is destructed (weak_ptr becomes invalid)
        while (true) {
            // Briefly lock Agent to get save path
            auto agentPtr = agent.lock();
            if (!agentPtr) {
                // Agent has been destructed, exit thread
                break;
            }
            
            // Copy save path
            std::string savePath = agentPtr->_modelSavePath;
            
            // Immediately release lock to avoid holding for long time
            agentPtr.reset();
            
            // Save model outside lock (IO operations may be slow)
            if (auto locked = agent.lock()) {
                locked->saveReuseModel(savePath);
            }
            
            // Wait for specified interval before saving again
            std::this_thread::sleep_for(interval);
        }
    }

}  // namespace fastbotx

#endif
