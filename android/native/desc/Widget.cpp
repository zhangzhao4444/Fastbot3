/*
 * This code is licensed under the Fastbot license. You may obtain a copy of this license in the LICENSE.txt file in the root directory of this source tree.
 */
/**
 * @authors Jianqiang Guo, Yuhui Su, Zhao Zhang
 */
#ifndef Widget_CPP_
#define Widget_CPP_


#include "Widget.h"
#include "../utils.hpp"
#include "Preference.h"
#include <algorithm>
#include <utility>
#include <vector>
#include <cstring>

namespace fastbotx {

    Widget::Widget() = default;

    const auto ifCharIsDigitOrBlank = [](const char &c) -> bool {
        return c == ' ' || (c >= '0' && c <= '9');
    };


    /**
     * @brief Constructor creates a Widget from an Element
     * 
     * Initializes a Widget object from an Element, extracting relevant properties
     * and computing the widget's hash code. The text is processed to remove
     * digits and spaces, and may be truncated for Chinese text handling.
     * 
     * Performance optimizations:
     * - Uses move semantics for parent to avoid copying
     * - Efficient string processing with remove_if algorithm
     * - Hash computation only when needed (based on configuration flags)
     * 
     * @param parent Parent widget (nullptr for root widgets)
     * @param element The Element to create widget from
     */
    Widget::Widget(std::shared_ptr<Widget> parent, const ElementPtr &element) {
        // Move parent to avoid copying shared_ptr
        this->_parent = std::move(parent);
        
        // Performance optimization: Cache Preference instance to avoid repeated inst() calls
        // Preference::inst() is thread-safe singleton, but caching avoids repeated function calls
        PreferencePtr pref = Preference::inst();
        
        // Initialize widget properties from element
        this->initFormElement(element);
        
        // Performance: Remove digits and blank spaces from text using efficient algorithm
        // remove_if moves matching elements to end, returns iterator to new end
        auto removeIterator = std::remove_if(this->_text.begin(), 
                                             this->_text.end(), 
                                             ifCharIsDigitOrBlank);
        // Erase removed elements
        this->_text.erase(removeIterator, this->_text.end());
        
        // Process text for hash computation if text-based state is enabled
        // Use cached Preference instance instead of calling inst() again
        bool useTextModel = STATE_WITH_TEXT || pref->isForceUseTextModel();
        bool overMaxLen = false;
        std::string finalText = this->_text; // Keep original for hash computation
        
        if (useTextModel) {
            overMaxLen = this->_text.size() > STATE_TEXT_MAX_LEN;
            
            // Performance optimization: Compute cut length first, then do single substr
            size_t cutLength = static_cast<size_t>(STATE_TEXT_MAX_LEN);
            
            // Handle Chinese characters: if cut point is in middle of Chinese char, adjust
            // Check if we need to adjust cut point for Chinese characters
            if (this->_text.length() > cutLength) {
                // Check a bit beyond cutLength to see if we're in middle of Chinese char
                size_t checkLen = std::min(cutLength + 4, this->_text.length());
                std::string tempText = this->_text.substr(0, checkLen);
                if (tempText.length() > cutLength && isZhCn(tempText[cutLength])) {
                    size_t ci = 0;
                    // Find safe cut point that doesn't split Chinese characters
                    for (; ci < cutLength && ci < tempText.length(); ci++) {
                        if (isZhCn(tempText[ci])) {
                            ci += 2; // Chinese chars are typically 2-3 bytes in UTF-8
                        }
                    }
                    cutLength = ci;
                }
            }

            // Final text truncation (single substr operation)
            finalText = this->_text.substr(0, cutLength);
            this->_text = finalText;
        }

        // Performance optimization: Compute text hash only once and reuse
        // Component hash for Text (for dynamic abstraction hashWithMask)
        // Use fast string hash for better performance
        uintptr_t textHash = finalText.empty() ? 0 : (0x79b9U + (fastbotx::fastStringHash(finalText) << 5));
        this->_hashText = textHash;
        
        // Only include text in hash if it wasn't truncated
        // (truncated text would make hash unstable)
        if (useTextModel && !overMaxLen) {
            this->_hashcode ^= textHash;
        }

        // Include index in hash if configured
        if (STATE_WITH_INDEX) {
            this->_hashcode ^= ((0x79b9 + (std::hash<int>{}(this->_index) << 6)) << 1);
        }
    }

