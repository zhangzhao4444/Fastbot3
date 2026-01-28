/*
 * This code is licensed under the Fastbot license. You may obtain a copy of this license in the LICENSE.txt file in the root directory of this source tree.
 */
/**
 * @authors Jianqiang Guo, Yuhui Su, Zhao Zhang
 */
#ifndef FASTBOTX_NAMING_CPP_
#define FASTBOTX_NAMING_CPP_

#include "Naming.h"
#include <algorithm>

namespace fastbotx {

    Naming::Naming(const std::vector<NameletPtr> &namelets)
            : _namelets(namelets) {
    }

    Naming::NamingResult Naming::namingWidgets(const WidgetPtrVec &widgets, const RectPtr &rootBounds) const {
        // Iterate over widgets, filter them by geometry / content / bounds,
        // then resolve a Namelet and build the corresponding Name via Namer.
        NamingResult result;
        result.nodes.reserve(widgets.size());
        result.names.reserve(widgets.size());
        for (const auto &widget : widgets) {
            if (!widget) {
                continue;
            }
#if IGNORE_EMPTY_NODE
            if (widget->getBounds() == nullptr || widget->getBounds()->isEmpty()) {
                continue;
            }
            if (!widget->hasAction() &&
                widget->getText().empty() &&
                widget->getResourceID().empty() &&
                widget->getContentDesc().empty()) {
                continue;
            }
#endif
#if IGNORE_OUT_OF_BOUNDS_NODE
            if (rootBounds && !rootBounds->isEmpty() && widget->getBounds()) {
                Point center = widget->getBounds()->center();
                if (!rootBounds->contains(center)) {
                    continue;
                }
            }
#endif
            NameletPtr namelet = resolveNamelet(widget);
            if (!namelet) {
                continue;
            }
            NamePtr name = namelet->getNamer()->naming(widget);
            if (!name) {
                continue;
            }
            result.nodes.emplace_back(widget);
            result.names.emplace_back(name);
        }
        return result;
    }

    NameletPtr Naming::resolveNamelet(const WidgetPtr &widget) const {
        // Hot path: avoid allocating a temporary matches vector per widget.
        // Pick the best namelet directly while scanning.
        NameletPtr best;
        int bestDepth = -1;
        const std::string *bestExpr = nullptr;
        for (const auto &namelet : _namelets) {
            if (!namelet || !namelet->matches(widget)) {
                continue;
            }
            int d = namelet->getDepth();
            const std::string &expr = namelet->getExprString();
            if (!best || d > bestDepth || (d == bestDepth && (!bestExpr || expr > *bestExpr))) {
                best = namelet;
                bestDepth = d;
                bestExpr = &expr;
            }
        }
        return best;
    }

    NameletPtr Naming::selectNamelet(const std::vector<NameletPtr> &matches) const {
        if (matches.empty()) {
            return nullptr;
        }
        // Select the "most refined" matching namelet using a single pass:
        //   - higher depth wins
        //   - if depths tie, choose lexicographically larger expression string
        NameletPtr best;
        int bestDepth = -1;
        const std::string *bestExpr = nullptr;
        for (const auto &candidate : matches) {
            if (!candidate) {
                continue;
            }
            int d = candidate->getDepth();
            const std::string &expr = candidate->getExprString();
            if (!best || d > bestDepth || (d == bestDepth && (!bestExpr || expr > *bestExpr))) {
                best = candidate;
                bestDepth = d;
                bestExpr = &expr;
            }
        }
        return best;
    }

    NamingPtr Naming::extend(const NameletPtr &parent, const NameletPtr &namelet) const {
        if (!namelet || !parent) {
            return nullptr;
        }
        std::vector<NameletPtr> next = _namelets;
        namelet->setParent(parent);
        next.emplace_back(namelet);
        auto created = std::make_shared<Naming>(next);
        created->_parent = shared_from_this();
        return created;
    }

    NamingPtr Naming::replaceLast(const NameletPtr &replaced, const NameletPtr &namelet) const {
        if (!replaced || !namelet) {
            return nullptr;
        }
        if (_namelets.empty()) {
            return nullptr;
        }
        auto parent = getParent();
        if (!parent) {
            return nullptr;
        }
        return parent->extend(replaced->getParent(), namelet);
    }

    bool Naming::isAncestor(const NamingPtr &other) const {
        if (!other) {
            return false;
        }
        auto current = other->getParent();
        while (current) {
            if (current == shared_from_this()) {
                return true;
            }
            current = current->getParent();
        }
        return false;
    }

    bool Naming::isReplaceable(const NameletPtr &namelet) const {
        if (!namelet || _namelets.empty()) {
            return false;
        }
        return _namelets.back() == namelet;
    }

    NameletPtr Naming::getLastNamelet() const {
        if (_namelets.empty()) {
            return nullptr;
        }
        return _namelets.back();
    }

    int Naming::getFineness() const {
        int maxSize = 0;
        for (const auto &namelet : _namelets) {
            if (!namelet || !namelet->getNamer()) {
                continue;
            }
            int size = static_cast<int>(namelet->getNamer()->getNamerTypes().size());
            maxSize = std::max(maxSize, size);
        }
        return maxSize;
    }

    uintptr_t Naming::hash() const {
        uintptr_t h = 0x1;
        for (const auto &namelet : _namelets) {
            if (namelet) {
                h ^= namelet->hash();
            }
        }
        return h;
    }

} // namespace fastbotx

#endif // FASTBOTX_NAMING_CPP_
