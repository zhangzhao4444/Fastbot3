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
import static com.android.commands.monkey.utils.Config.historyRestartRate;
import static com.android.commands.monkey.utils.Config.homeAfterNSecondsofsleep;
import static com.android.commands.monkey.utils.Config.homingRate;
import static com.android.commands.monkey.utils.Config.schemaTraversalMode;
import static com.android.commands.monkey.utils.Config.scrollAfterNSecondsofsleep;
import static com.android.commands.monkey.utils.Config.startAfterDoScrollAction;
import static com.android.commands.monkey.utils.Config.startAfterDoScrollActionTimes;
import static com.android.commands.monkey.utils.Config.startAfterDoScrollBottomAction;
import static com.android.commands.monkey.utils.Config.startAfterDoScrollBottomActionTimes;
import static com.android.commands.monkey.utils.Config.startAfterNSecondsofsleep;
import static com.android.commands.monkey.utils.Config.swipeDuration;
import static com.android.commands.monkey.utils.Config.throttleForExecPreSchema;
import static com.android.commands.monkey.utils.Config.throttleForExecPreShell;
import static com.android.commands.monkey.utils.Config.useRandomClick;

import android.content.ComponentName;
import android.content.pm.ActivityInfo;
import android.content.pm.PackageInfo;
import android.content.pm.PackageManager;
import android.graphics.PointF;
import android.graphics.Rect;
import android.hardware.display.DisplayManagerGlobal;
import android.os.Build;
import android.os.SystemClock;
import android.util.DisplayMetrics;
import android.view.Display;
import android.view.IWindowManager;
import android.view.KeyCharacterMap;
import android.view.KeyEvent;
import android.view.MotionEvent;

import com.android.commands.monkey.action.Action;
import com.android.commands.monkey.action.FuzzAction;
import com.android.commands.monkey.action.ModelAction;
import com.android.commands.monkey.events.CustomEvent;
import com.android.commands.monkey.events.MonkeyEvent;
import com.android.commands.monkey.events.MonkeyEventQueue;
import com.android.commands.monkey.events.base.MonkeyActivityEvent;
import com.android.commands.monkey.events.base.MonkeyCommandEvent;
import com.android.commands.monkey.events.base.MonkeyDataActivityEvent;
import com.android.commands.monkey.events.base.MonkeyIMEEvent;
import com.android.commands.monkey.events.base.MonkeyKeyEvent;
import com.android.commands.monkey.events.base.MonkeySchemaEvent;
import com.android.commands.monkey.events.base.MonkeyThrottleEvent;
import com.android.commands.monkey.events.base.MonkeyTouchEvent;
import com.android.commands.monkey.events.base.MonkeyWaitEvent;
import com.android.commands.monkey.events.base.mutation.MutationAirplaneEvent;
import com.android.commands.monkey.events.base.mutation.MutationAlwaysFinishActivityEvent;
import com.android.commands.monkey.events.base.mutation.MutationWifiEvent;
import com.android.commands.monkey.events.customize.ClickEvent;
import com.android.commands.monkey.events.customize.DragEvent;
import com.android.commands.monkey.events.customize.PinchOrZoomEvent;
import com.android.commands.monkey.events.customize.ShellEvent;
import com.android.commands.monkey.fastbot.client.ActionType;
import com.android.commands.monkey.framework.AndroidDevice;
import com.android.commands.monkey.provider.SchemaProvider;
import com.android.commands.monkey.provider.ShellProvider;
import com.android.commands.monkey.utils.Config;
import com.android.commands.monkey.utils.Logger;
import com.android.commands.monkey.utils.MonkeyUtils;
import com.android.commands.monkey.utils.RandomHelper;
import com.android.commands.monkey.utils.UUIDHelper;
import com.android.commands.monkey.utils.Utils;
import com.android.commands.monkey.utils.AndroidVersions;
import com.bytedance.fastbot.AiClient;

import java.io.File;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.HashMap;
import java.util.HashSet;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.Random;
import java.util.Stack;

