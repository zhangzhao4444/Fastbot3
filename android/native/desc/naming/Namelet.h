/*
 * This code is licensed under the Fastbot license. You may obtain a copy of this license in the LICENSE.txt file in the root directory of this source tree.
 */
/**
 * @authors Jianqiang Guo, Yuhui Su, Zhao Zhang
 */
#ifndef FASTBOTX_NAMELET_H_
#define FASTBOTX_NAMELET_H_

#include "../../Base.h"
#include "../Widget.h"
#include "Namer.h"
#include <functional>
#include <string>

namespace fastbotx {

    // A "namelet" is a small naming rule that matches a subset of widgets
    // and associates them with a particular Namer. Multiple namelets can be
    // composed into a higher‑level Naming.
    class Namelet : public HashNode, public std::enable_shared_from_this<Namelet> {
    public:
        enum class Type {
            BASE = 0,
            REFINED
        };

        enum class MatchRule {
            ANY = 0,
            ACTIONABLE,
            NON_ACTIONABLE,
            ATTRIBUTES
        };

        struct AttributeFilter {
            std::string clazz;
            std::string resourceID;
            std::string text;
            std::string contentDesc;
        };

        // Construct a namelet with explicit type and expression string.
        // The expression is kept for logging / debugging; the actual
        // matching is controlled by the MatchRule and AttributeFilter.
        Namelet(Type type, std::string expr, NamerPtr namer);
        // Helper for creating a refined namelet; defaults type to REFINED.
        Namelet(std::string expr, NamerPtr namer);

        // Parent namelet in the refinement chain; used for computing depth
        // and for checking ancestor relationships in Naming. Setting the
        // parent will also update this namelet's cached depth.
        void setParent(const std::shared_ptr<Namelet> &parent);
        std::shared_ptr<Namelet> getParent() const { return _parent.lock(); }

        // Original expression string (e.g., XPath‑like) for this rule.
        const std::string &getExprString() const { return _exprString; }
        // Namer that will be used to build Names for widgets matched by this rule.
        const NamerPtr &getNamer() const { return _namer; }
        Type getType() const { return _type; }

        // Configure how this namelet selects widgets.
        void setMatchRule(MatchRule rule) { _matchRule = rule; }
        void setAttributeFilter(AttributeFilter filter) { _attrFilter = std::move(filter); }

        // Check whether a widget satisfies this namelet's match rule and filter.
        bool matches(const WidgetPtr &widget) const;
        // Depth of this namelet in the refinement chain (root has depth 0).
        int getDepth() const;

        // HashNode interface: combines expression and Namer hash.
        uintptr_t hash() const override;

    private:
        Type _type;
        std::string _exprString;
        NamerPtr _namer;
        std::weak_ptr<Namelet> _parent;
        MatchRule _matchRule;
        AttributeFilter _attrFilter;
        int _depth{0};
    };

    typedef std::shared_ptr<Namelet> NameletPtr;

} // namespace fastbotx

#endif // FASTBOTX_NAMELET_H_
