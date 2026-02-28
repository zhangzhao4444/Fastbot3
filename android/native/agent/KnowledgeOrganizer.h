/**
 * @authors Zhao Zhang
 */
 
#ifndef FASTBOTX_KNOWLEDGE_ORGANIZER_H
#define FASTBOTX_KNOWLEDGE_ORGANIZER_H

#include "../desc/State.h"
#include "../desc/Action.h"
#include "../model/Model.h"

#include <memory>
#include <vector>
#include <unordered_map>
#include <string>

namespace fastbotx {

    /**
     * Interface for abstract action knowledge organization on one abstract state.
     *
     * Given an abstract state id, its valid concrete actions, and the model (for LLM, etc.),
     * returns a grouping result that maps each action to a group id plus optional
     * natural-language function descriptions for groups.
     */
    class IKnowledgeOrganizer {
    public:
        struct Result {
            // groupIds[i] is the group id for validActions[i]. When 0, caller should
            // fall back to 1:1 grouping for that action.
            std::vector<uintptr_t> groupIds;
            // Optional natural-language description for each group id.
            std::unordered_map<uintptr_t, std::string> groupFunctions;
            bool success = false;
        };

        virtual ~IKnowledgeOrganizer() = default;

        /**
         * Organize actions for one abstract state into same-function groups.
         *
         * @param absStateId abstract state id.
         * @param validActions actions available in this abstract state (ActivityStateActionPtr, all valid()).
         * @param model underlying model (may provide LlmClient or other context).
         * @param minSameFunctionGroupSize minimum group size to treat as same-function group.
         *
         * @return Result with per-action group ids and optional group function strings.
         *         When result.success is false, caller should fall back to 1:1 grouping.
         */
        virtual Result organize(uintptr_t absStateId,
                                const std::vector<ActivityStateActionPtr> &validActions,
                                const ModelPtr &model,
                                size_t minSameFunctionGroupSize) = 0;
    };

    using IKnowledgeOrganizerPtr = std::shared_ptr<IKnowledgeOrganizer>;

    /**
     * Default LLM-based implementation that calls the "knowledge_org" endpoint
     * via Model::getLlmClient() and parses its JSON response.
     *
     * Behavior is intentionally aligned with the original inlined implementation
     * inside LLMExplorerAgent::tryLlmKnowledgeOrganization, but moved to a separate,
     * pluggable component.
     */
    class LlmKnowledgeOrganizer : public IKnowledgeOrganizer {
    public:
        Result organize(uintptr_t absStateId,
                        const std::vector<ActivityStateActionPtr> &validActions,
                        const ModelPtr &model,
                        size_t minSameFunctionGroupSize) override;
    };

}  // namespace fastbotx

#endif  // FASTBOTX_KNOWLEDGE_ORGANIZER_H

