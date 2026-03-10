/*
 * This code is licensed under the Fastbot license. You may obtain a copy of this license in the LICENSE.txt file in the root directory of this source tree.
 */
/**
 * @authors Jianqiang Guo, Yuhui Su, Zhao Zhang
 */

#ifndef SarsaAgent_H_
#define SarsaAgent_H_

#include "AbstractAgent.h"
#include "Base.h"
#include "Action.h"
#include "State.h"
#include "ContentAwareInputProvider.h"
#include "WidgetPriorityProvider.h"
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <mutex>

namespace fastbotx {

    // Keep reuse-model container types consistent across agents, using unordered_map for better average performance.
    using ReuseEntryM = std::unordered_map<stringPtr, int>;
    using ReuseEntryIntMap = std::unordered_map<uint64_t, ReuseEntryM>;

    class Model;
    typedef std::shared_ptr<Model> ModelPtr;

    class SarsaAgent : public AbstractAgent {
    public:
        // Default SARSA hyper-parameters (kept from legacy ReuseAgent).
        static constexpr double kDefaultAlpha   = 0.25;
        static constexpr double kDefaultBeta    = 0.8;
        static constexpr double kDefaultEpsilon = 0.05;
        static constexpr double kDefaultGamma   = 0.8;

        // Max number of recent steps to keep in reward history / action history.
        static constexpr int kMaxHistorySteps = 5;

        // Entropy temperature used in Q-value based action selection.
        static constexpr double kEntropyAlpha = 0.1;

        /// Min steps between content-aware input LLM calls (reduces latency; provider cache still helps).
        static constexpr size_t kMinStepsBetweenContentAwareInputCalls = 5;
        /// When allowed by step throttle, only call LLM with this probability (0.0–1.0) to further reduce load.
        static constexpr double kContentAwareInputCallProbability = 0.3;

        /// Visit-count soft weight: weight *= 1/(1 + kVisitCountWeightFactor * visitCount).
        /// So unvisited=1.0, visited once=1/2, twice=1/3... Same abstract widget can be re-selected; resolveAt() then picks next concrete node (e.g. another tab).
        static constexpr double kVisitCountWeightFactor = 1.0;

        explicit SarsaAgent(const ModelPtr &model);

        ~SarsaAgent() override;

        /// Load reuse model from FBM file (`/sdcard/fastbot_{pkg}[.static].fbm`).
        void loadReuseModel(const std::string &packageName);

        /// Save reuse model to FBM file.
        void saveReuseModel(const std::string &modelFilepath);

        /// Save reuse model to current _modelSavePath (for JNI when test ends normally).
        void saveReuseModelNow();

        /// Background thread: periodically save reuse model.
        static void threadModelStorage(const std::weak_ptr<SarsaAgent> &agent);

        virtual void moveForward(StatePtr nextState) override;

        /// Optional: content-aware input (same as LLMExplorerAgent). When max.llm.contextAwareInput=true, editable widgets get LLM-suggested text.
        std::string getInputTextForAction(const StatePtr &state, const ActionPtr &action) const override;

        /// Set custom content-aware input provider (default: LlmContentAwareInputProvider).
        void setContentAwareInputProvider(const std::shared_ptr<IContentAwareInputProvider> &provider);

        /// Set custom widget priority provider (default: LlmWidgetPriorityProvider). When set and max.llm.knowledge=true, pre-allocates widgetPriorities for each new state.
        void setWidgetPriorityProvider(const std::shared_ptr<IWidgetPriorityProvider> &provider);

    protected:
        SarsaAgent();

        /// Compute reward for latest transition and update Q-values (SARSA).
        void updateStrategy() override;

        /// Select next action according to legacy ReuseAgent policy.
        ActionPtr selectNewAction() override;

        double getNewReward();

        double getReuseActionValue(const ActivityStateActionPtr &action,
                                   const stringPtrSet &visitedActivities) const;

        double getStateActionValue(const StatePtr &state,
                                   const stringPtrSet &visitedActivities) const;

        void updateReuseModel();

        ActivityStateActionPtr selectNewActionEpsilonGreedyRandomly() const;

        bool eGreedy() const;

        ActionPtr selectActionNotInModel() const;

        ActionPtr selectActionInModel(const stringPtrSet &visitedActivities) const;

        ActionPtr selectActionByQValue(const stringPtrSet &visitedActivities) const;

        void adjustActions() override;

    protected:
        double _alpha;
        double _gamma;
        double _epsilon;

        std::vector<double> _rewardHistory;
        std::vector<ActionPtr> _previousActions;

    private:
        ReuseEntryIntMap _reuseModel;
        std::string _modelSavePath;
        std::string _tmpSavePath;
        static std::string DefaultModelSavePath;
        mutable std::mutex _reuseModelLock;

        /// Pluggable content-aware input (same as LLMExplorerAgent); only used when max.llm.contextAwareInput=true.
        std::shared_ptr<IContentAwareInputProvider> _contentAwareInputProvider;

        /// Step count (incremented in moveForward); used to throttle content-aware input LLM calls.
        size_t _stepCount = 0;
        /// Last step at which we called content-aware input provider (0 = never); throttle: at most once every kMinStepsBetweenContentAwareInputCalls.
        mutable size_t _lastContentAwareInputStep = 0;

        /// Widget priorities from knowledge_org: (stateHash, actionHash) -> priority (about 1.0~5.0 after amplification). Default 1.0 when not set.
        std::unordered_map<uintptr_t, double> _actionPriority;
        /// State hashes for which we have already requested knowledge_org (one request per new state).
        std::unordered_set<uintptr_t> _stateWidgetPrioritiesRequested;
        /// Widget priority provider (LLM); when enabled, pre-allocates widgetPriorities per new state.
        std::shared_ptr<IWidgetPriorityProvider> _widgetPriorityProvider;

        /// Key for _actionPriority: (stateHash << 32) | (actionHash & 0xFFFFFFFFu)
        static uintptr_t kActionPriorityKey(uintptr_t stateHash, uintptr_t actionHash) {
            return (stateHash << 32) | (actionHash & 0xFFFFFFFFu);
        }
        /// Ensure knowledge_org has been run for this state; fills _actionPriority. Idempotent per state hash.
        void ensureWidgetPrioritiesForState(const StatePtr &state);
        /// Get widget priority for (state, action); 1.0 if not in map.
        double getWidgetPriority(uintptr_t stateHash, uintptr_t actionHash) const;

        /// Clear in-memory reuse model when loading from disk fails or model is invalid.
        void clearReuseModelOnLoadFailure();

        /// Whether to enable advanced reuse-based decision tuning (loop avoidance, coverage bias).
        static bool isReuseDecisionTuningEnabled();

        /// Compute how strongly this action tends to stay within the current activity / small loop.
        double computeLoopBias(uint64_t actionHash, const stringPtr &currentActivity) const;

        /// Compute a simple coverage diversity score based on the number of distinct target activities.
        double computeCoverageDiversity(uint64_t actionHash) const;
    };

    typedef std::shared_ptr<SarsaAgent> SarsaAgentPtr;

}

#endif /* SarsaAgent_H_ */

