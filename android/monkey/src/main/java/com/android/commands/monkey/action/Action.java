/*
 * Copyright 2020 Advanced Software Technologies Lab at ETH Zurich, Switzerland
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

package com.android.commands.monkey.action;

import com.android.commands.monkey.fastbot.client.ActionType;
import com.android.commands.monkey.utils.Config;

/**
 * @author Zhao Zhang
 */

/**
 * To keep logging simple, we make everything that is worth logging into an action.
 */
public class Action {

    public static final Action NOP = new Action(ActionType.NOP);

    static {
        NOP.setThrottle(Config.getInteger("max.nopActionThrottle", 1000));
    }

    private final ActionType type;
    private int throttle;

    public Action(ActionType type) {
        this.type = type;
    }

    public ActionType getType() {
        return type;
    }

    public boolean isModelAction() {
        return type.isModelAction();
    }

    public boolean requireTarget() {
        return type.requireTarget();
    }

    /**
     * Wait interval after the action is performed.
     *
     * @return Time to wait
     */
    public int getThrottle() {
        return throttle;
    }

    /**
     * Set the wait interval, after which the action is performed.
     * @param throttle Time to wait
     */
    public void setThrottle(int throttle) {
        this.throttle = throttle;
    }

    @Override
    public String toString() {
        if (!isModelAction()) {
            return type.toString();
        }
        return super.toString() + '@' + type.toString(); // + String.format("[V=%f]", value);
    }

}
