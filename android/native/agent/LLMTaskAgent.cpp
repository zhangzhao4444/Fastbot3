/**
 * @authors Zhao Zhang
 */

#include "LLMTaskAgent.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <functional>
#include <sstream>
#include <utility>

#include "../utils.hpp"
#include "../events/Preference.h"
#include "../thirdpart/json/json.hpp"

namespace fastbotx {

    namespace {
        long long currentTimeMillis() {
            using namespace std::chrono;
            return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
        }

        // Centralized limits (see LLM_TASK_AGENT_CODE_REVIEW.md)
        constexpr size_t kMaxHistory = 10;
        constexpr size_t kMaxHistoryEntries = 20;
        constexpr size_t kMaxScreenHashes = 15;
        constexpr int kMaxConsecutiveFailures = 3;
        constexpr size_t kMaxTodos = 32;
        constexpr size_t kMaxScratchpadItems = 32;
        constexpr int kMaxPlannerStepFailures = 2;
        constexpr size_t kMaxSummaryLen = 200;
    }

    LLMTaskAgent::LLMTaskAgent(const PreferencePtr &preference,
                               std::shared_ptr<LlmClient> llmClient)
        : _preference(preference),
          _llmClient(std::move(llmClient)),
          _session(nullptr) {
    }

    bool LLMTaskAgent::inSession() const {
        return _session != nullptr;
    }

    void LLMTaskAgent::resetSession() {
        if (_session && _session->taskConfig) {
            long long durationMs = currentTimeMillis() - _session->startTimestampMs;
            BDLOG("LLMTaskAgent: Session end | activity=%s | stepCount=%d | durationMs=%lld | reason=%s | task=%s",
                  _session->activity.c_str(), _session->stepCount, durationMs,
                  _session->abortReason.empty() ? "reset" : _session->abortReason.c_str(),
                  _session->taskConfig->taskDescription.substr(0, 60).c_str());
        }
        _session.reset();
    }

    bool LLMTaskAgent::maybeStartSession(const ElementPtr &rootXml,
                                         const std::string &activity,
                                         const std::string &deviceId,
                                         LlmTaskConfigPtr preMatchedTask) {
        if (this->inSession()) {
            return true;
        }
        if (!_preference || !rootXml) {
            return false;
        }

        LlmTaskConfigPtr cfg = preMatchedTask ? preMatchedTask : _preference->matchLlmTask(activity, rootXml);
        if (!cfg) {
            return false;
        }
        if (!_preference->canStartLlmTask(cfg)) {
            BDLOG("LLM task skipped: already reached max_times (activity=%s)", cfg->activity.c_str());
            return false;
        }

        _preference->incrementLlmTaskRunCount(cfg);
        _session = std::make_unique<LlmSessionState>();
        _session->taskConfig = cfg;
        _session->activity = activity;
        _session->deviceId = deviceId;
        _session->stepCount = 0;
        _session->startTimestampMs = currentTimeMillis();
        _session->completed = false;
        _session->aborted = false;
        _session->consecutiveFailures = 0;
        _session->abortReason.clear();
        _session->historySummaries.clear();
        _session->todos.clear();
        _session->scratchpad.clear();
        _session->recentScreenHashes.clear();
        _session->currentPlannerStep = PlannerStep();
        _session->plannerStepFailureCount = 0;
        return true;
    }

    bool LLMTaskAgent::isSessionExpired() const {
        if (!_session || !_session->taskConfig) {
            return true;
        }
        const auto &cfg = *_session->taskConfig;
        
        // Check max_steps limit
        if (cfg.maxSteps > 0 && _session->stepCount >= cfg.maxSteps) {
            if (_session->abortReason.empty()) {
                _session->abortReason = "max_steps";
            }
            return true;
        }
        
        // Check max_duration_ms limit
        if (cfg.maxDurationMs > 0) {
            long long now = currentTimeMillis();
            if (now - _session->startTimestampMs >= cfg.maxDurationMs) {
                if (_session->abortReason.empty()) {
                    _session->abortReason = "max_duration";
                }
                return true;
            }
        }
        
        // Check consecutive failures threshold
        if (_session->consecutiveFailures >= kMaxConsecutiveFailures) {
            if (_session->abortReason.empty()) {
                _session->abortReason = "consecutive_failures";
            }
            return true;
        }
        
        return false;
    }

    LLMTaskAgent::InteractiveElementsResult LLMTaskAgent::getScreenFingerprintWithElements(const ElementPtr &rootXml) const {
        InteractiveElementsResult result;
        if (!rootXml) return result;
        result.elements.reserve(64);
        std::ostringstream oss;
        int index = 0;
        std::vector<ElementPtr> stack;
        stack.push_back(rootXml);
        while (!stack.empty() && index < 64) {
            ElementPtr elem = stack.back();
            stack.pop_back();
            if (!elem) continue;
            const auto &children = elem->getChildren();
            for (const auto &c : children) stack.push_back(c);
            if (!elem->getClickable() && !elem->getLongClickable() && !elem->getScrollable()) continue;
            oss << "[" << index << "] " << elem->getClassname() << "|" << elem->getResourceID() << "|" << elem->getText() << "|" << elem->getContentDesc() << "\n";
            result.elements.push_back(elem);
            ++index;
        }
        result.fingerprint = oss.str();
        return result;
    }

    std::string LLMTaskAgent::getScreenFingerprint(const ElementPtr &rootXml) const {
        return getScreenFingerprintWithElements(rootXml).fingerprint;
    }

