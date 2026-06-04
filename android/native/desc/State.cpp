/*
 * This code is licensed under the Fastbot license. You may obtain a copy of this license in the LICENSE.txt file in the root directory of this source tree.
 */
/**
 * @file State.cpp
 *
 * Base `State` construction from accessibility trees, hashing with optional ordered widget combine,
 * duplicate-widget merge buckets, action lists, detail stripping for memory, `fillDetails` resync,
 * and weighted action sampling helpers.
 *
 * @authors Jianqiang Guo, Yuhui Su, Zhao Zhang
 */
#ifndef State_CPP_
#define State_CPP_

#include "../Base.h"
#include  "State.h"
#include "../utils.hpp"
#include "ActionFilter.h"
#include "Preference.h"
#include <regex>
#include <map>
#include <algorithm>
#include <utility>
#include <cmath>
#include <sstream>
#include <cinttypes>
#include <atomic>
#include <unordered_set>

namespace fastbotx {
namespace {
    /** Passed to `combineHash` so widget ordering is stable after deduplication sort (matches legacy behavior). */
    constexpr bool kStateCombineHashWithOrder = true;

    /** Activity short label for diagnostic logs (fallback "?"). */
    std::string stateActivityLabel(const State &s) {
        const stringPtr ap = s.getActivityString();
        return (ap && ap.get()) ? *ap : std::string("?");
    }

    /** Counts rows in `v` sharing the same widget hash (merged-group diagnostics). */
    size_t countWidgetsWithHash(const WidgetPtrVec &v, uintptr_t h) {
        size_t c = 0;
        for (const auto &w : v) {
            if (w && w->hash() == h) {
                ++c;
            }
        }
        return c;
    }

    /** Compact widget list snippet for mismatch logs. */
    std::string summarizeStateWidgets(const State &s, size_t limit = 3) {
        std::ostringstream oss;
        size_t n = 0;
        for (const auto &w : s.getWidgets()) {
            if (!w) {
                continue;
            }
            if (n != 0) {
                oss << " | ";
            }
            oss << "#" << n << "=" << w->getClass() << ":" << w->getResourceID();
            ++n;
            if (n >= limit) {
                break;
            }
        }
        if (n == 0) {
            return std::string("(empty)");
        }
        if (s.getWidgets().size() > n) {
            oss << " | ... total=" << s.getWidgets().size();
        }
        return oss.str();
    }

    /** Compact action list snippet for mismatch logs. */
    std::string summarizeStateActions(const State &s, size_t limit = 3) {
        std::ostringstream oss;
        size_t n = 0;
        for (const auto &a : s.getActions()) {
            if (!a) {
                continue;
            }
            if (n != 0) {
                oss << " | ";
            }
            const WidgetPtr tgt = a->getTarget();
            oss << "#" << n << "=" << static_cast<int>(a->getActionType()) << ":";
            if (tgt) {
                oss << tgt->getClass() << ":" << tgt->getResourceID();
            } else {
                oss << "(no-target)";
            }
            ++n;
            if (n >= limit) {
                break;
            }
        }
        if (n == 0) {
            return std::string("(empty)");
        }
        if (s.getActions().size() > n) {
            oss << " | ... total=" << s.getActions().size();
        }
        return oss.str();
    }

} // namespace

    State::State()
            : Node(), _hasNoDetail(false) {
    }

    /** Binds activity string used in hashing and logs; widgets/actions filled by `buildFromElement` / `create`. */
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
        
        // Compute base hash from activity name
        // Performance optimization: Use fast string hash instead of std::hash
        uintptr_t activityHash;
        if (sharedPtr->_activity == nullptr || sharedPtr->_activity.get() == nullptr) {
            BLOGE("State::create: activity is nullptr, using empty string for hash");
            activityHash = (fastbotx::fastStringHash("") * 31U) << 5;
            // Continue with empty activity hash (should not happen in normal flow)
        } else {
            activityHash =
                    (fastbotx::fastStringHash(*(sharedPtr->_activity.get())) * 31U) << 5;
        }
        
