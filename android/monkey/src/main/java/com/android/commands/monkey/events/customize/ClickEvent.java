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

import org.json.JSONException;
import org.json.JSONObject;

import java.util.Arrays;
import java.util.List;

/**
 * @author Zhao Zhang
 */

public class ClickEvent extends AbstractCustomEvent {
    /**
     *
     */
    private static final long serialVersionUID = 1L;
    float x;
    float y;
    long waitTime = 1000L;

    public ClickEvent(PointF point, long waitTime) {
        this.x = point.x;
        this.y = point.y;
        this.waitTime = waitTime;
    }

    public ClickEvent(float x, float y, long waitTime) {
        this.x = x;
        this.y = y;
        this.waitTime = waitTime;
    }

    public static CustomEvent fromJSONObject(JSONObject jEvent) throws JSONException {
        long waitTime = jEvent.getLong("waitTime");
        float x = (float) jEvent.getDouble("x");
        float y = (float) jEvent.getDouble("y");
        return new ClickEvent(x, y, waitTime);
    }

    @Override
    public List<MonkeyEvent> generateMonkeyEvents() {
        long downAt = SystemClock.uptimeMillis();
        MonkeyEvent down = new MonkeyTouchEvent(MotionEvent.ACTION_DOWN).setDownTime(downAt).addPointer(0, x, y)
                .setIntermediateNote(false);
        MonkeyEvent up = new MonkeyTouchEvent(MotionEvent.ACTION_UP).setDownTime(downAt).addPointer(0, x, y)
                .setIntermediateNote(false);
        if (waitTime == 0) {
            return Arrays.asList(down, up);
        }
        return Arrays.asList(down, MonkeyThrottleEvent.obtain(waitTime), up);
    }

    public float getX() { return x; }
    public float getY() { return y; }

    /** Returns a new PointF. Prefer getX()/getY() or getPoint(PointF) to avoid allocation. */
    public PointF getPoint() {
        return new PointF(this.x, this.y);
    }

    /** Writes (x,y) into {@code reuse} and returns it. Use to avoid allocation. */
    public PointF getPoint(PointF reuse) {
        if (reuse == null) return getPoint();
        reuse.x = x;
        reuse.y = y;
        return reuse;
    }

    public void setPoint(PointF point) {
        this.x = point.x;
        this.y = point.y;
    }

    /** Set coordinates without allocating a PointF. Prefer for shield path. */
    public void setPoint(float x, float y) {
        this.x = x;
        this.y = y;
    }

    public JSONObject toJSONObject() throws JSONException {
        JSONObject jEvent = new JSONObject();
        jEvent.put("type", "c");
        jEvent.put("waitTime", String.valueOf(waitTime));
        jEvent.put("x", String.valueOf(x));
        jEvent.put("y", String.valueOf(y));
        return jEvent;
    }
}
