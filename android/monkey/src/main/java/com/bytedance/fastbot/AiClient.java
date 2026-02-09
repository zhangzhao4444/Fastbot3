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

import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.net.HttpURLConnection;
import java.net.URL;
import java.nio.ByteBuffer;
import java.util.Arrays;
import java.util.List;
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

    public enum AlgorithmType {
        Random(0),
        SataRL(1),
        SataNStep(2),
        NStepQ(3),
        Reuse(4);

        private final int _value;

        AlgorithmType(int value) {
            this._value = value;
        }

        public int value() {
            return this._value;
        }
    }

    public static void InitAgent(AlgorithmType agentType, String packagename) {
        singleton.fgdsaf5d(agentType.value(), packagename, 0);
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
            e.printStackTrace();
            return false;
        }
        return true;
    }

    private static String getAiPathFromAPK(){
        String[] abis = Build.SUPPORTED_ABIS;
        List<String> abisList = Arrays.asList(abis);
        if (abisList.contains("x86_64")) {
            return "/data/local/tmp/monkey.apk!/lib/x86_64/libfastbot_native.so";
        } else if (abisList.contains("x86")) {
            return "/data/local/tmp/monkey.apk!/lib/x86/libfastbot_native.so";
        } else if (abisList.contains("arm64-v8a")) {
            return "/data/local/tmp/monkey.apk!/lib/arm64-v8a/libfastbot_native.so";
        } else {
            return "/data/local/tmp/monkey.apk!/lib/armeabi-v7a/libfastbot_native.so";
        }
    }

    private static String getAiPathLocally(){
        String[] abis = Build.SUPPORTED_ABIS;
        List<String> abisList = Arrays.asList(abis);
        if (abisList.contains("x86_64")) {
            return "/data/local/tmp/x86_64/libfastbot_native.so";
        } else if (abisList.contains("x86")) {
            return "/data/local/tmp/x86/libfastbot_native.so";
        } else if (abisList.contains("arm64-v8a")) {
            return "/data/local/tmp/arm64-v8a/libfastbot_native.so";
        } else {
            return "/data/local/tmp/armeabi-v7a/libfastbot_native.so";
        }
    }

    private static String getAiPath(boolean fromAPK) {
        if(Build.VERSION.SDK_INT <= com.android.commands.monkey.utils.AndroidVersions.API_22_ANDROID_5_1) {
            return getAiPathLocally();
        }else {
            if (fromAPK) {
                return getAiPathFromAPK();
            }else {
                return getAiPathLocally();
            }
        }
    }

    public static void loadResMapping(String resmapping) {
        if (null == singleton) {
            Logger.println("// Error: AiCore not initted!");
            return;
        }
        if (!singleton.loaded) {
            Logger.println("// Error: Could not load native library!");
            Logger.println("Please report this bug issue to github");
            System.exit(1);
        }
        singleton.jdasdbil(resmapping);
    }

    public static Operate getAction(String activity, String pageDesc) {
        return singleton.b1bhkadf(activity, pageDesc);
    }

    /**
     * Optional: set a pre-captured screenshot when no LlmScreenshotProvider is registered (e.g. tests).
     */
    public static void setLastScreenshotForLlm(byte[] png) {
        if (singleton != null) {
            singleton.lastScreenshotForLlm = png;
        }
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
        } else {
            Logger.errorPrintln("doLlmHttpPostFromPrompt: doLlmHttpPostBody returned null (check HTTP code / network above)");
        }
        return result;
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
            if (code < 200 || code >= 300) {
                Logger.errorPrintln("doLlmHttpPostBody: HTTP " + code + " for url=" + url + " (expected 2xx; 404=wrong path, check max.llm.apiUrl e.g. https://.../v1/chat/completions)");
                return null;
            }
            InputStream in = conn.getInputStream();
            if (in == null) return null;
            byte[] buf = new byte[4096];
            StringBuilder sb = new StringBuilder();
            int n;
            while ((n = in.read(buf)) > 0) {
                sb.append(new String(buf, 0, n, StandardCharsets.UTF_8));
            }
            return sb.toString();
        } catch (Exception e) {
            Logger.errorPrintln("doLlmHttpPostBody failed: " + e.getMessage());
            return null;
        } finally {
            if (conn != null) conn.disconnect();
        }
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

    private native void jdasdbil(String b9);

    private native String b0bhkadf(String a0, String a1);
    private native void fgdsaf5d(int b7, String b2, int t);
    private native boolean nkksdhdk(String a0, float p1, float p2);

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
        return singleton.nkksdhdk(activity, point.x, point.y);
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

    public Operate b1bhkadf(String activity, String pageDesc) {
        if (!loaded) {
            Logger.println("// Error: Could not load native library!");
            Logger.println("Please report this bug issue to github");
            System.exit(1);
        }
        String operateStr = b0bhkadf(activity, pageDesc);

        if (operateStr.length() < 1) {
            Logger.errorPrintln("native get operate failed " + operateStr);
            return null;
        }
        return Operate.fromJson(operateStr);
    }

}
