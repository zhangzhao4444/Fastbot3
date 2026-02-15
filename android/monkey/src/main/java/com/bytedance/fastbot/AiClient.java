/*
 * This code is licensed under the Fastbot license. You may obtain a copy of this license in the LICENSE.txt file in the root directory of this source tree.
 */

package com.bytedance.fastbot;

import android.graphics.PointF;
import android.os.Build;
import android.os.SystemClock;

import com.android.commands.monkey.fastbot.client.Operate;
import com.android.commands.monkey.fastbot.client.OperateResult;
import com.android.commands.monkey.utils.Logger;

import android.util.Base64;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.net.HttpURLConnection;
import java.net.URL;
import java.nio.ByteBuffer;
import org.json.JSONArray;
import org.json.JSONException;
import org.json.JSONObject;

import java.io.IOException;
import java.util.Iterator;
import java.util.concurrent.TimeUnit;

/**
 * @author Jianqiang Guo, Zhao Zhang
 */

public class AiClient {

    /**
     * Called when native triggers an LLM HTTP request (doLlmHttpPostFromPrompt). Implementations
     * should capture the current screen and return PNG bytes so every LLM request has an image
     * without capturing every step.
     */
    public interface LlmScreenshotProvider {
        byte[] captureForLlm();
    }

    private static volatile LlmScreenshotProvider sLlmScreenshotProvider;

    /** Set by monkey source so LLM requests capture screenshot on demand (no per-step capture). */
    public static void setLlmScreenshotProvider(LlmScreenshotProvider provider) {
        sLlmScreenshotProvider = provider;
    }

    /** Task output directory for LLM req/resp dumps (same as other task logs). Set by MonkeySourceApeNative. */
    private static volatile File sLlmDumpDirectory;

    public static void setLlmDumpDirectory(File taskOutputDir) {
        sLlmDumpDirectory = taskOutputDir;
    }

    private static final AiClient singleton;

    static {
        boolean success;
        long begin = SystemClock.elapsedRealtimeNanos();
        success = tryToLoadNativeLib(false);
        if (!success){
            success = tryToLoadNativeLib(true);
        }
        long end = SystemClock.elapsedRealtimeNanos();
        Logger.infoFormat("load fastbot_native takes %d ms.", TimeUnit.NANOSECONDS.toMillis(end - begin));
        singleton = new AiClient(success);
        if (success) {
            singleton.nativeRegisterLlmHttpRunner();
        }
    }

    /**
     * Agent algorithm types (must align with native fastbotx::AlgorithmType).
     *
     * Random:       placeholder for future random agent (currently unused).
     * Dfs:          depth-first-search exploration agent.
     * Bfs:          breadth-first-search exploration agent (reserved).
     * DoubleSarsa:  Double SARSA reinforcement learning agent with reuse model.
     * Frontier:     frontier-based exploration agent (infoGain × distance, MA-SLAM style).
     */
    public enum AlgorithmType {
        Random(0),
        Dfs(2),
        Bfs(4),
        DoubleSarsa(8),
        Frontier(16);

        private final int _value;

        AlgorithmType(int value) {
            this._value = value;
        }

        public int value() {
            return this._value;
        }
    }

    public static void InitAgent(AlgorithmType agentType, String packagename) {
        singleton.initAgentNative(agentType.value(), packagename, 0);
    }

    private boolean loaded = false;

    /** Fallback when no LlmScreenshotProvider is set (e.g. tests). */
    private byte[] lastScreenshotForLlm;

    protected AiClient(boolean success) {
        loaded = success;
    }

    private static boolean tryToLoadNativeLib(boolean fromAPK){
        String path = "";
        try {
            path = getAiPath(fromAPK);
            System.load(path);
            Logger.println("fastbot native : library load!");
            Logger.println("fastbot native path is : "+path);
        } catch (UnsatisfiedLinkError e) {
            Logger.errorPrintln("Error: Could not load library!");
            Logger.errorPrintln(path);
            Logger.errorPrintln(e.toString());
            return false;
        }
        return true;
    }

    private static boolean abiMatches(String[] abis, String abi) {
        for (String a : abis) {
            if (abi.equals(a)) return true;
        }
        return false;
    }

