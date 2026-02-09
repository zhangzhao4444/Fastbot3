/*
 * Simple HTTP-based LLM client using an OpenAI-compatible /chat/completions API.
 * The actual network implementation is placed in the corresponding .cpp file.
 */
 /**
 * @authors Zhao Zhang
 */

#ifndef FASTBOTX_HTTP_LLM_CLIENT_H
#define FASTBOTX_HTTP_LLM_CLIENT_H

#include <memory>
#include <string>
#include <vector>

#include "LlmTypes.h"
#include "../agent/AutodevAgent.h"

namespace fastbotx {

    /**
     * HttpLlmClient implements LlmClient on top of an OpenAI-compatible HTTP API.
     *
     * It sends a single non-streaming /chat/completions request with:
     * - Text-only: user content is the prompt string.
     * - Multimodal: when ImageData.bytes are provided, user content is an array
     *   [ {"type":"text","text":"<prompt>"}, {"type":"image_url","image_url":{"url":"data:image/png;base64,..."}}, ... ]
     *   Images are Base64-encoded and sent as data:image/png;base64,<data> per OpenAI API.
     */
    class HttpLlmClient : public LlmClient {
    public:
        explicit HttpLlmClient(const LlmRuntimeConfig &config);

        bool predict(const std::string &prompt,
                     const std::vector<ImageData> &images,
                     std::string &outResponse) override;

    private:
        LlmRuntimeConfig _config;

        bool isEnabled() const;

        bool doHttpPost(const std::string &payload, std::string &outResponse) const;

        std::string buildRequestBody(const std::string &prompt,
                                     const std::vector<ImageData> &images) const;
    };

} // namespace fastbotx

#endif // FASTBOTX_HTTP_LLM_CLIENT_H

