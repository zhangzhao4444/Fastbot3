/*
 * This code is licensed under the Fastbot license. You may obtain a copy of this license in the LICENSE.txt file in the root directory of this source tree.
 */
#ifndef PROCESS_PERF_MONITOR_H_
#define PROCESS_PERF_MONITOR_H_

namespace fastbotx {

/** Periodically logs Fastbot process CPU and memory usage to logcat (tag FastbotPerf). */
class ProcessPerfMonitor {
public:
    /** Starts a detached background thread if not already running (Android only). */
    static void start();
};

} // namespace fastbotx

#endif // PROCESS_PERF_MONITOR_H_
