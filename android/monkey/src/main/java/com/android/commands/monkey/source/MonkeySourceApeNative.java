/**
 * @author Zhao Zhang
 */

package com.android.commands.monkey.source;

import static com.android.commands.monkey.fastbot.client.ActionType.SCROLL_BOTTOM_UP;
import static com.android.commands.monkey.fastbot.client.ActionType.SCROLL_TOP_DOWN;
import static com.android.commands.monkey.framework.AndroidDevice.stopPackage;
import static com.android.commands.monkey.utils.Config.bytestStatusBarHeight;
import static com.android.commands.monkey.utils.Config.defaultGUIThrottle;
import static com.android.commands.monkey.utils.Config.doHistoryRestart;
import static com.android.commands.monkey.utils.Config.doHoming;
import static com.android.commands.monkey.utils.Config.execPreShell;
import static com.android.commands.monkey.utils.Config.execPreShellEveryStartup;
import static com.android.commands.monkey.utils.Config.execSchema;
import static com.android.commands.monkey.utils.Config.execSchemaEveryStartup;
import static com.android.commands.monkey.utils.Config.fuzzingRate;
import static com.android.commands.monkey.utils.Config.historyRestartRate;
import static com.android.commands.monkey.utils.Config.homeAfterNSecondsofsleep;
import static com.android.commands.monkey.utils.Config.homingRate;
import static com.android.commands.monkey.utils.Config.imageWriterCount;
import static com.android.commands.monkey.utils.Config.refetchInfoCount;
import static com.android.commands.monkey.utils.Config.refetchInfoWaitingInterval;
import static com.android.commands.monkey.utils.Config.saveGUITreeToXmlEveryStep;
import static com.android.commands.monkey.utils.Config.schemaTraversalMode;
import static com.android.commands.monkey.utils.Config.scrollAfterNSecondsofsleep;
import static com.android.commands.monkey.utils.Config.startAfterDoScrollAction;
import static com.android.commands.monkey.utils.Config.startAfterDoScrollActionTimes;
import static com.android.commands.monkey.utils.Config.startAfterDoScrollBottomAction;
import static com.android.commands.monkey.utils.Config.startAfterDoScrollBottomActionTimes;
import static com.android.commands.monkey.utils.Config.startAfterNSecondsofsleep;
import static com.android.commands.monkey.utils.Config.swipeDuration;
import static com.android.commands.monkey.utils.Config.takeScreenshotForEveryStep;
import static com.android.commands.monkey.utils.Config.llmEnabled;
import static com.android.commands.monkey.utils.Config.llmScreenshotJpegQuality;
import static com.android.commands.monkey.utils.Config.llmScreenshotMaxSize;
import static com.android.commands.monkey.utils.Config.throttleForExecPreSchema;
import static com.android.commands.monkey.utils.Config.throttleForExecPreShell;
import static com.android.commands.monkey.utils.Config.useRandomClick;

import android.accessibilityservice.AccessibilityServiceInfo;
import android.app.IActivityManager;
import android.app.UiAutomation;
import android.app.UiAutomationConnection;
import android.content.ComponentName;
import android.content.pm.ActivityInfo;
import android.content.pm.PackageInfo;
import android.content.pm.PackageManager;
import android.graphics.Bitmap;
import android.graphics.PointF;
import android.graphics.Rect;
import android.hardware.display.DisplayManagerGlobal;
import android.os.Build;
import android.os.HandlerThread;
import android.os.SystemClock;
import android.util.DisplayMetrics;
import android.view.Display;
import android.view.IWindowManager;
import android.view.KeyCharacterMap;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.Surface;
import android.view.accessibility.AccessibilityNodeInfo;

