/*
 * This code is licensed under the Fastbot license. You may obtain a copy of this license in the LICENSE.txt file in the root directory of this source tree.
 */

package com.android.commands.monkey.utils;

import android.util.Log;

import java.text.SimpleDateFormat;
import java.util.Date;
import java.util.Locale;

import static com.android.commands.monkey.utils.Config.debug;

/**
 * @author Zhao Zhang, Tianxiao Gu
 */

public class Logger {

    public static final String TAG = "[Fastbot]";

    public static void logo() {
        System.out.format
                ("▄▄▄▄▄▄▄▄▄▄▄  ▄▄▄▄▄▄▄▄▄▄▄  ▄▄▄▄▄▄▄▄▄▄▄  ▄▄▄▄▄▄▄▄▄▄▄  ▄▄▄▄▄▄▄▄▄▄   ▄▄▄▄▄▄▄▄▄▄▄  ▄▄▄▄▄▄▄▄▄▄▄ \n" +
                "▐░░░░░░░░░░░▌▐░░░░░░░░░░░▌▐░░░░░░░░░░░▌▐░░░░░░░░░░░▌▐░░░░░░░░░░▌ ▐░░░░░░░░░░░▌▐░░░░░░░░░░░▌\n" +
                "▐░█▀▀▀▀▀▀▀▀▀ ▐░█▀▀▀▀▀▀▀█░▌▐░█▀▀▀▀▀▀▀▀▀  ▀▀▀▀█░█▀▀▀▀ ▐░█▀▀▀▀▀▀▀█░▌▐░█▀▀▀▀▀▀▀█░▌ ▀▀▀▀█░█▀▀▀▀ \n" +
                "▐░▌          ▐░▌       ▐░▌▐░▌               ▐░▌     ▐░▌       ▐░▌▐░▌       ▐░▌     ▐░▌     \n" +
                "▐░█▄▄▄▄▄▄▄▄▄ ▐░█▄▄▄▄▄▄▄█░▌▐░█▄▄▄▄▄▄▄▄▄      ▐░▌     ▐░█▄▄▄▄▄▄▄█░▌▐░▌       ▐░▌     ▐░▌     \n" +
                "▐░░░░░░░░░░░▌▐░░░░░░░░░░░▌▐░░░░░░░░░░░▌     ▐░▌     ▐░░░░░░░░░░▌ ▐░▌       ▐░▌     ▐░▌     \n" +
                "▐░█▀▀▀▀▀▀▀▀▀ ▐░█▀▀▀▀▀▀▀█░▌ ▀▀▀▀▀▀▀▀▀█░▌     ▐░▌     ▐░█▀▀▀▀▀▀▀█░▌▐░▌       ▐░▌     ▐░▌     \n" +
                "▐░▌          ▐░▌       ▐░▌          ▐░▌     ▐░▌     ▐░▌       ▐░▌▐░▌       ▐░▌     ▐░▌     \n" +
                "▐░▌          ▐░▌       ▐░▌ ▄▄▄▄▄▄▄▄▄█░▌     ▐░▌     ▐░█▄▄▄▄▄▄▄█░▌▐░█▄▄▄▄▄▄▄█░▌     ▐░▌     \n" +
                "▐░▌          ▐░▌       ▐░▌▐░░░░░░░░░░░▌     ▐░▌     ▐░░░░░░░░░░▌ ▐░░░░░░░░░░░▌     ▐░▌     \n" +
                " ▀            ▀         ▀  ▀▀▀▀▀▀▀▀▀▀▀       ▀       ▀▀▀▀▀▀▀▀▀▀   ▀▀▀▀▀▀▀▀▀▀▀       ▀\n");
    }

    // PERFORMANCE_OPTIMIZATION_ITEMS §4.4: reuse SimpleDateFormat per thread to avoid allocation on every log.
    private static final ThreadLocal<SimpleDateFormat> sDateFormat = new ThreadLocal<SimpleDateFormat>() {
        @Override
        protected SimpleDateFormat initialValue() {
            return new SimpleDateFormat("yyyy-MM-dd HH:mm:ss.SSS", Locale.ENGLISH);
        }
    };

    public static String getCurrentTimeStamp() {
        return sDateFormat.get().format(new Date());
    }

    private static String safeToString(Object message) {
        return String.valueOf(message);
    }

    public static void println(Object message) {
        String text = safeToString(message);
        try {
            System.out.format("%s[%s] %s%n", TAG, getCurrentTimeStamp(), text);
            Log.i(TAG, text);
        } catch (Exception ignored) {
            // Swallow logging exceptions to avoid impacting caller.
        }
    }

    public static void format(String format, Object... args) {
        if (debug) {
            System.out.format("%s%s%n", TAG, String.format(format, args));
        }
    }

    public static void debugFormat(String format, Object... args) {
        if (debug) {
            System.out.format("%s*** DEBUG *** %s%n", TAG, String.format(format, args));
        }
    }

    public static void warningFormat(String format, Object... args) {
        System.out.format("%s*** WARNING *** %s%n", TAG, String.format(format, args));
    }

    public static void infoFormat(String format, Object... args) {
        if (debug) {
            System.out.format("%s*** INFO *** %s%n", TAG, String.format(format, args));
        }
    }

    public static void warningPrintln(Object message) {
        String text = safeToString(message);
        System.out.format("%s*** WARNING *** %s%n", TAG, text);
        Log.w(TAG, "*** WARNING *** " + text);
    }

    public static void infoPrintln(Object message) {
        if (debug) {
            String text = safeToString(message);
            System.out.format("%s*** INFO *** %s%n", TAG, text);
            Log.i(TAG, "*** INFO *** " + text);
        }
    }

    public static void errorPrintln(Object message) {
        String text = safeToString(message);
        System.err.format("%s*** ERROR *** %s%n", TAG, text);
        Log.e(TAG, "*** ERROR *** " + text);
    }
}
