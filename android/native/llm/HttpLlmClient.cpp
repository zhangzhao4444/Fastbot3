/*
 * HTTP-based LLM client using an OpenAI-compatible /chat/completions API.
 *
 * NOTE: This implementation assumes that libcurl (or an equivalent HTTP stack)
 * is available and linked. If not, doHttpPost will simply return false.
 */
 /**
 * @authors Zhao Zhang
 */

#include "HttpLlmClient.h"
#include "LlmJavaHttp.h"

#include <sstream>
#include <cstdint>
#include <chrono>
#include <thread>

#include "../utils.hpp"
#include "../thirdpart/json/json.hpp"

// Attempt to include libcurl; if not available, we will compile but always fail at runtime.
#ifdef __has_include
#  if __has_include(<curl/curl.h>)
#    define FASTBOTX_HAS_CURL 1
#    include <curl/curl.h>
#  else
#    define FASTBOTX_HAS_CURL 0
#  endif
#else
#  define FASTBOTX_HAS_CURL 0
#endif

namespace fastbotx {

    namespace {
        size_t writeCallback(char *ptr, size_t size, size_t nmemb, void *userdata) {
            auto *out = static_cast<std::string *>(userdata);
            size_t total = size * nmemb;
            out->append(ptr, total);
            return total;
        }

        /** Base64 encode binary data for OpenAI image_url (data:image/png;base64,...). */
        std::string base64Encode(const char *data, size_t len) {
            static const char kAlphabet[] =
                "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
            std::string out;
            out.reserve(((len + 2) / 3) * 4);
            for (size_t i = 0; i < len; i += 3) {
                uint32_t v = static_cast<unsigned char>(data[i]) << 16;
                if (i + 1 < len) v |= static_cast<unsigned char>(data[i + 1]) << 8;
                if (i + 2 < len) v |= static_cast<unsigned char>(data[i + 2]);
                out.push_back(kAlphabet[(v >> 18) & 63]);
                out.push_back(kAlphabet[(v >> 12) & 63]);
                out.push_back(i + 1 < len ? kAlphabet[(v >> 6) & 63] : '=');
                out.push_back(i + 2 < len ? kAlphabet[v & 63] : '=');
            }
            return out;
        }
    }

    HttpLlmClient::HttpLlmClient(const LlmRuntimeConfig &config)
        : _config(config) {
    }

    bool HttpLlmClient::isEnabled() const {
        return _config.enabled && !_config.apiUrl.empty() && !_config.apiKey.empty() && !_config.model.empty();
    }

    std::string HttpLlmClient::buildRequestBody(const std::string &prompt,
                                                const std::vector<ImageData> &images) const {
        using nlohmann::json;
        json body;
        body["model"] = _config.model;
        body["max_tokens"] = _config.maxTokens;
        body["stream"] = false;

        json systemMsg;
        systemMsg["role"] = "system";
        systemMsg["content"] = "You are a GUI testing agent that must respond with a strict JSON action object.";

        json userMsg;
        userMsg["role"] = "user";

        // Multimodal: build content array [text, image_url, ...] when we have image bytes.
        bool hasValidImage = false;
        for (const auto &img : images) {
            if (!img.bytes.empty()) {
                hasValidImage = true;
                break;
            }
        }

        if (hasValidImage) {
            json contentArr = json::array();
            // Text part first
            json textPart;
            textPart["type"] = "text";
            textPart["text"] = prompt;
            contentArr.push_back(textPart);
            // Then each image as data:image/png;base64,<data>
            for (const auto &img : images) {
                if (img.bytes.empty()) {
                    continue;
                }
                std::string b64 = base64Encode(img.bytes.data(), img.bytes.size());
                if (b64.empty()) {
                    BDLOGE("HttpLlmClient: base64 encode failed for image of size %zu", img.bytes.size());
                    continue;
                }
                json imagePart;
                imagePart["type"] = "image_url";
                json imageUrl;
                imageUrl["url"] = "data:image/png;base64," + b64;
                imagePart["image_url"] = imageUrl;
                contentArr.push_back(imagePart);
            }
            // If no image was actually added (e.g. encode failed), fall back to text-only
            if (contentArr.size() > 1) {
                userMsg["content"] = std::move(contentArr);
            } else {
                userMsg["content"] = prompt;
            }
        } else {
            userMsg["content"] = prompt;
        }

        body["messages"] = json::array({systemMsg, userMsg});

        return body.dump();
    }

#if FASTBOTX_HAS_CURL
    namespace {
        /** Max number of HTTP attempts (first try + retries). */
        const int kMaxHttpAttempts = 3;
        /** Delay in ms between retries. */
        const int kRetryDelayMs = 500;

