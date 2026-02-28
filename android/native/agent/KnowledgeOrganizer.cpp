/**
 * @authors Zhao Zhang
 */

#include "KnowledgeOrganizer.h"

#include "../utils.hpp"
#include "../thirdpart/json/json.hpp"

#include <unordered_set>

namespace fastbotx {

    namespace {

        using nlohmann::json;

        /**
         * Apply parsed JSON for knowledge_org to fill grouping result.
         *
         * JSON format (aligned with Java side):
         * {
         *   "groups": [[0,1,2], [3,4], ...],
         *   "functions": ["navigate home", "open detail", ...]
         * }
         *
         * Returns:
         *   - number of "large" groups (size >= minSameFunctionGroupSize), same as original
         *     implementation (used only for logging / success signal).
         *   - When small groups exist but no large groups, returns 1 to signal success.
         */
        size_t applyParsedKnowledgeOrg(const json &j,
                                       const std::vector<ActivityStateActionPtr> &validActions,
                                       size_t minSameFunctionGroupSize,
                                       IKnowledgeOrganizer::Result &out) {
            if (!j.contains("groups") || !j["groups"].is_array()) return 0;

            const auto &groups = j["groups"];
            const size_t n = validActions.size();
            out.groupIds.assign(n, 0);
            out.groupFunctions.clear();

            uintptr_t nextGroupId = 0;
            bool hadSmallGroups = false;

            for (const auto &group : groups) {
                if (!group.is_array()) continue;

                std::vector<size_t> indices;
                indices.reserve(group.size());
                for (const auto &idx : group) {
                    int i = idx.is_number_integer() ? static_cast<int>(idx.get<int>()) : -1;
                    if (i < 0 || i >= static_cast<int>(n)) continue;
                    indices.push_back(static_cast<size_t>(i));
                }
                if (indices.empty()) continue;

                if (indices.size() >= minSameFunctionGroupSize) {
                    uintptr_t gid = nextGroupId;
                    for (size_t i : indices) {
                        out.groupIds[i] = gid;
                    }
                    nextGroupId++;
                } else {
                    hadSmallGroups = true;
                    for (size_t i : indices) {
                        uintptr_t actHash = validActions[i]->hash();
                        out.groupIds[i] = actHash;
                    }
                }
            }

            if (nextGroupId > 0 && j.contains("functions") && j["functions"].is_array()) {
                const auto &funcArr = j["functions"];
                for (size_t g = 0; g < funcArr.size() && g < nextGroupId; ++g) {
                    if (funcArr[g].is_string()) {
                        std::string f = funcArr[g].get<std::string>();
                        if (!f.empty()) out.groupFunctions[static_cast<uintptr_t>(g)] = f;
                    }
                }
            }

            if (nextGroupId > 0) {
                return nextGroupId;
            }
            return hadSmallGroups ? 1 : 0;
        }

    }  // anonymous namespace

    IKnowledgeOrganizer::Result LlmKnowledgeOrganizer::organize(
            uintptr_t absStateId,
            const std::vector<ActivityStateActionPtr> &validActions,
            const ModelPtr &model,
            size_t minSameFunctionGroupSize) {
        Result result;
        if (!model) return result;

        std::shared_ptr<LlmClient> client = model->getLlmClient();
        if (!client || validActions.size() < 2) {
            BDLOG("LLMExplorerAgent: knowledge_org skip absStateId=%llu (no client or elements<2)",
                  (unsigned long long) absStateId);
            return result;
        }

        json payload;
        payload["max_index"] = static_cast<int>(validActions.size() - 1);
        json elements = json::array();
        for (const auto &a : validActions) {
            WidgetPtr w = a ? a->getTarget() : nullptr;
            json el;
            el["class"] = w ? w->getClassname() : "";
            el["resource_id"] = w ? w->getResourceID() : "";
            el["text"] = w ? w->getText() : "";
            el["content_desc"] = w ? w->getContentDesc() : "";
            elements.push_back(el);
        }
        payload["elements"] = std::move(elements);
        std::string payloadStr = payload.dump();

        BDLOG("LLMExplorerAgent: knowledge_org request absStateId=%llu elements=%zu",
              (unsigned long long) absStateId, validActions.size());

        std::string response;
        if (!client->predictWithPayload("knowledge_org", payloadStr, {}, response)) {
            BDLOGE("LLMExplorerAgent: knowledge_org predict failed (check Java LLM HTTP logs)");
            return result;
        }

        // Extract JSON from response: after "JSON:" (CoT format) or use full response
        std::string toParse = response;
        const std::string jsonMarker("JSON:");
        size_t pos = response.find(jsonMarker);
        if (pos != std::string::npos) {
            toParse = response.substr(pos + jsonMarker.size());
            size_t start = toParse.find_first_not_of(" \t\n\r");
            if (start != std::string::npos) toParse = toParse.substr(start);
        } else {
            size_t brace = response.find("{\"groups\"");
            if (brace != std::string::npos) toParse = response.substr(brace);
        }

        size_t groupCount = 0;
        try {
            json j = json::parse(toParse);
            groupCount = applyParsedKnowledgeOrg(j, validActions, minSameFunctionGroupSize, result);
        } catch (...) {
            // Repair: LLM sometimes omits closing ']' for "functions" array (e.g. ends with "}" instead of "]}").
            std::string repaired;
            if (toParse.find("\"functions\"") != std::string::npos && toParse.size() >= 2) {
                size_t lastBrace = toParse.rfind('}');
                if (lastBrace != std::string::npos && lastBrace > 0 && toParse[lastBrace - 1] == '"') {
                    repaired = toParse.substr(0, lastBrace) + "]" + toParse.substr(lastBrace);
                }
            }
            if (!repaired.empty()) {
                try {
                    json j = json::parse(repaired);
                    groupCount = applyParsedKnowledgeOrg(j, validActions, minSameFunctionGroupSize, result);
                } catch (...) {
                    groupCount = 0;
                }
            }
        }

        if (groupCount == 0) {
            BDLOG("LLMExplorerAgent: knowledge_org parse failed absStateId=%llu, will fallback 1:1",
                  (unsigned long long) absStateId);
            result.groupIds.clear();
            result.groupFunctions.clear();
            result.success = false;
            return result;
        }

        result.success = true;
        return result;
    }

}  // namespace fastbotx

