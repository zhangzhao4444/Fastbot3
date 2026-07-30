/*
 * This code is licensed under the Fastbot license. You may obtain a copy of this license in the LICENSE.txt file in the root directory of this source tree.
 */
/**
 * @authors Jianqiang Guo, Yuhui Su, Zhao Zhang
 */
#ifndef Element_CPP_
#define Element_CPP_

#include "../utils.hpp"
#include "Element.h"
#include "../thirdpart/tinyxml2/tinyxml2.h"
#include "../thirdpart/json/json.hpp"
#include <cstdio>
#include <cstring>
#include <algorithm>


namespace fastbotx {

    static bool isEditTextClassName(const std::string &cls) {
        const char *p = cls.c_str();
        const size_t len = cls.size();
        return (len == 23 && std::strcmp(p, "android.widget.EditText") == 0) ||
               (len == 42 && std::strcmp(p, "android.inputmethodservice.ExtractEditText") == 0) ||
               (len == 35 && std::strcmp(p, "android.widget.AutoCompleteTextView") == 0) ||
               (len == 42 && std::strcmp(p, "android.widget.MultiAutoCompleteTextView") == 0);
    }

    static std::string removeDoubleQuotes(const std::string &input) {
        if (input.find('"') == std::string::npos) {
            return input;
        }
        std::string out;
        out.reserve(input.size());
        for (char c : input) {
            if (c != '"') {
                out.push_back(c);
            }
        }
        return out;
    }

    static std::string truncateUtf8Codepoints(const std::string &input, size_t maxCodepoints) {
        if (input.empty() || maxCodepoints == 0) {
            return std::string();
        }
        size_t i = 0;
        size_t codepoints = 0;
        while (i < input.size() && codepoints < maxCodepoints) {
            const unsigned char c = static_cast<unsigned char>(input[i]);
            size_t step = 1;
            if ((c & 0x80u) == 0x00u) {
                step = 1;
            } else if ((c & 0xe0u) == 0xc0u) {
                step = 2;
            } else if ((c & 0xf0u) == 0xe0u) {
                step = 3;
            } else if ((c & 0xf8u) == 0xf0u) {
                step = 4;
            }
            if (i + step > input.size()) {
                step = 1;
            }
            i += step;
            ++codepoints;
        }
        return input.substr(0, i);
    }

    static std::string normalizeTextForGuitree(const std::string &input) {
        return truncateUtf8Codepoints(removeDoubleQuotes(input), 8);
    }

    static std::string normalizeContentDescForGuitree(const std::string &input) {
        return removeDoubleQuotes(input);
    }

    /// Parse one integer (optional '-', then digits) and advance p past it. Used for bounds "[xl,yl][xr,yr]".
    static int parseIntAndAdvance(const char *&p) {
        bool neg = (*p == '-');
        if (neg) ++p;
        int v = 0;
        while (*p >= '0' && *p <= '9')
            v = v * 10 + (*p++ - '0');
        return neg ? -v : v;
    }

    /// Log parsed DOM tree with node hierarchy (indent by depth). Stops when *lineCount <= 0 or depth > maxDepth.
    static void logDomTreeRecursive(const ElementPtr &e, int depth, int *lineCount, const int maxDepth = 40) {
        if (!e || depth > maxDepth) return;
        const int maxLines = 600;
        if (depth == 0) *lineCount = maxLines;
        if (*lineCount <= 0) return;
        std::string indent(static_cast<size_t>(depth * 2), ' ');
        std::string text = e->getText();
        if (text.size() > 36) text = text.substr(0, 33) + "...";
        for (char &c : text) if (c == '\n' || c == '\r') c = ' ';
        const char *rid = e->getResourceID().c_str();
        const char *clazz = e->getClassname().c_str();
        RectPtr b = e->getBounds();
        char boundsStr[32] = "";
        if (b) std::snprintf(boundsStr, sizeof(boundsStr), "[%d,%d][%d,%d]", b->left, b->top, b->right, b->bottom);
        BLOG("[domtree] %snode index=%d class=%s resource-id=%s text=\"%s\" bounds=%s children=%zu",
             indent.c_str(), e->getIndex(), clazz[0] ? clazz : "(none)", rid[0] ? rid : "(none)",
             text.c_str(), boundsStr, e->getChildren().size());
        --(*lineCount);
        for (const auto &child : e->getChildren())
            logDomTreeRecursive(child, depth + 1, lineCount, maxDepth);
    }

    /// Try short then long attribute name (SECURITY_AND_OPTIMIZATION §7 - Java outputs rid/cd/bnd etc.)
    static bool queryStringAttr(const tinyxml2::XMLElement *node, const char *shortName, const char *longName, const char *&out) {
        if (node->QueryStringAttribute(shortName, &out) == tinyxml2::XML_SUCCESS && out && *out != '\0') return true;
        if (node->QueryStringAttribute(longName, &out) == tinyxml2::XML_SUCCESS && out && *out != '\0') return true;
        return false;
    }
    static bool queryIntAttr(const tinyxml2::XMLElement *node, const char *shortName, const char *longName, int &out) {
        if (node->QueryIntAttribute(shortName, &out) == tinyxml2::XML_SUCCESS) return true;
        if (node->QueryIntAttribute(longName, &out) == tinyxml2::XML_SUCCESS) return true;
        return false;
    }
    static bool queryBoolAttr(const tinyxml2::XMLElement *node, const char *shortName, const char *longName, bool &out) {
        if (node->QueryBoolAttribute(shortName, &out) == tinyxml2::XML_SUCCESS) return true;
        if (node->QueryBoolAttribute(longName, &out) == tinyxml2::XML_SUCCESS) return true;
        return false;
    }

