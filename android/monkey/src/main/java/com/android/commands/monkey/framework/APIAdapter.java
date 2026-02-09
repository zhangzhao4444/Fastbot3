/*
 * Copyright 2020 Advanced Software Technologies Lab at ETH Zurich, Switzerland
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

import android.app.ActivityManager;
import android.app.ActivityManager.RunningTaskInfo;
import android.app.ActivityManagerNative;
import android.content.Context;
import android.app.IActivityManager;
import android.app.IApplicationThread;
import android.app.ProfilerInfo;
import android.content.IIntentReceiver;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.pm.IPackageManager;
import android.content.pm.PackageManager;
import android.content.pm.PermissionInfo;
import android.content.pm.ResolveInfo;
import android.os.Bundle;
import android.os.IBinder;
import android.os.IPowerManager;
import android.os.Looper;
import android.view.InputEvent;
import android.view.IWindowManager;
import android.view.inputmethod.InputMethodInfo;

import com.android.commands.monkey.utils.Logger;
import com.android.internal.view.IInputMethodManager;

import java.lang.reflect.Field;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.util.Collections;
import java.util.List;

/**
 * Reflection adapter for Android framework APIs. Compatible with Android 5–16 and vendor ROMs.
 * Uses multi-signature probing (sigs + loop try) and method caching; fatal paths call System.exit(1).
 *
 * @author Zhao Zhang, Tianxiao Gu
 */
public final class APIAdapter {

    /** Caller tag passed to freezeDisplayRotation/thawDisplayRotation on API 33+. */
    public static final String ROTATION_CALLER = "com.bytedance.fastbot";

    /** Hidden class for reading system properties (e.g. ro.serialno). */
    private static final String SYSTEM_PROPERTIES_CLASS = "android.os.SystemProperties";
    private static final String PROP_SERIAL = "ro.serialno";

    /** Hidden class: main thread for the app process; provides system context (API 1+). */
    private static final String ACTIVITY_THREAD_CLASS = "android.app.ActivityThread";

    private APIAdapter() {
    }

    // ----- Reflection helpers -----

    /**
     * Resolves a public method by name and parameter types. Returns null if not found.
     * SecurityException causes exit(1) to match original behaviour.
     */
    private static Method getMethod(Class<?> clazz, String name, Class<?>[] paramTypes) {
        try {
            Method m = clazz.getMethod(name, paramTypes);
            m.setAccessible(true);
            return m;
        } catch (NoSuchMethodException | NoSuchMethodError e) {
            return null;
        } catch (SecurityException e) {
            e.printStackTrace();
            System.exit(1);
            return null;
        }
    }

    /**
     * Resolves a declared method (including non-public / hidden). Returns null if not found.
     * Use for ActivityThread and other @hide APIs. SecurityException causes exit(1).
     */
    private static Method getDeclaredMethod(Class<?> clazz, String name, Class<?>[] paramTypes) {
        try {
            Method m = clazz.getDeclaredMethod(name, paramTypes);
            m.setAccessible(true);
            return m;
        } catch (NoSuchMethodException | NoSuchMethodError e) {
            return null;
        } catch (SecurityException e) {
            e.printStackTrace();
            System.exit(1);
            return null;
        }
    }

    /** Invokes method; on any exception prints stack trace and calls System.exit(1). Use for critical paths. */
    private static Object invokeFatal(Method method, Object receiver, Object... args) {
        try {
            return method.invoke(receiver, args);
        } catch (IllegalAccessException | IllegalArgumentException | InvocationTargetException e) {
            e.printStackTrace();
            System.exit(1);
            return null;
        }
    }

    /** Invokes method; on exception returns null (no exit). Use for optional or fallback paths. */
    private static Object invokeQuiet(Method method, Object receiver, Object... args) {
        try {
            return method.invoke(receiver, args);
        } catch (Throwable e) {
            return null;
        }
    }

    // ----- ActivityThread: system context (Android 5–16 compatible) -----
    // Path 1: systemMain() (static, no-arg) then getSystemContext() on result — for shell/system process.
    // Path 2: currentActivityThread() (static, no-arg) then getSystemContext() — fallback when process already has ActivityThread.

    private static Context sSystemContext = null;