    private static String getAiPathFromAPK() {
        String[] abis = Build.SUPPORTED_ABIS;
        if (abiMatches(abis, "x86_64")) {
            return "/data/local/tmp/monkey.apk!/lib/x86_64/libfastbot_native.so";
        } else if (abiMatches(abis, "x86")) {
            return "/data/local/tmp/monkey.apk!/lib/x86/libfastbot_native.so";
        } else if (abiMatches(abis, "arm64-v8a")) {
            return "/data/local/tmp/monkey.apk!/lib/arm64-v8a/libfastbot_native.so";
        } else {
            return "/data/local/tmp/monkey.apk!/lib/armeabi-v7a/libfastbot_native.so";
        }
    }

    private static String getAiPathLocally() {
        String[] abis = Build.SUPPORTED_ABIS;
        if (abiMatches(abis, "x86_64")) {
            return "/data/local/tmp/x86_64/libfastbot_native.so";
        } else if (abiMatches(abis, "x86")) {
            return "/data/local/tmp/x86/libfastbot_native.so";
        } else if (abiMatches(abis, "arm64-v8a")) {
            return "/data/local/tmp/arm64-v8a/libfastbot_native.so";
        } else {
            return "/data/local/tmp/armeabi-v7a/libfastbot_native.so";
        }
    }

    private static String getAiPath(boolean fromAPK) {
        if (Build.VERSION.SDK_INT <= com.android.commands.monkey.utils.AndroidVersions.API_22_ANDROID_5_1) {
            return getAiPathLocally();
        } else {
            return fromAPK ? getAiPathFromAPK() : getAiPathLocally();
        }
    }

    public static void loadResMapping(String resmapping) {
        if (!singleton.loaded) {
            Logger.println("// Error: Could not load native library!");
            Logger.println("Please report this bug issue to github");
            System.exit(1);
        }
        singleton.loadResMappingNative(resmapping);
    }

    public static Operate getAction(String activity, String pageDesc) {
        return singleton.getOperate(activity, pageDesc);
    }

    /**
     * Optional: set a pre-captured screenshot when no LlmScreenshotProvider is registered (e.g. tests).
     */
    public static void setLastScreenshotForLlm(byte[] png) {
        singleton.lastScreenshotForLlm = png;
    }

    /**
     * Build OpenAI-style request body (prompt + optional screenshot as base64) and POST.
     * Screenshot is taken on demand via LlmScreenshotProvider when native calls back for LLM request,
     * so we do not capture every step and every LLM request still gets an image.
     * Called from native via JNI when libcurl is not available.
     *
     * @param url       API endpoint
     * @param apiKey    Bearer token (may be empty)
     * @param prompt    User prompt text
     * @param model     Model name
     * @param maxTokens Max tokens
     * @return response body string, or null on failure
     */
    public String doLlmHttpPostFromPrompt(String url, String apiKey, String prompt, String model, int maxTokens) {
        if (url == null || url.isEmpty()) {
            Logger.errorPrintln("doLlmHttpPostFromPrompt: url null or empty");
            return null;
        }
        long tStart = System.currentTimeMillis();
        String body = buildLlmRequestBody(prompt, model, maxTokens);
        if (body == null) {
            Logger.errorPrintln("doLlmHttpPostFromPrompt: buildLlmRequestBody returned null");
            return null;
        }
        long tAfterBuild = System.currentTimeMillis();
        long ts = System.currentTimeMillis();
        saveLlmRawToFile(ts + "-req.json", body);
        String result = doLlmHttpPostBody(url, apiKey, body);
        long tEnd = System.currentTimeMillis();
        long buildMs = tAfterBuild - tStart;
        long requestMs = tEnd - tAfterBuild;
        long totalMs = tEnd - tStart;
        Logger.println("// [LLM timing] (ms) buildPrompt+body: " + buildMs + ", request: " + requestMs + ", total: " + totalMs);
        if (result != null) {
            saveLlmRawToFile(ts + "-resp.json", result);
            logLlmResponseMessage(result);
        } else {
            Logger.errorPrintln("doLlmHttpPostFromPrompt: doLlmHttpPostBody returned null (check HTTP code / network above)");
        }
        return result;
    }