    static const char *rawStringAttr(const tinyxml2::XMLElement *node, const char *shortName, const char *longName) {
        const char *out = node->Attribute(shortName);
        if (out) return out;
        return node->Attribute(longName);
    }

    static bool asciiEqualsIgnoreCase(const char *value, const char *literal) {
        if (!value || !literal) {
            return false;
        }
        while (*value && *literal) {
            char v = *value++;
            char l = *literal++;
            if (v >= 'A' && v <= 'Z') v = static_cast<char>(v + 32);
            if (l >= 'A' && l <= 'Z') l = static_cast<char>(l + 32);
            if (v != l) {
                return false;
            }
        }
        return *value == *literal;
    }

    static bool hasElementChild(const tinyxml2::XMLElement *node) {
        return node && node->FirstChildElement() != nullptr;
    }

    static bool xmlChildExcludedByGuitreeRules(const tinyxml2::XMLElement *node) {
        if (!node) {
            return true;
        }
        bool visibleToUser = true;
        if (queryBoolAttr(node, "vu", "visible-to-user", visibleToUser) && !visibleToUser) {
            return true;
        }
        const char *visibility = rawStringAttr(node, "vis", "visibility");
        if (asciiEqualsIgnoreCase(visibility, "gone") || asciiEqualsIgnoreCase(visibility, "invisible")) {
            return true;
        }
        const char *bounds = rawStringAttr(node, "bnd", "bounds");
        return bounds && bounds[0] == '\0' && !hasElementChild(node);
    }

    Element::Element()
            : _enabled(false), _checked(false), _checkable(false), _clickable(false),
              _focusable(false), _scrollable(false), _longClickable(false), _childCount(0),
              _focused(false), _index(0), _password(false), _selected(false), _isEditable(false),
              _depth(1), _height(1), _descendantCount(1), _actionableDescendantCount(0),
              _cachedScrollType(ScrollType::NONE), _scrollTypeCached(false),
              _cachedHash(0), _hashCached(false) {
        _children.clear();
        this->_bounds = Rect::RectZero;
    }

    /**
     * @brief Remove this element from its parent's children list
     * 
     * Removes this element from the parent's children vector and decrements
     * the parent's child count. This is used for dynamic UI tree manipulation.
     * 
     * Performance optimization:
     * - Uses remove_if algorithm for efficient removal
     * - Locks parent weak_ptr only once and reuses it
     * 
     * @note This method does not delete the element itself, only removes it from the tree
     * @warning Root elements cannot be deleted (they have no parent)
     */
    void Element::deleteElement() {
        auto parentOfElement = this->getParent();
        if (parentOfElement.expired()) {
            BLOGE("%s", "element is a root elements");
            return;
        }
        
        // Performance: Lock parent once and reuse the shared_ptr
        auto parentLocked = parentOfElement.lock();
        
        // Use remove_if algorithm for efficient removal
        auto iter = std::remove_if(parentLocked->_children.begin(),
                                   parentLocked->_children.end(),
                                   [this](const ElementPtr &elem) { 
                                       return elem.get() == this; 
                                   });
        
        // If element was found and removed, update parent's child count
        if (iter != parentLocked->_children.end()) {
            parentLocked->_childCount--;
            parentLocked->_children.erase(iter, parentLocked->_children.end());
        }
        
        // Clear this element's parent reference
        this->_parent.reset();
    }

/// According to given xpath selector, containing text, content, classname, resource id, test if
/// this current element has the same property value as the given xpath selector.
/// Text and content-desc use fuzzy (contains) match: element text/desc containing selector value matches.
/// \param xpathSelector Describe property values of a xml element should have.
/// \return If this element could be matched to the given xpath selector, return true.
    bool Element::matchXpathSelector(const XpathPtr &xpathSelector) const {
        if (!xpathSelector)
            return false;
        bool match;
        bool isResourceIDEqual = (!xpathSelector->resourceID.empty() &&
                                  this->getResourceID() == xpathSelector->resourceID);
        // Fuzzy match: element text contains selector text (e.g. selector "登录" matches "新用户登录送会员")
        const std::string &elText = this->getText();
        bool isTextEqual = (!xpathSelector->text.empty() &&
                            elText.size() >= xpathSelector->text.size() &&
                            elText.find(xpathSelector->text) != std::string::npos);
        
        // Fuzzy match: element content-desc contains selector content-desc
        bool isContentEqual = false;
        if (!xpathSelector->contentDescription.empty()) {
            const std::string &contentDesc = this->getContentDesc();
            isContentEqual = (contentDesc.size() >= xpathSelector->contentDescription.size() &&
                              contentDesc.find(xpathSelector->contentDescription) != std::string::npos);
        }
        bool isClassNameEqual = (!xpathSelector->clazz.empty() &&
                                 this->getClassname() == xpathSelector->clazz);
        bool isIndexEqual = xpathSelector->index > -1 && this->getIndex() == xpathSelector->index;
        
        // Performance optimization: Only log detailed xpath matching when enabled
#if FASTBOT_LOG_XPATH_MATCH
        BDLOG("begin find xpathSelector :\n "
              "XPathSelector:\n resourceID: %s text: %s contentDescription: %s clazz: %s index: %d \n"
              "UIPageElement:\n resourceID: %s text: %s contentDescription: %s clazz: %s index: %d \n"
              "equality: \n isResourceIDEqual:%d isTextEqual:%d isContentEqual:%d isClassNameEqual:%d isIndexEqual:%d",
              xpathSelector->resourceID.c_str(),
              xpathSelector->text.c_str(),
              xpathSelector->contentDescription.c_str(),
              xpathSelector->clazz.c_str(),
              xpathSelector->index,
              this->getResourceID().c_str(),
              this->getText().c_str(),
              this->getContentDesc().c_str(),
              this->getClassname().c_str(),
              this->getIndex(),
              isResourceIDEqual,
              isTextEqual,
              isContentEqual,
              isClassNameEqual,
              isIndexEqual);
#endif
        if (xpathSelector->operationAND) {
            // Performance optimization: AND mode with early exit
            // Fix bug: previous code had `match = isClassNameEqual` which overwrote the initial true
            match = true;
            // Early exit: if any required field doesn't match, return false immediately
            if (!xpathSelector->clazz.empty() && !isClassNameEqual) {
                match = false;
            } else if (!xpathSelector->contentDescription.empty() && !isContentEqual) {
                match = false;
            } else if (!xpathSelector->text.empty() && !isTextEqual) {
                match = false;
            } else if (!xpathSelector->resourceID.empty() && !isResourceIDEqual) {
                match = false;
            } else if (xpathSelector->index != -1 && !isIndexEqual) {
                match = false;
            }
        } else {
            // Performance optimization: OR mode with early exit
            // Return true as soon as any field matches
            match = isResourceIDEqual || isTextEqual || isContentEqual || isClassNameEqual;
        }
        return match;
    }

