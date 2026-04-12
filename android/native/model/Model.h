/*
 * This code is licensed under the Fastbot license. You may obtain a copy of this license in the LICENSE.txt file in the root directory of this source tree.
 */
/**
 * @authors Jianqiang Guo, Yuhui Su, Zhao Zhang
 */
#ifndef  Model_H_
#define  Model_H_

#include <memory>
#include "Base.h"
#include "State.h"
#include "../desc/naming/StateKey.h"
#include "../desc/naming/StateNamingManager.h"
#include "Element.h"
#include "Action.h"
#include "Graph.h"
#include "AbstractAgent.h"
#include "AgentFactory.h"
#include "Preference.h"
#include "agent/LLMTaskAgent.h"
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <array>
#include <utility>
#include <vector>
#include <set>
#ifndef NDEBUG
#include <thread>
#endif

namespace fastbotx {

namespace gui_tree {
    class GUITreeNode;
}

#if DYNAMIC_STATE_ABSTRACTION_ENABLED
    /// APE Naming lattice: transition log keyed by StateKey::hash().
    struct ApeTransitionEntry {
        uintptr_t sourceKeyHash{0};
        bool hasSourceStateKey{false};
        naming::StateKey sourceStateKey = naming::StateKey::fromFallbackXmlStringHash("", 0);
        uintptr_t actionHash{0};
        uintptr_t targetKeyHash{0};
        std::string sourceActivity;
        /// State hashes (Graph/RL identity) for transition replay/remap when Naming changes.
        uintptr_t sourceStateHash{0};
        uintptr_t targetStateHash{0};
        /// Action signature for replay/remap. actionHash itself may change after Naming updates.
        ActionType actionType{ActionType::NOP};
        bool hasTargetBounds{false};
        Rect targetBounds{};
        /// Stable concrete target widget identity (APE trace field `full`).
        bool hasTargetFullPath{false};
        uintptr_t targetFullPathHash{0};
        /// Optional exact target StateKey for collision defense / debugging.
        bool hasTargetStateKey{false};
        naming::StateKey targetStateKey = naming::StateKey::fromFallbackXmlStringHash("", 0);
        bool valid{false};
    };

    /// Per-activity Naming refine/coarsen (L′→L split on StateKey hashes, mirror ActivityAbstractionContext).
    struct ApeNamingAbstractionContext {
        naming::NamingPtr previousNamingBeforeRefine;
        std::string previousNamingFingerprintBeforeRefine;
        std::unordered_map<uintptr_t, std::unordered_set<uintptr_t>> oldKeyHashToNewKeyHashes;
        // Approximate APE batchAbstract "affected states" count per parent-key bucket.
        std::unordered_map<uintptr_t, size_t> oldKeyHashToObservationCount;
        size_t stateCountAtLastNamingRefinement{0};
        int nonDetPairsAtLastNamingRefinement{0};
        // Pair-driven refine/coarsen context (Java resolveNonDeterminism / batchAbstract style)
        uintptr_t triggerSourceKeyHash{0};        // XML-space hash (for coarsen gate alignment)
        uintptr_t triggerSourceKeyHashOriginal{0}; // Element-space hash (for blacklist lookup)
        bool triggerSourceKeyExact{false};
        naming::StateKey triggerSourceKey = naming::StateKey::fromFallbackXmlStringHash("", 0);
        uintptr_t triggerActionHash{0};
        std::unordered_set<uintptr_t> triggerTargetKeyHashes;
        size_t triggerTargetCountAtRefine{0};
    };

    struct ApePairKey {
        uintptr_t sourceKeyHash{0};
        uintptr_t actionHash{0};

        bool operator==(const ApePairKey &other) const {
            return sourceKeyHash == other.sourceKeyHash && actionHash == other.actionHash;
        }
    };

    struct ApePairKeyHash {
        size_t operator()(const ApePairKey &k) const {
            const size_t h1 = std::hash<uintptr_t>{}(k.sourceKeyHash);
            const size_t h2 = std::hash<uintptr_t>{}(k.actionHash);
            return h1 ^ (h2 + 0x9e3779b97f4a7c15ULL + (h1 << 6) + (h1 >> 2));
        }
    };

    /** Hybrid counts keyed by target StateKey hash: vector for tiny fan-out, map when large. */
    struct ApeTargetCounts {
        static constexpr size_t kSmallMax = 6;

        size_t size() const { return usingBig ? big.size() : small.size(); }
        bool empty() const { return size() == 0; }