    /**
     * LLM HTTP POST with prompt assembled in Java from payload JSON (reduces JNI string copy).
     * promptType: "executor" | "planner" | "step_summary".
     * Called from native via JNI when using predictWithPayload.
     */
    public String doLlmHttpPostFromPayload(String url, String apiKey, String promptType, String payloadJson, String model, int maxTokens) {
        if (url == null || url.isEmpty()) {
            Logger.errorPrintln("doLlmHttpPostFromPayload: url null or empty");
            return null;
        }
        String prompt = buildPromptFromPayload(promptType, payloadJson);
        if (prompt == null) {
            Logger.errorPrintln("doLlmHttpPostFromPayload: buildPromptFromPayload returned null");
            return null;
        }
        return doLlmHttpPostFromPrompt(url, apiKey, prompt, model, maxTokens);
    }

    /**
     * Build full prompt string from payload JSON (matches C++ AutodevAgent prompt content).
     */
    private static String buildPromptFromPayload(String promptType, String payloadJson) {
        try {
            JSONObject payload = new JSONObject((payloadJson == null || payloadJson.isEmpty()) ? "{}" : payloadJson);
            if ("executor".equals(promptType)) {
                return buildExecutorPrompt(payload);
            }
            if ("planner".equals(promptType)) {
                return buildPlannerPrompt(payload);
            }
            if ("step_summary".equals(promptType)) {
                return buildStepSummaryPrompt(payload);
            }
            Logger.errorPrintln("buildPromptFromPayload: unknown promptType=" + promptType);
            return null;
        } catch (JSONException e) {
            Logger.errorPrintln("buildPromptFromPayload: JSON parse failed " + e.getMessage());
            return null;
        }
    }

