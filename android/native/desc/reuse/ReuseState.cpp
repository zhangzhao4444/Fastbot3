/*
 * This code is licensed under the Fastbot license. You may obtain a copy of this license in the LICENSE.txt file in the root directory of this source tree.
 */
/**
 * @authors Jianqiang Guo, Yuhui Su, Zhao Zhang
 */
#ifndef ReuseState_CPP_
#define ReuseState_CPP_

#include "ReuseState.h"

#include <utility>
#include <unordered_map>
#include <unordered_set>
#include "RichWidget.h"
#include "ActivityNameAction.h"
#include "../utils.hpp"
#include "ActionFilter.h"
#include "../events/Preference.h"

namespace fastbotx {


    ReuseState::ReuseState()
    = default;

    ReuseState::ReuseState(stringPtr activityName)
            : ReuseState() {
        this->_activity = std::move(activityName);
        this->_hasNoDetail = false;
    }

    /**
     * @brief Build bounding box for root element
     * 
     * Sets the root bounds for this state. If the element is the root (no parent)
     * and has valid bounds, stores them. Uses shared root bounds if available.
     * 
     * Performance optimization:
     * - Checks bounds validity before accessing
     * - Avoids multiple getBounds() calls
     * 
     * @param element Element to get bounds from
     */
    void ReuseState::buildBoundingBox(const ElementPtr &element) {
        // Check if this is the root element (no parent)
        if (element->getParent().expired()) {
            RectPtr bounds = element->getBounds();
            // Check bounds validity before using
            if (bounds != nullptr && !bounds->isEmpty()) {
                // Initialize shared root bounds if empty
                if (_sameRootBounds->isEmpty() && element) {
                    _sameRootBounds = bounds;
                }
                // Use shared bounds if they match, otherwise use element's bounds
                if (equals(_sameRootBounds, bounds)) {
                    this->_rootBounds = _sameRootBounds;
                } else {
                    this->_rootBounds = bounds;
                }
            }
        }
    }

    void ReuseState::buildStateFromElement(WidgetPtr parentWidget, ElementPtr element) {
        buildBoundingBox(element);
        WidgetPtr widget = std::make_shared<RichWidget>(parentWidget, element);
        this->_widgets.emplace_back(widget);
        for (const auto &childElement: element->getChildren()) {
            buildFromElement(widget, childElement);
        }
    }

    /**
     * @brief Build widget tree from Element (using regular Widget for children)
     * 
     * This method is used for building child widgets after the root uses RichWidget.
     * It uses regular Widget instead of RichWidget for performance optimization.
     * 
     * Performance optimization:
     * - Removed unnecessary dynamic_pointer_cast (elem is already ElementPtr)
     * - Direct use of elem parameter
     * 
     * @param parentWidget Parent widget
     * @param elem Element to build widget from (already ElementPtr, no cast needed)
     */
    void ReuseState::buildFromElement(WidgetPtr parentWidget, ElementPtr elem) {
        buildBoundingBox(elem);
        WidgetPtr widget;
        if (Preference::inst() && Preference::inst()->useStaticReuseAbstraction()) {
            // Legacy static reuse mode: use RichWidget for all nodes so hash matches legacy ReuseWidget style
            widget = std::make_shared<RichWidget>(parentWidget, elem);
        } else {
            // Dynamic abstraction mode: still use lightweight Widget for non-root nodes
            widget = std::make_shared<Widget>(parentWidget, elem);
        }
        this->_widgets.emplace_back(widget);
        for (const auto &childElement: elem->getChildren()) {
            buildFromElement(widget, childElement);
        }
    }

    /**
     * @brief Factory method to create a ReuseState from Element and activity name
     * 
     * Creates a new ReuseState object by:
     * 1. Building widget tree from Element (using RichWidget)
     * 2. Merging duplicate widgets
     * 3. Computing state hash
     * 4. Creating actions for all widgets
     * 
     * @param element Root Element of the UI hierarchy (XML of this page)
     * @param activityName Activity name string pointer
     * @return Shared pointer to newly created ReuseState
     */
    ReuseStatePtr ReuseState::create(const ElementPtr &element, const stringPtr &activityName,
                                     WidgetKeyMask mask) {
        // Use new + shared_ptr instead of make_shared because constructor is protected
        ReuseStatePtr statePointer = std::shared_ptr<ReuseState>(new ReuseState(activityName));
        statePointer->_widgetKeyMask = mask;
        statePointer->buildState(element);
        return statePointer;
    }

    void ReuseState::buildState(const ElementPtr &element) {
        buildStateFromElement(nullptr, element);
        mergeWidgetsInState();
        buildHashForState();
        buildActionForState();
    }

    /**
     * @brief Build hash code for this state
     * 
     * Computes hash code based on activity name and widget collection.
     * The hash is used for state comparison and deduplication.
     * 
     * Performance optimization:
     * - Uses efficient hash combination with bit shifting
     * - Combines activity hash with widget hash
     */
    void ReuseState::buildHashForState() {
        // Build hash from activity name (guard against null _activity)
        std::string activityString = (_activity && _activity.get()) ? *_activity : "";
        uintptr_t activityHash = (fastbotx::fastStringHash(activityString) * 31U) << 5;

#if DYNAMIC_STATE_ABSTRACTION_ENABLED
        // In static reuse abstraction mode, ignore WidgetKeyMask and fall back to legacy hash
        if (Preference::inst() && Preference::inst()->useStaticReuseAbstraction()) {
            activityHash ^= (combineHash<Widget>(_widgets, STATE_WITH_WIDGET_ORDER) << 1);
        } else {
            uintptr_t widgetsHash = 0x1;
            for (const auto &w : _widgets) {
                if (w) {
                    widgetsHash ^= w->hashWithMask(_widgetKeyMask);
                }
            }
            activityHash ^= (widgetsHash << 1);
        }
#else
        // Combine with widget hash (may include order if STATE_WITH_WIDGET_ORDER is enabled)
        activityHash ^= (combineHash<Widget>(_widgets, STATE_WITH_WIDGET_ORDER) << 1);
#endif
        _hashcode = activityHash;
    }