/**
 * Abstract base for Ape-style event sources (Native and U2).
 * Holds shared logic for throttle/click/scroll/input, permissions, activity events, schema/shell, and coverage.
 */
public abstract class MonkeySourceApeBase {

    protected static final long CLICK_WAIT_TIME = 0L;
    protected static final long LONG_CLICK_WAIT_TIME = 1000L;
    /** Number of random click events to generate when action bounds are invalid (e.g. zero-area rect). */
    protected static final int INVALID_BOUNDS_RANDOM_CLICK_COUNT = 3;

    protected final MonkeyEventQueue mQ;
    protected Random mRandom;
    protected long mThrottle = defaultGUIThrottle;
    protected boolean mRandomizeThrottle = false;
    protected int mVerbose = 0;
    protected List<ComponentName> mMainApps;
    protected Map<String, String[]> packagePermissions;
    protected int statusBarHeight = bytestStatusBarHeight;
    protected File mOutputDirectory;

    protected int timestamp = 0;
    protected int lastInputTimestamp = -1;
    protected int mEventId = 0;
    protected HashSet<String> activityHistory = new HashSet<>();
    protected String currentActivity = "";
    protected HashSet<String> mTotalActivities = new HashSet<>();
    protected HashSet<String> stubActivities = new HashSet<>();
    protected HashSet<String> pluginActivities = new HashSet<>();
    protected static Locale stringFormatLocale = Locale.ENGLISH;
    protected int timeStep = 0;
    protected boolean firstExecShell = true;
    protected boolean firstSchema = true;
    protected Stack<String> schemaStack = new Stack<>();
    protected String appVersion = "";
    protected String packageName = "";
    protected String intentAction = null;
    protected String intentData = null;
    protected String quickActivity = null;
    protected int appRestarted = 0;
    protected boolean fullFuzzing = true;
    /** deviceid from /sdcard/max.uuid */
    protected String did = UUIDHelper.read();

    protected final Rect mReusableRect = new Rect();
    protected final List<PointF> mReusablePointFloats = new ArrayList<>();
    protected final float[] mShieldXCoords = new float[10];
    protected final float[] mShieldYCoords = new float[10];
    protected File mCachedOutputDir;

    private static KeyCharacterMap sKeyCharMapVirtual;
    private static KeyCharacterMap sKeyCharMapAlpha;

    protected MonkeySourceApeBase(Random random, List<ComponentName> mainApps,
                                  long throttle, boolean randomizeThrottle, File outputDirectory) {
        mRandom = random;
        mMainApps = mainApps;
        mThrottle = throttle;
        mRandomizeThrottle = randomizeThrottle;
        mQ = new MonkeyEventQueue(random, 0, false);
        mOutputDirectory = outputDirectory;
        packagePermissions = new HashMap<>();
        for (ComponentName app : mainApps) {
            packagePermissions.put(app.getPackageName(), AndroidDevice.getGrantedPermissions(app.getPackageName()));
        }
        getTotalActivities();
    }

    protected static float lerp(float a, float b, float alpha) {
        return (b - a) * alpha + a;
    }

    protected void sleep(long time) {
        try {
            Thread.sleep(time);
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
        }
    }

    public void setVerbose(int verbose) {
        mVerbose = verbose;
    }

    public Random getRandom() {
        return mRandom;
    }

    public int getStatusBarHeight() {
        if (this.statusBarHeight == 0) {
            Display display = DisplayManagerGlobal.getInstance().getRealDisplay(Display.DEFAULT_DISPLAY);
            DisplayMetrics dm = new DisplayMetrics();
            display.getMetrics(dm);
            int w = display.getWidth();
            int h = display.getHeight();
            if (w == 1080 && h > 2100) {
                statusBarHeight = (int) (40 * dm.density);
            } else if (w == 1200 && h == 1824) {
                statusBarHeight = (int) (30 * dm.density);
            } else if (w == 1440 && h == 2696) {
                statusBarHeight = (int) (30 * dm.density);
            } else {
                statusBarHeight = (int) (24 * dm.density);
            }
        }
        return this.statusBarHeight;
    }