    std::string LLMTaskAgent::buildPrompt(const ElementPtr &rootXml,
                                          const std::string &activity,
                                          std::string *outCurrentScreenHash,
                                          const std::string *precomputedFingerprint) const {
        std::ostringstream oss;

        std::string screenFingerprint;
        if (precomputedFingerprint && !precomputedFingerprint->empty()) {
            screenFingerprint = *precomputedFingerprint;
        } else if (rootXml) {
            screenFingerprint = getScreenFingerprint(rootXml);
        }
        // Navigation_state anti-loop: if we've seen this screen recently, warn the LLM
        if (_session && !screenFingerprint.empty()) {
            std::string hashStr = std::to_string(std::hash<std::string>{}(screenFingerprint));
            if (outCurrentScreenHash) *outCurrentScreenHash = hashStr;
            const auto &seen = _session->recentScreenHashes;
            if (std::find(seen.begin(), seen.end(), hashStr) != seen.end()) {
                oss << "Navigation hint: this screen was already seen recently; avoid repeating the same scroll or try a different strategy.\n\n";
            }
        }

        // v3: current sub-task from Planner (aligned with android_world tool_call_to_query style)
        if (_session && _session->taskConfig && _session->taskConfig->usePlannerLayer &&
            !_session->currentPlannerStep.tool.empty()) {
            const auto &p = _session->currentPlannerStep;
            oss << "=== EXECUTOR SUB-TASK (complete this fully) ===\n";
            if (p.tool == "tap") {
                oss << "Locate and tap on the \"" << p.intent << "\" on the current screen. Choose the correct element index and perform CLICK.\n";
            } else if (p.tool == "scroll") {
                oss << "Perform a scroll to \"" << p.intent << "\". Choose direction (UP/DOWN/LEFT/RIGHT) and optional target index. Use SCROLL action.\n";
            } else if (p.tool == "type_text") {
                oss << "Locate the \"" << p.intent << "\" input field, then type the following text exactly: \"" << p.text << "\". Use INPUT with the chosen index and text.\n";
            } else if (p.tool == "go_back") {
                oss << "Navigate back to the previous screen. Use BACK action.\n";
            } else if (p.tool == "answer") {
                oss << "Provide the answer (no UI action): \"" << p.text << "\". Use task_status COMPLETED and optionally STATUS action.\n";
            } else {
                oss << "Planner step: " << p.tool;
                if (!p.intent.empty()) oss << " intent=\"" << p.intent << "\"";
                if (!p.text.empty()) oss << " text=\"" << p.text << "\"";
                oss << ". Execute this single step (choose the right element index / action).\n";
            }
            oss << "Your action will be executed; the result is summarized for the Planner.\n\n";
        }

        // High level system hint
        oss << "You are an Android GUI testing agent. "
            << "Given the current screen description and a task, you must output the next GUI action "
            << "in JSON format.\n\n";

        if (_session && _session->taskConfig) {
            oss << "Task:\n" << _session->taskConfig->taskDescription << "\n\n";
        }

        oss << "Current activity: " << activity << "\n\n";

        // Extremely lightweight UI abstraction: list clickable elements with index and basic text.
        if (!screenFingerprint.empty()) {
            oss << "Visible interactive elements (index, class, resource-id, text, content-desc):\n";
            oss << screenFingerprint;
            oss << "\n";
        }

        if (_session && !_session->historySummaries.empty()) {
            oss << "Recent steps summary:\n";
            for (const auto &s : _session->historySummaries) {
                oss << "- " << s << "\n";
            }
            oss << "\n";
        }

        if (_session && !_session->todos.empty()) {
            oss << "Current todos:\n";
            for (size_t i = 0; i < _session->todos.size(); ++i) {
                const auto &t = _session->todos[i];
                oss << "  " << (i + 1) << ". [" << t.status << "] " << t.content;
                if (!t.id.empty()) oss << " (id=" << t.id << ")";
                oss << "\n";
            }
            oss << "You may update todos by including \"todo_updates\" in your JSON: [{\"id\":\"...\",\"content\":\"...\",\"status\":\"pending|in_progress|done\"}].\n\n";
        }

        if (_session && !_session->scratchpad.empty()) {
            oss << "Scratchpad (stored items, key -> title / content):\n";
            for (const auto &kv : _session->scratchpad) {
                oss << "  " << kv.first << " | title: " << kv.second.title << "\n  content: " << kv.second.text << "\n";
            }
            oss << "You may create/update items with \"scratchpad_updates\" in your JSON: [{\"key\":\"...\",\"title\":\"...\",\"text\":\"...\"}].\n\n";
        }

        oss << "You must respond with a single valid JSON object. No extra text.\n"
            << "task_status must be exactly one of: ONGOING, COMPLETED, ABORT.\n"
            << "You may use either (1) action object or (2) tool_calls. Tool names: click(index), input_text(index,text), scroll(direction[,index]), back, wait([duration_ms]), status.\n"
            << "action_type must be exactly one of: CLICK, INPUT, SCROLL, BACK, WAIT, STATUS.\n"
            << "Example (action format):\n"
            << "{\n"
            << "  \"task_status\": \"ONGOING\",\n"
            << "  \"action\": {\n"
            << "    \"action_type\": \"CLICK\",\n"
            << "    \"target\": { \"by\": \"INDEX\", \"value\": \"0\" },\n"
            << "    \"text\": \"\",\n"
            << "    \"reason\": \"short explanation\"\n"
            << "  }\n"
            << "}\n"
            << "Example (tool_calls format): {\"task_status\":\"ONGOING\",\"tool_calls\":[{\"name\":\"click\",\"arguments\":{\"index\":0,\"reason\":\"...\"}}]}\n";

        return oss.str();
    }

    // Planner JSON format (must match parsePlannerResponse): one object with "tool", "intent", "text".
    // Aligned with android_world PLANNER_SYSTEM_PROMPT: semantic tools only; Executor has no memory.
    std::string LLMTaskAgent::buildPlannerPrompt() const {
        std::ostringstream oss;
        if (!_session || !_session->taskConfig) return "";
        oss << "You are an expert PLANNER for Android GUI automation. You output ONE semantic step per response. "
            << "You NEVER output coordinates or element indices; the Executor will choose the concrete action.\n\n";
        oss << "**CRITICAL**: Give COMPLETE, DETAILED subgoals. The Executor has NO MEMORY - every instruction must be self-contained.\n\n";
        oss << "Task: " << _session->taskConfig->taskDescription << "\n\n";
        if (!_session->todos.empty()) {
            oss << "Current todos:\n";
            for (size_t i = 0; i < _session->todos.size(); ++i) {
                const auto &t = _session->todos[i];
                oss << "  " << (i + 1) << ". [" << t.status << "] " << t.content << "\n";
            }
            oss << "You may include \"todo_updates\" in your JSON to replace/update the todo list: [{\"id\":\"...\",\"content\":\"...\",\"status\":\"pending|in_progress|completed\"}].\n\n";
        }
        if (!_session->scratchpad.empty()) {
            oss << "Scratchpad keys (stored data): ";
            for (const auto &kv : _session->scratchpad) oss << kv.first << " ";
            oss << "\n\n";
        }
        if (!_session->historySummaries.empty()) {
            oss << "Steps done so far (Executor reports):\n";
            for (const auto &s : _session->historySummaries) oss << "- " << s << "\n";
            oss << "\n";
        }
        oss << "Tools (semantic only): tap(intent), scroll(intent), type_text(text,intent), answer(text), finish_task(), go_back().\n"
            << "Respond with ONE JSON object: {\"tool\": \"tap\"|\"scroll\"|\"type_text\"|\"answer\"|\"finish_task\"|\"go_back\", "
            << "\"intent\": \"e.g. login button or scroll down to find X\", \"text\": \"for type_text/answer only\"}. "
            << "Optional: \"todo_updates\": [...] to update the todo list. Use finish_task when the task is complete.";
        return oss.str();
    }

