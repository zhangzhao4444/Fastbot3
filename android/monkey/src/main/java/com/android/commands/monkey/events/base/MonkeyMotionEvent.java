/*
 * Copyright (c) 2020 Bytedance Inc.
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

package com.android.commands.monkey.events.base;

import android.app.IActivityManager;
import android.graphics.Rect;
import android.hardware.input.InputManager;
import android.os.SystemClock;
import android.view.IWindowManager;
import android.view.InputDevice;
import android.view.MotionEvent;

import com.android.commands.monkey.events.MonkeyEvent;
import com.android.commands.monkey.framework.AndroidDevice;
import com.android.commands.monkey.utils.Logger;

/**
 * @author Zhao Zhang
 */

/**
 * Monkey motion event (touch / optional future mouse).
 * Performance: reuse PointerProperties[] and PointerCoords[] to avoid allocation per getEvent() (see SCRCPY_VS_FASTBOT_OPTIMIZATION_ANALYSIS §3).
 * Future mouse: for SOURCE_MOUSE on API 23+, send ACTION_DOWN then ACTION_BUTTON_PRESS; on release ACTION_BUTTON_RELEASE then ACTION_UP; use InputManager.setActionButton(MotionEvent, actionButton) via reflection.
 */
public abstract class MonkeyMotionEvent extends MonkeyEvent {

    /** Max pointers for reuse arrays (PointerProperties + PointerCoords obtain). */
    private static final int MAX_POINTERS = 10;

    private static final ThreadLocal<MotionEvent.PointerProperties[]> sPointerProperties = new ThreadLocal<MotionEvent.PointerProperties[]>() {
        @Override
        protected MotionEvent.PointerProperties[] initialValue() {
            MotionEvent.PointerProperties[] p = new MotionEvent.PointerProperties[MAX_POINTERS];
            for (int i = 0; i < MAX_POINTERS; i++) {
                MotionEvent.PointerProperties prop = new MotionEvent.PointerProperties();
                prop.id = 0;
                prop.toolType = MotionEvent.TOOL_TYPE_FINGER;
                p[i] = prop;
            }
            return p;
        }
    };

    private static final ThreadLocal<MotionEvent.PointerCoords[]> sPointerCoords = new ThreadLocal<MotionEvent.PointerCoords[]>() {
        @Override
        protected MotionEvent.PointerCoords[] initialValue() {
            MotionEvent.PointerCoords[] c = new MotionEvent.PointerCoords[MAX_POINTERS];
            for (int i = 0; i < MAX_POINTERS; i++) {
                c[i] = new MotionEvent.PointerCoords();
            }
            return c;
        }
    };

    // statusBarHeight / bottomBarHeight from AndroidDevice (cached, invalidated on rotation).

    private long mDownTime;
    private long mEventTime;
    private int mAction;
    // Per-event cached display info to avoid repeated AndroidDevice calls per pointer.
    private boolean mDisplayInfoInitialized;
    private Rect mDisplayBounds;
    private int mStatusBarHeight;
    private int mBottomBarHeight;
    private final int[] mPointerIds = new int[MAX_POINTERS];
    // Lazily allocated per pointer index to avoid always allocating MAX_POINTERS objects.
    private final MotionEvent.PointerCoords[] mPointerCoords = new MotionEvent.PointerCoords[MAX_POINTERS];
    private int mPointerCount;
    private int mMetaState;
    private float mXPrecision;
    private float mYPrecision;
    private int mDeviceId;
    private int mSource;
    private int mFlags;
    private int mEdgeFlags;
    private int type = 0;

    // If true, this is an intermediate step (more verbose logging, only)
    private boolean mIntermediateNote;

    protected MonkeyMotionEvent(int type, int source, int action) {
        super(type);
        mSource = source;
        mDownTime = -1;
        mEventTime = -1;
        mAction = action;
        mXPrecision = 1;
        mYPrecision = 1;
    }

    public MonkeyMotionEvent addPointer(int id, float x, float y) {
        return addPointer(id, x, y, 0, 0);
    }

    public MonkeyMotionEvent addPointer(int id, float x, float y, float pressure, float size) {
        if (mPointerCount >= MAX_POINTERS) return this;
        ensureDisplayInfo();
        float clippedY = y;
        if (mDisplayBounds != null) {
            if (clippedY <= mStatusBarHeight) {
                clippedY = mStatusBarHeight + 1;
            } else if (clippedY >= mBottomBarHeight) {
                if (type == 1) clippedY = mBottomBarHeight - 1;
            }
        }
        MotionEvent.PointerCoords c = mPointerCoords[mPointerCount];
        if (c == null) {
            c = new MotionEvent.PointerCoords();
            mPointerCoords[mPointerCount] = c;
        }
        c.x = x;
        c.y = clippedY;
        c.pressure = pressure;
        c.size = size;
        mPointerIds[mPointerCount] = id;
        mPointerCount++;
        return this;
    }

    public boolean getIntermediateNote() {
        return mIntermediateNote;
    }

    public MonkeyMotionEvent setIntermediateNote(boolean b) {
        mIntermediateNote = b;
        return this;
    }

    public int getAction() {
        return mAction;
    }

    public long getDownTime() {
        return mDownTime;
    }

    public MonkeyMotionEvent setDownTime(long downTime) {
        mDownTime = downTime;
        return this;
    }

    public long getEventTime() {
        return mEventTime;
    }

