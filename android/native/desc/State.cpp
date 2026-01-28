/*
 * This code is licensed under the Fastbot license. You may obtain a copy of this license in the LICENSE.txt file in the root directory of this source tree.
 */
/**
 * @authors Jianqiang Guo, Yuhui Su, Zhao Zhang
 */
#ifndef State_CPP_
#define State_CPP_

#include "../Base.h"
#include  "State.h"
#include "../utils.hpp"
#include "ActionFilter.h"
#include <regex>
#include <map>
#include <algorithm>
#include <utility>
#include <cmath>
#include <sstream>
#include <cinttypes>
#include <mutex>

namespace fastbotx {

    State::State()
            : Node(), _hasNoDetail(false) {
    }

    State::State(stringPtr activityName)
            : Node(), _activity(std::move(activityName)), _hasNoDetail(false) {
        BLOG("create state");
    }

    /**
     * @brief Merge duplicate widgets and store merged ones
     * 
     * Identifies duplicate widgets (by hash) and stores them in the merged widgets map.
     * This is used for widget deduplication to optimize state comparison and action selection.
     * 
     * Performance optimization:
     * - Uses set for O(log n) duplicate detection
     * - Only processes if merge is enabled and widgets exist
     * 
     * @param mergeWidgets Set to track unique widgets (output parameter)
     * @return Number of widgets that were merged (duplicates found)
     */
    int State::mergeWidgetAndStoreMergedOnes(WidgetPtrSet &mergeWidgets) {
        int mergedWidgetCount = 0;
        if (STATE_MERGE_DETAIL_TEXT && !this->_widgets.empty()) {
            for (const auto &widgetPtr: this->_widgets) {
                // Null pointer check for defensive programming
                if (widgetPtr == nullptr) {
                    BLOGE("mergeWidgetAndStoreMergedOnes: found nullptr widget, skipping");
                    continue;
                }
                // Try to insert widget into set (returns false if duplicate)
                auto noMerged = mergeWidgets.emplace(widgetPtr).second;
                if (!noMerged) {
                    // Widget is a duplicate, store in merged widgets map
                    uintptr_t h = widgetPtr->hash();
                    mergedWidgetCount++;
                    
                    // Performance: Use find instead of count + at to avoid double lookup
                    auto mergedIt = this->_mergedWidgets.find(h);
                    if (mergedIt == this->_mergedWidgets.end()) {
                        // First duplicate for this hash, create new vector
                        WidgetPtrVec tempWidgetVector;
                        tempWidgetVector.emplace_back(widgetPtr);
                        this->_mergedWidgets.emplace(h, std::move(tempWidgetVector));
                    } else {
                        // Additional duplicate, add to existing vector
                        mergedIt->second.emplace_back(widgetPtr);
                    }
                }
            }
        }
        return mergedWidgetCount;
    }