    protected ComponentName getTopActivityComponentName() {
        return AndroidDevice.getTopActivityComponentName();
    }

    public ComponentName randomlyPickMainApp() {
        int total = mMainApps.size();
        int index = mRandom.nextInt(total);
        return mMainApps.get(index);
    }

    protected void startRandomMainApp() {
        generateActivityEvents(randomlyPickMainApp(), false, false);
    }

    protected final void addEvent(MonkeyEvent event) {
        mQ.addLast(event);
        event.setEventId(mEventId++);
    }

    protected final void addEvents(List<MonkeyEvent> events) {
        for (int i = 0; i < events.size(); i++) {
            addEvent(events.get(i));
        }
    }

    protected final void clearEvent() {
        while (!mQ.isEmpty()) {
            mQ.removeFirst();
        }
    }

    protected final boolean hasEvent() {
        return !mQ.isEmpty();
    }

    protected final MonkeyEvent popEvent() {
        return mQ.isEmpty() ? null : mQ.removeFirst();
    }

    public void setAttribute(String packageName, String appVersion, String intentAction, String intentData, String quickActivity) {
        this.packageName = packageName;
        this.appVersion = (!appVersion.equals("")) ? appVersion : getAppVersionCode();
        this.intentAction = intentAction;
        this.intentData = intentData;
        this.quickActivity = quickActivity;
    }

    public void initReuseAgent() {
        AiClient.InitAgent(AiClient.AlgorithmType.Reuse, this.packageName);
    }

    public File getOutputDir() {
        return mOutputDirectory;
    }

    public void clearPackage(String packageName) {
        String[] permissions = this.packagePermissions.get(packageName);
        if (permissions == null) {
            Logger.warningPrintln("Stop clearing untracked package: " + packageName);
            return;
        }
        if (AndroidDevice.clearPackage(packageName, permissions)) {
            Logger.infoPrintln("Package " + packageName + " cleared.");
        }
    }

    public void grantRuntimePermissions(String packageName, String reason) {
        String[] permissions = this.packagePermissions.get(packageName);
        if (permissions == null) {
            Logger.warningPrintln("Stop granting permissions to untracked package: " + packageName);
            return;
        }
        AndroidDevice.grantRuntimePermissions(packageName, permissions, reason);
    }

    public void grantRuntimePermissions(String reason) {
        for (ComponentName cn : mMainApps) {
            grantRuntimePermissions(cn.getPackageName(), reason);
        }
    }

    public void startMutation(IWindowManager iwm, android.app.IActivityManager iam, int verbose) {
        MonkeyEvent event = null;
        double total = Config.doMutationAirplaneFuzzing + Config.doMutationMutationAlwaysFinishActivitysFuzzing
                + Config.doMutationWifiFuzzing;
        double rate = RandomHelper.nextDouble();
        if (rate < Config.doMutationMutationAlwaysFinishActivitysFuzzing) {
            event = new MutationAlwaysFinishActivityEvent();
        } else if (rate < Config.doMutationMutationAlwaysFinishActivitysFuzzing
                + Config.doMutationWifiFuzzing) {
            event = new MutationWifiEvent();
        } else if (rate < total) {
            event = new MutationAirplaneEvent();
        }
        if (event != null) {
            event.injectEvent(iwm, iam, mVerbose);
        }
    }

    protected void restartPackage(ComponentName cn, boolean clearPackage, String reason) {
        if (doHoming && RandomHelper.toss(homingRate)) {
            if (mVerbose > 0) Logger.println("press HOME before app kill");
            generateKeyEvent(KeyEvent.KEYCODE_HOME);
            generateThrottleEvent(homeAfterNSecondsofsleep);
        }
        String pkg = cn.getPackageName();
        Logger.infoPrintln("Try to restart package " + pkg + " for " + reason);
        stopPackage(pkg);
        generateActivityEvents(cn, clearPackage, true);
    }

