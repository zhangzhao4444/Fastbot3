/**
 * @authors Zhao Zhang
 */
 
#ifndef DoubleSarsaAgent_H_
#define DoubleSarsaAgent_H_

#include "AbstractAgent.h"
#include "State.h"
#include "Action.h"
#include <vector>
#include <unordered_map>
#include <random>

namespace fastbotx {

    /**
     * @brief Double SARSA reinforcement learning algorithm constants namespace
     * 
     * Defines all constants used by DoubleSarsaAgent for Double SARSA algorithm.
     * Double SARSA uses two independent Q-functions to reduce overestimation bias.
     */
    namespace DoubleSarsaRLConstants {
        // ========== Basic Learning Parameters ==========
        /// Default learning rate (alpha), controls Q-value update step size, range [0,1]
        constexpr double DefaultAlpha = 0.25;
        /// Default exploration rate (epsilon), controls random exploration probability, range [0,1]
        constexpr double DefaultEpsilon = 0.05;
        /// Default discount factor (gamma), controls importance of future rewards, range [0,1]
        constexpr double DefaultGamma = 0.8;
        /// N-step Double SARSA step count, uses N-step returns to update Q-values
        constexpr int NStep = 5;
        
        // ========== Alpha Dynamic Adjustment Parameters ==========
        /// Initial moving average alpha value
        constexpr double InitialMovingAlpha = 0.5;
        /// Alpha decrement step, alpha gradually decreases as visit count increases
        constexpr double AlphaDecrement = 0.1;
        /// Alpha adjustment threshold 1: when visit count exceeds this, alpha decreases by 0.1
        constexpr long AlphaThreshold1 = 20000;
        /// Alpha adjustment threshold 2: when visit count exceeds this, alpha decreases by 0.1 again
        constexpr long AlphaThreshold2 = 50000;
        /// Alpha adjustment threshold 3: when visit count exceeds this, alpha decreases by 0.1 again
        constexpr long AlphaThreshold3 = 100000;
        /// Alpha adjustment threshold 4: when visit count exceeds this, alpha decreases by 0.1 again
        constexpr long AlphaThreshold4 = 250000;
        
        // ========== Reward Calculation Constants ==========
        /// Reward epsilon threshold, used to determine if reward value is close to 0
        constexpr double RewardEpsilon = 0.0001;
        /// Reward value for new actions (actions not in reuse model)
        constexpr double NewActionReward = 1.0;
        /// Reward value for visited actions (actions in reuse model and already visited)
        constexpr double VisitedActionReward = 0.5;
        /// Reward value for new actions in state (state contains new actions)
        constexpr double NewActionInStateReward = 1.0;
        
        // ========== Action Selection Constants ==========
        /// Entropy alpha value, used to add randomness in Q-value selection
        constexpr double EntropyAlpha = 0.1;
        /// Quality value multiplier, used to amplify action quality values
        constexpr float QualityValueMultiplier = 10.0f;
        /// Quality value threshold, only actions with quality value above this are considered
        constexpr float QualityValueThreshold = 1e-4f;
        
        // ========== Model Storage Constants ==========
        /// Model save interval (milliseconds), background thread saves model every 10 minutes
        constexpr int ModelSaveIntervalMs = 1000 * 60 * 10; // 10 minutes
        /// Maximum model file size (100MB), prevents loading overly large model files
        constexpr size_t MaxModelFileSize = 100 * 1024 * 1024; // 100MB
    }

    // ========== Reuse Model Data Structure Type Definitions ==========
    /// Reuse entry mapping: Activity name -> visit count
    typedef std::unordered_map<stringPtr, int> ReuseEntryM;
    /// Reuse model mapping: action hash -> (Activity name -> visit count)
    typedef std::unordered_map<uint64_t, ReuseEntryM> ReuseEntryIntMap;
    /// Q-value mapping: action hash -> Q-value (for Q1)
    typedef std::map<uint64_t, double> ReuseEntryQValueMap;
    /// Q-value mapping: action hash -> Q-value (for Q2)
    typedef std::map<uint64_t, double> ReuseEntryQValueMap2;

