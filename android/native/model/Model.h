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
#include "TreeTransition.h"
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
#if defined(FASTBOT_HAS_PUGIXML) && FASTBOT_HAS_PUGIXML && DYNAMIC_STATE_ABSTRACTION_ENABLED
#include <deque>
#include "../desc/gui_tree/GUITree.h"
#endif

namespace fastbotx {

namespace gui_tree {
    class GUITreeNode;
}

#if DYNAMIC_STATE_ABSTRACTION_ENABLED
    /// Disk-backed XML snapshot reference. Normally `path` points under
    /// `{--output-directory}/{target package}/` or `/sdcard/my-fastbot-logs/<target package>/`.
    /// `inlineXml` is only used as a correctness fallback when external storage cannot be written.
    struct XmlSnapshotRef {
        std::string path;
        std::string inlineXml;
        uint64_t contentHash{0};
        size_t byteSize{0};
        uint64_t snapshotSeq{0};

        bool empty() const {
            return path.empty() && inlineXml.empty();
        }
    };

    /// Naming transition log keyed by StateKey::hash().
    struct ApeTransitionEntry {
        uint64_t transitionSeq{0};
        uintptr_t sourceKeyHash{0};
        bool hasSourceStateKey{false};
        naming::StateKey sourceStateKey = naming::StateKey::fromParts("", nullptr, {});
        /// Runtime action object identity (NDActionBlacklist uses ModelAction object identity).
        uintptr_t actionIdentity{0};
        uintptr_t actionHash{0};
        uintptr_t targetKeyHash{0};
        std::string sourceActivity;
        /// State hashes (Graph/RL identity) for transition replay/remap when Naming changes.
        uintptr_t sourceStateHash{0};
        uintptr_t targetStateHash{0};
        XmlSnapshotRef sourceXmlSnapshot;
        /// Action signature for replay/remap. actionHash itself may change after Naming updates.
        ActionType actionType{ActionType::NOP};
        bool hasTargetBounds{false};
        Rect targetBounds{};
        /// Stable concrete target widget identity (`full` trace field).
        bool hasTargetFullPath{false};
        uintptr_t targetFullPathHash{0};
        /// Optional exact target StateKey for collision defense / debugging.
        bool hasTargetStateKey{false};
        naming::StateKey targetStateKey = naming::StateKey::fromParts("", nullptr, {});
        bool valid{false};
    };

    /// Per-activity naming refine/coarsen context.
    struct ApeNamingAbstractionContext {
        naming::NamingPtr previousNamingBeforeRefine;
        std::string previousNamingFingerprintBeforeRefine;
        std::unordered_map<uintptr_t, std::unordered_set<uintptr_t>> oldKeyHashToNewKeyHashes;
        // affected state observations per parent-key bucket (used by coarsen gate).
        std::unordered_map<uintptr_t, size_t> oldKeyHashToObservationCount;
        size_t stateCountAtLastNamingRefinement{0};
        int nonDetPairsAtLastNamingRefinement{0};
        // Pair-driven refine/coarsen context (Java resolveNonDeterminism / batchAbstract style)
        uintptr_t triggerSourceKeyHash{0};        // XML-space hash (for coarsen gate alignment)
        uintptr_t triggerSourceKeyHashOriginal{0}; // Element-space hash (for blacklist lookup)
        uintptr_t triggerSourceKeyHashUsed{0};     // XML-space hash used by coarsen gate
        bool triggerSourceKeyExact{false};
        naming::StateKey triggerSourceKey = naming::StateKey::fromParts("", nullptr, {});
        uintptr_t triggerActionHash{0};
        std::unordered_set<uintptr_t> triggerTargetKeyHashes;
        size_t triggerTargetCountAtRefine{0};
        // Diagnostic: last successful refine seed for this activity.
        uint64_t lastRefineSeedSeq{0};
        uintptr_t lastRefineSeedTriggerHash{0};
    };

    struct ApeActionDivergentPredicate {
        uintptr_t sourceStateHash{0};
        XmlSnapshotRef sourceXml;
#if defined(FASTBOT_HAS_PUGIXML) && FASTBOT_HAS_PUGIXML
        gui_tree::GUITreePtr sourceTree{};
#endif
        std::vector<std::vector<int>> partitionsStableIds;
    };