    void Widget::initFormElement(const ElementPtr &element) {
        if (element->getCheckable())
            enableOperate(OperateType::Checkable);
        if (element->getEnable())
            enableOperate(OperateType::Enable);
        if (element->getClickable())
            enableOperate(OperateType::Clickable);
        if (element->getScrollable())
            enableOperate(OperateType::Scrollable);
        if (element->getLongClickable()) {
            enableOperate(OperateType::LongClickable);
            this->_actions.insert(ActionType::LONG_CLICK);
        }
        if (this->hasOperate(OperateType::Checkable) ||
            this->hasOperate(OperateType::Clickable)) {
            this->_actions.insert(ActionType::CLICK);
        }

        ScrollType scrollType = element->getScrollType();
        switch (scrollType) {
            case ScrollType::NONE:
                break;
            case ScrollType::ALL:
                this->_actions.insert(ActionType::SCROLL_BOTTOM_UP);
                this->_actions.insert(ActionType::SCROLL_TOP_DOWN);
                this->_actions.insert(ActionType::SCROLL_LEFT_RIGHT);
                this->_actions.insert(ActionType::SCROLL_RIGHT_LEFT);
                break;
            case ScrollType::Horizontal:
                this->_actions.insert(ActionType::SCROLL_LEFT_RIGHT);
                this->_actions.insert(ActionType::SCROLL_RIGHT_LEFT);
                break;
            case ScrollType::Vertical:
                this->_actions.insert(ActionType::SCROLL_BOTTOM_UP);
                this->_actions.insert(ActionType::SCROLL_TOP_DOWN);
                break;
            default:
                break;
        }

        if (this->hasAction()) {
            this->_clazz = (element->getClassname());
            
            // Performance optimization: Use length check and pointer comparison for common class names
            // This avoids multiple string comparisons and allocations
            const std::string &clazz = this->_clazz;
            const char *clazzPtr = clazz.c_str();
            size_t clazzLen = clazz.length();
            
            // Check for EditText variants using optimized comparison
            // Most common case first: android.widget.EditText (length 23)
            this->_isEditable = (clazzLen == 23 && strcmp(clazzPtr, "android.widget.EditText") == 0)
                                 || (clazzLen == 42 && strcmp(clazzPtr, "android.inputmethodservice.ExtractEditText") == 0)
                                 || (clazzLen == 35 && strcmp(clazzPtr, "android.widget.AutoCompleteTextView") == 0)
                                 || (clazzLen == 42 && strcmp(clazzPtr, "android.widget.MultiAutoCompleteTextView") == 0);

            // Performance optimization: Use length check before compare for better branch prediction
            if (SCROLL_BOTTOM_UP_N_ENABLE) {
                if ((clazzLen == 25 && strcmp(clazzPtr, "android.widget.ListView") == 0)
                    || (clazzLen == 37 && strcmp(clazzPtr, "android.support.v7.widget.RecyclerView") == 0)
                    || (clazzLen == 35 && strcmp(clazzPtr, "androidx.recyclerview.widget.RecyclerView") == 0)) {
                    this->_actions.insert(ActionType::SCROLL_BOTTOM_UP_N);
                }
            }
            this->_resourceID = (element->getResourceID());
        }
        if (element->getBounds()) {
            this->_bounds = element->getBounds();
        } else {
            this->_bounds = Rect::RectZero;
            BLOGE("Widget::initFormElement: element->getBounds() returned null, using RectZero");
        }
        this->_index = element->getIndex();
        this->_enabled = element->getEnable();
        this->_text = element->getText();
        
        // Performance optimization: Use const reference to avoid string copy
        // Only copy if ContentDesc is actually used (non-empty)
        const std::string &contentDescRef = element->getContentDesc();
        if (!contentDescRef.empty()) {
            this->_contextDesc = contentDescRef; // Copy only when needed
        } else {
            this->_contextDesc.clear(); // Explicitly clear to avoid unnecessary allocation
        }
        
        // Filter long Chinese text: if text or contentDesc has more than 8 Chinese characters, clear it
        // This prevents long text from interfering with state abstraction or embedding
        auto countChineseChars = [](const std::string &str) -> size_t {
            size_t count = 0;
            for (size_t i = 0; i < str.length(); ) {
                unsigned char c = static_cast<unsigned char>(str[i]);
                // UTF-8 encoding:
                // - ASCII (0x00-0x7F): 1 byte
                // - 2-byte sequence: 0xC0-0xDF (not Chinese)
                // - 3-byte sequence: 0xE0-0xEF (most Chinese/CJK characters are here)
                // - 4-byte sequence: 0xF0-0xF7 (rare CJK characters)
                if ((c & 0xF0) == 0xE0) {  // 3-byte UTF-8 sequence
                    // Check if it's a valid 3-byte sequence
                    if (i + 2 < str.length()) {
                        unsigned char c1 = static_cast<unsigned char>(str[i + 1]);
                        unsigned char c2 = static_cast<unsigned char>(str[i + 2]);
                        // Valid continuation bytes: 0x80-0xBF
                        if ((c1 & 0xC0) == 0x80 && (c2 & 0xC0) == 0x80) {
                            // CJK Unified Ideographs range: 0xE4-0xE9 (most common)
                            // Also include 0xE0-0xE3 and 0xEA-0xEF for extended CJK ranges
                            if (c >= 0xE0 && c <= 0xEF) {
                                count++;
                            }
                            i += 3;
                            continue;
                        }
                    }
                    i++;  // Invalid sequence, skip one byte
                } else if ((c & 0xF8) == 0xF0) {  // 4-byte UTF-8 sequence (CJK Extension B/C/D, rare)
                    if (i + 3 < str.length()) {
                        unsigned char c1 = static_cast<unsigned char>(str[i + 1]);
                        unsigned char c2 = static_cast<unsigned char>(str[i + 2]);
                        unsigned char c3 = static_cast<unsigned char>(str[i + 3]);
                        // Valid continuation bytes
                        if ((c1 & 0xC0) == 0x80 && (c2 & 0xC0) == 0x80 && (c3 & 0xC0) == 0x80) {
                            count++;  // Count rare 4-byte CJK characters
                            i += 4;
                            continue;
                        }
                    }
                    i++;  // Invalid sequence, skip one byte
                } else {
                    i++;  // ASCII or other, skip one byte
                }
            }
            return count;
        };
        
        const size_t MAX_CHINESE_CHARS = 8;
        if (countChineseChars(this->_text) > MAX_CHINESE_CHARS) {
            this->_text.clear();
        }
        if (countChineseChars(this->_contextDesc) > MAX_CHINESE_CHARS) {
            this->_contextDesc.clear();
        }
        
        // Performance optimization: Use fast string hash function instead of std::hash
        // This provides better performance for typical UI strings (short to medium length)
        // compute for only 1 time (base + component hashes for dynamic abstraction)
        uintptr_t hashcode1 = fastbotx::fastStringHash(this->_clazz);
        uintptr_t hashcode2 = fastbotx::fastStringHash(this->_resourceID);
        uintptr_t hashcode3 = std::hash<int>{}(this->_operateMask);
        uintptr_t hashcode4 = std::hash<int>{}(scrollType);

        this->_hashClazz = hashcode1;
        this->_hashResourceID = hashcode2;
        this->_hashOperateMask = hashcode3;
        this->_hashScrollType = hashcode4;
        
        // Performance optimization: Compute ContentDesc hash only if not empty
        // Most widgets don't have ContentDesc, so this avoids unnecessary hash computation
        // Use fast string hash for better performance
        if (!this->_contextDesc.empty()) {
            this->_hashContentDesc = 0x79b9U + (fastbotx::fastStringHash(this->_contextDesc) << 5);
        } else {
            this->_hashContentDesc = 0; // Fast path for empty ContentDesc
        }
        
        this->_hashIndex = (0x79b9U + (static_cast<uintptr_t>(std::hash<int>{}(this->_index)) << 6)) << 1;

        this->_hashcode = ((hashcode1 ^ (hashcode2 << 4)) >> 2) ^
                          (((127U * hashcode3 << 1) ^ (256U * hashcode4 << 3)) >> 1);
    }

