/**
 * AutodevAgent: LLM-based GUI agent that can temporarily take over action selection
 * for predefined tasks (e.g. login flows) based on checkpoints configured in
 * external files. The agent runs on the C++ side and always returns standard
 * ActionPtr objects that are later converted to Operate and executed by the
 * existing Java layer.
 *
 * NOTE:
 * - This header only defines the core interfaces and data structures.
 * - The actual LLM integration is abstracted behind LlmClient; production
 *   implementations should provide a concrete client (e.g. HTTP-based).
 */
 /**
 * @authors Zhao Zhang
 */

#ifndef FASTBOTX_AUTODEV_AGENT_H
#define FASTBOTX_AUTODEV_AGENT_H

#include <memory>
#include <string>
#include <vector>

#include "../desc/Action.h"
#include "../desc/Element.h"
#include "../llm/LlmTypes.h"

namespace fastbotx {

    class Preference;
    using PreferencePtr = std::shared_ptr<Preference>;

    /**
     * Abstract LLM client interface.
     *
     * Implementations are responsible for:
     * - Building and sending HTTP / RPC requests to a concrete LLM backend.
     * - Returning raw JSON (or JSON-like) response that encodes LlmActionSpec.
     */
    class LlmClient {
    public:
        virtual ~LlmClient() = default;

        /**
         * Predict next action for the current GUI state.
         *
         * @param prompt       Text prompt containing task description and UI abstraction.
         * @param images       Optional screenshots for multimodal models.
         * @param outResponse  Raw response string (e.g. JSON) from the model.
         * @return true on success, false on error or timeout.
         */
        virtual bool predict(const std::string &prompt,
                             const std::vector<ImageData> &images,
                             std::string &outResponse) = 0;

        /**
         * Predict using payload JSON; Java assembles the prompt (reduces JNI copy).
         * promptType: "executor" | "planner" | "step_summary".
         */
        virtual bool predictWithPayload(const std::string &promptType,
                                        const std::string &payloadJson,
                                        const std::vector<ImageData> &images,
                                        std::string &outResponse) {
            (void) promptType;
            (void) payloadJson;
            (void) images;
            (void) outResponse;
            return false;
        }
    };

    /**
    /**
     * AutodevAgent:
     *
     * - Maintains session state for currently running LLM tasks.
     * - At each step, optionally starts a new session (when checkpoint matches)
     *   or continues an existing one.
     * - Uses LlmClient to query the LLM and converts its output into ActionPtr.
     *
     * IMPORTANT:
     * - This class does NOT execute any operations directly.
     * - It only returns ActionPtr, which Model later converts into OperatePtr
     *   via Model::convertActionToOperate() and sends to the Java layer.
     */
    class AutodevAgent {
    public:
        AutodevAgent(const PreferencePtr &preference, std::shared_ptr<LlmClient> llmClient);

        /**
         * Replace the underlying LLM client at runtime.
         * Passing nullptr effectively disables AutodevAgent.
         */
        void setLlmClient(std::shared_ptr<LlmClient> llmClient) {
            _llmClient = std::move(llmClient);
        }

        /**
         * Main entry point called by Model on every step.
         *
         * @param rootXml   Current page XML root element (may have been resolvePage'd).
         * @param activity  Current activity name.
         * @param deviceId  Device identifier (for future multi-device support).
         * @param preMatchedLlmTask  If non-null, LLM task already matched on raw tree (before resolvePage); used to start session.
         * Image for LLM is obtained in Java on demand when native triggers HTTP (LlmScreenshotProvider).
         * @return ActionPtr chosen by the LLM agent, or nullptr if no match / session ended / LLM failed.
         */
        ActionPtr selectNextAction(const ElementPtr &rootXml,
                                   const std::string &activity,
                                   const std::string &deviceId,
                                   LlmTaskConfigPtr preMatchedLlmTask = nullptr);

        /**
         * Whether AutodevAgent is currently inside a running task session.
         */
        bool inSession() const;

        /**
         * Forcefully terminate current session (if any).
         */
        void resetSession();

    private:
        PreferencePtr _preference;
        std::shared_ptr<LlmClient> _llmClient;
        std::unique_ptr<LlmSessionState> _session;

        bool maybeStartSession(const ElementPtr &rootXml,
                               const std::string &activity,
                               const std::string &deviceId,
                               LlmTaskConfigPtr preMatchedTask = nullptr);

        bool isSessionExpired() const;

        /**
         * Build a textual prompt for the LLM using:
         * - Task description
         * - Activity name
         * - Simple UI abstraction from rootXml
         * - Recent history summaries
         * @param outCurrentScreenHash  If non-null, set to current screen hash for navigation_state (caller appends to recentScreenHashes).
         */
        std::string buildPrompt(const ElementPtr &rootXml,
                                const std::string &activity,
                                std::string *outCurrentScreenHash = nullptr,
                                const std::string *precomputedFingerprint = nullptr) const;

        /** Result of a single tree walk: fingerprint string (for prompt) + ordered list of interactive elements (for INDEX lookup). */
        struct InteractiveElementsResult {
            std::string fingerprint;
            std::vector<ElementPtr> elements;
        };