    std::string LLMTaskAgent::buildExecutorPayload(const ElementPtr &rootXml,
                                                   const std::string &activity,
                                                   std::string *outCurrentScreenHash,
                                                   const std::string *precomputedFingerprint) const {
        using nlohmann::json;
        json j;
        std::string screenFingerprint;
        if (precomputedFingerprint && !precomputedFingerprint->empty()) {
            screenFingerprint = *precomputedFingerprint;
        } else if (rootXml) {
            screenFingerprint = getScreenFingerprint(rootXml);
        }
        bool navHint = false;
        if (_session && !screenFingerprint.empty()) {
            std::string hashStr = std::to_string(std::hash<std::string>{}(screenFingerprint));
            if (outCurrentScreenHash) *outCurrentScreenHash = hashStr;
            const auto &seen = _session->recentScreenHashes;
            navHint = (std::find(seen.begin(), seen.end(), hashStr) != seen.end());
        }
        j["nav_hint"] = navHint;
        j["activity"] = activity;
        j["screen_fingerprint"] = screenFingerprint;
        if (_session && _session->taskConfig) {
            j["task_description"] = _session->taskConfig->taskDescription;
        } else {
            j["task_description"] = "";
        }
        if (_session && !_session->historySummaries.empty()) {
            j["history_summaries"] = _session->historySummaries;
        } else {
            j["history_summaries"] = json::array();
        }
        if (_session && !_session->todos.empty()) {
            json todosArr = json::array();
            for (const auto &t : _session->todos) {
                json o;
                o["id"] = t.id;
                o["content"] = t.content;
                o["status"] = t.status;
                todosArr.push_back(o);
            }
            j["todos"] = todosArr;
        } else {
            j["todos"] = json::array();
        }
        if (_session && !_session->scratchpad.empty()) {
            json scratch = json::object();
            for (const auto &kv : _session->scratchpad) {
                json item;
                item["title"] = kv.second.title;
                item["text"] = kv.second.text;
                scratch[kv.first] = item;
            }
            j["scratchpad"] = scratch;
        } else {
            j["scratchpad"] = json::object();
        }
        if (_session && _session->taskConfig && _session->taskConfig->usePlannerLayer &&
            !_session->currentPlannerStep.tool.empty()) {
            const auto &p = _session->currentPlannerStep;
            json ps;
            ps["tool"] = p.tool;
            ps["intent"] = p.intent;
            ps["text"] = p.text;
            j["planner_step"] = ps;
        } else {
            j["planner_step"] = nullptr;
        }
        return j.dump();
    }

    std::string LLMTaskAgent::buildPlannerPayload() const {
        using nlohmann::json;
        if (!_session || !_session->taskConfig) return "{}";
        json j;
        j["task_description"] = _session->taskConfig->taskDescription;
        if (!_session->todos.empty()) {
            json todosArr = json::array();
            for (const auto &t : _session->todos) {
                json o;
                o["id"] = t.id;
                o["content"] = t.content;
                o["status"] = t.status;
                todosArr.push_back(o);
            }
            j["todos"] = todosArr;
        } else {
            j["todos"] = json::array();
        }
        if (!_session->scratchpad.empty()) {
            json keys = json::array();
            for (const auto &kv : _session->scratchpad) keys.push_back(kv.first);
            j["scratchpad_keys"] = keys;
        } else {
            j["scratchpad_keys"] = json::array();
        }
        if (!_session->historySummaries.empty()) {
            j["history_summaries"] = _session->historySummaries;
        } else {
            j["history_summaries"] = json::array();
        }
        return j.dump();
    }

    std::string LLMTaskAgent::buildStepSummaryPayload(const StepHistoryEntry &entry) const {
        using nlohmann::json;
        json j;
        j["step_index"] = entry.stepIndex;
        j["action_type"] = entry.actionType;
        j["target_by"] = entry.targetBy;
        j["target_value"] = entry.targetValue;
        j["action_reason"] = entry.actionReason;
        return j.dump();
    }

    namespace {
        /** Try to get a single JSON object string from content that may be wrapped in markdown/code fence. */
        std::string extractJsonObjectString(const std::string &s) {
            size_t start = s.find('{');
            if (start == std::string::npos) return "";
            size_t end = s.rfind('}');
            if (end == std::string::npos || end < start) return "";
            return s.substr(start, end - start + 1);
        }
        /** Parse response to a JSON object; on failure tries extractJsonObjectString then parse. Returns true iff outJson is an object. */
        bool tryParseResponseToJson(const std::string &response, nlohmann::json &outJson) {
            try {
                outJson = nlohmann::json::parse(response);
                if (outJson.is_object()) return true;
                std::string sub = extractJsonObjectString(response);
                if (sub.empty()) return false;
                outJson = nlohmann::json::parse(sub);
                return outJson.is_object();
            } catch (...) {
                try {
                    std::string sub = extractJsonObjectString(response);
                    if (sub.empty()) return false;
                    outJson = nlohmann::json::parse(sub);
                    return outJson.is_object();
                } catch (...) {
                    return false;
                }
            }
        }
    }

    // Parses Planner JSON: object with tool (required), intent, text (see buildPlannerPrompt).
    bool LLMTaskAgent::parsePlannerResponse(const std::string &response, PlannerStep &outStep) const {
        nlohmann::json j;
        if (!tryParseResponseToJson(response, j)) return false;
        return parsePlannerResponseFromJson(&j, outStep);
    }

    bool LLMTaskAgent::parsePlannerResponseFromJson(const void *jsonPtr, PlannerStep &outStep) const {
        if (!jsonPtr) return false;
        using nlohmann::json;
        const auto &j = *static_cast<const json *>(jsonPtr);
        if (!j.is_object()) return false;
        outStep.tool = j.value("tool", "");
        outStep.intent = j.value("intent", "");
        outStep.text = j.value("text", "");
        if (outStep.tool.empty()) return false;
        if (outStep.tool != "finish_task" && outStep.tool != "go_back" && outStep.intent.empty() && outStep.text.empty())
            return false;
        return true;
    }

