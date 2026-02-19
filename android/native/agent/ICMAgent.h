/*
 * This code is licensed under the Fastbot license. You may obtain a copy of this license in the LICENSE.txt file in the root directory of this source tree.
 */
/**
 * Curiosity-driven exploration agent (WebRLED-aligned).
 *
 * Aligns with WebRLED (arXiv:2504.19237) curiosity-driven reward model:
 * - Dual novelty: (1) Episode-internal novelty — prefer leaving states visited often in current episode.
 * - (2) Global novelty — prefer actions with low global visit count (getVisitedCount()).
 * - No DQN/grid; uses novelty as intrinsic score for action selection.
 * - Action selection: ε-greedy over curiosity score (with probability ε random, else greedy max score).
 * - Episode resets on CLEAN_RESTART (state-abstraction change clears episode counts).
 */
#ifndef FASTBOTX_ICM_AGENT_H
#define FASTBOTX_ICM_AGENT_H

#include "AbstractAgent.h"

#include <vector>
#include <random>
#include <unordered_map>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <utility>

namespace fastbotx {

    class ICMAgent : public AbstractAgent {
    public:
        explicit ICMAgent(const ModelPtr &model);

        ~ICMAgent() override = default;

    protected:
        void updateStrategy() override;

        /** Select action by curiosity score (dual novelty) + ε-greedy. */
        ActionPtr selectNewAction() override;

        void moveForward(StatePtr nextState) override;

        /** Clear episode counts when state abstraction (refine/coarsen) has changed. */
        void onStateAbstractionChanged() override;

    private:
        /** Curiosity score: globalNovelty * episodeMod * stateFactor (WebRLED §3.5: episodic = 1/√(1+n)). */
        double getCuriosityScore(const ActivityStateActionPtr &action, double episodeMod, double stateFactor) const;

        /** Fallback when no valid action: prefer low-visit, non-BACK, then random, then BACK. */
        ActionPtr fallbackPickAction() const;

        mutable std::mt19937 _rng{std::random_device{}()};

        /// Episode state visit count (reset on CLEAN_RESTART, onStateAbstractionChanged, max steps, or max states)
        std::unordered_map<uintptr_t, int> _episodeStateCount;
        /// Steps in current episode (for max-step truncation, align with WebRLED finite-length episode)
        int _episodeSteps = 0;
        /// Global state visit count (state-level novelty; cleared only on onStateAbstractionChanged)
        std::unordered_map<uintptr_t, int> _globalStateCount;
        /// Total selectNewAction calls (for ε decay)
        int _selectCount = 0;

        struct SuccessorStats {
            int total = 0;
            // Keep only top-K most frequent successor states to bound memory.
            std::vector<std::pair<uintptr_t, int>> topNext;
        };

        /// In/out degree per state hash, for lightweight bottleneck estimation.
        std::unordered_map<uintptr_t, int> _inDegree;
        std::unordered_map<uintptr_t, int> _outDegree;

        /// Per-action successor statistics (keyed by ActivityStateAction::hash()).
        std::unordered_map<uintptr_t, SuccessorStats> _succStats;

        /// Per-action self-loop count: how many times taking this action kept the same state hash.
        /// Keyed by ActivityStateAction::hash() to remain stable across state rebuilds.
        std::unordered_map<uintptr_t, int> _selfLoopCount;

        /// Recent state window for path diversity (rolling hash).
        std::deque<uintptr_t> _recentStates;
        std::unordered_map<std::uint64_t, int> _pathCount;

        static constexpr int kBlockBackThreshold = 5;
        static constexpr int kBlockDeepLinkThreshold = 10;
        static constexpr int kBlockCleanRestartThreshold = 15;

        /// ε-greedy: initial and min (decay from initial to min over kEpsilonDecaySteps); 0.4 aligns with WebRLED §3.6.
        /// NOTE: use a smaller decay horizon than WebRLED (kEpsilonDecaySteps=3000) to speed up convergence to greedy in UI testing.
        static constexpr double kEpsilonInitial = 0.4;
        static constexpr double kEpsilonMin = 0.05;
        static constexpr int kEpsilonDecaySteps = 3000;
        /// Episode novelty: 1/√(1+episodeCount) per WebRLED §3.5; cap count for numerical stability
        static constexpr int kEpisodeCap = 50;  // cap n in 1/sqrt(1+n) to avoid near-zero mod
        /// Max reward scaling for multiplicative combination (WebRLED L=5)
        static constexpr double kRewardCap = 5.0;
        /// Max steps per episode before resetting episode counts (finite-length episode).
        /// UI testing benefits from slightly shorter episodes to reduce long local plateaus.
        static constexpr int kMaxEpisodeSteps = 300;
        /// Max distinct states per episode before reset (memory cap)
        static constexpr size_t kMaxEpisodeStateCount = 2000;
        /// Global state factor: stateFactor = 1 + kGlobalStateBonus * min(globalStateCount, kGlobalStateCap), encourage leaving globally over-visited state
        static constexpr double kGlobalStateBonus = 0.15;
        static constexpr int kGlobalStateCap = 20;
        /// Self-loop penalty: if (state, action) causes self-loop at least this many times, down-weight its score.
        static constexpr int kSelfLoopPenaltyThreshold = 3;
        /// Multiplicative factor applied to scores of heavily self-looping actions.
        static constexpr double kSelfLoopPenaltyFactor = 0.2;

        /// Successor novelty: if an action tends to lead to globally over-visited states, down-weight it.
        /// This approximates "successor-state novelty" without training a successor representation.
        static constexpr int kSuccessorTopK = 4;
        static constexpr double kSuccessorMinFactor = 0.1;     // clamp successor factor to avoid vanishing scores
        static constexpr double kSuccessorAlpha = 1.0;         // exponent on successor factor (>=1 stronger)

        /// Bottleneck (structure-aware) factor: encourage states that are "junctions" in the graph.
        /// Approximate score(s) = log(1+inDegree)*log(1+outDegree)/(1+globalCount), then fold into stateFactor.
        static constexpr double kBottleneckBeta = 0.1;

        /// Path diversity: rolling window over recent state hashes, penalize over-used paths.
        static constexpr int kPathWindowSize = 16;
        static constexpr int kPathPenaltyThreshold = 3;
        static constexpr double kPathPenaltyFactor = 0.5;
    };

    using ICMAgentPtr = std::shared_ptr<ICMAgent>;

}  // namespace fastbotx

#endif  // FASTBOTX_ICM_AGENT_H