    /**
     * Returns the system (global) Context via ActivityThread. Compatible with Android 5–16 and vendor ROMs.
     * Tries systemMain() + getSystemContext() first, then currentActivityThread() + getSystemContext().
     * Callers must use this for PackageManager, Resources, etc. when running as shell (e.g. monkey).
     */
    public static synchronized Context getSystemContext() {
        if (sSystemContext != null) {
            return sSystemContext;
        }
        try {
            Looper.prepareMainLooper();
            Class<?> atClass = Class.forName(ACTIVITY_THREAD_CLASS);
            Method getSystemContextMethod = getDeclaredMethod(atClass, "getSystemContext", new Class<?>[0]);
            if (getSystemContextMethod == null) {
                Logger.warningPrintln("APIAdapter: ActivityThread.getSystemContext not found");
                return null;
            }
            // Path 1: systemMain() — standard for system/shell process (API 1+).
            Method systemMainMethod = getDeclaredMethod(atClass, "systemMain", new Class<?>[0]);
            if (systemMainMethod != null) {
                Object activityThread = systemMainMethod.invoke(null);
                if (activityThread != null) {
                    Object ctx = getSystemContextMethod.invoke(activityThread);
                    if (ctx instanceof Context) {
                        sSystemContext = (Context) ctx;
                        return sSystemContext;
                    }
                }
            }
            // Path 2: currentActivityThread() — fallback when process already has an ActivityThread (e.g. app process).
            Method currentMethod = getDeclaredMethod(atClass, "currentActivityThread", new Class<?>[0]);
            if (currentMethod != null) {
                Object activityThread = currentMethod.invoke(null);
                if (activityThread != null) {
                    Object ctx = getSystemContextMethod.invoke(activityThread);
                    if (ctx instanceof Context) {
                        sSystemContext = (Context) ctx;
                        return sSystemContext;
                    }
                }
            }
            Logger.warningPrintln("APIAdapter: getSystemContext failed (both systemMain and currentActivityThread paths)");
        } catch (Exception e) {
            Logger.warningPrintln("APIAdapter: getSystemContext failed: " + e.getMessage());
        }
        return null;
    }

    // ----- Cached Method and variant (param count or version) per API -----
    // Resolved once per process; subsequent calls reuse the same Method and dispatch by stored param count or version.

    private static Method getActivityManagerMethod = null;

    private static Method getTasksMethod = null;
    private static int getTasksParamCount = -1;
    private static boolean getTasksUseFallback = false;

    private static Method setActivityControllerMethod = null;
    private static int setActivityControllerParamCount = 0;

    private static Method registerReceiverMethod = null;
    private static int registerReceiverParamCount = 0;

    private static Method broadcastIntentMethod = null;
    private static int broadcastIntentParamCount = 0;

    private static Method startActivityMethod = null;
    private static int startActivityParamCount = 0;

    private static Method getPermissionInfoMethod = null;
    private static int getPermissionInfoParamCount = 0;

    private static Method freezeDisplayRotationMethod = null;
    private static int freezeDisplayRotationVersion = -1;

    private static Method thawDisplayRotationMethod = null;
    private static int thawDisplayRotationVersion = -1;

    private static Method getEnabledInputMethodListMethod = null;
    private static int getEnabledInputMethodListParamCount = 0;

    private static Method setInputMethodMethod = null;

    // ----- IWindowManager: freeze / thaw display rotation -----
    // Probe order: newest first. Version 3 = (displayId, rotation, caller) API 33+, 2 = (displayId, rotation) API 29–32, 1 = freezeRotation(rotation) API 18–28.

    public static void freezeDisplayRotation(IWindowManager iwm, int displayId, int rotationConstant, String caller) throws Exception {
        synchronized (APIAdapter.class) {
            if (freezeDisplayRotationVersion < 0) {
                Class<?> c = iwm.getClass();
                Class<?>[][] sigs = new Class<?>[][]{
                        {int.class, int.class, String.class},  // Android 13+ (API 33): displayId, rotation, caller
                        {int.class, int.class},                 // Android 10–12 (API 29–32): displayId, rotation
                };
                for (int i = 0; i < sigs.length; i++) {
                    Method m = getMethod(c, "freezeDisplayRotation", sigs[i]);
                    if (m != null) {
                        freezeDisplayRotationMethod = m;
                        freezeDisplayRotationVersion = (sigs[i].length == 3) ? 3 : 2;
                        break;
                    }
                }
                if (freezeDisplayRotationMethod == null) {
                    freezeDisplayRotationVersion = 1;  // API 18–28: fallback to IWindowManager.freezeRotation(rotation)
                }
            }
        }
        if (freezeDisplayRotationVersion == 1) {
            iwm.freezeRotation(rotationConstant);
            return;
        }
        if (freezeDisplayRotationVersion == 3) {
            freezeDisplayRotationMethod.invoke(iwm, displayId, rotationConstant, caller);
        } else {
            freezeDisplayRotationMethod.invoke(iwm, displayId, rotationConstant);
        }
    }