        /** Returns true if the failure is worth retrying (transient/network/timeout/server error). */
        bool shouldRetry(CURLcode curlCode, long httpCode) {
            if (curlCode != CURLE_OK) {
                return (curlCode == CURLE_OPERATION_TIMEDOUT || curlCode == CURLE_COULDNT_CONNECT ||
                        curlCode == CURLE_RECV_ERROR || curlCode == CURLE_SEND_ERROR ||
                        curlCode == CURLE_SSL_CONNECT_ERROR || curlCode == CURLE_GOT_NOTHING);
            }
            if (httpCode >= 500 && httpCode < 600) {
                return true; // Server error: retry
            }
            if (httpCode == 429) {
                return true; // Rate limit: retry
            }
            return false;
        }
    }
#endif

    bool HttpLlmClient::doHttpPost(const std::string &payload, std::string &outResponse) const {
#if FASTBOTX_HAS_CURL
        CURLcode res = CURLE_OK;
        long httpCode = 0;

        for (int attempt = 1; attempt <= kMaxHttpAttempts; ++attempt) {
            outResponse.clear();

            CURL *curl = curl_easy_init();
            if (!curl) {
                BLOGE("HttpLlmClient: failed to init curl (attempt %d/%d)", attempt, kMaxHttpAttempts);
                if (attempt < kMaxHttpAttempts) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(kRetryDelayMs));
                }
                continue;
            }

            struct curl_slist *headers = nullptr;
            std::string authHeader = "Authorization: Bearer " + _config.apiKey;
            headers = curl_slist_append(headers, "Content-Type: application/json");
            headers = curl_slist_append(headers, authHeader.c_str());

            curl_easy_setopt(curl, CURLOPT_URL, _config.apiUrl.c_str());
            curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
            curl_easy_setopt(curl, CURLOPT_POST, 1L);
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload.c_str());
            curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(payload.size()));
            curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCallback);
            curl_easy_setopt(curl, CURLOPT_WRITEDATA, &outResponse);

            if (_config.timeoutMs > 0) {
                curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, _config.timeoutMs);
            }

            res = curl_easy_perform(curl);
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);

            curl_slist_free_all(headers);
            curl_easy_cleanup(curl);

            if (res != CURLE_OK) {
                BLOGE("HttpLlmClient: curl error (attempt %d/%d): %s", attempt, kMaxHttpAttempts, curl_easy_strerror(res));
                if (attempt < kMaxHttpAttempts && shouldRetry(res, httpCode)) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(kRetryDelayMs));
                    continue;
                }
                return false;
            }

            if (httpCode >= 200 && httpCode < 300) {
                return true;
            }

            BLOGE("HttpLlmClient: HTTP %ld (attempt %d/%d)", httpCode, attempt, kMaxHttpAttempts);
            if (attempt < kMaxHttpAttempts && shouldRetry(res, httpCode)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(kRetryDelayMs));
                continue;
            }
            return false;
        }

        return false;
#else
        // Java path uses llmHttpPostViaJavaWithPrompt from predict(); doHttpPost is not used when !CURL.
        (void) payload;
        (void) outResponse;
        return false;
