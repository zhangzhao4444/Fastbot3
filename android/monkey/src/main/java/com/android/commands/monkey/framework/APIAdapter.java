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
import android.view.IWindowManager;
import android.view.inputmethod.InputMethodInfo;

import com.android.commands.monkey.utils.ContextUtils;
import com.android.commands.monkey.utils.Logger;
import com.android.internal.view.IInputMethodManager;

import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.util.Collections;
import java.util.List;

/**
 * @author Zhao Zhang, Tianxiao Gu
 */

/**
 * Reflection utilities for Android framework APIs.
 * 
 * <p>This class provides compatibility across Android 5-16 and various vendor ROMs by:
 * <ul>
 *   <li><b>Multi-signature probing</b>: Try newer API signatures first, then fallback to older ones</li>
 *   <li><b>Method caching</b>: Cache resolved Method objects to avoid repeated reflection lookups</li>
 *   <li><b>Fatal path exit</b>: Where the original code used System.exit(1) on resolution failure, that behavior is preserved</li>
 *   <li><b>Quiet probing</b>: Use findMethod() for multi-signature attempts (no log on NoSuchMethodException)</li>
 * </ul>
 * 
 * <p>All methods use interface classes (e.g., IActivityManager.class) instead of proxy instances
 * for method resolution, as proxy.getClass() may not expose interface methods correctly.
 * 
 * <p>Error handling: Methods that originally called System.exit(1) on "method not found" or invoke
 * failure still do so; others return null/false. This keeps behavior consistent with the original design.
 *
 * <p>Future: If using ContentProvider.call (e.g. for settings), API 31+ uses AttributionSource in the
 * signature; older versions use 4/5/6 param signatures; use multi-signature probing and cache the Method.
 */
public class APIAdapter {

    /** Cached Method for getTasks (see getTasks). */
    private static Method getTasksMethod = null;
    /** When true, IActivityManager.getTasks is unavailable; use ActivityManager.getRunningTasks fallback. */
    private static boolean getTasksUseFallback = false;

    /** Cached Method for getPermissionInfo; 2 = (String,int), 3 = (String,String,int). */
    private static Method getPermissionInfoMethod = null;
    private static int getPermissionInfoParamCount = 0;

    /** Cached Method for registerReceiver (multi-signature probe, then reuse). */
    private static Method registerReceiverMethod = null;
    /** 2 = (..., int userId), 3a = (..., boolean), 3b = (..., int). */
    private static int registerReceiverLastParamType = 0;

    /** Cached Method for getActivityManager (see getActivityManager). */
    private static Method getActivityManagerMethod = null;
    private static boolean getActivityManagerFromNative = false; // true = ActivityManagerNative.getDefault, false = ActivityManager.getService

    /** Cached Method for setActivityController (multi-signature probe, then reuse). */
    private static Method setActivityControllerMethod = null;
    private static boolean setActivityControllerHasBoolean = false;

    /** Cached Method for broadcastIntent (multi-signature probe, then reuse). */
    private static Method broadcastIntentMethod = null;
    /** 0 = 15 params (with String[]), 1 = 13 params (String instead of String[]). */
    private static int broadcastIntentSignature = -1;

    /** Cached Method for startActivity (multi-signature probe, then reuse). */
    private static Method startActivityMethod = null;

    /** Cached Method for getEnabledInputMethodList (multi-signature probe, then reuse). */
    private static Method getEnabledInputMethodListMethod = null;
    private static boolean getEnabledInputMethodListHasInt = false;

    /** Cached Method for setInputMethod (multi-signature probe, then reuse). */
    private static Method setInputMethodMethod = null;

    /** Cached Method for freeze display rotation. Version: 3=(displayId,rotation,caller), 2=(displayId,rotation), 1=freezeRotation(rotation). */
    private static Method freezeDisplayRotationMethod = null;
    private static int freezeDisplayRotationVersion = -1;

    /** Cached Method for thaw display rotation. Version: 2=(displayId,caller), 1=(displayId), 0=thawRotation(). */
    private static Method thawDisplayRotationMethod = null;
    private static int thawDisplayRotationVersion = -1;

    /** Default caller tag for rotation (freezeDisplayRotation/thawDisplayRotation). */
    public static final String ROTATION_CALLER = "com.bytedance.fastbot";