    protected void generateThrottleEvent(long base) {
        long throttle = base;
        if (mRandomizeThrottle && (throttle > 0)) {
            throttle = mRandom.nextLong();
            if (throttle < 0) throttle = -throttle;
            throttle %= base;
            ++throttle;
        }
        if (throttle < 0) throttle = -throttle;
        addEvent(MonkeyThrottleEvent.obtain(throttle));
    }

    protected void generateKeyEvent(int key) {
        MonkeyKeyEvent e = new MonkeyKeyEvent(KeyEvent.ACTION_DOWN, key);
        addEvent(e);
        e = new MonkeyKeyEvent(KeyEvent.ACTION_UP, key);
        addEvent(e);
    }

    protected void generateActivateEvent() {
        Logger.infoPrintln("generate app switch events.");
        generateAppSwitchEvent();
    }

    private void generateAppSwitchEvent() {
        generateKeyEvent(KeyEvent.KEYCODE_APP_SWITCH);
        generateThrottleEvent(500);
        if (RandomHelper.nextBoolean()) {
            if (mVerbose > 0) Logger.println("press HOME after app switch");
            generateKeyEvent(KeyEvent.KEYCODE_HOME);
        } else {
            if (mVerbose > 0) Logger.println("press BACK after app switch");
            generateKeyEvent(KeyEvent.KEYCODE_BACK);
        }
        generateThrottleEvent(mThrottle);
    }

    protected void attemptToSendTextByKeyEvents(String inputText) {
        char[] szRes = inputText.toCharArray();
        KeyCharacterMap charMap;
        if (Build.VERSION.SDK_INT >= AndroidVersions.API_11_ANDROID_3_0) {
            if (sKeyCharMapVirtual == null) sKeyCharMapVirtual = KeyCharacterMap.load(KeyCharacterMap.VIRTUAL_KEYBOARD);
            charMap = sKeyCharMapVirtual;
        } else {
            if (sKeyCharMapAlpha == null) sKeyCharMapAlpha = KeyCharacterMap.load(KeyCharacterMap.ALPHA);
            charMap = sKeyCharMapAlpha;
        }
        KeyEvent[] events = charMap.getEvents(szRes);
        for (int i = 0; i < events.length; i += 2) {
            generateKeyEvent(events[i].getKeyCode());
        }
        generateKeyEvent(KeyEvent.KEYCODE_ENTER);
    }

    protected PointF shieldBlackRect(PointF p) {
        Rect displayBounds = AndroidDevice.getDisplayBounds();
        float unitx = displayBounds.height() / 20.0f;
        float unity = displayBounds.width() / 10.0f;
        mShieldXCoords[0] = p.x;
        mShieldYCoords[0] = p.y;
        for (int i = 1; i < 10; i++) {
            int retryTimes = 10 - i;
            float nx = p.x + retryTimes * unitx * RandomHelper.nextInt(8);
            float ny = p.y + retryTimes * unity * RandomHelper.nextInt(17);
            mShieldXCoords[i] = nx % displayBounds.width();
            mShieldYCoords[i] = ny % displayBounds.height();
        }
        boolean[] inShield = AiClient.checkPointsInShield(this.currentActivity, mShieldXCoords, mShieldYCoords);
        if (inShield != null) {
            for (int i = 0; i < inShield.length; i++) {
                if (!inShield[i]) {
                    return new PointF(mShieldXCoords[i], mShieldYCoords[i]);
                }
            }
        }
        // All 10 candidates in black: try display center as fallback to avoid clicking black area
        float cx = displayBounds.exactCenterX();
        float cy = displayBounds.exactCenterY();
        boolean[] centerShield = AiClient.checkPointsInShield(this.currentActivity, new float[]{cx}, new float[]{cy});
        if (centerShield != null && centerShield.length > 0 && !centerShield[0]) {
            return new PointF(cx, cy);
        }
        return new PointF(mShieldXCoords[9], mShieldYCoords[9]);
    }

