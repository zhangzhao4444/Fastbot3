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

    public static CustomEvent fromJSONObject(JSONObject jEvent) throws JSONException {
        JSONArray jValues = jEvent.getJSONArray("values");
        float[] values = new float[jValues.length()];
        for (int i = 0; i < values.length; i++) {
            values[i] = (float) jValues.getDouble(i);
        }
        return new PinchOrZoomEvent(values);
    }

    /** Minimum points: 1 DOWN + 2 POINTER_DOWN + 2 POINTER_UP + 1 UP = 6. */
    private static final int MIN_POINTS = 6;

    @Override
    public List<MonkeyEvent> generateMonkeyEvents() {
        int index = 0;
        PointF[] points = toPointsArray(values);
        int size = points.length;
        if (size < MIN_POINTS) {
            return Collections.emptyList();
        }
        long downAt = SystemClock.uptimeMillis();
        List<MonkeyEvent> events = new ArrayList<MonkeyEvent>(size);
        PointF p = points[index++];
        events.add(new MonkeyTouchEvent(MotionEvent.ACTION_DOWN).setDownTime(downAt).addPointer(0, p.x, p.y)
                .setIntermediateNote(false));
        PointF p1 = points[index++];
        PointF p2 = points[index++];
        events.add(new MonkeyTouchEvent(MotionEvent.ACTION_POINTER_DOWN | (1 << MotionEvent.ACTION_POINTER_INDEX_SHIFT)).
                setDownTime(downAt).addPointer(0, p1.x, p1.y).addPointer(1, p2.x, p2.y).setIntermediateNote(true));
        for (; index < size - 3; ) {
            p1 = points[index++];
            p2 = points[index++];
            events.add(new MonkeyTouchEvent(MotionEvent.ACTION_MOVE).setDownTime(downAt).addPointer(0, p1.x, p1.y)
                    .addPointer(1, p2.x, p2.y).setIntermediateNote(true));
        }
        p1 = points[index++];
        p2 = points[index++];
        events.add(new MonkeyTouchEvent(MotionEvent.ACTION_POINTER_UP | (1 << MotionEvent.ACTION_POINTER_INDEX_SHIFT))
                .setDownTime(downAt).addPointer(0, p1.x, p1.y).addPointer(1, p2.x, p2.y).setIntermediateNote(true));
        p = points[index];
        events.add(new MonkeyTouchEvent(MotionEvent.ACTION_UP).setDownTime(downAt).addPointer(0, p.x, p.y)
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