    /**
     * @brief Recursively select elements that satisfy a predicate function
     * 
     * Traverses the element tree recursively and collects all elements (including
     * descendants) that match the given predicate function. Results are appended
     * to the provided result vector.
     * 
     * Performance optimization:
     * - Early return if function is null
     * - Uses const reference for children to avoid copying
     * - Result vector is passed by reference to avoid copying
     * 
     * @param func Predicate function that returns true for matching elements
     * @param result Output vector to store matching elements (appended to)
     * 
     * @note Time complexity: O(n) where n is the number of elements in the tree
     */
    void Element::recursiveElements(const std::function<bool(ElementPtr)> &func,
                                    std::vector<ElementPtr> &result) const {
        if (func != nullptr) {
            // Performance: Reserve to reduce reallocations when many children match
            result.reserve(result.size() + this->_children.size());
            for (const auto &child: this->_children) {
                // Check if current child matches
                if (func(child)) {
                    result.push_back(child);
                }
                // Recursively check children
                child->recursiveElements(func, result);
            }
        }
    }

    /**
     * @brief Recursively apply a function to all elements in the tree
     * 
     * Traverses the element tree recursively and applies the given function
     * to each element (including this element's children and their descendants).
     * 
     * Performance optimization:
     * - Early return if function is null
     * - Uses const reference for children to avoid copying
     * 
     * @param doFunc Function to apply to each element (takes ElementPtr as parameter)
     * 
     * @note Time complexity: O(n) where n is the number of elements in the tree
     * @note This is used for operations like setting all elements clickable
     */
    void Element::recursiveDoElements(const std::function<void(std::shared_ptr<Element>)> &doFunc) {
        if (doFunc != nullptr) {
            for (const auto &child: this->_children) {
                // Apply function to current child
                doFunc(child);
                // Recursively apply to children
                child->recursiveDoElements(doFunc);
            }
        }
    }

    bool Element::_allClickableFalse = false;

    /**
     * @brief Create an Element tree from XML string content
     * 
     * Parses XML string content and creates a hierarchical Element tree structure.
     * If no elements are clickable, all elements are made clickable as a fallback.
     * The root element is always set to scrollable.
     * 
     * Performance optimization:
     * - Pre-allocates string vector with estimated capacity
     * - Uses efficient string operations
     * 
     * @param xmlContent The XML content as a string
     * @return Shared pointer to root Element, or nullptr if parsing fails
     */
    ElementPtr Element::createFromXml(const std::string &xmlContent) {
        tinyxml2::XMLDocument doc;
        
        // Raw domtree XML log: line by line so node hierarchy is visible (from XML indentation)
#if FASTBOT_LOG_RAW_GUITREE
        std::vector<std::string> strings;
        strings.reserve(xmlContent.size() / 100 + 1);
        int startIndex = 0, endIndex = 0;
        for (size_t i = 0; i <= xmlContent.size(); i++) {
            if (i >= xmlContent.size() || xmlContent[i] == '\n') {
                endIndex = static_cast<int>(i);
                strings.emplace_back(xmlContent, startIndex, endIndex - startIndex);
                startIndex = endIndex + 1;
            }
        }
        BLOG("[domtree] raw XML lines=%zu (hierarchy from indentation):", strings.size());
        for (const auto &line : strings) {
            BLOG("[domtree] %s", line.c_str());
        }
#endif
        if (!FASTBOT_LOG_RAW_GUITREE) {
            BLOG("guitree size=%zu", xmlContent.size());
        }
        
        // Parse XML content
        tinyxml2::XMLError errXml = doc.Parse(xmlContent.c_str());

        if (errXml != tinyxml2::XML_SUCCESS) {
            BLOGE("parse xml error %d", static_cast<int>(errXml));
            return nullptr;
        }

        ElementPtr elementPtr = std::make_shared<Element>();

        // Track if any element is clickable during parsing. Root has no parent (nullptr).
        _allClickableFalse = true;
        elementPtr->fromXml(doc, nullptr);
        
        // If no elements are clickable, make all clickable as fallback
        // This ensures the UI can still be interacted with
        if (_allClickableFalse) {
            elementPtr->applySemanticClickFallback();
        }

        // Force set root element scrollable = true
        // Root element should always be scrollable to allow navigation
        elementPtr->_scrollable = true;
        elementPtr->optimizeTreeAfterParse();

#if FASTBOT_LOG_RAW_GUITREE
        int domTreeLineCount = 0;
        logDomTreeRecursive(elementPtr, 0, &domTreeLineCount);
        BLOG("[domtree] parsed tree (hierarchy by indent) logged above");
#endif

        doc.Clear();
        return elementPtr;
    }