    /** Resolve method; returns null on NoSuchMethodException/NoSuchMethodError (caller may try other signatures). On SecurityException, logs and exits. */
    private static Method findMethod(Class<?> clazz, String name, Class<?>... types) {
        return findMethod(clazz, name, true, types);
    }

    /** Same as findMethod; when logOnFailure is false, does not log on NoSuchMethod (for quiet multi-signature probe). */
    private static Method findMethod(Class<?> clazz, String name, boolean logOnFailure, Class<?>... types) {
        try {
            Method method = clazz.getMethod(name, types);
            method.setAccessible(true);
            return method;
        } catch (NoSuchMethodException e) {
            if (logOnFailure) {
                Logger.errorPrintln("findMethod() error, NoSuchMethodException happened, there is no such method: " + name);
            }
            return null;
        } catch (java.lang.NoSuchMethodError e) {
            if (logOnFailure) {
                Logger.errorPrintln("findMethod() error, NoSuchMethodError happened, there is no such method: " + name);
            }
            return null;
        } catch (SecurityException e) {
            e.printStackTrace();
            System.exit(1);
            return null;
        }
    }

    // ---------- IWindowManager rotation (freeze/thaw) multi-signature probe + cache ----------

    private static synchronized void resolveFreezeDisplayRotationMethod(IWindowManager iwm) {
        if (freezeDisplayRotationVersion >= 0) {
            return;
        }
        Class<?> iwmClass = iwm.getClass();
        try {
            freezeDisplayRotationMethod = iwmClass.getMethod("freezeDisplayRotation", int.class, int.class, String.class);
            freezeDisplayRotationMethod.setAccessible(true);
            freezeDisplayRotationVersion = 3;
            return;
        } catch (NoSuchMethodException ignored) {
        }
        try {
            freezeDisplayRotationMethod = iwmClass.getMethod("freezeDisplayRotation", int.class, int.class);
            freezeDisplayRotationMethod.setAccessible(true);
            freezeDisplayRotationVersion = 2;
            return;
        } catch (NoSuchMethodException ignored) {
        }
        freezeDisplayRotationVersion = 1; // use iwm.freezeRotation(rotation)
    }

    private static synchronized void resolveThawDisplayRotationMethod(IWindowManager iwm) {
        if (thawDisplayRotationVersion >= 0) {
            return;
        }
        Class<?> iwmClass = iwm.getClass();
        try {
            thawDisplayRotationMethod = iwmClass.getMethod("thawDisplayRotation", int.class, String.class);
            thawDisplayRotationMethod.setAccessible(true);
            thawDisplayRotationVersion = 2;
            return;
        } catch (NoSuchMethodException ignored) {
        }
        try {
            thawDisplayRotationMethod = iwmClass.getMethod("thawDisplayRotation", int.class);
            thawDisplayRotationMethod.setAccessible(true);
            thawDisplayRotationVersion = 1;
            return;
        } catch (NoSuchMethodException ignored) {
        }
        thawDisplayRotationVersion = 0; // use iwm.thawRotation()
    }