import com.android.commands.monkey.Monkey;
import com.android.commands.monkey.action.Action;
import com.android.commands.monkey.action.FuzzAction;
import com.android.commands.monkey.action.ModelAction;
import com.android.commands.monkey.events.CustomEvent;
import com.android.commands.monkey.events.CustomEventFuzzer;
import com.android.commands.monkey.events.MonkeyEvent;
import com.android.commands.monkey.events.MonkeyEventQueue;
import com.android.commands.monkey.events.MonkeyEventSource;
import com.android.commands.monkey.events.base.MonkeyActivityEvent;
import com.android.commands.monkey.events.base.MonkeyCommandEvent;
import com.android.commands.monkey.events.base.MonkeyDataActivityEvent;
import com.android.commands.monkey.events.base.MonkeyIMEEvent;
import com.android.commands.monkey.events.base.MonkeyKeyEvent;
import com.android.commands.monkey.events.base.MonkeyRotationEvent;
import com.android.commands.monkey.events.base.MonkeySchemaEvent;
import com.android.commands.monkey.events.base.MonkeyThrottleEvent;
import com.android.commands.monkey.events.base.MonkeyTouchEvent;
import com.android.commands.monkey.events.base.mutation.MutationAirplaneEvent;
import com.android.commands.monkey.events.base.mutation.MutationAlwaysFinishActivityEvent;
import com.android.commands.monkey.events.base.mutation.MutationWifiEvent;
import com.android.commands.monkey.events.customize.ClickEvent;
import com.android.commands.monkey.events.customize.ShellEvent;
import com.android.commands.monkey.fastbot.client.ActionType;
import com.android.commands.monkey.fastbot.client.Operate;
import com.android.commands.monkey.framework.AndroidDevice;
import com.android.commands.monkey.provider.SchemaProvider;
import com.android.commands.monkey.provider.ShellProvider;
import com.android.commands.monkey.tree.TreeBuilder;
import com.android.commands.monkey.utils.Config;
import com.android.commands.monkey.utils.ImageWriterQueue;
import com.android.commands.monkey.utils.Logger;
import com.android.commands.monkey.utils.MonkeyUtils;
import com.android.commands.monkey.utils.RandomHelper;
import com.android.commands.monkey.utils.UUIDHelper;
import com.bytedance.fastbot.AiClient;

import java.io.BufferedWriter;
import java.io.File;
import java.io.FileOutputStream;
import java.io.OutputStreamWriter;
import java.nio.ByteBuffer;
import java.nio.CharBuffer;
import java.nio.charset.CharsetEncoder;
import java.nio.charset.CoderResult;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.HashMap;
import java.util.HashSet;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.Random;
import java.util.Stack;
import java.util.concurrent.TimeoutException;

/**
 * @author Zhao Zhang
 */

/**
 * Monkey event core class, get gui page information, jni call native layer to generate events, inject execution events
 */
public class MonkeySourceApeNative extends MonkeySourceApeBase implements MonkeyEventSource {

    /**
     * UiAutomation client and connection
     */
    protected UiAutomation mUiAutomation;
    protected final HandlerThread mHandlerThread = new HandlerThread("MonkeySourceApeNative");
    public Monkey monkey = null;

    /** total number of events generated so far */
    private int mEventCount = 0;
    /** screenshot asynchronous storage queue */
    private ImageWriterQueue[] mImageWriters;
    /** Reusable direct buffer for XML over JNI (performance: avoid GetStringUTFChars copy). */
    private ByteBuffer mXmlBuffer;
    /** Reusable list for generateFuzzingAction simplify path (PERFORMANCE_OPTIMIZATION_ITEMS §8.3). */
    private final List<CustomEvent> mReusableFuzzEvents = new ArrayList<>();

    // Cached screenshot state for LLM-based AutodevAgent (PNG bytes in memory, throttled by time/activity)
    private long mLastLlmScreenshotTime = 0L;
    private String mLastLlmScreenshotActivity = "";
    private byte[] mLastLlmScreenshotPng = null;

    public MonkeySourceApeNative(Random random, List<ComponentName> MainApps,
                                 long throttle, boolean randomizeThrottle, boolean permissionTargetSystem,
                                 File outputDirectory) {
        super(random, MainApps, throttle, randomizeThrottle, outputDirectory);
        AiClient.setLlmDumpDirectory(getOutputDir());
        mImageWriters = new ImageWriterQueue[imageWriterCount];
        for (int i = 0; i < imageWriterCount; i++) {
            mImageWriters[i] = new ImageWriterQueue();
            Thread imageThread = new Thread(mImageWriters[i]);
            imageThread.start();
        }
        connect();
        Logger.println("// device uuid is " + did);
    }