    bool Widget::isEditable() const {
        return this->_isEditable;
    }

    void Widget::clearDetails() {
        this->_clazz.clear();
        this->_text.clear();
        this->_contextDesc.clear();
        this->_resourceID.clear();
        this->_bounds = Rect::RectZero;
        this->_hashClazz = this->_hashResourceID = this->_hashOperateMask = this->_hashScrollType = 0;
        this->_hashText = this->_hashContentDesc = this->_hashIndex = 0;
    }

    void Widget::fillDetails(const std::shared_ptr<Widget> &copy) {
        this->_text = copy->_text;
        this->_clazz = copy->_clazz;
        this->_contextDesc = copy->_contextDesc;
        this->_resourceID = copy->_resourceID;
        this->_bounds = copy->getBounds();
        this->_enabled = copy->_enabled;
        this->_hashClazz = copy->_hashClazz;
        this->_hashResourceID = copy->_hashResourceID;
        this->_hashOperateMask = copy->_hashOperateMask;
        this->_hashScrollType = copy->_hashScrollType;
        this->_hashText = copy->_hashText;
        this->_hashContentDesc = copy->_hashContentDesc;
        this->_hashIndex = copy->_hashIndex;
    }

    std::string Widget::toString() const {
        return this->toXPath();
    }


    std::string Widget::toXPath() const {
        // Details cleared for memory (e.g. after state merge); skip per-call log to avoid noise
        if (this->_text.empty() && this->_clazz.empty()
            && this->_resourceID.empty()) {
            return "";
        }

        // Make local copies to avoid issues if strings are modified during formatting
        std::string clazzCopy = this->_clazz;
        std::string resourceIDCopy = this->_resourceID;
        std::string textCopy = this->_text;
        std::string contextDescCopy = this->_contextDesc;
        
        std::stringstream stringStream;
        std::string boundsStr = "";
        if (this->_bounds != nullptr) {
            boundsStr = this->_bounds->toString();
        } else {
            boundsStr = "[null]";
            BLOGE("Widget::toXPath: _bounds is null for widget");
        }
        stringStream << "{xpath: /*" <<
                     "[@class=\"" << clazzCopy << "\"]" <<
                     "[@resource-id=\"" << resourceIDCopy << "\"]" <<
                     "[@text=\"" << textCopy << "\"]" <<
                     "[@content-desc=\"" << contextDescCopy << "\"]" <<
                     "[@index=" << this->_index << "]" <<
                     "[@bounds=\"" << boundsStr << "\"]}";
        return stringStream.str();
    }

