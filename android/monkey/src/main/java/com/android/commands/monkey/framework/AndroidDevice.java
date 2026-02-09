/*
 * Copyright 2020 Advanced Software Technologies Lab at ETH Zurich, Switzerland
 *
 * Modified - Copyright (c) 2020 Bytedance Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

package com.android.commands.monkey.framework;

import android.app.ActivityManager.RunningAppProcessInfo;
import android.app.ActivityManager.RunningTaskInfo;
import android.app.IActivityManager;
import android.app.admin.IDevicePolicyManager;
import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.content.pm.IPackageManager;
import android.content.pm.IPackageStatsObserver;
import android.content.pm.PackageInfo;
import android.content.pm.PackageManager;
import android.content.pm.PackageStats;
import android.content.pm.ResolveInfo;
import android.util.Base64;
import android.util.DisplayMetrics;
import android.graphics.Point;
import android.graphics.Rect;
import android.hardware.display.DisplayManagerGlobal;
import android.os.Build;
import android.os.IBinder;
import android.os.IPowerManager;
import android.os.RemoteException;
import android.os.ServiceManager;
import android.os.UserHandle;
import android.provider.MediaStore;
import android.view.InputEvent;
import android.view.IWindowManager;
import android.view.KeyEvent;
import android.view.inputmethod.EditorInfo;
import android.view.inputmethod.InputMethodInfo;
import android.view.inputmethod.InputMethodManager;

import com.android.commands.monkey.utils.ContextUtils;
import com.android.commands.monkey.utils.InputUtils;
import com.android.commands.monkey.utils.Logger;
import com.android.commands.monkey.utils.Utils;
import com.android.commands.monkey.utils.AndroidVersions;
import com.android.internal.statusbar.IStatusBarService;
import com.android.internal.view.IInputMethodManager;

import java.io.BufferedReader;
import java.io.IOException;
import java.io.InputStreamReader;
import java.io.StringReader;
import java.util.ArrayList;
import java.util.HashSet;
import java.util.List;
import java.util.Locale;
import java.util.Set;
import java.lang.reflect.Method;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

import static com.android.commands.monkey.utils.Config.bytestStatusBarHeight;
import static com.android.commands.monkey.utils.Config.clearPackage;
import static com.android.commands.monkey.utils.Config.enableStopPackage;
import static com.android.commands.monkey.utils.Config.grantAllPermission;


/**
 * @author Zhao Zhang, Tianxiao Gu
 */

/**
 * Android framework utils
 */
public class AndroidDevice {

    public static IActivityManager iActivityManager;
    public static IWindowManager iWindowManager;
    public static IPackageManager iPackageManager;
    public static PackageManager packageManager;
    public static IDevicePolicyManager iDevicePolicyManager;
    public static IStatusBarService iStatusBarService;
    public static IInputMethodManager iInputMethodManager;
    public static InputMethodManager inputMethodManager;
    public static IPowerManager iPowerManager;
    public static boolean useADBKeyboard;
    public static Set<String> inputMethodPackages = new HashSet<>();
    static Pattern DISPLAY_FOCUSED_STACK_PATTERN = Pattern.compile("mLastFocusedStack=Task[{][a-z0-9]+.*StackId=([0-9]+).*");
    static Pattern FOCUSED_STACK_PATTERN = Pattern.compile("mFocusedStack=ActivityStack[{][a-z0-9]+ stackId=([0-9]+), [0-9]+ tasks[}]");
    static Pattern DISPLAY_PATTERN = Pattern.compile("^Display #([0-9]+) .*:$");
    static Pattern STACK_PATTERN = Pattern.compile("^  Stack #([0-9]+):.*$");
    static Pattern TASK_PATTERN = Pattern.compile("^    \\* Task.*#([0-9]+).*$");
    static Pattern ACTIVITY_PATTERN = Pattern.compile("^      [*] Hist #[0-9]+: ActivityRecord[{][0-9a-z]+ u[0-9]+ ([^ /]+)/([^ ]+) t[0-9]+[}]$");
    private static Set<String> blacklistPermissions = new HashSet<String>();
    /**
     * https://github.com/senzhk/ADBKeyBoard
     * ADB_INPUT_TEXT may not pass UTF-8 correctly via adb shell on Oreo/P;
     * ADB_INPUT_B64 sends Base64(UTF-8(text)) for reliable Chinese/emoji input.
     */
    private static String IME_MESSAGE = "ADB_INPUT_TEXT";
    private static String IME_MESSAGE_B64 = "ADB_INPUT_B64";
    private static String IME_CHARS = "ADB_INPUT_CHARS";
    private static String IME_KEYCODE = "ADB_INPUT_CODE";
    private static String IME_EDITORCODE = "ADB_EDITOR_CODE";
    private static String IME_ADB_KEYBOARD;

    /** Default path on device for ADBKeyBoard APK when using auto-install (e.g. push via activate_fastbot.sh). */
    private static final String ADBKEYBOARD_APK_PATH = "/data/local/tmp/ADBKeyBoard.apk";

    public static void initializeAndroidDevice(IActivityManager mAm, IWindowManager mWm, IPackageManager mPm, String keyboard) {
        iActivityManager = mAm;
        iWindowManager = mWm;
        iPackageManager = mPm;
        IME_ADB_KEYBOARD = keyboard;
        packageManager = ContextUtils.getSystemContext().getPackageManager();
        // Performance: non-startup services (IStatusBarService, IInputMethodManager, etc.) are lazy-loaded (SCRCPY_VS_FASTBOT_OPTIMIZATION_ANALYSIS §5).
    }