    /**
     * Connect to AccessibilityService
     */
    public void connect() {
        if (mHandlerThread.isAlive()) {
            throw new IllegalStateException("Already connected!");
        }
        mHandlerThread.start();
        mUiAutomation = new UiAutomation(mHandlerThread.getLooper(), new UiAutomationConnection());
        mUiAutomation.connect();

        AccessibilityServiceInfo info = mUiAutomation.getServiceInfo();
        // Compress this node
        info.flags &= ~AccessibilityServiceInfo.FLAG_INCLUDE_NOT_IMPORTANT_VIEWS;

        mUiAutomation.setServiceInfo(info);
    }

    /**
     * Disconnect to AccessibilityService
     */
    public void disconnect() {
        if (!mHandlerThread.isAlive()) {
            throw new IllegalStateException("Already disconnected!");
        }
        if (mUiAutomation != null) {
            mUiAutomation.disconnect();
        }
        mHandlerThread.quit();
    }

    public int getEventCount() {
        return mEventCount;
    }

    public void tearDown() {
        this.disconnect();
        this.printCoverage();
        for (ImageWriterQueue writer : mImageWriters) {
            writer.tearDown();
        }
    }

    public boolean validate() {
        return mHandlerThread.isAlive();
    }

    /**
     * Capture a screenshot as PNG bytes for LLM-based AutodevAgent, with simple
     * throttling to avoid taking a screenshot every step.
     *
     * Strategy:
     * - Only active when llmEnabled == true (unless forceRetry is true for native requestScreenshotRetry path)
     * - If same activity and less than 1000ms since last capture, reuse cached PNG
     * - Otherwise, call UiAutomation.takeScreenshot(), compress to PNG in-memory,
     *   recycle the Bitmap and update cache.
     *
     * @param activity current activity name
     * @param forceRetry if true, take screenshot regardless of llmEnabled (used when native requested retry with screenshot)
     * @return PNG bytes or null if disabled or error
     */
    private byte[] captureLlmScreenshotIfNeeded(String activity, boolean forceRetry) {
        boolean gated = !forceRetry && (!llmEnabled || mUiAutomation == null);
        if (gated) {
            if (mVerbose > 0) {
                Logger.println("// [LLM screenshot] skip: llmEnabled=" + llmEnabled + " mUiAutomation=" + (mUiAutomation != null) + " forceRetry=" + forceRetry);
            }
            return null;
        }
        if (mUiAutomation == null) {
            if (mVerbose > 0) Logger.println("// [LLM screenshot] mUiAutomation is null");
            return null;
        }
        if (!forceRetry) {
            long now = System.currentTimeMillis();
            if (mLastLlmScreenshotPng != null
                    && activity != null
                    && activity.equals(mLastLlmScreenshotActivity)
                    && (now - mLastLlmScreenshotTime) < 1000L) {
                return mLastLlmScreenshotPng;
            }
        }

        Bitmap map = mUiAutomation.takeScreenshot();
        if (map == null) {
            if (mVerbose > 0) Logger.println("// [LLM screenshot] takeScreenshot() returned null");
            return null;
        }
        java.io.ByteArrayOutputStream baos = new java.io.ByteArrayOutputStream(64 * 1024);
        boolean ok = map.compress(Bitmap.CompressFormat.PNG, 100, baos);
        map.recycle();
        if (!ok) {
            if (mVerbose > 0) Logger.println("// [LLM screenshot] compress failed");
            return null;
        }
        byte[] png = baos.toByteArray();
        if (!forceRetry) {
            long now = System.currentTimeMillis();
            mLastLlmScreenshotPng = png;
            mLastLlmScreenshotTime = now;
            mLastLlmScreenshotActivity = (activity != null ? activity : "");
        }
        if (mVerbose > 0) Logger.println("// [LLM screenshot] captured len=" + png.length + " forceRetry=" + forceRetry);
        return png;
    }

