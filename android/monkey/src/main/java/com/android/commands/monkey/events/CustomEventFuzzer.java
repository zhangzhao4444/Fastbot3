/**
 * @author Zhao Zhang
 */

package com.android.commands.monkey.events;

import android.graphics.PointF;
import android.graphics.Rect;
import android.view.KeyCharacterMap;
import android.view.Surface;

import com.android.commands.monkey.events.customize.AirplaneEvent;
import com.android.commands.monkey.events.customize.AlwaysFinishActivityEvent;
import com.android.commands.monkey.events.customize.ClickEvent;
import com.android.commands.monkey.events.customize.DragEvent;
import com.android.commands.monkey.events.customize.KeyEvent;
import com.android.commands.monkey.events.customize.PinchOrZoomEvent;
import com.android.commands.monkey.events.customize.RotationEvent;
import com.android.commands.monkey.events.customize.SwitchEvent;
import com.android.commands.monkey.events.customize.TrackballEvent;
import com.android.commands.monkey.events.customize.WifiEvent;
import com.android.commands.monkey.framework.AndroidDevice;
import com.android.commands.monkey.utils.Config;
import com.android.commands.monkey.utils.Logger;
import com.android.commands.monkey.utils.RandomHelper;

import org.json.JSONException;
import org.json.JSONObject;

import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.List;
import java.util.function.Consumer;

/**
 * @author Zhao Zhang
 */

public class CustomEventFuzzer {


    /**
     * Possible screen rotation degrees
     **/
    private static final int[] SCREEN_ROTATION_DEGREES = {Surface.ROTATION_0, Surface.ROTATION_90,
            Surface.ROTATION_180, Surface.ROTATION_270,};
    private static final int[] NAV_KEYS = {android.view.KeyEvent.KEYCODE_DPAD_UP, android.view.KeyEvent.KEYCODE_DPAD_DOWN,
            android.view.KeyEvent.KEYCODE_DPAD_LEFT, android.view.KeyEvent.KEYCODE_DPAD_RIGHT,};
    /**
     * Key events that perform major navigation options (so shouldn't be sent as
     * much).
     */
    private static final int[] MAJOR_NAV_KEYS = {android.view.KeyEvent.KEYCODE_MENU, /*
     * KeyEvent
     * .
     * KEYCODE_SOFT_RIGHT,
     */
            android.view.KeyEvent.KEYCODE_DPAD_CENTER,};
    /**
     * Key events that perform system operations.
     */
    private static final int[] SYS_KEYS = {
            android.view.KeyEvent.KEYCODE_HOME, android.view.KeyEvent.KEYCODE_BACK,
            android.view.KeyEvent.KEYCODE_CALL,
            //KeyEvent.KEYCODE_ENDCALL,
            //KeyEvent.KEYCODE_VOLUME_UP,
            android.view.KeyEvent.KEYCODE_VOLUME_DOWN, android.view.KeyEvent.KEYCODE_VOLUME_MUTE,
            android.view.KeyEvent.KEYCODE_MUTE,};
    /**
     * If a physical key exists?
     */
    private static final boolean[] PHYSICAL_KEY_EXISTS = new boolean[android.view.KeyEvent.getMaxKeyCode() + 1];
    //private static Pattern BOUNDS_RECT = Pattern.compile("([0-9]+),([0-9]+),([0-9]+),([0-9]+)");

    static {
        for (int i = 0; i < PHYSICAL_KEY_EXISTS.length; ++i) {
            PHYSICAL_KEY_EXISTS[i] = true;
        }
        // Only examine SYS_KEYS
        for (int i = 0; i < SYS_KEYS.length; ++i) {
            PHYSICAL_KEY_EXISTS[SYS_KEYS[i]] = KeyCharacterMap.deviceHasKey(SYS_KEYS[i]);
        }
    }

