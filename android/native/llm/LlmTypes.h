/*
 * Shared LLM-related types used by AutodevAgent and Preference.
 */
 /**
 * @authors Zhao Zhang
 */

#ifndef FASTBOTX_LLM_TYPES_H
#define FASTBOTX_LLM_TYPES_H

#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "../desc/Element.h"

namespace fastbotx {

    /**
     * Lightweight image descriptor passed to LlmClient.
     * Concrete LLM clients can decide whether to use file paths or raw bytes.
     */
    struct ImageData {
        std::string path;   ///< Optional path on disk
        std::string bytes;  ///< Optional encoded bytes (e.g. PNG)
    };

    /**
     * LLM output action specification (intermediate representation).
     *
     * This mirrors the JSON schema described in FASTBOT_LLM_GUI_AGENT_DESIGN.md and
     * is later converted into a concrete ActionPtr.
     */
    struct LlmActionSpec {
        std::string taskStatus;   ///< "ONGOING" / "COMPLETED" / "ABORT"
        std::string actionType;   ///< "CLICK" / "INPUT" / "SCROLL" / "BACK" / "WAIT" / "STATUS"
        std::string targetBy;     ///< "VIEW_ID" / "TEXT" / "XPATH" / "BOUNDS" / "INDEX"
        std::string targetValue;  ///< Identifier value for the target
        std::vector<std::pair<std::string, std::string>> fallbacks; ///< Optional fallback selectors
        std::string text;         ///< Text payload for INPUT actions
        std::string direction;    ///< Scroll direction for SCROLL actions
        std::string reason;       ///< Natural language explanation (for logging)
    };

    /**
     * Per-task configuration, loaded from external JSON (e.g. /sdcard/max.llm.tasks).
     *
     * This struct is intentionally simple; Preference is responsible for loading
     * and managing its lifetime.
     */
    struct LlmTaskConfig {
        std::string activity;                 ///< Target activity name
        std::string checkpointXpathString;    ///< Original checkpoint XPath string
        XpathPtr checkpointXpath;             ///< Parsed XPath object
        std::string taskDescription;          ///< Natural language task description
        int maxSteps{10};                     ///< Maximum allowed AutodevAgent steps
        int maxDurationMs{30000};             ///< Maximum duration in milliseconds
        bool safeMode{false};                 ///< Whether to enable conservative behavior (forbidden_texts check)
        std::vector<std::string> forbiddenTexts; ///< Texts that must not be clicked
        bool useLlmForStepSummary{false};    ///< If true, call LLM for one-line step summary; else use local appendLocalSummary
        bool usePlannerLayer{true};            ///< v3: If true, use Planner LLM to output semantic steps; Executor fulfills each step
        int maxTimes{0};                       ///< Max times this task can be run in the session (0 = unlimited). From "max_times" in JSON.
        bool resetCount{false}; ///< If true, clear this task's run count when activity switches away ("reset_count" in JSON). If false, run count never resets.
    };

    using LlmTaskConfigPtr = std::shared_ptr<LlmTaskConfig>;

    /**
     * v3 Planner: one semantic step (tool + intent/text). Executor translates to concrete action.
     */
    struct PlannerStep {
        std::string tool;   ///< "tap" | "scroll" | "type_text" | "answer" | "finish_task" | "go_back"
        std::string intent;///< Semantic target, e.g. "登录按钮", "向下"
        std::string text;  ///< For type_text: content to type; for answer: reply text
    };

    /**
     * Per-step history entry for debugging and optional summarization.
     * Used locally (e.g. logs); not all fields are sent back to the LLM.
     */
    struct StepHistoryEntry {
        int stepIndex{0};
        std::string actionType;
        std::string targetBy;
        std::string targetValue;
        std::string actionReason;
        std::string actionOutputJson; ///< Raw or truncated LLM output for this step
    };

    /**
     * Single todo item for multi-step task breakdown (v2 TodoList).
     */
    struct TodoItem {
        std::string id;       ///< Optional id for updates
        std::string content;  ///< Description of the sub-task
        std::string status;   ///< "pending" | "in_progress" | "done"
        int priority{0};      ///< Optional ordering hint
    };

    /**
     * Scratchpad item for session-scoped key-value storage (v2 Scratchpad).
     */
    struct ScratchpadItem {
        std::string title;    ///< Short label
        std::string text;     ///< Full content
    };

    /**
     * Per-session state for a running AutodevAgent task.
     */
    struct LlmSessionState {
        LlmTaskConfigPtr taskConfig;
        std::string activity;
        std::string deviceId;
        int stepCount{0};
        long long startTimestampMs{0};
        bool completed{false};
        bool aborted{false};
        std::vector<std::string> historySummaries; ///< Short natural language summaries (injected into buildPrompt)
        std::vector<StepHistoryEntry> history;     ///< Per-step entries for logging/debugging (capped size)
        std::vector<TodoItem> todos;                ///< Current todo list (injected into buildPrompt, updated by LLM todo_updates)
        std::map<std::string, ScratchpadItem> scratchpad; ///< Key -> (title, text) for createItem/fetchItem (injected into buildPrompt)
        std::vector<std::string> recentScreenHashes;      ///< Last N screen hashes for navigation_state anti-loop hint (optional)
        PlannerStep currentPlannerStep;                  ///< v3: current semantic step from Planner; cleared after Executor uses it
        int plannerStepFailureCount{0};                  ///< v3: failures for current Planner step; when >= threshold we clear currentPlannerStep and ask Planner for next

        // Failure tracking for consecutive failures threshold
        int consecutiveFailures{0}; ///< Count of consecutive failed steps (parse error or no target found)

        // Termination reason for logging and debugging
        std::string abortReason; ///< Reason for session termination (e.g., "max_steps", "max_duration", "consecutive_failures", "llm_error", "parse_error", "completed")
    };

    /**
     * Runtime configuration for calling an OpenAI-compatible HTTP LLM endpoint.
     */
    struct LlmRuntimeConfig {
        bool enabled{false};           ///< Whether LLM integration is enabled
        std::string apiUrl;           ///< Full URL of the OpenAI-compatible /chat/completions endpoint
        std::string apiKey;           ///< API key for Authorization header
        std::string model;            ///< Model name (e.g. "gpt-4.1")
        int maxTokens{256};           ///< Max tokens for the completion
        int timeoutMs{15000};         ///< Request timeout in milliseconds
    };

} // namespace fastbotx

#endif // FASTBOTX_LLM_TYPES_H

