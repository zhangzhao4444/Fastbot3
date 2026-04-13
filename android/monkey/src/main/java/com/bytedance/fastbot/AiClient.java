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
import java.util.concurrent.Callable;
import java.util.concurrent.ExecutionException;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.Future;
import java.util.concurrent.ThreadFactory;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.TimeoutException;

/**
 * @author Jianqiang Guo, Zhao Zhang
 *
 * <p>Optional native build: CMake {@code FASTBOTX_APE_NATIVE_RECORD=ON} (with vendored pugixml) builds
 * APE {@code StateKey} in {@code Model::buildApeStateKeyFromElementTree} during {@code getOperateOpt} and
 * attaches sidecars via {@code recordApeStateKey}. Optional {@code max.apeGraphDedupByStateKey=true} merges
 * graph states by {@code StateKey} hash. When not in static reuse abstraction, native also aligns
 * {@code State::hash()} and {@code ActivityNameAction} hashes to the same APE identity (canonical activity
 * + abstract XPath targets where available). {@code max.apeNamingFixedPointSteps=N} enables per-step
 * {@code refineNaming}+{@code rebuildTree} in {@code StateNamingManager::getNamingFixedPoint} with
 * {@code StateKey} fixed-point early stop; {@code max.apeNamingPeriodicRefinement=false} turns off
 * the separate periodic naming refine batch; {@code max.apeNamingActionRefineHops=N} controls
 * periodic action-refinement hop search depth; {@code max.apeNamingActionRefineRequireFingerprintChange}
 * controls whether periodic refinement requires fingerprint change; {@code max.apeNamingActionRefinePredicateMode}
 * controls candidate acceptance mode (fingerprint_change / always_accept / fineness_increase);
 * {@code max.apeNamingActionRefineSelectionMode} controls whether to take first or deepest acceptable hop;
 * {@code max.apeNamingActionRefineMinActivityStates} sets minimum activity state count before periodic refine;
 * {@code max.apeNamingActionRefineMinNonDetPairs} sets minimum non-deterministic (stateKey,action) pair count;
 * {@code max.apeNamingMinNonDetTargets} sets per-pair target-count threshold for non-determinism;
 * {@code max.apeNamingActionRefineMinStateDelta} sets minimum activity state-count delta between refinements;
 * {@code max.apeNamingActionRefineMinNonDetPairDelta} sets minimum nonDet-pair delta between refinements;
 * {@code max.apeNamingActionRefineRuleProfile} switches `baseline` / `strict_baseline` /
 * `java_rule_01_preview` / `java_rule_02_preview` / `java_rule_03_preview` profile.
 * Full key table: {@code android/native/desc/APE_PARITY.md}
 * § {@code max.config} (APE-related); wire overview: {@code JNI_HIERARCHY_AUDIT.md}.
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

    /**
     * Single worker for OpenAI-compatible LLM HTTP. Build + POST run here; the JNI-attached thread
     * blocks on {@link Future#get} with a deadline derived from {@code max.llm.timeoutMs} (Phase 2, MIGRATION_LLMDROID_B2.md).
     */
    private static final ExecutorService sLlmHttpExecutor = Executors.newSingleThreadExecutor(new ThreadFactory() {
        @Override
        public Thread newThread(Runnable r) {
            Thread t = new Thread(r, "fastbot-llm-http");
            t.setDaemon(true);
            return t;
        }
    });

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
     * DoubleSarsa:  Double SARSA reinforcement learning agent with reuse model.
     * Curiosity:    curiosity-driven agent (WebRLED-style dual novelty + ε-greedy).
     */
    public enum AlgorithmType {
        Random(0),
        Sarsa(1),
        DoubleSarsa(8),
        Curiosity(32);

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

    /** Call when test ends normally to persist reuse model (Agent destructor is not run). */
    public static void saveReuseModel() {
        singleton.saveReuseModelNative();
    }

    /**
     * Legacy guide bridge: xml-only JNI entry that returns status code (0 = success).
     */
    public static int addCurrentPageAsPreconditionSyncStatus(String xml) {
        if (!singleton.loaded) {
            Logger.println("// Error: Could not load native library!");
            Logger.println("Please report this bug issue to github");
            return -1;
        }
        return singleton.addCurrentPageAsPreconditionSync(xml);
    }

    /**
     * Legacy guide bridge: xml-only JNI entry mapped to boolean success.
     */
    public static boolean addCurrentPageAsPreconditionSyncOk(String xml) {
        int status = addCurrentPageAsPreconditionSyncStatus(xml);
        return status == 0;
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
     * @param timeoutMs max.llm.timeoutMs from native; drives URLConnection timeouts and Future.get deadline
     * @return response body string, or null on failure
     */
    public String doLlmHttpPostFromPrompt(String url, String apiKey, String prompt, String model, int maxTokens, int timeoutMs) {
        return doLlmHttpPostFromPromptInternal(url, apiKey, prompt, model, maxTokens, null, timeoutMs);
    }

    private static long computeLlmFutureWaitMs(int timeoutMs) {
        if (timeoutMs > 0) {
            return Math.min(600_000L, (long) timeoutMs + 30_000L);
        }
        return 120_000L;
    }

    /** Connect / read millis for HttpURLConnection from max.llm.timeoutMs (<=0 → legacy 15s/20s). */
    private static int[] llmSocketTimeouts(int timeoutMs) {
        if (timeoutMs <= 0) {
            return new int[] { 15000, 20000 };
        }
        int cap = Math.min(timeoutMs, 300_000);
        int connect = Math.min(20000, Math.max(3000, cap / 4));
        int read = Math.min(300_000, Math.max(5000, cap));
        return new int[] { connect, read };
    }

    /**
     * Submits build + POST to {@link #sLlmHttpExecutor}; JNI thread waits with {@link Future#get(long, TimeUnit)}.
     * promptType null = direct prompt (legacy); non-null = executor/planner/step_summary/knowledge_org/content_aware_input.
     */
    private String doLlmHttpPostFromPromptInternal(String url, String apiKey, String prompt, String model, int maxTokens, String promptType, int timeoutMs) {
        if (url == null || url.isEmpty()) {
            Logger.errorPrintln("doLlmHttpPostFromPrompt: url null or empty");
            return null;
        }
        final long waitMs = computeLlmFutureWaitMs(timeoutMs);
        Future<String> fut = sLlmHttpExecutor.submit(new Callable<String>() {
            @Override
            public String call() {
                return doLlmHttpPostFromPromptInternalSync(url, apiKey, prompt, model, maxTokens, promptType, timeoutMs);
            }
        });
        try {
            return fut.get(waitMs, TimeUnit.MILLISECONDS);
        } catch (TimeoutException e) {
            fut.cancel(true);
            Logger.errorPrintln("doLlmHttpPostFromPrompt: Future timeout after " + waitMs + "ms");
            return null;
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
            fut.cancel(true);
            Logger.errorPrintln("doLlmHttpPostFromPrompt: interrupted");
            return null;
        } catch (ExecutionException e) {
            Throwable c = e.getCause();
            Logger.errorPrintln("doLlmHttpPostFromPrompt: execution failed: " + (c != null ? c.getMessage() : e.getMessage()));
            return null;
        }
    }

    /** Build JSON body + HTTP on the LLM worker thread. */
    private String doLlmHttpPostFromPromptInternalSync(String url, String apiKey, String prompt, String model, int maxTokens, String promptType, int timeoutMs) {
        long tStart = System.currentTimeMillis();
        String body = buildLlmRequestBody(prompt, model, maxTokens, promptType);
        if (body == null) {
            Logger.errorPrintln("doLlmHttpPostFromPrompt: buildLlmRequestBody returned null");
            return null;
        }
        long tAfterBuild = System.currentTimeMillis();
        long ts = System.currentTimeMillis();
        saveLlmRawToFile(ts + "-req.json", body);
        String result = doLlmHttpPostBody(url, apiKey, body, timeoutMs);
        long tEnd = System.currentTimeMillis();
        long buildMs = tAfterBuild - tStart;
        long requestMs = tEnd - tAfterBuild;
        long totalMs = tEnd - tStart;
        if (promptType != null) {
            Logger.println("// [LLM timing] (ms) promptType=" + promptType + " buildPrompt+body: " + buildMs + ", request: " + requestMs + ", total: " + totalMs);
        } else {
            Logger.println("// [LLM timing] (ms) buildPrompt+body: " + buildMs + ", request: " + requestMs + ", total: " + totalMs);
        }
        if (result != null) {
            saveLlmRawToFile(ts + "-resp.json", result);
            if ("knowledge_org".equals(promptType) || "content_aware_input".equals(promptType)) {
                logLlmExplorerResponse(promptType, result);
            } else {
                logLlmResponseMessage(result);
            }
        } else {
            Logger.errorPrintln("doLlmHttpPostFromPrompt: doLlmHttpPostBody returned null (check HTTP code / network above)");
        }
        return result;
    }

    /**
     * LLM HTTP POST with prompt assembled in Java from payload JSON (reduces JNI string copy).
     * promptType: "executor" | "planner" | "step_summary" (LLMTaskAgent) | "knowledge_org" (widget_priority) | "content_aware_input" (LLMExplorerAgent)
     * | "llmdroid_state_overview" | "llmdroid_reanalysis" | "llmdroid_guide" | "llmdroid_test_function"
     * (native GPTAgent; payload is structured JSON).
     * Called from native via JNI when using predictWithPayload.
     */
    public String doLlmHttpPostFromPayload(String url, String apiKey, String promptType, String payloadJson, String model, int maxTokens, int timeoutMs) {
        if (url == null || url.isEmpty()) {
            Logger.errorPrintln("doLlmHttpPostFromPayload: url null or empty");
            return null;
        }
        String prompt = buildPromptFromPayload(promptType, payloadJson);
        if (prompt == null) {
            Logger.errorPrintln("doLlmHttpPostFromPayload: buildPromptFromPayload returned null");
            return null;
        }
        return doLlmHttpPostFromPromptInternal(url, apiKey, prompt, model, maxTokens, promptType, timeoutMs);
    }

    /**
     * Build full prompt string from payload JSON (matches C++ LLMTaskAgent prompt content).
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
            if ("knowledge_org".equals(promptType)) {
                return buildKnowledgeOrgPrompt(payload);
            }
            if ("content_aware_input".equals(promptType)) {
                return buildContentAwareInputPrompt(payload);
            }
            if ("llmdroid_state_overview".equals(promptType)) {
                return buildLlmdroidStateOverviewPrompt(payload);
            }
            if ("llmdroid_reanalysis".equals(promptType)) {
                return buildLlmdroidReanalysisPrompt(payload);
            }
            if ("llmdroid_guide".equals(promptType)) {
                return buildLlmdroidGuidePrompt(payload);
            }
            if ("llmdroid_test_function".equals(promptType)) {
                return buildLlmdroidTestFunctionPrompt(payload);
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

    /**
     * LLMExplorerAgent widget priorities: infer user operation popularity per element.
     * Payload: {"elements": [{"id":"0x...", "class":"", ...}, ...], "max_index": N}.
     * Response: priorities by index [p0,p1,...], by id {"priorities":{"0x...":p,...}}, or recommend_order [i0,i1,...].
     */
    private static String buildKnowledgeOrgPrompt(JSONObject j) throws JSONException {
        int maxIndex = j.optInt("max_index", 0);
        StringBuilder sb = new StringBuilder();
        sb.append("You are helping an Android GUI testing agent. Given the following UI elements on the current screen, infer which elements users are most likely to tap first (user operation popularity). Consider typical behavior: primary CTA, back, settings, list items, etc.\n\n");
        sb.append("CRITICAL: Every element (index 0 to ").append(maxIndex).append(") MUST be assigned a priority. No element may be omitted.\n\n");
        sb.append("INDEX CORRESPONDENCE: priorities[i] MUST be the priority for Element i below. recommend_order uses these same 0-based indices. Each element also has an \"id\" (e.g. 0x1a2b); you may return priorities by id for unambiguous matching.\n\n");
        sb.append("Keep REASONING to 1-2 sentences, then output the JSON.\n\n");
        sb.append("Required JSON format (output exactly one; must be valid, complete, parseable):\n");
        sb.append("Option A (by index): {\"priorities\": [p0, p1, ..., p").append(maxIndex).append("]} — exactly ").append(maxIndex + 1).append(" floats, priorities[i] = priority for Element i (0 to 1).\n");
        sb.append("Option B (by id, recommended for clarity): {\"priorities\": {\"<id>\": float, ...}} — key = element id (e.g. \"0x1a2b\"), value = priority 0-1. Include all element ids; missing ids get 0.5.\n");
        sb.append("Option C: {\"recommend_order\": [i0, i1, ...]} — 0-based indices from most to least recommended. Include ALL indices for best results.\n");
        sb.append("Important: Output the complete JSON in one go. Close all brackets. No truncation.\n\n");
        sb.append("Elements:\n");
        JSONArray elements = j.optJSONArray("elements");
        if (elements != null) {
            for (int i = 0; i < elements.length(); i++) {
                JSONObject el = elements.optJSONObject(i);
                if (el != null) {
                    String id = el.optString("id", "");
                    sb.append("Element ").append(i).append(" id=").append(id.isEmpty() ? "?" : id)
                            .append(": class=").append(el.optString("class", ""))
                            .append(" resource-id=").append(el.optString("resource_id", ""))
                            .append(" text=").append(el.optString("text", ""))
                            .append(" content-desc=").append(el.optString("content_desc", "")).append("\n");
                }
            }
        }
        return sb.toString();
    }

    /**
     * LLMExplorerAgent content-aware input: same prompt as C++ getInputTextForAction.
     * Payload: {"package":"", "activity":"", "class":"", "resource_id":"", "text":"", "content_desc":""}.
     */
    private static String buildContentAwareInputPrompt(JSONObject j) throws JSONException {
        StringBuilder sb = new StringBuilder();
        sb.append("You are helping an Android GUI testing agent. Generate a single short, contextually appropriate input value for the given input field (e.g. username, email, phone, search term). Reply with only the input text, no quotes or explanation.\n");
        sb.append("App package: ").append(j.optString("package", "")).append("\n");
        sb.append("Activity: ").append(j.optString("activity", "")).append("\n");
        sb.append("Input field: class=").append(j.optString("class", ""))
                .append(" resource-id=").append(j.optString("resource_id", ""))
                .append(" text/hint=").append(j.optString("text", ""))
                .append(" content-desc=").append(j.optString("content_desc", "")).append("\n");
        return sb.toString();
    }

    private static String buildLlmdroidStateOverviewPrompt(JSONObject j) throws JSONException {
        StringBuilder sb = new StringBuilder();
        sb.append(j.optString("start_prompt", ""));
        sb.append("\n");
        sb.append("An app's page contains many controls to display information to users and provide interactive interfaces.\n");
        sb.append("Users can interact with the controls to perform a \"Function\", such as navigating to other tabs by clicking a navigation bar icon or accessing the settings page.\n");
        sb.append("I will provide an HTML description of an app's page, including the components and their structural information.\n");
        sb.append("In the HTML description of this page,\n");
        sb.append("I use five types of HTML tags, namely <button>, <checkbox>, <scroller>, <input>, and <p>, which represent elements that can be clicked, checked, swiped, edited, and any other views respectively.\n");
        sb.append("Each HTML element has the following attributes:\n");
        sb.append("id(the unique id of this component), class(the class name of this component), resource-id (the resource-id of this Android component), content-desc (the content description of this component), text (the text of this component), direction (if this component is scrollable, indicating its scroll direction), value (the text that has been input to the text box).\n");
        sb.append("\n```HTML Description\n");
        sb.append(j.optString("state_desc", ""));
        sb.append("```\n");
        boolean useTop5 = j.optBoolean("use_top5", false);
        if (useTop5) {
            sb.append("Based on the HTML description of this page, your tasks include:\n\n");
            sb.append("1. Page Overview: Summarize the current page, concluding what kind of information the page mainly presents to users, and what this page is primarily used for.\n");
            sb.append("2. Function Analysis: Identify the functions present on the page, listing their corresponding element IDs. Prioritize these functions by importance. A function's importance increases if it triggers a new page or results in more code being executed. Specifically:\n");
            sb.append("    - Navigation-related functions are crucial. These functions correspond to buttons usually located in menus, navigation drawers, or Tabs. These buttons are typically used to guide users to switch between different pages and enter pages with different functions. These buttons usually have the following characteristics:\n");
            sb.append("        1. They are usually located at the top or bottom of the page.\n");
            sb.append("        2. They usually appear in groups, possibly wrapped in a ScrollView.\n");
            sb.append("        3. In the HTML description, their resource-id attributes may be the same or similar, and the resource-id may also contain \"tab\". Their class should be the same; their text attributes have a similar format and are highly general.\n");
            sb.append("    - functions central to the page's main purpose, like video playback on a video page (play, like, subscribe, comment) or settings adjustments on a settings page.\n");
            sb.append("    - Any other functions you believe could trigger new pages or enhance code coverage.\n");
            sb.append("3. Page Importance Ranking: Assess this page's significance relative to the entire app, considering its content and functions in relation to the app's category and main functions. For example, if this page is a homepage or one of the main pgaes or includes core functions, it's considered more important.\n");
            sb.append("    - I will also provide descriptions and function lists for five other pages. Compare the importance of these pages with the current one and rank the top five most important pages.\n");
            sb.append("Current: State").append(j.optInt("current_state_id", -1)).append("\n");
            sb.append("Five other pages:\n").append(j.optJSONObject("top_pages") != null ? j.optJSONObject("top_pages").toString(4) : "{}").append("\n");
            sb.append("In summary, your response should include:\n\n");
            sb.append("1. A concise summary of the page, within 30 words.\n");
            sb.append("2. A list of the page's functions, including their element IDs, sorted by importance. If you believe the current page is empty or has no function, you can return an empty function list.\n");
            sb.append("3. A ranking of the top five most important pages among the current and the other five pages.\n");
            sb.append("Your anwser should be in json form. Here are the key elements to include:\n");
            sb.append("- \"Overview\": A string that provides a summary of the page.\n");
            sb.append("- \"Function List\": An object consisting of key-value pairs that list the functions in order of importance. The key is a string describing the function, and the value is an integer representing the element ID, which can be obtained from the 'id' attribute of the elements in the HTML description.\n");
            sb.append("- \"Top5\": An array of integers indicating the indices of the top five most important pages, where the index is the number behind \"State\".\n");
            sb.append("Note that the key must not be changed!\n");
        } else {
            sb.append("Based on the HTML description of this page, your tasks include:\n\n");
            sb.append("1. Page Overview: Summarize the current page, concluding what kind of information the page mainly presents to users, and what this page is primarily used for.\n");
            sb.append("2. Function Analysis: Identify the functions present on the page, listing their corresponding element IDs. Prioritize these functions by importance. A function's importance increases if it triggers a new page or results in more code being executed. Specifically:\n");
            sb.append("    - Navigation-related functions are crucial. These functions correspond to buttons usually located in menus, navigation drawers, or Tabs. These buttons are typically used to guide users to switch between different pages and enter pages with different functions. These buttons usually have the following characteristics:\n");
            sb.append("        1. They are usually located at the top or bottom of the page.\n");
            sb.append("        2. They usually appear in groups, possibly wrapped in a ScrollView.\n");
            sb.append("        3. In the HTML description, their resource-id attributes may be the same or similar, and the resource-id may also contain \"tab\". Their class should be the same; their text attributes have a similar format and are highly general.\n");
            sb.append("    - functions central to the page's main purpose, like video playback on a video page (play, like, subscribe, comment) or settings adjustments on a settings page.\n");
            sb.append("    - Any other functions you believe could trigger new pages or enhance code coverage.\n");
            sb.append("In summary, your response should include:\n\n");
            sb.append("1. A concise summary of the page, within 30 words.\n");
            sb.append("2. A list of the page's functions, including their element IDs, sorted by importance.\n");
            sb.append("Your anwser should be in json form. Here are the key elements to include:\n");
            sb.append("- \"Overview\": A string that provides a summary of the page.\n");
            sb.append("- \"Function List\": An object consisting of key-value pairs that list the functions in order of importance. The key is a string describing the function, and the value is an integer representing the element ID, which can be obtained from the 'id' attribute of the elements in the HTML description.\n");
            sb.append("Note that the key must not be changed!\n");
        }
        return sb.toString();
    }

    private static String buildLlmdroidGuidePrompt(JSONObject j) throws JSONException {
        StringBuilder sb = new StringBuilder();
        sb.append(j.optString("start_prompt", ""));
        sb.append("\n");
        sb.append("After a period of testing, we have identified some pages (referred to as States below) and had you analyze their roles and functionalities. Based on this, I also asked you to rank these States in terms of their importance to the overall app.\n");
        sb.append("Below is a list of States you ranked from highest to lowest importance in previous discussions. Each State includes its Overview and FunctionList, with FunctionList containing the five most important untested functions of that page.\n");
        sb.append("\n```State Informations\n");
        JSONObject infos = j.optJSONObject("state_informations");
        sb.append(infos != null ? infos.toString(4) : "{}");
        sb.append("\n```\n");
        sb.append("Based on the information above, please decide: Which State should we go next, and what function would be most appropriate to test in the target State?\n");
        sb.append("Your main objective should be to explore new pages and enhance code coverage by executing this function.\n");
        sb.append("Specifically, you can follow these strategies:\n");
        sb.append("1. Do not select function that has been chosen before:)");
        sb.append("{");
        JSONArray tested = j.optJSONArray("tested_functions");
        if (tested != null) {
            for (int i = 0; i < tested.length(); i++) {
                if (i > 0) {
                    sb.append(", ");
                }
                sb.append(tested.optString(i, ""));
            }
        }
        sb.append("}");
        sb.append("2. Do not choose functions related to login or registration.\n");
        sb.append("3. Prioritize choosing functions related to navigation.\n");
        sb.append("4. Choose other function which can trigger transition or lead to undiscovered pages.\n");
        sb.append("5. If there are no navigation-related functions, you can choose a core function from the higher-ranked pages, like video playback on a video page (play, like, subscribe, comment) or settings adjustments on a settings page.\n");
        sb.append("Your anwser should be in json form. Here are the key elements to include:\n");
        sb.append("- \"Target State\": The State you want to go to, which contains the functionality you want to test.\n");
        sb.append("- \"Target Function\": The function you want to test in the \"Target State\". This function must be chosen from the provided \"Function List\" of the corresponding State and cannot be made up.\n");
        sb.append("Please note that the key must not be changed. You should only give me one choice!\n");
        sb.append("Your final output should only contain the json result and no more.\n");
        return sb.toString();
    }

    private static String buildLlmdroidTestFunctionPrompt(JSONObject j) throws JSONException {
        StringBuilder sb = new StringBuilder();
        sb.append(j.optString("start_prompt", ""));
        sb.append("\n");
        sb.append("The app's current page is provided using HTML, including the components and their structural information.\n");
        sb.append("I use five types of HTML tags, namely <button>, <checkbox>, <scroller>, <input>, and <p>, which represent elements that can be clicked, checked, swiped, edited, and any other views respectively.\n");
        sb.append("Each HTML element has the following attributes:\n");
        sb.append("id(the unique id of this component), class(the class name of this component), resource-id (the resource-id of this Android component), content-desc (the content description of this component), text (the text of this component), direction (if this component is scrollable, indicating its scroll direction), value (the text that has been input to the text box).\n");
        sb.append("\n```Page Description\n").append(j.optString("page_desc", "")).append("```\n");
        sb.append("The target function I want to test is : ").append(j.optString("target_function", "")).append("\n");
        JSONArray executed = j.optJSONArray("executed_functions");
        if (executed != null && executed.length() > 0) {
            sb.append("I've already I have already executed: [");
            for (int i = 0; i < executed.length(); i++) {
                if (i != 0) sb.append(",\n");
                sb.append(executed.optString(i, ""));
            }
            sb.append("]\n");
        }
        sb.append("What action should I perform next to test the target function?\n");
        sb.append("Your response should include the selected element's id and the action to be performed on that element.\n");
        sb.append("The available types of actions include: click (0), long press (1), swipe from top to bottom (2), swipe from bottom to top (3), swipe from left to right (4), swipe from right to left (5) and input text (6).\n");
        sb.append("Your answer should be in json form.\n");
        sb.append("The key \"Element Id\" represents the value of the id attribute of the corresponding tag in the HTML description of the element you have chosen.\n");
        sb.append("The key \"Action Type\" represents the type of action you want to perform on the element, please use the number in the parentheses of the action type.\n");
        sb.append("The key \"Input\" represents the text you want to input to the target element, the value should be generated by you. This key is only needed when the value of \"Action Type\" is 6.\n");
        sb.append("If you believe the target function is finished testing and no more action is needed, the value of \"Element Id\" should be -1, the value of \"Action Type\" should be 0.\n");
        sb.append("Please note that the key must not be changed. The output should be pure json string starting with \"{\", NOT begin with \"```json\", and must not contain comments.\n");
        if (executed != null && executed.length() > 0) {
            sb.append("If you believe that the current page is the page that should be reached after executing the target function, or if the current page lacks the corresponding element to complete the target function, your response should be as follows:\n");
            sb.append("{\n");
            sb.append("    \"Element Id\": -1,\n");
            sb.append("    \"Action Type\": 0\n");
            sb.append("}\n");
        }
        return sb.toString();
    }

    private static String buildLlmdroidReanalysisPrompt(JSONObject j) throws JSONException {
        StringBuilder sb = new StringBuilder();
        sb.append(j.optString("start_prompt", ""));
        sb.append("\n");
        sb.append("You have previously analyzed a page and summarized its Overview and Function list.\n");
        sb.append("```Overview and Function List\n");
        Object ov = j.opt("overview_and_function_list");
        if (ov instanceof JSONObject) {
            sb.append(((JSONObject) ov).toString(4));
        } else if (ov != null) {
            sb.append(String.valueOf(ov));
        } else {
            sb.append("{}");
        }
        sb.append("\n```\n");
        sb.append("Now, you are provided with a set of similar pages containing controls not present in the previous page. Your task is to analyze the potential functions corresponding to these controls.\n");
        sb.append("The controls are provided in HTML format, consisting of five types of HTML tags: <button>, <checkbox>, <scroller>, <input>, and <p>, which represent elements that can be clicked, checked, swiped, edited, and other views, respectively.\n");
        sb.append("Each HTML element has the following attributes: id (the unique ID of this component), class (the class name of this component), resource-id (the resource ID of this Android component), content-desc (the content description of this component), text (the text of this component), direction (if this component is scrollable, indicating its scroll direction)\n");
        sb.append("value (the text that has been input to the text box)\n");
        sb.append("```Controls in HTML Description\n");
        sb.append(j.optString("controls_html", ""));
        sb.append("```\n");
        sb.append("Based on the HTML components, the page's Overview, and the existing Function List, your tasks are as follows:\n");
        sb.append("1. Analyze the functions corresponding to the controls that have an id attribute. Cross-reference these functions with the existing function list, prioritizing matches to ensure consistency.\n");
        sb.append("2. Rank the importance of these functions. A function's importance increases if it triggers a new page or results in more code being executed. Specifically:\n");
        sb.append("\t- Navigation-related functions are crucial.\n");
        sb.append("\t- Functions central to the page's main purpose, such as video playback on a video page (play, like, subscribe, comment) or settings adjustments on a settings page.\n");
        sb.append("\t- Any other functions you believe could trigger new pages or enhance code coverage.\n");
        sb.append("You should always respond using the correct JSON format.\n");
        sb.append("The key is the control's `id` attribute, which must be a string representation of an integer.\n");
        sb.append("The value is the corresponding function of that control.\n");
        sb.append("The closer a key-value pair is to the top, the higher the importance of its function.\n");
        sb.append("If there is no `id` attribute in html controls, just return an empty json.\n");
        sb.append("Please note that the output should be pure json string starting with \"{\", NOT begin with \"```json\", and must not contain comments.\n");
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

    /**
     * Build request body. System prompt is chosen by promptType so LLMTaskAgent (executor/planner/step_summary)
     * and LLMExplorerAgent (knowledge_org/content_aware_input) get appropriate instructions.
     * For llmdroid_* we intentionally do NOT add extra system instruction, to align with upstream
     * LLMDroid behavior (prompt semantics mainly come from user content).
     *
     * @param promptType null = legacy; "knowledge_org" (widget_priority) | "content_aware_input" | "llmdroid_*".
     */
    private String buildLlmRequestBody(String prompt, String model, int maxTokens, String promptType) {
        try {
            boolean llmdroid = promptType != null && promptType.startsWith("llmdroid_");
            // LLMExplorerAgent / LLMDroid: text-only (no screenshot) for these prompt types.
            boolean skipImage = "knowledge_org".equals(promptType) || "content_aware_input".equals(promptType) || llmdroid;
            long t0 = System.currentTimeMillis();
            byte[] img = null;
            if (!skipImage) {
                if (sLlmScreenshotProvider != null) {
                    img = sLlmScreenshotProvider.captureForLlm();
                }
                if (img == null || img.length == 0) {
                    img = lastScreenshotForLlm;
                }
            }
            long t1 = System.currentTimeMillis();
            long screenshotMs = t1 - t0;
            String systemContent = null;
            if ("knowledge_org".equals(promptType)) {
                systemContent = "You are a GUI testing agent. Output a short REASONING line, then a line starting with JSON: followed by a single, complete, parseable JSON object. Use \"priorities\" as either (1) array of floats [p0,p1,...] by element index, or (2) object {\"<id>\": float, ...} keyed by element id for unambiguous matching; or use \"recommend_order\" as array of indices. No groups or functions. Output full JSON—no truncation.";
            } else if ("content_aware_input".equals(promptType)) {
                systemContent = "You are a GUI testing agent. Reply with only the requested input text, no quotes or explanation.";
            } else if (llmdroid) {
                // Keep null intentionally: llmdroid_* relies on full user prompt text (upstream-compatible).
                systemContent = null;
            } else {
                systemContent = "You are a GUI testing agent that must respond with a strict JSON action object.";
            }
            StringBuilder sb = new StringBuilder();
            sb.append("{\"model\":");
            sb.append(escapeJson(model != null ? model : ""));
            sb.append(",\"max_tokens\":").append(Math.max(0, maxTokens));
            sb.append(",\"stream\":false,\"messages\":[");
            if (systemContent != null && !systemContent.isEmpty()) {
                sb.append("{\"role\":\"system\",\"content\":").append(escapeJson(systemContent)).append("},");
            }
            sb.append("{\"role\":\"user\",\"content\":");
            if (!skipImage && img != null && img.length > 0) {
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

    /**
     * Log LLMExplorerAgent LLM response (knowledge_org=widget_priority / content_aware_input).
     * For knowledge_org: LLM returns {"priorities": [0.9, ...]} or {"recommend_order": [0, 2, 1, ...]}.
     */
    private void logLlmExplorerResponse(String promptType, String response) {
        if (response == null || response.isEmpty()) return;
        try {
            JSONObject root = new JSONObject(response);
            JSONArray choices = root.optJSONArray("choices");
            if (choices == null || choices.length() == 0) return;
            JSONObject choice0 = choices.optJSONObject(0);
            if (choice0 == null) return;
            JSONObject message = choice0.optJSONObject("message");
            String content = null;
            if (message != null) content = message.optString("content", null);
            if (content == null || content.isEmpty()) {
                content = choice0.optString("content", choice0.optString("message", ""));
            }
            if (content == null || content.isEmpty()) return;

            if ("knowledge_org".equals(promptType)) {
                try {
                    int jsonMarkerPos = content.indexOf("JSON:");
                    int braceP = content.indexOf("{\"priorities\"");
                    int braceR = content.indexOf("{\"recommend_order\"");
                    int braceGen = content.indexOf('{');
                    int jsonStart = jsonMarkerPos >= 0 ? jsonMarkerPos : (braceP >= 0 ? braceP : (braceR >= 0 ? braceR : braceGen));
                    if (jsonStart >= 0) {
                        String reasoning = content.substring(0, jsonStart).trim();
                        if (reasoning.startsWith("REASONING:")) reasoning = reasoning.substring(10).trim();
                        if (!reasoning.isEmpty()) {
                            Logger.println("// [LLM Explorer widget_priority] reasoning: " + reasoning);
                        }
                    }
                    String toParse = content;
                    if (jsonStart >= 0) {
                        toParse = content.substring(jsonStart);
                        if (toParse.startsWith("JSON:")) toParse = toParse.substring(5).trim();
                    }
                    JSONObject inner = new JSONObject(toParse);
                    JSONArray prioritiesArr = inner.optJSONArray("priorities");
                    JSONObject prioritiesObj = inner.optJSONObject("priorities");
                    JSONArray recommendOrder = inner.optJSONArray("recommend_order");
                    if (prioritiesArr != null && prioritiesArr.length() > 0) {
                        StringBuilder sb = new StringBuilder();
                        sb.append("// [LLM Explorer widget_priority] priorities (by index) len=").append(prioritiesArr.length());
                        if (prioritiesArr.length() <= 8) {
                            sb.append(" [");
                            for (int i = 0; i < prioritiesArr.length(); i++) {
                                if (i > 0) sb.append(", ");
                                sb.append(prioritiesArr.optDouble(i, 0));
                            }
                            sb.append("]");
                        }
                        Logger.println(sb.toString());
                        // for (int i = 0; i < prioritiesArr.length(); i++) {
                        //     double p = prioritiesArr.optDouble(i, 0);
                        //     Logger.println("// [LLM Explorer widget_priority] [index " + i + "] priority=" + p);
                        // }
                    } else if (prioritiesObj != null && prioritiesObj.length() > 0) {
                        Logger.println("// [LLM Explorer widget_priority] priorities (by id) keys=" + prioritiesObj.length());
                        // java.util.Iterator<String> keys = prioritiesObj.keys();
                        // while (keys.hasNext()) {
                        //     String id = keys.next();
                        //     double p = prioritiesObj.optDouble(id, 0.5);
                        //     Logger.println("// [LLM Explorer widget_priority] widget(id=" + id + ") priority=" + p);
                        // }
                    } else if (recommendOrder != null && recommendOrder.length() > 0) {
                        StringBuilder sb = new StringBuilder();
                        sb.append("// [LLM Explorer widget_priority] recommend_order len=").append(recommendOrder.length());
                        if (recommendOrder.length() <= 12) {
                            sb.append(" [");
                            for (int i = 0; i < recommendOrder.length(); i++) {
                                if (i > 0) sb.append(", ");
                                sb.append(recommendOrder.optInt(i, -1));
                            }
                            sb.append("]");
                        }
                        Logger.println(sb.toString());
                    } else {
                        Logger.println("// [LLM Explorer widget_priority] content (parse: no priorities/recommend_order): " + (content.length() > 80 ? content.substring(0, 77) + "..." : content));
                    }
                } catch (JSONException e) {
                    Logger.println("// [LLM Explorer widget_priority] content (parse failed): " + (content.length() > 80 ? content.substring(0, 77) + "..." : content));
                }
            } else if ("content_aware_input".equals(promptType)) {
                String suggested = content.trim();
                if (suggested.length() > 60) suggested = suggested.substring(0, 57) + "...";
                Logger.println("// [LLM Explorer content_aware_input] suggested=" + suggested);
            }
        } catch (JSONException e) {
            // ignore outer parse
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

    private String doLlmHttpPostBody(String url, String apiKey, String body, int timeoutMs) {
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
            int[] sock = llmSocketTimeouts(timeoutMs);
            conn.setConnectTimeout(sock[0]);
            conn.setReadTimeout(sock[1]);
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
     * Native fallback scalar when Jacoco/AndroLog are not active: RL graph size, reported activities, step count.
     * {@link com.android.commands.monkey.utils.CodeCoverage#getCoverage} prefers Jacoco/AndroLog when Monkey initialized them.
     */
    public static double getLlmdroidCoverageMetric() {
        if (!singleton.loaded) {
            return 0.0;
        }
        return singleton.getLlmdroidCoverageMetricNative();
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
    private native void saveReuseModelNative();
    private native int addCurrentPageAsPreconditionSync(String xml);
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
    private native double getLlmdroidCoverageMetricNative();
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
