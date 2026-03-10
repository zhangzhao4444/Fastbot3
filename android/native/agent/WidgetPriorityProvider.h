/**
 * @authors Zhao Zhang
 */

#ifndef FASTBOTX_WIDGET_PRIORITY_PROVIDER_H
#define FASTBOTX_WIDGET_PRIORITY_PROVIDER_H

#include "../desc/State.h"
#include "../desc/Action.h"
#include "../model/Model.h"

#include <memory>
#include <vector>
#include <string>

namespace fastbotx {

    /**
     * Interface for LLM-based widget priority on one abstract state.
     *
     * Given an abstract state id, its valid concrete actions, and the model (for LLM),
     * returns per-action widget priorities (user operation popularity) for selection bias.
     */
    class IWidgetPriorityProvider {
    public:
        struct Result {
            /// LLM-inferred user operation popularity per widget (same index as validActions).
            /// Higher value = more likely to be clicked; used for widget selection priority. Empty if not provided.
            std::vector<double> widgetPriorities;
            bool success = false;
        };

        virtual ~IWidgetPriorityProvider() = default;

        /**
         * Get widget selection priorities for this abstract state (LLM infers from widget semantics).
         *
         * @param absStateId abstract state id.
         * @param validActions actions available in this abstract state (ActivityStateActionPtr, all valid()).
         * @param model underlying model (may provide LlmClient).
         *
         * @return Result with widgetPriorities when success; empty when failed or disabled.
         */
        virtual Result organize(uintptr_t absStateId,
                                const std::vector<ActivityStateActionPtr> &validActions,
                                const ModelPtr &model) = 0;
    };

    using IWidgetPriorityProviderPtr = std::shared_ptr<IWidgetPriorityProvider>;

    /**
     * LLM-based implementation: calls "knowledge_org" (or widget_priority) endpoint
     * via Model::getLlmClient() and parses priorities / recommend_order from JSON.
     */
    class LlmWidgetPriorityProvider : public IWidgetPriorityProvider {
    public:
        Result organize(uintptr_t absStateId,
                        const std::vector<ActivityStateActionPtr> &validActions,
                        const ModelPtr &model) override;
    };

}  // namespace fastbotx

#endif  // FASTBOTX_WIDGET_PRIORITY_PROVIDER_H
