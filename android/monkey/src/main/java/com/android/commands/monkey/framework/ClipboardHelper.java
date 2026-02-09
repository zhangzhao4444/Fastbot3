/*
 * Clipboard helper for Fastbot3 Monkey.
 *
 * ClipboardManager from a system Context uses package "android", which does not
 * match the shell UID (2000) used by app_process, causing SecurityException
 * ("Package android does not belong to 2000"). To avoid this, we talk directly
 * to the clipboard system service via android.content.IClipboard and pass
 * callingPackage "com.android.shell", which matches the shell UID.
 */
/*
 * @author Zhao Zhang
 */

package com.android.commands.monkey.framework;

import android.content.ClipData;
import android.os.IBinder;

import com.android.commands.monkey.framework.APIAdapter;
import com.android.commands.monkey.utils.Logger;

import java.lang.reflect.Method;

public final class ClipboardHelper {

    private static final String SHELL_PACKAGE = "com.android.shell";

    private ClipboardHelper() {
        // no instances
    }

    /** Get IClipboard binder via ServiceManager using reflection. */
    private static Object getIClipboard() {
        try {
            Class<?> smClass = Class.forName("android.os.ServiceManager");
            Method getService = smClass.getMethod("getService", String.class);
            IBinder binder = (IBinder) getService.invoke(null, "clipboard");
            if (binder == null) {
                Logger.warningPrintln("ClipboardHelper: clipboard service is null");
                return null;
            }
            Class<?> stubClass = Class.forName("android.content.IClipboard$Stub");
            Method asInterface = stubClass.getMethod("asInterface", IBinder.class);
            return asInterface.invoke(null, binder);
        } catch (Throwable t) {
            Logger.warningPrintln("ClipboardHelper: getIClipboard failed: " + t);
            return null;
        }
    }

    public static boolean setText(CharSequence text) {
        try {
            Object iClipboard = getIClipboard();
            if (iClipboard == null) {
                return false;
            }
            ClipData clip = ClipData.newPlainText(null, text);

            Class<?>[][] sigs = new Class<?>[][]{
                    // 1) Legacy (API 21-28): setPrimaryClip(ClipData, String)
                    {ClipData.class, String.class},
                    // 2) Android Q+ (API 29+): setPrimaryClip(ClipData, String, int userId)
                    {ClipData.class, String.class, int.class},
                    // 3) Android T+ (API 33+): setPrimaryClip(ClipData, String, String attributionTag, int userId)
                    {ClipData.class, String.class, String.class, int.class},
                    // 4) Android U+ (API 34+): setPrimaryClip(ClipData, String, String attributionTag, int userId, int deviceId)
                    {ClipData.class, String.class, String.class, int.class, int.class},
            };

            Object[][] args = new Object[][]{
                    {clip, SHELL_PACKAGE},                          // Legacy: callingPackage only
                    {clip, SHELL_PACKAGE, 0},                       // Q+: callingPackage, userId
                    {clip, SHELL_PACKAGE, null, 0},                 // T+: callingPackage, attributionTag, userId
                    {clip, SHELL_PACKAGE, null, 0, 0},              // U+: callingPackage, attributionTag, userId, deviceId
            };

            Exception last = null;
            for (int i = 0; i < sigs.length; i++) {
                try {
                    Method m = iClipboard.getClass().getMethod("setPrimaryClip", sigs[i]);
                    m.invoke(iClipboard, args[i]);
                    return true;
                } catch (Exception e) {
                    last = e;
                }
            }

            if (last != null) {
                Logger.warningPrintln("ClipboardHelper: setText failed via IClipboard: " + last);
            }
            return false;
        } catch (Throwable t) {
            Logger.warningPrintln("ClipboardHelper: setText failed (outer): " + t);
            return false;
        }
    }

    public static CharSequence getText() {
        try {
            Object iClipboard = getIClipboard();
            if (iClipboard == null) {
                return null;
            }

            Class<?>[][] sigs = new Class<?>[][]{
                    // 1) Legacy (API 21-28): getPrimaryClip(String)
                    {String.class},
                    // 2) Android Q+ (API 29+): getPrimaryClip(String, int userId)
                    {String.class, int.class},
                    // 3) Android T+ (API 33+): getPrimaryClip(String, String attributionTag, int userId)
                    {String.class, String.class, int.class},
                    // 4) Android U+ (API 34+): getPrimaryClip(String, String attributionTag, int userId, int deviceId)
                    {String.class, String.class, int.class, int.class},
            };

            Object[][] args = new Object[][]{
                    {SHELL_PACKAGE},                      // Legacy: pkg only
                    {SHELL_PACKAGE, 0},                   // Q+: pkg, userId
                    {SHELL_PACKAGE, null, 0},            // T+: pkg, attributionTag, userId
                    {SHELL_PACKAGE, null, 0, 0},         // U+: pkg, attributionTag, userId, deviceId
            };

            ClipData clip = null;
            Exception last = null;
            for (int i = 0; i < sigs.length; i++) {
                try {
                    Method m = iClipboard.getClass().getMethod("getPrimaryClip", sigs[i]);
                    clip = (ClipData) m.invoke(iClipboard, args[i]);
                    if (clip != null) {
                        break;
                    }
                } catch (Exception e) {
                    last = e;
                }
            }

            if (clip == null) {
                if (last != null) {
                    Logger.warningPrintln("ClipboardHelper: getText failed via IClipboard (tried all getPrimaryClip signatures); last error: " + last);
                }
                return null;
            }
            if (clip.getItemCount() == 0) {
                return null;
            }
            return clip.getItemAt(0).coerceToText(APIAdapter.getSystemContext());
        } catch (Throwable t) {
            Logger.warningPrintln("ClipboardHelper: getText failed (outer): " + t);
            return null;
        }
    }
}


