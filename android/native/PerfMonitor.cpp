/*
 * This code is licensed under the Fastbot license. You may obtain a copy of this license in the LICENSE.txt file in the root directory of this source tree.
 */
#include "PerfMonitor.h"

#include <chrono>
#include <mutex>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>
#include <unistd.h>

#ifdef __ANDROID__
#include <android/log.h>
#endif

namespace fastbotx {
namespace {

constexpr const char *kPerfLogTag = "FastbotPerf";

#ifdef __ANDROID__
#define PERF_LOGI(fmt, ...) __android_log_print(ANDROID_LOG_INFO, kPerfLogTag, fmt, ##__VA_ARGS__)
#else
#define PERF_LOGI(fmt, ...) printf("%s: " fmt "\n", kPerfLogTag, ##__VA_ARGS__)
#endif

std::once_flag g_perfMonitorOnce;

bool readProcCpuTicks(long &utime, long &stime) {
    FILE *f = std::fopen("/proc/self/stat", "r");
    if (!f) {
        return false;
    }
    char buf[1024];
    if (!std::fgets(buf, sizeof(buf), f)) {
        std::fclose(f);
        return false;
    }
    std::fclose(f);

    const char *rparen = std::strrchr(buf, ')');
    if (!rparen) {
        return false;
    }
    // Fields after comm: state ppid ... utime(14) stime(15) per proc(5).
    const int matched = std::sscanf(rparen + 2,
                                    " %*c %*d %*d %*d %*d %*d %*lu %*lu %*lu %*lu %*lu %*lu %ld %ld",
                                    &utime, &stime);
    return matched == 2;
}

bool readProcMemoryKb(long &rssKb, long &vssKb) {
    rssKb = 0;
    vssKb = 0;
    FILE *f = std::fopen("/proc/self/status", "r");
    if (!f) {
        return false;
    }
    char line[256];
    bool gotRss = false;
    bool gotVss = false;
    while (std::fgets(line, sizeof(line), f)) {
        if (std::strncmp(line, "VmRSS:", 6) == 0) {
            std::sscanf(line + 6, "%ld", &rssKb);
            gotRss = true;
        } else if (std::strncmp(line, "VmSize:", 7) == 0) {
            std::sscanf(line + 7, "%ld", &vssKb);
            gotVss = true;
        }
        if (gotRss && gotVss) {
            break;
        }
    }
    std::fclose(f);
    return gotRss && gotVss;
}

void perfMonitorLoop() {
    const long clkTck = sysconf(_SC_CLK_TCK);
    if (clkTck <= 0) {
        PERF_LOGI("perf monitor disabled: invalid CLK_TCK");
        return;
    }

    const pid_t pid = getpid();
    long prevUtime = 0;
    long prevStime = 0;
    bool hasPrevCpu = false;
    auto prevWall = std::chrono::steady_clock::now();

    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(1));

        long rssKb = 0;
        long vssKb = 0;
        readProcMemoryKb(rssKb, vssKb);

        double cpuPct = 0.0;
        long utime = 0;
        long stime = 0;
        if (readProcCpuTicks(utime, stime)) {
            const auto nowWall = std::chrono::steady_clock::now();
            const double elapsedSec =
                    std::chrono::duration<double>(nowWall - prevWall).count();
            if (hasPrevCpu && elapsedSec > 0.0) {
                const long deltaTicks = (utime - prevUtime) + (stime - prevStime);
                cpuPct = 100.0 * static_cast<double>(deltaTicks) /
                         (static_cast<double>(clkTck) * elapsedSec);
            }
            prevUtime = utime;
            prevStime = stime;
            prevWall = nowWall;
            hasPrevCpu = true;
        }

        PERF_LOGI("pid=%d, cpu=%.1f%%, rss=%ldKB, vss=%ldKB",
                  static_cast<int>(pid), cpuPct, rssKb, vssKb);
    }
}

} // namespace

void startPerfMonitor() {
    std::call_once(g_perfMonitorOnce, []() {
        std::thread(perfMonitorLoop).detach();
    });
}

} // namespace fastbotx
