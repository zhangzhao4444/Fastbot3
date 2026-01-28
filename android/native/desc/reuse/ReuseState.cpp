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
#include <mutex>
#include "RichWidget.h"
#include "ActivityNameAction.h"
#include "../utils.hpp"
#include "ActionFilter.h"

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
        if (!element) {
            return;
        }
        // Check if this is the root element (no parent)
        if (element->getParent().expired()) {
            RectPtr bounds = element->getBounds();
            // Check bounds validity before using
            if (bounds != nullptr && !bounds->isEmpty()) {
                // Protect access to static State::_sameRootBounds (shared across all State subclasses).
                static std::mutex g_sameRootBoundsMutex;
                std::lock_guard<std::mutex> lock(g_sameRootBoundsMutex);

                // Initialize shared root bounds if empty
                if (_sameRootBounds && _sameRootBounds->isEmpty()) {
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
        if (!element) {
            BLOGE("ReuseState::buildStateFromElement: element is nullptr");
            return;
        }
        buildBoundingBox(element);
        // use RichWidget build the states
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
        if (!elem) {
            BLOGE("ReuseState::buildFromElement: elem is nullptr");
            return;
        }
        buildBoundingBox(elem);
        // Performance: elem is already ElementPtr, no need for dynamic_cast
        // This method is called recursively for children, using regular Widget
        // (root uses RichWidget via buildStateFromElement)
        WidgetPtr widget = std::make_shared<Widget>(parentWidget, elem);
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
                                     const NamingPtr &naming) {
        // Use new + shared_ptr instead of make_shared because constructor is protected
        ReuseStatePtr statePointer = std::shared_ptr<ReuseState>(new ReuseState(activityName));
        statePointer->buildState(element, naming);
        return statePointer;
    }

    void ReuseState::buildState(const ElementPtr &element, const NamingPtr &naming) {
        if (!element) {
            BLOGE("ReuseState::buildState: element is nullptr");
            return;
        }
        buildStateFromElement(nullptr, element);
        mergeWidgetsInState();
        if (naming) {
            buildStateKey(naming);
        } else {
            buildHashForState();
        }
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
        // Deprecated: use buildStateKey() with Naming instead.
        if (_activity) {
            std::string activityString = *(_activity.get());
            uintptr_t activityHash = (std::hash<std::string>{}(activityString) * 31U) << 5;
            activityHash ^= (combineHash<Widget>(_widgets, STATE_WITH_WIDGET_ORDER) << 1);
            _hashcode = activityHash;
        }
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
        // Performance: Pre-allocate capacity to avoid vector reallocations
        // Estimate action count: sum of all widget actions + 1 for back action
        size_t estimatedActionCount = 1; // Reserve space for back action
        for (const auto &widget: _widgets) {
            if (widget->getBounds() != nullptr) {
                estimatedActionCount += widget->getActions().size();
            }
        }
        _actions.reserve(estimatedActionCount);
        
        for (const auto &widget: _widgets) {
            if (widget->getBounds() == nullptr) {
                BLOGE("NULL Bounds happened");
                continue;
            }
            // Create action for each action type supported by this widget
            for (ActionType action: widget->getActions()) {
                // Performance: Use make_shared for single memory allocation
                // (constructor is public, so make_shared can be used)
                ActivityNameActionPtr activityNameAction = std::make_shared<ActivityNameAction>(
                        getActivityString(), widget, action);
                // Performance: emplace_back() constructs in-place, avoiding copy
                _actions.emplace_back(activityNameAction);
            }
        }
        
        // Always add back action for navigation
        _backAction = std::make_shared<ActivityNameAction>(getActivityString(), nullptr,
                                                           ActionType::BACK);
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
        WidgetPtrSet mergedWidgets;
        int mergedCount = mergeWidgetAndStoreMergedOnes(mergedWidgets);
        if (mergedCount != 0) {
            BDLOG("build state merged  %d widget", mergedCount);
            // Replace widgets with deduplicated set
            _widgets.assign(mergedWidgets.begin(), mergedWidgets.end());
        }
    }


} // namespace fastbot


#endif // ReuseState_CPP_