    /**
     * Parse one fuzz action JSON from native (performance §3.3). Returns single-element list or empty on error.
     */
    public static List<CustomEvent> fromNativeFuzzJson(String json) {
        if (json == null || json.isEmpty()) return Collections.emptyList();
        try {
            JSONObject j = new JSONObject(json);
            String type = j.optString("type", "");
            CustomEvent ev = null;
            switch (type) {
                case "click":
                    ev = ClickEvent.fromJSONObject(j);
                    break;
                case "rotation":
                    ev = RotationEvent.fromJSONObject(j);
                    break;
                case "app_switch":
                    ev = SwitchEvent.fromJSONObject(j);
                    break;
                case "drag":
                    ev = DragEvent.fromJSONObject(j);
                    break;
                case "pinch":
                    ev = PinchOrZoomEvent.fromJSONObject(j);
                    break;
                default:
                    return Collections.emptyList();
            }
            return ev != null ? Collections.singletonList(ev) : Collections.<CustomEvent>emptyList();
        } catch (JSONException e) {
            Logger.warningPrintln("fromNativeFuzzJson parse error: " + e.getMessage());
            return Collections.emptyList();
        }
    }

    /** Simplify mode: only rotation, app_switch, drag, pinch, click. Reuses same fuzzers as full list. */
    public static List<CustomEvent> generateSimplifyFuzzingEvents() {
        if (eventFuzzers == null) initEventFuzzers();
        List<CustomEvent> events = new ArrayList<CustomEvent>();
        int repeat = RandomHelper.nextBetween(1, 3);
        for (int r = 0; r < repeat; r++) {
            int idx = simplifyIndices[RandomHelper.nextInt(simplifyIndices.length)];
            eventFuzzers.get(idx).genEvent(events, false);
        }
        return events;
    }

    public interface EventFuzzer {
        void genEvent(List<CustomEvent> events, boolean enableRate);
        double getRate();
    }

    /** Table-driven fuzzer: rate + generator. Weights are from Config; add same fuzzer multiple times to increase weight. */
    private static final class ConfigurableFuzzer implements EventFuzzer {
        private final double rate;
        private final Consumer<List<CustomEvent>> generator;

        ConfigurableFuzzer(double rate, Consumer<List<CustomEvent>> generator) {
            this.rate = rate;
            this.generator = generator;
        }

        @Override
        public void genEvent(List<CustomEvent> events, boolean enableRate) {
            if (!enableRate || RandomHelper.nextFloat() < rate) {
                generator.accept(events);
            }
        }

        @Override
        public double getRate() { return rate; }
    }

    private static ArrayList<EventFuzzer> eventFuzzers = null;
    private static double[] cumulativeRates = null;
    private static int[] simplifyIndices = null;

    /** Fuzzer indices in eventFuzzers (must match initEventFuzzers order). Used for simplify mode. */
    private static final int IDX_ROTATION = 0;
    private static final int IDX_APP_SWITCH = 1;
    private static final int IDX_DRAG = 7;
    private static final int IDX_PINCH = 8;
    private static final int IDX_CLICK = 12;

    private static void initEventFuzzers() {
        if (eventFuzzers != null) return;
        eventFuzzers = new ArrayList<EventFuzzer>();
        eventFuzzers.add(new ConfigurableFuzzer(Config.doRotationFuzzing, CustomEventFuzzer::generateRotationEvent));
        eventFuzzers.add(new ConfigurableFuzzer(Config.doAppSwitchFuzzing, CustomEventFuzzer::generateAppSwitchEvent));
        eventFuzzers.add(new ConfigurableFuzzer(Config.doTrackballFuzzing, CustomEventFuzzer::generateTrackballEvent));
        eventFuzzers.add(new ConfigurableFuzzer(Config.doNavKeyFuzzing, CustomEventFuzzer::generateFuzzingNavKeyEvent));
        eventFuzzers.add(new ConfigurableFuzzer(Config.doNavKeyFuzzing, CustomEventFuzzer::generateFuzzingMajorNavKeyEvent));
        eventFuzzers.add(new ConfigurableFuzzer(Config.doKeyCodeFuzzing, CustomEventFuzzer::generateKeyCodeEvent));
        eventFuzzers.add(new ConfigurableFuzzer(Config.doSystemKeyFuzzing, CustomEventFuzzer::generateFuzzingSysKeyEvent));
        eventFuzzers.add(new ConfigurableFuzzer(Config.doDragFuzzing, CustomEventFuzzer::generateDragEvent));
        eventFuzzers.add(new ConfigurableFuzzer(Config.doPinchZoomFuzzing, CustomEventFuzzer::generatePinchOrZoomEvent));
        eventFuzzers.add(new ConfigurableFuzzer(Config.doMutationWifiFuzzing, CustomEventFuzzer::generateWifiEvent));
        eventFuzzers.add(new ConfigurableFuzzer(Config.doMutationAirplaneFuzzing, CustomEventFuzzer::generateAirplaneEvent));
        eventFuzzers.add(new ConfigurableFuzzer(Config.doMutationMutationAlwaysFinishActivitysFuzzing, CustomEventFuzzer::generateAlwaysFinishActivitiesEvent));
        eventFuzzers.add(new ConfigurableFuzzer(Config.doClickFuzzing, events -> generateClickEvent(events, RandomHelper.nextInt(1000))));
        int n = eventFuzzers.size();
        cumulativeRates = new double[n];
        double sum = 0.0;
        for (int i = 0; i < n; i++) {
            sum += eventFuzzers.get(i).getRate();
            cumulativeRates[i] = sum;
        }
        simplifyIndices = new int[] { IDX_ROTATION, IDX_APP_SWITCH, IDX_DRAG, IDX_PINCH, IDX_CLICK };
    }

