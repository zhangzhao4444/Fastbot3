/**
 * @authors Zhao Zhang
 */

#include "ContentAwareInputProvider.h"

#include "../utils.hpp"
#include "../thirdpart/json/json.hpp"

namespace fastbotx {

    using nlohmann::json;

    std::string LlmContentAwareInputProvider::getInputTextForAction(
            const StatePtr &state,
            const ActionPtr &action,
            const ModelPtr &model) const {
        if (!state || !action) {
            BDLOG("LLMExplorerAgent: content_aware_input skip (no state or action)");
            return "";
        }

        auto stateAction = std::dynamic_pointer_cast<ActivityStateAction>(action);
        if (!stateAction || !stateAction->requireTarget()) {
            BDLOG("LLMExplorerAgent: content_aware_input skip (not ActivityStateAction or requireTarget()=false)");
            return "";
        }

        WidgetPtr target = stateAction->getTarget();
        if (!target || !target->isEditable()) {
            BDLOG("LLMExplorerAgent: content_aware_input skip (no target or !isEditable())");
            return "";
        }

        std::string activityStr = (state->getActivityString() && state->getActivityString().get())
                                  ? *state->getActivityString() : "";
        std::string resourceId = target->getResourceID();
        std::string text = target->getText();
        std::string contentDesc = target->getContentDesc();
        std::string cacheKey = activityStr + "\t" + resourceId + "\t" + text + "\t" + contentDesc;

        BDLOG("LLMExplorerAgent: content_aware_input eligible activity=%s resource_id=%s",
              activityStr.c_str(), resourceId.c_str());

        {
            auto it = _cache.find(cacheKey);
            if (it != _cache.end()) {
                BDLOG("LLMExplorerAgent: content_aware_input cache hit resource_id=%s", resourceId.c_str());
                return it->second;
            }
        }

        if (!model) return "";
        std::shared_ptr<LlmClient> client = model->getLlmClient();
        if (!client) return "";

        std::string packageName = model->getPackageName();
        json payload;
        payload["package"] = packageName;
        payload["activity"] = activityStr;
        payload["class"] = target->getClassname();
        payload["resource_id"] = resourceId;
        payload["text"] = text;
        payload["content_desc"] = contentDesc;
        std::string payloadStr = payload.dump();

        BDLOG("LLMExplorerAgent: content_aware_input cache miss, request LLM resource_id=%s", resourceId.c_str());
        std::string response;
        if (!client->predictWithPayload("content_aware_input", payloadStr, {}, response)) {
            BDLOGE("LLMExplorerAgent: content_aware_input predict failed (check Java LLM HTTP logs)");
            return "";
        }

        // Trim whitespace and surrounding quotes; limit length (paper: human-like short input)
        size_t start = 0;
        while (start < response.size() &&
               (std::isspace(static_cast<unsigned char>(response[start])) ||
                response[start] == '"' || response[start] == '\'')) {
            start++;
        }
        size_t end = response.size();
        while (end > start &&
               (std::isspace(static_cast<unsigned char>(response[end - 1])) ||
                response[end - 1] == '"' || response[end - 1] == '\'')) {
            end--;
        }
        if (start >= end) return "";
        response = response.substr(start, end - start);

        const size_t kMaxInputLen = 200;
        if (response.size() > kMaxInputLen) response.resize(kMaxInputLen);

        while (_cacheOrder.size() >= kMaxContentAwareInputCacheSize) {
            std::string oldKey = std::move(_cacheOrder.front());
            _cacheOrder.pop_front();
            _cache.erase(oldKey);
        }
        _cache[cacheKey] = response;
        _cacheOrder.push_back(cacheKey);

        std::string logPreview = response.size() > 40 ? response.substr(0, 37) + "..." : response;
        BDLOG("LLMExplorerAgent: content_aware_input ok suggested=%s", logPreview.c_str());
        return response;
    }

    void LlmContentAwareInputProvider::onStateAbstractionChanged() {
        _cache.clear();
        _cacheOrder.clear();
    }

}  // namespace fastbotx