    /**
     * @brief Double SARSA reinforcement learning agent
     * 
     * DoubleSarsaAgent implements a test agent based on Double SARSA reinforcement learning algorithm.
     * It maintains two independent Q-functions (Q1 and Q2) to reduce overestimation bias.
     * 
     * Core features:
     * 1. **Double Q-Functions**: Maintains Q1 and Q2 to reduce overestimation
     * 2. **Action Selection**: Randomly selects Q1 or Q2 for action selection
     * 3. **Q-value Updates**: Randomly updates Q1 or Q2, using the other Q-function for evaluation
     * 4. **Reuse Model**: Records action-to-activity mapping relationships
     * 
     * Double SARSA Algorithm:
     * - Action selection: Randomly choose Q1 or Q2, then select action with max Q-value
     * - Q-value update: Randomly update Q1 or Q2:
     *   * If updating Q1: target = R + γ * Q2(s', a')
     *   * If updating Q2: target = R + γ * Q1(s', a')
     * 
     * Advantages over standard SARSA:
     * - Reduces overestimation bias
     * - More stable learning in noisy environments
     * - Better action selection reliability
     * 
     * Action selection strategy (priority from high to low):
     * 1. Select unexecuted actions not in reuse model (explore new actions)
     * 2. Select unvisited unexecuted actions in reuse model (based on humble-gumbel distribution)
     * 3. Select unvisited actions
     * 4. Select actions based on Q-values (with added randomness)
     * 5. Epsilon-greedy strategy (random selection with epsilon probability, otherwise select
     *    action with highest Q-value from randomly chosen Q1 or Q2)
     * 
     * Performance optimizations:
     * - Uses member random number generator to avoid creating new generators each time
     * - Uses mutex to protect concurrent access to reuse model and Q-functions
     * - Uses binary search to optimize action selection
     * - Caches visitedActivities to avoid repeated queries
     */
    class DoubleSarsaAgent : public AbstractAgent {

    public:
        /**
         * @brief Constructor
         * 
         * Initializes Double SARSA parameters and reuse model.
         * 
         * @param model Model pointer
         */
        explicit DoubleSarsaAgent(const ModelPtr &model);

        /**
         * @brief Load reuse model
         * 
         * Loads previously saved reuse model from file system.
         * Model file path: /sdcard/fastbot_{packageName}.fbm
         * 
         * @param packageName Application package name, used to construct model file path
         */
        virtual void loadReuseModel(const std::string &packageName);

        /**
         * @brief Save reuse model
         * 
         * Serializes and saves current reuse model to file system.
         * Uses FlatBuffers for serialization, uses temporary file + atomic replace to ensure data integrity.
         * 
         * @param modelFilepath Model file path, uses _defaultModelSavePath if empty
         */
        void saveReuseModel(const std::string &modelFilepath);

        /**
         * @brief Background thread function for periodic model saving
         * 
         * Static method that runs in a background thread to periodically save the reuse model.
         * Saves model every 10 minutes (ModelSaveIntervalMs) until the agent is destructed.
         * 
         * @param agent Weak pointer to DoubleSarsaAgent instance
         * 
         * @note
         * - Uses weak_ptr to avoid circular references
         * - Thread automatically exits when agent is destructed (weak_ptr becomes invalid)
         * - Model should have been saved by periodic save thread (threadModelStorage)
         */
        static void threadModelStorage(const std::weak_ptr<DoubleSarsaAgent> &agent);

        /**
         * @brief Destructor
         * 
         * Saves reuse model and cleans up resources.
         */
        ~DoubleSarsaAgent() override;

    protected:
        /**
         * @brief Compute reward value of latest action
         * 
         * Computes reward based on probability that action can reach unvisited activities.
         * Reward calculation formula:
         * reward = probabilityOfVisitingNewActivities / sqrt(visitedCount + 1)
         *        + getStateActionExpectationValue / sqrt(stateVisitedCount + 1)
         * 
         * @return Reward value
         */
        virtual double computeRewardOfLatestAction();

        /**
         * @brief Update strategy
         * 
         * Implements AbstractAgent's pure virtual function.
         * Called after action execution, performs the following operations:
         * 1. Compute reward value of latest action
         * 2. Update reuse model (record action-to-activity mapping)
         * 3. Update Q-values using Double SARSA algorithm
         * 4. Add new action to action history cache
         */
        void updateStrategy() override;