        /** One DFS: both fingerprint string and element list (same order as getScreenFingerprint). Use to avoid second DFS in convertToAction(INDEX). */
        InteractiveElementsResult getScreenFingerprintWithElements(const ElementPtr &rootXml) const;

        /** Returns a stable string fingerprint of visible interactive elements for navigation_state hashing. */
        std::string getScreenFingerprint(const ElementPtr &rootXml) const;

        /**
         * Parse raw LLM response string into LlmActionSpec.
         * The initial implementation uses a very defensive parser and will
         * return false if the response is malformed.
         */
        bool parseLlmResponse(const std::string &response, LlmActionSpec &outSpec) const;

        /** Reason for convertToAction failure; used to treat safety hits as explicit ABORT. */
        enum class ConvertFailureReason { None, NotFound, Forbidden, Other };

        /**
         * Convert an LlmActionSpec into a concrete ActionPtr.
         *
         * The initial implementation focuses on CLICK and BACK to keep the PoC
         * simple and robust. Other action types can be added incrementally.
         * When conversion fails due to forbidden text (safe_mode), outFailure is set to Forbidden
         * so the caller can terminate the session immediately as a safety abort.
         */
        ActionPtr convertToAction(const LlmActionSpec &spec,
                                  const ElementPtr &rootXml,
                                  const std::string &activity,
                                  ConvertFailureReason *outFailure = nullptr,
                                  const std::vector<ElementPtr> *interactiveElements = nullptr) const;

        /**
         * Helper to locate a target element by simple selector.
         * Currently supports:
         * - by == "XPATH"  (using Element::matchXpathSelector)
         * - by == "TEXT"   (matching Element::getText)
         */
        ElementPtr findTargetElement(const std::string &by,
                                     const std::string &value,
                                     const ElementPtr &rootXml) const;

        /** INDEX: use list when provided (avoids second DFS); otherwise findTargetElement. */
        ElementPtr resolveTargetElement(const std::string &by, const std::string &value,
                                        const ElementPtr &rootXml,
                                        const std::vector<ElementPtr> *interactiveElements) const;

        /**
         * Depth-first search helper used by findTargetElement for XPATH.
         */
        ElementPtr findFirstMatchedElement(const XpathPtr &xpathSelector,
                                           const ElementPtr &elementXml) const;

        /**
         * Append a short natural language summary of this step into session history.
         * For now we keep it extremely simple and generate the summary locally
         * instead of making a second LLM call as in M3A.
         */
        void appendLocalSummary(const LlmActionSpec &spec);

        /**
         * Optional: ask LLM for a one-sentence summary of the given step (for use when
         * useLlmForStepSummary is true). Returns empty string on failure or if client is null.
         */
        std::string requestStepSummaryFromLlm(const StepHistoryEntry &entry) const;

        /**
         * If the LLM response JSON contains "todo_updates" array, merge/replace session todos.
         */
        void applyTodoUpdatesFromResponse(const std::string &response);

        /**
         * If the LLM response JSON contains "scratchpad_updates" array, merge into session scratchpad.
         */
        void applyScratchpadUpdatesFromResponse(const std::string &response);

        /** Apply todo_updates from pre-parsed JSON (pointer to nlohmann::json). Used to avoid re-parsing in selectNextAction. */
        void applyTodoUpdatesFromJson(const void *jsonPtr);
        /** Apply scratchpad_updates from pre-parsed JSON (pointer to nlohmann::json). */
        void applyScratchpadUpdatesFromJson(const void *jsonPtr);
        /** Parse LlmActionSpec from pre-parsed JSON. jsonPtr = &nlohmann::json. Returns false on invalid; rawResponsePrefix used for error log. */
        bool parseLlmResponseFromJson(const void *jsonPtr, LlmActionSpec &outSpec, const std::string &rawResponsePrefix) const;

        // --- v3 Planner + Executor ---
        /** Build prompt for Planner LLM (task, todos, scratchpad, history; no full UI tree). */
        std::string buildPlannerPrompt() const;

        /** Build payload JSON for Java to assemble Executor prompt. Sets outCurrentScreenHash if non-null. */
        std::string buildExecutorPayload(const ElementPtr &rootXml,
                                         const std::string &activity,
                                         std::string *outCurrentScreenHash = nullptr,
                                         const std::string *precomputedFingerprint = nullptr) const;
        /** Build payload JSON for Java to assemble Planner prompt. */
        std::string buildPlannerPayload() const;
        /** Build payload JSON for Java to assemble StepSummary prompt. */
        std::string buildStepSummaryPayload(const StepHistoryEntry &entry) const;
        /** Parse Planner LLM response into one semantic step (tool, intent, text). Returns true if valid. */
        bool parsePlannerResponse(const std::string &response, PlannerStep &outStep) const;
        /** Fill PlannerStep from already-parsed JSON (avoids second parse when applying todo_updates). */
        bool parsePlannerResponseFromJson(const void *jsonPtr, PlannerStep &outStep) const;
    };

} // namespace fastbotx

#endif // FASTBOTX_AUTODEV_AGENT_H

