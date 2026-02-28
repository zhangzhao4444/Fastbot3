/**
 * @author Zhao Zhang
 *
 * Depth-First-Search (DFS) exploration agent.
 *
 * DFSAgent implements a simple depth-first exploration strategy on top of
 * the existing state graph:
 *
 * - At each state, it prefers unvisited target actions (actions that have widgets)
 *   so that the exploration quickly "dives" along a UI path.
 * - When all actions in the current state have been visited, it prefers the BACK
 *   action so that the exploration backtracks to previous screens.
 * - As a last resort, it falls back to randomPickAction() for robustness.
 *
 * Unlike DoubleSarsaAgent, DFSAgent does not maintain Q-values or reuse models.
 * It is purely structural and stateless apart from visit counts maintained by the graph.
 */
#ifndef FASTBOTX_DFS_AGENT_H
#define FASTBOTX_DFS_AGENT_H

#include "AbstractAgent.h"

#include <vector>
#include <deque>
#include <unordered_set>
#include <unordered_map>
#include <random>
#include <cstdint>
#include <string>

namespace fastbotx {

    /**
     * Simple DFS exploration agent.
     *
     * DFS is implemented at the state-graph level using an explicit stack of
     * states and a visited-state set. For each state on the stack we keep the
     * next action index to try, so that repeated calls to selectNewAction()
     * continue the same DFS instead of restarting from the first action.
     */
    class DFSAgent : public AbstractAgent {
    public:
        /**
         * @brief Constructor
         *
         * Initializes DFSAgent with a model pointer. The underlying algorithm type
         * is kept as AlgorithmType::Random so that existing call sites that pass
         * Random can be mapped to DFS exploration without changing state types.
         *
         * @param model Model pointer
         */
        explicit DFSAgent(const ModelPtr &model);

        ~DFSAgent() override = default;

    protected:
        /**
         * @brief DFSAgent has no learning state, so updateStrategy is a no-op.
         */
        void updateStrategy() override;

        /**
         * @brief Select new action using depth-first strategy on the state graph.
         *
         * Policy:
         * 1. Prefer unvisited target actions (requireTarget == true, !isVisited()).
         * 2. Then prefer any unvisited valid action.
         * 3. Then re-use visited but non-saturated actions (state->isSaturated == false)
         *    to allow reaching potential new states again.
         * 4. When all actions for the current state frame are exhausted, pop the
         *    frame and backtrack via BACK action if available.
         * 5. If DFS stack is empty, fall back to randomPickAction() for robustness.
         */
        ActionPtr selectNewAction() override;

        /**
         * @brief Move forward in the state machine and maintain DFS stack/visited set.
         *
         * Extends AbstractAgent::moveForward by:
         * - Pushing newly reached states onto the DFS stack (with nextIndex = 0)
         * - Trimming the stack when revisiting an existing state (backtracking)
         */
        void moveForward(StatePtr nextState) override;

    private:
        struct Frame {
            StatePtr state;
            size_t nextIndex;              // cursor into shuffled order
            std::vector<size_t> order;     // shuffled indices into state's actions
        };

        struct EdgeStats {
            uint32_t total = 0;        // total times this (state, action) edge was used
            uint32_t newStates = 0;    // times it led to a previously unseen state
            uint32_t newActivities = 0; // times it led to a previously unseen activity
        };

        std::vector<Frame> _stack;
        std::unordered_set<uintptr_t> _visitedStates;
        std::unordered_set<std::string> _visitedActivities;
        std::unordered_map<std::uint64_t, EdgeStats> _edgeStats;
        mutable std::mt19937 _rng{std::random_device{}()};
        
        /// Root activity (first visited activity) - BACK should not be used on this activity
        std::string _rootActivity;
        
        /// State block counter: tracks consecutive steps with same state hash (for self-rescue)
        int _stateBlockCounter;
        /// Last state hash to detect state changes
        uintptr_t _lastStateHash;
        /// Activity block counter: same activity for many steps (GUI tree may change, so state hash is unstable)
        int _activityBlockCounter;
        /// Last activity name for activity block detection
        std::string _lastActivity;

        /// VET-style exploration tarpit: recent activity names (sliding window)
        static constexpr size_t kRecentActivityWindowSize = 50;
        static constexpr size_t kTarpitMinWindowSize = 30;   // need at least this many steps to decide tarpit
        static constexpr int kTarpitMaxDistinctActivities = 3; // distinct activities in window <= this => tarpit
        static constexpr int kCoverageDrivenDeepLinkInterval = 25;  // try DEEP_LINK every N select steps (Delm-style coverage)
        int _selectCallCount = 0;  // steps since last DEEP_LINK; used for coverage-driven DEEP_LINK
        std::deque<std::string> _recentActivities;
        /// True when distinct activities in recent window <= kTarpitMaxDistinctActivities and window >= kTarpitMinWindowSize
        bool inTarpit() const;

        /// (stateHash, actionHash) -> targetStateHash: which state we land in after taking action from state
        std::unordered_map<std::uint64_t, uintptr_t> _edgeToTarget;
        /// stateHash -> (total widget/action count, visited count): coverage of each state we've entered
        std::unordered_map<uintptr_t, std::pair<int, int>> _stateCoverage;

        /// True if action from state is saturated by target-state coverage (target state fully explored)
        bool isActionSaturatedByTargetState(const StatePtr &state,
                                           const ActivityStateActionPtr &action) const;

        double getEdgeNoveltyScore(const StatePtr &state,
                                   const ActivityStateActionPtr &action) const;
        double getActionTypeWeight(const ActivityStateActionPtr &action) const;
    };

    using DFSAgentPtr = std::shared_ptr<DFSAgent>;

}  // namespace fastbotx

#endif // FASTBOTX_DFS_AGENT_H

