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
import android.os.SystemClock;
import android.view.IWindowManager;

import com.android.commands.monkey.events.MonkeyEvent;
import com.android.commands.monkey.framework.AndroidDevice;
import com.android.commands.monkey.utils.Logger;

/**
 * @author Zhao Zhang
 */

/**
 * monkey input method editor (IME) event
 */
public class MonkeyIMEEvent extends MonkeyEvent {
    String text = "";

    public MonkeyIMEEvent(String t) {
        super(EVENT_TYPE_IME);
        text = t;
    }

    @Override
    public int injectEvent(IWindowManager iwm, IActivityManager iam, int verbose) {
        try {
            // No reliable cross-process API for "focus on input" (getInputMethodWindowVisibleHeight
            // requires IME client; dumpsys format varies). Optimistically: wait a short time for
            // focus and soft keyboard to settle, then send text via clipboard + Ctrl+V.
            Thread.sleep(600);
            Logger.println(":Trying to send Input (" + text + ")");
            boolean sent = AndroidDevice.sendText(text);
            Logger.println(":Input (" + text + ") is successfully sent: " + sent);
            if (!sent) {
                Logger.println(":Input (" + text + ") is not sent. Please check clipboard availability.");
            }
        } catch (InterruptedException e) {
            Logger.warningPrintln("sendText Fail.");
            return MonkeyEvent.INJECT_FAIL;
        } catch (Exception e) {
            Logger.warningPrintln(e.toString());
            return MonkeyEvent.INJECT_FAIL;
        }
        return MonkeyEvent.INJECT_SUCCESS;
    }
}