        template <class F>
        void forEach(F &&f) const {
            if (usingBig) {
                for (const auto &kv : big) {
                    f(kv.first, kv.second);
                }
            } else {
                for (const auto &kv : small) {
                    f(kv.first, kv.second);
                }
            }
        }

        void increment(uintptr_t key) {
            if (usingBig) {
                big[key]++;
                return;
            }
            for (auto &kv : small) {
                if (kv.first == key) {
                    kv.second++;
                    return;
                }
            }
            small.emplace_back(key, 1);
            if (small.size() > kSmallMax) {
                promoteToBig();
            }
        }

        bool decrement(uintptr_t key) {
            if (usingBig) {
                auto it = big.find(key);
                if (it == big.end()) {
                    return false;
                }
                --(it->second);
                if (it->second <= 0) {
                    big.erase(it);
                }
                if (big.empty()) {
                    usingBig = false;
                }
                return true;
            }
            for (size_t i = 0; i < small.size(); ++i) {
                if (small[i].first != key) {
                    continue;
                }
                --small[i].second;
                if (small[i].second <= 0) {
                    small[i] = small.back();
                    small.pop_back();
                }
                return true;
            }
            return false;
        }

    private:
        void promoteToBig() {
            usingBig = true;
            const size_t cap = (small.size() * 2 > 16) ? (small.size() * 2) : 16;
            big.reserve(cap);
            for (const auto &kv : small) {
                big[kv.first] += kv.second;
            }
            small.clear();
        }

        std::vector<std::pair<uintptr_t, int>> small;
        std::unordered_map<uintptr_t, int> big;
        bool usingBig{false};
    };

    struct ApePairAggValue {
        ApeTargetCounts targetCounts;
        std::string sourceActivity;
        bool hasSourceStateKey{false};
        naming::StateKey sourceStateKey = naming::StateKey::fromFallbackXmlStringHash("", 0);
    };

    /// APE EvidencePool sample for one (sourceKeyHash, actionSignature) pair.
    /// In native we use ApeTransitionEntry's concrete fields to approximate Java's evidence.
    struct ApeEvidenceSample {
        uintptr_t sourceStateHash{0};
        uintptr_t targetStateHash{0};
        uintptr_t targetKeyHash{0};
        ActionType actionType{ActionType::NOP};
        bool hasTargetBounds{false};
        Rect targetBounds{};
        bool hasTargetFullPath{false};
        uintptr_t targetFullPathHash{0};
        bool valid{false};
    };

    /// Fixed-capacity circular evidence pool per (sourceKeyHash, actionSignature).
    struct ApeEvidencePool {
        static constexpr size_t kCapacity = 8;

        template <class T>
        void push(T &&s, uint64_t epoch) {
            lastTouchEpoch = epoch;
            s.valid = true;
            samples[writeIndex] = std::move(s);
            writeIndex = (writeIndex + 1) % kCapacity;
            if (poolSize < kCapacity) {
                ++poolSize;
            }
        }

        template <class F>
        void forEach(F &&f) const {
            for (size_t i = 0; i < kCapacity; ++i) {
                const ApeEvidenceSample &s = samples[i];
                if (!s.valid) {
                    continue;
                }
                f(s);
            }
        }

        size_t size() const { return poolSize; }

        static_assert(kCapacity > 0, "kCapacity must be positive");

        uint64_t lastTouchEpoch{0};

    private:
        std::array<ApeEvidenceSample, kCapacity> samples{};
        size_t poolSize{0};
        size_t writeIndex{0};
    };

    struct ApeEvidencePoolClockEntry {
        ApePairKey key{};
        uint64_t epoch{0};
    };
#endif

    /**
     * @brief Constants namespace for model-related constants
     */
    namespace ModelConstants {
        /// Default device ID used when no device ID is specified
        constexpr const char* DefaultDeviceID = "0000001";
    }

    /**
     * @brief Model class representing the core RL (Reinforcement Learning) model
     * 
     * The Model class is the central component that:
     * - Manages the state-action graph
     * - Coordinates agents for different devices
     * - Handles action selection and state management
     * - Provides the main interface for getting next operations
     * 
     * It uses shared_from_this to allow agents to hold references to the model.
     */
    class Model : public std::enable_shared_from_this<Model> {
    public:
        /**
         * @brief Factory method to create a new Model instance
         * 
         * @return Shared pointer to a new Model object
         */
        static std::shared_ptr<Model> create();

        /**
         * @brief Get the number of states in the graph
         * 
         * @return Number of unique states in the graph
         */
        inline size_t stateSize() const { return this->getGraph()->stateSize(); }