    /** Lazy-loaded; use this instead of direct field access. */
    public static IDevicePolicyManager getIDevicePolicyManager() {
        if (iDevicePolicyManager == null) {
            iDevicePolicyManager = IDevicePolicyManager.Stub.asInterface(ServiceManager.getService("device_policy"));
            if (iDevicePolicyManager == null) {
                System.err.println("** Error: Unable to connect to device policy manager; is the system running?");
            }
        }
        return iDevicePolicyManager;
    }

    /** Lazy-loaded; use this instead of direct field access. */
    public static IStatusBarService getIStatusBarService() {
        if (iStatusBarService == null) {
            iStatusBarService = IStatusBarService.Stub.asInterface(ServiceManager.getService("statusbar"));
            if (iStatusBarService == null) {
                System.err.println("** Error: Unable to connect to status bar service; is the system running?");
            }
        }
        return iStatusBarService;
    }

    /** Lazy-loaded; use this instead of direct field access. */
    public static IInputMethodManager getIInputMethodManager() {
        if (iInputMethodManager == null) {
            iInputMethodManager = IInputMethodManager.Stub.asInterface(ServiceManager.getService("input_method"));
            if (iInputMethodManager == null) {
                System.err.println("** Error: Unable to connect to input method manager service; is the system running?");
            }
        }
        return iInputMethodManager;
    }

    /** Lazy-loaded; use this instead of direct field access. Sets useADBKeyboard on first call. */
    public static InputMethodManager getInputMethodManager() {
        if (inputMethodManager == null) {
            inputMethodManager = (InputMethodManager) ContextUtils.getSystemContext().getSystemService(Context.INPUT_METHOD_SERVICE);
            useADBKeyboard = enableADBKeyboard();
        }
        return inputMethodManager;
    }

    /** Lazy-loaded; use this instead of direct field access. */
    public static IPowerManager getIPowerManager() {
        if (iPowerManager == null) {
            iPowerManager = IPowerManager.Stub.asInterface(ServiceManager.getService("power"));
            if (iPowerManager == null) {
                System.err.println("** Error: Unable to connect to power manager service; is the system running?");
            }
        }
        return iPowerManager;
    }

    /**
     * Try to get all enabled keyboards, and find ADBKeyboard. If ADBKeyBoard is not installed, tries
     * to install from {@link #ADBKEYBOARD_APK_PATH} (pm install -r). If installed but not enabled,
     * tries "ime enable &lt;id&gt;". Requires shell permission (e.g. when monkey runs via adb shell).
     *
     * @return If ADBKeyboard IME exists and is enabled (or was auto-installed/enabled), return true.
     */
    private static boolean enableADBKeyboard() {
        InputMethodManager imm = getInputMethodManager();
        List<InputMethodInfo> enabled = imm.getEnabledInputMethodList();
        if (enabled != null) {
            for (InputMethodInfo imi : enabled) {
                Logger.println("InputMethod ID: " + imi.getId());
                if (IME_ADB_KEYBOARD.equals(imi.getId())) {
                    Logger.println("Find Keyboard: " + IME_ADB_KEYBOARD);
                    return true;
                }
            }
        }
        List<InputMethodInfo> all = imm.getInputMethodList();
        boolean installed = false;
        if (all != null) {
            for (InputMethodInfo imi : all) {
                if (IME_ADB_KEYBOARD.equals(imi.getId())) {
                    installed = true;
                    break;
                }
            }
        }
        if (!installed) {
            Logger.println("ADBKeyBoard not installed, trying: pm install -r " + ADBKEYBOARD_APK_PATH);
            try {
                int ret = executeCommandAndWaitFor(new String[]{"pm", "install", "-r", ADBKEYBOARD_APK_PATH});
                if (ret == 0) {
                    all = imm.getInputMethodList();
                    if (all != null) {
                        for (InputMethodInfo imi : all) {
                            if (IME_ADB_KEYBOARD.equals(imi.getId())) {
                                installed = true;
                                Logger.println("ADBKeyBoard auto-installed successfully.");
                                break;
                            }
                        }
                    }
                }
            } catch (Exception e) {
                Logger.warningPrintln("Auto-install ADBKeyBoard failed: " + e.getMessage());
            }
            if (!installed) {
                return false;
            }
        }
        // Installed but not enabled: try ime enable
        Logger.println("ADBKeyBoard installed but not enabled, trying: ime enable " + IME_ADB_KEYBOARD);
        try {
            int ret = executeCommandAndWaitFor(new String[]{"ime", "enable", IME_ADB_KEYBOARD});
            if (ret == 0) {
                List<InputMethodInfo> enabledAfter = imm.getEnabledInputMethodList();
                if (enabledAfter != null) {
                    for (InputMethodInfo imi2 : enabledAfter) {
                        if (IME_ADB_KEYBOARD.equals(imi2.getId())) {
                            Logger.println("ADBKeyBoard auto-enabled successfully.");
                            return true;
                        }
                    }
                }
            }
        } catch (Exception e) {
            Logger.warningPrintln("Auto-enable ADBKeyBoard failed: " + e.getMessage());
        }
        return false;
    }

    // Performance: cache display bounds to avoid Binder + allocation on every call (PERFORMANCE_OPTIMIZATION_ITEMS §2.1).
    private static final Rect sDisplayBoundsCache = new Rect();
    private static final Point sDisplaySizeCache = new Point();
    private static boolean sDisplayBoundsCached = false;

    /** Returns cached display bounds for default display; callers must not mutate. Refreshed on first call. */
    public static Rect getDisplayBounds() {
        return getDisplayBounds(DEFAULT_DISPLAY_ID);
    }

