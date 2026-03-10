/**
 * @authors Zhao Zhang
 */
/**
 * Standalone Go-Explore style agent: archive of cells, return to cell (via BFS path), then explore from cell.
 * Does not combine with DoubleSarsa, Frontier, or other agents.
 * See GO_EXPLORE_AGENT_DESIGN.md and GO_EXPLORE_TECHNICAL_PROPOSAL.md.
 */
#ifndef FASTBOTX_GO_EXPLORE_AGENT_H
#define FASTBOTX_GO_EXPLORE_AGENT_H

#include "AbstractAgent.h"

#include <vector>
#include <random>
#include <unordered_map>
#include <deque>
#include <cstddef>

namespace fastbotx {

    /**
     * Metadata for one cell in the archive (cell_id = state hash).
     * Used to weight "which cell to return to and explore from".
     */
    struct CellMeta {
        int seenTimes = 0;   // number of times we have reached this cell
        int chosenTimes = 0; // number of times this cell was chosen as explore target
    };

    class GOExploreAgent : public AbstractAgent {
    public:
        explicit GOExploreAgent(const ModelPtr &model);

        ~GOExploreAgent() override = default;

    protected:
        void updateStrategy() override;

        /**
         * Select action: either follow path to cell, or explore from cell (N steps), or choose new cell and plan.
         */
        ActionPtr selectNewAction() override;

        void moveForward(StatePtr nextState) override;

        void onStateAbstractionChanged() override;

    private:
        /** Ensure cell is in archive and increment seen count (call from moveForward when landing on nextState). */
        void ensureArchiveAndSeen(uintptr_t stateHash);

        /** Ensure cell is in archive without incrementing seen (call before chooseCellFromArchive so current is a candidate). */
        void ensureInArchiveOnly(uintptr_t stateHash);

        /**
         * Choose one cell from archive by weighted random (prefer less seen/chosen).
         * Optionally bias by BFS distance from current state (if distToCurrent non-null).
         * Returns chosen state hash or 0 if none.
         */
        uintptr_t chooseCellFromArchive(const std::unordered_map<uintptr_t, int> *distToCurrent) const;

        /** BFS from startHash on _outEdges; returns state hash -> distance. */
        std::unordered_map<uintptr_t, int> bfsDistances(uintptr_t startHash) const;

        /** Build path from startHash to targetHash; path = [(sourceHash, action), ...]. Returns true if path exists. */
        bool buildPathTo(uintptr_t startHash, uintptr_t targetHash,
                         std::vector<std::pair<uintptr_t, ActivityStateActionPtr>> &path) const;

        /** Pick one action for "exploration" at current state (e.g. random or prefer unvisited). */
        ActivityStateActionPtr pickExploreAction() const;

        /** Fallback when no path / no candidates. */
        ActionPtr fallbackPickAction() const;

        mutable std::mt19937 _rng{std::random_device{}()};

        /// Archive: cell_id (state hash) -> metadata
        std::unordered_map<uintptr_t, CellMeta> _archive;
        /// Observed edges: sourceHash -> [(action, targetHash)]
        std::unordered_map<uintptr_t, std::vector<std::pair<ActivityStateActionPtr, uintptr_t>>> _outEdges;
        /// Path to current target cell
        std::vector<std::pair<uintptr_t, ActivityStateActionPtr>> _pathToCell;
        size_t _pathIndex = 0;
        /// After reaching target cell, explore for this many steps
        uintptr_t _exploreTargetCell = 0;
        int _exploreStepsLeft = 0;

        static constexpr int kBlockBackThreshold = 5;
        static constexpr int kBlockDeepLinkThreshold = 10;
        static constexpr int kBlockCleanRestartThreshold = 15;
        static constexpr int kExploreStepsPerCell = 10;
        static constexpr int kMaxBFSDistance = 256;
        /// Distance weight for cell selection (encourage occasionally choosing farther cells).
        static constexpr double kDistanceAlpha = 0.1;
        static constexpr int kMaxDistanceForWeight = 32;
        /// Probability of picking random action during exploration (else prefer unvisited/low-visit)
        static constexpr double kExploreRandomEpsilon = 0.15;
    };

    using GOExploreAgentPtr = std::shared_ptr<GOExploreAgent>;

}  // namespace fastbotx

#endif  // FASTBOTX_GO_EXPLORE_AGENT_H
