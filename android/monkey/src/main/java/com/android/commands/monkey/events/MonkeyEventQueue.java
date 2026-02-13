/*
 * Copyright 2007, The Android Open Source Project
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

package com.android.commands.monkey.events;

import com.android.commands.monkey.events.base.MonkeyThrottleEvent;

import java.util.ArrayDeque;
import java.util.Deque;
import java.util.List;
import java.util.Random;

/**
 * Queue for monkey events. Uses ArrayDeque for efficient head remove / tail add.
 * When throttle &gt; 0, a Throttle event is appended after each throttlable event.
 */
public class MonkeyEventQueue {

    private final Deque<MonkeyEvent> mQueue = new ArrayDeque<>();
    private final Random mRandom;
    private final long mThrottle;
    private final boolean mRandomizeThrottle;

    public MonkeyEventQueue(Random random, long throttle, boolean randomizeThrottle) {
        mRandom = random;
        mThrottle = throttle;
        mRandomizeThrottle = randomizeThrottle;
    }

    /**
     * Appends an event. If throttle &gt; 0 and the event is throttlable, a Throttle event is also appended.
     */
    public void addLast(MonkeyEvent e) {
        mQueue.addLast(e);
        if (mThrottle > 0 && e.isThrottlable()) {
            long throttle = mThrottle;
            if (mRandomizeThrottle) {
                throttle = mRandom.nextLong();
                if (throttle < 0) throttle = -throttle;
                throttle %= mThrottle;
                ++throttle;
            }
            mQueue.addLast(MonkeyThrottleEvent.obtain(throttle));
        }
    }

    /** Appends an event (same as addLast). Exists for compatibility with callers using add(). */
    public void add(MonkeyEvent e) {
        addLast(e);
    }

    /**
     * Appends all events. When throttle &lt;= 0, appends directly without inserting Throttle events (faster).
     */
    public void addLastAll(List<MonkeyEvent> events) {
        if (events == null) return;
        if (mThrottle <= 0) {
            for (int i = 0; i < events.size(); i++) {
                mQueue.addLast(events.get(i));
            }
        } else {
            for (int i = 0; i < events.size(); i++) {
                addLast(events.get(i));
            }
        }
    }

    public boolean isEmpty() {
        return mQueue.isEmpty();
    }

    public MonkeyEvent removeFirst() {
        return mQueue.removeFirst();
    }

    public MonkeyEvent getFirst() {
        return mQueue.getFirst();
    }
}