    private static final int KEYCODE_PICK_MAX_ITERATIONS = 100;

    private static void generateKeyCodeEvent(List<CustomEvent> events) {
        int lastKey;
        for (int iter = 0; iter < KEYCODE_PICK_MAX_ITERATIONS; iter++) {
            lastKey = 1 + RandomHelper.nextInt(android.view.KeyEvent.getMaxKeyCode() - 1);
            if (lastKey != android.view.KeyEvent.KEYCODE_POWER && lastKey != android.view.KeyEvent.KEYCODE_ENDCALL
                    && lastKey != android.view.KeyEvent.KEYCODE_SLEEP && PHYSICAL_KEY_EXISTS[lastKey]) {
                generateKeyEvent(events, lastKey);
                return;
            }
        }
        int fallback = SYS_KEYS[RandomHelper.nextInt(SYS_KEYS.length)];
        generateKeyEvent(events, fallback);
    }

    public static List<CustomEvent> generateFuzzingEvents() {
        if (eventFuzzers == null)
            initEventFuzzers();

        double totalRate = cumulativeRates[cumulativeRates.length - 1];
        if (totalRate <= 0.0) return Collections.emptyList();

        int eventSize = RandomHelper.nextBetween(5, 10);
        List<CustomEvent> fuzzingEvents = new ArrayList<CustomEvent>(eventSize);
        while (fuzzingEvents.size() < eventSize) {
            double pick = RandomHelper.nextDouble() * totalRate;
            int i = Arrays.binarySearch(cumulativeRates, pick);
            if (i < 0) i = -i - 1;
            if (i >= cumulativeRates.length) i = cumulativeRates.length - 1;
            eventFuzzers.get(i).genEvent(fuzzingEvents, false);
        }
        return fuzzingEvents;
    }

    private static void generateClickEvent(List<CustomEvent> events, long waitTime) {
        Rect displayBounds = AndroidDevice.getDisplayBounds(AndroidDevice.getFocusedDisplayId());
        int x = RandomHelper.nextInt(displayBounds.right);
        int y = RandomHelper.nextInt(displayBounds.bottom);
        events.add(new ClickEvent(new PointF(x, y), waitTime));
    }


    private static void generateWifiEvent(List<CustomEvent> events) {
        events.add(new WifiEvent());
    }


    private static void generateAirplaneEvent(List<CustomEvent> events) {
        events.add(new AirplaneEvent());
    }


    private static void generateAlwaysFinishActivitiesEvent(List<CustomEvent> events) {
        events.add(new AlwaysFinishActivityEvent());
    }