    /** Version 2 = (displayId, caller) API 33+, 1 = (displayId) API 29–32, 0 = thawRotation() API 18–28. */
    public static void thawDisplayRotation(IWindowManager iwm, int displayId, String caller) throws Exception {
        synchronized (APIAdapter.class) {
            if (thawDisplayRotationVersion < 0) {
                Class<?> c = iwm.getClass();
                Class<?>[][] sigs = new Class<?>[][]{
                        {int.class, String.class},  // Android 13+ (API 33): displayId, caller
                        {int.class},                 // Android 10–12 (API 29–32): displayId
                };
                for (int i = 0; i < sigs.length; i++) {
                    Method m = getMethod(c, "thawDisplayRotation", sigs[i]);
                    if (m != null) {
                        thawDisplayRotationMethod = m;
                        thawDisplayRotationVersion = sigs[i].length == 2 ? 2 : 1;
                        break;
                    }
                }
                if (thawDisplayRotationMethod == null) {
                    thawDisplayRotationVersion = 0;  // API 18–28: fallback to IWindowManager.thawRotation()
                }
            }
        }
        if (thawDisplayRotationVersion == 0) {
            iwm.thawRotation();
            return;
        }
        if (thawDisplayRotationVersion == 2) {
            thawDisplayRotationMethod.invoke(iwm, displayId, caller);
        } else {
            thawDisplayRotationMethod.invoke(iwm, displayId);
        }
    }

    // ----- IPackageManager: getPermissionInfo -----
    // 2-param (name, flags) API 23 and below; 3-param (name, callingPackage, flags) API 24+. Use "shell" for callingPackage.

    public static PermissionInfo getPermissionInfo(IPackageManager ipm, String perm, int flags) {
        if (getPermissionInfoMethod == null) {
            Class<?> c = ipm.getClass();
            Class<?>[][] sigs = new Class<?>[][]{
                    {String.class, int.class},                    // API 23 and below
                    {String.class, String.class, int.class},      // API 24+ (callingPackage required)
            };
            for (Class<?>[] sig : sigs) {
                Method m = getMethod(c, "getPermissionInfo", sig);
                if (m != null) {
                    getPermissionInfoMethod = m;
                    getPermissionInfoParamCount = sig.length;
                    break;
                }
            }
            if (getPermissionInfoMethod == null) {
                Logger.println("Cannot resolve method: getPermissionInfo");
                System.exit(1);
            }
        }
        if (getPermissionInfoParamCount == 2) {
            return (PermissionInfo) invokeFatal(getPermissionInfoMethod, ipm, perm, flags);
        }
        return (PermissionInfo) invokeFatal(getPermissionInfoMethod, ipm, perm, "shell", flags);
    }

    // ----- IActivityManager: registerReceiver -----
    // 6-param API 21–25; 7-param (..., userId, boolean) API 26+; 7-param (..., userId, int flags) API 30+. Build args by param count and last type.

    public static void registerReceiver(IActivityManager am, IIntentReceiver receiver, IntentFilter filter, int userId) {
        if (registerReceiverMethod == null) {
            Class<?> c = IActivityManager.class;
            Class<?>[][] sigs = new Class<?>[][]{
                    {IApplicationThread.class, String.class, IIntentReceiver.class, IntentFilter.class, String.class, int.class},                      // API 21–25
                    {IApplicationThread.class, String.class, IIntentReceiver.class, IntentFilter.class, String.class, int.class, boolean.class},       // API 26–29
                    {IApplicationThread.class, String.class, IIntentReceiver.class, IntentFilter.class, String.class, int.class, int.class},           // API 30+
            };
            for (Class<?>[] sig : sigs) {
                Method m = getMethod(c, "registerReceiver", sig);
                if (m != null) {
                    registerReceiverMethod = m;
                    registerReceiverParamCount = sig.length;
                    break;
                }
            }
            if (registerReceiverMethod == null) {
                Logger.println("Cannot resolve method: registerReceiver");
                System.exit(1);
            }
        }
        Object[] args = registerReceiverParamCount == 6
                ? new Object[]{null, null, receiver, filter, null, userId}
                : (registerReceiverParamCount == 7 && registerReceiverMethod.getParameterTypes()[6] == boolean.class)
                ? new Object[]{null, null, receiver, filter, null, userId, false}
                : new Object[]{null, null, receiver, filter, null, userId, 0};
        invokeFatal(registerReceiverMethod, am, args);
    }

