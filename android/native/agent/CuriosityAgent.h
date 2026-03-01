/**
 * @authors Zhao Zhang
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
#ifndef FASTBOTX_CURIOSITY_AGENT_H
#define FASTBOTX_CURIOSITY_AGENT_H

#include "AbstractAgent.h"
#include "../desc/StateEncoder.h"

#include <vector>
#include <random>
#include <unordered_map>
#include <deque>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <unordered_set>

namespace fastbotx {

    class CuriosityAgent : public AbstractAgent {
    public:
        explicit CuriosityAgent(const ModelPtr &model);

        ~CuriosityAgent() override = default;

        /** Set optional state encoder (e.g. DNN); clustering uses encoder->getOutputDim(). When null, handcrafted 16-dim is used and _clusterDim=16. */
        void setStateEncoder(const IStateEncoderPtr &encoder);

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
        /// Smoothed global count for stochastic transitions (EMA); used when kEnableCountSmoothing.
        std::unordered_map<uintptr_t, double> _smoothedGlobalStateCount;
        /// Total selectNewAction calls (for ε decay)
        int _selectCount = 0;
        /// Total moveForward calls (for embedding log frequency control)
        int _moveForwardCount = 0;

        struct SuccessorStats {
            int total = 0;
            // Keep only top-K most frequent successor states to bound memory.
            std::vector<std::pair<uintptr_t, int>> topNext;
        };

        /// Per-action successor statistics (keyed by ActivityStateAction::hash()).
        std::unordered_map<uintptr_t, SuccessorStats> _succStats;

        /// Per-action self-loop count: how many times taking this action kept the same state hash.
        /// Keyed by ActivityStateAction::hash() to remain stable across state rebuilds.
        std::unordered_map<uintptr_t, int> _selfLoopCount;

        /// Bottleneck (3.1): state hash -> out-degree (number of actions). Used to down-weight actions
        /// that tend to lead to hub states (high out-degree); prefer actions leading to leaves.
        std::unordered_map<uintptr_t, int> _stateOutDegree;

        /// Sliding window of recent state hashes for path diversity (rolling window over last kPathWindow states).
        std::deque<uintptr_t> _recentStates;
        /// Rolling path signature -> visit count, for lightweight path-level diversity.
        std::unordered_map<std::uint64_t, int> _pathCount;
        /// Cached path signature from last moveForward (when path diversity on); invalidated when _recentStates cleared.
        std::uint64_t _lastPathSignature = 0;
        bool _lastPathSignatureValid = false;

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
        /// Precomputed episode mod table: episodeMod[n] = 1.0 / sqrt(1.0 + n) for n in [0, kEpisodeCap]
        /// Performance optimization: avoid std::sqrt call in hot path (selectNewAction)
        static constexpr double kEpisodeModTable[kEpisodeCap + 1] = {
            1.0000000000000000, 0.7071067811865476, 0.5773502691896258, 0.5000000000000000, 0.4472135954999580,
            0.4082482904638631, 0.3779644730092273, 0.3535533905932738, 0.3333333333333333, 0.3162277660168380,
            0.3015113445777636, 0.2886751345948129, 0.2773500981126146, 0.2672612419124244, 0.2581988897471611,
            0.2500000000000000, 0.2425356250363330, 0.2357022603955159, 0.2294157338705618, 0.2236067977499790,
            0.2182178902359924, 0.2132007163556105, 0.2085144140570748, 0.2041241452319315, 0.2000000000000000,
            0.1961161351381840, 0.1924500897298753, 0.1889822365046136, 0.1856953381770519, 0.1825741858350554,
            0.1796053020267749, 0.1767766952966369, 0.1740776559556978, 0.1714985851425088, 0.1690308509457033,
            0.1666666666666667, 0.1643989873053573, 0.1622214211307625, 0.1601281538050871, 0.1581138830084189,
            0.1561737618886061, 0.1543033499620919, 0.1524985703315367, 0.1507556722888818, 0.1490711984999859,
            0.1474419561548972, 0.1458649914978945, 0.1443375672974065, 0.1428571428571429, 0.1414213562373095,
            0.1400280084028010
        };
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
        /// Count smoothing for stochastic UI (transfer noise): use EMA of global count; default on (see CURIOSITY_ALGORITHM_EXPLANATION §2.4.5).
        static constexpr bool kEnableCountSmoothing = true;
        static constexpr double kSmoothBeta = 0.2;  // EMA: smoothed = (1-beta)*smoothed + beta*raw
        /// Curriculum: early bias global novelty, later bias episode (leave repeated states); disable by default.
        static constexpr bool kEnableCurriculum = false;
        static constexpr int kCurriculumSteps = 3000;       // progress = min(1, _selectCount / this)
        static constexpr double kCurriculumEarlyBlend = 0.5;  // early: blend episodeMod toward 1.0
        /// Self-loop penalty: if (state, action) causes self-loop at least this many times, down-weight its score.
        static constexpr int kSelfLoopPenaltyThreshold = 3;
        /// Multiplicative factor applied to scores of heavily self-looping actions.
        static constexpr double kSelfLoopPenaltyFactor = 0.2;

        /// Successor novelty: if an action tends to lead to globally over-visited states, down-weight it.
        /// This approximates "successor-state novelty" without training a successor representation.
        static constexpr int kSuccessorTopK = 4;
        static constexpr double kSuccessorMinFactor = 0.1;     // clamp successor factor to avoid vanishing scores
        static constexpr double kSuccessorAlpha = 1.0;         // exponent on successor factor (>=1 stronger)

        /// Bottleneck diversity (3.1, experimental): A/B tests showed coverage drop vs baseline; disable by default.
        /// When enabled, down-weight actions that tend to lead to hub states (high out-degree); prefer leaves.
        static constexpr bool kEnableBottleneckDiversity = false;
        static constexpr double kBottleneckMinFactor = 0.3;   // clamp bottleneck factor
        static constexpr int kBottleneckOutDegreeCap = 50;     // cap out-degree in factor to avoid overflow

        /// Path diversity (3.2, experimental): A/B tests showed coverage drop vs baseline; disable by default.
        /// Set to true to re-enable path-level diversity penalty.
        static constexpr bool kEnablePathDiversity = false;
        static constexpr int kPathWindow = 16;
        static constexpr int kPathMinLengthForPenalty = 6;
        static constexpr double kPathMinFactor = 0.3;          // clamp path factor to avoid vanishing scores
        static constexpr double kPathAlpha = 1.0;

        /// Long-horizon novelty: use graph structure to boost actions that tend to lead to "frontier" states
        /// (states with at least one unvisited action). No BFS distance; lightweight frontier set only.
        static constexpr bool kEnableLongHorizonNovelty = false;
        static constexpr double kLongHorizonFrontierBonus = 0.2;  // factor += bonus * min(frontierSuccessorCount, cap)
        static constexpr int kLongHorizonFrontierCap = 3;         // cap count for multiplier

        /// Cluster novelty (learned embedding + online clustering): state -> embedding -> nearest cluster;
        /// centroids updated by moving average; prefer actions leading to rarely-visited clusters.
        static constexpr bool kEnableClusterNovelty = true;
        static constexpr double kClusterMinFactor = 0.2;   // clamp cluster factor to avoid vanishing scores
        static constexpr int kClusterDim = 8;              // encoder embedding dim hint (Dnn=8). Clustering uses _clusterDim at runtime (handcrafted=16).
        static constexpr int kNumClusters = 32;            // number of clusters (online K-means style)
        static constexpr double kClusterCentroidAlpha = 0.08;  // moving-average rate for centroid update

        /// Embedding logging (low-frequency): print embedding summary to help debug clustering quality.
        static constexpr bool kEnableEmbeddingLog = true;
        static constexpr int kEmbeddingLogEvery = 200;  // log once per N moveForward calls
        static constexpr int kEmbeddingLogHead = 16;    // log all dimensions (16 for handcrafted, 8 for DNN)
        /// Runtime cluster dimension: 16 when using handcrafted only, or encoder->getOutputDim() when encoder is set.
        int _clusterDim = 16;
        /// State hash -> cluster index (0..kNumClusters-1).
        std::unordered_map<uintptr_t, int> _stateClusterIndex;
        /// Per-cluster visit count; size equals current number of initialized clusters (up to kNumClusters).
        std::vector<int> _clusterCount;
        /// Cluster centroids (learned); each row has _clusterDim elements.
        std::vector<std::vector<double>> _clusterCentroids;

        /// Optional encoder (DNN etc.); when set, used instead of hand-crafted embedding. Output size must equal _clusterDim.
        IStateEncoderPtr _stateEncoder;

        /** Compute state embedding: uses _stateEncoder if set, else hand-crafted. Size must equal _clusterDim for clustering. */
        std::vector<double> computeStateEmbedding(const StatePtr &state) const;
        /** Hand-crafted 16-dim embedding; used when _stateEncoder is null. */
        static std::vector<double> computeStateEmbeddingHandcrafted(const StatePtr &state);
        /** Assign state to nearest cluster and update centroid + count; returns cluster index. */
        int assignStateToCluster(uintptr_t stateHash, const std::vector<double> &embedding);
    };

    using CuriosityAgentPtr = std::shared_ptr<CuriosityAgent>;

}  // namespace fastbotx

#endif  // FASTBOTX_CURIOSITY_AGENT_H
