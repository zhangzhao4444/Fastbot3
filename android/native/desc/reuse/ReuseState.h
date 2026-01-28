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
         * @return Shared pointer to created ReuseState
         */
        static std::shared_ptr<ReuseState>
        create(const ElementPtr &element, const stringPtr &activityName, const NamingPtr &naming);

    protected:
        virtual void buildStateFromElement(WidgetPtr parentWidget, ElementPtr element);

        virtual void buildHashForState();

        virtual void buildActionForState();

        virtual void mergeWidgetsInState();

        explicit ReuseState(stringPtr activityName);

        ReuseState();

        virtual void buildState(const ElementPtr &element, const NamingPtr &naming);

        virtual void buildBoundingBox(const ElementPtr &element);

    private:
        void buildFromElement(WidgetPtr parentWidget, ElementPtr elem) override;
    };

    typedef std::shared_ptr<ReuseState> ReuseStatePtr;

}


#endif //ReuseState_H_
