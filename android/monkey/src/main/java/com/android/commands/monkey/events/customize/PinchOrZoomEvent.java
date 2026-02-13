/*
 * This code is licensed under the Fastbot license. You may obtain a copy of this license in the LICENSE.txt file in the root directory of this source tree.
 */

package com.android.commands.monkey.events.customize;

import android.graphics.PointF;
import android.os.SystemClock;
import android.view.MotionEvent;

import com.android.commands.monkey.events.CustomEvent;
import com.android.commands.monkey.events.MonkeyEvent;
import com.android.commands.monkey.events.base.MonkeyTouchEvent;

import org.json.JSONArray;
import org.json.JSONException;
import org.json.JSONObject;

import java.util.ArrayList;
import java.util.Collections;
import java.util.List;

/**
 * @author Jiarong Fu
 */

public class PinchOrZoomEvent extends AbstractCustomEvent {

    /**
     *
     */
    private static final long serialVersionUID = 1L;
    float[] values;

    public PinchOrZoomEvent(PointF[] points) {
        this.values = fromPointsArray(points);
        if (points.length < 4) {
            throw new IllegalArgumentException();
        }
    }

    public PinchOrZoomEvent(float[] values) {
        this.values = values;
    }

    /** For black-rect shielding: get all trajectory points. */
    public PointF[] getPoints() {
        return toPointsArray(values);
    }

    /** For black-rect shielding: set trajectory points after deflection. */
    public void setPoints(PointF[] points) {
        if (points != null && points.length >= 4) {
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
        return new PinchOrZoomEvent(values);
    }

    /** Minimum points: 1 DOWN + 2 POINTER_DOWN + 2 POINTER_UP + 1 UP = 6 (12 floats). */
    private static final int MIN_FLOATS = 12;

    @Override
    public List<MonkeyEvent> generateMonkeyEvents() {
        final int pointCount = values.length >> 1;
        if (values.length < MIN_FLOATS || pointCount < 6) {
            return Collections.emptyList();
        }
        long downAt = SystemClock.uptimeMillis();
        List<MonkeyEvent> events = new ArrayList<MonkeyEvent>(pointCount + 2);
        float x0 = values[0], y0 = values[1];
        events.add(new MonkeyTouchEvent(MotionEvent.ACTION_DOWN).setDownTime(downAt).addPointer(0, x0, y0)
                .setIntermediateNote(false));
        float x1 = values[2], y1 = values[3];
        float x2 = values[4], y2 = values[5];
        events.add(new MonkeyTouchEvent(MotionEvent.ACTION_POINTER_DOWN | (1 << MotionEvent.ACTION_POINTER_INDEX_SHIFT))
                .setDownTime(downAt).addPointer(0, x1, y1).addPointer(1, x2, y2).setIntermediateNote(true));
        for (int i = 3; i < pointCount - 3; i++) {
            x1 = values[i * 2];
            y1 = values[i * 2 + 1];
            x2 = values[(i + 1) * 2];
            y2 = values[(i + 1) * 2 + 1];
            events.add(new MonkeyTouchEvent(MotionEvent.ACTION_MOVE).setDownTime(downAt).addPointer(0, x1, y1)
                    .addPointer(1, x2, y2).setIntermediateNote(true));
        }
        x1 = values[(pointCount - 3) * 2];
        y1 = values[(pointCount - 3) * 2 + 1];
        x2 = values[(pointCount - 2) * 2];
        y2 = values[(pointCount - 2) * 2 + 1];
        events.add(new MonkeyTouchEvent(MotionEvent.ACTION_POINTER_UP | (1 << MotionEvent.ACTION_POINTER_INDEX_SHIFT))
                .setDownTime(downAt).addPointer(0, x1, y1).addPointer(1, x2, y2).setIntermediateNote(true));
        x0 = values[(pointCount - 1) * 2];
        y0 = values[(pointCount - 1) * 2 + 1];
        events.add(new MonkeyTouchEvent(MotionEvent.ACTION_UP).setDownTime(downAt).addPointer(0, x0, y0)
                .setIntermediateNote(false));
        return events;
    }

    public JSONObject toJSONObject() throws JSONException {
        JSONObject jEvent = new JSONObject();
        jEvent.put("type", "p");
        JSONArray jPoint = new JSONArray();
        for (float x : values) {
            jPoint.put(x);
        }
        jEvent.put("values", jPoint);
        return jEvent;
    }
}