    /**
     * Freeze display rotation (multi-signature: freezeDisplayRotation(displayId,rotation,caller) → (displayId,rotation) → freezeRotation(rotation)).
     * @throws Exception InvocationTargetException, IllegalAccessException, RemoteException
     */
    public static void freezeDisplayRotation(IWindowManager iwm, int displayId, int rotationConstant, String caller) throws Exception {
        resolveFreezeDisplayRotationMethod(iwm);
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

    /**
     * Thaw display rotation (multi-signature: thawDisplayRotation(displayId,caller) → thawDisplayRotation(displayId) → thawRotation()).
     * @throws Exception InvocationTargetException, IllegalAccessException, RemoteException
     */
    public static void thawDisplayRotation(IWindowManager iwm, int displayId, String caller) throws Exception {
        resolveThawDisplayRotationMethod(iwm);
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

    public static PermissionInfo getPermissionInfo(IPackageManager ipm, String perm, int flags) {
        if (getPermissionInfoMethod == null) {
            Class<?> clazz = ipm.getClass();
            String name = "getPermissionInfo";
            Method m2 = findMethod(clazz, name, String.class, int.class);
            if (m2 != null) {
                getPermissionInfoMethod = m2;
                getPermissionInfoParamCount = 2;
            } else {
                Method m3 = findMethod(clazz, name, String.class, String.class, int.class);
                if (m3 != null) {
                    getPermissionInfoMethod = m3;
                    getPermissionInfoParamCount = 3;
                }
            }
            if (getPermissionInfoMethod == null) {
                Logger.println("Cannot resolve method: " + name);
                System.exit(1);
            }
        }
        if (getPermissionInfoParamCount == 2) {
            return (PermissionInfo) invoke(getPermissionInfoMethod, ipm, perm, flags);
        } else {
            return (PermissionInfo) invoke(getPermissionInfoMethod, ipm, perm, "shell", flags);
        }
    }

    public static void registerReceiver(IActivityManager am, IIntentReceiver receiver, IntentFilter filter, int userId) {
        if (registerReceiverMethod == null) {
            Class<?> clazz = IActivityManager.class;
            String name = "registerReceiver";
            Method m = findMethod(clazz, name, IApplicationThread.class, String.class, IIntentReceiver.class,
                    IntentFilter.class, String.class, int.class);
            if (m != null) {
                registerReceiverMethod = m;
                registerReceiverLastParamType = 2; // (..., int userId)
            } else {
                m = findMethod(clazz, name, IApplicationThread.class, String.class, IIntentReceiver.class,
                        IntentFilter.class, String.class, int.class, boolean.class);
                if (m != null) {
                    registerReceiverMethod = m;
                    registerReceiverLastParamType = 3; // (..., boolean)
                } else {
                    m = findMethod(clazz, name, IApplicationThread.class, String.class, IIntentReceiver.class,
                            IntentFilter.class, String.class, int.class, int.class);
                    if (m != null) {
                        registerReceiverMethod = m;
                        registerReceiverLastParamType = 4; // (..., int)
                    }
                }
            }
            if (registerReceiverMethod == null) {
                Logger.println("Cannot resolve method: " + name);
                System.exit(1);
            }
        }
        if (registerReceiverLastParamType == 2) {
            invoke(registerReceiverMethod, am, null, null, receiver, filter, null, userId);
        } else if (registerReceiverLastParamType == 3) {
            invoke(registerReceiverMethod, am, null, null, receiver, filter, null, userId, false);
        } else {
            invoke(registerReceiverMethod, am, null, null, receiver, filter, null, userId, 0);
        }
    }

    public static IActivityManager getActivityManager() {
        if (getActivityManagerMethod == null) {
            // Try ActivityManagerNative.getDefault() first (older Android versions)
            Method m = findMethod(ActivityManagerNative.class, "getDefault");
            if (m != null) {
                getActivityManagerMethod = m;
                getActivityManagerFromNative = true;
            } else {
                // Fallback to ActivityManager.getService() (Android 8.0+)
                m = findMethod(ActivityManager.class, "getService");
                if (m != null) {
                    getActivityManagerMethod = m;
                    getActivityManagerFromNative = false;
                }
            }
            if (getActivityManagerMethod == null) {
                Logger.println("Cannot getActivityManager");
                System.exit(1);
            }
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

    /** Invoke with exit on failure (original behavior for fatal paths). */
    private static Object invoke(Method method, Object reciver, Object... args) {
        try {
            return method.invoke(reciver, args);
        } catch (IllegalAccessException | IllegalArgumentException | InvocationTargetException e) {
            e.printStackTrace();
            System.exit(1);
            return null;
        }
    }

    private static Object invokej(Method method, Object reciver, Object... args) {
        try {
            return method.invoke(reciver, args);
        } catch (IllegalAccessException | IllegalArgumentException | InvocationTargetException e) {
            e.printStackTrace();
            return null;
        }
    }

    private static Object invokek(Method method, Object reciver, Object... args) {
        try {
            return method.invoke(reciver, args);
        } catch (IllegalAccessException | IllegalArgumentException | InvocationTargetException | SecurityException e) {
            return null;
        }
    }

    public static List<ResolveInfo> queryIntentActivities(PackageManager mPm, Intent intent) {
        return mPm.queryIntentActivities(intent, 0);
    }

    @SuppressWarnings("unchecked")
    public static List<RunningTaskInfo> getTasks(IActivityManager iAm, int maxNum) {
        if (getTasksUseFallback) {
            return getTasksViaActivityManager(maxNum);
        }
        Method method = getTasksMethod;
        if (method == null) {
            // Use IActivityManager.class so getMethod() finds the interface method; iAm.getClass()
            // returns the Binder proxy class and getMethod() may not find getTasks() on it.
            Class<?> clazz = IActivityManager.class;
            String name = "getTasks";
            method = findMethod(clazz, name, false, int.class, int.class);
            if (method == null) {
                method = findMethod(clazz, name, false, int.class);
            }
            if (method == null) {
                Logger.warningPrintln("IActivityManager.getTasks not available, using ActivityManager.getRunningTasks fallback");
                getTasksUseFallback = true;
                return getTasksViaActivityManager(maxNum);
            }
            getTasksMethod = method;
        }
        int parameterCount = method.getParameterTypes().length;
        if (parameterCount == 2) {
            return (List<RunningTaskInfo>) invokej(method, iAm, maxNum, 0 /* flags */);
        } else { // 1
            return (List<RunningTaskInfo>) invokej(method, iAm, maxNum);
        }
    }

    /**
     * Fallback when IActivityManager.getTasks() is not available (e.g. hidden API or vendor ROM).
     * Uses public ActivityManager.getRunningTasks(int). On API 31+ this may return only the caller's tasks.
     */
    private static List<RunningTaskInfo> getTasksViaActivityManager(int maxNum) {
        try {
            Context ctx = ContextUtils.getSystemContext();
            if (ctx == null) {
                return Collections.emptyList();
            }
            ActivityManager am = (ActivityManager) ctx.getSystemService(Context.ACTIVITY_SERVICE);
            if (am == null) {
                return Collections.emptyList();
            }
            List<ActivityManager.RunningTaskInfo> list = am.getRunningTasks(maxNum);
            return list != null ? list : Collections.<RunningTaskInfo>emptyList();
        } catch (Exception e) {
            Logger.warningPrintln("getTasksViaActivityManager failed: " + e.getMessage());
            return Collections.emptyList();
        }
    }


    public static void setActivityController(IActivityManager mAm, Object controller) {
        if (setActivityControllerMethod == null) {
            Class<?> clazz = mAm.getClass();
            String name = "setActivityController";
            Method m = findMethod(clazz, name, android.app.IActivityController.class, boolean.class);
            if (m != null) {
                setActivityControllerMethod = m;
                setActivityControllerHasBoolean = true;
            } else {
                m = findMethod(clazz, name, android.app.IActivityController.class);
                if (m != null) {
                    setActivityControllerMethod = m;
                    setActivityControllerHasBoolean = false;
                }
            }
            if (setActivityControllerMethod == null) {
                Logger.println("Cannot resolve method: " + name);
                System.exit(1);
            }
        }
        if (setActivityControllerHasBoolean) {
            invoke(setActivityControllerMethod, mAm, controller, true);
        } else {
            invoke(setActivityControllerMethod, mAm, controller);
        }
    }

    public static void broadcastIntent(IActivityManager mAm, Intent paramIntent) {
        if (broadcastIntentMethod == null) {
            Class<?> c0 = IActivityManager.class;
            String c1 = "broadcastIntent";
            // Try newer signature first (15 params with String[] for requiredPermissions)
            Method m = findMethod(c0, c1, IApplicationThread.class,
                    Intent.class, String.class, IIntentReceiver.class,
                    int.class, String.class, Bundle.class,
                    String[].class, int.class, Bundle.class,
                    boolean.class, boolean.class, int.class);
            if (m != null) {
                broadcastIntentMethod = m;
                broadcastIntentSignature = 0;
            } else {
                // Fallback to older signature (13 params with String instead of String[])
                m = findMethod(c0, c1, IApplicationThread.class,
                        Intent.class, String.class, IIntentReceiver.class,
                        int.class, String.class, Bundle.class,
                        String.class, int.class,
                        boolean.class, boolean.class, int.class);
                if (m != null) {
                    broadcastIntentMethod = m;
                    broadcastIntentSignature = 1;
                }
            }
            if (broadcastIntentMethod == null) {
                Logger.println("Cannot resolve method: " + c1);
                System.exit(1);
            }
        }
        if (broadcastIntentSignature == 0) {
            invoke(broadcastIntentMethod, mAm, null, paramIntent, null, null, 0, null, null, null, 0, null, false, false, 0);
        } else {
            invoke(broadcastIntentMethod, mAm, null, paramIntent, null, null, 0, null, null, null, 0, false, false, 0);
        }
    }


    public static Object startActivity(IActivityManager mAm, Intent paramIntent) {
        if (startActivityMethod == null) {
            Class<?> c0 = IActivityManager.class;
            String c1 = "startActivity";
            // Try standard signature (11 params)
            Method m = findMethod(c0, c1, IApplicationThread.class,
                    String.class, Intent.class, String.class, IBinder.class,
                    String.class, int.class, int.class, ProfilerInfo.class, Bundle.class);
            if (m != null) {
                startActivityMethod = m;
            } else {
                // Try alternative signature (some vendor ROMs may have different param order/types)
                // Note: This is a fallback; most devices should use the standard signature above
                Logger.warningPrintln("APIAdapter: Cannot resolve method " + c1 + ", startActivity will return null.");
            }
        }
        if (startActivityMethod != null) {
            return invokek(startActivityMethod, mAm, null, null, paramIntent, null, null, null, 0, 0, null, null);
        }
        return null;
    }

    @SuppressWarnings("unchecked")
    public static List<InputMethodInfo> getEnabledInputMethodList(IInputMethodManager iIMM) {
        if (getEnabledInputMethodListMethod == null) {
            Class<?> clazz = iIMM.getClass();
            String name = "getEnabledInputMethodList";
            Method m = findMethod(clazz, name, int.class);
            if (m != null) {
                getEnabledInputMethodListMethod = m;
                getEnabledInputMethodListHasInt = true;
            } else {
                m = findMethod(clazz, name);
                if (m != null) {
                    getEnabledInputMethodListMethod = m;
                    getEnabledInputMethodListHasInt = false;
                }
            }
            if (getEnabledInputMethodListMethod == null) {
                Logger.println("Cannot resolve method: " + name);
                System.exit(1);
            }
        }
        if (getEnabledInputMethodListHasInt) {
            return (List<InputMethodInfo>) invoke(getEnabledInputMethodListMethod, iIMM, 0);
        } else {
            return (List<InputMethodInfo>) invoke(getEnabledInputMethodListMethod, iIMM);
        }
    }

    public static boolean setInputMethod(IInputMethodManager iIMM, String ime) {
        if (setInputMethodMethod == null) {
            Class<?> clazz = iIMM.getClass();
            String name = "setInputMethod";
            Method m = findMethod(clazz, name, IBinder.class, String.class);
            if (m != null) {
                setInputMethodMethod = m;
            } else {
                Logger.warningPrintln("APIAdapter: Cannot resolve method " + name + ", setInputMethod will return false.");
            }
        }
        if (setInputMethodMethod != null) {
            if (invokej(setInputMethodMethod, iIMM, null, ime) != null) {
                return true;
            }
        }
        return false;
    }

    public static String getSerial() {
        String serial = "unknown";
        try {
            // Try SystemProperties.get() (most Android versions)
            Class<?> classType = Class.forName("android.os.SystemProperties");
            Method getMethod = classType.getDeclaredMethod("get", String.class);
            getMethod.setAccessible(true);
            serial = (String) getMethod.invoke(null, "ro.serialno");
            if (serial == null || serial.isEmpty()) {
                // Fallback: try getString() if available (some vendor ROMs)
                try {
                    Method getStringMethod = classType.getDeclaredMethod("getString", String.class, String.class);
                    getStringMethod.setAccessible(true);
                    String result = (String) getStringMethod.invoke(null, "ro.serialno", "unknown");
                    if (result != null && !result.isEmpty()) {
                        serial = result;
                    }
                } catch (Exception ignored) {
                    // Ignore fallback failures
                }
            }
        } catch (ClassNotFoundException e) {
            Logger.warningPrintln("APIAdapter: SystemProperties class not found, serial will be 'unknown'");
        } catch (NoSuchMethodException e) {
            Logger.warningPrintln("APIAdapter: SystemProperties.get() method not found, serial will be 'unknown'");
        } catch (Exception e) {
            Logger.warningPrintln("APIAdapter: Failed to get serial: " + e.getMessage());
        }
        Logger.println("// device serial number is " + serial);
        return serial;
    }
}
