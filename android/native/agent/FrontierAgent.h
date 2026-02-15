/*
 * This code is licensed under the Fastbot license. You may obtain a copy of this license in the LICENSE.txt file in the root directory of this source tree.
 */
/**
 * Frontier-based exploration agent.
 *
 * Algorithm framework aligned with MA-SLAM (arXiv:2511.14330):
 * - Structured map: state graph + _outEdges (boundary/visit) + BFS distance.
 * - Global planner: select one frontier (state, action) as long-range target by score.
 * - Local execution: either one-step (target at current state) or follow BFS path to target then execute.
 *
 * Scoring follows FH-DRL (arXiv:2407.18892): exponential-hyperbolic balance of
 * "proximity" vs "information gain", i.e. score = infoGain * exp(-beta * distance).
 * No Q-values or reuse model; standalone from Double-SARSA/DFS/BFS.
 */
#ifndef FASTBOTX_FRONTIER_AGENT_H
#define FASTBOTX_FRONTIER_AGENT_H

#include "AbstractAgent.h"

#include <vector>
#include <random>
#include <unordered_map>
#include <deque>
#include <cstddef>

namespace fastbotx {

    /**
     * A single frontier candidate: (source state, action) with scoring fields.
     */
    struct FrontierCandidate {
        StatePtr sourceState;
        ActivityStateActionPtr action;
        double infoGain = 0.0;
        int distance = 0;  // 0 for local frontier (current state only)
    };

    class FrontierAgent : public AbstractAgent {
    public:
        explicit FrontierAgent(const ModelPtr &model);

        ~FrontierAgent() override = default;

    protected:
        void updateStrategy() override;

        /**
         * Select new action: global planner picks best frontier (FH-DRL score),
         * then local execution returns the action (direct or first step of path).
         */
        ActionPtr selectNewAction() override;

        void moveForward(StatePtr nextState) override;

        /** Clear _outEdges and path when state abstraction (refine/coarsen) has changed. */
        void onStateAbstractionChanged() override;

    private:
        /**
         * Build candidate list: global (all states + BFS distance) when we have edges and >1 state,
         * else local (current state only, distance 0).
         */
        std::vector<FrontierCandidate> buildFrontierCandidates() const;

        /** Local-only candidate build (current state). */
        std::vector<FrontierCandidate> buildFrontierCandidatesLocal() const;

        /** Information gain for one candidate: unvisited = base + type weight, else decay by visit count. */
        double getInfoGain(const ActivityStateActionPtr &action) const;

        /** FH-DRL style: score = infoGain * exp(-beta * distance). */
        static double getScore(double infoGain, int distance);

        /** BFS from startHash using _outEdges; returns state hash -> distance. */
        std::unordered_map<uintptr_t, int> bfsDistances(uintptr_t startHash) const;

        /** Build path from current state (startHash) to targetHash; path = [(sourceHash, action), ...]. */
        bool buildPathTo(uintptr_t startHash, uintptr_t targetHash,
                         std::vector<std::pair<uintptr_t, ActivityStateActionPtr>> &path) const;

        /** Fallback when no frontier: random pick preferring low-visit / non-BACK. */
        ActionPtr fallbackPickAction() const;

        mutable std::mt19937 _rng{std::random_device{}()};

        /// Out-edges we observed: sourceHash -> [(action, targetHash)]
        std::unordered_map<uintptr_t, std::vector<std::pair<ActivityStateActionPtr, uintptr_t>>> _outEdges;
        /// Path to current target frontier state; each element (sourceHash, action to take)
        std::vector<std::pair<uintptr_t, ActivityStateActionPtr>> _pathToFrontier;
        size_t _pathIndex = 0;

        /// BFS distance cache: avoid recomputing when current state unchanged (e.g. following path)
        mutable std::unordered_map<uintptr_t, int> _cachedDistMap;
        mutable uintptr_t _cachedDistMapStartHash = 0;

        static constexpr int kBlockBackThreshold = 5;      // after N same-state steps, allow BACK if only option
        static constexpr int kBlockDeepLinkThreshold = 10; // after N steps, try DEEP_LINK to escape
        static constexpr int kBlockCleanRestartThreshold = 15; // after N steps, CLEAN_RESTART

        static constexpr double kBeta = 0.6;   // FH-DRL exp decay: score = infoGain * exp(-beta*d)
        static constexpr double kBaseInfoGain = 1.0;
        static constexpr int kMaxBFSDistance = 256;  // cap distance for scoring
    };

    using FrontierAgentPtr = std::shared_ptr<FrontierAgent>;

}  // namespace fastbotx

#endif  // FASTBOTX_FRONTIER_AGENT_H