    /**
     * @brief Factory method to create a State from Element and activity name
     * 
     * Creates a new State object by:
     * 1. Building widget tree from Element
     * 2. Merging duplicate widgets
     * 3. Computing state hash
     * 4. Creating actions for all widgets
     * 5. Adding back action
     * 
     * Performance optimizations:
     * - Uses move semantics for activity name
     * - Efficient widget merging
     * - Pre-allocates action vector capacity if possible
     * 
     * @param elem Root Element of the UI hierarchy
     * @param activityName Activity name string pointer
     * @return Shared pointer to created State
     */
    StatePtr State::create(ElementPtr elem, stringPtr activityName) {
        // Use new + shared_ptr instead of make_shared because constructor is protected
        // and make_shared cannot access protected constructors from outside the class
        StatePtr sharedPtr = std::shared_ptr<State>(new State(std::move(activityName)));
        sharedPtr->buildFromElement(nullptr, std::move(elem));
        
        // Compute hash based on activity name (fallback when naming is not available)
        uintptr_t activityHash;
        if (sharedPtr->_activity == nullptr || sharedPtr->_activity.get() == nullptr) {
            BLOGE("State::create: activity is nullptr, using empty string for hash");
            activityHash = (std::hash<std::string>{}("") * 31U) << 5;
        } else {
            activityHash =
                    (std::hash<std::string>{}(*(sharedPtr->_activity.get())) * 31U) << 5;
        }
        
        // Merge duplicate widgets for optimization
        WidgetPtrSet mergedWidgets;
        int mergedWidgetCount = sharedPtr->mergeWidgetAndStoreMergedOnes(mergedWidgets);
        if (mergedWidgetCount != 0) {
            BDLOG("build state merged  %d widget", mergedWidgetCount);
            // Use merged widgets (deduplicated) instead of original widgets
            sharedPtr->_widgets.assign(mergedWidgets.begin(), mergedWidgets.end());
            
            // If widget order matters for hash computation, sort by hash to ensure consistency
            // This ensures that same set of widgets always produces same hash regardless of
            // the order they were inserted into the set
            if (STATE_WITH_WIDGET_ORDER) {
                std::sort(sharedPtr->_widgets.begin(), sharedPtr->_widgets.end(),
                          [](const WidgetPtr& a, const WidgetPtr& b) {
                              if (a == nullptr || b == nullptr) {
                                  return a != nullptr; // nullptr widgets go to end
                              }
                              return a->hash() < b->hash();
                          });
            }
        }
        
        // Combine activity hash with widget hash
        activityHash ^= (combineHash<Widget>(sharedPtr->_widgets, STATE_WITH_WIDGET_ORDER) << 1);
        sharedPtr->_hashcode = activityHash;
        
        // Build actions for all widgets
        // Performance: Pre-allocate capacity to avoid reallocations
        // Estimate: average 1-2 actions per widget, plus back action
        size_t estimatedActionCount = sharedPtr->_widgets.size() * 2 + 1;
        sharedPtr->_actions.reserve(estimatedActionCount);
        
        for (const auto &w: sharedPtr->_widgets) {
            // Null pointer check for widget itself
            if (w == nullptr) {
                BLOGE("NULL Widget happened");
                continue;
            }
            // Null pointer check for bounds
            if (w->getBounds() == nullptr) {
                BLOGE("NULL Bounds happened for widget");
                continue;
            }
            // Create action for each action type supported by this widget
            for (ActionType act: w->getActions()) {
                ActivityStateActionPtr modelAction = std::make_shared<ActivityStateAction>(
                        sharedPtr, w, act);
                sharedPtr->_actions.emplace_back(modelAction);
            }
        }
        
        // Always add back action for navigation
        sharedPtr->_backAction = std::make_shared<ActivityStateAction>(sharedPtr, nullptr,
                                                                       ActionType::BACK);
        sharedPtr->_actions.emplace_back(sharedPtr->_backAction);

        return sharedPtr;
    }

    void State::buildStateKey(const NamingPtr &naming) {
        if (!naming) {
            return;
        }
        _currentNaming = naming;
        Naming::NamingResult result = naming->namingWidgets(_widgets, _rootBounds);
        _nameWidgets = result.names;
        _widgetNames.clear();
        for (size_t i = 0; i < result.nodes.size() && i < result.names.size(); i++) {
            if (result.nodes[i] && result.names[i]) {
                _widgetNames[result.nodes[i]->hash()] = result.names[i];
            }
        }
        std::string activity = _activity ? *_activity.get() : "";
        _stateKey = std::make_shared<StateKey>(activity, naming, _nameWidgets);
        _hashcode = _stateKey->hash();
    }

    /**
     * @brief Check if an action is saturated (visited too many times)
     * 
     * An action is considered saturated if:
     * - For actions without targets: visited at least once
     * - For actions with targets: visited more times than the number of merged widgets
     *   with the same hash (to account for duplicate widgets)
     * 
     * Performance optimization:
     * - Uses find instead of count + at to avoid double lookup
     * 
     * @param action Action to check
     * @return true if action is saturated (should be avoided)
     */
    bool State::isSaturated(const ActivityStateActionPtr &action) const {
        if (action == nullptr) {
            return false;
        }
        
        // Actions without targets are saturated if visited at least once
        if (!action->requireTarget()) {
            return action->isVisited();
        }
        
        // Actions with targets: check if visited more times than merged widget count
        const WidgetPtr& target = action->getTarget();
        if (target == nullptr) {
            // No target but requires target: default to saturated if visited
            return action->getVisitedCount() >= 1;
        }
        
        uintptr_t h = target->hash();
        auto mergedIt = this->_mergedWidgets.find(h);
        if (mergedIt != this->_mergedWidgets.end()) {
            // Action is saturated if visited more times than merged widget count
            return action->getVisitedCount() > static_cast<int>(mergedIt->second.size());
        }
        
        // Target not found in merged widgets: default to saturated if visited at least once
        return action->getVisitedCount() >= 1;
    }

