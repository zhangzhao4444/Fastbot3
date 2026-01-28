/*
 * This code is licensed under the Fastbot license. You may obtain a copy of this license in the LICENSE.txt file in the root directory of this source tree.
 */
/**
 * @authors Jianqiang Guo, Yuhui Su, Zhao Zhang
 */
#ifndef FASTBOTX_NAMELET_CPP_
#define FASTBOTX_NAMELET_CPP_

#include "Namelet.h"

namespace fastbotx {

    Namelet::Namelet(Type type, std::string expr, NamerPtr namer)
            : _type(type), _exprString(std::move(expr)), _namer(std::move(namer)),
              _matchRule(MatchRule::ANY), _depth(0) {
    }

    Namelet::Namelet(std::string expr, NamerPtr namer)
            : Namelet(Type::REFINED, std::move(expr), std::move(namer)) {
    }

    void Namelet::setParent(const std::shared_ptr<Namelet> &parent) {
        _parent = parent;
        if (parent) {
            _depth = parent->_depth + 1;
        } else {
            _depth = 0;
        }
    }

    bool Namelet::matches(const WidgetPtr &widget) const {
        if (!widget) {
            return false;
        }
        switch (_matchRule) {
            case MatchRule::ANY:
                return true;
            case MatchRule::ACTIONABLE:
                return widget->hasAction();
            case MatchRule::NON_ACTIONABLE:
                return !widget->hasAction();
            case MatchRule::ATTRIBUTES: {
                if (!_attrFilter.clazz.empty() && widget->getClassName() != _attrFilter.clazz) {
                    return false;
                }
                if (!_attrFilter.resourceID.empty() && widget->getResourceID() != _attrFilter.resourceID) {
                    return false;
                }
                if (!_attrFilter.text.empty() && widget->getText() != _attrFilter.text) {
                    return false;
                }
                if (!_attrFilter.contentDesc.empty() && widget->getContentDesc() != _attrFilter.contentDesc) {
                    return false;
                }
                return true;
            }
        }
        return true;
    }

    int Namelet::getDepth() const {
        return _depth;
    }

    uintptr_t Namelet::hash() const {
        uintptr_t h = std::hash<std::string>{}(_exprString);
        if (_namer) {
            h ^= (_namer->hash() << 1);
        }
        return h;
    }

} // namespace fastbotx

#endif // FASTBOTX_NAMELET_CPP_
