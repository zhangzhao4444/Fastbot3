/*
 * This code is licensed under the Fastbot license. You may obtain a copy of this license in the LICENSE.txt file in the root directory of this source tree.
 */

package com.android.commands.monkey.framework;

import android.content.Intent;
import android.content.IntentFilter;
import android.content.pm.PackageManager;
import android.content.pm.ResolveInfo;
import android.net.Uri;
import android.os.Build;
import android.os.PatternMatcher;

import com.android.commands.monkey.utils.Logger;

import java.lang.reflect.Field;

import static com.android.commands.monkey.utils.Config.debug;
import java.util.ArrayList;
import java.util.HashSet;
import java.util.List;
import java.util.Set;

/**
 * Device-side deep link discovery via reflection of GET_RESOLVED_FILTER +
 * queryIntentActivities(ACTION_VIEW).setPackage(pkg). See Delm_survey.md §4.3.1.
 */
public final class DeepLinkHelper {

    private static final int FALLBACK_GET_RESOLVED_FILTER = 0x40;
    private static Boolean sResolvedFilterAvailable = null;
    private static int sResolvedFilterFlag = 0;

    private DeepLinkHelper() {
    }

    /**
     * Get GET_RESOLVED_FILTER flag via reflection (or fallback 0x40). Returns 0 if unavailable.
     */
    public static int getResolvedFilterFlag() {
        if (sResolvedFilterAvailable != null) {
            return sResolvedFilterFlag;
        }
        try {
            Field f = PackageManager.class.getField("GET_RESOLVED_FILTER");
            sResolvedFilterFlag = f.getInt(null);
            sResolvedFilterAvailable = true;
            if (debug) {
                Logger.println("// DeepLinkHelper: GET_RESOLVED_FILTER = 0x" + Integer.toHexString(sResolvedFilterFlag));
            }
        } catch (Throwable t) {
            sResolvedFilterAvailable = false;
            sResolvedFilterFlag = 0;
            try {
                Field f = PackageManager.class.getDeclaredField("GET_RESOLVED_FILTER");
                f.setAccessible(true);
                sResolvedFilterFlag = f.getInt(null);
                sResolvedFilterAvailable = true;
            } catch (Throwable t2) {
                if (debug) {
                    Logger.println("// DeepLinkHelper: GET_RESOLVED_FILTER not available, using fallback 0x40");
                }
                sResolvedFilterFlag = FALLBACK_GET_RESOLVED_FILTER;
                sResolvedFilterAvailable = true;
            }
        }
        return sResolvedFilterFlag;
    }

    /**
     * MATCH_ALL for package visibility (API 23+). Use 0 if not available.
     */
    private static int getMatchAllFlag() {
        if (Build.VERSION.SDK_INT < 23) {
            return 0;
        }
        try {
            Field f = PackageManager.class.getField("MATCH_ALL");
            return f.getInt(null);
        } catch (Throwable t) {
            try {
                Field f = PackageManager.class.getDeclaredField("MATCH_ALL");
                f.setAccessible(true);
                return f.getInt(null);
            } catch (Throwable t2) {
                return 0;
            }
        }
    }

    /**
     * Collect deep-link URIs for the given package only. Uses GET_RESOLVED_FILTER +
     * queryIntentActivities(ACTION_VIEW).setPackage(pkg). Only ResolveInfo from that package are used.
     */
    public static List<String> collectDeepLinkUris(PackageManager pm, String packageName) {
        List<String> uris = new ArrayList<>();
        if (pm == null || packageName == null || packageName.isEmpty()) {
            Logger.println("// [deep link] collectDeepLinkUris: skip (pm or packageName null/empty), pkg=" + packageName);
            return uris;
        }
        int flag = getResolvedFilterFlag();
        if (flag == 0) {
            Logger.println("// [deep link] collectDeepLinkUris: skip (GET_RESOLVED_FILTER=0), pkg=" + packageName);
            return uris;
        }

        Intent intent = new Intent(Intent.ACTION_VIEW);
        intent.setPackage(packageName);
        List<ResolveInfo> list;
        try {
            list = pm.queryIntentActivities(intent, flag | getMatchAllFlag());
        } catch (Throwable t) {
            Logger.println("// [deep link] collectDeepLinkUris: queryIntentActivities failed pkg=" + packageName + " err=" + t.getMessage());
            return uris;
        }
        if (list == null) {
            Logger.println("// [deep link] collectDeepLinkUris: query returned null, pkg=" + packageName);
            return uris;
        }

        int samePkg = 0, withFilter = 0;
        Set<String> seen = new HashSet<>();
        for (ResolveInfo ri : list) {
            if (ri.activityInfo == null || !packageName.equals(ri.activityInfo.packageName)) continue;
            samePkg++;
            IntentFilter filter = ri.filter;
            if (filter == null) continue;
            withFilter++;
            for (String u : urisFromIntentFilter(filter)) {
                if (u != null && !u.isEmpty() && seen.add(u)) uris.add(u);
            }
        }
        Logger.println("// [deep link] collectDeepLinkUris: pkg=" + packageName + " resolveCount=" + list.size() + " samePkg=" + samePkg + " withFilter=" + withFilter + " uris=" + uris.size());
        return uris;
    }

    /**
     * Build URI strings from one IntentFilter (scheme, authority, path).
     */
    private static List<String> urisFromIntentFilter(IntentFilter filter) {
        List<String> out = new ArrayList<>();
        int schemeCount = filter.countDataSchemes();
        int authorityCount = filter.countDataAuthorities();
        int pathCount = filter.countDataPaths();
        // [deep link] temporary: log why filter yields 0 uris
        Logger.println("// [deep link] urisFromIntentFilter: schemeCount=" + schemeCount + " authorityCount=" + authorityCount + " pathCount=" + pathCount);
        if (schemeCount == 0) {
            return out;
        }

        for (int s = 0; s < schemeCount; s++) {
            String scheme = filter.getDataScheme(s);
            if (scheme == null || scheme.isEmpty()) {
                Logger.println("// [deep link] urisFromIntentFilter: scheme[" + s + "] is null or empty");
                continue;
            }
            if (authorityCount == 0) {
                out.add(scheme + "://");
                continue;
            }
            for (int a = 0; a < authorityCount; a++) {
                IntentFilter.AuthorityEntry auth = filter.getDataAuthority(a);
                if (auth == null) {
                    continue;
                }
                String host = auth.getHost();
                int port = auth.getPort();
                String hostPart = host != null ? host : "";
                if (port > 0) {
                    hostPart = hostPart + ":" + port;
                }
                String base = scheme + "://" + hostPart;
                if (pathCount == 0) {
                    out.add(base + "/");
                    continue;
                }
                for (int p = 0; p < pathCount; p++) {
                    PatternMatcher pm = filter.getDataPath(p);
                    String path = pm != null ? pm.getPath() : null;
                    if (path != null && !path.startsWith("/")) {
                        path = "/" + path;
                    }
                    out.add(base + (path != null ? path : "/"));
                }
            }
        }
        return out;
    }

    /**
     * Build an Intent for launching a deep link (ACTION_VIEW + data URI + package).
     */
    public static Intent newDeepLinkIntent(String packageName, String uriString) {
        Intent intent = new Intent(Intent.ACTION_VIEW);
        intent.setData(Uri.parse(uriString));
        intent.setPackage(packageName);
        intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
        return intent;
    }
}