    namespace {
        bool isValidTaskStatus(const std::string &s) {
            return s == "ONGOING" || s == "COMPLETED" || s == "ABORT";
        }
        bool isValidActionType(const std::string &s) {
            return s == "CLICK" || s == "INPUT" || s == "SCROLL" || s == "BACK" || s == "WAIT" || s == "STATUS";
        }
        bool isTargetOptionalForAction(const std::string &actionType) {
            return actionType == "STATUS" || actionType == "WAIT";
        }
        std::string truncateForLog(const std::string &s, size_t maxLen) {
            if (s.size() <= maxLen) return s;
            return s.substr(0, maxLen) + "...";
        }

        constexpr size_t kMaxRawPromptLogLen = 4096;
        constexpr size_t kMaxRawResponseLogLen = 2048;

        /** Format prompt for logging: full text (truncated) + image placeholder so raw prompt is visible without dumping base64. */
        std::string formatPromptForLog(const std::string &prompt, const std::vector<ImageData> &images) {
            std::string out = truncateForLog(prompt, kMaxRawPromptLogLen);
            if (!images.empty()) {
                size_t totalBytes = 0;
                for (const auto &img : images) totalBytes += img.bytes.size();
                out += "\n[image: ";
                out += std::to_string(images.size());
                out += " image(s), total ";
                out += std::to_string(totalBytes);
                out += " bytes]";
            }
            return out;
        }
    }

    namespace {
        /** Parse a single tool call into LlmActionSpec (action_type, target, text, direction). Returns true if valid. */
        bool parseOneToolCall(const nlohmann::json &toolObj, LlmActionSpec &outSpec) {
            std::string name = toolObj.value("name", "");
            if (name.empty()) return false;
            nlohmann::json args = nlohmann::json::object();
            if (toolObj.contains("arguments")) {
                if (toolObj["arguments"].is_object()) {
                    args = toolObj["arguments"];
                } else if (toolObj["arguments"].is_string()) {
                    try {
                        args = nlohmann::json::parse(toolObj["arguments"].get<std::string>());
                    } catch (...) {
                        return false;
                    }
                }
            }

            if (name == "click") {
                outSpec.actionType = "CLICK";
                if (args.contains("index") && args["index"].is_number_integer()) {
                    outSpec.targetBy = "INDEX";
                    outSpec.targetValue = std::to_string(args["index"].get<int>());
                } else {
                    return false;
                }
            } else if (name == "input_text") {
                outSpec.actionType = "INPUT";
                if (args.contains("index") && args["index"].is_number_integer()) {
                    outSpec.targetBy = "INDEX";
                    outSpec.targetValue = std::to_string(args["index"].get<int>());
                } else {
                    return false;
                }
                outSpec.text = args.value("text", "");
            } else if (name == "scroll") {
                outSpec.actionType = "SCROLL";
                outSpec.direction = args.value("direction", "DOWN");
                if (args.contains("index") && args["index"].is_number_integer()) {
                    outSpec.targetBy = "INDEX";
                    outSpec.targetValue = std::to_string(args["index"].get<int>());
                }
            } else if (name == "back") {
                outSpec.actionType = "BACK";
            } else if (name == "wait") {
                outSpec.actionType = "WAIT";
                if (args.contains("duration_ms") && args["duration_ms"].is_number_integer()) {
                    outSpec.text = std::to_string(args["duration_ms"].get<int>());
                }
            } else if (name == "status") {
                outSpec.actionType = "STATUS";
            } else {
                return false;
            }
            outSpec.reason = args.value("reason", "");
            return true;
        }
    }

    bool LLMTaskAgent::parseLlmResponseFromJson(const void *jsonPtr, LlmActionSpec &outSpec,
                                                const std::string &rawResponsePrefix) const {
        if (!jsonPtr) return false;
        using nlohmann::json;
        const auto &j = *static_cast<const nlohmann::json *>(jsonPtr);
        if (!j.is_object()) {
            BDLOGE("LLMTaskAgent: LLM response is not a JSON object; prefix=%.80s", rawResponsePrefix.c_str());
            return false;
        }
        outSpec = LlmActionSpec();
        outSpec.taskStatus = j.value("task_status", "");
            if (outSpec.taskStatus.empty()) {
                BDLOGE("LLMTaskAgent: missing task_status");
                return false;
            }
            if (!isValidTaskStatus(outSpec.taskStatus)) {
                BDLOGE("LLMTaskAgent: invalid task_status '%s' (must be ONGOING/COMPLETED/ABORT)", outSpec.taskStatus.c_str());
                return false;
            }

            // Optional: tool_calls format (e.g. [{ "name": "click", "arguments": { "index": 1 } }])
            if (j.contains("tool_calls") && j["tool_calls"].is_array() && !j["tool_calls"].empty()) {
                const json &first = j["tool_calls"][0];
                if (first.is_object() && parseOneToolCall(first, outSpec)) {
                    return true;
                }
                // Fall through to action object if tool_calls parse failed
            }

            if (!j.contains("action") || !j["action"].is_object()) {
                BDLOGE("LLMTaskAgent: missing action object");
                return false;
            }
            const json &action = j["action"];

            outSpec.actionType = action.value("action_type", "");
            if (outSpec.actionType.empty()) {
                BDLOGE("LLMTaskAgent: missing action_type");
                return false;
            }
            if (!isValidActionType(outSpec.actionType)) {
                BDLOGE("LLMTaskAgent: invalid action_type '%s' (must be CLICK/INPUT/SCROLL/BACK/WAIT/STATUS)", outSpec.actionType.c_str());
                return false;
            }

            bool needTarget = !isTargetOptionalForAction(outSpec.actionType);
            if (action.contains("target") && action["target"].is_object()) {
                const json &target = action["target"];
                outSpec.targetBy = target.value("by", "");
                outSpec.targetValue = target.value("value", "");
            }
            if (needTarget && (outSpec.targetBy.empty() || outSpec.targetValue.empty())) {
                BDLOGE("LLMTaskAgent: invalid target selector (by=%s, value=%s) for action_type=%s",
                       outSpec.targetBy.c_str(), outSpec.targetValue.c_str(), outSpec.actionType.c_str());
                return false;
            }

            outSpec.text = action.value("text", "");
            outSpec.direction = action.value("direction", "");
            outSpec.reason = action.value("reason", "");

            outSpec.fallbacks.clear();
            if (action.contains("fallbacks") && action["fallbacks"].is_array()) {
                for (const auto &fb : action["fallbacks"]) {
                    if (!fb.is_object()) continue;
                    std::string fbBy = fb.value("by", "");
                    std::string fbVal = fb.value("value", "");
                    if (!fbBy.empty() && !fbVal.empty()) {
                        outSpec.fallbacks.emplace_back(fbBy, fbVal);
                    }
                }
            }

            return true;
    }