        // Merge duplicate widgets for optimization
        WidgetPtrSet mergedWidgets;
        int mergedWidgetCount = sharedPtr->mergeWidgetAndStoreMergedOnes(mergedWidgets);
        if (mergedWidgetCount != 0) {
            BDLOG("build state merged  %d widget", mergedWidgetCount);
            // Performance optimization: Pre-allocate vector capacity before assign
            // This avoids multiple reallocations during assign operation
            sharedPtr->_widgets.clear();
            sharedPtr->_widgets.reserve(mergedWidgets.size());
            // Use merged widgets (deduplicated) instead of original widgets
            sharedPtr->_widgets.assign(mergedWidgets.begin(), mergedWidgets.end());
            
            // If widget order matters for hash computation, sort by hash to ensure consistency
            // This ensures that same set of widgets always produces same hash regardless of
            // the order they were inserted into the set
            if (kStateCombineHashWithOrder) {
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
        activityHash ^=
                (combineHash<Widget>(sharedPtr->_widgets, kStateCombineHashWithOrder) << 1);
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
                RectPtr wb = w->getBounds();
                RectPtr ab = modelAction->getTarget() ? modelAction->getTarget()->getBounds() : nullptr;
                const bool boundsMismatch =
                    wb && ab &&
                    (wb->left != ab->left || wb->top != ab->top ||
                     wb->right != ab->right || wb->bottom != ab->bottom);
                if (boundsMismatch) {
                    auto actPtr = sharedPtr->getActivityString();
                    const std::string actName = (actPtr && actPtr.get()) ? *actPtr : std::string();
                    BDLOG("state actions: widget_action_bounds_mismatch activity=%s state=%zu widgetHash=%zu "
                          "actType=%d class=%s rid=%s widgetBounds=%s actionBounds=%s",
                          actName.c_str(), sharedPtr->hash(), w->hash(), static_cast<int>(act),
                          w->getClass().c_str(), w->getResourceID().c_str(),
                          wb->toString().c_str(), ab->toString().c_str());
                }
                if (!wb || wb->isEmpty()) {
                    auto actPtr = sharedPtr->getActivityString();
                    const std::string actName = (actPtr && actPtr.get()) ? *actPtr : std::string();
                    BDLOG("state actions: zero_or_missing_target_bounds activity=%s state=%zu widgetHash=%zu "
                          "actType=%d class=%s rid=%s bounds=%s",
                          actName.c_str(), sharedPtr->hash(), w->hash(), static_cast<int>(act),
                          w->getClass().c_str(), w->getResourceID().c_str(),
                          wb ? wb->toString().c_str() : "(null)");
                }
                if (!ab || ab->isEmpty()) {
                    auto actPtr = sharedPtr->getActivityString();
                    const std::string actName = (actPtr && actPtr.get()) ? *actPtr : std::string();
                    BDLOG("state actions: action_target_bounds_empty activity=%s state=%zu widgetHash=%zu "
                          "actType=%d class=%s rid=%s widgetBounds=%s actionBounds=%s",
                          actName.c_str(), sharedPtr->hash(), w->hash(), static_cast<int>(act),
                          w->getClass().c_str(), w->getResourceID().c_str(),
                          wb ? wb->toString().c_str() : "(null)",
                          ab ? ab->toString().c_str() : "(null)");
                }
                sharedPtr->_actions.emplace_back(modelAction);
            }
        }
        
        // Always add back action for navigation
        sharedPtr->_backAction = std::make_shared<ActivityStateAction>(sharedPtr, nullptr,
                                                                       ActionType::BACK);
        sharedPtr->_actions.emplace_back(sharedPtr->_backAction);