        /**
         * @brief Get the graph object
         * 
         * @return Const reference to the graph object
         */
        GraphPtr getGraph() { return this->_graph; }
        const GraphPtr &getGraph() const { return this->_graph; }

        /**
         * @brief Create and add an agent to the model for a specific device
         * 
         * Creates a new agent, adds it to the device-agent map, and registers it
         * as a listener to the graph for state change notifications.
         * 
         * @param deviceIDString Device ID string (empty string uses default device ID)
         * @param agentType The type of algorithm/agent to create
         * @param deviceType The type of device (default: Normal)
         * @return Shared pointer to the newly created agent
         */
        AbstractAgentPtr addAgent(const std::string &deviceIDString, AlgorithmType agentType,
                                  DeviceType deviceType = DeviceType::Normal);
        /**
         * @brief LLMDroid compatibility overload.
         *
         * The current architecture controls code coverage dynamically via Java/JNI side.
         * Keep this signature for source compatibility but ignore useCodeCoverage.
         */
        AbstractAgentPtr addAgent(const std::string &deviceIDString, AlgorithmType agentType,
                                  bool useCodeCoverage,
                                  DeviceType deviceType = DeviceType::Normal);

        /**
         * @brief Get the agent for a specific device
         * 
         * @param deviceID Device ID string (empty string uses default device ID)
         * @return Shared pointer to the agent, or nullptr if not found
         */
        AbstractAgentPtr getAgent(const std::string &deviceID) const;

        /**
         * @brief Get next operation step from XML string, returning JSON format
         * 
         * This is the main entry point that accepts XML content as a string.
         * Parses the XML and delegates to the ElementPtr-based version.
         * 
         * @param descContent XML content of the current page as a string
         * @param activity Activity name string
         * @param deviceID Device ID string (default: empty string uses default device)
         * @return Next operation step in JSON format
         */
        std::string getOperate(const std::string &descContent, const std::string &activity,
                               const std::string &deviceID = "");

        /**
         * @brief Get next operation step from Element object, returning JSON format
         * 
         * This method wraps getOperateOpt() and converts the result to JSON string.
         */
        std::string getOperate(const ElementPtr &element, const std::string &activity,
                               const std::string &deviceID = "");

        /**
         * @brief Core method for getting next operation and updating RL model
         * 
         * Image for LLM is obtained in Java on demand when native triggers HTTP (no screenshot param).
         */
        OperatePtr getOperateOpt(const ElementPtr &element, const std::string &activity,
                                 const std::string &deviceID = "");

        /** Event-driven precondition page report: build state and forward to agent guide logic. */
        void addCurrentPageAsPrecondition(const ElementPtr &element, const std::string &activity,
                                          const std::string &deviceID = "");

        /**
         * @brief Get the preference object
         * 
         * @return Shared pointer to the preference object
         */
        PreferencePtr getPreference() const { return this->_preference; }

        /**
         * @brief Get the shared LLM client (if any) used by LLMTaskAgent.
         * Other agents (e.g. LLMExplorerAgent) may use it for content-aware input or knowledge org.
         */
        std::shared_ptr<LlmClient> getLlmClient() const;

        /**
         * @brief Get widget key mask for an activity (dynamic state abstraction).
         * Returns DefaultWidgetKeyMask if activity not found.
         */
        WidgetKeyMask getActivityKeyMask(const std::string &activity) const;

        /**
         * @brief Set widget key mask for an activity (dynamic state abstraction).
         */
        void setActivityKeyMask(const std::string &activity, WidgetKeyMask mask);

        /**
         * @brief Set the package name for network action parameters
         * 
         * @param packageName The package name string
         */
        void setPackageName(const std::string &packageName) { 
            this->_netActionParam.packageName = packageName; 
        }

        /**
         * @brief Get the package name
         * 
         * @return Const reference to the package name string
         */
        const std::string &getPackageName() const { return this->_netActionParam.packageName; }

        /**
         * @brief Get the network action task ID
         * 
         * @return Network action task ID
         */
        int getNetActionTaskID() const { return this->_netActionParam.netActionTaskid; }

        /**
         * @brief Report current activity for coverage tracking (performance: coverage in C++, PERF §3.4)
         */
        void reportActivity(const std::string &activity);

        /**
         * @brief Get coverage summary as JSON: {"stepsCount":N,"testedActivities":["a1",...]}
         */
        std::string getCoverageJson() const;

        /**
         * Scalar for LLMDroid CodeCoverageMonitor / stagnation: RL graph stateSize, distinct
         * activities from reportActivity, and step count. Java: AiClient.getLlmdroidCoverageMetric /
         * com.android.commands.monkey.utils.CodeCoverage.getCoverage.
         */
        double getLlmdroidStagnationMetric() const;