    ElementPtr Element::createFromXml(const tinyxml2::XMLDocument &doc) {
        ElementPtr elementPtr = std::make_shared<Element>(); // Root element has no parent
        _allClickableFalse = true;
        elementPtr->fromXml(doc, nullptr);
        if (_allClickableFalse) {
            elementPtr->applySemanticClickFallback();
        }
        elementPtr->optimizeTreeAfterParse();
        return elementPtr;
    }

    // Binary format (little-endian): magic "FB\0\1" (4), then node: bounds(16), index(2), flags(2), num_strings(1), [tag(1) len(2) data(len)]*, num_children(2), children*
    // Binary format v2 (little-endian): magic "FB\0\2" (4), then node:
    // bounds(16), index(2), flags(2), scroll_type(1), num_strings(1), strings, num_children(2), children.
    // v1 ("FB\0\1") is still accepted and infers scroll type from class.
    static const char BINARY_MAGIC_V1[] = { 'F', 'B', 0, 1 };
    static const char BINARY_MAGIC_V2[] = { 'F', 'B', 0, 2 };
    enum { TAG_TEXT = 0, TAG_RID = 1, TAG_CLASS = 2, TAG_PKG = 3, TAG_CD = 4 };
    static bool readBytes(const char *buf, size_t len, size_t *offset, void *out, size_t n) {
        if (*offset + n > len) return false;
        memcpy(out, buf + *offset, n);
        *offset += n;
        return true;
    }

    ElementPtr Element::parseBinaryNode(const char *buf, size_t len, size_t *offset, const ElementPtr &parent,
                                    bool hasExplicitScrollType) {
        if (*offset + (hasExplicitScrollType ? 22 : 21) > len) return nullptr; // min header
        ElementPtr elm = std::make_shared<Element>();
        if (!elm->parseBinaryNodeSelf(buf, len, offset, parent, hasExplicitScrollType)) return nullptr;
        return elm;
    }

    bool Element::parseBinaryNodeSelf(const char *buf, size_t len, size_t *offset, const ElementPtr &parent,
                                  bool hasExplicitScrollType) {
        int32_t left, top, right, bottom;
        int16_t idx;
        uint16_t flags;
        uint8_t explicitScrollType = static_cast<uint8_t>(ScrollType::NONE);
        uint8_t numStrings;
        if (!readBytes(buf, len, offset, &left, 4) || !readBytes(buf, len, offset, &top, 4) ||
            !readBytes(buf, len, offset, &right, 4) || !readBytes(buf, len, offset, &bottom, 4) ||
            !readBytes(buf, len, offset, &idx, 2) || !readBytes(buf, len, offset, &flags, 2)) return false;
        if (hasExplicitScrollType && !readBytes(buf, len, offset, &explicitScrollType, 1)) return false;
        if (!readBytes(buf, len, offset, &numStrings, 1)) return false;
        if (parent) _parent = parent;
        _index = idx;
        _bounds = std::make_shared<Rect>(left, top, right, bottom);
        _checkable = (flags & 1) != 0;
        _checked = (flags & 2) != 0;
        _clickable = (flags & 4) != 0;
        if (_clickable) _allClickableFalse = false;
        _enabled = (flags & 8) != 0;
        _focusable = (flags & 16) != 0;
        _focused = (flags & 32) != 0;
        _scrollable = (flags & 64) != 0;
        _longClickable = (flags & 128) != 0;
        _password = (flags & 256) != 0;
        _selected = (flags & 512) != 0;
        for (uint8_t i = 0; i < numStrings && *offset + 3 <= len; i++) {
            uint8_t tag;
            uint16_t slen;
            if (!readBytes(buf, len, offset, &tag, 1) || !readBytes(buf, len, offset, &slen, 2) || *offset + slen > len) break;
            std::string s(buf + *offset, slen);
            *offset += slen;
            if (tag == TAG_TEXT) _text = std::move(s);
            else if (tag == TAG_RID) _resourceID = std::move(s);
            else if (tag == TAG_CLASS) _classname = std::move(s);
            else if (tag == TAG_PKG) _packageName = std::move(s);
            else if (tag == TAG_CD) _contentDesc = std::move(s);
        }
        _isEditable = isEditTextClassName(_classname);
        if (_isEditable) {
            _text.clear();
        } else {
            _text = normalizeTextForGuitree(_text);
        }
        _contentDesc = normalizeContentDescForGuitree(_contentDesc);
        uint16_t numChildren;
        if (!readBytes(buf, len, offset, &numChildren, 2)) return true;
        _children.reserve(numChildren > 32 ? 32 : numChildren);
        for (uint16_t c = 0; c < numChildren; ++c) {
            ElementPtr child = Element::parseBinaryNode(buf, len, offset, shared_from_this(), hasExplicitScrollType);
            if (!child) break;
            _children.push_back(child);
        }
        _childCount = static_cast<int>(_children.size());
        if (_isEditable) _longClickable = _clickable = _enabled = true;
        if (hasExplicitScrollType && explicitScrollType < static_cast<uint8_t>(ScrollType::ScrollTypeSize)) {
            _cachedScrollType = static_cast<ScrollType>(explicitScrollType);
        } else {
            _cachedScrollType = _computeScrollType();
        }
        _scrollTypeCached = true;
        return true;
    }