    struct ApeStatesFewerThanPredicate {
        int threshold{0};
        std::vector<uintptr_t> stateHashes;
#if defined(FASTBOT_HAS_PUGIXML) && FASTBOT_HAS_PUGIXML
        std::vector<gui_tree::GUITreePtr> sourceTrees;
#endif
    };

    /// Java AssertSourceDivergent: two ND branch source documents must not map to the same StateKey
    /// under a candidate naming. `sharedSourceStateHash` is the single graph state for the pair
    /// (st1.getSource() == st2.getSource()); used for predicate affected-scoping like
    /// AssertActionDivergent.
    struct ApeSourceDivergentPredicate {
        XmlSnapshotRef xmlA;
        XmlSnapshotRef xmlB;
        uintptr_t sharedSourceStateHash{0};
#if defined(FASTBOT_HAS_PUGIXML) && FASTBOT_HAS_PUGIXML
        gui_tree::GUITreePtr sourceTreeA{};
        gui_tree::GUITreePtr sourceTreeB{};
#endif
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
        naming::StateKey sourceStateKey = naming::StateKey::fromParts("", nullptr, {});
    };

    /// EvidencePool sample for one (sourceKeyHash, actionSignature) pair.
    /// Uses concrete transition-entry fields to approximate Java's evidence.
    struct ApeEvidenceSample {
        uintptr_t sourceStateHash{0};
        uintptr_t sourceTreeHash{0};
        uint64_t sourceTransitionSeq{0};
        XmlSnapshotRef sourceXml;
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

#if DYNAMIC_STATE_ABSTRACTION_ENABLED
        /** Root from `--output-directory` (default `/sdcard/my-fastbot-logs`); XML under `{dir}/{package}/`. */
        void setXmlSnapshotOutputDirectory(const std::string &outputDirectory) {
            _xmlSnapshotOutputDirectory = outputDirectory;
        }
        const std::string &getXmlSnapshotOutputDirectory() const { return _xmlSnapshotOutputDirectory; }
#endif

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
         * v1 files may list legacy widget-key masks and coarseningBlacklist; they are ignored (dynamic
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
         * @brief Optional StateKey sidecar: store StateKey alongside an RL state (indexed by state->hash()).
         * Call after the state is in the graph (e.g. after createAndAddState) when GUITree + Naming produced a key.
         */
        void recordApeStateKey(const StatePtr &state, const naming::StateKey &key);

        /**
         * Lookup a previously recorded StateKey by state hash.
         * When hash bucket has collisions, use hints to select exact key (object-equality style):
         * - hintActivity: canonical activity string match
         * - hintKeyHash: expected StateKey::hash()
         */
        bool tryGetApeStateKey(uintptr_t stateHash, naming::StateKey *out,
                               const std::string &hintActivity = std::string(),
                               uintptr_t hintKeyHash = 0) const;

        /** Hash-only lookup for previously recorded StateKey. */
        bool tryGetApeStateKeyHash(uintptr_t stateHash, uintptr_t *outKeyHash,
                                   const std::string &hintActivity = std::string(),
                                   uintptr_t hintKeyHash = 0) const;

#if DYNAMIC_STATE_ABSTRACTION_ENABLED
        /**
         * When naming changes, Model prunes stale states from Graph. This returns the action hashes
         * (ActivityNameAction::hash in dynamic mode) collected from those pruned states so agents can
         * invalidate hash-keyed caches in a scoped way. Valid during the call stack of
         * Model::notifyAgentsOfNamingChange().
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
         *  When @p stateForDynamicApply is non-null (dynamic RL identity), applies action hashes
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

        /// Record one transition for naming non-determinism detection (StateKey sidecars).
        void recordTransition(const AbstractAgentPtr &agent, const StatePtr &targetState);
        /// Run naming refinement batch if step count reached interval
        void runRefinementAndCoarseningIfScheduled();
        /// Naming lattice: record transition when both ends have StateKey sidecars.
        /// When skipNonDeterministicResolve (StatefulAgent.currentStateRecovered): keep pair aggregation /
        /// logs aligned with Graph.addTransition; skip NamingFactory.resolveNonDeterminism equivalent only.
        void recordApeTransitionForAbstraction(const StatePtr &src, const StatePtr &tgt,
                                               const ActivityStateActionPtr &act,
                                               bool skipNonDeterministicResolve = false);
        std::vector<std::string> detectNonDeterminismApe() const;
        struct ApeRefinePair {
            uint64_t nstTransitionSeq{0};
            uintptr_t sourceKeyHash{0};
            bool hasSourceStateKey{false};
            naming::StateKey sourceStateKey = naming::StateKey::fromParts("", nullptr, {});
            uintptr_t actionHash{0};
            uintptr_t actionIdentity{0};
            std::unordered_set<uintptr_t> targetKeyHashes;
            size_t targetCount{0};
        };
        enum class ApeRefineFailReason {
            None = 0,
            ActionBlacklisted,
            NoDefaultRootNaming,
            MaxStatesPerActivity,
            MaxGuitreesPerState,
            PairTargetsInsufficient,
            UnsupportedRefineRelation,
            NoAcceptedCandidates,
            /// Distinct from NoAcceptedCandidates: branch data (ND transition log / XML snapshots) was
            /// unavailable for this (src,action) pair, so we never actually enumerated candidates.
            /// The caller must NOT penalize the action via NDBlacklist in this case (would produce
            /// false-positive blacklisting of transitions whose evidence was trimmed from the log).
            BranchPairsUnavailable,
            Other,
        };
        /// @param precomputedActivityNonDetPairCount if >= 0 (from batch collectNonDetPairs), skip per-activity log scan
        bool refineActivityApeNaming(const std::string &activity, const ApeRefinePair *pair,
                                     int precomputedActivityNonDetPairCount = -1);
        bool refineActivityApeNaming(const std::string &activity, const ApeRefinePair *pair,
                                     int precomputedActivityNonDetPairCount,
                                     ApeRefineFailReason *outFailReason);
        bool coarsenActivityApeNamingIfNeeded(const std::string &activity);
        /// checkOverAbstractedState: sort targetedActions, dedupe by target, per action
        /// replaceLast/extend + check (see NamingFactory.actionRefinement). Falls back to batch
        /// NamingFactory::actionRefinement when pugixml or state XML cache is unavailable.
        size_t runApeOverAbstractedPreEvolvePhase(const std::string &activity, const StatePtr &state);
        /// After naming changes, StateKey::hash() space shifts; stale entries must not dedup new visits.
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

        /// Drop transition log + pair-agg rows for @p actKeyCanonical (rebuild clears stale hash space).
        void apeClearTransitionAggregationForActivity(const std::string &actKeyCanonical);
        void notifyAgentsOfApeNamingChange();
        /// Model.version epoch: bumps each time naming/graph refresh notifications run;
        /// Java increments Model.version on full rebuild() after a successful refine or coarsen).
        uint64_t getApeStructuralVersion() const { return _apeStructuralVersion; }

        /// Count of distinct StateKeys recorded for an activity under a specific Naming fingerprint.
        ///
        /// This helps keep refinement gates (e.g. minStates/minStateDelta) aligned with the *current* naming,
        /// instead of being biased by historical states created under older naming versions.
        size_t getApeStateCountByActivityAndNamingFingerprint(
            const std::string &activityKeyCanonical, const std::string &namingFingerprint) const;

        /// Sweep every state whose stored StateKey fingerprint
        /// differs from the activity's current naming fingerprint. Model.rebuild() iterates
        /// all states and removes any whose tree.currentNaming != namingManager.getNaming(tree);
        /// our fingerprint comparison is the storage-level equivalent and catches both the
        /// "just-superseded" fingerprint AND any older leaks that the explicit
        /// stale-fingerprint prune call did not reach. Invoked before the
        /// maxStatesPerActivity gate so the gate count reflects live states only (otherwise
        /// stale states pin the activity above the threshold forever and refine never fires -
        /// the end-of-refine prune never runs because the gate blocks it).
        void pruneDivergentApeStatesForActivity(const std::string &activityKeyCanonical);

        void pruneStaleApeStatesForActivity(const std::string &activityKeyCanonical,
            const std::string &staleNamingFingerprint,
            const std::unordered_set<uintptr_t> *affectedStateHashes = nullptr);

        /// Shared cleanup body for per-activity state prunes. Drops graph states + sidecar caches
        /// for the given hash set, preserving mini-history-referenced XML for later local rebuild.
        /// Takes the set by reference (non-const) as a minor impl-level convenience: callers have
        /// no further use for the set after the call, and we avoid an extra copy.
        void pruneApeStatesByStateHashesCommon(const std::string &activityKeyCanonical,
                                               std::unordered_set<uintptr_t> &staleStateHashes,
                                               const char *reasonTag);

        /// graph state from refine source-side GUITrees.
        bool evalApeGuiTreeNamingBlacklist(const std::vector<uintptr_t> &stateHashes,
                                           const naming::NamingPtr &naming) const;
        /// NamingFactory.checkPredicate order (type enum); eval matches AbstractPredicate.getState/getName:
        /// 'affected' is object-level GUITree set (not state-hash proxy). Trees in affected use candidate
        /// naming; otherwise use per-tree naming via StateNamingManager::treeToNaming(tree)
        /// (NamingManager.getNaming(tree)). If affected is empty/null, candidate naming is never used.
        bool evalApeActionRefinementPredicates(const std::string &activity, const naming::NamingPtr &naming,
                                               const std::vector<gui_tree::GUITreePtr> *affectedSourceTrees);
        /// Resolve effective Naming for a concrete state's GUITree (state-key edge walk), `getNaming(tree)`.
        naming::NamingPtr apeNamingResolvedViaTreeWalk(const std::string &activity, uintptr_t stateHash);
        void addApeStatesFewerThanPredicate(const std::string &activity,
                                            const std::unordered_set<uintptr_t> &affectedStateHashes,
                                            int threshold);
        /// Approximates Java AssertActionDivergent / AssertActionDivergent2 registration from ND refine and
        /// over-abstracted pre-evolve phase (merged-widget partitions keyed by preorder indices).
        void addApeActionRefinementPredicate(const std::string &activity,
                                             uintptr_t sourceStateHash,
                                             const std::string &sourceXml,
                                             const std::vector<int> &resolvedNodeStableIds,
                                             const naming::NamingPtr &updatedNaming);
        void removeConflictingApeActionRefinementPredicates(
            const std::string &activity, const naming::NamingPtr &naming,
            const std::unordered_set<uintptr_t> &affectedStateHashes);
        void removeConflictingApeStatesFewerThanPredicates(
            const std::string &activity, const naming::NamingPtr &naming,
            const std::unordered_set<uintptr_t> &affectedStateHashes);
        void addApeSourceDivergentPredicate(const std::string &activity, const std::string &xmlA,
                                            const std::string &xmlB, uintptr_t sharedSourceStateHash);
        void removeConflictingApeSourceDivergentPredicates(
            const std::string &activity, const naming::NamingPtr &naming,
            const std::unordered_set<uintptr_t> &affectedStateHashes);
        void apeBlacklistFinerNamingOnRollback(
            const std::string &activity, const naming::NamingPtr &finerNaming,
            const ApeNamingAbstractionContext &ctx, const std::unordered_set<uintptr_t> &affectedStateHashesForBlacklist);
        void apeCapGuiTreeNamingBlacklist();
        /// Cap coarsening / pair / ND-action blacklists (long-run stability; Java rebuild drops stale data).
        void apeCapApeNamingCoarsenAndRefineBlacklists();
        /** Sync Graph.namingToStates analogue after state-key sidecar mutation. */
        void syncApeNamingGraphIndex(const StatePtr &state);
        /** Repopulate Graph naming index from sidecars (cold start / repair drift). */
        void warmApeNamingGraphIndex();
#endif
        
        /// Smart pointer to the graph object managing all states and actions
        GraphPtr _graph;
        
        /// Map from device ID to agent object
        /// Allows multiple devices to have different agents with different strategies
        AbstractAgentPtrStrMap _deviceIDAgentMap;
        /// Latest screen State from getOperateOpt (used to validate actions after naming rebuild).
        StatePtr _apeLastScreenStateForValidate;
        
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

        /// Optional StateKey sidecar (parallel to widget-hash State); not used by Graph dedup.
        /// Note: state hash equals StateKey::hash() in dynamic mode; keep a bucket to defend rare hash collisions.
        std::unordered_map<uintptr_t, std::vector<naming::StateKey>> _ape_state_keys_by_hash;

        /// When state-key dedup is enabled: canonical StatePtr per StateKey::hash().
        /// Uses hash bucket + full StateKey equality check.
        std::unordered_map<uintptr_t, std::vector<ApeGraphStateKeyDedupEntry>> _ape_graph_state_by_key;
        /// Correctness counters (debug/telemetry).
        ApeCorrectnessCounters _ape_correctness_counters{};

        /// Naming manager wrapper + optional getNamingFixedPoint(actionRefinement on same dom).
        std::shared_ptr<naming::StateNamingManager> _apeStateNamingManager;

#if DYNAMIC_STATE_ABSTRACTION_ENABLED
        std::vector<ApeTransitionEntry> _apeTransitionLog;
        size_t _apeTransitionLogWriteIndex{0};
        std::vector<TreeTransitionEntry> _apeTreeTransitionLog;
        size_t _apeTreeTransitionLogWriteIndex{0};
        std::unordered_map<ApePairKey, ApePairAggValue, ApePairKeyHash> _apePairAgg;
        void apePairAggRemove(const ApeTransitionEntry &e);
        void apePairAggAdd(const ApeTransitionEntry &e);

        void apeEvidencePoolAdd(const ApePairKey &pairKey, const ApeTransitionEntry &e,
                                const XmlSnapshotRef *sourceXmlSnapshot = nullptr);
        void apeEvidencePoolClockEvict();

        std::unordered_map<ApePairKey, ApeEvidencePool, ApePairKeyHash> _apeEvidencePools;
        std::vector<ApeEvidencePoolClockEntry> _apeEvidencePoolClock;
        size_t _apeEvidencePoolClockWriteIndex{0};
        size_t _apeEvidencePoolClockEvictIndex{0};
        uint64_t _apeEvidenceEpoch{0};
        uint64_t _apeTransitionSeq{0};
        uint64_t _apeStructuralVersion{0};

        size_t _apeEventRefineSuccessCount{0};
        size_t _apeEventCoarsenRollbackCount{0};
        std::unordered_map<std::string, ApeNamingAbstractionContext> _apeNamingContext;
        std::set<std::pair<std::string, std::string>> _apeNamingCoarseningBlacklist;
        // NDActionBlacklist: blacklist exact action object identity.
        // actionHash remains for pair grouping/statistics only.
        std::unordered_set<uintptr_t> _apeRefineActionIdentityBlacklist;
        /// actionRefinementBlacklist during preEvolveModel over-abstracted phase.
        std::unordered_set<uint64_t> _apeOverAbstractedPreEvolveActionBlacklist;
        /// AssertActionDivergent2 predicates persisted after successful action-refinement.
        std::unordered_map<std::string, std::vector<ApeActionDivergentPredicate>>
            _apeActionRefinementPredicates;
        /// AssertStatesFewerThan predicates persisted after rollback coarsening.
        std::unordered_map<std::string, std::vector<ApeStatesFewerThanPredicate>>
            _apeStatesFewerThanPredicates;
        /// AssertSourceDivergent: ND branch source XML pair per successful refine.
        std::unordered_map<std::string, std::vector<ApeSourceDivergentPredicate>>
            _apeSourceDivergentPredicates;
        /// Action hashes belonging to states pruned after a Naming change; used for agent cache invalidation.
        std::unordered_set<uintptr_t> _apeInvalidatedReuseActionHashes;

#if defined(FASTBOT_HAS_PUGIXML) && FASTBOT_HAS_PUGIXML && DYNAMIC_STATE_ABSTRACTION_ENABLED
        /** State tree history: capped deque of immutable GUITree clones per concrete state hash. */
        static constexpr size_t kMaxApeGuiTreeSnapshotsPerState = 32;
        std::unordered_map<uintptr_t, std::deque<gui_tree::GUITreePtr>> _apeGuiTreeSnapshotsByStateHash;
        void apeRememberGuiTreeSnapshot(uintptr_t stateHash, const gui_tree::GUITree &tree);
        gui_tree::GUITreePtr apeLatestGuiTreeSnapshot(uintptr_t stateHash) const;
        /** Prefer snapshot whose cached XML equals `xml` (transition-tree consistency). */
        gui_tree::GUITreePtr guiTreeSnapshotForExactCachedXml(const std::string &xml) const;
        /** Drop in-memory GUITree clones once XML is disk-backed. */
        void apeEvictGuiTreeSnapshotsForState(uintptr_t stateHash);
        /** Refine gate: distinct XML content hashes seen for `stateHash`, else in-memory snapshot count. */
        size_t apeGuitreeGateCountForState(uintptr_t stateHash) const;
#endif

        struct ApeMiniHistoryTransition {
            uintptr_t sourceStateHash{0};
            uintptr_t targetStateHash{0};
            uintptr_t actionHash{0};
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
        void apeInsertTransitionEntryNoRefine(
            const ApeTransitionEntry &e, const TreeTransitionEntry *treeMeta = nullptr);
        void apeInsertTreeTransitionNoRefine(const TreeTransitionEntry &e);
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
        struct XmlHotCacheEntry {
            std::string xml;
            size_t byteSize{0};
            uint64_t lastTouch{0};
        };

        /// Disk-backed page XML per state hash for transition-level refine candidate replay.
        std::unordered_map<uintptr_t, XmlSnapshotRef> _stateXmlByStateHash;
        /// Distinct GUI page bodies (contentHash) observed per concrete state hash.
        std::unordered_map<uintptr_t, std::unordered_set<uint64_t>> _distinctXmlVariantsByStateHash;
        void apeNoteDistinctXmlVariant(uintptr_t stateHash, uint64_t contentHash);
        bool storeStateXmlSnapshot(uintptr_t stateHash, const std::string &activity,
                                      const std::string &xml, XmlSnapshotRef *outRef = nullptr);
        bool storeStandaloneXmlSnapshot(const std::string &activity, const std::string &tag,
                                           const std::string &xml, XmlSnapshotRef *outRef);
        bool loadStateXmlSnapshot(uintptr_t stateHash, std::string *outXml) const;
        bool loadXmlSnapshot(const XmlSnapshotRef &ref, std::string *outXml) const;
        void eraseStateXmlSnapshot(uintptr_t stateHash);
        XmlSnapshotRef inlineXmlSnapshotForFallback(const std::string &xml) const;
        std::string apeXmlSnapshotRootDir(const std::string &activity) const;
        /// Small process-local hot cache; cold XML lives on disk under apeXmlSnapshotRootDir.
        mutable std::unordered_map<std::string, XmlHotCacheEntry> _xmlHotCache;
        mutable size_t _xmlHotCacheBytes{0};
        mutable uint64_t _xmlHotCacheClock{0};
        /// Same keys when the live Element snapshot exists - avoids tinyxml re-parse; matches buildFromElement semantics.
        std::unordered_map<uintptr_t, ElementPtr> _apeStateElementByStateHash;
        std::string _xmlSnapshotOutputDirectory;
        /** Resolve widget XPath + parent Namelet under @p cur for cached XML of @p stateHash (Java resolveCurrentNamelet). */
        bool resolveApeWidgetExprAndParentNamelet(uintptr_t stateHash, const std::string &activityForSplit,
                                                  const naming::NamingPtr &cur, const WidgetPtr &targetWidget,
                                                  std::string *outExpr, naming::NameletPtr *outParent) const;
#endif
        /// Graph state hash -> Naming fingerprints forbidden after batchAbstract-style rollback.
        std::unordered_map<uintptr_t, std::unordered_set<std::string>> _apeGuiTreeNamingBlacklist;
#endif

    };

    typedef std::shared_ptr<Model> ModelPtr;
}

#endif  // Model_H_
