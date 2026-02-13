/*
 * This code is licensed under the Fastbot license. You may obtain a copy of this license in the LICENSE.txt file in the root directory of this source tree.
 */

package com.android.commands.monkey.events.customize;

import android.graphics.PointF;
import android.os.SystemClock;
import android.view.MotionEvent;

import com.android.commands.monkey.events.CustomEvent;
import com.android.commands.monkey.events.MonkeyEvent;
import com.android.commands.monkey.events.base.MonkeyThrottleEvent;
import com.android.commands.monkey.events.base.MonkeyTouchEvent;

import org.json.JSONArray;
import org.json.JSONException;
import org.json.JSONObject;

import java.util.ArrayList;
import java.util.List;

import static com.android.commands.monkey.utils.Config.swipeDuration;

/**
 * @author Zhao Zhang
 */

public class DragEvent extends AbstractCustomEvent {

    /**
     *
     */
    private static final long serialVersionUID = 1L;
    float[] values;

    public DragEvent(PointF[] points) {
        this.values = fromPointsArray(points);
        if (points.length < 2) {
            throw new IllegalArgumentException();
        }
    }

    /** Internal use: construct from raw float[x0,y0,...] values (used by JSON/fuzzer). */
    public DragEvent(float[] values) {
        this.values = values;
    }

    /** For black-rect shielding: get all trajectory points. */
    public PointF[] getPoints() {
        return toPointsArray(values);
    }

    /** For black-rect shielding: set trajectory points after deflection. */
    public void setPoints(PointF[] points) {
        if (points != null && points.length >= 2) {
            this.values = fromPointsArray(points);
        }
    }

    /** Apply shield in-place to values (x,y pairs). Avoids allocating PointF[]. */
    public void applyShieldInPlace(AbstractCustomEvent.ShieldInPlace shield) {
        for (int i = 0; i < values.length >> 1; i++) {
            shield.apply(values[i * 2], values[i * 2 + 1], values, i * 2);
        }
    }

    public static CustomEvent fromJSONObject(JSONObject jEvent) throws JSONException {
        JSONArray jValues = jEvent.getJSONArray("values");
        float[] values = new float[jValues.length()];
        for (int i = 0; i < values.length; i++) {
            values[i] = (float) jValues.getDouble(i);
        }
        return new DragEvent(values);
    }

    @Override
    public List<MonkeyEvent> generateMonkeyEvents() {
        final int pointCount = values.length >> 1;
        if (pointCount < 2) return new ArrayList<MonkeyEvent>(0);
        List<MonkeyEvent> events = new ArrayList<MonkeyEvent>(pointCount * 2 - 1);
        long downAt = SystemClock.uptimeMillis();
        float x = values[0], y = values[1];
        events.add(new MonkeyTouchEvent(MotionEvent.ACTION_DOWN).setDownTime(downAt).addPointer(0, x, y)
                .setIntermediateNote(false));
        long waitTime = swipeDuration / pointCount;
        for (int i = 1; i < pointCount - 1; i++) {
            x = values[i * 2];
            y = values[i * 2 + 1];
            events.add(new MonkeyTouchEvent(MotionEvent.ACTION_MOVE).setDownTime(downAt).addPointer(0, x, y)
                    .setIntermediateNote(true));
            events.add(MonkeyThrottleEvent.obtain(waitTime));
        }
        x = values[(pointCount - 1) * 2];
        y = values[(pointCount - 1) * 2 + 1];
        events.add(new MonkeyTouchEvent(MotionEvent.ACTION_UP).setDownTime(downAt).addPointer(0, x, y)
                .setIntermediateNote(false));
        return events;
    }

    public JSONObject toJSONObject() throws JSONException {
        JSONObject jEvent = new JSONObject();
        jEvent.put("type", "d");
        JSONArray jPoint = new JSONArray();
        for (float x : values) {
            jPoint.put(x);
        }
        jEvent.put("values", jPoint);
        return jEvent;
    }
}
