/*
 * This code is licensed under the Fastbot license. You may obtain a copy of this license in the LICENSE.txt file in the root directory of this source tree.
 */

package com.android.commands.monkey.utils;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileInputStream;
import java.io.IOException;
import java.io.InputStreamReader;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Collections;
import java.util.List;
import java.util.concurrent.ThreadLocalRandom;

/**
 * Reads lines from a file and provides random line selection.
 * Used for schema and shell command lists (e.g. /sdcard/max.schema, /sdcard/max.shell).
 *
 * @author Zhao Zhang
 */
public final class FileLineProvider {

    /** Provider for /sdcard/max.schema (predefined schema list). */
    public static final FileLineProvider SCHEMA = new FileLineProvider("/sdcard/max.schema");

    /** Provider for /sdcard/max.shell (predefined shell command list). */
    public static final FileLineProvider SHELL = new FileLineProvider("/sdcard/max.shell");

    private final List<String> lines;

    public FileLineProvider(String path) {
        this.lines = loadLines(new File(path));
    }

    private static List<String> loadLines(File file) {
        List<String> result = new ArrayList<>();
        if (!file.exists()) {
            return result;
        }
        try (BufferedReader br = new BufferedReader(
                new InputStreamReader(new FileInputStream(file), StandardCharsets.UTF_8))) {
            String line;
            while ((line = br.readLine()) != null) {
                result.add(line);
            }
        } catch (IOException e) {
            throw new RuntimeException("Failed to load file: " + file, e);
        }
        return result;
    }

    /**
     * Returns a random line, or empty string if no lines are loaded.
     */
    public String randomNext() {
        if (lines.isEmpty()) {
            return "";
        }
        int i = ThreadLocalRandom.current().nextInt(lines.size());
        return lines.get(i);
    }

    /**
     * Returns an unmodifiable view of all loaded lines.
     */
    public List<String> getLines() {
        return Collections.unmodifiableList(lines);
    }
}