    /** @see #captureLlmScreenshotIfNeeded(String, boolean) */
    private byte[] captureLlmScreenshotIfNeeded(String activity) {
        return captureLlmScreenshotIfNeeded(activity, false);
    }

    /**
     * Capture screenshot when native triggers an LLM request (doLlmHttpPostFromPrompt).
     * No throttle, no llmEnabled gate — ensures every LLM request gets a fresh image.
     * Optionally resizes (max.llm.screenshotMaxSize) and compresses as JPEG (max.llm.screenshotJpegQuality)
     * to reduce tokens and latency; 0 = no resize / PNG.
     */
    private byte[] captureScreenshotForLlmRequest() {
        if (mUiAutomation == null) return null;
        Bitmap map = mUiAutomation.takeScreenshot();
        if (map == null) return null;
        int maxSize = llmScreenshotMaxSize > 0 ? llmScreenshotMaxSize : Integer.MAX_VALUE;
        int w = map.getWidth();
        int h = map.getHeight();
        if (w > maxSize || h > maxSize) {
            float scale = Math.min((float) maxSize / w, (float) maxSize / h);
            int nw = Math.max(1, Math.round(w * scale));
            int nh = Math.max(1, Math.round(h * scale));
            Bitmap scaled = Bitmap.createScaledBitmap(map, nw, nh, true);
            map.recycle();
            map = scaled;
        }
        java.io.ByteArrayOutputStream baos = new java.io.ByteArrayOutputStream(64 * 1024);
        int jpegQuality = llmScreenshotJpegQuality;
        boolean ok;
        if (jpegQuality > 0 && jpegQuality <= 100) {
            ok = map.compress(Bitmap.CompressFormat.JPEG, jpegQuality, baos);
        } else {
            ok = map.compress(Bitmap.CompressFormat.PNG, 100, baos);
        }
        map.recycle();
        return ok ? baos.toByteArray() : null;
    }

    /**
     * ActiveWindow may not belong to activity package.
     *
     * @return AccessibilityNodeInfo of the root
     */
    public AccessibilityNodeInfo getRootInActiveWindow() {
        return mUiAutomation.getRootInActiveWindow();
    }

    public AccessibilityNodeInfo getRootInActiveWindowSlow() {
        try {
            mUiAutomation.waitForIdle(1000, 1000 * 10);
        } catch (TimeoutException e) {
            //e.printStackTrace();
        }
        return mUiAutomation.getRootInActiveWindow();
    }

    /**
     * Ensure reusable direct buffer has at least minCapacity; (performance: PERF §3.1).
     */
    private void ensureXmlBufferCapacity(int minCapacity) {
        if (mXmlBuffer == null || mXmlBuffer.capacity() < minCapacity) {
            mXmlBuffer = ByteBuffer.allocateDirect(Math.max(minCapacity, 64 * 1024));
        }
    }

    /**
     * Get action from XML string via Direct ByteBuffer to avoid JNI GetStringUTFChars copy.
     * PERFORMANCE_OPTIMIZATION_ITEMS §8.5: encode string directly into mXmlBuffer to avoid intermediate byte[].
     */
    private Operate getActionFromXmlBuffer(String activity, String stringOfGuiTree) {
        try {
            int est = stringOfGuiTree.length() * 4;  // UTF-8 max bytes per char
            ensureXmlBufferCapacity(est);
            mXmlBuffer.clear();
            CharsetEncoder enc = StandardCharsets.UTF_8.newEncoder();
            CoderResult cr = enc.encode(CharBuffer.wrap(stringOfGuiTree), mXmlBuffer, true);
            if (cr.isOverflow()) {
                ensureXmlBufferCapacity(Math.max(mXmlBuffer.capacity() * 2, est * 2));
                mXmlBuffer.clear();
                enc.reset();
                cr = enc.encode(CharBuffer.wrap(stringOfGuiTree), mXmlBuffer, true);
            }
            if (cr.isError()) {
                throw new RuntimeException("UTF-8 encode error");
            }
            enc.flush(mXmlBuffer);
            mXmlBuffer.flip();
            return AiClient.getActionFromBuffer(activity, mXmlBuffer);
        } catch (Exception e) {
            if (mVerbose > 0) Logger.println("// getActionFromXmlBuffer error: " + e.getMessage());
            return null;
        }
    }

