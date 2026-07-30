/*
 * This code is licensed under the Fastbot license. You may obtain a copy of this license in the LICENSE.txt file in the root directory of this source tree.
 */
/**
 * @authors Jianqiang Guo, Yuhui Su, Zhao Zhang
 */
#ifndef ReuseState_H_
#define ReuseState_H_

#include "State.h"
#include "RichWidget.h"
#include "../Base.h"
#include <vector>


namespace fastbotx {

    /**
     * @brief ReuseState class for building states with RichWidgets
     * 
     * ReuseState extends State to use RichWidget instead of regular Widget.
     * RichWidget contains additional information for reuse-based algorithms.
     * 
     * This class builds a state that holds all RichWidgets and their associated
     * actions, optimized for reuse-based reinforcement learning algorithms.
     */
    class ReuseState : public State {
    public:
        /**
         * @brief Factory method to create a ReuseState from Element and activity name
         * 
         * @param element Root Element of the UI hierarchy
         * @param activityName Activity name string pointer
         * @param mask Widget key mask for dynamic state abstraction (default: DefaultWidgetKeyMask)
         * @return Shared pointer to created ReuseState
         */
        static std::shared_ptr<ReuseState>
        create(const ElementPtr &element, const stringPtr &activityName,
               WidgetKeyMask mask = DefaultWidgetKeyMask);

        WidgetKeyMask getWidgetKeyMask() const override { return _widgetKeyMask; }

    protected:
        virtual void buildStateFromElement(WidgetPtr parentWidget, ElementPtr element);

        virtual void buildHashForState();

        virtual void buildActionForState();

        virtual void mergeWidgetsInState();

        /**
         * @brief Get max widgets per model action (for α / Action Refinement).
         * ReuseState stores full group in _mergedWidgets, so max is max of group sizes.
         */
        size_t getMaxWidgetsPerModelAction() const override;

#if DYNAMIC_STATE_ABSTRACTION_ENABLED
        /**
         * @brief Get state hash as if computed under the given widget key mask (for coarsening).
         */
        uintptr_t getHashUnderMask(WidgetKeyMask mask) const override;
        /**
         * @brief Number of distinct widget hashes under the given mask (for "skip Text if would explode").
         */
        size_t getUniqueWidgetCountUnderMask(WidgetKeyMask mask) const override;
#endif

        explicit ReuseState(stringPtr activityName);

        ReuseState();

        virtual void buildState(const ElementPtr &element);

        virtual void buildBoundingBox(const ElementPtr &element);

        /// Widget key mask for dynamic state abstraction (used in buildHashForState and mergeWidgetsInState)
        WidgetKeyMask _widgetKeyMask{DefaultWidgetKeyMask};

    private:
        void buildFromElement(WidgetPtr parentWidget, ElementPtr elem) override;
    };

    typedef std::shared_ptr<ReuseState> ReuseStatePtr;

}


#endif //ReuseState_H_