        /**
         * @brief Load persisted dynamic state abstraction policy metadata for the current package (if enabled).
         *
         * Policy file path (per package):
         *   /sdcard/fastbot_{packageName}.statekey.json
         *
         * v1 files may list legacy widget-key masks and coarseningBlacklist; they are ignored (APE dynamic
         * identity does not use mask refinement). v2 writes an empty activities array only.
         */
        void loadStateAbstractionPolicy();

        /**
         * @brief Save dynamic state abstraction policy stub for the current package (if enabled).
         *
         * Writes version 2 JSON (no per-activity widget masks). Safe to call multiple times.
         */
        void saveStateAbstractionPolicy() const;

        /**
         * @brief Optional APE bridge: store StateKey alongside an RL state (indexed by state->hash()).
         * Call after the state is in the graph (e.g. after createAndAddState) when GUITree + Naming produced a key.
         */
        void recordApeStateKey(const StatePtr &state, const naming::StateKey &key);

        /** Lookup a previously recorded APE StateKey by state hash (returns false if none). */
        bool tryGetApeStateKey(uintptr_t stateHash, naming::StateKey *out) const;

        /** Hash-only lookup for previously recorded APE StateKey. */
        bool tryGetApeStateKeyHash(uintptr_t stateHash, uintptr_t *outKeyHash) const;

#if DYNAMIC_STATE_ABSTRACTION_ENABLED
        /**
         * When APE Naming changes, Model prunes stale states from Graph. This returns the action hashes
         * (ActivityNameAction::hash in dynamic mode) collected from those pruned states so agents can
         * invalidate hash-keyed caches in a scoped way. Valid during the call stack of
         * Model::notifyAgentsOfApeNamingChange().
         */
        const std::unordered_set<uintptr_t> &getPendingInvalidatedReuseActionHashes() const {
            return _apeInvalidatedReuseActionHashes;
        }
#else
        const std::unordered_set<uintptr_t> &getPendingInvalidatedReuseActionHashes() const {
            static const std::unordered_set<uintptr_t> kEmpty;
            return kEmpty;
        }
#endif

        virtual ~Model();

    protected:
        Model();

    private:
        enum class ApeStateKeyBuildFailReason : uint8_t {
            None = 0,
            NullInput,
            BuildTreeOrDomFailed,
            NoNaming,
            RebuildTreeFailed,
        };

        struct ApeGraphStateKeyDedupEntry {
            naming::StateKey key;
            StatePtr state;
        };

        struct ApeCorrectnessCounters {
            uint64_t statekey_build_ok{0};
            uint64_t statekey_build_fail{0};
            uint64_t statekey_fallback_used{0};

            uint64_t statekey_fail_null_input{0};
            uint64_t statekey_fail_build_tree_dom{0};
            uint64_t statekey_fail_no_naming{0};
            uint64_t statekey_fail_rebuild_tree{0};

            uint64_t statekey_record_hash_collision{0};

            uint64_t graph_dedup_hash_hit{0};
            uint64_t graph_dedup_exact_hit{0};
            uint64_t graph_dedup_hash_collision{0};

            uint64_t naming_update_by_hash{0};

            uint64_t evidence_pool_sample_add{0};
            uint64_t evidence_pool_new_pair{0};
            uint64_t evidence_pool_evict{0};
        };

        /**
         * @brief Get custom action from preference if one exists for this page
         * 
         * @param activity Activity name string
         * @param element XML Element object of the current page
         * @return Custom action if exists, nullptr otherwise
         */
        /**
         * @brief Get or create an activity string pointer (memory optimization)
         * 
         * Reuses existing activity string pointers from the graph to avoid duplication.
         * 
         * @param activity The activity name string
         * @return Shared pointer to the activity string (cached or newly created)
         */
        stringPtr getOrCreateActivityPtr(const std::string &activity);
        
        /**
         * @brief Get or create an agent for the given device ID
         * 
         * Returns existing agent or default agent if device ID not found.
         * Creates default agent if no agents exist.
         * 
         * @param deviceID Device ID string (empty string uses default device ID)
         * @return Shared pointer to the agent
         */
        AbstractAgentPtr getOrCreateAgent(const std::string &deviceID);
        
        /**
         * @brief Build a state from element without adding to the graph (for moveForward-before-addState flow).
         * 
         * @param element XML Element object of the current page
         * @param agent The agent to use for state creation
         * @param activityPtr Shared pointer to activity name string
         * @return Shared pointer to the created state (not yet in graph)
         */
        StatePtr buildStateOnly(const ElementPtr &element, const AbstractAgentPtr &agent,
                               const stringPtr &activityPtr);