    /**
     * Same as getActionFromXmlBuffer; LLM screenshot provider is already set before tree dump.
     */
    private Operate getActionFromXmlBufferWithScreenshot(String activity, String stringOfGuiTree) {
        return getActionFromXmlBuffer(activity, stringOfGuiTree);
    }

    void resetRotation() {
        addEvent(new MonkeyRotationEvent(Surface.ROTATION_0, false));
    }

    /**
     * if the queue is empty, we generate events first
     *
     * @return the first event in the queue
     */
    public MonkeyEvent getNextEvent() {
        checkAppActivity();
        if (!hasEvent()) {
            try {
                generateEvents();
            } catch (RuntimeException e) {
                Logger.errorPrintln(e.getMessage());
                if (mVerbose > 0) Logger.println("// getNextEvent: " + e.toString());
                clearEvent();
                return null;
            }
        }
        mEventCount++;
        return popEvent();
    }

    public Random getRandom() {
        return mRandom;
    }

    public long getThrottle() {
        return this.mThrottle;
    }

    /**
     * If the given component is not allowed to interact with, start a random app or
     * generating a fuzzing action.
     * @param cn Component that is not allowed to interact with
     */
    private void dealWithBlockedActivity(ComponentName cn) {
        String className = cn.getClassName();
        if (!hasEvent()) {
            if (appRestarted == 0) {
                if (mVerbose > 0) Logger.println("// the top activity is " + className + ", not testing app, need inject restart app");
                startRandomMainApp();
                appRestarted = 1;
            } else {
                if (!AndroidDevice.isAtPhoneLauncher(className)) {
                    if (mVerbose > 0) Logger.println("// the top activity is " + className + ", not testing app, need inject fuzz event");
                    Action fuzzingAction = generateFuzzingAction(true);
                    generateEventsForAction(fuzzingAction);
                } else {
                    fullFuzzing = false;
                }
                appRestarted = 0;
            }
        }
    }

    /**
     * If this activity could be interacted with. Should be in white list or not in blacklist or
     * not specified.
     * @param cn Component Name of this activity
     * @return If could be interacted, return true
     */
    private boolean checkAppActivity(ComponentName cn) {
        return cn == null || MonkeyUtils.getPackageFilter().checkEnteringPackage(cn.getPackageName());
    }

    protected void checkAppActivity() {
        ComponentName cn = getTopActivityComponentName();
        if (cn == null) {
            if (mVerbose > 0) Logger.println("// get activity api error");
            clearEvent();
            startRandomMainApp();
            return;
        }
        String className = cn.getClassName();
        String pkg = cn.getPackageName();
        boolean allow = MonkeyUtils.getPackageFilter().checkEnteringPackage(pkg);

        if (allow) {
            if (!this.currentActivity.equals(className)) {
                this.currentActivity = className;
                activityHistory.add(this.currentActivity);
                AiClient.reportActivity(this.currentActivity);  // coverage in C++ (PERF §3.4)
                if (mVerbose > 0) Logger.println("// current activity is " + this.currentActivity);
                timestamp++;
            }
        } else {
            dealWithBlockedActivity(cn);
        }
    }

    public Bitmap captureBitmap() {
        return mUiAutomation.takeScreenshot();
    }

    /**
     * If current window belongs to the system UI, randomly pick an allowed app to start
     * @param info AccessibilityNodeInfo of the root of this current window
     * @return If this window belongs to system UI, return true
     */
    public boolean dealWithSystemUI(AccessibilityNodeInfo info) {
        if (info == null || info.getPackageName() == null) {
            if (mVerbose > 0) Logger.println("get null accessibility node");
            return false;
        }
        String packageName = info.getPackageName().toString();
        if (packageName.equals("com.android.systemui")) {

            if (mVerbose > 0) Logger.println("get notification window or other system windows");
            Rect bounds = AndroidDevice.getDisplayBounds(AndroidDevice.getFocusedDisplayId());
            // press home
            generateKeyEvent(KeyEvent.KEYCODE_HOME);
            //scroll up
            generateScrollEventAt(bounds, SCROLL_BOTTOM_UP);
            // launch app
            generateActivityEvents(randomlyPickMainApp(), false, false);
            generateThrottleEvent(1000);
            return true;
        }
        return false;
    }


