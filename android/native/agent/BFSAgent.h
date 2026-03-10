/*
 * This code is licensed under the Fastbot license. You may obtain a copy of this license in the LICENSE.txt file in the root directory of this source tree.
 */
/**
 * @author Zhao Zhang
 *
 * Breadth-First-Search (BFS) exploration agent.
 *
 * BFSAgent implements a breadth-first exploration strategy on top of
 * the existing state graph:
 *
 * - Explores in "layer" order: first exhaust one-step reachable interactions
 *   from the current UI state, then two-step reachable, and so on.
 * - Uses a queue (FIFO) of states to explore instead of a stack; states
 *   are processed in the order they were first discovered.
 * - On an unweighted graph this yields more uniform coverage and shortest
 *   step count to each reachable node.
 *
 * Unlike DoubleSarsaAgent, BFSAgent does not maintain Q-values or reuse models.
 * It is purely structural, similar to DFSAgent but with BFS ordering.
 */
#ifndef FASTBOTX_BFS_AGENT_H
#define FASTBOTX_BFS_AGENT_H

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
     * BFS exploration agent.
     *
     * BFS is implemented using an explicit queue of states (frontier) and
     * a set of visited state hashes. The front of the queue is the next
     * state to explore; when an action is taken, the resulting state is
     * enqueued at the back. This gives "one-step, then two-step, ..." order.
     */
    class BFSAgent : public AbstractAgent {
    public:
        explicit BFSAgent(const ModelPtr &model);

        ~BFSAgent() override = default;

    protected:
        void updateStrategy() override;

        /**
         * Select new action using breadth-first strategy: pick from the
         * state at the front of the queue; when that state is exhausted,
         * dequeue it and continue with the next.
         */
        ActionPtr selectNewAction() override;

        /**
         * Move forward: enqueue the new state at the back of the frontier
         * if not already in the queue (no stack trim like DFS).
         */
        void moveForward(StatePtr nextState) override;

    private:
        struct Frame {
            StatePtr state;
            size_t nextIndex;
            std::vector<size_t> order;
            int depth = 0;  // step count from root; used for level-order / novelty weighting
        };

        struct EdgeStats {
            uint32_t total = 0;
            uint32_t newStates = 0;
            uint32_t newActivities = 0;
        };

        std::deque<Frame> _queue;
        std::unordered_set<uintptr_t> _stateInQueue;  // hashes of states currently in _queue
        std::unordered_map<uintptr_t, int> _stateDepth;  // state hash -> depth (steps from root)
        int _currentDepth = 0;  // current BFS layer; only process frames with frame.depth == _currentDepth (strict level-order)
        std::unordered_map<std::string, int> _activityInQueueCount;  // activity -> count in queue (for activity-level dedup)
        std::unordered_set<uintptr_t> _visitedStates;
        std::unordered_set<std::string> _visitedActivities;
        std::unordered_map<std::uint64_t, EdgeStats> _edgeStats;
        mutable std::mt19937 _rng{std::random_device{}()};

        std::string _rootActivity;
        int _stateBlockCounter;
        uintptr_t _lastStateHash;
        int _activityBlockCounter;
        std::string _lastActivity;

        static constexpr size_t kRecentActivityWindowSize = 50;
        static constexpr size_t kTarpitMinWindowSize = 30;
        static constexpr int kTarpitMaxDistinctActivities = 3;
        static constexpr int kCoverageDrivenDeepLinkInterval = 25;  // try DEEP_LINK every N select steps (Delm-style coverage)
        int _selectCallCount = 0;  // steps since last DEEP_LINK; used for coverage-driven DEEP_LINK
        std::deque<std::string> _recentActivities;
        bool inTarpit() const;

        std::unordered_map<std::uint64_t, uintptr_t> _edgeToTarget;
        std::unordered_map<uintptr_t, std::pair<int, int>> _stateCoverage;

        bool isActionSaturatedByTargetState(const StatePtr &state,
                                           const ActivityStateActionPtr &action) const;

        double getEdgeNoveltyScore(const StatePtr &state,
                                  const ActivityStateActionPtr &action) const;
        double getActionTypeWeight(const ActivityStateActionPtr &action) const;
    };

    using BFSAgentPtr = std::shared_ptr<BFSAgent>;

}  // namespace fastbotx

#endif // FASTBOTX_BFS_AGENT_H