    private static String buildExecutorPrompt(JSONObject j) throws JSONException {
        StringBuilder sb = new StringBuilder();
        if (j.optBoolean("nav_hint", false)) {
            sb.append("Navigation hint: this screen was already seen recently; avoid repeating the same scroll or try a different strategy.\n\n");
        }
        JSONObject plannerStep = j.optJSONObject("planner_step");
        String tool = plannerStep != null ? plannerStep.optString("tool", "") : "";
        if (!tool.isEmpty()) {
            String intent = plannerStep.optString("intent", "");
            String text = plannerStep.optString("text", "");
            sb.append("=== EXECUTOR SUB-TASK (complete this fully) ===\n");
            if ("tap".equals(tool)) {
                sb.append("Locate and tap on the \"").append(intent).append("\" on the current screen. Choose the correct element index and perform CLICK.\n");
            } else if ("scroll".equals(tool)) {
                sb.append("Perform a scroll to \"").append(intent).append("\". Choose direction (UP/DOWN/LEFT/RIGHT) and optional target index. Use SCROLL action.\n");
            } else if ("type_text".equals(tool)) {
                sb.append("Locate the \"").append(intent).append("\" input field, then type the following text exactly: \"").append(text).append("\". Use INPUT with the chosen index and text.\n");
            } else if ("go_back".equals(tool)) {
                sb.append("Navigate back to the previous screen. Use BACK action.\n");
            } else if ("answer".equals(tool)) {
                sb.append("Provide the answer (no UI action): \"").append(text).append("\". Use task_status COMPLETED and optionally STATUS action.\n");
            } else {
                sb.append("Planner step: ").append(tool);
                if (!intent.isEmpty()) sb.append(" intent=\"").append(intent).append("\"");
                if (!text.isEmpty()) sb.append(" text=\"").append(text).append("\"");
                sb.append(". Execute this single step (choose the right element index / action).\n");
            }
            sb.append("Your action will be executed; the result is summarized for the Planner.\n\n");
        }
        sb.append("You are an Android GUI testing agent. ");
        sb.append("Given the current screen description and a task, you must output the next GUI action ");
        sb.append("in JSON format.\n\n");
        String taskDesc = j.optString("task_description", "");
        if (!taskDesc.isEmpty()) {
            sb.append("Task:\n").append(taskDesc).append("\n\n");
        }
        sb.append("Current activity: ").append(j.optString("activity", "")).append("\n\n");
        String screenFingerprint = j.optString("screen_fingerprint", "");
        if (!screenFingerprint.isEmpty()) {
            sb.append("Visible interactive elements (index, class, resource-id, text, content-desc):\n");
            sb.append(screenFingerprint).append("\n");
        }
        JSONArray historySummaries = j.optJSONArray("history_summaries");
        if (historySummaries != null && historySummaries.length() > 0) {
            sb.append("Recent steps summary:\n");
            for (int i = 0; i < historySummaries.length(); i++) {
                sb.append("- ").append(historySummaries.optString(i, "")).append("\n");
            }
            sb.append("\n");
        }
        JSONArray todos = j.optJSONArray("todos");
        if (todos != null && todos.length() > 0) {
            sb.append("Current todos:\n");
            for (int i = 0; i < todos.length(); i++) {
                JSONObject t = todos.optJSONObject(i);
                if (t != null) {
                    sb.append("  ").append(i + 1).append(". [").append(t.optString("status", "")).append("] ").append(t.optString("content", ""));
                    String id = t.optString("id", "");
                    if (!id.isEmpty()) sb.append(" (id=").append(id).append(")");
                    sb.append("\n");
                }
            }
            sb.append("You may update todos by including \"todo_updates\" in your JSON: [{\"id\":\"...\",\"content\":\"...\",\"status\":\"pending|in_progress|done\"}].\n\n");
        }
        JSONObject scratchpad = j.optJSONObject("scratchpad");
        if (scratchpad != null && scratchpad.length() > 0) {
            sb.append("Scratchpad (stored items, key -> title / content):\n");
            Iterator<String> keys = scratchpad.keys();
            while (keys.hasNext()) {
                String key = keys.next();
                JSONObject item = scratchpad.optJSONObject(key);
                if (item != null) {
                    sb.append("  ").append(key).append(" | title: ").append(item.optString("title", "")).append("\n  content: ").append(item.optString("text", "")).append("\n");
                }
            }
            sb.append("You may create/update items with \"scratchpad_updates\" in your JSON: [{\"key\":\"...\",\"title\":\"...\",\"text\":\"...\"}].\n\n");
        }
        sb.append("You must respond with a single valid JSON object. No extra text.\n");
        sb.append("task_status must be exactly one of: ONGOING, COMPLETED, ABORT.\n");
        sb.append("You may use either (1) action object or (2) tool_calls. Tool names: click(index), input_text(index,text), scroll(direction[,index]), back, wait([duration_ms]), status.\n");
        sb.append("action_type must be exactly one of: CLICK, INPUT, SCROLL, BACK, WAIT, STATUS.\n");
        sb.append("Example (action format):\n");
        sb.append("{\n  \"task_status\": \"ONGOING\",\n  \"action\": {\n    \"action_type\": \"CLICK\",\n    \"target\": { \"by\": \"INDEX\", \"value\": \"0\" },\n    \"text\": \"\",\n    \"reason\": \"short explanation\"\n  }\n}\n");
        sb.append("Example (tool_calls format): {\"task_status\":\"ONGOING\",\"tool_calls\":[{\"name\":\"click\",\"arguments\":{\"index\":0,\"reason\":\"...\"}}]}\n");
        return sb.toString();
    }