    bool State::isSaturated() const {
        for (const auto &action : _actions) {
            if (!action) {
                continue;
            }
            // Check if action is enabled and valid
            if (!action->getEnabled() || !action->isValid()) {
                continue;
            }
            // If any action is not saturated, state is not saturated
            if (!isSaturated(action)) {
                return false;
            }
        }
        return true;
    }

    int State::getMergedWidgetCount(uintptr_t widgetHash) const {
        auto mergedIt = this->_mergedWidgets.find(widgetHash);
        if (mergedIt != this->_mergedWidgets.end()) {
            return static_cast<int>(mergedIt->second.size());
        }
        return 0;
    }

    NamePtr State::getNameForWidgetHash(uintptr_t widgetHash) const {
        auto iter = _widgetNames.find(widgetHash);
        if (iter != _widgetNames.end()) {
            return iter->second;
        }
        return nullptr;
    }

    ActivityStateActionPtr State::getActionByTargetHash(ActionType type, uintptr_t widgetHash) const {
        for (const auto &action : _actions) {
            if (!action || !action->requireTarget()) {
                continue;
            }
            auto target = action->getTarget();
            if (!target) {
                continue;
            }
            if (action->getActionType() == type && target->hash() == widgetHash) {
                return action;
            }
        }
        return nullptr;
    }

    ActivityStateActionPtr State::getActionByType(ActionType type) const {
        for (const auto &action : _actions) {
            if (!action || action->requireTarget()) {
                continue;
            }
            if (action->getActionType() == type) {
                return action;
            }
        }
        return nullptr;
    }

    ActivityStateActionPtr State::relocateAction(ActionType type, const NamePtr &targetName) const {
        if (!targetName) {
            return getActionByType(type);
        }
        ActivityStateActionPtr best;
        size_t bestFineness = 0;
        for (const auto &action : _actions) {
            if (!action || !action->requireTarget()) {
                continue;
            }
            if (action->getActionType() != type) {
                continue;
            }
            auto target = action->getTarget();
            if (!target) {
                continue;
            }
            NamePtr candidateName = getNameForWidgetHash(target->hash());
            if (!candidateName || !candidateName->getNamer()) {
                continue;
            }
            if (!candidateName->refinesTo(targetName) && !targetName->refinesTo(candidateName)) {
                continue;
            }
            size_t fineness = candidateName->getNamer()->getNamerTypes().size();
            if (!best || fineness > bestFineness) {
                best = action;
                bestFineness = fineness;
            }
        }
        return best;
    }

    void State::appendTree(const ElementPtr &tree) {
        if (!tree) {
            return;
        }
        _treeHistory.emplace_back(tree);
    }

    ElementPtr State::getLatestTree() const {
        if (_treeHistory.empty()) {
            return nullptr;
        }
        return _treeHistory.back();
    }

    void State::resolveActionTargets(const ElementPtr &tree) {
        if (!tree) {
            return;
        }
        for (const auto &action : _actions) {
            if (!action || !action->requireTarget()) {
                continue;
            }
            auto target = action->getTarget();
            if (!target) {
                continue;
            }
            auto xpath = std::make_shared<Xpath>();
            xpath->clazz = target->getClassName();
            xpath->resourceID = target->getResourceID();
            xpath->text = target->getText();
            xpath->contentDescription = target->getContentDesc();
            xpath->index = target->getIndex();
            xpath->operationAND = true;

            std::vector<ElementPtr> matched;
            if (tree->matchXpathSelector(xpath)) {
                matched.emplace_back(tree);
            }
            tree->recursiveElements([&](const ElementPtr &elem) {
                return elem->matchXpathSelector(xpath);
            }, matched);
            action->setResolvedNodes(matched);
        }
    }

    RectPtr State::_sameRootBounds = std::make_shared<Rect>();
    namespace {
        // Protect static shared root bounds across threads/states.
        std::mutex g_sameRootBoundsMutex;
    }

