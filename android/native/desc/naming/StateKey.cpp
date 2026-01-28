/*
 * This code is licensed under the Fastbot license. You may obtain a copy of this license in the LICENSE.txt file in the root directory of this source tree.
 */
/**
 * @authors Jianqiang Guo, Yuhui Su, Zhao Zhang
 */
#ifndef FASTBOTX_STATEKEY_CPP_
#define FASTBOTX_STATEKEY_CPP_

#include "StateKey.h"
#include "../../utils.hpp"
#include <algorithm>
#include <sstream>

namespace fastbotx {

    StateKey::StateKey(std::string activity, NamingPtr naming, NamePtrVec widgets)
            : _activity(std::move(activity)), _naming(std::move(naming)), _widgets(std::move(widgets)) {
        buildHash();
    }

    void StateKey::buildHash() {
        // Combine activity string, naming hash and a normalized multiset of
        // widget Names into a single hash value. The widget list is sorted
        // by hash to ensure order‑independence.
        uintptr_t h = 0x1;
        h ^= std::hash<std::string>{}(_activity);
        if (_naming) {
            h ^= (_naming->hash() << 1);
        }
        std::sort(_widgets.begin(), _widgets.end(),
                  [](const NamePtr &a, const NamePtr &b) {
                      return a->hash() < b->hash();
                  });
        h ^= (combineHash<Name>(_widgets, true) << 2);
        _hash = h;
    }

    bool StateKey::operator==(const StateKey &other) const {
        if (_activity != other._activity) {
            return false;
        }
        if (_naming && other._naming) {
            if (_naming->hash() != other._naming->hash()) {
                return false;
            }
        } else if (_naming || other._naming) {
            return false;
        }
        if (_widgets.size() != other._widgets.size()) {
            return false;
        }
        for (size_t i = 0; i < _widgets.size(); i++) {
            if (_widgets[i]->hash() != other._widgets[i]->hash()) {
                return false;
            }
        }
        return true;
    }

    bool StateKey::operator<(const StateKey &other) const {
        if (_activity != other._activity) {
            return _activity < other._activity;
        }
        uintptr_t thisNamingHash = _naming ? _naming->hash() : 0;
        uintptr_t otherNamingHash = other._naming ? other._naming->hash() : 0;
        if (thisNamingHash != otherNamingHash) {
            return thisNamingHash < otherNamingHash;
        }
        if (_widgets.size() != other._widgets.size()) {
            return _widgets.size() < other._widgets.size();
        }
        for (size_t i = 0; i < _widgets.size(); i++) {
            uintptr_t thisHash = _widgets[i] ? _widgets[i]->hash() : 0;
            uintptr_t otherHash = other._widgets[i] ? other._widgets[i]->hash() : 0;
            if (thisHash != otherHash) {
                return thisHash < otherHash;
            }
        }
        return false; // Equal
    }

    bool StateKey::isTrivialState() const {
        if (_widgets.size() <= TRIVIAL_STATE_WIDGET_THRESHOLD) {
            return true;
        }
        size_t actionCount = 0;
        for (const auto &name : _widgets) {
            if (name && name->getNamer()) {
                actionCount++;
            }
        }
        return actionCount <= TRIVIAL_STATE_ACTION_THRESHOLD;
    }

    std::string StateKey::toString() const {
        std::ostringstream oss;
        oss << _activity << "@" << _hash << "@"
            << (_naming ? std::to_string(_naming->hash()) : "null")
            << "@[W=" << _widgets.size() << "]";
        return oss.str();
    }

} // namespace fastbotx

#endif // FASTBOTX_STATEKEY_CPP_