    // ----- IActivityManager: getActivityManager -----
    // ActivityManagerNative.getDefault() API 21–25 (Lollipop–Marshmallow); ActivityManager.getService() API 26+ (Oreo+). Both no-arg static.

    public static IActivityManager getActivityManager() {
        if (getActivityManagerMethod == null) {
            Method m = getMethod(ActivityManagerNative.class, "getDefault", new Class<?>[0]);
            if (m == null) {
                m = getMethod(ActivityManager.class, "getService", new Class<?>[0]);
            }
            if (m == null) {
                Logger.println("Cannot getActivityManager");
                System.exit(1);
            }
            getActivityManagerMethod = m;
        }
        try {
            return (IActivityManager) getActivityManagerMethod.invoke(null);
        } catch (IllegalAccessException | InvocationTargetException e) {
            e.printStackTrace();
            Logger.println("getActivityManager invoke failed: " + e.getMessage());
            System.exit(1);
            return null;
        }
    }

    // ----- PackageManager (no reflection) -----

    public static List<ResolveInfo> queryIntentActivities(PackageManager mPm, Intent intent) {
        return mPm.queryIntentActivities(intent, 0);
    }

    // ----- IActivityManager: getTasks -----
    // getTasks(maxNum, flags) API 26–28 (Oreo–Pi); getTasks(maxNum) API 29+ (Android 10+). Fallback: ActivityManager.getRunningTasks(maxNum).

    @SuppressWarnings("unchecked")
    public static List<RunningTaskInfo> getTasks(IActivityManager iAm, int maxNum) {
        if (getTasksUseFallback) {
            return getTasksViaActivityManager(maxNum);
        }
        if (getTasksMethod == null) {
            Class<?> c = IActivityManager.class;
            Class<?>[][] sigs = new Class<?>[][]{
                    {int.class, int.class},  // API 26–28: maxNum, flags
                    {int.class},             // API 29+: maxNum only
            };
            for (Class<?>[] sig : sigs) {
                Method m = getMethod(c, "getTasks", sig);
                if (m != null) {
                    getTasksMethod = m;
                    getTasksParamCount = sig.length;
                    break;
                }
            }
            if (getTasksMethod == null) {
                Logger.warningPrintln("IActivityManager.getTasks not available, using ActivityManager.getRunningTasks fallback");
                getTasksUseFallback = true;
                return getTasksViaActivityManager(maxNum);
            }
        }
        if (getTasksParamCount == 2) {
            return (List<RunningTaskInfo>) invokeQuiet(getTasksMethod, iAm, maxNum, 0);
        }
        return (List<RunningTaskInfo>) invokeQuiet(getTasksMethod, iAm, maxNum);
    }

    /** Fallback when IActivityManager.getTasks is unavailable (e.g. hidden API or vendor ROM). On API 31+ may return only caller's tasks. */
    private static List<RunningTaskInfo> getTasksViaActivityManager(int maxNum) {
        try {
            Context ctx = getSystemContext();
            if (ctx == null) return Collections.emptyList();
            ActivityManager am = (ActivityManager) ctx.getSystemService(Context.ACTIVITY_SERVICE);
            if (am == null) return Collections.emptyList();
            List<ActivityManager.RunningTaskInfo> list = am.getRunningTasks(maxNum);
            return list != null ? list : Collections.<RunningTaskInfo>emptyList();
        } catch (Exception e) {
            Logger.warningPrintln("getTasksViaActivityManager failed: " + e.getMessage());
            return Collections.emptyList();
        }
    }

    // ----- RunningTaskInfo: displayId (Android 5–16) -----
    // displayId exists on RunningTaskInfo/TaskInfo from API 24+; walk class and superclasses for OEM/vendor.

