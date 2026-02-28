/**
 * Pluggable content-aware input provider for editable widgets.
 *
 * Extracts the "content_aware_input" LLM logic from LLMExplorerAgent into a
 * separate, replaceable component (similar to IStateEncoder for ICMAgent),
 * with an LLM-based default implementation and internal caching.
 */
/**
 * @authors Zhao Zhang
 */

#ifndef FASTBOTX_CONTENT_AWARE_INPUT_PROVIDER_H
#define FASTBOTX_CONTENT_AWARE_INPUT_PROVIDER_H

#include "../desc/State.h"
#include "../desc/Action.h"
#include "../model/Model.h"

#include <memory>
#include <string>
#include <unordered_map>
#include <list>

namespace fastbotx {

    class IContentAwareInputProvider {
    public:
        virtual ~IContentAwareInputProvider() = default;

        /**
         * Return content-aware input text for a given (state, action, model).
         * Implementations may use LLMs or heuristic rules, and may maintain
         * their own internal caches.
         */
        virtual std::string getInputTextForAction(const StatePtr &state,
                                                  const ActionPtr &action,
                                                  const ModelPtr &model) const = 0;

        /// Optional hook for clearing caches when state abstraction changes.
        virtual void onStateAbstractionChanged() {}
    };

    using IContentAwareInputProviderPtr = std::shared_ptr<IContentAwareInputProvider>;

    /**
     * Default LLM-based implementation that calls "content_aware_input"
     * via Model::getLlmClient(), with a small FIFO cache keyed by
     * (activity, resource_id, text, content_desc).
     */
    class LlmContentAwareInputProvider : public IContentAwareInputProvider {
    public:
        std::string getInputTextForAction(const StatePtr &state,
                                          const ActionPtr &action,
                                          const ModelPtr &model) const override;

        void onStateAbstractionChanged() override;

    private:
        static constexpr size_t kMaxContentAwareInputCacheSize = 64;

        mutable std::unordered_map<std::string, std::string> _cache;
        mutable std::list<std::string> _cacheOrder;
    };

}  // namespace fastbotx

#endif  // FASTBOTX_CONTENT_AWARE_INPUT_PROVIDER_H