    bool LLMTaskAgent::parseLlmResponse(const std::string &response,
                                        LlmActionSpec &outSpec) const {
        nlohmann::json j;
        if (!tryParseResponseToJson(response, j)) {
            BDLOGE("LLMTaskAgent: parse LLM response failed; raw prefix: %s", truncateForLog(response, 200).c_str());
            return false;
        }
        return parseLlmResponseFromJson(&j, outSpec, truncateForLog(response, 200));
    }

    ElementPtr LLMTaskAgent::findFirstMatchedElement(const XpathPtr &xpathSelector,
                                                     const ElementPtr &elementXml) const {
        if (!xpathSelector || !elementXml) {
            return nullptr;
        }
        if (elementXml->matchXpathSelector(xpathSelector)) {
            return elementXml;
        }
        const auto &children = elementXml->getChildren();
        for (const auto &child : children) {
            ElementPtr found = findFirstMatchedElement(xpathSelector, child);
            if (found) {
                return found;
            }
        }
        return nullptr;
    }

    ElementPtr LLMTaskAgent::findTargetElement(const std::string &by,
                                               const std::string &value,
                                               const ElementPtr &rootXml) const {
        if (!rootXml) {
            return nullptr;
        }
        if (by == "XPATH") {
            XpathPtr xp = std::make_shared<Xpath>(value);
            return findFirstMatchedElement(xp, rootXml);
        }
        if (by == "TEXT") {
            std::vector<ElementPtr> stack;
            stack.push_back(rootXml);
            while (!stack.empty()) {
                ElementPtr elem = stack.back();
                stack.pop_back();
                if (!elem) {
                    continue;
                }
                const auto &children = elem->getChildren();
                for (const auto &c : children) {
                    stack.push_back(c);
                }
                if (elem->getText() == value) {
                    return elem;
                }
            }
        }
        if (by == "INDEX") {
            // Find element by index (matching the index used in buildPrompt)
            int targetIndex = -1;
            try {
                targetIndex = std::stoi(value);
            } catch (...) {
                return nullptr;
            }
            if (targetIndex < 0) {
                return nullptr;
            }
            int currentIndex = 0;
            std::vector<ElementPtr> stack;
            stack.push_back(rootXml);
            while (!stack.empty() && currentIndex <= targetIndex) {
                ElementPtr elem = stack.back();
                stack.pop_back();
                if (!elem) {
                    continue;
                }
                const auto &children = elem->getChildren();
                for (const auto &c : children) {
                    stack.push_back(c);
                }
                if (elem->getClickable() || elem->getLongClickable() || elem->getScrollable()) {
                    if (currentIndex == targetIndex) {
                        return elem;
                    }
                    ++currentIndex;
                }
            }
        }
        return nullptr;
    }

    /** Use interactiveElements for INDEX when provided to avoid second DFS; otherwise findTargetElement. */
    ElementPtr LLMTaskAgent::resolveTargetElement(const std::string &by, const std::string &value,
                                                  const ElementPtr &rootXml,
                                                  const std::vector<ElementPtr> *interactiveElements) const {
        if (by == "INDEX" && interactiveElements && !interactiveElements->empty()) {
            int idx = -1;
            try { idx = std::stoi(value); } catch (...) { return nullptr; }
            if (idx >= 0 && static_cast<size_t>(idx) < interactiveElements->size())
                return (*interactiveElements)[static_cast<size_t>(idx)];
        }
        return findTargetElement(by, value, rootXml);
    }