        /**
         * @brief Create a new state from element and add it to the graph
         * 
         * @param element XML Element object of the current page
         * @param agent The agent to use for state creation
         * @param activityPtr Shared pointer to activity name string
         * @return Shared pointer to the created/existing state
         */
        StatePtr createAndAddState(const ElementPtr &element, const AbstractAgentPtr &agent, 
                                   const stringPtr &activityPtr);
        
        /**
         * @brief Select an action based on state, agent, and custom preferences
         * 
         * @param state The current state (may be modified)
         * @param agent The agent to use for action selection (may be modified)
         * @param customAction Custom action from preference, if any
         * @param actionCost Output parameter: time cost for action generation in seconds
         * @return Selected action, or nullptr if selection failed
         */
        ActionPtr selectAction(StatePtr &state, AbstractAgentPtr &agent, ActionPtr customAction, double &actionCost);
        
        /**
         * @brief Convert an action to an operate object and apply patches
         * 
         * @param action The action to convert
         * @param state The current state (used for detail clearing optimization)
         * @return OperatePtr The operation object ready for execution
         */
        OperatePtr convertActionToOperate(ActionPtr action, StatePtr state);

#if defined(FASTBOT_HAS_PUGIXML) && FASTBOT_HAS_PUGIXML
        /** Build GUITree (+ dom for XPath); set outKey from Naming + tree. Returns false on failure.
         *  When @p stateForDynamicApply is non-null (dynamic RL identity), applies APE action hashes
         *  while the GUITree is still alive — must not defer to after return (node pointers invalid). */
        bool buildApeStateKeyFromElementTree(const ElementPtr &element, const std::string &activity,
                                             naming::StateKey *outKey,
                                             ApeStateKeyBuildFailReason *outFailReason = nullptr,
                                             const StatePtr &stateForDynamicApply = StatePtr(),
                                             std::string *ioXmlCache = nullptr);
#endif

#if DYNAMIC_STATE_ABSTRACTION_ENABLED
        static void logApeStateKeySnapshot(const std::string &rawActivity, const StatePtr &state,
                                           const naming::StateKey &key, const GraphPtr &graph);

        /// Record one transition for APE naming non-determinism detection (StateKey sidecars).
        void recordTransition(const AbstractAgentPtr &agent, const StatePtr &targetState);
        /// Run APE naming refinement batch if step count reached interval
        void runRefinementAndCoarseningIfScheduled();
        /// APE Naming lattice: record transition when both ends have StateKey sidecars
        void recordApeTransitionForAbstraction(const StatePtr &src, const StatePtr &tgt,
                                               const ActivityStateActionPtr &act);
        std::vector<std::string> detectNonDeterminismApe() const;
        struct ApeRefinePair {
            uintptr_t sourceKeyHash{0};
            bool hasSourceStateKey{false};
            naming::StateKey sourceStateKey = naming::StateKey::fromFallbackXmlStringHash("", 0);
            uintptr_t actionHash{0};
            std::unordered_set<uintptr_t> targetKeyHashes;
            size_t targetCount{0};
        };
        bool refineActivityApeNaming(const std::string &activity);
        /// @param precomputedActivityNonDetPairCount if >= 0 (from batch collectNonDetPairs), skip per-activity log scan
        bool refineActivityApeNaming(const std::string &activity, const ApeRefinePair *pair,
                                     int precomputedActivityNonDetPairCount = -1);
        bool coarsenActivityApeNamingIfNeeded(const std::string &activity);
        /// @return true if any refine success or coarsen rollback ran in this batch (for agent notify).
        bool runApeNamingAbstractionBatch();
        /// After APE Naming changes, StateKey::hash() space shifts; stale entries must not dedup new visits.
        void invalidateApeGraphStateKeyDedupMap();

        /**
         * Rebuild a small set of representative states for old StateKey hashes (budget Model.rebuild analogue).
         * Keeps concrete samples in the new naming space for future refinement/rollback.
         */
        void rebuildApeStateRepresentativesForKeyHashes(
            const std::string &rawActivity,
            const naming::NamingPtr &oldNaming,
            const std::vector<uintptr_t> &oldKeyHashes,
            size_t maxStatesPerKeyHash);