        return sharedPtr;
    }

    /**
     * @brief Check if an action is saturated (visited too many times)
     * 
     * An action is considered saturated if:
     * - For actions without targets: visited at least once
     * - For actions with targets: visited enough times to cover every concrete widget represented
     *   by the abstract target (representative widget + merged duplicates)
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

        const size_t totalTargets = getConcreteTargetCount(target);
        if (totalTargets == 0) {
            return action->getVisitedCount() >= 1;
        }
        return action->getVisitedCount() >= static_cast<int>(totalTargets);
    }

    size_t State::getConcreteTargetCount(const WidgetPtr &target) const {
        if (!target) {
            return 0;
        }
        size_t total = 1; // representative widget stored in `_widgets`
        auto mergedIt = this->_mergedWidgets.find(target->hash());
        if (mergedIt != this->_mergedWidgets.end()) {
            total += mergedIt->second.size();
        }
        return total;
    }

    size_t State::getMaxWidgetsPerModelAction() const {
        size_t maxCount = 1;
        for (const auto &w : this->_widgets) {
            if (!w) continue;
            size_t n = 1;
            auto it = this->_mergedWidgets.find(w->hash());
            if (it != this->_mergedWidgets.end()) {
                n += it->second.size();
            }
            if (n > maxCount) maxCount = n;
        }
        return maxCount;
    }

    RectPtr State::_sameRootBounds = std::make_shared<Rect>();

    namespace {
        // Helper function to estimate widget count from element tree
        // Counts elements that are likely to become widgets (clickable, scrollable, checkable, etc.)
        size_t estimateWidgetCount(const ElementPtr &elem) {
            if (!elem) return 0;
            size_t count = 0;
            // Count this element if it's actionable
            if (elem->getClickable() || elem->getScrollable() || elem->getCheckable() 
                || elem->getLongClickable() || elem->isEditText()) {
                count = 1;
            }
            // Recursively count children
            for (const auto &child : elem->getChildren()) {
                count += estimateWidgetCount(child);
            }
            return count;
        }
    }

    void State::buildFromElement(WidgetPtr parentWidget, ElementPtr elem) {
        // Handle root element bounds
        if (elem != nullptr && elem->getParent().expired()) {
            RectPtr elemBounds = elem->getBounds();
            if (elemBounds != nullptr && !elemBounds->isEmpty()) {
                // Initialize static root bounds (only on first call)
                if (_sameRootBounds->isEmpty()) {
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
            
            // Performance optimization: Pre-allocate widgets vector capacity for root element
            // Estimate widget count to avoid multiple reallocations during recursive build
            size_t estimatedCount = estimateWidgetCount(elem);
            if (estimatedCount > 0) {
                this->_widgets.reserve(estimatedCount);
            }
        }
        
        // Create widget from element
        if (elem == nullptr) {
            BLOGE("buildFromElement: elem is nullptr");
            return;
        }
        
        WidgetPtr widget = std::make_shared<Widget>(parentWidget, elem);
        this->_widgets.emplace_back(widget);
        RectPtr wb = widget ? widget->getBounds() : nullptr;
        if (!wb || wb->isEmpty()) {
            auto actPtr = this->getActivityString();
            const std::string actName = (actPtr && actPtr.get()) ? *actPtr : std::string();
            RectPtr eb = elem ? elem->getBounds() : nullptr;
            BDLOG("state build: widget_empty_bounds activity=%s state=%zu widgetHash=%zu class=%s rid=%s "
                  "elemBounds=%s widgetBounds=%s",
                  actName.c_str(), this->hash(), widget ? widget->hash() : 0,
                  widget ? widget->getClass().c_str() : "(null)",
                  widget ? widget->getResourceID().c_str() : "(null)",
                  eb ? eb->toString().c_str() : "(null)",
                  wb ? wb->toString().c_str() : "(null)");
        }
        
        // Recursively process children
        for (const auto &childElement: elem->getChildren()) {
            buildFromElement(widget, childElement);
        }
    }

    /** Fixed after construction (`create` / dynamic abstraction); equality uses this value. */
    uintptr_t State::hash() const {
        return this->_hashcode;
    }