    /**
     * generate a random event based on mFactor
     */
    protected void generateEvents() {
        long start = System.currentTimeMillis();
        if (hasEvent()) {
            return;
        }

        resetRotation();
        ComponentName topActivityName = null;
        String stringOfGuiTree = "";
        Operate operate = null;
        Action fuzzingAction = null;
        AccessibilityNodeInfo info = null;
        int repeat = refetchInfoCount;
        long tGetTop = 0, tGetRoot = 0, tGetRootSlow = 0, tDumpBinary = 0, tDumpXml = 0;
        boolean usedGetRootSlow = false;

        // PERFORMANCE_OPTIMIZATION_ITEMS §2.4: get topActivityName once, loop only retries getRootInActiveWindow.
        long t0 = System.currentTimeMillis();
        topActivityName = this.getTopActivityComponentName();
        tGetTop = System.currentTimeMillis() - t0;

        // try to get AccessibilityNodeInfo quickly for several times.
        long t1 = System.currentTimeMillis();
        while (repeat-- > 0) {
            info = getRootInActiveWindow();
            if (info == null || topActivityName == null) {
                sleep(refetchInfoWaitingInterval);
                continue;
            }

            if (mVerbose > 0) Logger.println("// Event id: " + mEventId);
            if (dealWithSystemUI(info)) return;
            break;
        }
        tGetRoot = System.currentTimeMillis() - t1;

        // If node is null, try to get AccessibilityNodeInfo slow for only once
        if (info == null) {
            topActivityName = this.getTopActivityComponentName();
            long tSlow0 = System.currentTimeMillis();
            info = getRootInActiveWindowSlow();
            tGetRootSlow = System.currentTimeMillis() - tSlow0;
            usedGetRootSlow = true;
            if (info != null) {
                if (mVerbose > 0) Logger.println("// Event id: " + mEventId);
                if (dealWithSystemUI(info)) return;
            }
        }

        // If node is not null, build tree and recycle this resource.
        if (info!=null){
            // So when C++ triggers LLM request (doLlmHttpPostFromPrompt), we capture on demand — no per-step screenshot.
            AiClient.setLlmScreenshotProvider(this::captureScreenshotForLlmRequest);
            boolean useXmlOnly = "xml".equalsIgnoreCase(Config.treeDumpMode);
            if (useXmlOnly) {
                // max.treeDumpMode=xml: skip binary, always dump XML (e.g. for perf comparison or compatibility).
                long tXml0 = System.currentTimeMillis();
                stringOfGuiTree = TreeBuilder.dumpDocumentStrWithOutTree(info);
                tDumpXml = System.currentTimeMillis() - tXml0;
                if (mVerbose > 0) Logger.println("// dumpXml: " + tDumpXml + " ms (treeDumpMode=xml)");
                if (mVerbose > 3) Logger.println("//" + stringOfGuiTree);
            } else {
                // Default: try compact binary first; fall back to XML if buffer too small or fail.
                ensureXmlBufferCapacity(1024 * 1024);  // 1MB for large trees; dumpNodeRecBinary returns -1 if full
                mXmlBuffer.clear();  // critical: remaining() must be capacity; otherwise limit was last write size
                long tBin0 = System.currentTimeMillis();
                int binaryWritten = TreeBuilder.dumpToBinary(info, mXmlBuffer);
                tDumpBinary = System.currentTimeMillis() - tBin0;
                if (mVerbose > 0) {
                    Logger.println("// dumpBinary: " + tDumpBinary + " ms" + (binaryWritten <= 0 ? " (buffer full or fail)" : ""));
                }
                if (binaryWritten > 0) {
                    mXmlBuffer.position(0);
                    mXmlBuffer.limit(binaryWritten);
                    String activityForLlm = topActivityName != null ? topActivityName.getClassName() : "";
                    operate = AiClient.getActionFromBuffer(activityForLlm, mXmlBuffer);
                }
                if (operate == null) {
                    if (mVerbose > 0) {
                        Logger.println("// event time: fallback to XML (binaryWritten=" + binaryWritten + ")");
                    }
                    long tXml0 = System.currentTimeMillis();
                    stringOfGuiTree = TreeBuilder.dumpDocumentStrWithOutTree(info);
                    tDumpXml = System.currentTimeMillis() - tXml0;
                    if (mVerbose > 3) Logger.println("//" + stringOfGuiTree);
                }
            }
            info.recycle();
        }

        // For user specified actions, during executing, fuzzing is not allowed.
        boolean allowFuzzing = true;

        if (topActivityName != null && (operate != null || !"".equals(stringOfGuiTree))) {
            try {
                long rpc_start = System.currentTimeMillis();

                if (mVerbose > 0) Logger.println("topActivityName: " + topActivityName.getClassName());
                if (operate == null) {
                    // Performance: pass XML via Direct ByteBuffer (PERF §3.1)
                    operate = getActionFromXmlBuffer(topActivityName.getClassName(), stringOfGuiTree);
                }
                if (operate == null) {
                    operate = AiClient.getAction(topActivityName.getClassName(), stringOfGuiTree);
                }
                if (operate == null) {
                    generateThrottleEvent(mThrottle);
                    return;
                }
                operate.throttle += (int) this.mThrottle;
                // For user specified actions, during executing, fuzzing is not allowed.
                allowFuzzing = operate.allowFuzzing;
                ActionType type = operate.act;
                if (mVerbose > 0) {
                    Logger.println("action type: " + type.toString());
                    Logger.println("rpc cost time: " + (System.currentTimeMillis() - rpc_start) + " ms");
                }

                mReusableRect.set(0, 0, 0, 0);
                mReusablePointFloats.clear();

                if (type.requireTarget()) {
                    if (!operate.setRectFromPos(mReusableRect)) type = ActionType.NOP;
                }

                timeStep++;
                String sid = operate.sid;
                String aid = operate.aid;
                long timeMillis = System.currentTimeMillis();

                if (saveGUITreeToXmlEveryStep) {
                    checkOutputDir();
                    File xmlFile = new File(checkOutputDir(), String.format(stringFormatLocale,
                            "step-%d-%s-%s-%s.xml", timeStep, sid, aid, timeMillis));
                    Logger.infoFormat("Saving GUI tree to %s at step %d %s %s",
                            xmlFile, timeStep, sid, aid);

                    BufferedWriter out;
                    try {
                        out = new BufferedWriter(new OutputStreamWriter(new FileOutputStream(xmlFile, false)));
                        out.write(stringOfGuiTree);
                        out.flush();
                        out.close();
                    } catch (java.io.IOException e) {
                        if (mVerbose > 0) Logger.println("// saveGUITreeToXml: " + e.getMessage());
                    }
                }

                if (takeScreenshotForEveryStep) {
                    checkOutputDir();
                    File screenshotFile = new File(checkOutputDir(), String.format(stringFormatLocale,
                            "step-%d-%s-%s-%s.png", timeStep, sid, aid, timeMillis));
                    Logger.infoFormat("Saving screen shot to %s at step %d %s %s",
                            screenshotFile, timeStep, sid, aid);
                    takeScreenshot(screenshotFile);
                }

                ModelAction modelAction = new ModelAction(type, topActivityName, mReusablePointFloats, mReusableRect);
                modelAction.setThrottle(operate.throttle);

                // Complete the info for specific action type
                switch (type) {
                    case CLICK:
                        modelAction.setInputText(operate.text);
                        modelAction.setClearText(operate.clear);
                        modelAction.setEditText(operate.editable);
                        modelAction.setRawInput(operate.rawinput);
                        break;
                    case LONG_CLICK:
                        modelAction.setWaitTime(operate.waitTime);
                        break;
                    case SHELL_EVENT:
                        modelAction.setShellCommand(operate.text);
                        modelAction.setWaitTime(operate.waitTime);
                        break;
                    default:
                        break;
                }

                generateEventsForAction(modelAction);

                // check if could select next fuzz action from full fuzz-able action options.
                switch (type) {
                    case RESTART:
                    case CLEAN_RESTART:
                    case CRASH:
                        fullFuzzing = false;
                        break;
                    case BACK:
                        fullFuzzing = !AndroidDevice.isAtAppMain(topActivityName.getClassName(), topActivityName.getPackageName());
                        break;
                    default:
                        fullFuzzing = !AndroidDevice.isAtPhoneLauncher(topActivityName.getClassName());
                        break;
                }

            } catch (Exception e) {
                e.printStackTrace();
                generateThrottleEvent(mThrottle);
            }
        } else {
            if (mVerbose > 0) Logger.println(
                    "// top activity is null or the corresponding tree is null, " +
                    "accessibility maybe error, fuzz needed."
            );
            fuzzingAction = generateFuzzingAction(fullFuzzing);
            generateEventsForAction(fuzzingAction);
        }

        if (allowFuzzing && fuzzingAction == null && RandomHelper.toss(fuzzingRate)) {
            if (mVerbose > 0) Logger.println("// generate fuzzing action.");
            fuzzingAction = generateFuzzingAction(fullFuzzing);
            generateEventsForAction(fuzzingAction);
        }

        if (mVerbose > 0) {
            long total = System.currentTimeMillis() - start;
            Logger.println(" event time:" + Long.toString(total));
            if (tGetRootSlow > 0 || tDumpXml > 50 || total > 200) {
                Logger.println("// event time: getTop=" + tGetTop + " getRoot=" + tGetRoot
                        + (usedGetRootSlow ? " getRootSlow=" + tGetRootSlow : "")
                        + " dumpBinary=" + tDumpBinary + " dumpXml=" + tDumpXml + " ms");
            }
        }
    }

