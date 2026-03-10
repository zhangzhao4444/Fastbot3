/*
 * This code is licensed under the Fastbot license. You may obtain a copy of this license in the LICENSE.txt file in the root directory of this source tree.
 */
/**
 * @authors Jianqiang Guo, Yuhui Su, Zhao Zhang
 */
#ifndef ActionFilter_H_
#define ActionFilter_H_

#include "Action.h"
#include "utils.hpp"
#include <memory>
#include <cmath>

namespace fastbotx {


    /**
     * @brief Base class for action filtering
     * 
     * ActionFilter provides a way to filter and prioritize actions during selection.
     * Different filter implementations can be used to select actions based on
     * various criteria (validity, enabled state, visited status, etc.).
     */
    class ActionFilter {
    public:
        /**
         * @brief Check if an action should be included in selection
         * 
         * @param action Action to check
         * @return true if action should be included
         */
        virtual bool include(ActivityStateActionPtr action) const = 0;

        /**
         * @brief Get priority value for an action
         * 
         * Default implementation returns the action's own priority.
         * Subclasses can override to modify priority based on filter criteria.
         * 
         * @param action Action to get priority for
         * @return Priority value (higher = more important)
         */
        virtual int getPriority(ActivityStateActionPtr action) const {
            return action->getPriority();
        }

        /**
         * @brief Virtual destructor for proper cleanup
         */
        virtual ~ActionFilter() = default;
    };

    /**
     * @brief Filter that includes all actions (no filtering)
     */
    class ActionFilterALL : public ActionFilter {
    public:
        bool include(ActivityStateActionPtr /* action */) const override {
            return true;
        }
    };

    /**
     * @brief Filter that only includes actions requiring a target widget
     */
    class ActionFilterTarget : public ActionFilter {
    public:
        bool include(ActivityStateActionPtr action) const override {
            return action->requireTarget();
        }
    };

    /**
     * @brief Filter that only includes valid actions
     */
    class ActionFilterValid : public ActionFilter {
    public:
        bool include(ActivityStateActionPtr action) const override {
            return action->isValid();
        }
    };

    /**
     * @brief Filter that only includes enabled and valid actions
     */
    class ActionFilterEnableValid : public ActionFilter {
    public:
        bool include(ActivityStateActionPtr action) const override {
            return action->getEnabled() && action->isValid();
        }
    };

    /**
     * @brief Filter that only includes enabled, valid, and unvisited actions
     */
    class ActionFilterUnvisitedValid : public ActionFilter {
    public:
        bool include(ActivityStateActionPtr action) const override {
            return action->getEnabled() && action->isValid() && !action->isVisited();
        }
    };

    /**
     * @brief Filter that only includes valid and non-saturated actions
     * 
     * Saturated actions are those that have been visited too many times
     * relative to the number of merged widgets.
     */
    class ActionFilterValidUnSaturated : public ActionFilter {
    public:
        bool include(ActivityStateActionPtr action) const override;
    };

    /**
     * @brief Filter that includes enabled/valid actions and prioritizes by Q-value
     * 
     * This filter modifies priority to include Q-value from reinforcement learning,
     * giving higher priority to actions with higher Q-values.
     */
    class ActionFilterValidValuePriority : public ActionFilter {
    public:
        bool include(ActivityStateActionPtr action) const override {
            return (action->getEnabled() && action->isValid());
        }

        /**
         * @brief Get priority including Q-value boost
         * 
         * Adds Q-value to priority (scaled by 10) for non-back actions.
         * This gives preference to actions with higher Q-values.
         * 
         * @param action Action to get priority for
         * @return Priority value with Q-value boost
         */
        int getPriority(ActivityStateActionPtr action) const override {
            int pri = action->getPriority();
            if (!action->isBack()) {
                // Boost priority by Q-value (scaled by 10, rounded up)
                pri += static_cast<int>(ceil(10 * action->getQValue()));
            }
            return pri;
        }
    };

    /**
     * @brief Filter with date-based priority that includes specific action types
     * 
     * This filter includes system actions (START, RESTART, etc.) and UI actions
     * (CLICK, SCROLL, etc.) that are enabled, valid, and non-empty.
     */
    class ActionFilterValidDatePriority : public ActionFilter {
    public:
        /**
         * @brief Include action based on its type and validity
         * 
         * System actions (START, RESTART, BACK, etc.) are always included.
         * UI actions (CLICK, SCROLL, etc.) must be enabled, valid, and non-empty.
         * 
         * @param action Action to check
         * @return true if action should be included
         */
        bool include(ActivityStateActionPtr action) const override {
            if (nullptr == action)
                return false;
            switch (action->getActionType()) {
                // System actions are always included
                case ActionType::START:
                case ActionType::RESTART:
                case ActionType::CLEAN_RESTART:
                case ActionType::FUZZ:
                case ActionType::DEEP_LINK:
                case ActionType::NOP:
                case ActionType::ACTIVATE:
                case ActionType::BACK:
                    return true;
                // UI actions must be enabled, valid, and non-empty
                case ActionType::CLICK:
                case ActionType::LONG_CLICK:
                case ActionType::SCROLL_BOTTOM_UP:
                case ActionType::SCROLL_TOP_DOWN:
                case ActionType::SCROLL_LEFT_RIGHT:
                case ActionType::SCROLL_RIGHT_LEFT:
                case ActionType::SCROLL_BOTTOM_UP_N:
                    return action->getEnabled() && action->isValid() && !action->isEmpty();
                default:
                    BLOGE("Should not reach here");
                    return false;

            }
        }

    private:

    };


    typedef std::shared_ptr<ActionFilter> ActionFilterPtr;

    extern ActionFilterPtr allFilter;
    extern ActionFilterPtr targetFilter;
    extern ActionFilterPtr validFilter;
    extern ActionFilterPtr enableValidFilter;
    extern ActionFilterPtr enableValidUnvisitedFilter;
    extern ActionFilterPtr enableValidUnSaturatedFilter;
    extern ActionFilterPtr enableValidValuePriorityFilter;
    extern ActionFilterPtr validDatePriorityFilter;

}
#endif /* ActionFilter_H_ */
