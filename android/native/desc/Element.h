/*
 * This code is licensed under the Fastbot license. You may obtain a copy of this license in the LICENSE.txt file in the root directory of this source tree.
 */
/**
 * @authors Jianqiang Guo, Yuhui Su, Zhao Zhang
 */
#ifndef Element_H_
#define Element_H_

#include "../Base.h"
#include <string>
#include <utility>
#include <vector>
#include <memory>
#include <functional>

namespace tinyxml2 {
    class XMLElement;

    class XMLDocument;
}


namespace fastbotx {

    /**
     * @brief XPath selector for matching UI elements
     * 
     * Represents an XPath-like selector that can match elements based on
     * class name, resource ID, text, content description, and index.
     * Supports both AND and OR operations for matching.
     */
    class Xpath {
    public:
        /**
         * @brief Default constructor creates empty XPath selector
         */
        Xpath();

        /**
         * @brief Constructor from XPath string
         * 
         * @param xpathString XPath string to parse
         */
        explicit Xpath(const std::string &xpathString);

        /// Class name to match
        std::string clazz;
        
        /// Resource ID to match
        std::string resourceID;
        
        /// Text content to match
        std::string text;
        
        /// Content description to match
        std::string contentDescription;
        
        /// Index to match (-1 means ignore index)
        int index;
        
        /// If true, use AND operation (all non-empty fields must match)
        /// If false, use OR operation (any non-empty field can match)
        bool operationAND;

        /**
         * @brief Get string representation of this XPath
         * 
         * @return Original XPath string
         */
        std::string toString() const { return _xpathStr; }

    private:
        /// Original XPath string
        std::string _xpathStr;
    };

    typedef std::shared_ptr<Xpath> XpathPtr;


    /**
     * @brief Element class representing a UI element in the XML hierarchy
     * 
     * Element represents a single UI element parsed from XML. It maintains
     * a tree structure with parent-child relationships and contains all
     * properties of the UI element (bounds, text, class name, etc.).
     * 
     * Features:
     * - XML parsing and serialization
     * - Tree structure with parent-child relationships
     * - XPath matching
     * - Hash computation for state comparison
     * - JSON/XML conversion
     */
    class Element : public Serializable {
    public:
        Element();

        bool matchXpathSelector(const XpathPtr &xpathSelector) const;

        void deleteElement();


        bool isWebView() const;

        bool isEditText() const;

        const std::vector<std::shared_ptr<Element> > &
        getChildren() const { return this->_children; }

        // recursive get elements depends func
        void recursiveElements(const std::function<bool(std::shared_ptr<Element>)> &func,
                               std::vector<std::shared_ptr<Element>> &result) const;

        void recursiveDoElements(const std::function<void(std::shared_ptr<Element>)> &doFunc);

        std::weak_ptr<Element> getParent() const { return this->_parent; }

        const std::string &getClassname() const { return this->_classname; }

        const std::string &getResourceID() const { return this->_resourceID; }

        const std::string &getText() const { return this->_text; }

        const std::string &getContentDesc() const { return this->_contentDesc; }

        const std::string &getPackageName() const { return this->_packageName; }

        RectPtr getBounds() const { return this->_bounds; };

        int getIndex() const { return this->_index; }

        bool getClickable() const { return this->_clickable; }

        bool getLongClickable() const { return this->_longClickable; }

        bool getCheckable() const { return this->_checkable; }

        bool getScrollable() const { return this->_scrollable; }

        bool getEnable() const { return this->_enabled; }

        bool getVisible() const { return this->_visible; }

        ScrollType getScrollType() const;

        // APE alignment: timestamp of GUI tree snapshot (used for transition sorting)
        int getTimestamp() const { return _timestamp; }
        void setTimestamp(int timestamp) { _timestamp = timestamp; }

        // reset properties, in Preference
        void reSetResourceID(const std::string &resourceID) { this->_resourceID = resourceID; }

        void reSetContentDesc(const std::string &content) { this->_contentDesc = content; }

        void reSetText(const std::string &text) { this->_text = text; }

        void reSetIndex(const int &index) { this->_index = index; }

        void reSetClassname(const std::string &className) { this->_classname = className; }

        void reSetClickable(bool clickable) { this->_clickable = clickable; }

        void reSetScrollable(bool scrollable) { this->_scrollable = scrollable; }

        void reSetEnabled(bool enable) { this->_enabled = enable; }

        void reSetCheckable(bool checkable) { this->_checkable = checkable; }

        void reSetLongClickable(bool longClickable) { this->_longClickable = longClickable; }

        void reSetVisible(bool visible) { this->_visible = visible; }

        void reSetBounds(RectPtr rect) { this->_bounds = std::move(rect); }

        void reSetParent(const std::shared_ptr<Element> &parent) { this->_parent = parent; }

        void reAddChild(const std::shared_ptr<Element> &child) {
            this->_children.emplace_back(child);
        }

        void adoptChildrenToParent();

        void clearChildren();

        int countDescendants() const;

        std::string toJson() const;

        std::string toXML() const;

        void fromJson(const std::string &jsonData);

        std::string toString() const override;

        static std::shared_ptr<Element> createFromXml(const std::string &xmlContent);

        static std::shared_ptr<Element> createFromXml(const tinyxml2::XMLDocument &doc);

        long hash(bool recursive = true);

        std::string validText;

        virtual ~Element();

    protected:
        void fromXMLNode(const tinyxml2::XMLElement *xmlNode,
                         const std::shared_ptr<Element> &parentOfNode);

        void fromXml(const tinyxml2::XMLDocument &nodeOfDoc,
                     const std::shared_ptr<Element> &parentOfNode);

        void recursiveToXML(tinyxml2::XMLElement *xml, const Element *elm) const;

        std::string _resourceID;
        std::string _classname;
        std::string _packageName;
        std::string _text;
        std::string _contentDesc;
        std::string _inputText;
        std::string _activity;

        bool _enabled;
        bool _checked;
        bool _checkable;
        bool _clickable;
        bool _focusable;
        bool _scrollable;
        bool _longClickable;
        bool _visible;
        int _childCount;
        bool _focused;
        int _index;
        bool _password;
        bool _selected;
        bool _isEditable;

        RectPtr _bounds;
        std::vector<std::shared_ptr<Element> > _children;
        std::weak_ptr<Element> _parent;

        // APE alignment: GUI tree snapshot timestamp (monotonic from Graph)
        int _timestamp{0};

        // a construct helper
        static bool _allClickableFalse;
    };

    typedef std::shared_ptr<Element> ElementPtr;


}

#endif //Element_H_