        /**
         * Remap transition evidence in the ring buffer from @p fromNaming to @p toNaming,
         * preserving non-determinism statistics instead of clearing aggregation.
         */
        void remapApeTransitionAggregationForActivity(
            const std::string &rawActivity,
            const naming::NamingPtr &fromNaming,
            const naming::NamingPtr &toNaming,
            const std::unordered_set<uintptr_t> *focusOldKeyHashes = nullptr);

        /**
         * APE AssertSourceDivergent: ordered partitions of graph state hashes (keys into _apeStateXmlByStateHash).
         * eval rejects naming if a StateKey::hash() under naming appears in more than one partition sweep
         * (same semantics as NamingFactory.AssertSourceDivergent).
         */
        struct ApeSourcePartitionPredicate {
            std::string activityKey;
            std::string updatedNamingFingerprint;
            std::vector<std::vector<uintptr_t>> partitions;
        };
        bool evalApeSourcePartitionPredicates(const std::string &activity,
                                              const naming::NamingPtr &naming) const;
        void pushApeSourcePartitionPredicate(const std::string &activity,
                                            const naming::NamingPtr &updatedNaming,
                                            std::vector<std::vector<uintptr_t>> partitions);
        void pruneApeSourcePartitionPredicates(const std::string &activity,
                                               const naming::NamingPtr &namingPrev,
                                               const naming::NamingPtr &namingCur,
                                               const std::unordered_set<uintptr_t> &affectedStateHashes);

        /**
         * APE AssertActionDivergent / AssertActionDivergent2: partitions of (graph state hash, preorder index)
         * into cached XML; after rebuildTree(naming), action widget Name::toXPath() must not repeat across
         * partitions (first occurrence per partition only, same as Java HashSet temp/actions logic).
         */
        struct ApeActionPartitionPredicate {
            std::string activityKey;
            std::string updatedNamingFingerprint;
            std::vector<std::vector<std::pair<uintptr_t, size_t>>> partitions;
        };
        bool evalApeActionPartitionPredicates(const std::string &activity,
                                             const naming::NamingPtr &naming) const;
        void pushApeActionPartitionPredicate(const std::string &activity,
                                            const naming::NamingPtr &updatedNaming,
                                            std::vector<std::vector<std::pair<uintptr_t, size_t>>> partitions);
        void pruneApeActionPartitionPredicates(const std::string &activity,
                                              const naming::NamingPtr &namingPrev,
                                              const naming::NamingPtr &namingCur,
                                              const std::unordered_set<uintptr_t> &affectedStateHashes);
        /// Drop APE transition log + pair-agg rows for @p actKeyCanonical (Java rebuild clears stale hash space).
        void apeClearTransitionAggregationForActivity(const std::string &actKeyCanonical);
        void notifyAgentsOfApeNamingChange();

        /// Count of distinct APE StateKeys recorded for an activity under a specific Naming fingerprint.
        ///
        /// This helps keep refinement gates (e.g. minStates/minStateDelta) aligned with the *current* naming,
        /// instead of being biased by historical states created under older naming versions.
        size_t getApeStateCountByActivityAndNamingFingerprint(
            const std::string &activityKeyCanonical, const std::string &namingFingerprint) const;

        /// Drop stale states created under a previous naming fingerprint for the given activity.
        ///
        /// This is a lightweight alternative to a full Java-style Model.rebuild(): we remove states/actions
        /// created under an old naming key space so that agents and naming heuristics see a closer-to-consistent
        /// abstract graph.
        void pruneStaleApeStatesForActivity(const std::string &activityKeyCanonical,
                                            const std::string &staleNamingFingerprint,
                                            const std::unordered_set<uintptr_t> *affectedStateHashes = nullptr);

        /// Java NamingFactory.guiTreeNamingBlaclist: reject candidate if fingerprint is banned for any affected
        /// graph state (source-side + one repr per ND target key, see refineActivityApeNaming).
        bool evalApeGuiTreeNamingBlacklist(const std::vector<uintptr_t> &stateHashes,
                                           const naming::NamingPtr &naming) const;
        void apeBlacklistFinerNamingOnRollback(
            const std::string &activity, const naming::NamingPtr &finerNaming,
            const ApeNamingAbstractionContext &ctx, const std::unordered_set<uintptr_t> &affectedStateHashesForBlacklist);
        /// Java AssertStatesFewerThan (STATE_ABSTRACTION): distinct StateKeys under naming across trees <= threshold.
        struct ApeStatesFewerThanPredicate {
            std::string activityKey;
            std::string updatedNamingFingerprint;
            int threshold{8};
            std::vector<uintptr_t> stateHashes;
        };
        bool evalApeStatesFewerThanPredicates(const std::string &activity,
                                              const naming::NamingPtr &naming) const;
        void pushApeStatesFewerThanPredicate(const std::string &activity, const naming::NamingPtr &updatedNaming,
                                            int threshold, std::vector<uintptr_t> stateHashes);
        void pruneApeStatesFewerThanPredicates(const std::string &activity, const naming::NamingPtr &namingPrev,
                                                const naming::NamingPtr &namingCur,
                                                const std::unordered_set<uintptr_t> &affectedStateHashes);
        void apeCapGuiTreeNamingBlacklist();
        /// Cap coarsening / pair / ND-action blacklists (long-run stability; Java rebuild drops stale data).
        void apeCapApeNamingCoarsenAndRefineBlacklists();
#endif
        
