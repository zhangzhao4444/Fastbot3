/*
 * This code is licensed under the Fastbot license. You may obtain a copy of this license in the LICENSE.txt file in the root directory of this source tree.
 */
#include "ProcessPerfMonitor.h"

#ifdef __ANDROID__

#include <android/log.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>

namespace fastbotx {

namespace {

constexpr const char *kPerfTag = "FastbotPerf";

std::once_flag s_startOnce;
std::atomic<bool> s_running{false};

bool readProcTimes(long *utime, long *stime) {
    std::ifstream stat("/proc/self/stat");
    if (!stat.is_open() || utime == nullptr || stime == nullptr) {
        return false;
    }
    std::string line;
    if (!std::getline(stat, line)) {
        return false;
    }
    const size_t rparen = line.rfind(')');
    if (rparen == std::string::npos || rparen + 2 >= line.size()) {
        return false;
    }
    const char *cursor = line.c_str() + rparen + 2;
    char state = '\0';
    long ppid = 0;
    long pgrp = 0;
    long session = 0;
    long tty_nr = 0;
    long tpgid = 0;
    unsigned long flags = 0;
    unsigned long minflt = 0;
    unsigned long cminflt = 0;
    unsigned long majflt = 0;
    unsigned long cmajflt = 0;
    if (std::sscanf(cursor,
                    "%c %ld %ld %ld %ld %ld %lu %lu %lu %lu %lu %ld %ld",
                    &state, &ppid, &pgrp, &session, &tty_nr, &tpgid, &flags,
                    &minflt, &cminflt, &majflt, &cmajflt, utime, stime) != 13) {
        return false;
    }
    return true;
}

bool readProcMemoryKb(long *rss_kb, long *vss_kb) {
    if (rss_kb == nullptr || vss_kb == nullptr) {
        return false;
    }
    *rss_kb = 0;
    *vss_kb = 0;
    std::ifstream status("/proc/self/status");
    if (!status.is_open()) {
        return false;
    }
    std::string line;
    while (std::getline(status, line)) {
        if (line.compare(0, 6, "VmRSS:") == 0) {
            std::sscanf(line.c_str(), "VmRSS: %ld kB", rss_kb);
        } else if (line.compare(0, 7, "VmSize:") == 0) {
            std::sscanf(line.c_str(), "VmSize: %ld kB", vss_kb);
        }
    }
    return *rss_kb > 0 || *vss_kb > 0;
}

void perfMonitorLoop() {
    const pid_t pid = getpid();
    const long clk_tck = sysconf(_SC_CLK_TCK);
    long prev_utime = 0;
    long prev_stime = 0;
    if (!readProcTimes(&prev_utime, &prev_stime)) {
        __android_log_print(ANDROID_LOG_WARN, kPerfTag, "failed to read initial /proc/self/stat");
        return;
    }

    while (s_running.load(std::memory_order_relaxed)) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        if (!s_running.load(std::memory_order_relaxed)) {
            break;
        }

        long utime = 0;
        long stime = 0;
        long rss_kb = 0;
        long vss_kb = 0;
        if (!readProcTimes(&utime, &stime) || !readProcMemoryKb(&rss_kb, &vss_kb)) {
            continue;
        }

        double cpu_percent = 0.0;
        if (clk_tck > 0) {
            const long delta_jiffies = (utime - prev_utime) + (stime - prev_stime);
            cpu_percent = 100.0 * static_cast<double>(delta_jiffies) / static_cast<double>(clk_tck);
        }
        prev_utime = utime;
        prev_stime = stime;

        __android_log_print(ANDROID_LOG_INFO, kPerfTag,
                            "pid=%d, cpu=%.1f%%, rss=%ldKB, vss=%ldKB",
                            static_cast<int>(pid), cpu_percent, rss_kb, vss_kb);
    }
}

} // namespace

void ProcessPerfMonitor::start() {
    std::call_once(s_startOnce, []() {
        s_running.store(true, std::memory_order_relaxed);
        std::thread(perfMonitorLoop).detach();
    });
}

} // namespace fastbotx

#else

namespace fastbotx {

void ProcessPerfMonitor::start() {
}

} // namespace fastbotx

#endif // __ANDROID__
