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
import android.view.inputmethod.InputMethodInfo;
import android.view.inputmethod.InputMethodManager;

import com.android.commands.monkey.framework.APIAdapter;
import com.android.commands.monkey.utils.Logger;
import com.android.commands.monkey.utils.ProcessUtils;
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
import java.util.regex.Matcher;
import java.util.regex.Pattern;

import static com.android.commands.monkey.utils.Config.bytestStatusBarHeight;
import static com.android.commands.monkey.utils.Config.clearPackage;
import static com.android.commands.monkey.utils.Config.enableStopPackage;
import static com.android.commands.monkey.utils.Config.grantAllPermission;


/**
 * Central access point for Android framework services and device utilities used by Monkey.
 * Holds lazy-initialized service references (IActivityManager, IWindowManager, etc.), display
 * bounds/rotation caches, permission and package helpers, and text input via clipboard + key injection.
 *
 * @author Zhao Zhang, Tianxiao Gu
 */
public class AndroidDevice {

    // ----- Core service references (set at startup; others lazy-loaded) -----

    public static IActivityManager iActivityManager;
    public static IWindowManager iWindowManager;
    /** Binder proxy to PackageManagerService; used for hidden APIs (e.g. getPackageSizeInfo). */
    public static IPackageManager iPackageManager;
    /** Public Context-based PackageManager; used for getPackageInfo, queryIntentActivities, etc. */
    public static PackageManager packageManager;

    private static IDevicePolicyManager iDevicePolicyManager;
    private static IStatusBarService iStatusBarService;
    private static IInputMethodManager iInputMethodManager;
    private static InputMethodManager inputMethodManager;
    private static IPowerManager iPowerManager;

    /** Package names of installed IMEs; populated by caller. Used to avoid targeting IME as app. */
    public static final Set<String> inputMethodPackages = new HashSet<>();

    /** Permissions that failed to grant at least once; skipped on subsequent grant attempts. */
    private static final Set<String> blacklistPermissions = new HashSet<>();

    /** Regex patterns for parsing "dumpsys activity a" output (Display/Stack/Task/Activity lines). */
    private static final Pattern DISPLAY_FOCUSED_STACK_PATTERN = Pattern.compile("mLastFocusedStack=Task[{][a-z0-9]+.*StackId=([0-9]+).*");
    private static final Pattern FOCUSED_STACK_PATTERN = Pattern.compile("mFocusedStack=ActivityStack[{][a-z0-9]+ stackId=([0-9]+), [0-9]+ tasks[}]");
    private static final Pattern DISPLAY_PATTERN = Pattern.compile("^Display #([0-9]+) .*:$");
    private static final Pattern STACK_PATTERN = Pattern.compile("^  Stack #([0-9]+):.*$");
    private static final Pattern TASK_PATTERN = Pattern.compile("^    \\* Task.*#([0-9]+).*$");
    private static final Pattern ACTIVITY_PATTERN = Pattern.compile("^      [*] Hist #[0-9]+: ActivityRecord[{][0-9a-z]+ u[0-9]+ ([^ /]+)/([^ ]+) t[0-9]+[}]$");

    /** Default display id (primary). Use setInputEventDisplayId when injecting to secondary display (API 29+). */
    public static final int DEFAULT_DISPLAY_ID = 0;

    private static final int WAIT_FOR_NOTIFY_MS = 5000;
    private static final int STOP_PACKAGE_RETRY_SLEEP_MS = 1000;

    /** KeyEvent.KEYCODE_WAKEUP (224): wake only, no toggle. Prefer over KEYCODE_POWER (26) to avoid turning screen off. */
    private static final int KEYCODE_WAKEUP = 224;

    // ----- Initialization -----

    /** Must be called early with system service proxies. PackageManager is obtained from APIAdapter.getSystemContext(). */
    public static void initializeAndroidDevice(IActivityManager mAm, IWindowManager mWm, IPackageManager mPm) {
        iActivityManager = mAm;
        iWindowManager = mWm;
        iPackageManager = mPm;
        packageManager = APIAdapter.getSystemContext().getPackageManager();
        // Non-startup services (IStatusBarService, IInputMethodManager, etc.) are lazy-loaded on first use.
    }

    // ----- Lazy-loaded system services (use getters instead of direct field access) -----