    ElementPtr Element::createFromBinary(const char *buf, size_t len) {
        if (len < 4) return nullptr;
        bool hasExplicitScrollType = false;
        if (memcmp(buf, BINARY_MAGIC_V2, 4) == 0) {
            hasExplicitScrollType = true;
        } else if (memcmp(buf, BINARY_MAGIC_V1, 4) == 0) {
            hasExplicitScrollType = false;
        } else {
            return nullptr;
        }

        size_t offset = 4;
        Element::_allClickableFalse = true;
        ElementPtr root = Element::parseBinaryNode(buf, len, &offset, nullptr, hasExplicitScrollType);
        if (!root) return nullptr;
        if (Element::_allClickableFalse) {
            root->applySemanticClickFallback();
        }
        root->_scrollable = true;
        root->optimizeTreeAfterParse();
#if FASTBOT_LOG_RAW_GUITREE
        BLOG("[domtree] from binary (no raw XML); parsed tree hierarchy:");
        int domTreeLineCount = 0;
        logDomTreeRecursive(root, 0, &domTreeLineCount);
#endif
        return root;
    }

    void Element::fromJson(const std::string &/*jsonData*/) {
        //nlohmann::json
    }

    std::string Element::toString() const {
        return this->toJson();
    }


    std::string Element::toJson() const {
        nlohmann::json j;
        RectPtr bounds = this->getBounds();
        j["bounds"] = bounds ? bounds->toString().c_str() : "";
        j["index"] = this->getIndex();
        j["class"] = this->getClassname().c_str();
        j["resource-id"] = this->getResourceID().c_str();
        j["package"] =  this->getPackageName().c_str();
        j["content-desc"] = this->getContentDesc().c_str();
        j["checkable"] = this->getCheckable() ? "true" : "false";
        j["checked"] = this->_checked ? "true" : "false";
        j["clickable"] = this->getClickable() ? "true" : "false";
        j["enabled"] = this->getEnable() ? "true" : "false";
        j["focusable"] = this->_focusable ? "true" : "false";
        j["focused"] = this->_focused ? "true" : "false";
        j["scrollable"] = this->_scrollable ? "true" : "false";
        j["long-clickable"] = this->_longClickable ? "true" : "false";
        j["password"] = this->_password ? "true" : "false";
        return j.dump();
    }

    void Element::recursiveToXML(tinyxml2::XMLElement *xml, const Element *elm) const {
        RectPtr bounds = elm->getBounds();
        if (bounds) {
            char boundsBuf[48];
            std::snprintf(boundsBuf, sizeof(boundsBuf), "[%d,%d][%d,%d]",
                          bounds->left, bounds->top, bounds->right, bounds->bottom);
            xml->SetAttribute("bounds", boundsBuf);
        } else {
            xml->SetAttribute("bounds", "");
        }
        xml->SetAttribute("index", elm->getIndex());
        xml->SetAttribute("class", elm->getClassname().c_str());
        xml->SetAttribute("resource-id", elm->getResourceID().c_str());
        xml->SetAttribute("package", elm->getPackageName().c_str());
        std::string text = elm->isEditText() ? std::string() : normalizeTextForGuitree(elm->getText());
        std::string contentDesc = normalizeContentDescForGuitree(elm->getContentDesc());
        xml->SetAttribute("text", text.c_str());
        xml->SetAttribute("content-desc", contentDesc.c_str());
        xml->SetAttribute("checkable", elm->getCheckable() ? "true" : "false");
        xml->SetAttribute("checked", elm->_checked ? "true" : "false");
        xml->SetAttribute("clickable", elm->getClickable() ? "true" : "false");
        xml->SetAttribute("enabled", elm->getEnable() ? "true" : "false");
        xml->SetAttribute("focusable", elm->_focusable ? "true" : "false");
        xml->SetAttribute("focused", elm->_focused ? "true" : "false");
        xml->SetAttribute("scrollable", elm->_scrollable ? "true" : "false");
        xml->SetAttribute("long-clickable", elm->_longClickable ? "true" : "false");
        xml->SetAttribute("password", elm->_password ? "true" : "false");
        xml->SetAttribute("scroll-type", "none");

        const auto &children = elm->getChildren();
        for (const auto &child : children) {
            tinyxml2::XMLElement *xmlChild = xml->InsertNewChildElement("node");
            xml->LinkEndChild(xmlChild);
            recursiveToXML(xmlChild, child.get());
        }
    }

    std::string Element::toXML() const {
        tinyxml2::XMLDocument doc;
        tinyxml2::XMLDeclaration *xmlDeclarationNode = doc.NewDeclaration();
        doc.InsertFirstChild(xmlDeclarationNode);

        tinyxml2::XMLElement *root = doc.NewElement("node");
        recursiveToXML(root, this);
        doc.LinkEndChild(root);

        tinyxml2::XMLPrinter printer;
        doc.Print(&printer);
        std::string xmlStr = std::string(printer.CStr());
        return xmlStr;
    }

    void Element::fromXml(const tinyxml2::XMLDocument &nodeOfDoc, const ElementPtr &parentOfNode) {
        const ::tinyxml2::XMLElement *node = nodeOfDoc.RootElement();
        this->fromXMLNode(node, parentOfNode);

        if (0 != nodeOfDoc.ErrorID())
            BLOGE("parse xml error %s", nodeOfDoc.ErrorStr());

    }