    private static String buildPlannerPrompt(JSONObject j) throws JSONException {
        StringBuilder sb = new StringBuilder();
        sb.append("You are an expert PLANNER for Android GUI automation. You output ONE semantic step per response. ");
        sb.append("You NEVER output coordinates or element indices; the Executor will choose the concrete action.\n\n");
        sb.append("**CRITICAL**: Give COMPLETE, DETAILED subgoals. The Executor has NO MEMORY - every instruction must be self-contained.\n\n");
        sb.append("Task: ").append(j.optString("task_description", "")).append("\n\n");
        JSONArray todos = j.optJSONArray("todos");
        if (todos != null && todos.length() > 0) {
            sb.append("Current todos:\n");
            for (int i = 0; i < todos.length(); i++) {
                JSONObject t = todos.optJSONObject(i);
                if (t != null) {
                    sb.append("  ").append(i + 1).append(". [").append(t.optString("status", "")).append("] ").append(t.optString("content", "")).append("\n");
                }
            }
            sb.append("You may include \"todo_updates\" in your JSON to replace/update the todo list: [{\"id\":\"...\",\"content\":\"...\",\"status\":\"pending|in_progress|completed\"}].\n\n");
        }
        JSONArray scratchpadKeys = j.optJSONArray("scratchpad_keys");
        if (scratchpadKeys != null && scratchpadKeys.length() > 0) {
            sb.append("Scratchpad keys (stored data): ");
            for (int i = 0; i < scratchpadKeys.length(); i++) {
                if (i > 0) sb.append(" ");
                sb.append(scratchpadKeys.optString(i, ""));
            }
            sb.append("\n\n");
        }
        JSONArray historySummaries = j.optJSONArray("history_summaries");
        if (historySummaries != null && historySummaries.length() > 0) {
            sb.append("Steps done so far (Executor reports):\n");
            for (int i = 0; i < historySummaries.length(); i++) {
                sb.append("- ").append(historySummaries.optString(i, "")).append("\n");
            }
            sb.append("\n");
        }
        sb.append("Tools (semantic only): tap(intent), scroll(intent), type_text(text,intent), answer(text), finish_task(), go_back().\n");
        sb.append("Respond with ONE JSON object: {\"tool\": \"tap\"|\"scroll\"|\"type_text\"|\"answer\"|\"finish_task\"|\"go_back\", ");
        sb.append("\"intent\": \"e.g. login button or scroll down to find X\", \"text\": \"for type_text/answer only\"}. ");
        sb.append("Optional: \"todo_updates\": [...] to update the todo list. Use finish_task when the task is complete.");
        return sb.toString();
    }

    private static String buildStepSummaryPrompt(JSONObject j) throws JSONException {
        StringBuilder sb = new StringBuilder();
        sb.append("You are summarizing a single GUI automation step. ");
        sb.append("Step index: ").append(j.optInt("step_index", 0)).append(". ");
        sb.append("Action: ").append(j.optString("action_type", "")).append(" target(").append(j.optString("target_by", "")).append(")=\"").append(j.optString("target_value", "")).append("\". ");
        String reason = j.optString("action_reason", "");
        if (!reason.isEmpty()) {
            sb.append("Reason: ").append(reason).append(". ");
        }
        sb.append("Reply with exactly one short sentence summarizing what was done, in the same language as the reason. No JSON.");
        return sb.toString();
    }

    private static void saveLlmRawToFile(String filename, String content) {
        if (content == null || sLlmDumpDirectory == null) return;
        try {
            if (!sLlmDumpDirectory.exists()) sLlmDumpDirectory.mkdirs();
            File f = new File(sLlmDumpDirectory, filename);
            try (FileOutputStream os = new FileOutputStream(f)) {
                os.write(content.getBytes(StandardCharsets.UTF_8));
            }
        } catch (Exception e) {
            Logger.errorPrintln("saveLlmRawToFile failed: " + filename + " " + e.getMessage());
        }
    }

    private String buildLlmRequestBody(String prompt, String model, int maxTokens) {
        try {
            long t0 = System.currentTimeMillis();
            byte[] img = null;
            if (sLlmScreenshotProvider != null) {
                img = sLlmScreenshotProvider.captureForLlm();
            }
            if (img == null || img.length == 0) {
                img = lastScreenshotForLlm;
            }
            long t1 = System.currentTimeMillis();
            long screenshotMs = t1 - t0;
            StringBuilder sb = new StringBuilder();
            sb.append("{\"model\":");
            sb.append(escapeJson(model != null ? model : ""));
            sb.append(",\"max_tokens\":").append(Math.max(0, maxTokens));
            sb.append(",\"stream\":false,\"messages\":[");
            sb.append("{\"role\":\"system\",\"content\":\"You are a GUI testing agent that must respond with a strict JSON action object.\"},");
            sb.append("{\"role\":\"user\",\"content\":");
            if (img != null && img.length > 0) {
                String b64 = Base64.encodeToString(img, Base64.NO_WRAP);
                String mime = isPngBytes(img) ? "image/png" : "image/jpeg";
                sb.append("[{\"type\":\"text\",\"text\":");
                sb.append(escapeJson(prompt != null ? prompt : ""));
                sb.append("},{\"type\":\"image_url\",\"image_url\":{\"url\":\"data:");
                sb.append(mime);
                sb.append(";base64,");
                sb.append(b64);
                sb.append("\"}}]}]}");
            } else {
                sb.append(escapeJson(prompt != null ? prompt : ""));
                sb.append("}]}");
            }
            long t2 = System.currentTimeMillis();
            long buildBodyMs = t2 - t1;
            Logger.println("// [LLM timing] (ms) screenshot: " + screenshotMs + ", assembleBody: " + buildBodyMs);
            return sb.toString();
        } catch (Exception e) {
            Logger.errorPrintln("buildLlmRequestBody failed: " + e.getMessage());
            return null;
        }
    }