    void State::buildFromElement(WidgetPtr parentWidget, ElementPtr elem) {
        // Handle root element bounds
        if (elem != nullptr && elem->getParent().expired()) {
            RectPtr elemBounds = elem->getBounds();
            if (elemBounds != nullptr && !elemBounds->isEmpty()) {
                // Protect access to static _sameRootBounds to avoid races under concurrent state builds.
                std::lock_guard<std::mutex> lock(g_sameRootBoundsMutex);

                // Initialize static root bounds (only on first call)
                if (_sameRootBounds && _sameRootBounds->isEmpty()) {
                    _sameRootBounds = elemBounds;
                }

                // If current bounds match static bounds, use static reference (save memory)
                if (equals(_sameRootBounds, elemBounds)) {
                    this->_rootBounds = _sameRootBounds;
                } else {
                    // Different bounds, use current bounds
                    this->_rootBounds = elemBounds;
                }
            }
        }
        
        // Create widget from element
        if (elem == nullptr) {
            BLOGE("buildFromElement: elem is nullptr");
            return;
        }
        
        WidgetPtr widget = std::make_shared<Widget>(parentWidget, elem);
        this->_widgets.emplace_back(widget);
        
        // Recursively process children
        for (const auto &childElement: elem->getChildren()) {
            if (!childElement) {
                continue;
            }
            buildFromElement(widget, childElement);
        }
    }

    uintptr_t State::hash() const {
        return this->_hashcode;
    }

    bool State::operator<(const State &state) const {
        return this->hash() < state.hash();
    }

    State::~State() {
        this->_activity.reset();
        this->_actions.clear();
        this->_backAction = nullptr;
        this->_widgets.clear();

        this->_mergedWidgets.clear();
    }


    void State::clearDetails() {
        for (auto const &widget: this->_widgets) {
            if (widget != nullptr) {
                widget->clearDetails();
            }
        }
        this->_mergedWidgets.clear();
        _hasNoDetail = true;
    }

    void State::fillDetails(const std::shared_ptr<State> &copy) {
        if (copy == nullptr) {
            BLOGE("fillDetails: copy state is nullptr");
            return;
        }
        
        for (const auto &widgetPtr: this->_widgets) {
            if (widgetPtr == nullptr) {
                BLOGE("fillDetails: found nullptr widget, skipping");
                continue;
            }
            auto widgetIterator = std::find_if(copy->_widgets.begin(), copy->_widgets.end(),
                                               [&widgetPtr](const WidgetPtr &cw) {
                                                   if (cw == nullptr || widgetPtr == nullptr) {
                                                       return false;
                                                   }
                                                   return *(cw.get()) == *widgetPtr;
                                               });
            if (widgetIterator != copy->_widgets.end() && *widgetIterator != nullptr) {
                widgetPtr->fillDetails(*widgetIterator);
            } else {
                LOGE("ERROR can not refill widget");
            }
        }
        for (const auto &miter: this->_mergedWidgets) {
            auto mkw = copy->_mergedWidgets.find(miter.first);
            if (mkw == copy->_mergedWidgets.end())
                continue;
            for (const auto &widgetPtr: miter.second) {
                if (widgetPtr == nullptr) {
                    continue;
                }
                auto widgetIterator = std::find_if((*mkw).second.begin(), (*mkw).second.end(),
                                                   [&widgetPtr](const WidgetPtr &cw) {
                                                       if (cw == nullptr || widgetPtr == nullptr) {
                                                           return false;
                                                       }
                                                       return *(cw.get()) == *widgetPtr;
                                                   });
                if (widgetIterator != (*mkw).second.end() && *widgetIterator != nullptr) {
                    widgetPtr->fillDetails(*widgetIterator);
                }
            }

        }
        _hasNoDetail = false;
    }

    std::string State::toString() const {
        std::ostringstream oss;
        oss << "{state: " << this->hash() << "\n    widgets: \n";
        for (auto const &widget: this->_widgets) {
            if (widget != nullptr) {
                oss << "   " << widget->toString() << "\n";
            } else {
                oss << "   [null widget]\n";
            }
        }
        oss << "action: \n";
        for (auto const &action: this->_actions) {
            if (action != nullptr) {
                oss << "   " << action->toString() << "\n";
            } else {
                oss << "   [null action]\n";
            }
        }
        oss << "\n}";
        return oss.str();
    }