#if DYNAMIC_STATE_ABSTRACTION_ENABLED
    /** Overrides `_hashcode` with the identity produced by dynamic abstraction (mask/coarsening pipeline). */
    void State::applyDynamicAbstractionIdentityHash(uintptr_t apeStateKeyHash) {
        this->_hashcode = apeStateKeyHash;
        this->_usesApeIdentityHash = true;
    }

    uintptr_t State::getHashUnderMask(WidgetKeyMask /*mask*/) const {
        return this->hash();
    }

    size_t State::getWidgetsWithNonEmptyTextCount() const {
        size_t n = 0;
        for (const auto &w : _widgets) {
            if (w && !w->getText().empty()) ++n;
        }
        return n;
    }

    size_t State::getUniqueWidgetCountUnderMask(WidgetKeyMask /*mask*/) const {
        return _widgets.size();
    }

    void State::filterActionsByKeepMask(const std::vector<uint8_t> &keepMask) {
        if (keepMask.size() != _actions.size()) {
            return;
        }

        ActivityStateActionPtrVec filtered;
        filtered.reserve(_actions.size());
        ActivityStateActionPtr back = nullptr;

        for (size_t i = 0; i < _actions.size(); ++i) {
            const auto &a = _actions[i];
            if (!a) {
                continue;
            }

            if (a->isBack()) {
                filtered.push_back(a);
                back = a;
                continue;
            }

            if (keepMask[i] != 0) {
                filtered.push_back(a);
            }
        }

        if (!back && _backAction) {
            filtered.push_back(_backAction);
            back = _backAction;
        }

        _actions.swap(filtered);
        if (back) {
            _backAction = back;
        }
    }