    private ImageWriterQueue nextImageWriter() {
        return mImageWriters[mRandom.nextInt(mImageWriters.length)];
    }

    private void takeScreenshot(File screenshotFile) {
        Bitmap map = mUiAutomation.takeScreenshot();
        nextImageWriter().add(map, screenshotFile);
    }

    /** Native adds throttle after action; base does not. */
    @Override
    protected void generateEventsForAction(Action action) {
        super.generateEventsForAction(action);
        long throttle = (action instanceof FuzzAction ? 0 : action.getThrottle());
        generateThrottleEvent(throttle);
    }

    /** When simplify, prefers native fuzz (performance §3.3). */
    @Override
    protected FuzzAction generateFuzzingAction(boolean sampleFromAllFuzzingActions) {
        List<CustomEvent> events;
        if (sampleFromAllFuzzingActions) {
            events = CustomEventFuzzer.generateFuzzingEvents();
            return new FuzzAction(events);
        } else {
            // Prefer native fuzz for simplify (performance §3.3); reuse list (§8.3). Use focused display bounds (SCRCPY_VS_FASTBOT_OPTIMIZATION_ANALYSIS §三.1).
            Rect bounds = AndroidDevice.getDisplayBounds(AndroidDevice.getFocusedDisplayId());
            int w = bounds.width();
            int h = bounds.height();
            mReusableFuzzEvents.clear();
            int repeat = RandomHelper.nextBetween(1, 3);
            for (int i = 0; i < repeat; i++) {
                String json = AiClient.getNextFuzzAction(w, h, true);
                if (json != null) {
                    mReusableFuzzEvents.addAll(CustomEventFuzzer.fromNativeFuzzJson(json));
                }
            }
            if (mReusableFuzzEvents.isEmpty()) {
                return new FuzzAction(CustomEventFuzzer.generateSimplifyFuzzingEvents());
            }
            return new FuzzAction(new ArrayList<>(mReusableFuzzEvents));
        }
    }
}