    ActionPtr LLMTaskAgent::convertToAction(const LlmActionSpec &spec,
                                            const ElementPtr &rootXml,
                                            const std::string &activity,
                                            ConvertFailureReason *outFailure,
                                            const std::vector<ElementPtr> *interactiveElements) const {
        if (outFailure) {
            *outFailure = ConvertFailureReason::None;
        }
        // Handle STATUS: just update task status, no actual action needed.
        if (spec.actionType == "STATUS") {
            // STATUS is used to signal completion/abort without executing any action.
            // Return NOP action as a placeholder.
            return std::make_shared<Action>(ActionType::NOP);
        }

        // Handle WAIT: create a NOP action with waitTime set.
        if (spec.actionType == "WAIT") {
            auto waitAction = std::make_shared<CustomAction>(ActionType::NOP);
            waitAction->activity = activity;
            // Parse wait time from text field or use default (e.g., 1000ms)
            int waitMs = 1000;
            if (!spec.text.empty()) {
                try {
                    waitMs = std::stoi(spec.text);
                    if (waitMs < 0) waitMs = 0;
                    if (waitMs > 10000) waitMs = 10000; // Cap at 10 seconds
                } catch (...) {
                    // Use default if parsing fails
                }
            }
            waitAction->waitTime = waitMs;
            return waitAction;
        }

        // Handle BACK as a dedicated back action.
        if (spec.actionType == "BACK") {
            return std::make_shared<Action>(ActionType::BACK);
        }

        // Handle SCROLL: map direction to appropriate ActionType.
        if (spec.actionType == "SCROLL") {
            ElementPtr targetElem = nullptr;
            if (!spec.targetValue.empty() && spec.targetBy != "NONE") {
                targetElem = resolveTargetElement(spec.targetBy, spec.targetValue, rootXml, interactiveElements);
                if (!targetElem && !spec.fallbacks.empty()) {
                    for (const auto &fb : spec.fallbacks) {
                        targetElem = resolveTargetElement(fb.first, fb.second, rootXml, interactiveElements);
                        if (targetElem) break;
                    }
                }
            }
            
            ActionType scrollType = ActionType::NOP;
            if (spec.direction == "UP" || spec.direction == "TOP_DOWN") {
                scrollType = ActionType::SCROLL_TOP_DOWN;
            } else if (spec.direction == "DOWN" || spec.direction == "BOTTOM_UP") {
                scrollType = ActionType::SCROLL_BOTTOM_UP;
            } else if (spec.direction == "LEFT" || spec.direction == "LEFT_RIGHT") {
                scrollType = ActionType::SCROLL_LEFT_RIGHT;
            } else if (spec.direction == "RIGHT" || spec.direction == "RIGHT_LEFT") {
                scrollType = ActionType::SCROLL_RIGHT_LEFT;
            } else {
                // Default to vertical scroll down if direction not specified
                scrollType = ActionType::SCROLL_BOTTOM_UP;
            }

            auto scrollAction = std::make_shared<CustomAction>(scrollType);
            scrollAction->activity = activity;
            
            // If target element found, use its bounds; otherwise use viewport center
            if (targetElem) {
                RectPtr bounds = targetElem->getBounds();
                if (bounds && !bounds->isEmpty()) {
                    scrollAction->bounds.resize(4);
                    scrollAction->bounds[0] = static_cast<float>(bounds->left);
                    scrollAction->bounds[1] = static_cast<float>(bounds->top);
                    scrollAction->bounds[2] = static_cast<float>(bounds->right);
                    scrollAction->bounds[3] = static_cast<float>(bounds->bottom);
                }
            }
            // If no bounds set, the Java layer will use default viewport scrolling
            
            return scrollAction;
        }

        // Handle CLICK and INPUT: require a target element.
        if (spec.actionType != "CLICK" && spec.actionType != "INPUT") {
            BDLOGE("LLMTaskAgent: unsupported action_type '%s'", spec.actionType.c_str());
            if (outFailure) {
                *outFailure = ConvertFailureReason::Other;
            }
            return nullptr;
        }

        ElementPtr targetElem = resolveTargetElement(spec.targetBy, spec.targetValue, rootXml, interactiveElements);
        if (!targetElem && !spec.fallbacks.empty()) {
            for (const auto &fb : spec.fallbacks) {
                targetElem = resolveTargetElement(fb.first, fb.second, rootXml, interactiveElements);
                if (targetElem) break;
            }
        }
        
        if (!targetElem) {
            BDLOGE("LLMTaskAgent: target element not found (by=%s, value=%s)", 
                   spec.targetBy.c_str(), spec.targetValue.c_str());
            if (outFailure) {
                *outFailure = ConvertFailureReason::NotFound;
            }
            return nullptr;
        }

        // Safe mode: avoid clicking elements whose text/content-desc contains forbidden strings (substring match).
        if (_session && _session->taskConfig && _session->taskConfig->safeMode) {
            const auto &forbidden = _session->taskConfig->forbiddenTexts;
            if (!forbidden.empty()) {
                const std::string txt = targetElem->getText();
                const std::string desc = targetElem->getContentDesc();
                for (const auto &f : forbidden) {
                    if (!f.empty() && (txt.find(f) != std::string::npos || desc.find(f) != std::string::npos)) {
                        BDLOGE("LLMTaskAgent: target element contains forbidden text '%s' (safety abort)", f.c_str());
                        if (outFailure) {
                            *outFailure = ConvertFailureReason::Forbidden;
                        }
                        return nullptr;
                    }
                }
            }
        }

        RectPtr bounds = targetElem->getBounds();
        if (!bounds || bounds->isEmpty()) {
            BDLOGE("LLMTaskAgent: target element has empty bounds");
            if (outFailure) {
                *outFailure = ConvertFailureReason::Other;
            }
            return nullptr;
        }

        auto customAction = std::make_shared<CustomAction>(ActionType::CLICK);
        customAction->activity = activity;
        customAction->bounds.resize(4);
        customAction->bounds[0] = static_cast<float>(bounds->left);
        customAction->bounds[1] = static_cast<float>(bounds->top);
        customAction->bounds[2] = static_cast<float>(bounds->right);
        customAction->bounds[3] = static_cast<float>(bounds->bottom);

        if (spec.actionType == "INPUT") {
            customAction->text = spec.text;
            customAction->clearText = true;
        }

        return customAction;
    }

    void LLMTaskAgent::appendLocalSummary(const LlmActionSpec &spec) {
        if (!_session) {
            return;
        }
        std::ostringstream oss;
        oss << "Step " << _session->stepCount
            << ": action=" << spec.actionType
            << " target(" << spec.targetBy << ")=" << spec.targetValue;
        if (!spec.reason.empty()) {
            oss << " reason=" << spec.reason;
        }
        _session->historySummaries.push_back(oss.str());
        if (_session->historySummaries.size() > kMaxHistory) {
            _session->historySummaries.erase(_session->historySummaries.begin());
        }
    }

    std::string LLMTaskAgent::requestStepSummaryFromLlm(const StepHistoryEntry &entry) const {
        if (!_llmClient || !_session || !_session->taskConfig) {
            return {};
        }
        if (!_session->taskConfig->useLlmForStepSummary) {
            return {};
        }
        std::string payload = buildStepSummaryPayload(entry);
        BDLOG("LLMTaskAgent: [StepSummary] payload len=%zu", payload.size());
        std::string response;
        std::vector<ImageData> noImages;
        if (!_llmClient->predictWithPayload("step_summary", payload, noImages, response)) {
            BDLOGE("LLMTaskAgent: LLM step summary request failed");
            return {};
        }
        BDLOG("LLMTaskAgent: [StepSummary] raw response:\n%s", truncateForLog(response, kMaxRawResponseLogLen).c_str());
        // Trim and take first line (up to 200 chars) as summary
        auto trim = [](std::string &s) {
            while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
            size_t i = 0;
            while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
            s = s.substr(i);
        };
        trim(response);
        size_t firstNewline = response.find('\n');
        if (firstNewline != std::string::npos) {
            response = response.substr(0, firstNewline);
            trim(response);
        }
        if (response.size() > kMaxSummaryLen) {
            response = response.substr(0, kMaxSummaryLen);
            trim(response);
        }
        return response;
    }

    void LLMTaskAgent::applyTodoUpdatesFromResponse(const std::string &response) {
        if (!_session) return;
        try {
            using nlohmann::json;
            json j = json::parse(response);
            applyTodoUpdatesFromJson(&j);
        } catch (...) {
            // Ignore parse errors for optional todo_updates
        }
    }

    void LLMTaskAgent::applyTodoUpdatesFromJson(const void *jsonPtr) {
        if (!_session || !jsonPtr) return;
        const auto &j = *static_cast<const nlohmann::json *>(jsonPtr);
        if (!j.is_object() || !j.contains("todo_updates") || !j["todo_updates"].is_array()) return;
        // Merge by id: update existing by id, append new items without id or with new id; cap size.
        std::vector<TodoItem> merged = _session->todos;
        std::map<std::string, size_t> idToIndex;
        for (size_t i = 0; i < merged.size(); ++i)
            if (!merged[i].id.empty()) idToIndex[merged[i].id] = i;
        for (const auto &item : j["todo_updates"]) {
            if (!item.is_object()) continue;
            TodoItem t;
            t.id = item.value("id", "");
            t.content = item.value("content", "");
            t.status = item.value("status", "pending");
            t.priority = item.value("priority", 0);
            if (t.content.empty() && t.id.empty()) continue;
            if (!t.id.empty()) {
                auto it = idToIndex.find(t.id);
                if (it != idToIndex.end()) {
                    merged[it->second] = std::move(t);
                    continue;
                }
            }
            merged.push_back(std::move(t));
            if (!t.id.empty()) idToIndex[t.id] = merged.size() - 1;
            if (merged.size() >= kMaxTodos) break;
        }
        if (merged.size() > kMaxTodos)
            merged.resize(kMaxTodos);
        if (!merged.empty())
            _session->todos = std::move(merged);
    }