        /// Smart pointer to the graph object managing all states and actions
        GraphPtr _graph;
        
        /// Map from device ID to agent object
        /// Allows multiple devices to have different agents with different strategies
        AbstractAgentPtrStrMap _deviceIDAgentMap;
        
        /// User-specified preferences for customizing behavior
        PreferencePtr _preference;

        /// Parameters for communicating with network-based action models
        NetActionParam _netActionParam;

        /// Optional LLM-based GUI agent (LLMTaskAgent). When configured with a concrete
        /// LlmClient implementation, this agent can temporarily take over action
        /// selection for predefined tasks (e.g. login flows).
        std::shared_ptr<LLMTaskAgent> _llmTaskAgent;

        /// Last model action returned per device; visit() on the next getOperateOpt after moveForward
        /// (counts only after a new observation, and keeps visitedCount aligned with resolveAt).
        std::unordered_map<std::string, ActionPtr> _pendingModelActionVisitByDevice;

        /// Coverage tracking: visited activities and step count (performance optimization)
        std::unordered_set<std::string> _visitedActivities;
        int _coverageStepCount{0};
        mutable std::mutex _coverageMutex;

        /// Per-activity widget key mask for dynamic state abstraction
        mutable std::unordered_map<std::string, WidgetKeyMask> _activityKeyMask;

        /// Optional: APE-native StateKey sidecar (parallel to widget-hash State); not used by Graph dedup.
        /// Note: state hash equals StateKey::hash() in dynamic mode; keep a bucket to defend rare hash collisions.
        std::unordered_map<uintptr_t, std::vector<naming::StateKey>> _ape_state_keys_by_hash;

        /// When max.apeGraphDedupByStateKey: canonical StatePtr per StateKey::hash().
        /// When max.apeGraphDedupByStateKey: hash bucket + full StateKey equality check.
        std::unordered_map<uintptr_t, std::vector<ApeGraphStateKeyDedupEntry>> _ape_graph_state_by_key;
        /// APE correctness counters (debug/telemetry).
        ApeCorrectnessCounters _ape_correctness_counters{};

        /// APE naming: wraps ActivityNamingManager + optional getNamingFixedPoint(actionRefinement on same dom).
        std::shared_ptr<naming::StateNamingManager> _apeStateNamingManager;

#if DYNAMIC_STATE_ABSTRACTION_ENABLED
        std::vector<ApeTransitionEntry> _apeTransitionLog;
        size_t _apeTransitionLogWriteIndex{0};
        std::unordered_map<ApePairKey, ApePairAggValue, ApePairKeyHash> _apePairAgg;
        void apePairAggRemove(const ApeTransitionEntry &e);
        void apePairAggAdd(const ApeTransitionEntry &e);

        void apeEvidencePoolAdd(const ApePairKey &pairKey, const ApeTransitionEntry &e);
        void apeEvidencePoolClockEvict();

        std::unordered_map<ApePairKey, ApeEvidencePool, ApePairKeyHash> _apeEvidencePools;
        std::vector<ApeEvidencePoolClockEntry> _apeEvidencePoolClock;
        size_t _apeEvidencePoolClockWriteIndex{0};
        size_t _apeEvidencePoolClockEvictIndex{0};
        uint64_t _apeEvidenceEpoch{0};