        /**
         * @brief Select action using epsilon-greedy strategy with Double SARSA
         * 
         * With (1-epsilon) probability selects action with highest Q-value from randomly chosen Q1 or Q2,
         * with epsilon probability randomly selects action (exploration).
         * 
         * @return Selected action
         */
        virtual ActivityStateActionPtr selectNewActionEpsilonGreedyRandomly() const;

        /**
         * @brief Determine whether to use greedy strategy
         * 
         * Generates random number, if >= epsilon uses greedy strategy, otherwise uses random strategy.
         * 
         * @return true means use greedy strategy, false means use random strategy
         */
        virtual bool eGreedy() const;

        /**
         * @brief Select new action (implements AbstractAgent's pure virtual function)
         * 
         * Attempts to select action in priority order:
         * 1. Select unexecuted actions not in reuse model
         * 2. Select unvisited unexecuted actions in reuse model
         * 3. Select unvisited actions
         * 4. Select actions based on Q-values (using randomly chosen Q1 or Q2)
         * 5. Epsilon-greedy strategy
         * 6. If all fail, call handleNullAction()
         * 
         * @return Selected action, or nullptr on failure
         */
        ActionPtr selectNewAction() override;

        /**
         * @brief Compute probability that action can reach unvisited activities
         * 
         * Based on reuse model, computes the ratio of unvisited activity visit counts
         * to total visit counts among activities that this action can reach.
         * 
         * @param action Action to evaluate
         * @param visitedActivities Set of visited activities
         * @return Probability value, range [0,1]
         */
        double probabilityOfVisitingNewActivities(const ActivityStateActionPtr &action,
                                                  const stringPtrSet &visitedActivities) const;

        /**
         * @brief Compute state action expectation value
         * 
         * Evaluates expected value of reaching unvisited activities after executing actions from this state.
         * 
         * @param state State to evaluate
         * @param visitedActivities Set of visited activities
         * @return Expectation value
         */
        double getStateActionExpectationValue(const StatePtr &state,
                                              const stringPtrSet &visitedActivities) const;

        /**
         * @brief Update reuse model
         * 
         * Records latest executed action and the activity it reached.
         * If action not in reuse model, creates new entry; otherwise updates visit count.
         */
        virtual void updateReuseModel();

        /**
         * @brief Adjust action priorities (overrides parent class method)
         * 
         * First calls parent class's adjustActions(), then can add additional priority adjustment logic.
         */
        void adjustActions() override;

        /**
         * @brief Select unexecuted action not in reuse model
         * 
         * @return Selected action, or nullptr if none
         */
        ActionPtr selectUnperformedActionNotInReuseModel() const;

        /**
         * @brief Select unvisited unexecuted action in reuse model
         * 
         * @return Selected action, or nullptr if none
         */
        ActionPtr selectUnperformedActionInReuseModel() const;

        /**
         * @brief Select action based on Q-value (using randomly chosen Q1 or Q2)
         * 
         * Uses humble-gumbel distribution to add randomness, selects action with maximum adjusted Q-value.
         * Randomly chooses Q1 or Q2 for Q-value calculation.
         * 
         * @return Selected action, or nullptr if none
         */
        ActionPtr selectActionByQValue();

    protected:
        /// Learning rate (alpha), controls Q-value update step size, dynamically adjusted based on visit count
        double _alpha{};
        
        /// Exploration rate (epsilon), controls random exploration probability
        double _epsilon{};

        /**
         * @brief Reward cache
         * 
         * _rewardCache[i] stores reward value for _previousActions[i].
         * Used for N-step Double SARSA algorithm, caches rewards of recent N steps.
         */
        std::vector<double> _rewardCache;
        
        /**
         * @brief Action history cache
         * 
         * Stores recently executed actions of last N steps, used for N-step Double SARSA algorithm to update Q-values.
         * Length does not exceed NStep (5).
         */
        std::vector<ActionPtr> _previousActions;

