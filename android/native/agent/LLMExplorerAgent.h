/**
 * LLMExplorerAgent: knowledge-guided exploration with Abstract Interaction Graph (AIG).
 * Uses abstract states (rule-based from widget structure) and abstract actions with
 * exploration flags; app-wide action selection and fault-tolerant navigation.
 * See LLM_VLM_TRAVERSAL_TESTING_SOTA_AND_PROPOSAL.md §2.1 and §6.8.
 */
 /**
 * @authors Zhao Zhang
 */

#ifndef FASTBOTX_LLM_EXPLORER_AGENT_H
#define FASTBOTX_LLM_EXPLORER_AGENT_H

#include "AbstractAgent.h"

#include <memory>
#include <vector>
#include <random>
#include <unordered_map>
#include <unordered_set>
#include <deque>
#include <utility>
#include <cstddef>
#include <string>

namespace fastbotx {

    class IKnowledgeOrganizer;
    class IContentAwareInputProvider;

    /// Exploration flag for an abstract action (per LLM-Explorer paper).
    enum class LLMExplorerActionFlag : int {
        Unexplored = 0,
        Explored = 1,
        Ineffective = 2
    };

    class LLMExplorerAgent : public AbstractAgent {
    public:
        explicit LLMExplorerAgent(const ModelPtr &model);

        ~LLMExplorerAgent() override = default;

        /// Set custom knowledge organizer implementation (default: LlmKnowledgeOrganizer).
        void setKnowledgeOrganizer(const std::shared_ptr<IKnowledgeOrganizer> &organizer);

        /// Set custom content-aware input provider (default: LlmContentAwareInputProvider).
        void setContentAwareInputProvider(const std::shared_ptr<IContentAwareInputProvider> &provider);

    protected:
        void updateStrategy() override;

        ActionPtr selectNewAction() override;

        void moveForward(StatePtr nextState) override;

        void onStateAbstractionChanged() override;

        /**
         * Content-aware input text generation (LLM-Explorer paper §3.3.4).
         * When the chosen action targets an editable widget, call LLM to generate context-appropriate text.
         */
        std::string getInputTextForAction(const StatePtr &state, const ActionPtr &action) const override;

    private:
        /// Compute abstract state key from state (structural: activity + widget set under mask).
        /// Uses DefaultWidgetKeyMask (excludes Text/ContentDesc) when model available.
        uintptr_t computeAbstractStateKey(const StatePtr &state) const;

        /// Get or create abstract state id for the given key.
        uintptr_t getOrCreateAbstractStateId(uintptr_t key);

        /// Update knowledge after step (s', a', s): match states, update flags, add AIG edge.
        void updateKnowledge(const StatePtr &srcState, const ActivityStateActionPtr &actionTaken,
                            const StatePtr &tgtState);

        /// Ensure all actions in state are registered as abstract actions (unexplored if new).
        /// When LLM is available, optionally clusters same-function elements into abstract actions (paper §3.2.2).
        void ensureAbstractActionsForState(uintptr_t absStateId, const StatePtr &state);

        /// Try to run LLM knowledge organization for this state: group actions by function; fallback 1:1.
        /// Returns true if grouping was applied (LLM or fallback). Idempotent per absStateId.
        bool tryLlmKnowledgeOrganization(uintptr_t absStateId, const StatePtr &state);

        /// Select one unexplored abstract action: prefer current state, else app-wide random.
        /// Returns (abstractStateId where action is available, actionHash). actionHash may be 0 if none.
        std::pair<uintptr_t, uintptr_t> selectExploreAction(const StatePtr &state, uintptr_t currentAbsId);

        /// Find shortest path on AIG from currentAbsId to targetAbsId. Returns list of action hashes.
        /// If excludeEdgeKey != 0, that edge (e.g. kAigEdgeKey(src, actHash)) is skipped (for alternative path).
        bool findNavigatePath(uintptr_t currentAbsId, uintptr_t targetAbsId,
                             std::vector<uintptr_t> &outActionHashes,
                             uintptr_t excludeEdgeKey = 0);

        /// Find in state an action with the given hash.
        ActivityStateActionPtr findActionByHash(const StatePtr &state, uintptr_t actionHash) const;

        /// Remove one (absId, actHash) from _unexploredAbsIdActionPairs (for updateKnowledge).
        void removeUnexploredPair(uintptr_t absId, uintptr_t actHash);

        ActionPtr fallbackPickAction() const;

        mutable std::mt19937 _rng{std::random_device{}()};

