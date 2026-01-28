/*
 * This code is licensed under the Fastbot license. You may obtain a copy of this license in the LICENSE.txt file in the root directory of this source tree.
 */
/**
 * @authors Jianqiang Guo, Yuhui Su, Zhao Zhang, Zhengwei Lv
 */
#ifndef Action_CPP_H_
#define Action_CPP_H_

#include "Action.h"

#include <utility>
#include "State.h"


namespace fastbotx {

    /**
     * @brief Default constructor creates a NOP (no operation) action
     * 
     * Initializes action with NOP type and zero Q-value.
     */
    Action::Action()
            : Node(), PriorityNode(), _actionType(ActionType::NOP), _qValue(0) {
    }

    /**
     * @brief Constructor with specific action type
     * 
     * Initializes action with the given type and zero Q-value.
     * 
     * @param actionType The type of action to create
     */
    Action::Action(ActionType actionType)
            : Node(), PriorityNode(), _actionType(actionType), _qValue(0) {
    }

    /// Static throttle value for action execution (default: 100ms)
    int Action::_throttle = 100;

    /**
     * @brief Get priority value based on action type
     * 
     * Returns a priority value that determines action selection order.
     * Higher priority actions are preferred during selection.
     * 
     * Priority levels:
     * - CLICK: 4 (highest priority)
     * - LONG_CLICK, SCROLL actions: 2 (medium priority)
     * - Others: 1 (lowest priority)
     * 
     * @return Priority value (higher = more important)
     */
    int Action::getPriorityByActionType() const {
        switch (this->_actionType) {
            case ActionType::CLICK:
                return 4;
            case ActionType::LONG_CLICK:
            case ActionType::SCROLL_TOP_DOWN:
            case ActionType::SCROLL_BOTTOM_UP:
            case ActionType::SCROLL_LEFT_RIGHT:
            case ActionType::SCROLL_RIGHT_LEFT:
                return 2;
            default:
                return 1;
        }
    }

    bool Action::isValid() const {
        return true;
    }

    bool Action::isModelAct() const {
        return this->_actionType >= ActionType::BACK &&
               this->_actionType <= ActionType::SCROLL_BOTTOM_UP_N;
    }

    bool Action::requireTarget() const {
        return this->_actionType >= ActionType::CLICK &&
               this->_actionType <= ActionType::SCROLL_BOTTOM_UP_N;
    }

    bool Action::canStartTestApp() const {
        return this->_actionType == ActionType::START ||
               this->_actionType == ActionType::RESTART ||
               this->_actionType == ActionType::CLEAN_RESTART;
    }

    bool Action::operator==(const Action &action) {
        return this->_actionType == action._actionType;
    }

    void Action::setPriority(int priority) {
        this->_priority = priority;
    }

    std::string Action::toString() const {
        std::stringstream strs;
        std::string actStr;
        if (this->_actionType >= 0 && this->_actionType < ActionType::ActTypeSize) {
            actStr = actName[this->_actionType];
        } else {
            actStr = "INVALID_ACTION(" + std::to_string(static_cast<int>(this->_actionType)) + ")";
        }
        strs << "{id: " << this->getId() << ", act: " << actStr <<
             ", value: " << this->_qValue << "}";
        return strs.str();
    }

    OperatePtr Action::toOperate() const {
        OperatePtr opt = std::make_shared<DeviceOperateWrapper>();
        opt->act = this->_actionType;
        opt->aid = this->getId();
        if (this->_visitedCount <= 1) {
            opt->throttle = static_cast<float>(randomInt(10, Action::_throttle));
        }
        return opt;
    }

    std::shared_ptr<Action> Action::NOP = std::make_shared<Action>(ActionType::NOP);
    std::shared_ptr<Action> Action::ACTIVATE = std::make_shared<Action>(ActionType::ACTIVATE);
    std::shared_ptr<Action> Action::RESTART = std::make_shared<Action>(ActionType::RESTART);

    PropertyIDPrefixImpl(Action, "g0a");
    const int Action::DefaultValue = 0;

/// Construct an empty ActivityStateAction object with no action, state or target.
    ActivityStateAction::ActivityStateAction()
            : Action(), _target(nullptr) {

    }

/// Construct an ActivityStateAction object constituted of state, targetWidget and the corresponding action type
/// \param state The StatePtr object, describing the current page
/// \param targetWidget The WidgetPtr object, describing the targetWidget on the page for acting, could be type of Widget
/// \param actionType The corresponding action type
    ActivityStateAction::ActivityStateAction(const StatePtr &state, WidgetPtr targetWidget,
                                             ActionType actionType)
            : Action(actionType), _state(state), _target(std::move(targetWidget)) {

        uintptr_t hashcode = std::hash<int>{}(this->getActionType());
        uintptr_t stateHash = this->_state.expired() ? 0x1 : this->_state.lock()->hash();
        uintptr_t targetHash = nullptr == this->_target ? 0x1 : this->_target->hash();

        this->_hashcode =
                0x9e3779b9 + (hashcode << 2) ^ (((stateHash << 4) ^ (targetHash << 3)) << 1);
    }

    bool ActivityStateAction::isValid() const {
        return (this->_target == nullptr || !this->_target->getBounds()->isEmpty());
    }

    bool ActivityStateAction::getEnabled() const {
        return (this->_target == nullptr || this->_target->getEnabled());
    }

    uintptr_t ActivityStateAction::hash() const {
        return this->_hashcode;
    }

/// check if two actions are the same
/// \param action the action to compare with
/// \return if two actions are the same return true
    bool ActivityStateAction::operator==(const ActivityStateAction &action) const {
        return this->hash() == action.hash();
    }

    bool ActivityStateAction::operator<(const ActivityStateAction &action) const {
        return this->hash() < action.hash();
    }


    bool ActivityStateAction::isEmpty() const {
        auto rect = this->getTarget()->getBounds();
        return rect->isEmpty();
    }

    ActivityStateAction::~ActivityStateAction() {
        this->_state.reset();
        this->_target = nullptr;
        this->_resolvedNodes.clear();
        this->_resolvedNode = nullptr;
    }

    OperatePtr ActivityStateAction::toOperate() const {
        auto opt = Action::toOperate(); // call base virtual method
        opt->sid = this->getState().expired() ? "" : this->getState().lock()->getId();
        if (this->_resolvedNode && this->_resolvedNode->getBounds()) {
            opt->pos = *(this->_resolvedNode->getBounds());
        } else if (this->getTarget()) {
            opt->pos = *(this->getTarget()->getBounds());
        }
        if (this->getTarget()) {
            opt->editable = this->getTarget()->isEditable();
        }
        return opt;
    }

    std::string ActivityStateAction::toString() const {
        std::stringstream strs;
        strs << "{" << Action::toString() <<
             ", state: " << (this->_state.expired() ? "" : this->_state.lock()->getId()) <<
             ", node: " << (this->_target ? this->_target->toString() : "") << "}";
        return strs.str();
    }


}

#endif // Action_CPP_H_