    std::string Widget::toJson() const {
        // Details cleared for memory (e.g. after state merge); skip per-call log to avoid noise
        if (this->_text.empty() && this->_clazz.empty()
            && this->_resourceID.empty()) {
            return "";
        }

        nlohmann::json j;
        j["index"] = this->_index;
        j["class"] = this->_clazz;
        j["resource-id"] = this->_resourceID;
        j["text"] =  this->_text;
        j["content-desc"] = this->_contextDesc;
        if (this->_bounds != nullptr) {
            j["bounds"] = this->_bounds->toString();
        } else {
            j["bounds"] = "[null]";
            BLOGE("Widget::toJson: _bounds is null for widget");
        }
        return j.dump();
    }

    std::string Widget::buildFullXpath() const {
        std::vector<std::string> segments;
        segments.push_back(this->toXPath());
        std::shared_ptr<Widget> parent = _parent;
        while (parent) {
            segments.push_back(parent->toXPath());
            parent = parent->_parent;
        }
        std::string fullXpathString;
        size_t total = 0;
        for (const auto &s : segments)
            total += s.size();
        fullXpathString.reserve(total);
        for (auto it = segments.rbegin(); it != segments.rend(); ++it)
            fullXpathString.append(*it);
        return fullXpathString;
    }

    Widget::~Widget() {
        this->_actions.clear();
        this->_parent = nullptr;
    }

    uintptr_t Widget::hash() const {
        return _hashcode;
    }

    uintptr_t Widget::hashWithMask(WidgetKeyMask mask) const {
        uintptr_t h;
        const auto defaultMask = static_cast<WidgetKeyMask>(DefaultWidgetKeyMask);
        if ((mask & defaultMask) == defaultMask) {
            h = ((_hashClazz ^ (_hashResourceID << 4)) >> 2) ^
                (((127U * _hashOperateMask << 1) ^ (256U * _hashScrollType << 3)) >> 1);
        } else {
            h = 0x1;
            if (mask & static_cast<WidgetKeyMask>(WidgetKeyAttr::Clazz)) h ^= _hashClazz;
            if (mask & static_cast<WidgetKeyMask>(WidgetKeyAttr::ResourceID)) h ^= (_hashResourceID << 4);
            if (mask & static_cast<WidgetKeyMask>(WidgetKeyAttr::OperateMask)) h ^= (127U * _hashOperateMask << 1);
            if (mask & static_cast<WidgetKeyMask>(WidgetKeyAttr::ScrollType)) h ^= (256U * _hashScrollType << 3);
        }
        if (mask & static_cast<WidgetKeyMask>(WidgetKeyAttr::Text)) h ^= _hashText;
        if (mask & static_cast<WidgetKeyMask>(WidgetKeyAttr::ContentDesc)) h ^= _hashContentDesc;
        if (mask & static_cast<WidgetKeyMask>(WidgetKeyAttr::Index)) h ^= _hashIndex;
        return h;
    }

}

#endif //Widget_CPP_