    void Element::fromXMLNode(const tinyxml2::XMLElement *xmlNode, const ElementPtr &parentOfNode) {
        if (nullptr == xmlNode)
            return;
        if (parentOfNode)
            this->_parent = parentOfNode;
        int indexOfNode = 0;
        if (queryIntAttr(xmlNode, "idx", "index", indexOfNode))
            this->_index = indexOfNode;
        const char *boundingBoxStr = nullptr;
        if (queryStringAttr(xmlNode, "bnd", "bounds", boundingBoxStr) && boundingBoxStr && *boundingBoxStr == '[') {
            const char *p = boundingBoxStr + 1;
            int xl = parseIntAndAdvance(p);
            if (*p == ',') {
                ++p;
                int yl = parseIntAndAdvance(p);
                if (p[0] == ']' && p[1] == '[') {
                    p += 2;
                    int xr = parseIntAndAdvance(p);
                    if (*p == ',') {
                        ++p;
                        int yr = parseIntAndAdvance(p);
                        if (*p == ']') {
                            this->_bounds = std::make_shared<Rect>(xl, yl, xr, yr);
                            if (this->_bounds->isEmpty())
                                this->_bounds = Rect::RectZero;
                        }
                    }
                }
            }
        }
        const char *text = nullptr;
        if (queryStringAttr(xmlNode, "t", "text", text)) this->_text = std::string(text);
        const char *resource_id = nullptr;
        if (queryStringAttr(xmlNode, "rid", "resource-id", resource_id)) this->_resourceID = std::string(resource_id);
        const char *tclassname = nullptr;
        if (queryStringAttr(xmlNode, "class", "class", tclassname)) this->_classname = std::string(tclassname);
        const char *pkgname = nullptr;
        if (queryStringAttr(xmlNode, "pkg", "package", pkgname)) this->_packageName = std::string(pkgname);
        const char *content_desc = nullptr;
        if (queryStringAttr(xmlNode, "cd", "content-desc", content_desc)) this->_contentDesc = std::string(content_desc);
        this->_isEditable = isEditTextClassName(this->_classname);
        if (this->_isEditable) {
            this->_text.clear();
        } else {
            this->_text = normalizeTextForGuitree(this->_text);
        }
        this->_contentDesc = normalizeContentDescForGuitree(this->_contentDesc);
        bool b = false;
        if (queryBoolAttr(xmlNode, "ck", "checkable", b)) this->_checkable = b;
        if (queryBoolAttr(xmlNode, "clk", "clickable", b)) { this->_clickable = b; if (b) _allClickableFalse = false; }
        if (queryBoolAttr(xmlNode, "cked", "checked", b)) this->_checked = b;
        if (queryBoolAttr(xmlNode, "en", "enabled", b)) this->_enabled = b;
        if (queryBoolAttr(xmlNode, "fcd", "focused", b)) this->_focused = b;
        if (queryBoolAttr(xmlNode, "foc", "focusable", b)) this->_focusable = b;
        if (queryBoolAttr(xmlNode, "scl", "scrollable", b)) this->_scrollable = b;
        if (queryBoolAttr(xmlNode, "lclk", "long-clickable", b)) this->_longClickable = b;
        if (queryBoolAttr(xmlNode, "pwd", "password", b)) this->_password = b;
        if (queryBoolAttr(xmlNode, "sel", "selected", b)) this->_selected = b;
        const char *scrollType = nullptr;
        if (queryStringAttr(xmlNode, "st", "scroll-type", scrollType)) {
            this->_cachedScrollType = stringToScrollType(scrollType);
            this->_scrollTypeCached = true;
        }

        if (FORCE_EDITTEXT_CLICK_TRUE && this->_isEditable) {
            this->_longClickable = this->_clickable = this->_enabled = true;
        }

#if !FASTBOT_PATCH_CONTAINER_ACTIONS
        if (PARENT_CLICK_CHANGE_CHILDREN && parentOfNode && parentOfNode->_longClickable) {
            this->_longClickable = parentOfNode->_longClickable;
        }
        if (PARENT_CLICK_CHANGE_CHILDREN && parentOfNode && parentOfNode->_clickable) {
            this->_clickable = parentOfNode->_clickable;
        }
#endif
        if (this->_clickable || this->_longClickable) {
            this->_enabled = true;
        }

        if (!this->_scrollTypeCached) {
            this->_cachedScrollType = this->_computeScrollType();
            this->_scrollTypeCached = true;
        }

        // Performance: Only call shared_from_this() and reserve when node has children (most nodes are leaves).
        if (!xmlNode->NoChildren()) {
            this->_children.reserve(8);
            const ElementPtr self = shared_from_this();
            for (const tinyxml2::XMLElement *childNode = xmlNode->FirstChildElement();
                 childNode != nullptr; childNode = childNode->NextSiblingElement()) {
                if (xmlChildExcludedByGuitreeRules(childNode)) {
                    continue;
                }
                ElementPtr childElement = std::make_shared<Element>();
                this->_children.emplace_back(childElement);
                childElement->fromXMLNode(childNode, self);
            }
        }
        this->_childCount = static_cast<int>(this->_children.size());
    }

    bool Element::isWebView() const {
        return "android.webkit.WebView" == this->_classname;
    }

    bool Element::isEditText() const {
        return this->_isEditable;
    }

    bool Element::hasActionableFlag() const {
        return this->_clickable || this->_longClickable || this->_checkable || this->_scrollable || this->_isEditable;
    }

    bool Element::hasValidActionBounds() const {
        if (!this->_bounds || this->_bounds->isEmpty()) {
            return false;
        }
        return this->_bounds->right > this->_bounds->left && this->_bounds->bottom > this->_bounds->top;
    }

    void Element::clearActions() {
        this->_clickable = false;
        this->_longClickable = false;
        this->_checkable = false;
        this->_scrollable = false;
        this->_scrollTypeCached = false;
        this->_hashCached = false;
    }