    /**
     * Returns display bounds for the given display (SCRCPY_VS_FASTBOT_OPTIMIZATION_ANALYSIS §三.1).
     * For default display (0) uses cache; for other displays queries DisplayManagerGlobal each time.
     * Callers must not mutate the returned Rect.
     */
    public static Rect getDisplayBounds(int displayId) {
        if (displayId == DEFAULT_DISPLAY_ID) {
            if (sDisplayBoundsCached) {
                return sDisplayBoundsCache;
            }
            android.view.Display display = DisplayManagerGlobal.getInstance().getRealDisplay(android.view.Display.DEFAULT_DISPLAY);
            display.getSize(sDisplaySizeCache);
            sDisplayBoundsCache.set(0, 0, sDisplaySizeCache.x, sDisplaySizeCache.y);
            sDisplayBoundsCached = true;
            return sDisplayBoundsCache;
        }
        try {
            android.view.Display display = DisplayManagerGlobal.getInstance().getRealDisplay(displayId);
            if (display != null) {
                Point size = new Point();
                display.getSize(size);
                Rect out = new Rect(0, 0, size.x, size.y);
                return out;
            }
        } catch (Exception e) {
            Logger.warningPrintln("getDisplayBounds(displayId=" + displayId + ") failed: " + e.getMessage());
        }
        return getDisplayBounds(DEFAULT_DISPLAY_ID);
    }

    /** Invalidate display bounds cache (e.g. after rotation). */
    public static void invalidateDisplayBoundsCache() {
        sDisplayBoundsCached = false;
    }

    /**
     * Maps a point from source coordinate space to target display bounds (SCRCPY_VS_FASTBOT_OPTIMIZATION_ANALYSIS §三.1).
     * Like scrcpy PositionMapper: when coordinates come from another resolution/rotation (e.g. remote view),
     * scale and clamp so (x,y) corresponds to the device display space. Returns mapped point; null if source is empty.
     */
    public static Point mapPointToDisplay(float x, float y, int sourceWidth, int sourceHeight, Rect targetBounds) {
        if (sourceWidth <= 0 || sourceHeight <= 0 || targetBounds == null) {
            return null;
        }
        int tw = targetBounds.width();
        int th = targetBounds.height();
        if (tw <= 0 || th <= 0) {
            return null;
        }
        float scaleX = (float) tw / sourceWidth;
        float scaleY = (float) th / sourceHeight;
        int mx = (int) (x * scaleX);
        int my = (int) (y * scaleY);
        mx = Math.max(targetBounds.left, Math.min(targetBounds.right - 1, mx));
        my = Math.max(targetBounds.top, Math.min(targetBounds.bottom - 1, my));
        return new Point(mx, my);
    }

    // Performance: cache status bar / bottom bar heights; invalidate on rotation (SCRCPY_VS_FASTBOT_OPTIMIZATION_ANALYSIS §4).
    private static int sStatusBarHeight;
    private static int sBottomBarHeight;
    private static boolean sDisplayBarHeightsCached = false;

    /** Returns cached status bar height; refreshed on first call or after invalidateDisplayBarHeights(). */
    public static int getStatusBarHeight() {
        if (!sDisplayBarHeightsCached) {
            refreshDisplayBarHeights();
        }
        return sStatusBarHeight;
    }

    /** Returns cached bottom bar Y (display height - status bar height); refreshed on first call or after invalidateDisplayBarHeights(). */
    public static int getBottomBarHeight() {
        if (!sDisplayBarHeightsCached) {
            refreshDisplayBarHeights();
        }
        return sBottomBarHeight;
    }

    private static void refreshDisplayBarHeights() {
        android.view.Display display = DisplayManagerGlobal.getInstance().getRealDisplay(0);
        Point size = new Point();
        display.getSize(size);
        int w = size.x;
        int h = size.y;
        sStatusBarHeight = bytestStatusBarHeight;
        if (sStatusBarHeight == 0) {
            DisplayMetrics dm = ContextUtils.getSystemContext().getResources().getDisplayMetrics();
            if (w == 1080 && h > 2100) {
                sStatusBarHeight = (int) (40f * dm.density);
            } else if (w == 1200 && h == 1824) {
                sStatusBarHeight = (int) (30f * dm.density);
            } else if (w == 1440 && h == 2696) {
                sStatusBarHeight = (int) (30f * dm.density);
            } else {
                sStatusBarHeight = (int) (24f * dm.density);
            }
            sStatusBarHeight += 15;
        }
        sBottomBarHeight = h - sStatusBarHeight;
        sDisplayBarHeightsCached = true;
    }

    /** Invalidate display bar heights cache (e.g. after rotation). */
    public static void invalidateDisplayBarHeights() {
        sDisplayBarHeightsCached = false;
    }

    /** Default display id (primary display). Multi-display: use setInputEventDisplayId when displayId != 0 (API 29+). */
    public static final int DEFAULT_DISPLAY_ID = 0;

    /** Cached focused display id from top task; invalidate on demand. */
    private static int sFocusedDisplayIdCache = DEFAULT_DISPLAY_ID;
    private static boolean sFocusedDisplayIdCached = false;

    /**
     * Returns the display id of the top/focused task (SCRCPY_VS_FASTBOT_OPTIMIZATION_ANALYSIS §三.1).
     * Used so touch/key events are injected to the correct display in multi-display.
     * Uses RunningTaskInfo.displayId (API 24+) via reflection; falls back to DEFAULT_DISPLAY_ID.
     */
    public static int getFocusedDisplayId() {
        if (sFocusedDisplayIdCached) {
            return sFocusedDisplayIdCache;
        }
        try {
            List<RunningTaskInfo> taskInfo = APIAdapter.getTasks(AndroidDevice.iActivityManager, Integer.MAX_VALUE);
            if (taskInfo != null && !taskInfo.isEmpty()) {
                RunningTaskInfo task = taskInfo.get(0);
                for (Class<?> c = task.getClass(); c != null; c = c.getSuperclass()) {
                    try {
                        java.lang.reflect.Field field = c.getDeclaredField("displayId");
                        field.setAccessible(true);
                        int displayId = field.getInt(task);
                        if (displayId >= 0) {
                            sFocusedDisplayIdCache = displayId;
                        }
                        break;
                    } catch (NoSuchFieldException e) {
                        // try superclass (e.g. TaskInfo)
                    }
                }
            }
        } catch (Exception e) {
            Logger.warningPrintln("getFocusedDisplayId failed: " + e.getMessage());
        }
        sFocusedDisplayIdCached = true;
        Logger.println("displayId=" + sFocusedDisplayIdCache);
        return sFocusedDisplayIdCache;
    }