    /**
     * Log key info from LLM chat response.
     * For executor responses, prefer printing the action.reason.
     * For planner responses, prefer intent/text.
     * Falls back to logging the inner content string if schema is unknown.
     */
    private void logLlmResponseMessage(String response) {
        if (response == null || response.isEmpty()) {
            return;
        }
        try {
            JSONObject root = new JSONObject(response);
            JSONArray choices = root.optJSONArray("choices");
            if (choices == null || choices.length() == 0) return;
            JSONObject choice0 = choices.optJSONObject(0);
            if (choice0 == null) return;
            JSONObject message = choice0.optJSONObject("message");
            String content = null;
            if (message != null) {
                content = message.optString("content", null);
            }
            if (content == null || content.isEmpty()) {
                // Some providers may put text directly under "message" or "content"
                content = choice0.optString("content",
                        choice0.optString("message", ""));
            }
            if (content == null || content.isEmpty()) return;

            // Try to parse inner content as JSON to extract task_status / reason / planner fields
            try {
                JSONObject inner = new JSONObject(content);

                // Task-level status, if provided by LLM: ONGOING / COMPLETED / ABORT
                String taskStatus = inner.optString("task_status", "");
                if (!taskStatus.isEmpty()) {
                    Logger.println("// [LLM task_status] " + taskStatus);
                }

                // Executor style: {"task_status": "...", "action": {..., "reason": "..."}}
                JSONObject action = inner.optJSONObject("action");
                if (action != null) {
                    String reason = action.optString("reason", "");
                    if (!reason.isEmpty()) {
                        Logger.println("// [LLM executor reason] " + reason);
                        return;
                    }
                }

                // Tool-calls style: {"tool_calls":[{"arguments":{"reason":"..."}}], ...}
                JSONArray toolCalls = inner.optJSONArray("tool_calls");
                if (toolCalls != null && toolCalls.length() > 0) {
                    JSONObject tc0 = toolCalls.optJSONObject(0);
                    if (tc0 != null) {
                        JSONObject args = tc0.optJSONObject("arguments");
                        if (args != null) {
                            String reason = args.optString("reason", "");
                            if (!reason.isEmpty()) {
                                Logger.println("// [LLM executor reason] " + reason);
                                return;
                            }
                        }
                    }
                }

                // Planner style: {"tool":"tap|scroll|type_text|answer|finish_task|go_back", "intent":"...", "text":"..."}
                String tool = inner.optString("tool", "");
                if (!tool.isEmpty()) {
                    String intent = inner.optString("intent", "");
                    String text = inner.optString("text", "");
                    StringBuilder sb = new StringBuilder();
                    sb.append("// [LLM planner] tool=").append(tool);
                    if (!intent.isEmpty()) {
                        sb.append(", intent=").append(intent);
                    }
                    if (!text.isEmpty()) {
                        sb.append(", text=").append(text);
                    }
                    Logger.println(sb.toString());
                    return;
                }

                // Fallback: content is JSON but no reason/intent; log one line to avoid full JSON noise
                Logger.println("// [LLM content JSON] " + inner.toString());
            } catch (JSONException ignore) {
                // Content is plain text, not JSON
                Logger.println("// [LLM content text] " + content);
            }
        } catch (JSONException e) {
            // Logger.errorPrintln("logLlmResponseMessage: JSON parse failed " + e.getMessage());
            // Logger.println("// [LLM raw response]\n" + response);
        }
    }

