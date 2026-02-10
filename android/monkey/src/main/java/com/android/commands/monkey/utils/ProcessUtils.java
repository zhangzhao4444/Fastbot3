/*
 * Utility methods for working with external processes.
 */

package com.android.commands.monkey.utils;

import java.io.BufferedReader;
import java.io.IOException;
import java.io.InputStreamReader;

public final class ProcessUtils {

    private ProcessUtils() {
    }

    /**
     * Execute the given command and return its stdout/stderr output as a trimmed string.
     * stderr is redirected into stdout (per ProcessBuilder.redirectErrorStream(true)).
     */
    public static String getProcessOutput(String[] cmd) throws IOException, InterruptedException {
        ProcessBuilder processBuilder = new ProcessBuilder(cmd);
        processBuilder.redirectErrorStream(true);
        Process process = processBuilder.start();
        StringBuilder processOutput = new StringBuilder();
        try (BufferedReader reader = new BufferedReader(
                new InputStreamReader(process.getInputStream()))) {
            String readLine;
            while ((readLine = reader.readLine()) != null) {
                processOutput.append(readLine).append(System.lineSeparator());
            }
            process.waitFor();
        }
        return processOutput.toString().trim();
    }

    /**
     * Execute shell command (single string). WARNING: on many systems this is passed to the
     * system shell; do not pass user-controlled or untrusted input. Prefer {@link #executeShellCommand(String[])}.
     *
     * @param command shell command to execute
     * @return output of the execution result.
     */
    public static String executeShellCommand(String command) {
        StringBuilder sb = new StringBuilder();
        Process proc = null;
        try {
            proc = Runtime.getRuntime().exec(command);
            try (BufferedReader br = new BufferedReader(new InputStreamReader(proc.getInputStream()))) {
                String line;
                while ((line = br.readLine()) != null) {
                    sb.append(line).append('\n');
                }
            }
        } catch (Exception e) {
            Logger.errorPrintln("executeShellCommand() error! command: " + command);
            Logger.errorPrintln(e.getMessage());
        } finally {
            if (proc != null) {
                proc.destroy();
            }
        }
        return sb.toString();
    }

    /**
     * Execute command with explicit argument array (no shell parsing). Safe for untrusted arguments
     * when the program name is trusted.
     *
     * @param cmdarray program and arguments, e.g. {"ime", "set", "com.pkg/.IME"}
     * @return output of the execution result, or empty string on failure.
     */
    public static String executeShellCommand(String[] cmdarray) {
        if (cmdarray == null || cmdarray.length == 0) {
            return "";
        }
        StringBuilder sb = new StringBuilder();
        Process proc = null;
        try {
            proc = Runtime.getRuntime().exec(cmdarray);
            try (BufferedReader br = new BufferedReader(new InputStreamReader(proc.getInputStream()))) {
                String line;
                while ((line = br.readLine()) != null) {
                    sb.append(line).append('\n');
                }
            }
        } catch (Exception e) {
            Logger.errorPrintln("executeShellCommand(String[]) error: " + e.getMessage());
        } finally {
            if (proc != null) {
                proc.destroy();
            }
        }
        return sb.toString();
    }
}