    public static IDevicePolicyManager getIDevicePolicyManager() {
        if (iDevicePolicyManager == null) {
            iDevicePolicyManager = IDevicePolicyManager.Stub.asInterface(ServiceManager.getService("device_policy"));
            if (iDevicePolicyManager == null) {
                System.err.println("** Error: Unable to connect to device policy manager; is the system running?");
            }
        }
        return iDevicePolicyManager;
    }

    public static IStatusBarService getIStatusBarService() {
        if (iStatusBarService == null) {
            iStatusBarService = IStatusBarService.Stub.asInterface(ServiceManager.getService("statusbar"));
            if (iStatusBarService == null) {
                System.err.println("** Error: Unable to connect to status bar service; is the system running?");
            }
        }
        return iStatusBarService;
    }

    public static IInputMethodManager getIInputMethodManager() {
        if (iInputMethodManager == null) {
            iInputMethodManager = IInputMethodManager.Stub.asInterface(ServiceManager.getService("input_method"));
            if (iInputMethodManager == null) {
                System.err.println("** Error: Unable to connect to input method manager service; is the system running?");
            }
        }
        return iInputMethodManager;
    }

    public static InputMethodManager getInputMethodManager() {
        if (inputMethodManager == null) {
            inputMethodManager = (InputMethodManager) APIAdapter.getSystemContext().getSystemService(Context.INPUT_METHOD_SERVICE);
        }
        return inputMethodManager;
    }

    public static IPowerManager getIPowerManager() {
        if (iPowerManager == null) {
            iPowerManager = IPowerManager.Stub.asInterface(ServiceManager.getService("power"));
            if (iPowerManager == null) {
                System.err.println("** Error: Unable to connect to power manager service; is the system running?");
            }
        }
        return iPowerManager;
    }

    // ----- Display bounds and mapping -----
    private static final Rect sDisplayBoundsCache = new Rect();
    private static final Point sDisplaySizeCache = new Point();
    private static boolean sDisplayBoundsCached = false;

    /** Returns cached display bounds for default display; callers must not mutate. Refreshed on first call. */
    public static Rect getDisplayBounds() {
        return getDisplayBounds(DEFAULT_DISPLAY_ID);
    }