    /**
     * Reads the displayId field from a RunningTaskInfo via reflection. Compatible with Android 5–16:
     * API 24+ has displayId on RunningTaskInfo or superclass TaskInfo; API 21–23 may not have it (returns -1).
     *
     * @param task the top task (e.g. from getTasks)
     * @return display id if present and >= 0, otherwise -1
     */
    public static int getTaskDisplayId(RunningTaskInfo task) {
        if (task == null) {
            return -1;
        }
        for (Class<?> c = task.getClass(); c != null; c = c.getSuperclass()) {
            try {
                Field field = c.getDeclaredField("displayId");
                field.setAccessible(true);
                int displayId = field.getInt(task);
                return displayId >= 0 ? displayId : -1;
            } catch (NoSuchFieldException e) {
                // try superclass (e.g. TaskInfo on some ROMs)
            } catch (Exception e) {
                Logger.warningPrintln("APIAdapter: getTaskDisplayId failed: " + e.getMessage());
                return -1;
            }
        }
        return -1;
    }

    // ----- IPowerManager: isDisplayInteractive (API 34+) -----
    // Per-display isDisplayInteractive(int) from Android 14; API 21–33 only have isInteractive() (no displayId).

    /**
     * Calls IPowerManager.isDisplayInteractive(displayId) via reflection if available. Compatible with Android 5–16:
     * API 34+ may have isDisplayInteractive(int); API 21–33 do not (returns null so caller can use isInteractive()).
     *
     * @param pm        IPowerManager service (must not be null)
     * @param displayId display to query
     * @return true/false if the call succeeded, or null if method not found or invocation failed (use isInteractive() as fallback)
     */
    public static Boolean isDisplayInteractive(IPowerManager pm, int displayId) {
        if (pm == null) {
            return null;
        }
        try {
            Method m = getMethod(pm.getClass(), "isDisplayInteractive", new Class<?>[]{int.class});
            if (m == null) {
                return null;
            }
            Object r = m.invoke(pm, displayId);
            return Boolean.TRUE.equals(r);
        } catch (Exception e) {
            Logger.warningPrintln("APIAdapter: isDisplayInteractive(displayId) failed: " + e.getMessage());
            return null;
        }
    }

    // ----- InputManager: injectInputEvent (API 16+) -----
    // Hidden API: InputManager.getInstance().injectInputEvent(InputEvent, int mode). Supported Android 5–16.

    /**
     * Injects an input event via InputManager. Compatible with Android 5–16 (hidden API since API 16).
     * Caller should set display id on the event when needed (e.g. APIAdapter.applyDisplayIdToInputEvent).
     *
     * @param event the event to inject (KeyEvent, MotionEvent, etc.)
     * @param mode  injection mode (0 = default)
     * @return true if injection succeeded or return value was not Boolean, false on failure
     */
    public static boolean injectInputEvent(InputEvent event, int mode) {
        if (event == null) return false;
        try {
            Class<?> imClass = Class.forName("android.hardware.input.InputManager");
            Method getInstance = getMethod(imClass, "getInstance", new Class<?>[0]);
            if (getInstance == null) return false;
            Object im = getInstance.invoke(null);
            if (im == null) return false;
            Method inject = getMethod(imClass, "injectInputEvent", new Class<?>[]{InputEvent.class, int.class});
            if (inject == null) return false;
            Object result = inject.invoke(im, event, mode);
            return !(result instanceof Boolean) || Boolean.TRUE.equals(result);
        } catch (Exception e) {
            Logger.warningPrintln("APIAdapter: injectInputEvent failed: " + e.getMessage());
            return false;
        }
    }

    // ----- InputEvent: setDisplayId (API 29+) -----
    // InputEvent.setDisplayId(int) exists from Android 10 for multi-display input routing; API 21–28 have no such method.

    /**
     * Sets the display id on an InputEvent via reflection. Compatible with Android 5–16:
     * API 29+ has setDisplayId(int); API 21–28 do not (returns false).
     *
     * @param inputEvent KeyEvent, MotionEvent, etc. (must not be null)
     * @param displayId  target display id
     * @return true if the call succeeded, false if method not found (e.g. API &lt; 29) or invocation failed
     */
    public static boolean applyDisplayIdToInputEvent(Object inputEvent, int displayId) {
        if (inputEvent == null) {
            return false;
        }
        try {
            Method m = getMethod(inputEvent.getClass(), "setDisplayId", new Class<?>[]{int.class});
            if (m == null) {
                return false;
            }
            m.invoke(inputEvent, displayId);
            return true;
        } catch (Exception e) {
            Logger.warningPrintln("APIAdapter: applyDisplayIdToInputEvent failed: " + e.getMessage());
            return false;
        }
    }