        /// Abstract state: key (from computeAbstractStateKey) -> id
        std::unordered_map<uintptr_t, uintptr_t> _abstractKeyToId;
        uintptr_t _nextAbstractId = 1;

        /// (abstractStateId, actionHash) -> flag
        std::unordered_map<uintptr_t, LLMExplorerActionFlag> _actionFlags;
        static constexpr uintptr_t kActionFlagKey(uintptr_t absId, uintptr_t actHash) {
            return (absId << 32) | (actHash & 0xFFFFFFFFu);
        }

        /// AIG: from (abstractStateId, actionHash) -> next abstract state id
        std::unordered_map<uintptr_t, uintptr_t> _aigNextState;
        static constexpr uintptr_t kAigEdgeKey(uintptr_t absId, uintptr_t actHash) {
            return (absId << 32) | (actHash & 0xFFFFFFFFu);
        }

        /// For each abstract state, which action hashes are available (for path finding)
        std::unordered_map<uintptr_t, std::unordered_set<uintptr_t>> _absStateToActionHashes;

        /// LLM knowledge org (paper §3.2.2): (absStateId, actionHash) -> groupId; same group = same function.
        std::unordered_map<uintptr_t, uintptr_t> _actionToGroup;
        /// (absStateId, groupId) -> set of action hashes in that group (for marking whole group explored).
        static constexpr uintptr_t kGroupKey(uintptr_t absId, uintptr_t gid) {
            return (absId << 32) | (gid & 0xFFFFFFFFu);
        }
        std::unordered_map<uintptr_t, std::unordered_set<uintptr_t>> _groupToActionHashes;
        /// Paper §3.2.2 Function: (absStateId, groupId) -> short natural-language description of the group.
        std::unordered_map<uintptr_t, std::string> _groupFunction;
        /// States for which we have already run LLM grouping (or 1:1 fallback).
        std::unordered_set<uintptr_t> _llmGroupingDoneForState;
        /// App-wide unexplored: full (absId, actHash) for random pick (avoids truncating hash in key).
        std::vector<std::pair<uintptr_t, uintptr_t>> _unexploredAbsIdActionPairs;
        /// Nav-failed: kActionFlagKey(absId, actHash) to skip in app-wide selection (align with official repo nav_failed_actions).
        std::unordered_set<uintptr_t> _navFailedActionKeys;

        /// Pluggable implementations (default-constructed in ctor).
        std::shared_ptr<IKnowledgeOrganizer> _knowledgeOrganizer;
        std::shared_ptr<IContentAwareInputProvider> _contentAwareInputProvider;

        /// Navigation stack: action hashes to execute in order to reach target
        std::deque<uintptr_t> _navActionHashes;
        /// Target abstract state id for current nav (0 when not navigating). Used for UpdateNavigatePath and restart retry.
        uintptr_t _navTargetAbsId = 0;
        /// Action hash we want to execute on target state (for nav-failed marking when nav is abandoned).
        uintptr_t _navTargetActionHash = 0;
        /// Fault-tolerant: after path failure, retry from current (or after CLEAN_RESTART) before giving up.
        bool _navRetryAfterRestart = false;
        /// Edge that led to wrong state; removed from AIG when we give up after retries.
        uintptr_t _navFailedEdgeKey = 0;
        /// Number of CLEAN_RESTARTs done for this nav retry (capped by kMaxNavRetryRestarts).
        int _navRetryRestartCount = 0;
        /// Nav steps taken in current nav session; exceed kMaxNavStepsPerSession -> clear nav and mark target failed (official: MAX_NAVIGATE_NUM_AT_ONE_TIME=10).
        int _navStepsInSession = 0;

        static constexpr int kBlockBackThreshold = 5;
        static constexpr int kBlockDeepLinkThreshold = 10;
        static constexpr int kBlockCleanRestartThreshold = 15;
        static constexpr int kMaxBFSDepth = 256;
        /// Max restarts for nav retry before removing failed edge (paper: fault-tolerant navigation).
        static constexpr int kMaxNavRetryRestarts = 1;
        /// Max nav steps in one session before giving up and marking target as nav-failed (official: MAX_NAVIGATE_NUM_AT_ONE_TIME=10).
        static constexpr int kMaxNavStepsPerSession = 10;
        /// Min group size to apply LLM same-function grouping (official: MIN_SIZE_SAME_FUNCTION_ELEMENT_GROUP=5).
        static constexpr size_t kMinSameFunctionGroupSize = 5;
    };

    using LLMExplorerAgentPtr = std::shared_ptr<LLMExplorerAgent>;

}  // namespace fastbotx

#endif  // FASTBOTX_LLM_EXPLORER_AGENT_H
