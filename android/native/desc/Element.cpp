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
#include <cstring>


namespace fastbotx {

    Element::Element()
            : _enabled(false), _checked(false), _checkable(false), _clickable(false),
              _focusable(false), _scrollable(false), _longClickable(false), _visible(true),
              _childCount(0), _focused(false), _index(0), _password(false), _selected(false),
              _isEditable(false) {
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
        // Lock parent once; if expired, lock() returns nullptr.
        auto parentLocked = this->getParent().lock();
        if (!parentLocked) {
            BLOGE("%s", "element is a root elements");
            return;
        }
        
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

    void Element::adoptChildrenToParent() {
        auto parentOfElement = this->getParent();
        if (parentOfElement.expired()) {
            return;
        }
        auto parentLocked = parentOfElement.lock();
        for (const auto &child : this->_children) {
            if (child) {
                child->_parent = parentLocked;
                parentLocked->_children.emplace_back(child);
            }
        }
        this->_children.clear();
        this->deleteElement();
    }

    void Element::clearChildren() {
        this->_children.clear();
        this->_childCount = 0;
    }

    int Element::countDescendants() const {
        int count = 0;
        for (const auto &child : _children) {
            if (!child) {
                continue;
            }
            count += 1;
            count += child->countDescendants();
        }
        return count;
    }

/// According to given xpath selector, containing text, content, classname, resource id, test if
/// this current element has the same property value as the given xpath selector.
/// \param xpathSelector Describe property values of a xml element should have.
/// \return If this element could be matched to the given xpath selector, return true.
    bool Element::matchXpathSelector(const XpathPtr &xpathSelector) const {
        if (!xpathSelector)
            return false;
        bool match;
        bool isResourceIDEqual = (!xpathSelector->resourceID.empty() &&
                                  this->getResourceID() == xpathSelector->resourceID);
        bool isTextEqual = (!xpathSelector->text.empty() && this->getText() == xpathSelector->text);
        bool isContentEqual = (!xpathSelector->contentDescription.empty() &&
                               this->getContentDesc() == xpathSelector->contentDescription);
        bool isClassNameEqual = (!xpathSelector->clazz.empty() &&
                                 this->getClassname() == xpathSelector->clazz);
        bool isIndexEqual = xpathSelector->index > -1 && this->getIndex() == xpathSelector->index;
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
        if (xpathSelector->operationAND) {
            match = true;
            if (!xpathSelector->clazz.empty())
                match = isClassNameEqual;
            if (!xpathSelector->contentDescription.empty())
                match = match && isContentEqual;
            if (!xpathSelector->text.empty())
                match = match && isTextEqual;
            if (!xpathSelector->resourceID.empty())
                match = match && isResourceIDEqual;
            if (xpathSelector->index != -1)
                match = match && isIndexEqual;
        } else
            match = isResourceIDEqual || isTextEqual || isContentEqual || isClassNameEqual;
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
            // Performance: Reserve capacity if we can estimate result size
            // (Not done here as it's hard to estimate, but could be optimized further)
            for (const auto &child: this->_children) {
                if (!child) {
                    continue;
                }
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
                if (!child) {
                    continue;
                }
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

#if _DEBUG_
        // Debug-only: log XML content line by line.
        // Keep it out of release builds to avoid heavy string work and log spam.
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
        for (const auto &line: strings) {
            BLOG("The content of XML is: %s", line.c_str());
        }
#endif
        
        // Parse XML content
        tinyxml2::XMLError errXml = doc.Parse(xmlContent.c_str());

        if (errXml != tinyxml2::XML_SUCCESS) {
            BLOGE("parse xml error %d", static_cast<int>(errXml));
            return nullptr;
        }

        ElementPtr elementPtr = std::make_shared<Element>();

        // Track if any element is clickable during parsing
        _allClickableFalse = true;
        // Root has no parent.
        elementPtr->fromXml(doc, nullptr);

        // Diagnostic: if the parsed tree has no children, we most likely received
        // a truncated hierarchy (only root node). This would lead to scroll-only actions.
        // Keep this log lightweight (prefix only) to avoid logcat truncation.
        if (elementPtr->_childCount == 0 && elementPtr->countDescendants() == 0) {
            std::string prefix = xmlContent.substr(0, std::min<std::size_t>(512, xmlContent.size()));
            BLOGE("Parsed GUI tree has no children. XML prefix: %s", prefix.c_str());
        }
        
        // If no elements are clickable, make all clickable as fallback
        // This ensures the UI can still be interacted with
        if (_allClickableFalse) {
            elementPtr->recursiveDoElements([](const ElementPtr &elm) {
                elm->_clickable = true;
            });
        }
        
        // Force set root element scrollable = true
        // Root element should always be scrollable to allow navigation
        elementPtr->_scrollable = true;
        
        doc.Clear();
        return elementPtr;
    }

    ElementPtr Element::createFromXml(const tinyxml2::XMLDocument &doc) {
        ElementPtr elementPtr = std::make_shared<Element>(); // Use the empty element as the FAKE root element
        _allClickableFalse = true;
        // Root has no parent.
        elementPtr->fromXml(doc, nullptr);
        if (_allClickableFalse) {
            elementPtr->recursiveDoElements([](const ElementPtr &elm) {
                elm->_clickable = true;
            });
        }
        return elementPtr;
    }

    void Element::fromJson(const std::string &/*jsonData*/) {
        //nlohmann::json
    }

    std::string Element::toString() const {
        return this->toJson();
    }


    std::string Element::toJson() const {
        nlohmann::json j;
        j["bounds"] = this->getBounds()->toString().c_str();
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
        BDLOG("add a xml %p %p", xml, elm);
        xml->SetAttribute("bounds", elm->getBounds()->toString().c_str());
        BDLOG("add a xml 111");
        xml->SetAttribute("index", elm->getIndex());
        xml->SetAttribute("class", elm->getClassname().c_str());
        xml->SetAttribute("resource-id", elm->getResourceID().c_str());
        xml->SetAttribute("package", elm->getPackageName().c_str());
        xml->SetAttribute("content-desc", elm->getContentDesc().c_str());
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

        BDLOG("add a xml 1111");
        for (const auto &child: elm->getChildren()) {
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
        if (xmlNode == nullptr)
            return;
        // Set parent link for this element.
        this->_parent = parentOfNode;
//    BLOG("This Node is %s", std::string(xmlNode->GetText()).c_str());
        int indexOfNode = 0;
        tinyxml2::XMLError err = xmlNode->QueryIntAttribute("index", &indexOfNode);
        if (err == tinyxml2::XML_SUCCESS) {
            this->_index = indexOfNode;
        }
        const char *boundingBoxStr = "get attribute failed";
        err = xmlNode->QueryStringAttribute("bounds", &boundingBoxStr);
        if (err == tinyxml2::XML_SUCCESS) {
            int xl, yl, xr, yr;
            if (sscanf(boundingBoxStr, "[%d,%d][%d,%d]", &xl, &yl, &xr, &yr) == 4) {
                this->_bounds = std::make_shared<Rect>(xl, yl, xr, yr);
                if (this->_bounds->isEmpty())
                    this->_bounds = Rect::RectZero;
            }
        }
        const char *text = "attribute text get failed";  // need copy
        err = xmlNode->QueryStringAttribute("text", &text);
        if (err == tinyxml2::XML_SUCCESS) {
            this->_text = std::string(text); // copy
        }
        const char *resource_id = "attribute resource_id get failed";  // need copy
        err = xmlNode->QueryStringAttribute("resource-id", &resource_id);
        if (err == tinyxml2::XML_SUCCESS) {
            this->_resourceID = std::string(resource_id); // copy
        }
        const char *tclassname = "attribute class name get failed";  // need copy
        err = xmlNode->QueryStringAttribute("class", &tclassname);
        if (err == tinyxml2::XML_SUCCESS) {
            this->_classname = std::string(tclassname); // copy
        }
        const char *pkgname = "attribute package name get failed";  // need copy
        err = xmlNode->QueryStringAttribute("package", &pkgname);
        if (err == tinyxml2::XML_SUCCESS) {
            this->_packageName = std::string(pkgname); // copy
        }
        const char *content_desc = "attribute content description get failed";  // need copy
        err = xmlNode->QueryStringAttribute("content-desc", &content_desc);
        if (err == tinyxml2::XML_SUCCESS) {
            this->_contentDesc = std::string(content_desc); // copy
        }
        bool checkable = false;
        err = xmlNode->QueryBoolAttribute("checkable", &checkable);
        if (err == tinyxml2::XML_SUCCESS) {
            this->_checkable = checkable;
        }
        bool clickable = false;
        err = xmlNode->QueryBoolAttribute("clickable", &clickable);
        if (err == tinyxml2::XML_SUCCESS) {
            this->_clickable = clickable;
        }
        if (clickable)
            _allClickableFalse = false;
        bool checked = false;
        err = xmlNode->QueryBoolAttribute("checked", &checked);
        if (err == tinyxml2::XML_SUCCESS) {
            this->_checked = checked;
        }
        bool enabled = false;
        err = xmlNode->QueryBoolAttribute("enabled", &enabled);
        if (err == tinyxml2::XML_SUCCESS) {
            this->_enabled = enabled;
        }
        bool focused = false;
        err = xmlNode->QueryBoolAttribute("focused", &focused);
        if (err == tinyxml2::XML_SUCCESS) {
            this->_focused = focused;
        }
        bool focusable = false;
        err = xmlNode->QueryBoolAttribute("focusable", &focusable);
        if (err == tinyxml2::XML_SUCCESS) {
            this->_focusable = focusable;
        }
        bool scrollable = false;
        err = xmlNode->QueryBoolAttribute("scrollable", &scrollable);
        if (err == tinyxml2::XML_SUCCESS) {
            this->_scrollable = scrollable;
        }
        bool longclickable = false;
        err = xmlNode->QueryBoolAttribute("long-clickable", &longclickable);
        if (err == tinyxml2::XML_SUCCESS) {
            this->_longClickable = longclickable;
        }
        bool password = false;
        err = xmlNode->QueryBoolAttribute("password", &password);
        if (err == tinyxml2::XML_SUCCESS) {
            this->_password = password;
        }
        bool selected = false;
        err = xmlNode->QueryBoolAttribute("selected", &selected);
        if (err == tinyxml2::XML_SUCCESS) {
            this->_selected = selected;
        }

        bool visibleToUser = true;
        err = xmlNode->QueryBoolAttribute("visible-to-user", &visibleToUser);
        if (err == tinyxml2::XML_SUCCESS) {
            this->_visible = visibleToUser;
        }

        this->_isEditable = "android.widget.EditText" == this->_classname;
        if (FORCE_EDITTEXT_CLICK_TRUE && this->_isEditable) {
            this->_longClickable = this->_clickable = this->_enabled = true;
        }

        if (PARENT_CLICK_CHANGE_CHILDREN && parentOfNode && parentOfNode->_longClickable) {
            this->_longClickable = parentOfNode->_longClickable;
        }
        if (PARENT_CLICK_CHANGE_CHILDREN && parentOfNode && parentOfNode->_clickable) {
            this->_clickable = parentOfNode->_clickable;
        }
        if (this->_clickable || this->_longClickable) {
            this->_enabled = true;
        }

#if FORCE_RECOVER_MISSING_CLICK_ACTIONS
        // Ape-style principle: do not lose potentially interactive widgets.
        // Some UI hierarchies expose tappable elements without setting clickable=true.
        // Recover a CLICK action for "interesting" leaf nodes to avoid exploration
        // degenerating into scroll-only behavior.
        //
        // Heuristic (conservative):
        // - only for leaf nodes (no children)
        // - visible to user
        // - enabled or focusable
        // - has non-empty bounds
        // - has at least one stable identifier (resource-id/text/content-desc)
        if (!this->_clickable && !this->_longClickable && !this->_checkable && !this->_scrollable &&
            xmlNode->NoChildren() && this->_visible && (this->_enabled || this->_focusable) &&
            this->_bounds && !this->_bounds->isEmpty() &&
            (!this->_resourceID.empty() || !this->_text.empty() || !this->_contentDesc.empty())) {
            this->_clickable = true;
            // Only log in debug builds (BDLOG is a no-op otherwise).
            BDLOG("Recover CLICK for non-clickable leaf: class=%s rid=%s text=%s desc=%s",
                  this->_classname.c_str(), this->_resourceID.c_str(), this->_text.c_str(),
                  this->_contentDesc.c_str());
        }
#endif

        int childrenCountOfCurrentNode = 0;
        if (!xmlNode->NoChildren()) {
            // Capture shared_ptr for parent assignment in children.
            ElementPtr self = shared_from_this();
            for (const tinyxml2::XMLElement *childNode = xmlNode->FirstChildElement();
                 nullptr != childNode; childNode = childNode->NextSiblingElement()) {
                const char *childClass = "";
                childNode->QueryStringAttribute("class", &childClass);
                bool childVisible = true;
                childNode->QueryBoolAttribute("visible-to-user", &childVisible);
                const char *childRes = "";
                childNode->QueryStringAttribute("resource-id", &childRes);
                const char *childText = "";
                childNode->QueryStringAttribute("text", &childText);
                const char *childContent = "";
                childNode->QueryStringAttribute("content-desc", &childContent);

#if IGNORE_INVISIBLE_NODE
                if (!childVisible) {
                    continue;
                }
#endif
#if ALWAYS_IGNORE_WEBVIEW
                if (childClass != nullptr && std::strcmp(childClass, "android.webkit.WebView") == 0) {
                    continue;
                }
#endif
#if EXCLUDE_EMPTY_CHILD
                // Avoid constructing temporary std::string objects on the hot path.
                const bool classEmpty = (childClass == nullptr) || (*childClass == '\0');
                const bool resEmpty = (childRes == nullptr) || (*childRes == '\0');
                const bool textEmpty = (childText == nullptr) || (*childText == '\0');
                const bool contentEmpty = (childContent == nullptr) || (*childContent == '\0');
                if (childNode->NoChildren() && classEmpty && resEmpty && textEmpty && contentEmpty) {
                    continue;
                }
#endif

                const tinyxml2::XMLElement *nextXMLElement = childNode;
                ElementPtr childElement = std::make_shared<Element>();
                this->_children.emplace_back(childElement);
                childrenCountOfCurrentNode++;
                // Parse child with correct parent links.
                childElement->fromXMLNode(nextXMLElement, self);
            }
        }
        this->_childCount = childrenCountOfCurrentNode;
    }

    bool Element::isWebView() const {
        return "android.webkit.WebView" == this->_classname;
    }

    bool Element::isEditText() const {
        return this->_isEditable;
    }

    ScrollType Element::getScrollType() const {
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
        uintptr_t hashcode = 0x1;
        
        // Compute individual property hashes with different bit shifts for better distribution
        uintptr_t hashcode1 = 127U * std::hash<std::string>{}(this->_resourceID) << 1;
        uintptr_t hashcode2 = std::hash<std::string>{}(this->_classname) << 2;
        uintptr_t hashcode3 = std::hash<std::string>{}(this->_packageName) << 3;
        uintptr_t hashcode4 = 256U * std::hash<std::string>{}(this->_text) << 4;
        uintptr_t hashcode5 = std::hash<std::string>{}(this->_contentDesc) << 5;
        uintptr_t hashcode6 = std::hash<std::string>{}(this->_activity) << 2;
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
        }
        
        return static_cast<long>(hashcode);
    }

}

#endif //Element_CPP_