    /** Invalidate focused display id cache (e.g. after activity/display change). */
    public static void invalidateFocusedDisplayIdCache() {
        sFocusedDisplayIdCached = false;
    }

    /**
     * Whether input events are supported for the given display (SCRCPY_VS_FASTBOT_OPTIMIZATION_ANALYSIS §二.1).
     * Primary display (0) always; secondary displays only on API 29+.
     */
    public static boolean supportsInputEvents(int displayId) {
        return displayId == DEFAULT_DISPLAY_ID || Build.VERSION.SDK_INT >= AndroidVersions.API_29_ANDROID_10;
    }

    /**
     * Set display id on input event for multi-display (API 29+). No-op for primary (0).
     * Returns false if displayId != 0 and SDK &lt; 29 or reflection fails.
     */
    public static boolean setInputEventDisplayId(InputEvent event, int displayId) {
        if (event == null || displayId == DEFAULT_DISPLAY_ID) {
            return true;
        }
        if (Build.VERSION.SDK_INT < AndroidVersions.API_29_ANDROID_10 || !supportsInputEvents(displayId)) {
            return false;
        }
        try {
            Method setDisplayId = event.getClass().getMethod("setDisplayId", int.class);
            setDisplayId.invoke(event, displayId);
            return true;
        } catch (Exception e) {
            Logger.warningPrintln("setInputEventDisplayId failed: " + e.getMessage());
            return false;
        }
    }

    /**
     * Whether the given display is interactive (SCRCPY_VS_FASTBOT_OPTIMIZATION_ANALYSIS §二.3).
     * API 34+ uses isDisplayInteractive(displayId); otherwise isInteractive().
     */
    public static boolean isDisplayInteractive(int displayId) {
        IPowerManager pm = getIPowerManager();
        if (pm == null) {
            return false;
        }
        if (Build.VERSION.SDK_INT >= AndroidVersions.API_34_ANDROID_14) {
            try {
                Method m = pm.getClass().getMethod("isDisplayInteractive", int.class);
                Object r = m.invoke(pm, displayId);
                return Boolean.TRUE.equals(r);
            } catch (Exception e) {
                Logger.warningPrintln("isDisplayInteractive(displayId) failed, fallback to isInteractive: " + e.getMessage());
            }
        }
        try {
            return pm.isInteractive();
        } catch (RemoteException e) {
            Logger.warningPrintln("isInteractive() failed: " + e.getMessage());
            return false;
        }
    }

    /**
     * Placeholder for display power on/off (SCRCPY_VS_FASTBOT_OPTIMIZATION_ANALYSIS §二.4).
     * Full implementation would use SurfaceControl/setDisplayPowerMode per API and vendor.
     */
    public static boolean setDisplayPower(int displayId, boolean on) {
        Logger.println("setDisplayPower(displayId=" + displayId + ", on=" + on + ") not implemented; use 'input keyevent 26' to wake.");
        return false;
    }

    public static ComponentName getTopActivityComponentName() {
        try {
            List<RunningTaskInfo> taskInfo = APIAdapter.getTasks(AndroidDevice.iActivityManager, Integer.MAX_VALUE);
            if (taskInfo != null && !taskInfo.isEmpty()) {
                RunningTaskInfo task = taskInfo.get(0);
                return task.topActivity;
            }
        } catch (Exception e) {
            e.printStackTrace();
        }

        return null;
    }

    public static List<ActivityName> getCurrentTaskActivityStack() {
        StackInfo stackInfo = getFocusedStack();
        if (stackInfo != null && !stackInfo.getTasks().isEmpty()) {
            return stackInfo.getTasks().get(0).activityNames;
        }
        return null;
    }

    /**
     * Best-effort check: whether the soft keyboard (IME) is currently visible.
     * Uses only InputMethodManager.getInputMethodWindowVisibleHeight() to avoid
     * dumpsys format differences across Android versions and OEMs.
     * When the process has no IME client (e.g. monkey run as app_process), this
     * throws on the server side and we return false.
     * <p>
     * There is no other reliable cross-process API: getInputMethodWindowVisibleHeight
     * requires an IME client; dumpsys output format varies by OS/OEM. Prefer
     * {@code action.isEditText()} from the model (GUI tree / native) when available—
     * that is the only stable signal for "focus is on an input field" from monkey.
     */
    public static boolean isVirtualKeyboardOpened() {
        try {
            int height = getInputMethodManager().getInputMethodWindowVisibleHeight();
            return height != 0;
        } catch (IllegalArgumentException e) {
            // No IME client (e.g. app_process): server throws "unknown client".
            Logger.warningPrintln("getInputMethodWindowVisibleHeight failed (no IME client): " + e.getMessage());
            return false;
        }
    }

    public static void checkInteractive() {
        try {
            if (!isDisplayInteractive(DEFAULT_DISPLAY_ID)) {
                Logger.format("Power Manager says we are NOT interactive");
                int ret = Runtime.getRuntime().exec(new String[]{"input", "keyevent", "26"}).waitFor();
                Logger.format("Wakeup ret code %d %s", ret, (isDisplayInteractive(DEFAULT_DISPLAY_ID) ? "Interactive" : "Not interactive"));
            } else {
                Logger.format("Power Manager says we are interactive");
            }
        } catch (Exception e) {
            // TODO Auto-generated catch block
            e.printStackTrace();
        }
    }