    protected void generateClickEventAt(Rect nodeRect, long waitTime) {
        generateClickEventAt(nodeRect, waitTime, useRandomClick);
    }

    protected void generateClickEventAt(Rect nodeRect, long waitTime, boolean useRandomClick) {
        Rect bounds = nodeRect;
        if (bounds == null) {
            Logger.warningPrintln("Error to fetch bounds.");
            bounds = AndroidDevice.getDisplayBounds();
        }
        PointF p1;
        if (useRandomClick) {
            int width = bounds.width() > 0 ? getRandom().nextInt(bounds.width()) : 0;
            int height = bounds.height() > 0 ? getRandom().nextInt(bounds.height()) : 0;
            p1 = new PointF(bounds.left + width, bounds.top + height);
        } else {
            p1 = new PointF(bounds.left + bounds.width() / 2.0f, bounds.top + bounds.height() / 2.0f);
        }
        if (!bounds.contains((int) p1.x, (int) p1.y)) {
            Logger.warningFormat("Invalid bounds: %s", bounds);
            generateRandomClickEventsWithin(AndroidDevice.getDisplayBounds(), waitTime, INVALID_BOUNDS_RANDOM_CLICK_COUNT);
            return;
        }
        p1 = shieldBlackRect(p1);
        long downAt = SystemClock.uptimeMillis();
        addEvent(new MonkeyTouchEvent(MotionEvent.ACTION_DOWN).setDownTime(downAt).addPointer(0, p1.x, p1.y).setIntermediateNote(false));
        if (waitTime > 0) {
            addEvent(new MonkeyWaitEvent(waitTime));
        }
        addEvent(new MonkeyTouchEvent(MotionEvent.ACTION_UP).setDownTime(downAt).addPointer(0, p1.x, p1.y).setIntermediateNote(false));
    }

    /**
     * Generate several random click events within the given bounds (e.g. display bounds).
     * Used as fallback when action bounds are invalid (e.g. zero-area rect).
     */
    protected void generateRandomClickEventsWithin(Rect bounds, long waitTime, int count) {
        if (bounds == null || bounds.width() <= 0 || bounds.height() <= 0) {
            bounds = AndroidDevice.getDisplayBounds();
        }
        int w = bounds.width();
        int h = bounds.height();
        if (w <= 0 || h <= 0) {
            return;
        }
        for (int i = 0; i < count; i++) {
            int x = bounds.left + getRandom().nextInt(w);
            int y = bounds.top + getRandom().nextInt(h);
            PointF p = shieldBlackRect(new PointF(x, y));
            long downAt = SystemClock.uptimeMillis();
            addEvent(new MonkeyTouchEvent(MotionEvent.ACTION_DOWN).setDownTime(downAt).addPointer(0, p.x, p.y).setIntermediateNote(false));
            if (waitTime > 0) {
                addEvent(new MonkeyWaitEvent(waitTime));
            }
            addEvent(new MonkeyTouchEvent(MotionEvent.ACTION_UP).setDownTime(downAt).addPointer(0, p.x, p.y).setIntermediateNote(false));
        }
    }

