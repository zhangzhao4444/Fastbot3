/*
 * This code is licensed under the Fastbot license. You may obtain a copy of this license in the LICENSE.txt file in the root directory of this source tree.
 */
/**
 * @authors Jianqiang Guo, Yuhui Su, Zhao Zhang
 */
#ifndef Widget_H_
#define Widget_H_


#include <set>
#include <map>
#include <string>
#include <memory>
#include "Element.h"
#include "../Base.h"

namespace fastbotx {

    /**
     * @brief Widget class representing an actionable UI widget
     * 
     * Widget is a lightweight representation of a UI element that can be
     * interacted with. It extracts essential properties from Element and
     * computes a hash for state comparison. Widgets are used in State
     * objects to represent the actionable elements on a screen.
     * 
     * Features:
     * - Hash-based identification
     * - Action type tracking
     * - XPath generation
     * - Memory-efficient (can clear details when not needed)
     * 
     * @note Text is used for embedding and identifying widgets by default
     */
    class Widget : Serializable, public HashNode {
    public:
        /**
         * @brief Constructor creates a Widget from an Element
         * 
         * Extracts relevant properties from the Element and computes the
         * widget's hash code. Processes text to remove digits and spaces.
         * 
         * @param parent Parent widget (nullptr for root widgets)
         * @param element The Element to create widget from
         */
        Widget(std::shared_ptr<Widget> parent, const ElementPtr &element);

        std::shared_ptr<Widget> getParent() const { return this->_parent; }

        std::shared_ptr<Rect> getBounds() const { return this->_bounds; }

        std::set<ActionType> getActions() const { return this->_actions; }

        std::string getText() const { return this->_text; }

        const std::string &getClassName() const { return this->_clazz; }

        const std::string &getResourceID() const { return this->_resourceID; }

        const std::string &getContentDesc() const { return this->_contextDesc; }

        int getIndex() const { return this->_index; }

        bool getEnabled() const { return this->_enabled; }

        bool hasOperate(OperateType opt) const { return this->_operateMask & opt; }

        bool hasAction() const { return !this->_actions.empty(); }

        bool isEditable() const;

        uintptr_t hash() const override;

        std::string toString() const override;

        std::string toJson() const;

        std::string buildFullXpath() const;

        virtual void clearDetails();

        void fillDetails(const std::shared_ptr<Widget> &copy);

        virtual ~Widget();


    protected:
        Widget();

        void enableOperate(OperateType opt) { this->_operateMask |= opt; }

        void initFormElement(const ElementPtr &element);

        uintptr_t _hashcode{};
        std::shared_ptr<Widget> _parent;
        std::string _text;
        int _index{};
        std::string _clazz;
        std::string _resourceID;
        bool _enabled{};
        bool _isEditable{};
        int _operateMask{OperateType::None};
    private:
        std::string toXPath() const;

        RectPtr _bounds;
        std::string _contextDesc;
        std::set<ActionType> _actions;
    };


    typedef std::shared_ptr<Widget> WidgetPtr;
    typedef std::vector<WidgetPtr> WidgetPtrVec;
    typedef std::set<WidgetPtr, Comparator<Widget>> WidgetPtrSet;
    typedef std::map<uintptr_t, WidgetPtrVec> WidgetPtrVecMap;

}


#endif //Widget_H_
