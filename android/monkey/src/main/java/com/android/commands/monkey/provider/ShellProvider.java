/*
 * This code is licensed under the Fastbot license. You may obtain a copy of this license in the LICENSE.txt file in the root directory of this source tree.
 */

package com.android.commands.monkey.provider;

import com.android.commands.monkey.utils.FileLineProvider;

/**
 * Read predefined shell from /sdcard/max.shell.
 * Delegates to {@link FileLineProvider#SHELL}.
 *
 * @author Zhao Zhang
 */
public class ShellProvider {

    public static String randomNext() {
        return FileLineProvider.SHELL.randomNext();
    }
}