    protected void generateScrollEventAt(Rect nodeRect, ActionType type) {
        Rect displayBounds = AndroidDevice.getDisplayBounds();
        if (nodeRect == null) {
            nodeRect = AndroidDevice.getDisplayBounds();
        }
        PointF start = new PointF(nodeRect.exactCenterX(), nodeRect.exactCenterY());
        PointF end;
        switch (type) {
            case SCROLL_BOTTOM_UP:
                int top = getStatusBarHeight();
                if (top < displayBounds.top) top = displayBounds.top;
                end = new PointF(start.x, top);
                break;
            case SCROLL_TOP_DOWN:
                end = new PointF(start.x, displayBounds.bottom - 1);
                break;
            case SCROLL_LEFT_RIGHT:
                end = new PointF(displayBounds.right - 1, start.y);
                break;
            case SCROLL_RIGHT_LEFT:
                end = new PointF(displayBounds.left, start.y);
                break;
            default:
                throw new RuntimeException("Should not reach here");
        }
        long downAt = SystemClock.uptimeMillis();
        addEvent(new MonkeyTouchEvent(MotionEvent.ACTION_DOWN).setDownTime(downAt).addPointer(0, start.x, start.y).setIntermediateNote(false).setType(1));
        int steps = 10;
        long waitTime = swipeDuration / steps;
        for (int i = 0; i < steps; i++) {
            float alpha = i / (float) steps;
            addEvent(new MonkeyTouchEvent(MotionEvent.ACTION_MOVE).setDownTime(downAt)
                    .addPointer(0, lerp(start.x, end.x, alpha), lerp(start.y, end.y, alpha)).setIntermediateNote(true).setType(1));
            addEvent(new MonkeyWaitEvent(waitTime));
        }
        addEvent(new MonkeyTouchEvent(MotionEvent.ACTION_UP).setDownTime(downAt).addPointer(0, end.x, end.y).setIntermediateNote(false).setType(1));
    }

    protected void doInput(ModelAction action) {
        String inputText = action.getInputText();
        boolean useAdbInput = action.isUseAdbInput();
        // Only send IME/text input when the action target is an editable widget (EditText).
        // Otherwise e.g. clicking a button ("获取短信验证码") would focus the button and
        // commitText() would go nowhere, so nothing appears in the UI.
        if (inputText != null && !inputText.equals("") && action.isEditText()) {
            if (mVerbose > 0) Logger.println("Input text is " + inputText);
            if (action.isClearText()) {
                generateClearEvent(action.getBoundingBox());
            }
            if (action.isRawInput()) {
                if (!AndroidDevice.sendText(inputText)) {
                    attemptToSendTextByKeyEvents(inputText);
                }
                return;
            }
            if (!useAdbInput) {
                if (mVerbose > 0) Logger.println("MonkeyIMEEvent added " + inputText);
                addEvent(new MonkeyIMEEvent(inputText));
            } else {
                if (mVerbose > 0) Logger.println("MonkeyCommandEvent added " + inputText);
                addEvent(new MonkeyCommandEvent("input text " + inputText));
            }
        } else if (inputText != null && !inputText.equals("")) {
            if (mVerbose > 0) Logger.println("Skip IME input for non-edit widget: " + inputText);
        } else {
            if (lastInputTimestamp == timestamp) {
                Logger.warningPrintln("checkVirtualKeyboard: Input only once.");
                return;
            }
            lastInputTimestamp = timestamp;
            if (action.isEditText() || AndroidDevice.isVirtualKeyboardOpened()) {
                generateKeyEvent(KeyEvent.KEYCODE_ESCAPE);
            }
        }
    }

    protected void generateClearEvent(Rect bounds) {
        generateClickEventAt(bounds, LONG_CLICK_WAIT_TIME);
        generateKeyEvent(KeyEvent.KEYCODE_DEL);
        generateClickEventAt(bounds, CLICK_WAIT_TIME);
    }

    protected void generateActivityEvents(ComponentName app, boolean clearPackage, boolean startFromHistory) {
        if (clearPackage) {
            clearPackage(app.getPackageName());
        }
        generateShellEvents();
        boolean startbyHistory = false;
        if (startFromHistory && doHistoryRestart && RandomHelper.toss(historyRestartRate)) {
            if (mVerbose > 0) Logger.println("start from history task");
            startbyHistory = true;
        }
        if (intentData != null) {
            addEvent(new MonkeyDataActivityEvent(app, intentAction, intentData, quickActivity, startbyHistory));
        } else {
            addEvent(new MonkeyActivityEvent(app, startbyHistory));
        }
        generateThrottleEvent(startAfterNSecondsofsleep);
        generateSchemaEvents();
        generateActivityScrollEvents();
    }