        size_t _stepCountSinceLastCheck{0};
        size_t _apeEventRefineSuccessCount{0};
        size_t _apeEventCoarsenRollbackCount{0};
        size_t _apeBatchRefineSuccessCount{0};
        size_t _apeBatchCoarsenRollbackCount{0};
        std::unordered_map<std::string, ApeNamingAbstractionContext> _apeNamingContext;
        std::set<std::pair<std::string, std::string>> _apeNamingCoarseningBlacklist;
        // Java-like predicate memory: avoid repeatedly refining on proven-bad trigger pairs.
        std::unordered_map<std::string, std::unordered_set<ApePairKey, ApePairKeyHash>> _apeRefinePairBlacklist;
        // APE NamingFactory.NDActionBlacklist: after failed resolveNonDeterminism, if out-edge count for this
        // action is >= 3 (Java: getOutStateTransitions(action).size() >= 3), blacklist the action.
        std::unordered_map<std::string, std::unordered_set<uintptr_t>> _apeRefineActionBlacklist;
        /// Action hashes belonging to states pruned after a Naming change; used for agent cache invalidation.
        std::unordered_set<uintptr_t> _apeInvalidatedReuseActionHashes;

        struct ApeMiniHistoryTransition {
            uintptr_t sourceStateHash{0};
            uintptr_t targetStateHash{0};
            ActionType actionType{ActionType::NOP};
            bool hasTargetBounds{false};
            Rect targetBounds{};
            bool hasTargetFullPath{false};
            uintptr_t targetFullPathHash{0};
            bool valid{false};
        };

        struct ApeMiniHistory {
            static constexpr size_t kStateCap = 32;
            static constexpr size_t kTransitionCap = 64;

            void touchState(uintptr_t sh) {
                if (sh == 0) {
                    return;
                }
                for (size_t i = 0; i < kStateCap; ++i) {
                    if (stateHashes[i] == sh) {
                        return;
                    }
                }
                stateHashes[stateWrite] = sh;
                stateWrite = (stateWrite + 1) % kStateCap;
            }

            void pushTransition(const ApeMiniHistoryTransition &t) {
                transitions[transitionWrite] = t;
                transitions[transitionWrite].valid = true;
                transitionWrite = (transitionWrite + 1) % kTransitionCap;
            }

            std::array<uintptr_t, kStateCap> stateHashes{};
            size_t stateWrite{0};
            std::array<ApeMiniHistoryTransition, kTransitionCap> transitions{};
            size_t transitionWrite{0};
        };

        struct ApeActivityRebuildStats {
            int consecutiveRollbacks{0};
            int actionBlacklistChecks{0};
            int actionBlacklistHits{0};
            uint64_t lastRebuildTimestamp{0};
        };

        void apeMiniHistoryTouchState(const std::string &activityKeyCanonical, uintptr_t stateHash);
        void apeMiniHistoryRecordTransition(const std::string &activityKeyCanonical,
                                            const ApeTransitionEntry &e);
        void apeInsertTransitionEntryNoRefine(const ApeTransitionEntry &e);
        bool apeLocalRebuildFromHistoryIfNeeded(const std::string &activityKeyCanonical,
                                                const char *reason);
        bool apeLocalRebuildFromHistory(const std::string &activityKeyCanonical);
        std::unordered_map<std::string, ApeMiniHistory> _apeMiniHistoryByActivity;
        std::unordered_map<std::string, ApeActivityRebuildStats> _apeRebuildStatsByActivity;
#ifndef NDEBUG
        mutable std::thread::id _apeOwnerThread{};
        mutable bool _apeOwnerThreadSet{false};
        void assertApeSingleThreaded() const;
#endif
#if defined(FASTBOT_HAS_PUGIXML) && FASTBOT_HAS_PUGIXML
        /// Recent page XML per state hash for transition-level refine candidate replay (APE checkActionRefinement-style).
        std::unordered_map<uintptr_t, std::string> _apeStateXmlByStateHash;
        /** Resolve widget XPath + parent Namelet under @p cur for cached XML of @p stateHash (Java resolveCurrentNamelet). */
        bool resolveApeWidgetExprAndParentNamelet(uintptr_t stateHash, const std::string &activityForSplit,
                                                  const naming::NamingPtr &cur, const WidgetPtr &targetWidget,
                                                  std::string *outExpr, naming::NameletPtr *outParent) const;
#endif
        /// APE NamingFactory.predicates queue (AssertSourceDivergent analogue).
        std::vector<ApeSourcePartitionPredicate> _apeSourcePartitionPredicates;
        /// AssertActionDivergent analogue (multi-source same APE key, distinct XML/layout).
        std::vector<ApeActionPartitionPredicate> _apeActionPartitionPredicates;
        /// Graph state hash -> Naming fingerprints forbidden after batchAbstract-style rollback.
        std::unordered_map<uintptr_t, std::unordered_set<std::string>> _apeGuiTreeNamingBlacklist;
        std::vector<ApeStatesFewerThanPredicate> _apeStatesFewerThanPredicates;
#endif

    };

    typedef std::shared_ptr<Model> ModelPtr;
}

#endif  // Model_H_
