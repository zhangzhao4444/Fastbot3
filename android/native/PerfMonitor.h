/*
 * This code is licensed under the Fastbot license. You may obtain a copy of this license in the LICENSE.txt file in the root directory of this source tree.
 */
#ifndef PERF_MONITOR_H_
#define PERF_MONITOR_H_

namespace fastbotx {

/** Starts a background thread that logs process CPU/RSS/VSS every second (logcat tag FastbotPerf). */
void startPerfMonitor();

} // namespace fastbotx

#endif // PERF_MONITOR_H_
