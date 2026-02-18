/*
 * This code is licensed under the Fastbot license. You may obtain a copy of this license in the LICENSE.txt file in the root directory of this source tree.
 */
/**
 * @authors Jianqiang Guo, Yuhui Su
 */
#ifndef UTILS_HPP_
#define UTILS_HPP_

#define _DEBUG_ 1
#define TAG "[FastbotNative]"

#include <string>
#include <algorithm>

#ifdef __ANDROID__

#include <android/log.h>

#define LOGD(fmt, ...) __android_log_print(ANDROID_LOG_DEBUG,TAG ,fmt, ##__VA_ARGS__)
#define LOGI(fmt, ...) __android_log_print(ANDROID_LOG_INFO,TAG ,fmt, ##__VA_ARGS__)
#define LOGW(fmt, ...) __android_log_print(ANDROID_LOG_WARN,TAG ,fmt, ##__VA_ARGS__)
#define LOGE(fmt, ...) __android_log_print(ANDROID_LOG_ERROR,TAG ,fmt, ##__VA_ARGS__)
#define LOGF(fmt, ...) __android_log_print(ANDROID_LOG_FATAL,TAG ,fmt, ##__VA_ARGS__)
#else
#include <ctime>
inline const char *getLogTimeStr() {
    static thread_local char buf[80];
    time_t now = time(nullptr);
    struct tm t{};
#ifdef _WIN32
    localtime_s(&t, &now);
#else
    localtime_r(&now, &t);
#endif
    strftime(buf, sizeof(buf), "%Y-%m-%d %T", &t);
    return buf;
}
#define Time_Format_Now getLogTimeStr()
#define LOGD(fmt, ...) printf(TAG "[%s] DEBUG[%s][%s][%d]:" fmt "\n", Time_Format_Now, __FILE__, __FUNCTION__, __LINE__, ##__VA_ARGS__)
#define LOGI(fmt, ...) printf(TAG "[%s] :" fmt "\n", Time_Format_Now ,##__VA_ARGS__)
#define LOGW(fmt, ...) printf(TAG "[%s] WARNING:" fmt "\n", Time_Format_Now, ##__VA_ARGS__)
#define LOGE(fmt, ...) printf(TAG "[%s] ERROR:" fmt "\n", Time_Format_Now, ##__VA_ARGS__)
#define LOGF(...)
#endif

#ifdef __ANDROID__
#define ACTIVITY_VC_STR "activity"
#else
#define ACTIVITY_VC_STR "ViewController"
#endif

//#if _DEBUG_
#define BDLOG(fmt, ...)   LOGD(fmt,##__VA_ARGS__)
#define BDLOGE(fmt, ...)  LOGE(fmt,##__VA_ARGS__)
//#else
//#define BDLOGE(...)
//#define BDLOG(...)
//#endif

#define BLOG(fmt, ...)    LOGI(fmt,##__VA_ARGS__)
#define BLOGE(fmt, ...)   LOGE(fmt,##__VA_ARGS__)

// Android logcat has a limit of ~4KB per log line
// Split long strings into chunks to avoid truncation
inline void logLongStringError(const std::string& longStr) {
    // Android logcat has a limit of ~4KB per log line, but considering log prefix
    // (timestamp, tag, pid, etc.), we use a smaller chunk size to ensure complete output
    const size_t MAX_LOG_LEN = 3000; // Reduced from 4000 to ensure no truncation
    size_t pos = 0;
    size_t totalLen = longStr.length();
    
    if (totalLen <= MAX_LOG_LEN) {
        BDLOGE("%s", longStr.c_str());
        return;
    }
    
    // Split into chunks
    size_t chunkNum = 0;
    size_t totalChunks = (totalLen + MAX_LOG_LEN - 1) / MAX_LOG_LEN;
    while (pos < totalLen) {
        size_t chunkLen = std::min(MAX_LOG_LEN, totalLen - pos);
        std::string chunk = longStr.substr(pos, chunkLen);
        BDLOGE("[chunk %zu/%zu] %s", chunkNum + 1, totalChunks, chunk.c_str());
        pos += chunkLen;
        chunkNum++;
    }
}

