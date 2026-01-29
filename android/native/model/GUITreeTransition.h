/*
 * This code is licensed under the Fastbot license. You may obtain a copy of this license in the LICENSE.txt file in the root directory of this source tree.
 */
/**
 * @authors Jianqiang Guo, Yuhui Su, Zhao Zhang
 */
#ifndef FASTBOTX_GUI_TREE_TRANSITION_H_
#define FASTBOTX_GUI_TREE_TRANSITION_H_

#include "../../Base.h"
#include "../desc/Element.h"
#include "../desc/Action.h"

namespace fastbotx {

    // Forward declaration so we can hold a weak_ptr to StateTransition
    class StateTransition;
    typedef std::shared_ptr<StateTransition> StateTransitionPtr;

    /**
     * @brief Represents a transition between two GUI trees with an action
     * 
     * APE alignment: GUITreeTransition stores source tree, target tree, and action.
     * Used for tracking GUI tree transitions and rebuilding model.
     */
    class GUITreeTransition {
    public:
        /**
         * @brief Constructor
         * 
         * @param sourceTree Source GUI tree
         * @param action Action that triggered the transition
         * @param targetTree Target GUI tree
         */
        GUITreeTransition(ElementPtr sourceTree,
                         ActivityStateActionPtr action,
                         ElementPtr targetTree);

        ElementPtr getSource() const { return _sourceTree; }
        ElementPtr getTarget() const { return _targetTree; }
        ActivityStateActionPtr getAction() const { return _action; }

        /**
         * @brief Get timestamp from source tree
         * 
         * @return Timestamp (0 if source tree is null)
         */
        int getTimestamp() const;

        /**
         * @brief Set the current state transition that contains this GUITreeTransition
         * 
         * @param stateTransition The state transition
         */
        void setCurrentStateTransition(StateTransitionPtr stateTransition) {
            _stateTransition = stateTransition;
        }

        /**
         * @brief Get the current state transition
         * 
         * @return State transition pointer
         */
        StateTransitionPtr getCurrentStateTransition() const {
            return _stateTransition.lock();
        }

        /**
         * @brief Get throttle value
         * 
         * @return Throttle value
         */
        int getThrottle() const { return _throttle; }

        /**
         * @brief Set throttle value
         * 
         * @param throttle Throttle value
         */
        void setThrottle(int throttle) { _throttle = throttle; }

    private:
        ElementPtr _sourceTree;
        ElementPtr _targetTree;
        ActivityStateActionPtr _action;
        std::weak_ptr<StateTransition> _stateTransition;
        int _throttle{0};
    };

    typedef std::shared_ptr<GUITreeTransition> GUITreeTransitionPtr;
    typedef std::vector<GUITreeTransitionPtr> GUITreeTransitionPtrVec;

} // namespace fastbotx

#endif // FASTBOTX_GUI_TREE_TRANSITION_H_