    public static boolean checkAndSetInputMethod() {
        try {
            if (!useADBKeyboard) {
                return false;
            }
            InputUtils.switchToIme(IME_ADB_KEYBOARD);
            return true;
        } catch (SecurityException e) {
            e.printStackTrace();
        }
        return false;
    }

    public static String[] getGrantedPermissions(String packageName) {
        PackageInfo packageInfo = null;
        try {
            packageInfo = packageManager.getPackageInfo(packageName, PackageManager.GET_PERMISSIONS);

            if (packageInfo == null) {
                return new String[0];
            }
            if (packageInfo.requestedPermissions == null) {
                return new String[0];
            }
            for (String s : packageInfo.requestedPermissions) {
                Logger.debugFormat("%s requrested permission %s", packageName, s);
            }

            return packageInfo.requestedPermissions;
        } catch (PackageManager.NameNotFoundException e) {
            e.printStackTrace();
        }
        return null;
    }

    public static boolean grantRuntimePermission(String packageName, String permission) {
        try {
            int ret = executeCommandAndWaitFor(new String[]{"pm", "grant", packageName, permission});
            return ret == 0;
        } catch (Exception e) {
            Logger.warningFormat("Granting saved permission %s to %s results in error %s", permission, packageName, e);
        }
        return false;
    }

    public static boolean grantRuntimePermissions(String packageName, String[] savedPermissions, String reason) {
        try {
            Logger.infoFormat("Try to grant saved permission to %s for %s... ", packageName, reason);
            for (String permission : savedPermissions) {
                try {
                    Logger.infoFormat("Grant saved permission %s to %s... ", permission, packageName);
                    if (grantRuntimePermission(packageName, permission)) {
                        Logger.infoFormat("Permission %s is granted to %s... ", permission, packageName);
                    } else {
                        Logger.infoFormat("Permission %s is NOT granted to %s... ", permission, packageName);
                    }
                } catch (RuntimeException e) {
                    if (!blacklistPermissions.contains(permission)) {
                        Logger.warningFormat("Granting saved permission %s top %s results in error %s", permission,
                                packageName, e);
                    }
                    blacklistPermissions.add(permission);
                }
            }
        } catch (Exception e) {
            e.printStackTrace();
            return false;
        }
        return true;
    }

    private static void waitForNotify(Object lock) {
        synchronized (lock) {
            try {
                lock.wait(5000);
            } catch (InterruptedException e) {
                e.printStackTrace();
            } // at most wait for 5s
        }
    }

    /**
     * Used for getting package size, but could only be used when API version is no more than 26,
     * otherwise it will throw exceptions.
     * @param packageName The name of package of which you want to check size
     * @return If the query is successful, return true.
     */
    public static boolean checkNativeApp(String packageName) {
        final PackageStats[] result = new PackageStats[1];
        try {
            IPackageStatsObserver observer = new IPackageStatsObserver() {

                @Override
                public IBinder asBinder() {
                    return null;
                }

                @Override
                public void onGetStatsCompleted(PackageStats pStats, boolean succeeded) throws RemoteException {
                    synchronized (this) {
                        if (succeeded) {
                            result[0] = pStats;
                        }
                        this.notifyAll();
                    }

                }

            };
            iPackageManager.getPackageSizeInfo(packageName, UserHandle.myUserId(), observer);
            waitForNotify(observer);
            if (result[0] != null) {
                PackageStats stat = result[0];
                Logger.format("Code size: %d", stat.codeSize);
            }
        } catch (RemoteException e) {
            e.printStackTrace();
        } catch (java.lang.UnsupportedOperationException e){
            Logger.errorPrintln("Operation of getting package size is not support above api 26.");
        }
        return false;
    }

    public static int executeCommandAndWaitFor(String[] cmd) throws InterruptedException, IOException {
        return Runtime.getRuntime().exec(cmd).waitFor();
    }

    /**
     * Get all the pid of running app
     * @param packageName Package name of the running app
     * @return List of pid-s
     */
    public static List<Integer> getPIDs(String packageName) {
        List<Integer> pids = new ArrayList<Integer>(3);
        try {
            List<RunningAppProcessInfo> processes = iActivityManager.getRunningAppProcesses();
            for (RunningAppProcessInfo process : processes) {
                for (String pkg : process.pkgList) {
                    if (packageName.equals(pkg)) {
                        pids.add(process.pid);
                        break;
                    }
                }
            }
        } catch (RemoteException e) {
            e.printStackTrace();
        }
        return pids;
    }

    /**
     * Check if a crashed app is among applications we can switch to.
     * @param processName name of the crashed app
     * @param apps applications we are allowed to switch to
     * @return If this crashed app is among applications we can switch to, return true.
     */
    public static boolean isAppCrash(String processName, ArrayList<ComponentName> apps) {
        for (ComponentName cn : apps) {
            if (processName.contains(cn.getPackageName())) {
                Logger.println("// crash app's package is " + cn.getPackageName());
                return true;
            }
        }
        return false;
    }

    public static boolean stopPackage(String packageName) {
        if (enableStopPackage) {
            int retryCount = 10;
            while (retryCount-- > 0) {
                List<Integer> pids = getPIDs(packageName);
                if (pids.isEmpty()) {
                    return true;
                }
                Logger.println("Stop all packages, retry count " + retryCount);
                try {
                    Logger.println("Try to stop package " + packageName);
                    iActivityManager.forceStopPackage(packageName, UserHandle.myUserId());
                } catch (RemoteException e) {
                    e.printStackTrace();
                }
                try {
                    Thread.sleep(1000);
                } catch (InterruptedException e) {
                    e.printStackTrace();
                }
            }
        }
        return false;
    }

