/**
 * @authors Zhao Zhang
 */

#include "WidgetPriorityProvider.h"

#include "../utils.hpp"
#include "../thirdpart/json/json.hpp"

#include <cmath>
#include <cstdio>

namespace fastbotx {

    namespace {

        using nlohmann::json;

        std::string actionHashToId(uintptr_t h) {
            char buf[24];
            snprintf(buf, sizeof(buf), "0x%llx", (unsigned long long) h);
            return std::string(buf);
        }

        /**
         * Parse JSON for widget priorities.
         * Supports: (1) "priorities" array [p0,p1,...]; (2) "priorities" object {"id": p, ...}; (3) "recommend_order" [i0,i1,...].
         * LLM returns 0~1; we scale to a wider range so that weighted random strongly favors high-priority widgets.
         * Scale: 1.0 + k * p^exp with k=4, exp=0.8 → [1.0, ~5.0], low p suppressed so high p dominates more.
         */
        bool applyParsedWidgetPriorities(const json &j,
                                         const std::vector<ActivityStateActionPtr> &validActions,
                                         IWidgetPriorityProvider::Result &out) {
            out.widgetPriorities.clear();
            const size_t n = validActions.size();
            if (n == 0) return false;

            // Amplify LLM priority so high-priority widgets get much larger weight (higher chance in weighted random).
            // LLM p in [0,1]. Use 1.0 + k * p^exponent: exponent<1 suppresses low p, k>1 widens range.
            // Example: k=4, exp=0.8 → p=0.1→1.35, p=0.5→2.74, p=0.9→4.46 (ratio ~3.3x vs original ~1.9x).
            const double kAmplify = 4.0;
            const double kExponent = 0.8;
            auto scaleLlmPriority = [kAmplify, kExponent](double p) -> double {
                if (p < 0.0) p = 0.0;
                if (p > 1.0) p = 1.0;
                double q = std::pow(p, kExponent);
                return 1.0 + kAmplify * q;
            };

            if (j.contains("priorities")) {
                const auto &pri = j["priorities"];
                out.widgetPriorities.resize(n, 1.0);  // omitted -> 1.0
                if (pri.is_array()) {
                    for (size_t i = 0; i < n && i < pri.size(); ++i) {
                        if (pri[i].is_number()) {
                            out.widgetPriorities[i] = scaleLlmPriority(pri[i].get<double>());
                        }
                    }
                    return true;
                }
                if (pri.is_object()) {
                    for (size_t i = 0; i < n; ++i) {
                        std::string id = actionHashToId(validActions[i]->hash());
                        if (pri.contains(id) && pri[id].is_number()) {
                            out.widgetPriorities[i] = scaleLlmPriority(pri[id].get<double>());
                        }
                    }
                    return true;
                }
            }
            if (j.contains("recommend_order") && j["recommend_order"].is_array()) {
                const auto &order = j["recommend_order"];
                out.widgetPriorities.assign(n, 1.0);  // omitted -> 1.0
                const double nD = static_cast<double>(order.size());
                for (size_t rank = 0; rank < order.size(); ++rank) {
                    int idx = order[rank].is_number_integer() ? static_cast<int>(order[rank].get<int>()) : -1;
                    if (idx >= 0 && idx < static_cast<int>(n)) {
                        double raw = 1.0 - (static_cast<double>(rank) / (nD > 1 ? nD : 1.0));
                        out.widgetPriorities[static_cast<size_t>(idx)] = scaleLlmPriority(raw);
                    }
                }
                return true;
            }
            return false;
        }

    }  // anonymous namespace

    IWidgetPriorityProvider::Result LlmWidgetPriorityProvider::organize(
            uintptr_t absStateId,
            const std::vector<ActivityStateActionPtr> &validActions,
            const ModelPtr &model) {
        Result result;
        if (!model) return result;

        std::shared_ptr<LlmClient> client = model->getLlmClient();
        if (!client || validActions.size() < 2) {
            BDLOG("WidgetPriorityProvider: widget_priority skip absStateId=%llu (no client or elements<2)",
                  (unsigned long long) absStateId);
            return result;
        }

        json payload;
        payload["max_index"] = static_cast<int>(validActions.size() - 1);
        json elements = json::array();
        for (size_t i = 0; i < validActions.size(); ++i) {
            const auto &a = validActions[i];
            WidgetPtr w = a ? a->getTarget() : nullptr;
            json el;
            el["id"] = actionHashToId(a->hash());
            el["class"] = w ? w->getClassname() : "";
            el["resource_id"] = w ? w->getResourceID() : "";
            el["text"] = w ? w->getText() : "";
            el["content_desc"] = w ? w->getContentDesc() : "";
            elements.push_back(el);
        }
        payload["elements"] = std::move(elements);
        std::string payloadStr = payload.dump();

        BDLOG("WidgetPriorityProvider: widget_priority request absStateId=%llu elements=%zu",
              (unsigned long long) absStateId, validActions.size());

        std::string response;
        if (!client->predictWithPayload("knowledge_org", payloadStr, {}, response)) {
            BDLOGE("WidgetPriorityProvider: widget_priority predict failed (check Java LLM HTTP logs)");
            return result;
        }

        std::string toParse = response;
        const std::string jsonMarker("JSON:");
        size_t pos = response.find(jsonMarker);
        if (pos != std::string::npos) {
            toParse = response.substr(pos + jsonMarker.size());
            size_t start = toParse.find_first_not_of(" \t\n\r");
            if (start != std::string::npos) toParse = toParse.substr(start);
        } else {
            size_t braceP = response.find("{\"priorities\"");
            size_t braceR = response.find("{\"recommend_order\"");
            if (braceP != std::string::npos) toParse = response.substr(braceP);
            else if (braceR != std::string::npos) toParse = response.substr(braceR);
            else {
                size_t brace = response.find('{');
                if (brace != std::string::npos) toParse = response.substr(brace);
            }
        }

        try {
            json j = json::parse(toParse);
            if (applyParsedWidgetPriorities(j, validActions, result)) {
                result.success = true;
                BDLOG("WidgetPriorityProvider: widget_priority done absStateId=%llu n=%zu",
                      (unsigned long long) absStateId, result.widgetPriorities.size());
            }
        } catch (...) {
            BDLOG("WidgetPriorityProvider: widget_priority parse failed absStateId=%llu",
                  (unsigned long long) absStateId);
        }

        return result;
    }

}  // namespace fastbotx