    static bool sameRow(const std::vector<ElementPtr> &children) {
        int top = -1, bottom = -1;
        for (const auto &child : children) {
            RectPtr bounds = child ? child->getBounds() : nullptr;
            if (!bounds) continue;
            if (top == -1) top = bounds->top;
            else if (top != bounds->top) return false;
            if (bottom == -1) bottom = bounds->bottom;
            else if (bottom != bounds->bottom) return false;
        }
        return top != -1;
    }

    static bool sameColumn(const std::vector<ElementPtr> &children) {
        int left = -1, right = -1;
        for (const auto &child : children) {
            RectPtr bounds = child ? child->getBounds() : nullptr;
            if (!bounds) continue;
            if (left == -1) left = bounds->left;
            else if (left != bounds->left) return false;
            if (right == -1) right = bounds->right;
            else if (right != bounds->right) return false;
        }
        return left != -1;
    }

    static bool containsRect(const RectPtr &outer, const RectPtr &inner) {
        return outer && inner && outer->left <= inner->left && outer->top <= inner->top
            && outer->right >= inner->right && outer->bottom >= inner->bottom;
    }

    static RectPtr unionChildBounds(const std::vector<ElementPtr> &children) {
        RectPtr result = nullptr;
        for (const auto &child : children) {
            RectPtr bounds = child ? child->getBounds() : nullptr;
            if (!bounds || bounds->isEmpty()) continue;
            if (!result) {
                result = std::make_shared<Rect>(bounds->left, bounds->top, bounds->right, bounds->bottom);
            } else {
                result->left = std::min(result->left, bounds->left);
                result->top = std::min(result->top, bounds->top);
                result->right = std::max(result->right, bounds->right);
                result->bottom = std::max(result->bottom, bounds->bottom);
            }
        }
        return result;
    }

    static bool containsPoint(const RectPtr &rect, int x, int y) {
        return rect && rect->left <= x && x < rect->right && rect->top <= y && y < rect->bottom;
    }

    static bool intersectsRect(const RectPtr &a, const RectPtr &b) {
        return a && b && a->left < b->right && b->left < a->right
            && a->top < b->bottom && b->top < a->bottom;
    }

    bool Element::patchChildrenFromContainer() {
        if (!_clickable || _children.empty() || isWebView()) {
            return false;
        }
        if (_children.size() > 1 && !sameRow(_children) && !sameColumn(_children)) {
            return false;
        }
        bool patched = false;
        for (const auto &child : _children) {
            if (!child || !containsRect(_bounds, child->getBounds()) || !child->hasValidActionBounds()) {
                continue;
            }
            if (!child->_clickable && !child->_checkable && !child->_scrollable) {
                child->_clickable = true;
                child->_enabled = true;
                child->_hashCached = false;
                patched = true;
            }
        }
        if (patched) {
            RectPtr childrenBounds = unionChildBounds(_children);
            if (childrenBounds && _bounds) {
                int centerX = _bounds->left + (_bounds->right - _bounds->left) / 2;
                int centerY = _bounds->top + (_bounds->bottom - _bounds->top) / 2;
                if (containsPoint(childrenBounds, centerX, centerY)) {
                    this->_clickable = false;
                    this->_hashCached = false;
                }
            }
        }
        return patched;
    }

    void Element::patchContainerActions() {
        patchChildrenFromContainer();
        for (const auto &child : _children) {
            if (child) child->patchContainerActions();
        }
    }

    bool Element::pruneLargeWebViews() {
        bool changed = false;
        if (isWebView() && _descendantCount > FASTBOT_WEBVIEW_NODE_THRESHOLD) {
            BLOG("prune WebView subtree nodes=%d threshold=%d", _descendantCount, FASTBOT_WEBVIEW_NODE_THRESHOLD);
            _children.clear();
            _childCount = 0;
            changed = true;
        } else {
            for (const auto &child : _children) {
                if (child && child->pruneLargeWebViews()) changed = true;
            }
        }
        if (changed) this->_hashCached = false;
        return changed;
    }

    void Element::clearActionsInWebView(bool insideWebView) {
        bool ignore = insideWebView || isWebView();
        if (insideWebView) {
            clearActions();
        }
        for (const auto &child : _children) {
            if (child) child->clearActionsInWebView(ignore);
        }
    }

    void Element::clearInvalidActions(const RectPtr &rootBounds) {
        bool outsideRoot = rootBounds && hasValidActionBounds() && !intersectsRect(rootBounds, _bounds);
        if (hasActionableFlag() && (!hasValidActionBounds() || (_depth > 1 && outsideRoot))) {
            clearActions();
        }
        for (const auto &child : _children) {
            if (child) child->clearInvalidActions(rootBounds);
        }
    }

    void Element::applySemanticClickFallback() {
        if (!FASTBOT_SEMANTIC_CLICK_FALLBACK) {
            recursiveDoElements([](const ElementPtr &elm) { elm->_clickable = true; });
            return;
        }
        bool applied = false;
        recursiveDoElements([&applied](const ElementPtr &elm) {
            if (!elm || !elm->hasValidActionBounds() || !elm->_children.empty()) return;
            if (!elm->_text.empty() || !elm->_contentDesc.empty() || !elm->_resourceID.empty() || elm->_isEditable) {
                elm->_clickable = true;
                elm->_enabled = true;
                applied = true;
            }
        });
        if (!applied) {
            recursiveDoElements([](const ElementPtr &elm) {
                if (elm && elm->hasValidActionBounds() && elm->_children.empty()) {
                    elm->_clickable = true;
                    elm->_enabled = true;
                }
            });
        }
    }