    /** PNG magic: 89 50 4E 47 0D 0A 1A 0A */
    private static boolean isPngBytes(byte[] img) {
        return img != null && img.length >= 8
                && img[0] == (byte) 0x89 && img[1] == 0x50 && img[2] == 0x4E && img[3] == 0x47;
    }

    private static String escapeJson(String s) {
        if (s == null) return "\"\"";
        StringBuilder sb = new StringBuilder(s.length() + 2);
        sb.append('"');
        for (int i = 0; i < s.length(); i++) {
            char c = s.charAt(i);
            if (c == '"') sb.append("\\\"");
            else if (c == '\\') sb.append("\\\\");
            else if (c == '\n') sb.append("\\n");
            else if (c == '\r') sb.append("\\r");
            else if (c == '\t') sb.append("\\t");
            else if (c < ' ') sb.append(String.format("\\u%04x", (int) c));
            else sb.append(c);
        }
        sb.append('"');
        return sb.toString();
    }

    private String doLlmHttpPostBody(String url, String apiKey, String body) {
        if (url == null || url.isEmpty()) return null;
        HttpURLConnection conn = null;
        try {
            URL u = new URL(url);
            conn = (HttpURLConnection) u.openConnection();
            conn.setRequestMethod("POST");
            conn.setRequestProperty("Content-Type", "application/json");
            if (apiKey != null && !apiKey.isEmpty()) {
                conn.setRequestProperty("Authorization", "Bearer " + apiKey);
            }
            conn.setConnectTimeout(15000);
            conn.setReadTimeout(20000);
            conn.setDoOutput(true);
            if (body != null && !body.isEmpty()) {
                byte[] bytes = body.getBytes(StandardCharsets.UTF_8);
                conn.setFixedLengthStreamingMode(bytes.length);
                try (OutputStream os = conn.getOutputStream()) {
                    os.write(bytes);
                }
            }
            int code = conn.getResponseCode();
            InputStream in = (code >= 200 && code < 300) ? conn.getInputStream() : conn.getErrorStream();
            if (code < 200 || code >= 300) {
                Logger.errorPrintln("doLlmHttpPostBody: HTTP " + code + " for url=" + url + " (expected 2xx; 404=wrong path, check max.llm.apiUrl e.g. https://.../v1/chat/completions)");
                if (in != null) {
                    drainStream(in);
                }
                return null;
            }
            if (in == null) return null;
            byte[] raw = readStreamToByteArray(in);
            return raw != null ? new String(raw, StandardCharsets.UTF_8) : null;
        } catch (Exception e) {
            Logger.errorPrintln("doLlmHttpPostBody failed: " + e.getMessage());
            return null;
        } finally {
            if (conn != null) conn.disconnect();
        }
    }

    private static void drainStream(InputStream in) {
        try {
            byte[] buf = new byte[4096];
            while (in.read(buf) > 0) { }
        } catch (IOException ignored) { }
    }

    private static byte[] readStreamToByteArray(InputStream in) throws IOException {
        ByteArrayOutputStream baos = new ByteArrayOutputStream();
        byte[] buf = new byte[4096];
        int n;
        while ((n = in.read(buf)) > 0) {
            baos.write(buf, 0, n);
        }
        return baos.toByteArray();
    }

    /**
     * Report current activity for coverage tracking (performance: coverage in C++, PERF §3.4).
     */
    public static void reportActivity(String activity) {
        if (activity != null && singleton.loaded) {
            singleton.reportActivityNative(activity);
        }
    }

    /**
     * Get coverage summary from native: {"stepsCount":N,"testedActivities":["a1",...]}
     */
    public static String getCoverageJson() {
        if (!singleton.loaded) return "{}";
        String s = singleton.getCoverageJsonNative();
        return s != null ? s : "{}";
    }

    /**
     * Get next fuzz action JSON from native (performance §3.3). Returns one fuzz action as JSON;
     * simplify=true picks from rotation/app_switch/drag/pinch/click only.
     */
    public static String getNextFuzzAction(int displayWidth, int displayHeight, boolean simplify) {
        if (!singleton.loaded) return null;
        return singleton.getNextFuzzActionNative(displayWidth, displayHeight, simplify);
    }