    /**
     * Clear android application user data
     * @param packageName The package name of which data to delete.
     * @return If succeed, return true.
     */
    private static boolean clearPackage(String packageName) {
        try {
            if (!clearPackage) {
                return true;
            }
            int ret = executeCommandAndWaitFor(new String[]{"pm", "clear", packageName});
            return ret == 0;
        } catch (Exception e) {
            Logger.warningFormat("Clear package %s results in error %s", packageName, e);
        }
        return false;
    }

    /**
     * Clear android application user data, if succeed and all requested permissions are
     * granted, revoke them.
     * @param packageName The package name of which data to delete.
     * @param savedPermissions The package name of which permission to revoke.
     * @return If succeed, return true.
     */
    public static boolean clearPackage(String packageName, String[] savedPermissions) {
        return clearPackage(packageName) && grantAllPermission && grantRuntimePermissions(packageName, savedPermissions, "clearing package");
    }

    public static boolean isInputMethod(String packageName) {
        return inputMethodPackages.contains(packageName);
    }

    public static boolean switchToLastInputMethod() {
        try {
            getIInputMethodManager().switchToLastInputMethod(null);
            return true;
        } catch (RemoteException e) {
            Logger.warningPrintln("Fail to switch to last input method");
            e.printStackTrace();
        }
        return false;
    }

    public static boolean isAtPhoneLauncher(String topActivity) {
        Intent intent = new Intent(Intent.ACTION_MAIN);
        intent.addCategory(Intent.CATEGORY_HOME);
        List<ResolveInfo> homeApps = APIAdapter.queryIntentActivities(packageManager, intent);
        final int NA = homeApps.size();
        for (int a = 0; a < NA; a++) {
            ResolveInfo r = homeApps.get(a);
            String activity = r.activityInfo.name;
            //Logger.println("// the top activity is " + topActivity + ", phone launcher activity is " + activity);
            if (topActivity.equals(activity)) {
                return true;
            }
        }
        return false;
    }

    public static boolean isAtPhoneCapture(String topActivity){
        Intent intent = new Intent(MediaStore.ACTION_IMAGE_CAPTURE);
        List<ResolveInfo> homeApps = APIAdapter.queryIntentActivities(packageManager, intent);
        final int NA = homeApps.size();
        for (int a = 0; a < NA; a++) {
            ResolveInfo r = homeApps.get(a);
            String activity = r.activityInfo.name;
            Logger.println("// the top activity is " + topActivity + ", phone capture activity is " + activity);

            if (topActivity.equals(activity)){
                return true;
            }
        }
        return false;
    }

    public static boolean isAtAppMain(String topActivityClassName, String topActivityPackageName) {
        Intent intent = new Intent(Intent.ACTION_MAIN);
        List<ResolveInfo> homeApps = APIAdapter.queryIntentActivities(packageManager, intent);
        final int NA = homeApps.size();
        for (int a = 0; a < NA; a++) {
            ResolveInfo r = homeApps.get(a);
            String packageName = r.activityInfo.applicationInfo.packageName;
            String activity = r.activityInfo.name;
            if (topActivityClassName.equals(activity) && packageName.equals(topActivityPackageName)) {
                return true;
            }
        }
        return false;
    }

    public static void sendIMEActionGo() {
        sendIMEAction(EditorInfo.IME_ACTION_GO);
    }

    public static void sendIMEAction(int actionId) {
        // adb shell am broadcast -a ADB_EDITOR_CODE --ei code 2
        Intent intent = new Intent();
        intent.setAction(IME_EDITORCODE);
        intent.putExtra("code", actionId);
        sendIMEIntent(intent);
    }

    /** Package part of {@link #IME_ADB_KEYBOARD} (e.g. com.android.adbkeyboard) for explicit broadcast. */
    private static String getImePackage() {
        if (IME_ADB_KEYBOARD == null) {
            return null;
        }
        int slash = IME_ADB_KEYBOARD.indexOf('/');
        return slash > 0 ? IME_ADB_KEYBOARD.substring(0, slash) : null;
    }

    public static boolean sendIMEIntent(Intent intent) {
        try {
            if (checkAndSetInputMethod()) {
                String pkg = getImePackage();
                if (pkg != null) {
                    intent.setPackage(pkg);
                }
                return broadcastIntent(intent);
            }
            return false;
        } finally {
        }
    }

    public static int startActivity(Intent intent) {
        try {
            Object object = APIAdapter.startActivity(iActivityManager, intent);
            if (object == null) {
                if (null != intent && null != intent.getComponent()) {
                    Logger.println("IActivityManager.startActivity failed, execute am start activity");
                    String activity = intent.getComponent().flattenToShortString();
                    executeCommandAndWaitFor(new String[]{"am", "start", "-n", activity});
                }
            }
        } catch (Exception e) {
            Logger.println("Start Activity error: " + e);
            return 0;
        }
        return 1;
    }

    public static int startUri(Intent intent) {
        try {
            Object object = APIAdapter.startActivity(iActivityManager, intent);
            if (object == null) {
                if (null != intent && null != intent.getData()) {
                    Logger.println("IActivityManager.startActivity failed, execute am start uri");
                    String uri = intent.getData().toString();
                    executeCommandAndWaitFor(new String[]{"am", "start", "-d", uri});
                }
            }
        } catch (Exception e) {
            Logger.println("Start Activity error: " + e);
            return 0;
        }
        return 1;
    }

    public static boolean sendChars(int[] chars) {
        Intent intent = new Intent();
        intent.setAction(IME_CHARS);
        intent.putExtra("chars", chars);
        return sendIMEIntent(intent);
    }

