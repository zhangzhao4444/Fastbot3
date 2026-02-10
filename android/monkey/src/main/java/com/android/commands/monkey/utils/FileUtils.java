/*
 * Simple file and directory utilities used across Monkey.
 */

package com.android.commands.monkey.utils;

import java.io.File;
import java.io.FileWriter;
import java.io.IOException;

public final class FileUtils {

    private FileUtils() {
    }

    /**
     * Ensure the directory exists (mkdirs if needed).
     *
     * @return true if the directory exists after this call.
     */
    public static boolean ensureDir(File dir) {
        if (!dir.exists()) {
            dir.mkdirs();
        }
        return dir.exists();
    }

    /**
     * Write a string to the given path.
     *
     * @return true on success, false otherwise.
     */
    public static boolean writeStringToFile(String path, String data, boolean isAppend) {
        return writeStringToFile(new File(path), data, isAppend);
    }

    /**
     * Write a string to the given file.
     *
     * @return true on success, false otherwise.
     */
    public static boolean writeStringToFile(File file, String data, boolean isAppend) {
        try (FileWriter writer = new FileWriter(file, isAppend)) {
            writer.write(data);
            return true;
        } catch (IOException e) {
            Logger.println("cannot write to " + file.getAbsolutePath() + ": " + e.getMessage());
            return false;
        }
    }
}