    /**
     * Get action from XML supplied as Direct ByteBuffer (performance: avoids JNI string copy).
     * Tries structured result first to avoid JSON parse (opt4); falls back to JSON if needed.
     * Image for LLM is obtained in Java on demand when native triggers HTTP (LlmScreenshotProvider).
     *
     * @param activity  current activity name
     * @param xmlBuffer direct ByteBuffer containing UTF-8 XML bytes
     * @return Operate object or null on error
     */
    public static Operate getActionFromBuffer(String activity, ByteBuffer xmlBuffer) {
        if (xmlBuffer == null || !xmlBuffer.isDirect() || xmlBuffer.remaining() <= 0) {
            return null;
        }
        int byteLength = xmlBuffer.remaining();
        OperateResult r = singleton.getActionFromBufferNativeStructured(activity, xmlBuffer, byteLength);
        if (r != null) {
            return Operate.fromOperateResult(r);
        }
        String operateStr = singleton.getActionFromBufferNative(activity, xmlBuffer, byteLength);
        if (operateStr == null || operateStr.length() < 1) {
            return null;
        }
        return Operate.fromJson(operateStr);
    }

    private native void loadResMappingNative(String resMapping);

    private native String getOperateJsonNative(String activity, String pageDesc);
    private native void initAgentNative(int algorithmType, String packageName, int flags);
    private native boolean checkPointInShieldNative(String activity, float x, float y);

    /**
     * Batch check: multiple points in one JNI call (performance optimization).
     * @param activity current activity name
     * @param xCoords x coordinates, same length as yCoords
     * @param yCoords y coordinates
     * @return array of booleans, true if point is in black rect (shielded), null if error or native not loaded
     */
    private native boolean[] checkPointsInShieldNative(String activity, float[] xCoords, float[] yCoords);

    /**
     * Get action from XML in Direct ByteBuffer (performance: avoid GetStringUTFChars copy, PERF §3.1).
     * Image for LLM is obtained in Java on demand when native triggers HTTP (no screenshot param).
     */
    private native String getActionFromBufferNative(String activity, ByteBuffer xmlBuffer, int byteLength);

    /** Structured result to avoid JSON parse (SECURITY_AND_OPTIMIZATION §7 opt4). Returns null on error. */
    private native OperateResult getActionFromBufferNativeStructured(String activity, ByteBuffer xmlBuffer, int byteLength);

    private native void reportActivityNative(String activity);
    private native String getCoverageJsonNative();
    private native String getNextFuzzActionNative(int displayWidth, int displayHeight, boolean simplify);

    /** Register this instance as the LLM HTTP runner for native when libcurl is not available. */
    private native void nativeRegisterLlmHttpRunner();

    public static native String getNativeVersion();

    public static boolean checkPointIsShield(String activity, PointF point) {
        return singleton.checkPointInShieldNative(activity, point.x, point.y);
    }

    /**
     * Batch check points for black rect (shield). Reduces JNI round-trips from up to N to 1.
     * @param activity current activity name
     * @param xCoords x coordinates
     * @param yCoords y coordinates (same length as xCoords)
     * @return array of booleans (true = in shield), or null if error; length same as input
     */
    public static boolean[] checkPointsInShield(String activity, float[] xCoords, float[] yCoords) {
        if (!singleton.loaded || xCoords == null || yCoords == null || xCoords.length != yCoords.length) {
            return null;
        }
        return singleton.checkPointsInShieldNative(activity, xCoords, yCoords);
    }

    /**
     * Gets the next Operate (action) from native layer by activity and page description XML.
     * Called by {@link #getAction(String, String)}.
     */
    public Operate getOperate(String activity, String pageDesc) {
        if (!loaded) {
            Logger.println("// Error: Could not load native library!");
            Logger.println("Please report this bug issue to github");
            System.exit(1);
        }
        String operateStr = getOperateJsonNative(activity, pageDesc);
        if (operateStr == null || operateStr.isEmpty()) {
            Logger.errorPrintln("native get operate failed " + operateStr);
            return null;
        }
        return Operate.fromJson(operateStr);
    }

}