    private:
        // ========== Random Number Generators (Performance Optimization) ==========
        /// Mersenne Twister random number generator, thread-safe and performant
        mutable std::mt19937 _rng;
        /// Uniform distribution for double in range [0,1)
        mutable std::uniform_real_distribution<double> _uniformDist{0.0, 1.0};
        /// Uniform distribution for float in range [0,1)
        mutable std::uniform_real_distribution<float> _uniformFloatDist{0.0f, 1.0f};
        /// Uniform distribution for int in range [0,1) for choosing Q1 or Q2
        mutable std::uniform_int_distribution<int> _uniformIntDist{0, 1};
        
        // ========== Reuse Model Data ==========
        /**
         * @brief Reuse model
         * 
         * Records activities that each action (identified by hash) can reach and their visit counts.
         * Structure: action hash -> (Activity name -> visit count)
         */
        ReuseEntryIntMap _reuseModel;
        
        /**
         * @brief Q1-value mapping
         * 
         * Records Q1-value (quality value) of each action.
         * Structure: action hash -> Q1-value
         */
        ReuseEntryQValueMap _reuseQValue1;
        
        /**
         * @brief Q2-value mapping
         * 
         * Records Q2-value (quality value) of each action.
         * Structure: action hash -> Q2-value
         */
        ReuseEntryQValueMap2 _reuseQValue2;
        
        // ========== Model File Paths ==========
        /// Model save path (main path)
        std::string _modelSavePath;
        /// Default model save path (temporary path, used when main path is empty)
        std::string _defaultModelSavePath;
        /// Static default model save path
        static std::string DefaultModelSavePath;
        
        // ========== Thread Safety ==========
        /// Reuse model mutex, protects concurrent access to _reuseModel and Q-value maps
        /// Uses mutable to allow locking in const methods
        mutable std::mutex _reuseModelLock;

        /**
         * @brief Compute alpha value
         * 
         * Dynamically adjusts learning rate alpha based on current state's visit count.
         * More visits result in smaller alpha (learning rate decay).
         */
        void computeAlphaValue();
        
        /**
         * @brief Calculate alpha value based on visit count
         * 
         * @param visitCount Total visit count
         * @return Calculated alpha value
         */
        double calculateAlphaByVisitCount(long visitCount) const;

        /**
         * @brief Get action's Q1-value
         * 
         * @param action Action pointer
         * @return Q1-value
         */
        double getQ1Value(const ActionPtr &action);
        
        /**
         * @brief Get action's Q2-value
         * 
         * @param action Action pointer
         * @return Q2-value
         */
        double getQ2Value(const ActionPtr &action);
        
        /**
         * @brief Get action's Q-value (from randomly chosen Q1 or Q2)
         * 
         * Randomly chooses Q1 or Q2 and returns the Q-value.
         * Used for action selection.
         * 
         * @param action Action pointer
         * @return Q-value from randomly chosen Q-function
         */
        double getQValue(const ActionPtr &action);

        /**
         * @brief Set action's Q1-value
         * 
         * @param action Action pointer
         * @param qValue Q1-value
         */
        void setQ1Value(const ActionPtr &action, double qValue);
        
        /**
         * @brief Set action's Q2-value
         * 
         * @param action Action pointer
         * @param qValue Q2-value
         */
        void setQ2Value(const ActionPtr &action, double qValue);
        
        /**
         * @brief Update Q-values using N-step Double SARSA algorithm
         * 
         * Implements N-step Double SARSA algorithm:
         * - Randomly chooses to update Q1 or Q2
         * - Uses the other Q-function for bootstrapping
         * - Updates all actions in the N-step window
         * 
         * Double SARSA update rule:
         * - Randomly choose Q1 or Q2 to update
         * - If updating Q1: target = R + γ * Q2(s', a')
         * - If updating Q2: target = R + γ * Q1(s', a')
         */
        void updateQValues();
        
        /**
         * @brief Check if action is in reuse model
         * 
         * @param actionHash Action's hash value
         * @return true if in reuse model, false otherwise
         */
        bool isActionInReuseModel(uintptr_t actionHash) const;

        /// Clear in-memory reuse model and Q-value tables when loading from disk fails or model is invalid.
        void clearReuseModelOnLoadFailure();
    };

    typedef std::shared_ptr<DoubleSarsaAgent> DoubleSarsaAgentPtr;

}

#endif /* DoubleSarsaAgent_H_ */