    public MonkeyMotionEvent setEventTime(long eventTime) {
        mEventTime = eventTime;
        return this;
    }

    public MonkeyMotionEvent setMetaState(int metaState) {
        mMetaState = metaState;
        return this;
    }

    public MonkeyMotionEvent setPrecision(float xPrecision, float yPrecision) {
        mXPrecision = xPrecision;
        mYPrecision = yPrecision;
        return this;
    }

    public MonkeyMotionEvent setDeviceId(int deviceId) {
        mDeviceId = deviceId;
        return this;
    }

    public MonkeyMotionEvent setEdgeFlags(int edgeFlags) {
        mEdgeFlags = edgeFlags;
        return this;
    }

    public MonkeyMotionEvent setType(int type) {
        this.type = type;
        return this;
    }

    private void ensureDisplayInfo() {
        if (mDisplayInfoInitialized) return;
        Rect bounds = AndroidDevice.getDisplayBounds(AndroidDevice.getFocusedDisplayId());
        int statusBarHeight = AndroidDevice.getStatusBarHeight();
        int bottomBarHeight = AndroidDevice.getBottomBarHeight();
        if (bounds != null) {
            int maxY = bounds.height() - 1;
            if (bottomBarHeight > maxY) bottomBarHeight = maxY;
        }
        mDisplayBounds = bounds;
        mStatusBarHeight = statusBarHeight;
        mBottomBarHeight = bottomBarHeight;
        mDisplayInfoInitialized = true;
    }

    /**
     * @return instance of a motion event
     * use PointerProperties + PointerCoords obtain, set toolType (FINGER/MOUSE).
     * Reuses ThreadLocal PointerProperties[] and PointerCoords[] to avoid allocation per call.
     */
    /* private */ MotionEvent getEvent() {
        int pointerCount = Math.min(mPointerCount, MAX_POINTERS);
        MotionEvent.PointerProperties[] props = sPointerProperties.get();
        MotionEvent.PointerCoords[] coords = sPointerCoords.get();
        int toolType = (mSource == InputDevice.SOURCE_MOUSE) ? MotionEvent.TOOL_TYPE_MOUSE : MotionEvent.TOOL_TYPE_FINGER;
        for (int i = 0; i < pointerCount; i++) {
            props[i].id = mPointerIds[i];
            props[i].toolType = toolType;
            MotionEvent.PointerCoords src = mPointerCoords[i];
            coords[i].x = src.x;
            coords[i].y = src.y;
            coords[i].pressure = src.pressure;
            coords[i].size = src.size;
        }
        long eventTime = mEventTime < 0 ? SystemClock.uptimeMillis() : mEventTime;
        int buttonState = 0;
        return MotionEvent.obtain(mDownTime, eventTime, mAction, pointerCount, props, coords,
                mMetaState, buttonState, mXPrecision, mYPrecision, mDeviceId, mEdgeFlags, mSource, mFlags);
    }

    @Override
    public boolean isThrottlable() {
        return (getAction() == MotionEvent.ACTION_UP);
    }

    @Override
    public int injectEvent(IWindowManager iwm, IActivityManager iam, int verbose) {
        MotionEvent me = getEvent();
        if ((verbose > 0 && !mIntermediateNote) || verbose > 1) {
            StringBuilder msg = new StringBuilder(":Sending ");
            msg.append(getTypeLabel()).append(" (");
            switch (me.getActionMasked()) {
                case MotionEvent.ACTION_DOWN:
                    msg.append("ACTION_DOWN");
                    break;
                case MotionEvent.ACTION_MOVE:
                    msg.append("ACTION_MOVE");
                    break;
                case MotionEvent.ACTION_UP:
                    msg.append("ACTION_UP");
                    break;
                case MotionEvent.ACTION_CANCEL:
                    msg.append("ACTION_CANCEL");
                    break;
                case MotionEvent.ACTION_POINTER_DOWN:
                    msg.append("ACTION_POINTER_DOWN ").append(me.getPointerId(me.getActionIndex()));
                    break;
                case MotionEvent.ACTION_POINTER_UP:
                    msg.append("ACTION_POINTER_UP ").append(me.getPointerId(me.getActionIndex()));
                    break;
                default:
                    msg.append(me.getAction());
                    break;
            }
            msg.append("):");

            int pointerCount = me.getPointerCount();
            for (int i = 0; i < pointerCount; i++) {
                msg.append(" ").append(me.getPointerId(i));
                msg.append(":(").append(me.getX(i)).append(",").append(me.getY(i)).append(")");
            }
            Logger.println(msg.toString());
        }
        try {
            // use focused display so touch goes to correct display in multi-display.
            int displayId = AndroidDevice.getFocusedDisplayId();
            if (displayId != 0 && !AndroidDevice.supportsInputEvents(displayId)) {
                return MonkeyEvent.INJECT_FAIL;
            }
            if (!AndroidDevice.setInputEventDisplayId(me, displayId)) {
                return MonkeyEvent.INJECT_FAIL;
            }
            if (!InputManager.getInstance().injectInputEvent(me,
                    InputManager.INJECT_INPUT_EVENT_MODE_WAIT_FOR_RESULT)) {
                return MonkeyEvent.INJECT_FAIL;
            }
        } catch (SecurityException e) {
            return MonkeyEvent.INJECT_FAIL;
        } finally {
            me.recycle();
        }
        return MonkeyEvent.INJECT_SUCCESS;
    }

    protected abstract String getTypeLabel();
}
