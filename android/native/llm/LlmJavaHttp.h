/*
 * Optional HTTP POST via JNI when libcurl is not available (e.g. monkey/Android build).
 * Image stays in Java: native passes (url, apiKey, prompt, model, maxTokens); Java builds
 * body with prompt + stored screenshot and performs POST.
 */
#ifndef FASTBOTX_LLM_JAVA_HTTP_H
#define FASTBOTX_LLM_JAVA_HTTP_H

#include <cstddef>
#include <string>

namespace fastbotx {

/**
 * Perform HTTP POST using Java's stored screenshot (no image passed from C++).
 * Java builds the request body (prompt + lastScreenshotForLlm as base64) and POSTs.
 * Called from HttpLlmClient when FASTBOTX_HAS_CURL is 0 so image never crosses JNI.
 *
 * @param url       API URL
 * @param apiKey    Bearer token (may be empty)
 * @param prompt    User prompt text
 * @param model     Model name
 * @param maxTokens Max tokens
 * @param outResponse  On success, set to response body; unchanged on failure
 * @return true if HTTP 2xx and outResponse was set; false otherwise
 */
bool llmHttpPostViaJavaWithPrompt(const char *url,
                                  const char *apiKey,
                                  const char *prompt,
                                  const char *model,
                                  int maxTokens,
                                  std::string *outResponse);

} // namespace fastbotx

#endif // FASTBOTX_LLM_JAVA_HTTP_H