    void LLMTaskAgent::applyScratchpadUpdatesFromResponse(const std::string &response) {
        if (!_session) return;
        try {
            using nlohmann::json;
            json j = json::parse(response);
            applyScratchpadUpdatesFromJson(&j);
        } catch (...) {
            // Ignore parse errors for optional scratchpad_updates
        }
    }

    void LLMTaskAgent::applyScratchpadUpdatesFromJson(const void *jsonPtr) {
        if (!_session || !jsonPtr) return;
        const auto &j = *static_cast<const nlohmann::json *>(jsonPtr);
        if (!j.is_object() || !j.contains("scratchpad_updates") || !j["scratchpad_updates"].is_array()) return;
        size_t count = 0;
        for (const auto &item : j["scratchpad_updates"]) {
            if (!item.is_object()) continue;
            std::string key = item.value("key", "");
            if (key.empty()) continue;
            ScratchpadItem si;
            si.title = item.value("title", "");
            si.text = item.value("text", "");
            _session->scratchpad[key] = std::move(si);
            if (++count >= kMaxScratchpadItems) break;
        }
    }

    ActionPtr LLMTaskAgent::selectNextAction(const ElementPtr &rootXml,
                                             const std::string &activity,
                                             const std::string &deviceId,
                                             LlmTaskConfigPtr preMatchedLlmTask) {
        if (!_llmClient) {
            // LLM client is not configured yet; LLMTaskAgent is effectively disabled.
            return nullptr;
        }

        // 1) Try to enter a new session if we are currently idle (use pre-matched task from raw tree when provided).
        if (!this->inSession()) {
            if (!maybeStartSession(rootXml, activity, deviceId, preMatchedLlmTask)) {
                return nullptr;
            }
        }

        if (!_session || !_session->taskConfig) {
            return nullptr;
        }

        // 2) Check session limits (max_steps, max_duration, consecutive_failures).
        if (isSessionExpired()) {
            std::string reason = _session->abortReason.empty() ? "unknown" : _session->abortReason;
            BDLOGE("LLMTaskAgent: Session expired (reason=%s, stepCount=%d, consecutiveFailures=%d)", 
                   reason.c_str(), _session->stepCount, _session->consecutiveFailures);
            _session->aborted = true;
            if (_session->abortReason.empty()) {
                _session->abortReason = reason;
            }
            resetSession();
            return nullptr;
        }

        // 2b) v3 Planner: if enabled and no current sub-task, ask Planner for one semantic step.
        if (_session->taskConfig->usePlannerLayer && _session->currentPlannerStep.tool.empty()) {
            std::string plannerPayload = buildPlannerPayload();
            if (plannerPayload.empty()) {
                BDLOGE("LLMTaskAgent: buildPlannerPayload failed");
                return nullptr;
            }
            BDLOG("LLMTaskAgent: [Planner] payload len=%zu", plannerPayload.size());
            std::string plannerResponse;
            std::vector<ImageData> noImages;
            if (!_llmClient->predictWithPayload("planner", plannerPayload, noImages, plannerResponse)) {
                _session->consecutiveFailures += 1;
                BDLOGE("LLMTaskAgent: Planner LLM predict failed (consecutiveFailures=%d); check HttpLlmClient/LLM Java HTTP logs for cause (disabled? runner not registered? Java null? response parse?)", _session->consecutiveFailures);
                if (isSessionExpired()) {
                    _session->abortReason = "llm_error";
                    _session->aborted = true;
                    resetSession();
                }
                return nullptr;
            }
            BDLOG("LLMTaskAgent: [Planner] raw response:\n%s", truncateForLog(plannerResponse, kMaxRawResponseLogLen).c_str());
            nlohmann::json plannerJson;
            if (!tryParseResponseToJson(plannerResponse, plannerJson)) {
                _session->consecutiveFailures += 1;
                BDLOGE("LLMTaskAgent: Failed to parse Planner response (consecutiveFailures=%d)", _session->consecutiveFailures);
                if (isSessionExpired()) {
                    _session->abortReason = "parse_error";
                    _session->aborted = true;
                    resetSession();
                }
                return nullptr;
            }
            PlannerStep step;
            if (!parsePlannerResponseFromJson(&plannerJson, step)) {
                _session->consecutiveFailures += 1;
                BDLOGE("LLMTaskAgent: Failed to parse Planner response (consecutiveFailures=%d)", _session->consecutiveFailures);
                if (isSessionExpired()) {
                    _session->abortReason = "parse_error";
                    _session->aborted = true;
                    resetSession();
                }
                return nullptr;
            }
            _session->currentPlannerStep = step;
            applyTodoUpdatesFromJson(&plannerJson);
            if (step.tool == "finish_task") {
                _session->completed = true;
                _session->abortReason = "completed";
                BDLOG("LLMTaskAgent: Planner requested finish_task (stepCount=%d)", _session->stepCount);
                resetSession();
                return nullptr;
            }
            BDLOG("LLMTaskAgent: Planner step: %s intent=%s text=%.40s", step.tool.c_str(), step.intent.c_str(), step.text.c_str());
        }

        // 3) Build payload and call LLM (Executor); Java assembles prompt from payload to reduce JNI copy.
        InteractiveElementsResult interactive = getScreenFingerprintWithElements(rootXml);
        std::string currentScreenHash;
        std::string executorPayload = buildExecutorPayload(rootXml, activity, &currentScreenHash, &interactive.fingerprint);
        if (_session && !currentScreenHash.empty()) {
            _session->recentScreenHashes.push_back(currentScreenHash);
            if (_session->recentScreenHashes.size() > kMaxScreenHashes) {
                _session->recentScreenHashes.erase(_session->recentScreenHashes.begin());
            }
        }
        std::string rawResponse;
        std::vector<ImageData> images;  // Empty on Java path; image obtained in Java on demand.

        BDLOG("LLMTaskAgent: [Executor] payload len=%zu", executorPayload.size());
        bool ok = _llmClient->predictWithPayload("executor", executorPayload, images, rawResponse);
        if (!ok) {
            _session->consecutiveFailures += 1;
            if (_session->taskConfig->usePlannerLayer && !_session->currentPlannerStep.tool.empty()) {
                _session->plannerStepFailureCount += 1;
                if (_session->plannerStepFailureCount >= kMaxPlannerStepFailures) {
                    _session->currentPlannerStep = PlannerStep();
                    _session->plannerStepFailureCount = 0;
                    BDLOG("LLMTaskAgent: Planner step failed %d times, asking Planner for next step", kMaxPlannerStepFailures);
                }
            }
            BDLOGE("LLMTaskAgent: Executor LLM predict failed (consecutiveFailures=%d); check HttpLlmClient/LLM Java HTTP logs for cause", _session->consecutiveFailures);
            if (isSessionExpired()) {
                _session->abortReason = "llm_error";
                _session->aborted = true;
                resetSession();
            }
            return nullptr;
        }

        BDLOG("LLMTaskAgent: [Executor] raw response:\n%s", truncateForLog(rawResponse, kMaxRawResponseLogLen).c_str());

        // Parse once (with extract fallback) and reuse for spec + todo_updates + scratchpad_updates (avoid apply*FromResponse double parse).
        nlohmann::json parsedResponse;
        bool hasObject = tryParseResponseToJson(rawResponse, parsedResponse);
        LlmActionSpec spec;
        if (hasObject) {
            applyTodoUpdatesFromJson(&parsedResponse);
            applyScratchpadUpdatesFromJson(&parsedResponse);
        }
        if (hasObject && parseLlmResponseFromJson(&parsedResponse, spec, rawResponse.empty() ? "" : rawResponse.substr(0, 200))) {
            // spec and todo/scratchpad done with single parse
        } else {
            if (!parseLlmResponse(rawResponse, spec)) {
                _session->consecutiveFailures += 1;
                if (_session->taskConfig->usePlannerLayer && !_session->currentPlannerStep.tool.empty()) {
                    _session->plannerStepFailureCount += 1;
                    if (_session->plannerStepFailureCount >= kMaxPlannerStepFailures) {
                        _session->currentPlannerStep = PlannerStep();
                        _session->plannerStepFailureCount = 0;
                        BDLOG("LLMTaskAgent: Planner step failed %d times, asking Planner for next step", kMaxPlannerStepFailures);
                    }
                }
                BDLOGE("LLMTaskAgent: Failed to parse LLM response (consecutiveFailures=%d) responsePrefix=%.80s",
                       _session->consecutiveFailures, rawResponse.empty() ? "" : rawResponse.c_str());
                if (isSessionExpired()) {
                    _session->abortReason = "parse_error";
                    _session->aborted = true;
                    resetSession();
                }
                return nullptr;
            }
            if (!hasObject) {
                applyTodoUpdatesFromResponse(rawResponse);
                applyScratchpadUpdatesFromResponse(rawResponse);
            }
        }

        _session->stepCount += 1;

        // If the model indicates completion, terminate the session and let RL resume.
        if (spec.taskStatus == "COMPLETED") {
            _session->completed = true;
            _session->abortReason = "completed";
            BDLOG("LLMTaskAgent: Task completed successfully (stepCount=%d)", _session->stepCount);
            resetSession();
            return nullptr;
        }
        if (spec.taskStatus == "ABORT") {
            _session->aborted = true;
            _session->abortReason = "llm_abort";
            BDLOG("LLMTaskAgent: LLM requested abort (stepCount=%d)", _session->stepCount);
            resetSession();
            return nullptr;
        }

        // 4) Convert LLM action to a concrete ActionPtr (reuse interactive.elements for INDEX lookup).
        ConvertFailureReason convertFailure = ConvertFailureReason::None;
        ActionPtr action = convertToAction(spec, rootXml, activity, &convertFailure, &interactive.elements);
        if (!action) {
            if (convertFailure == ConvertFailureReason::Forbidden) {
                _session->aborted = true;
                _session->abortReason = "safety_forbidden";
                BDLOGE("LLMTaskAgent: Safety abort (forbidden text), terminating session (stepCount=%d)", _session->stepCount);
                resetSession();
                return nullptr;
            }
            _session->consecutiveFailures += 1;
            if (_session->taskConfig->usePlannerLayer && !_session->currentPlannerStep.tool.empty()) {
                _session->plannerStepFailureCount += 1;
                if (_session->plannerStepFailureCount >= kMaxPlannerStepFailures) {
                    _session->currentPlannerStep = PlannerStep();
                    _session->plannerStepFailureCount = 0;
                    BDLOG("LLMTaskAgent: Planner step failed %d times, asking Planner for next step", kMaxPlannerStepFailures);
                }
            }
            BDLOGE("LLMTaskAgent: Failed to convert action (consecutiveFailures=%d, actionType=%s, targetBy=%s, targetValue=%s)", 
                   _session->consecutiveFailures, spec.actionType.c_str(), 
                   spec.targetBy.c_str(), spec.targetValue.c_str());
            if (isSessionExpired()) {
                _session->abortReason = "convert_error";
                _session->aborted = true;
                resetSession();
            }
            return nullptr;
        }

        // Success: reset consecutive failure and planner-step failure counters, record step history, summary, log
        _session->consecutiveFailures = 0;
        _session->plannerStepFailureCount = 0;

        StepHistoryEntry entry;
        entry.stepIndex = _session->stepCount;
        entry.actionType = spec.actionType;
        entry.targetBy = spec.targetBy;
        entry.targetValue = spec.targetValue;
        entry.actionReason = spec.reason;
        entry.actionOutputJson = rawResponse.size() > 300 ? rawResponse.substr(0, 300) + "..." : rawResponse;
        _session->history.push_back(entry);
        if (_session->history.size() > kMaxHistoryEntries) {
            _session->history.erase(_session->history.begin());
        }

        if (_session->taskConfig->useLlmForStepSummary) {
            std::string llmSummary = requestStepSummaryFromLlm(entry);
            if (!llmSummary.empty()) {
                _session->historySummaries.push_back(llmSummary);
            } else {
                appendLocalSummary(spec);
            }
        } else {
            appendLocalSummary(spec);
        }
        if (_session->historySummaries.size() > kMaxHistory) {
            _session->historySummaries.erase(_session->historySummaries.begin());
        }

        BDLOG("LLMTaskAgent: step %d action=%s target=%s/%s reason=%.40s",
              _session->stepCount, spec.actionType.c_str(), spec.targetBy.c_str(), spec.targetValue.c_str(),
              spec.reason.empty() ? "" : spec.reason.c_str());
        // v3: clear Planner sub-task so next step will ask Planner again for the next semantic step
        if (_session->taskConfig->usePlannerLayer) {
            _session->currentPlannerStep = PlannerStep();
        }
        return action;
    }

} // namespace fastbotx

