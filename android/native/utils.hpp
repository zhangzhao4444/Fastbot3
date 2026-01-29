/*
 * This code is licensed under the Fastbot license. You may obtain a copy of this license in the LICENSE.txt file in the root directory of this source tree.
 */
/**
 * @authors Jianqiang Guo, Yuhui Su
 */
#ifndef UTILS_HPP_
#define UTILS_HPP_

#ifndef _DEBUG_
#define _DEBUG_ 0
#endif
#define TAG "[FastbotNative]"

#include <string>
#include <algorithm>
#include <cstddef>

#ifdef __ANDROID__

#include <android/log.h>

#define LOGD(fmt, ...) __android_log_print(ANDROID_LOG_DEBUG,TAG ,fmt, ##__VA_ARGS__)
#define LOGI(fmt, ...) __android_log_print(ANDROID_LOG_INFO,TAG ,fmt, ##__VA_ARGS__)
#define LOGW(fmt, ...) __android_log_print(ANDROID_LOG_WARN,TAG ,fmt, ##__VA_ARGS__)
#define LOGE(fmt, ...) __android_log_print(ANDROID_LOG_ERROR,TAG ,fmt, ##__VA_ARGS__)
#define LOGF(fmt, ...) __android_log_print(ANDROID_LOG_FATAL,TAG ,fmt, ##__VA_ARGS__)
#else
#define Time_Format_Now (getTimeFormatStr().c_str())
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

#if _DEBUG_
#define BDLOG(fmt, ...)   LOGD(fmt,##__VA_ARGS__)
#define BDLOGE(fmt, ...)  LOGE(fmt,##__VA_ARGS__)
#else
#define BDLOGE(...)
#define BDLOG(...)
#endif

#define BLOG(fmt, ...)    LOGI(fmt,##__VA_ARGS__)
#define BLOGE(fmt, ...)   LOGE(fmt,##__VA_ARGS__)

// Shared constant for chunking long log lines (Android logcat truncation guard)
constexpr std::size_t FASTBOT_MAX_LOG_LEN = 3000;

// Android logcat has a limit of ~4KB per log line
// Split long strings into chunks to avoid truncation
inline void logLongStringError(const std::string& longStr) {
    // Android logcat has a limit of ~4KB per log line, but considering log prefix
    // (timestamp, tag, pid, etc.), we use a smaller chunk size to ensure complete output
    const size_t MAX_LOG_LEN = FASTBOT_MAX_LOG_LEN;
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
    // Silence unused-variable warnings in non-debug builds where BDLOGE is a no-op.
    (void)chunkNum;
    (void)totalChunks;
}

// Log long string at INFO level (for debug information like state)
inline void logLongStringInfo(const std::string& longStr) {
    // Android logcat has a limit of ~4KB per log line, but considering log prefix
    // (timestamp, tag, pid, etc.), we use a smaller chunk size to ensure complete output
    const size_t MAX_LOG_LEN = FASTBOT_MAX_LOG_LEN;
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
    // Silence unused-variable warnings in non-debug builds where BDLOG is a no-op.
    (void)chunkNum;
    (void)totalChunks;
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

#define ACTION_REFINEMENT_THRESHOLD 3
#define MAX_INITIAL_NAMES_PER_STATE_THRESHOLD 20
#define MAX_STATES_PER_ACTIVITY 10
#define MAX_GUI_TREES_PER_STATE 20
#define TRIVIAL_STATE_ACTION_THRESHOLD 5
#define TRIVIAL_STATE_WIDGET_THRESHOLD 5
#define ACTION_REFINEMENT_FIRST 1
#define MAX_EXTRA_PRIORITY_ALIASED_ACTIONS 5
#define IGNORE_EMPTY_NODE 1
#define IGNORE_OUT_OF_BOUNDS_NODE 1
#define IGNORE_INVISIBLE_NODE 1
#define ALWAYS_IGNORE_WEBVIEW 0
#define IGNORE_WEBVIEW_THRESHOLD 64
#define EXCLUDE_EMPTY_CHILD 1
#define ALWAYS_IGNORE_WEBVIEW_ACTION 0
#define PATCH_GUI_TREE 1
#define ENABLE_REPLACING_NAMELET 0

#define FASTBOT_VERSION "local build"

#endif // UTILS_HPP_

