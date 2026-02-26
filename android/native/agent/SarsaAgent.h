/**
 * @authors Zhao Zhang
 */

#ifndef SarsaAgent_H_
#define SarsaAgent_H_

#include "AbstractAgent.h"
#include "Base.h"
#include "Action.h"
#include "State.h"
#include <unordered_map>
#include <vector>
#include <mutex>

namespace fastbotx {

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

        explicit SarsaAgent(const ModelPtr &model);

        ~SarsaAgent() override;

        /// Load reuse model from FBM file (`/sdcard/fastbot_{pkg}[.static].fbm`).
        void loadReuseModel(const std::string &packageName);

        /// Save reuse model to FBM file.
        void saveReuseModel(const std::string &modelFilepath);

        /// Background thread: periodically save reuse model.
        static void threadModelStorage(const std::weak_ptr<SarsaAgent> &agent);

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
    };

    typedef std::shared_ptr<SarsaAgent> SarsaAgentPtr;

}

#endif /* SarsaAgent_H_ */

