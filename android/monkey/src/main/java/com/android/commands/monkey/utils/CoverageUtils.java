/*
 * Utilities for sampling and persisting activity coverage statistics.
 */

package com.android.commands.monkey.utils;

import org.json.JSONArray;
import org.json.JSONException;
import org.json.JSONObject;

import java.io.BufferedWriter;
import java.io.File;
import java.io.FileWriter;
import java.io.IOException;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.Set;

public final class CoverageUtils {

    private CoverageUtils() {
    }

    /**
     * Sample activity coverage at the current time and append one point into the given list.
     * Each point is a map: timestamp(string) -> coveragePercent(string).
     */
    public static void samplingActivityCoverage(Set<String> tested,
                                                Set<String> total,
                                                List<Map<String, String>> list) {
        long time = System.currentTimeMillis();
        int coveredCount = 0;
        for (String activity : tested) {
            if (total.contains(activity)) {
                coveredCount++;
            }
        }

        float coverage = 0.0f;
        int totalSize = total.size();
        if (totalSize > 0) {
            coverage = (coveredCount * 100.0f) / totalSize;
        }

        Map<String, String> point = new HashMap<>(1);
        point.put(Long.toString(time), Float.toString(coverage));
        list.add(point);
    }

    /**
     * Persist activity coverage statistics to max.activity.statistics.log under the given output dir.
     */
    public static void activityStatistics(File outputDir,
                                          String[] tested,
                                          String[] total,
                                          List<Map<String, String>> samples,
                                          float coverage,
                                          Map<String, Integer> activityTimes) {
        JSONObject root = new JSONObject();
        File outFile = new File(outputDir, "max.activity.statistics.log");
        try (BufferedWriter writer = new BufferedWriter(new FileWriter(outFile, false))) {
            JSONArray array = new JSONArray();

            // Sampling points
            for (Map<String, String> sample : samples) {
                JSONObject jsonSample = new JSONObject();
                for (Map.Entry<String, String> entry : sample.entrySet()) {
                    jsonSample.put(entry.getKey(), entry.getValue());
                }
                array.put(jsonSample);
            }
            root.put("Sampling", array);

            // Tested activities
            array = new JSONArray();
            for (String it : tested) {
                array.put(it);
            }
            root.put("TestedActivity", array);

            // Total activities
            array = new JSONArray();
            for (String it : total) {
                array.put(it);
            }
            root.put("TotalActivity", array);

            // Activity times
            array = new JSONArray();
            JSONObject jsonTimes = new JSONObject();
            for (Map.Entry<String, Integer> entry : activityTimes.entrySet()) {
                jsonTimes.put(entry.getKey(), entry.getValue());
            }
            array.put(jsonTimes);
            root.put("ActivityTimes", array);

            // Overall coverage
            root.put("Coverage", coverage);

            writer.write(root.toString());
            writer.newLine();
        } catch (IOException e) {
            Logger.println(" cannot write acitivity-statistis msg to " + outFile);
        } catch (JSONException e) {
            Logger.println(" cannot write json");
        }
    }
}