    /**
     * Returns full physical display bounds (real size) for the given display. Use getRealSize() so
     * bounds match the coordinate space used for input injection across OEMs. Default display (0) is
     * cached; invalidate with invalidateDisplayBoundsCache() after rotation. On failure, falls back
     * to DisplayMetrics when possible.
     * <p>All callers use this for system-level input injection (random click/scroll/drag/pinch
     * within bounds, or clamping y using statusBarHeight/bottomBarHeight); hence physical screen
     * is required, not application-visible area (getSize would be wrong).
     */
    public static Rect getDisplayBounds(int displayId) {
        if (displayId == DEFAULT_DISPLAY_ID) {
            if (sDisplayBoundsCached) {
                return sDisplayBoundsCache;
            }
            try {
                android.view.Display display = DisplayManagerGlobal.getInstance().getRealDisplay(android.view.Display.DEFAULT_DISPLAY);
                if (display != null) {
                    display.getRealSize(sDisplaySizeCache);
                    sDisplayBoundsCache.set(0, 0, sDisplaySizeCache.x, sDisplaySizeCache.y);
                    sDisplayBoundsCached = true;
                    return sDisplayBoundsCache;
                }
            } catch (Exception e) {
                Logger.warningPrintln("getDisplayBounds(default) via DisplayManagerGlobal failed: " + e.getMessage());
            }
            // Fallback: DisplayMetrics from system context (works when DisplayManagerGlobal is unavailable on some OEMs).
            try {
                android.content.Context ctx = APIAdapter.getSystemContext();
                if (ctx != null) {
                    android.util.DisplayMetrics dm = ctx.getResources().getDisplayMetrics();
                    sDisplayBoundsCache.set(0, 0, dm.widthPixels, dm.heightPixels);
                    sDisplayBoundsCached = true;
                    return sDisplayBoundsCache;
                }
            } catch (Exception e) {
                Logger.warningPrintln("getDisplayBounds(default) DisplayMetrics fallback failed: " + e.getMessage());
            }
            sDisplayBoundsCache.set(0, 0, 0, 0);
            sDisplayBoundsCached = true;
            return sDisplayBoundsCache;
        }
        try {
            android.view.Display display = DisplayManagerGlobal.getInstance().getRealDisplay(displayId);
            if (display != null) {
                Point size = new Point();
                display.getRealSize(size);
                return new Rect(0, 0, size.x, size.y);
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
     * Maps (x,y) from source resolution to target display bounds: scale and clamp. Returns null if source or target is empty.
     * Kept for scrcpy / remote coordinate mapping (e.g. map viewer coordinates to device getDisplayBounds()).
     */
    @SuppressWarnings("unused")
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

    /** Cached status bar and bottom bar heights; invalidated on rotation. */
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
        int h = size.y;

        // 1) Prefer explicit config override.
        sStatusBarHeight = bytestStatusBarHeight;

        // 2) If not configured, query system dimen "status_bar_height" (standard cross-device pattern).
        //    See e.g. https://stackoverflow.com/questions/3407256/height-of-status-bar-in-android
        if (sStatusBarHeight == 0) {
            Context ctx = APIAdapter.getSystemContext();
            if (ctx != null) {
                try {
                    android.content.res.Resources res = ctx.getResources();
                    int resId = res.getIdentifier("status_bar_height", "dimen", "android");
                    if (resId > 0) {
                        sStatusBarHeight = res.getDimensionPixelSize(resId);
                    }
                    // 3) Fallback: approximate 24dp if system dimen is not available.
                    if (sStatusBarHeight == 0) {
                        DisplayMetrics dm = res.getDisplayMetrics();
                        sStatusBarHeight = (int) (24f * dm.density + 0.5f);
                    }
                } catch (Exception ignored) {
                    // Keep 0; caller will treat as no status bar.
                }
            }
        }

        sBottomBarHeight = h - sStatusBarHeight;
        sDisplayBarHeightsCached = true;
    }

    /** Invalidate display bar heights cache (e.g. after rotation). */
    public static void invalidateDisplayBarHeights() {
        sDisplayBarHeightsCached = false;
    }

    // ----- Focused display (multi-display) -----

    /** Cached focused display id from top task; invalidate on demand. */
    private static int sFocusedDisplayIdCache = DEFAULT_DISPLAY_ID;
    private static boolean sFocusedDisplayIdCached = false;

    /** Display id of the top task (API 24+ via APIAdapter). Used to inject input to the correct display. Falls back to DEFAULT_DISPLAY_ID on API 21–23 or when reflection fails. */
    public static int getFocusedDisplayId() {
        if (sFocusedDisplayIdCached) {
            return sFocusedDisplayIdCache;
        }
        try {
            List<RunningTaskInfo> taskInfo = APIAdapter.getTasks(AndroidDevice.iActivityManager, Integer.MAX_VALUE);
            if (taskInfo != null && !taskInfo.isEmpty()) {
                int displayId = APIAdapter.getTaskDisplayId(taskInfo.get(0));
                if (displayId >= 0) {
                    sFocusedDisplayIdCache = displayId;
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

    /** True if the display accepts input: primary (0) always; secondary only on API 29+. */
    public static boolean supportsInputEvents(int displayId) {
        return displayId == DEFAULT_DISPLAY_ID || Build.VERSION.SDK_INT >= AndroidVersions.API_29_ANDROID_10;
    }

    /** Sets display id on the event for multi-display (API 29+). No-op for display 0. Returns false if SDK < 29 or reflection fails. */
    public static boolean setInputEventDisplayId(InputEvent event, int displayId) {
        if (event == null || displayId == DEFAULT_DISPLAY_ID) {
            return true;
        }
        if (Build.VERSION.SDK_INT < AndroidVersions.API_29_ANDROID_10 || !supportsInputEvents(displayId)) {
            return false;
        }
        return APIAdapter.applyDisplayIdToInputEvent(event, displayId);
    }

    /** True if the display is interactive. API 34+ uses isDisplayInteractive(displayId) via APIAdapter; else isInteractive(). */
    public static boolean isDisplayInteractive(int displayId) {
        IPowerManager pm = getIPowerManager();
        if (pm == null) {
            return false;
        }
        Boolean r = APIAdapter.isDisplayInteractive(pm, displayId);
        if (r != null) {
            return r;
        }
        try {
            return pm.isInteractive();
        } catch (RemoteException e) {
            Logger.warningPrintln("isInteractive() failed: " + e.getMessage());
            return false;
        }
    }

    /** Placeholder: not implemented. Use {@code input keyevent 26} to wake. */
    public static boolean setDisplayPower(int displayId, boolean on) {
        Logger.println("setDisplayPower(displayId=" + displayId + ", on=" + on + ") not implemented; use 'input keyevent 26' to wake.");
        return false;
    }

    /** Returns the top activity component of the focused task, or null if unavailable. */
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

    /**
     * Returns the full activity back stack (all activities in the top task). Kept for scrcpy / full-stack
     * scenarios (e.g. back-stack analysis, reporting). Uses {@link #getFocusedStack()} (dumpsys); format
     * may vary across Android/OEM. For only the top activity use {@link #getTopActivityComponentName()}.
     */
    @SuppressWarnings("unused")
    public static List<ActivityName> getCurrentTaskActivityStack() {
        StackInfo stackInfo = getFocusedStack();
        if (stackInfo != null && !stackInfo.getTasks().isEmpty()) {
            return stackInfo.getTasks().get(0).activityNames;
        }
        return null;
    }

    /** Best-effort: true if IME window height is non-zero. May be false when run as app_process (no IME client). Prefer model isEditText when available. */
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

    /** If the display is not interactive, sends KEYCODE_WAKEUP to wake the device (no toggle); then rechecks and logs. */
    public static void checkInteractive() {
        try {
            if (!isDisplayInteractive(DEFAULT_DISPLAY_ID)) {
                Logger.format("Power Manager says we are NOT interactive");
                int ret = Runtime.getRuntime().exec(new String[]{"input", "keyevent", String.valueOf(KEYCODE_WAKEUP)}).waitFor();
                Logger.format("Wakeup ret code %d %s", ret, (isDisplayInteractive(DEFAULT_DISPLAY_ID) ? "Interactive" : "Not interactive"));
            } else {
                Logger.format("Power Manager says we are interactive");
            }
        } catch (Exception e) {
            e.printStackTrace();
        }
    }

    // ----- Permissions -----

    /**
     * Returns the list of permissions declared (requested) by the package in its manifest.
     * Note: this is not the list of currently granted runtime permissions.
     *
     * @param packageName package to query
     * @return requested permission names, or empty array / null on error
     */
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
                Logger.debugFormat("%s requested permission %s", packageName, s);
            }

            return packageInfo.requestedPermissions;
        } catch (PackageManager.NameNotFoundException e) {
            e.printStackTrace();
        }
        return null;
    }

    /** Grants a single runtime permission via {@code pm grant}. */
    public static boolean grantRuntimePermission(String packageName, String permission) {
        try {
            int ret = executeCommandAndWaitFor(new String[]{"pm", "grant", packageName, permission});
            return ret == 0;
        } catch (Exception e) {
            Logger.warningFormat("Granting saved permission %s to %s results in error %s", permission, packageName, e);
        }
        return false;
    }

    /** Grants multiple runtime permissions via {@code pm grant}; skips permissions that previously failed (blacklist). */
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

    // ----- Package lifecycle (stop / clear) -----

    private static void waitForNotify(Object lock) {
        synchronized (lock) {
            try {
                lock.wait(WAIT_FOR_NOTIFY_MS);
            } catch (InterruptedException e) {
                Thread.currentThread().interrupt();
                e.printStackTrace();
            }
        }
    }

    /**
     * Legacy: gets package size via IPackageManager.getPackageSizeInfo. Only works on API 26 and below;
     * throws UnsupportedOperationException on API 27+. Always returns false; kept for compatibility.
     *
     * @param packageName package to query
     * @return false (result is only logged)
     * @deprecated API 27+; use StorageStatsManager or similar instead.
     */
    @Deprecated
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
            Logger.errorPrintln("Operation of getting package size is not supported above API 26.");
        }
        return false;
    }

    /** Runs a shell command and returns its exit code. */
    /** Returns exit code of the shell command. */
    public static int executeCommandAndWaitFor(String[] cmd) throws InterruptedException, IOException {
        return Runtime.getRuntime().exec(cmd).waitFor();
    }

    /** Returns PIDs of all running processes that belong to the given package. */
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

    /** Returns true if the crashed process name matches the package of any of the allowed app components. */
    public static boolean isAppCrash(String processName, ArrayList<ComponentName> apps) {
        for (ComponentName cn : apps) {
            if (processName.contains(cn.getPackageName())) {
                Logger.println("// crash app's package is " + cn.getPackageName());
                return true;
            }
        }
        return false;
    }

    /** Force-stops the package (when enableStopPackage is true) with retries until no PIDs remain. */
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
                    Thread.sleep(STOP_PACKAGE_RETRY_SLEEP_MS);
                } catch (InterruptedException e) {
                    Thread.currentThread().interrupt();
                    e.printStackTrace();
                }
            }
        }
        return false;
    }

    /** Clears application data via {@code pm clear} when clearPackage config is true. */
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

    /** Clears package data; if grantAllPermission is true, re-grants the given permissions after clear. */
    public static boolean clearPackage(String packageName, String[] savedPermissions) {
        return clearPackage(packageName) && grantAllPermission && grantRuntimePermissions(packageName, savedPermissions, "clearing package");
    }

    /** Returns true if the package is one of the registered IME packages (see inputMethodPackages). */
    public static boolean isInputMethod(String packageName) {
        return inputMethodPackages.contains(packageName);
    }

    /** Switches to the last used IME; used e.g. after closing a custom IME. */
    /** Switches to the last used IME. Kept for switching back to previous input method (e.g. after closing custom IME). */
    @SuppressWarnings("unused")
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

    // ----- Activity / intent helpers -----

    /** Returns true if the given top activity class name is a home/launcher activity. */
    public static boolean isAtPhoneLauncher(String topActivity) {
        if (topActivity == null) return false;
        Intent intent = new Intent(Intent.ACTION_MAIN);
        intent.addCategory(Intent.CATEGORY_HOME);
        List<ResolveInfo> homeApps = APIAdapter.queryIntentActivities(packageManager, intent);
        if (homeApps == null) return false;
        for (int a = 0; a < homeApps.size(); a++) {
            ResolveInfo r = homeApps.get(a);
            String activity = r.activityInfo.name;
            if (activity != null && activity.startsWith(".")) {
                activity = r.activityInfo.packageName + activity;
            }
            //Logger.println("// the top activity is " + topActivity + ", phone launcher activity is " + activity);
            if (topActivity.equals(activity)) return true;
        }
        return false;
    }

    /** Returns true if the given top activity is a camera capture activity. */
    public static boolean isAtPhoneCapture(String topActivity) {
        if (topActivity == null) return false;
        Intent intent = new Intent(MediaStore.ACTION_IMAGE_CAPTURE);
        List<ResolveInfo> list = APIAdapter.queryIntentActivities(packageManager, intent);
        if (list == null) return false;
        for (int a = 0; a < list.size(); a++) {
            ResolveInfo r = list.get(a);
            String activity = r.activityInfo.name;
            if (activity != null && activity.startsWith(".")) {
                activity = r.activityInfo.packageName + activity;
            }
            //Logger.println("// the top activity is " + topActivity + ", phone capture activity is " + activity);
            if (topActivity.equals(activity)) return true;
        }
        return false;
    }

    /** Returns true if the given top activity is the main/launcher activity of the given package. */
    public static boolean isAtAppMain(String topActivityClassName, String topActivityPackageName) {
        if (topActivityClassName == null || topActivityPackageName == null) return false;
        Intent intent = new Intent(Intent.ACTION_MAIN);
        List<ResolveInfo> list = APIAdapter.queryIntentActivities(packageManager, intent);
        if (list == null) return false;
        for (int a = 0; a < list.size(); a++) {
            ResolveInfo r = list.get(a);
            String packageName = r.activityInfo.applicationInfo.packageName;
            String activity = r.activityInfo.name;
            if (activity != null && activity.startsWith(".")) {
                activity = packageName + activity;
            }
            if (topActivityClassName.equals(activity) && packageName.equals(topActivityPackageName)) return true;
        }
        return false;
    }


    /** Returns true if IActivityManager.startActivity succeeded (non-null result). */
    private static boolean tryStartActivityViaAm(Intent intent) {
        try {
            return APIAdapter.startActivity(iActivityManager, intent) != null;
        } catch (Exception e) {
            Logger.println("Start Activity error: " + e);
            return false;
        }
    }

    /** Starts an activity via IActivityManager or falls back to {@code am start -n}. Returns 1 on success, 0 on failure. */
    public static int startActivity(Intent intent) {
        if (tryStartActivityViaAm(intent)) return 1;
        if (intent != null && intent.getComponent() != null) {
            try {
                Logger.println("IActivityManager.startActivity failed, execute am start activity");
                int ret = executeCommandAndWaitFor(new String[]{"am", "start", "-n", intent.getComponent().flattenToShortString()});
                return ret == 0 ? 1 : 0;
            } catch (Exception e) {
                Logger.println("am start activity failed: " + e.getMessage());
                return 0;
            }
        }
        return 0;
    }

    /** Starts an activity by URI via IActivityManager or falls back to {@code am start -d}. Returns 1 on success, 0 on failure. */
    public static int startUri(Intent intent) {
        if (tryStartActivityViaAm(intent)) return 1;
        if (intent != null && intent.getData() != null) {
            try {
                Logger.println("IActivityManager.startActivity failed, execute am start uri");
                int ret = executeCommandAndWaitFor(new String[]{"am", "start", "-d", intent.getData().toString()});
                return ret == 0 ? 1 : 0;
            } catch (Exception e) {
                Logger.println("am start uri failed: " + e.getMessage());
                return 0;
            }
        }
        return 0;
    }


    // ----- Text input (clipboard + key injection) -----

    /**
     * Sends text by writing to clipboard and injecting Ctrl+V. Supports ~CLEAR~ (clear field) and \\n (newline).
     */
    public static boolean sendText(String text) {
        if (text == null) {
            return false;
        }

        // Special flags handling (YADB-style extensions).
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

        // Use clipboard + Ctrl+V path.
        return sendTextViaClipboard(text);
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
        long now = android.os.SystemClock.uptimeMillis();
        KeyEvent event = new KeyEvent(now, now, action, keyCode, 0, metaState);
        setInputEventDisplayId(event, displayId);
        return APIAdapter.injectInputEvent(event, 0);
    }


    /**
     * Parses "dumpsys activity a" to build the current activity stack for the focused display.
     * Hierarchy: Display -> Stack -> Task -> Activity (Hist). Focus lines (mFocusedStack etc.) set
     * which stack is focused per display; we return the first display that has a matching stack
     * (prefer primary display when multiple match). Output format may vary across Android/OEM.
     *
     * @return StackInfo for the focused stack, or null on parse failure or IO error
     */
    public static StackInfo getFocusedStack() {
        String[] cmd = new String[]{"dumpsys", "activity", "a"};

        try {
            String output = ProcessUtils.getProcessOutput(cmd);
            String line;
            Display currentDisplay = null;
            StackInfo currentStackInfo = null;
            Task currentTask = null;
            List<Display> displays = new ArrayList<>();
            BufferedReader br = new BufferedReader(new StringReader(output));
            while ((line = br.readLine()) != null) {
                Matcher m = DISPLAY_PATTERN.matcher(line);
                if (m.matches()) {
                    currentDisplay = new Display(Integer.parseInt(m.group(1)));
                    displays.add(currentDisplay);
                    currentStackInfo = null;
                    currentTask = null;
                    continue;
                }
                m = STACK_PATTERN.matcher(line);
                if (m.matches() && currentDisplay != null) {
                    currentStackInfo = new StackInfo(Integer.parseInt(m.group(1)));
                    currentDisplay.stackInfos.add(currentStackInfo);
                    currentTask = null;
                    continue;
                }
                m = TASK_PATTERN.matcher(line);
                if (m.matches() && currentStackInfo != null) {
                    currentTask = new Task(Integer.parseInt(m.group(1)));
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
                    currentTask.activityNames.add(new ActivityName(new ComponentName(packageName, className)));
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
            // Prefer primary display (0) when multiple displays have a matching focused stack.
            for (Display d : displays) {
                if (d.id != DEFAULT_DISPLAY_ID) continue;
                for (StackInfo s : d.stackInfos) {
                    if (s.id == d.focusedStackId) return s;
                }
            }
            for (Display d : displays) {
                if (d.id == DEFAULT_DISPLAY_ID) continue;
                for (StackInfo s : d.stackInfos) {
                    if (s.id == d.focusedStackId) return s;
                }
            }
        } catch (IOException ignore) {
        } catch (InterruptedException ignore) {
        }
        return null;
    }


    // ----- Dumpsys stack / display model (inner types below) -----

    /** One display from dumpsys; holds stacks and the focused stack id. */
    public static class Display {
        int id;
        int focusedStackId;
        List<StackInfo> stackInfos = new ArrayList<>();

        public Display(int id) {
            this.id = id;
        }
    }

    /** One stack on a display; holds tasks (list of activities). */
    public static class StackInfo {
        int id;
        List<Task> tasks = new ArrayList<>();

        public StackInfo(int id) {
            this.id = id;
        }

        public List<Task> getTasks() {
            return tasks;
        }

        /** Logs stack and task structure for debugging. */
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

    /** One task in a stack; holds the list of activities in the back stack. */
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

    /** Wrapper for a single activity component in the task. */
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