    // for algorithm
    int State::countActionPriority(const ActionFilterPtr &filter, bool includeBack) const {
        int totalP = 0;
        for (const auto &action: this->_actions) {
            if (!includeBack && action->isBack()) {
                continue;
            }
            if (filter->include(action)) {
                int fp = filter->getPriority(action);
                if (fp <= 0) {
                    BDLOG("Error: Action should has a positive priority, but we get %d", fp);
                    return -1;
                }
                totalP += fp;
            }
        }
        return totalP;
    }

    ActivityStateActionPtrVec State::targetActions() const {
        ActivityStateActionPtrVec retV;
        ActionFilterPtr filter = targetFilter; //(ActionFilterPtr(new ActionFilterTarget());)
        for (const auto &a: this->_actions) {
            if (filter->include(a))
                retV.emplace_back(a);
        }
        return retV;
    }

    ActivityStateActionPtr State::greedyPickMaxQValue(const ActionFilterPtr &filter) const {
        ActivityStateActionPtr retA;
        long maxvalue = 0;
        for (const auto &m: this->_actions) {
            if (!filter->include(m))
                continue;
            if (filter->getPriority(m) > maxvalue) {
                maxvalue = filter->getPriority(m);
                retA = m;
            }
        }
        return retA;
    }

    ActivityStateActionPtr State::randomPickAction(const ActionFilterPtr &filter) const {
        return this->randomPickAction(filter, true);
    }

    ActivityStateActionPtr
    State::randomPickAction(const ActionFilterPtr &filter, bool includeBack) const {
        int total = this->countActionPriority(filter, includeBack);
        if (total == 0)
            return nullptr;
        // Use thread-local random number generator for better performance
        int index = randomInt(0, total);
        return pickAction(filter, includeBack, index);
    }

    ActivityStateActionPtr
    State::pickAction(const ActionFilterPtr &filter, bool includeBack, int index) const {
        int ii = index;
        for (auto action: this->_actions) {
            if (!includeBack && action->isBack())
                continue;
            if (filter->include(action)) {
                int p = filter->getPriority(action);
                if (p > ii)
                    return action;
                else
                    ii = ii - p;
            }
        }
        BDLOG("%s", "ERROR: action filter is unstable");
        return nullptr;
    }

    ActivityStateActionPtr State::randomPickUnvisitedAction() const {
        ActivityStateActionPtr action = this->randomPickAction(enableValidUnvisitedFilter, false);
        if (action == nullptr && enableValidUnvisitedFilter->include(getBackAction())) {
            action = getBackAction();
        }
        return action;
    }


    ActivityStateActionPtr State::resolveAt(ActivityStateActionPtr action, time_t /*t*/) {
        if (action == nullptr) {
            return action;
        }
        
        if (action->getTarget() == nullptr) {
            return action;
        }

        const auto &resolvedNodes = action->getResolvedNodes();
        if (!resolvedNodes.empty()) {
            int total = static_cast<int>(resolvedNodes.size());
            if (total > 0) {
                int index = action->getVisitedCount() % total;
                action->setResolvedNode(resolvedNodes[index]);
                return action;
            }
        }
        
        uintptr_t h = action->getTarget()->hash();
        auto targetWidgets = this->_mergedWidgets.find(h);
        if (targetWidgets == this->_mergedWidgets.end()) {
            return action;
        }
        
        int total = static_cast<int>(targetWidgets->second.size());
        // Safety check: ensure total is positive to avoid division by zero
        if (total <= 0) {
            BLOGE("resolveAt: merged widgets vector is empty for hash %" PRIuPTR, h);
            return action;
        }
        
        int index = action->getVisitedCount() % total;
        BLOG("resolve a merged widget %d/%d for action %s", index, total, action->getId().c_str());
        action->setTarget(targetWidgets->second[index]);
        return action;
    }

    bool State::containsTarget(const WidgetPtr &widget) const {
        if (widget == nullptr) {
            return false;
        }
        for (const auto &w: this->_widgets) {
            if (equals(w, widget))
                return true;
        }
        return false;
    }

    PropertyIDPrefixImpl(State, "g0s");

    bool State::operator==(const State &state) const {
        return this->hash() == state.hash();
    }


} // namespace fastbot


#endif // State_CPP_