    /**
     * @brief Build actions for all widgets in this state
     * 
     * Creates ActivityNameAction objects for each action type supported by each widget.
     * ActivityNameAction includes the activity name for reuse-based algorithms.
     * Also creates and adds a back action for navigation.
     * 
     * Performance optimizations:
     * - Pre-allocates _actions vector capacity to avoid reallocations
     * - Uses make_shared instead of new + shared_ptr (single memory allocation)
     * - Uses emplace_back() to construct objects in-place (avoids copy)
     * - Skips widgets with null bounds
     */
    void ReuseState::buildActionForState() {
        // Performance: Cache activity string (shared_ptr copy) once instead of per action
        stringPtr activityStr = getActivityString();
        // Performance: Heuristic reserve to avoid first-pass count; typical ~2–4 actions per widget
        _actions.reserve(_widgets.size() * 4 + 1);

        for (const auto &widget : _widgets) {
            RectPtr bounds = widget->getBounds();
            if (bounds == nullptr) {
                BLOGE("NULL Bounds happened");
                continue;
            }
            // getActions() returns const ref — no set copy per widget
            const std::set<ActionType> &actions = widget->getActions();
            for (ActionType action : actions) {
                _actions.emplace_back(std::make_shared<ActivityNameAction>(activityStr, widget, action));
            }
        }

        _backAction = std::make_shared<ActivityNameAction>(activityStr, nullptr, ActionType::BACK);
        _actions.emplace_back(_backAction);
    }

    /**
     * @brief Merge duplicate widgets in this state
     * 
     * Identifies and merges duplicate widgets (by hash) to optimize state comparison.
     * Replaces the widget vector with deduplicated widgets.
     * 
     * Performance optimization:
     * - Reduces widget count by removing duplicates
     * - Improves hash computation and state comparison speed
     */
    void ReuseState::mergeWidgetsInState() {
#if DYNAMIC_STATE_ABSTRACTION_ENABLED
        if (Preference::inst() && Preference::inst()->useStaticReuseAbstraction()) {
            // Static reuse abstraction: fall back to legacy merge-by-Widget::hash behavior
            WidgetPtrSet mergedWidgets;
            int mergedCount = mergeWidgetAndStoreMergedOnes(mergedWidgets);
            if (mergedCount != 0) {
                BDLOG("build state merged  %d widget", mergedCount);
                _widgets.assign(mergedWidgets.begin(), mergedWidgets.end());
            }
        } else {
            std::unordered_map<uintptr_t, WidgetPtr> uniqueByMaskHash;
            WidgetPtrVec uniqueWidgets;
            int mergedCount = 0;
            for (const auto &w : _widgets) {
                if (!w) continue;
                uintptr_t keyMask = w->hashWithMask(_widgetKeyMask);
                auto it = uniqueByMaskHash.find(keyMask);
                if (it == uniqueByMaskHash.end()) {
                    uniqueByMaskHash[keyMask] = w;
                    uniqueWidgets.push_back(w);
                } else {
                    mergedCount++;
                    WidgetPtr representative = it->second;
                    uintptr_t repHash = representative->hash();
                    auto mergedIt = _mergedWidgets.find(repHash);
                    if (mergedIt == _mergedWidgets.end()) {
                        WidgetPtrVec vec;
                        vec.push_back(representative);
                        vec.push_back(w);
                        _mergedWidgets[repHash] = std::move(vec);
                    } else {
                        mergedIt->second.push_back(w);
                    }
                }
            }
            if (mergedCount != 0) {
                BDLOG("build state merged  %d widget", mergedCount);
                _widgets = std::move(uniqueWidgets);
            }
        }
#else
        WidgetPtrSet mergedWidgets;
        int mergedCount = mergeWidgetAndStoreMergedOnes(mergedWidgets);
        if (mergedCount != 0) {
            BDLOG("build state merged  %d widget", mergedCount);
            _widgets.assign(mergedWidgets.begin(), mergedWidgets.end());
        }
#endif
    }

    size_t ReuseState::getMaxWidgetsPerModelAction() const {
#if DYNAMIC_STATE_ABSTRACTION_ENABLED
        size_t maxCount = 1;
        for (const auto &p : _mergedWidgets) {
            if (p.second.size() > maxCount) {
                maxCount = p.second.size();
            }
        }
        return maxCount;
#else
        return State::getMaxWidgetsPerModelAction();
#endif
    }

#if DYNAMIC_STATE_ABSTRACTION_ENABLED
    uintptr_t ReuseState::getHashUnderMask(WidgetKeyMask mask) const {
        std::string activityString = (_activity && _activity.get()) ? *_activity : "";
        // Performance optimization: Use fast string hash instead of std::hash
        uintptr_t activityHash = (fastbotx::fastStringHash(activityString) * 31U) << 5;
        uintptr_t widgetsHash = 0x1;
        for (const auto &w : _widgets) {
            if (w) {
                widgetsHash ^= w->hashWithMask(mask);
            }
        }
        return activityHash ^ (widgetsHash << 1);
    }

    size_t ReuseState::getUniqueWidgetCountUnderMask(WidgetKeyMask mask) const {
        std::unordered_set<uintptr_t> seen;
        for (const auto &w : _widgets) {
            if (w) seen.insert(w->hashWithMask(mask));
        }
        return seen.size();
    }
#endif

} // namespace fastbotx


#endif // ReuseState_CPP_