    protected void generateShellEvents() {
        if (execPreShell) {
            String command = ShellProvider.randomNext();
            if (!"".equals(command) && (firstExecShell || execPreShellEveryStartup)) {
                if (mVerbose > 0) Logger.println("shell: " + command);
                try {
                    AndroidDevice.executeCommandAndWaitFor(command.split(" "));
                    sleep(throttleForExecPreShell);
                    this.firstExecShell = false;
                } catch (Exception e) {
                    // ignore
                }
            }
        }
    }

    protected void generateSchemaEvents() {
        if (execSchema) {
            if (firstSchema || execSchemaEveryStartup) {
                String schema = SchemaProvider.randomNext();
                if (schemaTraversalMode) {
                    if (schemaStack.empty()) {
                        ArrayList<String> strings = SchemaProvider.getStrings();
                        for (String s : strings) schemaStack.push(s);
                    }
                    if (schemaStack.empty()) return;
                    schema = schemaStack.pop();
                }
                if ("".equals(schema)) return;
                if (mVerbose > 0) Logger.println("fastbot exec schema: " + schema);
                addEvent(new MonkeySchemaEvent(schema));
                generateThrottleEvent(throttleForExecPreSchema);
                this.firstSchema = false;
            }
        }
    }

    protected void generateActivityScrollEvents() {
        if (startAfterDoScrollAction) {
            int i = startAfterDoScrollActionTimes;
            while (i-- > 0) {
                generateScrollEventAt(AndroidDevice.getDisplayBounds(), SCROLL_TOP_DOWN);
                generateThrottleEvent(scrollAfterNSecondsofsleep);
            }
        }
        if (startAfterDoScrollBottomAction) {
            int i = startAfterDoScrollBottomActionTimes;
            while (i-- > 0) {
                generateScrollEventAt(AndroidDevice.getDisplayBounds(), SCROLL_BOTTOM_UP);
                generateThrottleEvent(scrollAfterNSecondsofsleep);
            }
        }
    }

    protected void generateEventsForAction(Action action) {
        generateEventsForActionInternal(action);
    }

    protected void generateEventsForActionInternal(Action action) {
        ActionType actionType = action.getType();
        switch (actionType) {
            case FUZZ:
                generateFuzzingEvents((FuzzAction) action);
                break;
            case START:
                generateActivityEvents(randomlyPickMainApp(), false, false);
                break;
            case RESTART:
                restartPackage(randomlyPickMainApp(), false, "start action(RESTART)");
                break;
            case CLEAN_RESTART:
                restartPackage(randomlyPickMainApp(), true, "start action(CLEAN_RESTART)");
                break;
            case NOP:
                generateThrottleEvent(action.getThrottle());
                break;
            case ACTIVATE:
                generateActivateEvent();
                break;
            case BACK:
                generateKeyEvent(KeyEvent.KEYCODE_BACK);
                break;
            case CLICK:
                generateClickEventAt(((ModelAction) action).getBoundingBox(), CLICK_WAIT_TIME);
                doInput((ModelAction) action);
                break;
            case LONG_CLICK:
                long waitTime = ((ModelAction) action).getWaitTime();
                if (waitTime == 0) waitTime = LONG_CLICK_WAIT_TIME;
                generateClickEventAt(((ModelAction) action).getBoundingBox(), waitTime);
                break;
            case SCROLL_BOTTOM_UP:
            case SCROLL_TOP_DOWN:
            case SCROLL_LEFT_RIGHT:
            case SCROLL_RIGHT_LEFT:
                generateScrollEventAt(((ModelAction) action).getBoundingBox(), action.getType());
                break;
            case SCROLL_BOTTOM_UP_N:
                int scrollN = 3 + RandomHelper.nextInt(5);
                while (scrollN-- > 0) {
                    generateScrollEventAt(((ModelAction) action).getBoundingBox(), SCROLL_BOTTOM_UP);
                }
                break;
            case SHELL_EVENT:
                ModelAction modelAction = (ModelAction) action;
                ShellEvent shellEvent = new ShellEvent(modelAction.getShellCommand(), modelAction.getWaitTime());
                addEvents(shellEvent.generateMonkeyEvents());
                break;
            default:
                throw new RuntimeException("Should not reach here");
        }
    }