#endif

    bool State::operator<(const State &state) const {
        return this->hash() < state.hash();
    }

    /** Drops activity reference and clears widgets/actions (merged buckets included). */
    State::~State() {
        this->_activity.reset();
        this->_actions.clear();
        this->_backAction = nullptr;
        this->_widgets.clear();

        this->_mergedWidgets.clear();
    }


    /**
     * Strips non-essential widget text when allowed; keeps targets of current actions.
     * When merged-state overview is enabled in preferences, retains strings for planner-facing widgets.
     */
    void State::clearDetails() {
        auto pref = Preference::inst();
        const bool keepWidgetTextForPlanner =
            (pref != nullptr) && pref->isLlmdroidEnabled();
        std::unordered_set<const Widget *> actionTargetWidgets;
        actionTargetWidgets.reserve(this->_actions.size());
        for (const auto &action : this->_actions) {
            if (!action) {
                continue;
            }
            const WidgetPtr target = action->getTarget();
            if (target) {
                actionTargetWidgets.insert(target.get());
            }
        }

        if (!keepWidgetTextForPlanner) {
            for (auto const &widget: this->_widgets) {
                if (widget != nullptr) {
                    if (actionTargetWidgets.count(widget.get()) != 0) {
                        continue;
                    }
                    widget->clearDetails();
                }
            }
        }
        this->_mergedWidgets.clear();
        _hasNoDetail = true;
    }

    /**
     * Copies rich widget fields from a fresher `State` snapshot onto this canonical row when hashes align.
     * Handles merged-widget buckets and logs structural mismatches.
     */
    void State::fillDetails(const std::shared_ptr<State> &copy, const char *debugFrom) {
        if (copy == nullptr) {
            BLOGE("fillDetails: copy state is nullptr (from=%s)", debugFrom ? debugFrom : "?");
            return;
        }

        const std::string actCanon = stateActivityLabel(*this);
        const std::string actFresh = stateActivityLabel(*copy);
        const uintptr_t stCanon = this->hash();
        const uintptr_t stFresh = copy->hash();
#if DYNAMIC_STATE_ABSTRACTION_ENABLED
        const int dynAbsCanon = this->usesDynamicAbstractionIdentityHash() ? 1 : 0;
        const int dynAbsFresh = copy->usesDynamicAbstractionIdentityHash() ? 1 : 0;
#else
        const int dynAbsCanon = 0;
        const int dynAbsFresh = 0;
#endif
        const char *fromTag = debugFrom ? debugFrom : "?";
        if (debugFrom) {
            static std::atomic<uint64_t> g_fd_enter{0};
            const uint64_t en = ++g_fd_enter;
            if (en <= 24 || (en % 400) == 0) {
                BDLOG(
                    "state chain: fillDetails enter from=%s canonId=%d freshId=%d st=%" PRIuPTR "/%" PRIuPTR
                    " stEq=%d nw=%zu/%zu noDet=%d/%d",
                    debugFrom, getIdi(), copy->getIdi(), static_cast<uintptr_t>(stCanon),
                    static_cast<uintptr_t>(stFresh), stCanon == stFresh ? 1 : 0, this->_widgets.size(),
                    copy->_widgets.size(), hasNoDetail() ? 1 : 0, copy->hasNoDetail() ? 1 : 0);
            }
        }

        // Same abstract state hash but widget row count drifted: rebuild lists from the fresh snapshot.
        if (hasNoDetail() && this->_widgets.size() != copy->_widgets.size()) {
            const size_t nwWas = this->_widgets.size();
            const std::string canonWidgetSummary = summarizeStateWidgets(*this);
            const std::string freshWidgetSummary = summarizeStateWidgets(*copy);
            const std::string canonActionSummary = summarizeStateActions(*this);
            const std::string freshActionSummary = summarizeStateActions(*copy);
            this->_widgets = copy->_widgets;
            this->_mergedWidgets = copy->_mergedWidgets;
            _hasNoDetail = false;
            BDLOG(
                "fillDetails: widget list resync from fresh (count mismatch) from=%s nw_was=%zu nw_fresh=%zu",
                fromTag, nwWas, copy->_widgets.size());
            BDLOG(
                "fillDetails: mismatch details from=%s canonId=%d freshId=%d act=%s/%s "
                "stateHash=%" PRIuPTR "/%" PRIuPTR " actions=%zu/%zu",
                fromTag, getIdi(), copy->getIdi(), actCanon.c_str(), actFresh.c_str(),
                static_cast<uintptr_t>(stCanon), static_cast<uintptr_t>(stFresh),
                this->_actions.size(), copy->_actions.size());
            BDLOG("fillDetails: mismatch widgets canon=%s fresh=%s",
                canonWidgetSummary.c_str(), freshWidgetSummary.c_str());
            BDLOG("fillDetails: mismatch actions canon=%s fresh=%s",
                canonActionSummary.c_str(), freshActionSummary.c_str());
            return;
        }

        for (size_t wi = 0; wi < this->_widgets.size(); ++wi) {
            const auto &widgetPtr = this->_widgets[wi];
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
                const uintptr_t wh = widgetPtr->hash();
                const uintptr_t myh = widgetPtr->getMyHashcode();
                const size_t nmatch = countWidgetsWithHash(copy->_widgets, wh);
                LOGE(
                    "ERROR can not refill widget: from=%s canonId=%d freshId=%d act canon=%s fresh=%s "
                    "sameAct=%d st=%" PRIuPTR "/%" PRIuPTR " stEq=%d dynAbs=%d/%d noDet=%d/%d "
                    "nw=%zu/%zu merged=%zu/%zu wi=%zu wHash=%" PRIuPTR " myHash=%" PRIuPTR
                    " freshCountSameWHash=%zu",
                    fromTag, getIdi(), copy->getIdi(), actCanon.c_str(), actFresh.c_str(),
                    actCanon == actFresh ? 1 : 0, static_cast<uintptr_t>(stCanon),
                    static_cast<uintptr_t>(stFresh), stCanon == stFresh ? 1 : 0, dynAbsCanon, dynAbsFresh,
                    hasNoDetail() ? 1 : 0, copy->hasNoDetail() ? 1 : 0, this->_widgets.size(),
                    copy->_widgets.size(), this->_mergedWidgets.size(), copy->_mergedWidgets.size(),
                    static_cast<unsigned long>(wi), static_cast<uintptr_t>(wh),
                    static_cast<uintptr_t>(myh), static_cast<unsigned long>(nmatch));
                const long long nwDelta = static_cast<long long>(copy->_widgets.size()) -
                    static_cast<long long>(this->_widgets.size());
                BDLOG(
                    "fillDetails miss extra: from=%s wi=%zu nw_delta=%lld fresh_nonempty=%d",
                    fromTag, wi, static_cast<long long>(nwDelta),
                    copy->_widgets.empty() ? 0 : 1);
            }
        }
        for (const auto &miter: this->_mergedWidgets) {
            auto mkw = copy->_mergedWidgets.find(miter.first);
            if (mkw == copy->_mergedWidgets.end()) {
                BDLOG(
                    "fillDetails[merged]: fresh missing merge bucket key=%" PRIuPTR " act=%s/%s",
                    static_cast<uintptr_t>(miter.first), actCanon.c_str(), actFresh.c_str());
                continue;
            }
            for (size_t mj = 0; mj < miter.second.size(); ++mj) {
                const auto &widgetPtr = miter.second[mj];
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
                } else {
                    const uintptr_t wh = widgetPtr->hash();
                    const size_t nmatch = countWidgetsWithHash((*mkw).second, wh);
                    BDLOG(
                        "fillDetails[merged] miss: mergeKey=%" PRIuPTR " idx=%zu wHash=%" PRIuPTR
                        " bucketFresh=%zu sameHashInFresh=%zu act=%s/%s",
                        static_cast<uintptr_t>(miter.first), static_cast<unsigned long>(mj),
                        static_cast<uintptr_t>(wh), (*mkw).second.size(),
                        static_cast<unsigned long>(nmatch), actCanon.c_str(), actFresh.c_str());
                }
            }
        }
        _hasNoDetail = false;
    }

    /** Multi-line debug dump of hash, widgets, and actions. */
    std::string State::toString() const {
        std::ostringstream oss;
        oss << "{state: " << this->hash() << "\n    widgets: \n";
        for (auto const &widget: this->_widgets) {
            if (widget != nullptr) {
                std::string ws = widget->toString();
                if (!ws.empty()) oss << "   " << ws << "\n";
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


    /** Sums filter priorities for actions passing `filter` (optional exclusion of BACK). */
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

    /** Actions that match `targetFilter` (typically spatial targets). */
    ActivityStateActionPtrVec State::targetActions() const {
        ActivityStateActionPtrVec retV;
        ActionFilterPtr filter = targetFilter; //(ActionFilterPtr(new ActionFilterTarget());)
        for (const auto &a: this->_actions) {
            if (filter->include(a))
                retV.emplace_back(a);
        }
        return retV;
    }

    /** Highest filter-priority action (ties broken by scan order). */
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

    /** Random selection including BACK in the support when priorities allow. */
    ActivityStateActionPtr State::randomPickAction(const ActionFilterPtr &filter) const {
        return this->randomPickAction(filter, true);
    }

    /** Weighted random choice: roll in `[0, total)` then walk cumulative filter priorities. */
    ActivityStateActionPtr
    State::randomPickAction(const ActionFilterPtr &filter, bool includeBack) const {
        int total = this->countActionPriority(filter, includeBack);
        if (total == 0)
            return nullptr;
        // Use thread-local random number generator for better performance
        int index = randomInt(0, total);
        return pickAction(filter, includeBack, index);
    }

    /** Deterministic slice of the discrete distribution implied by `filter` priorities. */
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

    /** Random valid-unvisited pick; falls back to BACK when nothing else matches. */
    ActivityStateActionPtr State::randomPickUnvisitedAction() const {
        ActivityStateActionPtr action = this->randomPickAction(enableValidUnvisitedFilter, false);
        if (action == nullptr && enableValidUnvisitedFilter->include(getBackAction())) {
            action = getBackAction();
        }
        return action;
    }


    /** Random valid-unsaturated pick; falls back to BACK when nothing else matches. */
    ActivityStateActionPtr State::randomPickUnsaturatedAction() const {
        ActivityStateActionPtr action = this->randomPickAction(enableValidUnSaturatedFilter, false);
        if (action == nullptr && enableValidUnSaturatedFilter->include(getBackAction())) {
            action = getBackAction();
        }
        return action;
    }

    /** Rotates among all concrete widgets represented by one abstract action. */
    ActivityStateActionPtr State::resolveAt(ActivityStateActionPtr action, time_t /*t*/) {
        if (action == nullptr) {
            return action;
        }

        if (action->getTarget() == nullptr) {
            return action;
        }

        uintptr_t h = action->getTarget()->hash();
        auto representative = std::find_if(this->_widgets.begin(), this->_widgets.end(),
            [h](const WidgetPtr &widget) {
                return widget && widget->hash() == h;
            });
        if (representative == this->_widgets.end()) {
            return action;
        }

        const size_t totalTargets = getConcreteTargetCount(action->getTarget());
        int total = static_cast<int>(totalTargets);
        if (total <= 0) {
            BLOGE("resolveAt: concrete target count is 0 for hash %" PRIuPTR, h);
            return action;
        }

        int slot = action->getVisitedCount() % total;
        if (slot == 0) {
            BDLOG("resolveAt: action=%s hash=%" PRIuPTR " visited=%d concrete=%d slot=%d whichWidget=-1 target=representative",
                action->getId().c_str(),
                h,
                action->getVisitedCount(),
                total,
                slot);
            action->setTarget(*representative);
            action->setWhichWidget(-1);
            return action;
        }

        auto targetWidgets = this->_mergedWidgets.find(h);
        if (targetWidgets == this->_mergedWidgets.end() ||
            static_cast<size_t>(slot - 1) >= targetWidgets->second.size()) {
            BLOGE("resolveAt: merged widget slot out of range for hash %" PRIuPTR " slot=%d total=%d",
                h, slot, total);
            action->setTarget(*representative);
            action->setWhichWidget(-1);
            return action;
        }

        const int mergedIndex = slot - 1;
        BDLOG("resolveAt: action=%s hash=%" PRIuPTR " visited=%d concrete=%d slot=%d whichWidget=%d target=merged",
            action->getId().c_str(),
            h,
            action->getVisitedCount(),
            total,
            slot,
            mergedIndex);
        action->setTarget(targetWidgets->second[static_cast<size_t>(mergedIndex)]);
        action->setWhichWidget(mergedIndex);
        return action;
    }

#if DYNAMIC_STATE_ABSTRACTION_ENABLED
    /** Size of the merge bucket for `target`'s hash, or 1 if none / empty. */
    size_t State::getMergedTargetGroupSize(const WidgetPtr &target) const {
        if (!target) {
            return 0;
        }
        auto it = this->_mergedWidgets.find(target->hash());
        if (it == this->_mergedWidgets.end() || it->second.empty()) {
            return 1;
        }
        return it->second.size();
    }

    /** Non-null only when at least two widgets share `target`'s hash in `_mergedWidgets`. */
    const WidgetPtrVec *State::getMergedTargetsIfAny(const WidgetPtr &target) const {
        if (!target) {
            return nullptr;
        }
        auto it = this->_mergedWidgets.find(target->hash());
        if (it == this->_mergedWidgets.end() || it->second.size() < 2) {
            return nullptr;
        }
        return &it->second;
    }
#endif

    /** Linear scan of `_widgets` using pointer equality helper. */
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

    /** Value equality is hash equality for `State` (used after abstraction stabilizes the hash). */
    bool State::operator==(const State &state) const {
        return this->hash() == state.hash();
    }


} // namespace fastbotx


#endif // State_CPP_