    // ----- IActivityManager: setActivityController -----
    // (watcher, imAMonkey) API 26+ (Oreo+); (watcher) API 21–25. Pass true for imAMonkey when 2-param.

    public static void setActivityController(IActivityManager mAm, Object controller) {
        if (setActivityControllerMethod == null) {
            Class<?> c = IActivityManager.class;
            Class<?>[][] sigs = new Class<?>[][]{
                    {android.app.IActivityController.class, boolean.class},  // API 26+
                    {android.app.IActivityController.class},                 // API 21–25
            };
            for (Class<?>[] sig : sigs) {
                Method m = getMethod(c, "setActivityController", sig);
                if (m != null) {
                    setActivityControllerMethod = m;
                    setActivityControllerParamCount = sig.length;
                    break;
                }
            }
            if (setActivityControllerMethod == null) {
                Logger.println("Cannot resolve method: setActivityController");
                System.exit(1);
            }
        }
        if (setActivityControllerParamCount == 2) {
            invokeFatal(setActivityControllerMethod, mAm, controller, true);
        } else {
            invokeFatal(setActivityControllerMethod, mAm, controller);
        }
    }

    // ----- IActivityManager: broadcastIntent (deprecated) -----
    // 15-param (requiredPermissions String[], options Bundle) API 26+; 13-param (requiredPermission String) API 21–25. Unused; kept for compatibility.

    /** @deprecated Unused; kept for compatibility. */
    @Deprecated
    public static void broadcastIntent(IActivityManager mAm, Intent paramIntent) {
        if (broadcastIntentMethod == null) {
            Class<?> c = IActivityManager.class;
            Class<?>[][] sigs = new Class<?>[][]{
                    {IApplicationThread.class, Intent.class, String.class, IIntentReceiver.class,
                            int.class, String.class, Bundle.class, String[].class, int.class, Bundle.class,
                            boolean.class, boolean.class, int.class},   // API 26+
                    {IApplicationThread.class, Intent.class, String.class, IIntentReceiver.class,
                            int.class, String.class, Bundle.class, String.class, int.class,
                            boolean.class, boolean.class, int.class},   // API 21–25
            };
            for (Class<?>[] sig : sigs) {
                Method m = getMethod(c, "broadcastIntent", sig);
                if (m != null) {
                    broadcastIntentMethod = m;
                    broadcastIntentParamCount = sig.length;
                    break;
                }
            }
            if (broadcastIntentMethod == null) {
                Logger.println("Cannot resolve method: broadcastIntent");
                System.exit(1);
            }
        }
        if (broadcastIntentParamCount == 15) {
            invokeFatal(broadcastIntentMethod, mAm, null, paramIntent, null, null, 0, null, null, null, 0, null, false, false, 0);
        } else {
            invokeFatal(broadcastIntentMethod, mAm, null, paramIntent, null, null, 0, null, null, null, 0, false, false, 0);
        }
    }

    // ----- IActivityManager: startActivity -----
    // startActivity(10 params) API 21–25; startActivityWithFeature(11 params, adds callingFeatureId) API 26+. Returns null if neither found.

    public static Object startActivity(IActivityManager mAm, Intent paramIntent) {
        if (startActivityMethod == null) {
            Class<?> c = IActivityManager.class;
            Method m = getMethod(c, "startActivity",   // API 21–25
                    new Class<?>[]{IApplicationThread.class, String.class, Intent.class, String.class, IBinder.class,
                            String.class, int.class, int.class, ProfilerInfo.class, Bundle.class});
            if (m == null) {
                m = getMethod(c, "startActivityWithFeature",  // API 26+
                        new Class<?>[]{IApplicationThread.class, String.class, String.class, Intent.class, String.class, IBinder.class,
                                String.class, int.class, int.class, ProfilerInfo.class, Bundle.class});
            }
            if (m == null) {
                Logger.warningPrintln("APIAdapter: Cannot resolve startActivity/startActivityWithFeature, startActivity will return null.");
            } else {
                startActivityMethod = m;
                startActivityParamCount = m.getParameterTypes().length;
            }
        }
        if (startActivityMethod == null) return null;
        if (startActivityParamCount == 11) {
            return invokeQuiet(startActivityMethod, mAm, null, null, null, paramIntent, null, null, null, 0, 0, null, null);
        }
        return invokeQuiet(startActivityMethod, mAm, null, null, paramIntent, null, null, null, 0, 0, null, null);
    }