    void Element::computeTreeStats(int depth) {
        _depth = depth;
        _height = 1;
        _descendantCount = 1;
        _actionableDescendantCount = hasActionableFlag() ? 1 : 0;
        for (const auto &child : _children) {
            if (!child) continue;
            child->computeTreeStats(depth + 1);
            _descendantCount += child->_descendantCount;
            _actionableDescendantCount += child->_actionableDescendantCount;
            _height = std::max(_height, child->_height + 1);
        }
    }

    void Element::optimizeTreeAfterParse() {
        computeTreeStats(1);
#if FASTBOT_WEBVIEW_NODE_THRESHOLD > 0
        if (pruneLargeWebViews()) {
            computeTreeStats(1);
        }
#endif
#if FASTBOT_FILTER_INVALID_ACTION_BOUNDS
        clearInvalidActions(this->_bounds);
#endif
#if FASTBOT_IGNORE_WEBVIEW_ACTIONS
        clearActionsInWebView(false);
#endif
#if FASTBOT_PATCH_CONTAINER_ACTIONS
        patchContainerActions();
#endif
        computeTreeStats(1);
    }

    ScrollType Element::_computeScrollType() const {
        if (!this->_scrollable) {
            return ScrollType::NONE;
        }
        if ("android.widget.ScrollView" == this->_classname
            || "android.widget.ListView" == _classname
            || "android.widget.ExpandableListView" == _classname
            || "android.support.v17.leanback.widget.VerticalGridView" == _classname
            || "android.support.v7.widget.RecyclerView" == _classname
            || "androidx.recyclerview.widget.RecyclerView" == _classname) {
            return ScrollType::Vertical;
        } else if ("android.widget.HorizontalScrollView" == _classname
                   || "android.support.v17.leanback.widget.HorizontalGridView" == _classname
                   || "android.support.v4.view.ViewPager" == _classname) {
            return ScrollType::Horizontal;
        }
        if (this->_classname.find("ScrollView") != std::string::npos) {
            return ScrollType::ALL;
        }

        // for ios
//    return ScrollType::NONE;
        return ScrollType::ALL;
    }

    ScrollType Element::getScrollType() const {
        // Performance optimization: Return cached value if available
        if (this->_scrollTypeCached) {
            return this->_cachedScrollType;
        }
        // Fallback: compute and cache (should not happen in normal flow)
        this->_cachedScrollType = this->_computeScrollType();
        this->_scrollTypeCached = true;
        return this->_cachedScrollType;
    }

    Element::~Element() {
        this->_children.clear();
        this->_parent.reset();
    }

    /**
     * @brief Compute hash code for this element and optionally its children
     * 
     * Generates a hash code based on element properties (resource ID, class name,
     * package name, text, content description, activity, clickable state).
     * If recursive is true, also includes children hashes with order information.
     * 
     * Performance optimization:
     * - Uses bit shifting and XOR for efficient hash combination
     * - Caches individual property hashes before combining
     * - Only processes children if recursive flag is set
     * 
     * @param recursive If true, include children in hash computation
     * @return Hash code as long integer
     * 
     * @note Hash computation includes order information for children when recursive=true
     */
    long Element::hash(bool recursive) {
        // Performance optimization: Return cached hash if available and recursive flag matches
        // Note: We cache only recursive hash (recursive=true) as it's the most expensive
        // Non-recursive hash is cheap and rarely reused, so we don't cache it
        if (recursive && this->_hashCached) {
            return this->_cachedHash;
        }
        
        uintptr_t hashcode = 0x1;
        
        // Performance optimization: Use fast string hash function instead of std::hash
        // This provides better performance for typical UI strings (short to medium length)
        // Compute individual property hashes with different bit shifts for better distribution
        uintptr_t hashcode1 = 127U * fastbotx::fastStringHash(this->_resourceID) << 1;
        uintptr_t hashcode2 = fastbotx::fastStringHash(this->_classname) << 2;
        uintptr_t hashcode3 = fastbotx::fastStringHash(this->_packageName) << 3;
        uintptr_t hashcode4 = 256U * fastbotx::fastStringHash(this->_text) << 4;
        
        // Performance optimization: Only compute ContentDesc hash if not empty
        // Most elements don't have ContentDesc, so this avoids unnecessary hash computation
        // Use fast string hash for better performance
        uintptr_t hashcode5 = 0;
        if (!this->_contentDesc.empty()) {
            hashcode5 = fastbotx::fastStringHash(this->_contentDesc) << 5;
        }
        
        uintptr_t hashcode6 = fastbotx::fastStringHash(this->_activity) << 2;
        uintptr_t hashcode7 = 64U * std::hash<int>{}(static_cast<int>(this->_clickable)) << 6;

        // Combine all property hashes using XOR
        hashcode = hashcode1 ^ hashcode2 ^ hashcode3 ^ hashcode4 ^ hashcode5 ^ hashcode6 ^ hashcode7;
        
        // If recursive, include children hashes with order information
        if (recursive) {
            // Performance: Use size_t for index to match container type
            for (size_t i = 0; i < this->_children.size(); i++) {
                // Get child hash and shift for better distribution
                long childHash = this->_children[i]->hash() << 2;
                hashcode ^= static_cast<uintptr_t>(childHash);
                // Include order information in hash to distinguish different child orders
                hashcode ^= 0x7398c + (std::hash<size_t>{}(i) << 8);
            }
            
            // Cache the computed hash for future use
            this->_cachedHash = static_cast<long>(hashcode);
            this->_hashCached = true;
        }
        
        return static_cast<long>(hashcode);
    }

}

#endif //Element_CPP_