    protected void generateFuzzingEvents(FuzzAction action) {
        List<CustomEvent> events = action.getFuzzingEvents();
        long throttle = action.getThrottle();
        for (CustomEvent event : events) {
            if (event instanceof ClickEvent) {
                PointF point = ((ClickEvent) event).getPoint();
                point = shieldBlackRect(point);
                ((ClickEvent) event).setPoint(point);
            } else if (event instanceof DragEvent) {
                DragEvent drag = (DragEvent) event;
                PointF[] pts = drag.getPoints();
                if (pts != null) {
                    for (int i = 0; i < pts.length; i++) {
                        pts[i] = shieldBlackRect(pts[i]);
                    }
                    drag.setPoints(pts);
                }
            } else if (event instanceof PinchOrZoomEvent) {
                PinchOrZoomEvent pinch = (PinchOrZoomEvent) event;
                PointF[] pts = pinch.getPoints();
                if (pts != null) {
                    for (int i = 0; i < pts.length; i++) {
                        pts[i] = shieldBlackRect(pts[i]);
                    }
                    pinch.setPoints(pts);
                }
            }
            for (MonkeyEvent me : event.generateMonkeyEvents()) {
                if (me == null) throw new RuntimeException();
                addEvent(me);
            }
            generateThrottleEvent(throttle);
        }
    }

    protected void getTotalActivities() {
        try {
            for (String p : MonkeyUtils.getPackageFilter().getmValidPackages()) {
                PackageInfo packageInfo = AndroidDevice.packageManager.getPackageInfo(p, PackageManager.GET_ACTIVITIES);
                if (packageInfo != null) {
                    if ("com.android.packageinstaller".equals(packageInfo.packageName)) continue;
                    if (packageInfo.activities != null) {
                        for (ActivityInfo activityInfo : packageInfo.activities) {
                            mTotalActivities.add(activityInfo.name);
                        }
                    }
                }
            }
        } catch (Exception e) {
            // ignore
        }
    }

    protected String getAppVersionCode() {
        try {
            for (String p : MonkeyUtils.getPackageFilter().getmValidPackages()) {
                PackageInfo packageInfo = AndroidDevice.packageManager.getPackageInfo(p, PackageManager.GET_ACTIVITIES);
                if (packageInfo != null && packageInfo.packageName.equals(this.packageName)) {
                    return packageInfo.versionName != null ? packageInfo.versionName : "";
                }
            }
        } catch (Exception e) {
            // ignore
        }
        return "";
    }

    protected void printCoverage() {
        String[] testedActivities = this.activityHistory.toArray(new String[0]);
        Arrays.sort(testedActivities);
        HashSet<String> set = mTotalActivities;
        int j = 0;
        for (String activity : testedActivities) {
            if (set.contains(activity)) j++;
        }
        float f = set.size() > 0 ? 1.0f * j / set.size() * 100 : 0;
        if (mVerbose > 0) {
            Logger.println("Total app activities:");
            int i = 0;
            for (String activity : set) {
                i++;
                Logger.println(String.format(stringFormatLocale, "%4d %s", i, activity));
            }
            Logger.println("Explored app activities:");
            int k = 0;
            for (int idx = 0; idx < testedActivities.length; idx++) {
                String activity = testedActivities[idx];
                if (set.contains(activity)) {
                    Logger.println(String.format(stringFormatLocale, "%4d %s", k + 1, activity));
                    k++;
                }
            }
            Logger.println("Activity of Coverage: " + f + "%");
        }
        String[] totalActivities = set.toArray(new String[0]);
        Arrays.sort(totalActivities);
        Utils.activityStatistics(mOutputDirectory, testedActivities, totalActivities, new ArrayList<Map<String, String>>(), f, new HashMap<String, Integer>());
    }

    /** Subclasses implement to return a fuzzing action (e.g. from native or simplified list). */
    protected abstract FuzzAction generateFuzzingAction(boolean sampleFromAllFuzzingActions);
}