    public static boolean sendInputKeyCode(int keycode) {
        Intent intent = new Intent();
        intent.setAction(IME_KEYCODE);
        intent.putExtra("code", keycode);
        return sendIMEIntent(intent);
    }

    private static boolean broadcastIntent(Intent intent) {
        boolean ret = false;
        try {
            APIAdapter.broadcastIntent(iActivityManager, intent);
            ret = true;
        } catch (Exception e) {
            Logger.println("Broadcast Intent error: " + e);
        }
        return ret;
    }

    /**
     * Send text via ADBKeyBoard IME. Uses ADB_INPUT_B64 for any non-ASCII character
     * (Chinese, emoji, etc.) so that Unicode is reliably delivered; uses ADB_INPUT_TEXT
     * for pure ASCII. Requires ADBKeyBoard (senzhk/ADBKeyBoard) to be installed and
     * <b>enabled</b> in Settings → Languages & input → Keyboards (or: adb shell ime enable
     * com.android.adbkeyboard/.AdbIME). If not enabled, returns false.
     */
    /**
     * Send text using clipboard + Ctrl+V when possible; fall back to ADBKeyBoard broadcast
     * (ADB_INPUT_TEXT / ADB_INPUT_B64) when clipboard or key injection is not available.
     */
    public static boolean sendText(String text) {
        if (text == null) {
            return false;
        }

        // 0) Special flags handling (YADB-style extensions).
        final String FLAG_CLEAR = "~CLEAR~";
        final String FLAG_ENTER = "\\n";

        // If CLEAR flag is present, just clear current input and return.
        if (text.contains(FLAG_CLEAR)) {
            return clearCurrentInput();
        }

        // Replace encoded newlines with real '\n'.
        if (text.contains(FLAG_ENTER)) {
            text = text.replace(FLAG_ENTER, "\n");
        }

        // 1) Preferred path: clipboard + Ctrl+V (YADB-style).
        boolean clipboardOk = sendTextViaClipboard(text);
        if (clipboardOk) {
            return true;
        }

        // 2) Fallback path: keep existing ADBKeyBoard protocol for robustness.
        if (containsNonAscii(text)) {
            return sendTextViaB64(text);
        }
        Intent intent = new Intent();
        intent.setAction(IME_MESSAGE);
        intent.putExtra("msg", text);
        return sendIMEIntent(intent);
    }

    private static boolean containsNonAscii(CharSequence text) {
        for (int i = 0; i < text.length(); i++) {
            if (text.charAt(i) > 127) {
                return true;
            }
        }
        return false;
    }

    /**
     * Preferred text input implementation: write text to system clipboard and inject Ctrl+V
     * so that the focused input field pastes the content. Returns true on success.
     */
    public static boolean sendTextViaClipboard(String text) {
        try {
            if (text == null) {
                return false;
            }
            // Optimization: avoid redundant clipboard writes when content is unchanged.
            CharSequence current = ClipboardHelper.getText();
            if (current == null || !text.contentEquals(current)) {
                if (!ClipboardHelper.setText(text)) {
                    Logger.warningPrintln("sendTextViaClipboard: failed to write clipboard");
                    return false;
                }
            }
            // Inject Ctrl+V on the focused display to paste from clipboard.
            boolean injected = injectCtrlV();
            if (!injected) {
                Logger.warningPrintln("sendTextViaClipboard: inject Ctrl+V failed");
            }
            return injected;
        } catch (Throwable t) {
            Logger.warningPrintln("sendTextViaClipboard: exception: " + t);
            return false;
        }
    }

    private static boolean injectCtrlV() {
        int displayId = getFocusedDisplayId();
        int metaCtrl = KeyEvent.META_CTRL_ON | KeyEvent.META_CTRL_LEFT_ON;
        boolean ok = true;
        ok &= injectKeyEvent(KeyEvent.ACTION_DOWN, KeyEvent.KEYCODE_CTRL_LEFT, metaCtrl, displayId);
        ok &= injectKeyEvent(KeyEvent.ACTION_DOWN, KeyEvent.KEYCODE_V, metaCtrl, displayId);
        ok &= injectKeyEvent(KeyEvent.ACTION_UP, KeyEvent.KEYCODE_V, metaCtrl, displayId);
        ok &= injectKeyEvent(KeyEvent.ACTION_UP, KeyEvent.KEYCODE_CTRL_LEFT, 0, displayId);
        return ok;
    }

    /**
     * Clear current input field content: Ctrl+A (select all) + Delete.
     */
    public static boolean clearCurrentInput() {
        int displayId = getFocusedDisplayId();
        int metaCtrl = KeyEvent.META_CTRL_ON | KeyEvent.META_CTRL_LEFT_ON;
        boolean ok = true;
        // Ctrl+A to select all
        ok &= injectKeyEvent(KeyEvent.ACTION_DOWN, KeyEvent.KEYCODE_CTRL_LEFT, metaCtrl, displayId);
        ok &= injectKeyEvent(KeyEvent.ACTION_DOWN, KeyEvent.KEYCODE_A, metaCtrl, displayId);
        ok &= injectKeyEvent(KeyEvent.ACTION_UP, KeyEvent.KEYCODE_A, metaCtrl, displayId);
        ok &= injectKeyEvent(KeyEvent.ACTION_UP, KeyEvent.KEYCODE_CTRL_LEFT, 0, displayId);
        // Delete selection
        ok &= injectKeyEvent(KeyEvent.ACTION_DOWN, KeyEvent.KEYCODE_DEL, 0, displayId);
        ok &= injectKeyEvent(KeyEvent.ACTION_UP, KeyEvent.KEYCODE_DEL, 0, displayId);
        return ok;
    }