// Log long string at INFO level (for debug information like state)
inline void logLongStringInfo(const std::string& longStr) {
    // Android logcat has a limit of ~4KB per log line, but considering log prefix
    // (timestamp, tag, pid, etc.), we use a smaller chunk size to ensure complete output
    const size_t MAX_LOG_LEN = 3000; // Reduced from 4000 to ensure no truncation
    size_t pos = 0;
    size_t totalLen = longStr.length();
    
    if (totalLen <= MAX_LOG_LEN) {
        BDLOG("%s", longStr.c_str());
        return;
    }
    
    // Split into chunks
    size_t chunkNum = 0;
    size_t totalChunks = (totalLen + MAX_LOG_LEN - 1) / MAX_LOG_LEN;
    while (pos < totalLen) {
        size_t chunkLen = std::min(MAX_LOG_LEN, totalLen - pos);
        std::string chunk = longStr.substr(pos, chunkLen);
        BDLOG("[chunk %zu/%zu] %s", chunkNum + 1, totalChunks, chunk.c_str());
        pos += chunkLen;
        chunkNum++;
    }
}

// If should drop detail after hashing
#define DROP_DETAIL_AFTER_SATE 1

// If should generate hash based on text
#define STATE_WITH_TEXT        0

// Whether the text attribute participates in the hash generation,
// the longest length of the text participating in the abstraction,
// generally a multiple of 3. More than 2 Chinese characters will
// not participate in the abstraction, the length of the character will be truncated,
#define STATE_TEXT_MAX_LEN     (2*3)

// If should generate hash based on index
#define STATE_WITH_INDEX       0

// If should order widgets before generating hash
#define STATE_WITH_WIDGET_ORDER 0

#define STATE_MERGE_DETAIL_TEXT 1

#define BLOCK_STATE_TIME_RESTART (-1)

#define FORCE_EDITTEXT_CLICK_TRUE 1

#define PARENT_CLICK_CHANGE_CHILDREN 1

#define SCROLL_BOTTOM_UP_N_ENABLE 0

// Dynamic state abstraction (refinement/coarsening)
#define DYNAMIC_STATE_ABSTRACTION_ENABLED 1
#define RefinementCheckInterval           50
#define MaxTransitionLogSize              2000
#define MinNonDeterminismCount            2
#define BetaMaxStateGrowth                8
/// β for coarsening (APE): coarsen when one old state L′ splits into more than β new states
#define BetaMaxSplitCount                 8
// α: soft upper bound for "same state, same action" candidate widget count (paper APE; reserved for future use)
#define AlphaMaxGuiActionsPerModelAction  3
/// When true: per-round paper order — ActionRefinement(α) then StateCoarsening(β) then StateRefinement; when false: per-K-step batch (merge α + non-determinism)
#define UsePaperRefinementOrder           0
/// Skip adding Text when widgets with non-empty text > this count (avoid list/article screens exploding)
#define MaxTextWidgetCount                20
/// Skip adding Text when (widgets with non-empty text) / total widgets > this ratio (0–100, e.g. 50 = 50%)
#define MaxTextWidgetRatioPercent         50
/// Skip adding Text when unique widgets under (cur|Text) would exceed this (avoid refinement explosion)
#define MaxUniqueWidgetsAfterText         50

/// Compile-time timestamp (e.g. "Jan 30 2026 08:15:28")
#define FASTBOT_VERSION __DATE__ " " __TIME__

// Performance optimization: Control raw guitree XML logging
// Set to 1 to enable detailed line-by-line XML logging (for debugging)
// Set to 0 to disable (default) for better performance on large dumps
#ifndef FASTBOT_LOG_RAW_GUITREE
#define FASTBOT_LOG_RAW_GUITREE 0
#endif

// Performance optimization: Control xpath matching detailed logging
// Set to 1 to enable detailed xpath match logging (for debugging)
// Set to 0 to disable (default) for better performance on large dumps
#ifndef FASTBOT_LOG_XPATH_MATCH
#define FASTBOT_LOG_XPATH_MATCH 0
#endif

// Performance optimization: Control black widget point check logging
// Set to 1 to enable detailed logging for checkPointIsInBlackRects (for debugging)
// Set to 0 to disable (default) for better performance (this function is called frequently)
#ifndef FASTBOT_LOG_BLACK_RECT_CHECK
#define FASTBOT_LOG_BLACK_RECT_CHECK 0
#endif

#endif // UTILS_HPP_

