/*
 * This code is licensed under the Fastbot license. You may obtain a copy of this license in the LICENSE.txt file in the root directory of this source tree.
 */

package com.android.commands.monkey.provider;

import com.android.commands.monkey.utils.FileLineProvider;

import java.util.List;

/**
 * Read predefined schema from /sdcard/max.schema.
 * Delegates to {@link FileLineProvider#SCHEMA}.
 *
 * @author Zhao Zhang
 */
public class SchemaProvider {

    public static String randomNext() {
        return FileLineProvider.SCHEMA.randomNext();
    }

    public static List<String> getStrings() {
        return FileLineProvider.SCHEMA.getLines();
    }
}