    private static boolean injectKeyEvent(int action, int keyCode, int metaState, int displayId) {
        try {
            long now = android.os.SystemClock.uptimeMillis();
            KeyEvent event = new KeyEvent(now, now, action, keyCode, 0, metaState);
            // Attach display id when possible so key goes to the correct display.
            InputEvent ie = event;
            setInputEventDisplayId(ie, displayId);
            // Use hidden InputManager via reflection to inject the event.
            Class<?> imClass = Class.forName("android.hardware.input.InputManager");
            Method getInstance = imClass.getMethod("getInstance");
            Object im = getInstance.invoke(null);
            Method inject = imClass.getMethod("injectInputEvent", InputEvent.class, int.class);
            Object result = inject.invoke(im, ie, 0 /*mode*/);
            return !(result instanceof Boolean) || (Boolean) result;
        } catch (Throwable t) {
            Logger.warningPrintln("injectKeyEvent failed: " + t);
            return false;
        }
    }

    /**
     * Send text via ADBKeyBoard ADB_INPUT_B64 (Base64-encoded UTF-8). Use this for
     * Chinese, emoji, and any Unicode when ADB_INPUT_TEXT might not preserve encoding.
     */
    public static boolean sendTextViaB64(String text) {
        if (text == null) {
            return false;
        }
        try {
            byte[] utf8 = text.getBytes("UTF-8");
            String b64 = Base64.encodeToString(utf8, Base64.NO_WRAP);
            Intent intent = new Intent();
            intent.setAction(IME_MESSAGE_B64);
            intent.putExtra("msg", b64);
            return sendIMEIntent(intent);
        } catch (java.io.UnsupportedEncodingException e) {
            return false;
        }
    }

    /**
     * Get activity stack through dumpsys
     * @return StackInfo object containing current activity stack
     */
    public static StackInfo getFocusedStack() {
        String[] cmd = new String[]{
                "dumpsys", "activity", "a"
        };

        try {
            String output = Utils.getProcessOutput(cmd);
            String line = null;
            Display currentDisplay = null;
            StackInfo currentStackInfo = null;
            Task currentTask = null;
            ActivityName currentActivityName = null;
            List<Display> displays = new ArrayList<>();
            BufferedReader br = new BufferedReader(new StringReader(output));
            while ((line = br.readLine()) != null) {
                Matcher m = DISPLAY_PATTERN.matcher(line);
                if (m.matches()) {
                    currentDisplay = new Display(Integer.parseInt(m.group(1)));
                    displays.add(currentDisplay);
                    continue;
                }
                m = STACK_PATTERN.matcher(line);
                if (m.matches() && currentDisplay != null) {
                    currentStackInfo = new StackInfo(Integer.parseInt(m.group(1)));

                    currentDisplay.stackInfos.add(currentStackInfo);
                    continue;
                }
                m = TASK_PATTERN.matcher(line);
                if (m.matches() && currentStackInfo != null) {
                    currentTask = new Task(Integer.parseInt(m.group(1)));
                    //Logger.println("// zhangzhao stack.id=" + currentStack.id + ", task.id=" + currentTask.id);
                    currentStackInfo.tasks.add(currentTask);
                    continue;
                }
                m = ACTIVITY_PATTERN.matcher(line);
                if (m.matches() && currentTask != null) {
                    String packageName = m.group(1);
                    String className = m.group(2);
                    if (className.startsWith(".")) {
                        className = packageName + className;
                    }
                    ComponentName comp = new ComponentName(packageName, className);
                    currentActivityName = new ActivityName(comp);
                    currentTask.activityNames.add(currentActivityName);
                    //Logger.println("// zhangzhao stack.id=" + currentStack.id + ", task.id=" + currentTask.id + ", act=" + currentActivity);
                    continue;
                }
                m = FOCUSED_STACK_PATTERN.matcher(line);
                if (m.find() && currentDisplay != null) {
                    currentDisplay.focusedStackId = Integer.parseInt(m.group(1));
                    continue;
                }
                m = DISPLAY_FOCUSED_STACK_PATTERN.matcher(line);
                if (m.find() && currentDisplay != null) {
                    currentDisplay.focusedStackId = Integer.parseInt(m.group(1));
                }
            }
            for (Display d : displays) {
                for (StackInfo s : d.stackInfos) {
                    if (s.id == d.focusedStackId) {
                        return s;
                    }
                }
            }
        } catch (IOException ignore) {
        } catch (InterruptedException ignore) {
        }
        return null;
    }


    public static void main(String[] args) {
        getFocusedStack();
    }

    public static class Display {
        int id;
        int focusedStackId;
        List<StackInfo> stackInfos = new ArrayList<>();

        public Display(int id) {
            this.id = id;
        }
    }

    public static class StackInfo {
        int id;
        List<Task> tasks = new ArrayList<>();

        public StackInfo(int id) {
            this.id = id;
        }

        public List<Task> getTasks() {
            return tasks;
        }

        public void dump() {
            Logger.infoFormat("Stack #%d, sz=%d", id, tasks.size());
            for (Task task : tasks) {
                Logger.infoFormat("- Task #%d, sz=%d", task.id, task.activityNames.size());
                for (ActivityName activityName : task.activityNames) {
                    Logger.infoFormat("  - %s", activityName.activity);
                }
            }
        }
    }

    public static class Task {
        int id;
        List<ActivityName> activityNames = new ArrayList<>();

        public Task(int id) {
            this.id = id;
        }

        public List<ActivityName> getActivityNames() {
            return this.activityNames;
        }
    }

    public static class ActivityName {
        public final ComponentName activity;

        public ActivityName(ComponentName activity) {
            this.activity = activity;
        }

        public ComponentName getActivity() {
            return activity;
        }
    }
}



