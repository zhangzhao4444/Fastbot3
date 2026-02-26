/*
 * This code is licensed under the Fastbot license. You may obtain a copy of this license in the LICENSE.txt file in the root directory of this source tree.
 */
/**
 * @authors Jianqiang Guo, Yuhui Su, Zhao Zhang
 */
#ifndef RichWidget_CPP_
#define RichWidget_CPP_


#include "RichWidget.h"
#include "../utils.hpp"
#include "../events/Preference.h"
#include <algorithm>
#include <utility>

namespace fastbotx {


    /**
     * @brief Constructor creates RichWidget with enhanced hash computation
     * 
     * Computes hash code based on:
     * - Class name
     * - Resource ID
     * - Supported actions
     * - Valid text (from widget or its children)
     * 
     * Performance optimization:
     * - Uses efficient hash combination with bit shifting
     * - Only includes text hash if text is not empty
     * 
     * @param parent Parent widget (moved to avoid copy)
     * @param element XML Element to create widget from
     */
    RichWidget::RichWidget(WidgetPtr parent, const ElementPtr &element)
            : Widget(std::move(parent), element) {
        // Performance optimization: Use fast string hash function instead of std::hash
        // Compute hash components
        uintptr_t hashcode1 = fastbotx::fastStringHash(this->_clazz);
        uintptr_t hashcode2 = fastbotx::fastStringHash(this->_resourceID);
        
        // Combine action types into hash
        uintptr_t hashcode3 = 0x1;
        for (ActionType actionType: this->getActions()) {
            hashcode3 ^= (127U * std::hash<int>{}(static_cast<int>(actionType)));
        }
        
        // Combine class, resource ID, and actions
        this->_widgetHashcode = ((hashcode1 ^ (hashcode2 << 4)) >> 2) ^ ((127U * hashcode3 << 1));
        
        // Include text from widget or children if available
        // Use fast string hash for better performance
        std::string elementText = this->getValidTextFromWidgetAndChildren(element);
        if (!elementText.empty()) {
            this->_widgetHashcode ^= (0x79b9 + (fastbotx::fastStringHash(elementText) << 1));
        }
    }

    /**
     * @brief Get valid text from widget or its children (iterative search)
     * 
     * Searches for valid text in the widget and its children using iterative
     * depth-first search instead of recursion. This avoids stack overflow for
     * deeply nested UI trees and reduces function call overhead.
     * 
     * Performance optimizations:
     * - Uses iterative search instead of recursion (avoids stack overflow)
     * - Pre-allocates stack space to reduce reallocations
     * - Early return when text is found
     * - Uses const reference for children to avoid copying
     * 
     * @param element Element to get text from
     * @return Valid text from widget or its children/offspring, empty if none found
     */
    std::string RichWidget::getValidTextFromWidgetAndChildren(const ElementPtr &element) const {
        // Legacy static reuse mode: mimic old ReuseWidget::getElementText behavior
        if (Preference::inst() && Preference::inst()->useStaticReuseAbstraction()) {
            std::function<std::string(const ElementPtr &)> getElementText =
                [&getElementText](const ElementPtr &elem) -> std::string {
                    std::string txt = elem->validText;
                    if (txt.empty()) {
                        bool useChildText = true;
                        for (auto &child : elem->getChildren()) {
                            if (child->getClickable() || child->getLongClickable()) {
                                useChildText = false;
                            }
                            if (txt.empty()) {
                                txt = getElementText(child);
                            }
                        }
                        if (useChildText && !txt.empty()) {
                            return txt;
                        }
                    }
                    return txt;
                };
            return getElementText(element);
        }

        // Dynamic abstraction mode: use iterative DFS over validText (current RichWidget behavior)
        if (!element->validText.empty()) {
            return element->validText;
        }

        std::vector<ElementPtr> stack;
        stack.reserve(32);

        const auto &children = element->getChildren();
        stack.insert(stack.end(), children.begin(), children.end());

        while (!stack.empty()) {
            ElementPtr current = stack.back();
            stack.pop_back();

            if (!current->validText.empty()) {
                return current->validText;
            }

            const auto &currentChildren = current->getChildren();
            stack.insert(stack.end(), currentChildren.begin(), currentChildren.end());
        }

        return "";
    }

    RichWidget::RichWidget()
            : Widget() {

    }

    uintptr_t RichWidget::hash() const {
        return getActHashCode();
    }

    uintptr_t RichWidget::hashWithMask(WidgetKeyMask mask) const {
        return Widget::hashWithMask(mask);
    }

}

#endif //RichWidget_CPP_