    private static void generatePinchOrZoomEvent(List<CustomEvent> events) {
        Rect displayBounds = AndroidDevice.getDisplayBounds(AndroidDevice.getFocusedDisplayId());
        int width = displayBounds.right;
        int height = displayBounds.bottom;
        int count = RandomHelper.nextInt(10);
        int pointCount = 6 + count * 2;
        float[] values = new float[pointCount * 2];
        int index = 0;

        // First finger down
        float x0 = RandomHelper.nextInt(width);
        float y0 = RandomHelper.nextInt(height);
        values[index++] = x0;
        values[index++] = y0;

        // Second and third points (two fingers)
        float x1 = RandomHelper.nextInt(width);
        float y1 = RandomHelper.nextInt(height);
        float x2 = RandomHelper.nextInt(width);
        float y2 = RandomHelper.nextInt(height);
        values[index++] = x1;
        values[index++] = y1;
        values[index++] = x2;
        values[index++] = y2;

        // Random movement vectors
        float vx1 = (RandomHelper.nextFloat() - 0.5f) * 50;
        float vy1 = (RandomHelper.nextFloat() - 0.5f) * 50;
        float vx2 = (RandomHelper.nextFloat() - 0.5f) * 50;
        float vy2 = (RandomHelper.nextFloat() - 0.5f) * 50;
        // An extra point in addition to count is required to simulate finger lift
        for (int i = 0; i < count + 1; i++) {
            x1 = clamp(x1 + RandomHelper.nextFloat() * vx1, 0, width);
            y1 = clamp(y1 + RandomHelper.nextFloat() * vy1, 0, height);
            x2 = clamp(x2 + RandomHelper.nextFloat() * vx2, 0, width);
            y2 = clamp(y2 + RandomHelper.nextFloat() * vy2, 0, height);
            values[index++] = x1;
            values[index++] = y1;
            values[index++] = x2;
            values[index++] = y2;
        }

        // Final point for finger lift
        x1 = clamp(x1 + RandomHelper.nextFloat() * vx1, 0, width);
        y1 = clamp(y1 + RandomHelper.nextFloat() * vy1, 0, height);
        values[index++] = x1;
        values[index++] = y1;

        events.add(new PinchOrZoomEvent(values));
    }

    private static void generateDragEvent(List<CustomEvent> events) {
        Rect displayBounds = AndroidDevice.getDisplayBounds(AndroidDevice.getFocusedDisplayId());
        int width = displayBounds.right;
        int height = displayBounds.bottom;
        int count = RandomHelper.nextInt(10);
        int pointCount = 2 + count;
        float[] values = new float[pointCount * 2];
        int index = 0;

        float x = RandomHelper.nextInt(width);
        float y = RandomHelper.nextInt(height);
        values[index++] = x;
        values[index++] = y;

        float vx = (RandomHelper.nextFloat() - 0.5f) * 50;
        float vy = (RandomHelper.nextFloat() - 0.5f) * 50;
        for (int i = 1; i < pointCount; i++) {
            x = clamp(x + RandomHelper.nextFloat() * vx, 0, width);
            y = clamp(y + RandomHelper.nextFloat() * vy, 0, height);
            values[index++] = x;
            values[index++] = y;
        }
        events.add(new DragEvent(values));
    }

    private static void generateKeyEvent(List<CustomEvent> events, int key) {
        events.add(new KeyEvent(key));
    }


    private static void generateFuzzingSysKeyEvent(List<CustomEvent> events) {
        int key = SYS_KEYS[RandomHelper.nextInt(SYS_KEYS.length)];
        generateKeyEvent(events, key);
    }

    private static void generateFuzzingMajorNavKeyEvent(List<CustomEvent> events) {
        int key = MAJOR_NAV_KEYS[RandomHelper.nextInt(MAJOR_NAV_KEYS.length)];
        generateKeyEvent(events, key);
    }

    private static void generateFuzzingNavKeyEvent(List<CustomEvent> events) {
        int key = NAV_KEYS[RandomHelper.nextInt(NAV_KEYS.length)];
        generateKeyEvent(events, key);
    }

    private static void generateTrackballEvent(List<CustomEvent> events) {
        int moves = 10;
        int[] deltaX = new int[moves];
        int[] deltaY = new int[moves];
        for (int i = 0; i < moves; ++i) {
            deltaX[i] = RandomHelper.nextInt(10) - 5;
            deltaY[i] = RandomHelper.nextInt(10) - 5;
        }
        events.add(new TrackballEvent(deltaX, deltaY, RandomHelper.nextBoolean()));
    }

    private static void generateAppSwitchEvent(List<CustomEvent> events) {
        events.add(new SwitchEvent(RandomHelper.nextBoolean()));
    }

    private static void generateRotationEvent(List<CustomEvent> events) {
        int degree = SCREEN_ROTATION_DEGREES[RandomHelper.nextInt(SCREEN_ROTATION_DEGREES.length)];
        events.add(new RotationEvent(degree, RandomHelper.nextBoolean()));
    }

    private static float clamp(float value, float min, float max) {
        if (value < min) return min;
        if (value > max) return max;
        return value;
    }
}