#endif
    }

    bool HttpLlmClient::predict(const std::string &prompt,
                                const std::vector<ImageData> &images,
                                std::string &outResponse) {
        if (!isEnabled()) {
            BLOGE("HttpLlmClient: LLM is disabled or misconfigured");
            return false;
        }

        outResponse.clear();
#if FASTBOTX_HAS_CURL
        std::string payload = buildRequestBody(prompt, images);
        bool ok = doHttpPost(payload, outResponse);
#else
        // Image stays in Java: only pass prompt; Java builds body with stored screenshot and POSTs.
        bool ok = llmHttpPostViaJavaWithPrompt(_config.apiUrl.c_str(),
                                              _config.apiKey.c_str(),
                                              prompt.c_str(),
                                              _config.model.c_str(),
                                              _config.maxTokens,
                                              &outResponse);
        if (!ok) {
            BLOGE("HttpLlmClient: Java HTTP POST failed (runner not registered, Java returned null, or network/API error; check LLM Java HTTP logs)");
            return false;
        }
#endif
        if (!ok) {
#if FASTBOTX_HAS_CURL
            BLOGE("HttpLlmClient: predict failed (curl HTTP error or non-2xx)");
#endif
            return false;
        }

        // Validate and extract the assistant message content from OpenAI-compatible response:
        // {
        //   "choices": [
        //     {
        //       "message": { "role": "assistant", "content": "..." }
        //     }
        //   ],
        //   "error": { ... }  // optional, presence means failure
        // }
        try {
            using nlohmann::json;
            if (outResponse.empty()) {
                BDLOGE("HttpLlmClient: response body is empty");
                return false;
            }
            json j = json::parse(outResponse);
            if (!j.is_object()) {
                BDLOGE("HttpLlmClient: response root is not a JSON object");
                return false;
            }
            if (j.contains("error") && j["error"].is_object()) {
                std::string errMsg = j["error"].value("message", "unknown error");
                BDLOGE("HttpLlmClient: API error: %s", errMsg.c_str());
                return false;
            }
            if (!j.contains("choices") || !j["choices"].is_array() || j["choices"].empty()) {
                BDLOGE("HttpLlmClient: response missing or empty 'choices' array");
                return false;
            }
            const auto &choice = j["choices"][0];
            if (!choice.contains("message") || !choice["message"].is_object()) {
                BDLOGE("HttpLlmClient: response choices[0] missing 'message' object");
                return false;
            }
            const auto &msg = choice["message"];
            std::string content = msg.value("content", "");
            if (content.empty()) {
                BDLOGE("HttpLlmClient: assistant message content is empty");
                return false;
            }
            outResponse = std::move(content);
            return true;
        } catch (const std::exception &ex) {
            BDLOGE("HttpLlmClient: invalid response JSON: %s", ex.what());
            return false;
        }
    }

    bool HttpLlmClient::predictWithPayload(const std::string &promptType,
                                            const std::string &payloadJson,
                                            const std::vector<ImageData> &images,
                                            std::string &outResponse) {
        (void) images; // Java path: image captured in Java on demand
        if (!isEnabled()) {
            BLOGE("HttpLlmClient: LLM is disabled or misconfigured");
            return false;
        }
        outResponse.clear();
#if FASTBOTX_HAS_CURL
        (void) promptType;
        (void) payloadJson;
        BLOGE("HttpLlmClient: predictWithPayload not implemented for CURL path; use prompt assembly in Java only");
        return false;
#else
        bool ok = llmHttpPostViaJavaWithPayload(_config.apiUrl.c_str(),
                                                _config.apiKey.c_str(),
                                                promptType.c_str(),
                                                payloadJson.c_str(),
                                                _config.model.c_str(),
                                                _config.maxTokens,
                                                &outResponse);
        if (!ok) {
            BLOGE("HttpLlmClient: Java HTTP POST (payload) failed");
            return false;
        }
        try {
            using nlohmann::json;
            if (outResponse.empty()) {
                BDLOGE("HttpLlmClient: response body is empty");
                return false;
            }
            json j = json::parse(outResponse);
            if (!j.is_object()) {
                BDLOGE("HttpLlmClient: response root is not a JSON object");
                return false;
            }
            if (j.contains("error") && j["error"].is_object()) {
                std::string errMsg = j["error"].value("message", "unknown error");
                BDLOGE("HttpLlmClient: API error: %s", errMsg.c_str());
                return false;
            }
            if (!j.contains("choices") || !j["choices"].is_array() || j["choices"].empty()) {
                BDLOGE("HttpLlmClient: response missing or empty 'choices' array");
                return false;
            }
            const auto &choice = j["choices"][0];
            if (!choice.contains("message") || !choice["message"].is_object()) {
                BDLOGE("HttpLlmClient: response choices[0] missing 'message' object");
                return false;
            }
            std::string content = choice["message"].value("content", "");
            if (content.empty()) {
                BDLOGE("HttpLlmClient: assistant message content is empty");
                return false;
            }
            outResponse = std::move(content);
            return true;
        } catch (const std::exception &ex) {
            BDLOGE("HttpLlmClient: invalid response JSON: %s", ex.what());
            return false;
        }
#endif
    }

} // namespace fastbotx