    // ----- IInputMethodManager (deprecated) -----
    // getEnabledInputMethodList(userId) API 24+; getEnabledInputMethodList() API 21–23. setInputMethod(IBinder, String) unchanged. Unused; kept for compatibility.

    /** @deprecated Unused; kept for compatibility. */
    @Deprecated
    @SuppressWarnings("unchecked")
    public static List<InputMethodInfo> getEnabledInputMethodList(IInputMethodManager iIMM) {
        if (getEnabledInputMethodListMethod == null) {
            Class<?> c = iIMM.getClass();
            Class<?>[][] sigs = new Class<?>[][]{
                    {int.class},  // API 24+: userId
                    {},           // API 21–23: no-arg
            };
            for (Class<?>[] sig : sigs) {
                Method m = getMethod(c, "getEnabledInputMethodList", sig);
                if (m != null) {
                    getEnabledInputMethodListMethod = m;
                    getEnabledInputMethodListParamCount = sig.length;
                    break;
                }
            }
            if (getEnabledInputMethodListMethod == null) {
                Logger.println("Cannot resolve method: getEnabledInputMethodList");
                System.exit(1);
            }
        }
        if (getEnabledInputMethodListParamCount == 1) {
            return (List<InputMethodInfo>) invokeFatal(getEnabledInputMethodListMethod, iIMM, 0);
        }
        return (List<InputMethodInfo>) invokeFatal(getEnabledInputMethodListMethod, iIMM);
    }

    /** @deprecated Unused; kept for compatibility. */
    @Deprecated
    public static boolean setInputMethod(IInputMethodManager iIMM, String ime) {
        if (setInputMethodMethod == null) {
            Method m = getMethod(iIMM.getClass(), "setInputMethod", new Class<?>[]{IBinder.class, String.class});
            if (m != null) setInputMethodMethod = m;
            else Logger.warningPrintln("APIAdapter: Cannot resolve method setInputMethod, setInputMethod will return false.");
        }
        if (setInputMethodMethod != null && invokeQuiet(setInputMethodMethod, iIMM, null, ime) != null) {
            return true;
        }
        return false;
    }

    // ----- SystemProperties: getSerial -----
    // get(key) API 21+; get(key, def) on some ROMs. If empty, try getString(key, def) on vendor ROMs. Class is @hide.

    public static String getSerial() {
        String serial = "unknown";
        try {
            Class<?> sp = Class.forName(SYSTEM_PROPERTIES_CLASS);
            Class<?>[][] sigs = new Class<?>[][]{
                    {String.class},              // get(key) — API 21+
                    {String.class, String.class}, // get(key, def) — some ROMs
            };
            Object[][] args = new Object[][]{
                    {PROP_SERIAL},
                    {PROP_SERIAL, "unknown"},
            };
            for (int i = 0; i < sigs.length; i++) {
                Method m = getMethod(sp, "get", sigs[i]);
                if (m != null) {
                    String r = (String) m.invoke(null, args[i]);
                    if (r != null && !r.isEmpty()) {
                        serial = r;
                        break;
                    }
                }
            }
            if (serial.equals("unknown")) {
                try {
                    Method m = sp.getDeclaredMethod("getString", String.class, String.class);  // optional: vendor ROMs
                    m.setAccessible(true);
                    String r = (String) m.invoke(null, PROP_SERIAL, "unknown");
                    if (r != null && !r.isEmpty()) serial = r;
                } catch (Exception ignored) {
                }
            }
        } catch (ClassNotFoundException e) {
            Logger.warningPrintln("APIAdapter: SystemProperties not found, serial will be 'unknown'");
        } catch (Exception e) {
            Logger.warningPrintln("APIAdapter: Failed to get serial: " + e.getMessage());
        }
        Logger.println("// device serial number is " + serial);
        return serial;
    }
}
