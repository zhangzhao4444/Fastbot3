/*
 * This code is licensed under the Fastbot license. You may obtain a copy of this license in the LICENSE.txt file in the root directory of this source tree.
 */
/**
 * @authors Jianqiang Guo, Yuhui Su, Zhao Zhang
 */
#include <fstream>
#include <sstream>
#include <algorithm>
#include <mutex>
#include <cstring>
#include <cstdio>
#include "utils.hpp"
#include "Preference.h"
#include "../thirdpart/json/json.hpp"


namespace fastbotx {

    namespace {
        // Lightweight XPath-like parser helpers.
        // Input examples (from configs):
        //   "//*[@resource-id='xxx' and text='yyy' and index=3]"
        static bool extractQuotedValue(const std::string &s, const char *key, std::string &out) {
            std::string prefix;
            prefix.reserve(std::strlen(key) + 2);
            prefix.append(key);
            prefix.append("='");
            const auto start = s.find(prefix);
            if (start == std::string::npos) {
                return false;
            }
            const auto valueStart = start + prefix.size();
            const auto end = s.find('\'', valueStart);
            if (end == std::string::npos || end < valueStart) {
                return false;
            }
            out.assign(s, valueStart, end - valueStart);
            return true;
        }

        static bool extractIntValue(const std::string &s, const char *key, int &out) {
            std::string prefix;
            prefix.reserve(std::strlen(key) + 1);
            prefix.append(key);
            prefix.push_back('=');
            const auto start = s.find(prefix);
            if (start == std::string::npos) {
                return false;
            }
            std::size_t i = start + prefix.size();
            // parse consecutive digits
            std::size_t j = i;
            while (j < s.size() && s[j] >= '0' && s[j] <= '9') {
                ++j;
            }
            if (j == i) {
                return false;
            }
            out = std::stoi(s.substr(i, j - i));
            return true;
        }
    } // namespace


    CustomAction::CustomAction()
            : Action(), xpath(nullptr) {

    }

    CustomAction::CustomAction(ActionType act)
            : Action(act), xpath(nullptr) {

    }

    OperatePtr CustomAction::toOperate() const {
        OperatePtr opt = Action::toOperate();
        opt->sid = "customact";
        opt->aid = "customact";
        opt->editable = true;
        opt->setText(this->text);
        if (this->bounds.size() >= 4) {
            opt->pos = Rect(static_cast<int>(this->bounds[0]), static_cast<int>(this->bounds[1]),
                            static_cast<int>(this->bounds[2]), static_cast<int>(this->bounds[3]));
        }
        opt->clear = this->clearText;
        opt->throttle = static_cast<float>(this->throttle);
        opt->waitTime = this->waitTime;
        opt->adbInput = this->adbInput;
        opt->allowFuzzing = this->allowFuzzing;
        if (opt->act == ActionType::SHELL_EVENT) {
            opt->setText(this->command);
        }
        return opt;
    }

    Xpath::Xpath()
            : index(-1), operationAND(false) {}

    Xpath::Xpath(const std::string &xpathString)
            : Xpath() {
        if (xpathString.empty())
            return;
        this->_xpathStr = xpathString;
        // Performance: avoid std::regex (expensive on some Android runtimes).
        extractQuotedValue(xpathString, "resource-id", this->resourceID);
        extractQuotedValue(xpathString, "text", this->text);
        extractQuotedValue(xpathString, "content-desc", this->contentDescription);
        extractQuotedValue(xpathString, "class", this->clazz);
        extractIntValue(xpathString, "index", this->index);
        // Determine operation mode:
        // - If string contains "and" and has more than one '=' token, treat as AND selector.
        // Use a single-pass scan to avoid multiple full-string traversals.
        int eqCount = 0;
        bool hasAndToken = false;
        for (size_t i = 0; i < xpathString.size(); i++) {
            const char c = xpathString[i];
            if (c == '=') {
                eqCount++;
            }
            // Only treat "and" as a boolean operator token when it is delimited,
            // to avoid false positives like "android".
            if (!hasAndToken && c == 'a' && i + 2 < xpathString.size()
                && xpathString[i + 1] == 'n' && xpathString[i + 2] == 'd') {
                const bool leftDelim = (i == 0) || xpathString[i - 1] == ' ' || xpathString[i - 1] == '[';
                const bool rightDelim = (i + 3 >= xpathString.size()) || xpathString[i + 3] == ' ' || xpathString[i + 3] == ']';
                if (leftDelim && rightDelim) {
                    hasAndToken = true;
                }
            }
        }
        if (hasAndToken && eqCount > 1) {
            this->operationAND = true;
        }
        BDLOG(" xpath parsed: res id %s, text %s, index %d, content %s %d",
              this->resourceID.c_str(), this->text.c_str(), this->index,
              this->contentDescription.c_str(), this->operationAND);
    }


    Preference::Preference()
            : _randomInputText(false), _doInputFuzzing(true), _pruningValidTexts(false),
              _skipAllActionsFromModel(false), _rootScreenSize(nullptr) {
        loadConfigs();
    }

    PreferencePtr Preference::inst() {
        // Thread-safety: protect singleton initialization for concurrent JNI calls.
        static std::once_flag once;
        std::call_once(once, []() {
            _preferenceInst = std::make_shared<Preference>();
        });
        return _preferenceInst;
    }

    PreferencePtr Preference::_preferenceInst = nullptr;

    Preference::~Preference() {
        this->_resMixedMapping.clear();
        this->_resMapping.clear();
        this->_blackWidgetActions.clear();
        this->_treePrunings.clear();
        this->_inputTexts.clear();
        this->_blackList.clear();
        std::queue<ActionPtr> empty;
        this->_currentActions.swap(empty);
        this->_customEvents.clear();
        this->_validTexts.clear();
    }

    ActionPtr Preference::resolvePageAndGetSpecifiedAction(const std::string &activity,
                                                           const ElementPtr &rootXML) {
        if (nullptr != rootXML)
            this->resolvePage(activity, rootXML);

        // resolve action
        ActionPtr returnAction = nullptr;
        if (this->_currentActions.empty()) {
            for (const CustomEventPtr &customEvent: this->_customEvents) {
                float eventRate = randomInt(0, 10) / 10.0;
                BLOG("customEvent activities %s, page event is %s, event times %d , rate is %f/%f",
                     customEvent->activity.c_str(),
                     activity.c_str(), customEvent->times, eventRate, customEvent->prob);
                if (eventRate < customEvent->prob &&
                    customEvent->times > 0 &&
                    customEvent->activity == activity) {
                    if (!this->_currentActions.empty()) {
                        std::queue<ActionPtr> emptyActions;
                        this->_currentActions.swap(emptyActions);
                        BLOG("custom event clear happened when another event matched");
                    }
                    BLOG("custom event matched: %s actions size: %d", activity.c_str(),
                         (int) customEvent->actions.size());
                    for (const auto &matchedAction: customEvent->actions) {
                        this->_currentActions.push(matchedAction);
                    }
                    customEvent->times--;
                }
            }
        }
        if (!this->_currentActions.empty()) {
            BLOG("check custom action queue");
            auto frontAction = this->_currentActions.front();
            this->_currentActions.pop();
            if (frontAction->getActionType() >= ActionType::CLICK &&
                frontAction->getActionType() <= ActionType::SCROLL_RIGHT_LEFT) {
                // android action type
                auto customAction = std::dynamic_pointer_cast<CustomAction>(frontAction);
                if (rootXML && !this->patchActionBounds(customAction, rootXML)) {
                    return nullptr; // do nothing when action match failed
                }
                BLOG("custom action %s happened", customAction->xpath->toString().c_str());
                BLOG("custom action: %s happened", customAction->toString().c_str());
                return customAction;
            }
        }
        return returnAction;
    }

    /// Used for get the bounding boxes of the specified actions
    /// \param action The action specified by users, need to query its bounding box from XML tree
    /// \param rootXML The XML tree of the current page
    /// \return If the bonding box of the action is found, return true.
    bool Preference::patchActionBounds(const CustomActionPtr &action, const ElementPtr &rootXML) {
        if (nullptr == action)
            return false;
        std::vector<ElementPtr> elementVector; // vector for storing elements satisfying xpath selector
        this->findMatchedElements(elementVector, action->xpath, rootXML);
        if (!elementVector.empty()) {
            // the matched elements could be more than one, but we only use the first matched one
            RectPtr rect = elementVector.at(0)->getBounds();
            action->bounds.push_back(static_cast<float>(rect->left));
            action->bounds.push_back(static_cast<float>(rect->top));
            action->bounds.push_back(static_cast<float>(rect->right));
            action->bounds.push_back(static_cast<float>(rect->bottom));
        } else {
            // action->bounds.clear();
            BLOG("action xpath not found %s", action->xpath->toString().c_str());
            return false;
        }
        return true;
    }

    void Preference::patchOperate(const OperatePtr &opt) {
        if (!this->_doInputFuzzing)
            return;

        // input texts
        char prelog[30] = {0};
        // Use thread-local random number generator for better performance
        // (randomInt already uses thread_local RNG, so no need to initialize here)
        if (opt->editable && opt->getText().empty()
            && (opt->act == ActionType::CLICK || opt->act == ActionType::LONG_CLICK)) {
            if (this->_randomInputText &&
                this->_inputTexts.size() > 0) {
                const int inputSize = static_cast<int>(this->_inputTexts.size());
                int randIdx = randomInt(0, inputSize);
                std::string &txt = this->_inputTexts[randIdx];
                opt->setText(txt);
                std::snprintf(prelog, sizeof(prelog), "%s", "user preset strings");
            } else {
                float rate = randomInt(0, 100);
                if (!this->_fuzzingTexts.empty() && rate < 50) {
                    const int fuzzSize = static_cast<int>(this->_fuzzingTexts.size());
                    int randIdx = randomInt(0, fuzzSize);
                    std::string &txt = this->_fuzzingTexts[randIdx];
                    opt->setText(txt);
                    std::snprintf(prelog, sizeof(prelog), "%s", "fuzzing text");
                } else if (rate < 85) {
                    const int pageTextSize = static_cast<int>(this->_pageTextsCache.size());
                    if (pageTextSize > 0) {
                        int randIdx = randomInt(0, pageTextSize);
                        std::string &txt = this->_pageTextsCache[randIdx];
                        opt->setText(txt);
                        std::snprintf(prelog, sizeof(prelog), "%s", "page text");
                    }
                }
            }
            BLOG("patch %s input text: %s", prelog, opt->getText().c_str());
        }
    }

    /// Before exploring page, prune the UI tree of this page if possible
    /// \param activity
    /// \param rootXML
    void Preference::resolvePage(const std::string &activity, const ElementPtr &rootXML) {
        // cache page texts
        this->cachePageTexts(rootXML);

        BDLOG("preference resolve page: %s black widget %zu tree pruning %zu", activity.c_str(),
              this->_blackWidgetActions.size(), this->_treePrunings.size());
        // deMixResMapping
        this->deMixResMapping(rootXML);

        // get root size
        if (nullptr == this->_rootScreenSize
            || (this->_rootScreenSize->left + this->_rootScreenSize->top) != 0) {
            RectPtr rootSize = rootXML->getBounds();
            if (!rootSize || rootSize->isEmpty()) {
                auto &children = rootXML->getChildren();
                if (!children.empty())
                    rootSize = children[0]->getBounds();
            }
            this->_rootScreenSize = rootSize;
        }
        if (!this->_rootScreenSize || this->_rootScreenSize->isEmpty()) {
            BLOGE("%s", "No root size in current page");
        }
#if PATCH_GUI_TREE
        this->patchGUITree(rootXML);
#endif
#if ALWAYS_IGNORE_WEBVIEW_ACTION
#if !ALWAYS_IGNORE_WEBVIEW
        this->ignoreActionsInWebView(rootXML, rootXML ? rootXML->isWebView() : false);
#endif
#endif
        this->pruneInvalidNodes(rootXML);
        // recursively resolve black widgets
        this->resolveBlackWidgets(rootXML, activity);
        // recursively deal all rootXML tree
        this->resolveElement(rootXML, activity);

    }

    void Preference::resolveElement(const ElementPtr &element, const std::string &activity) {
        // resolve tree pruning
        if (element)
            this->resolveTreePruning(element, activity);
        // pruning Valid Texts
        if (this->_pruningValidTexts && element)
            this->pruningValidTexts(element);
        if (element) {
            for (const auto &child: element->getChildren()) {
                this->resolveElement(child, activity);
            }
        }
    }

    void Preference::pruneInvalidNodes(const ElementPtr &rootXML) {
        if (!rootXML) {
            return;
        }
        std::vector<ElementPtr> toDelete;
        std::vector<ElementPtr> toLift;
        RectPtr rootBounds = this->_rootScreenSize;
        if (!rootBounds || rootBounds->isEmpty()) {
            return;
        }
        rootXML->recursiveElements([&](const ElementPtr &elem) {
            if (!elem || elem->getParent().expired()) {
                return false;
            }
#if ALWAYS_IGNORE_WEBVIEW
            if (elem->isWebView()) {
                return true;
            }
#else
            if (IGNORE_WEBVIEW_THRESHOLD > 0 && elem->isWebView() &&
                elem->getChildren().size() > static_cast<size_t>(IGNORE_WEBVIEW_THRESHOLD)) {
                return true;
            }
#endif
#if IGNORE_INVISIBLE_NODE
            if (!elem->getVisible()) {
                return true;
            }
#endif
            RectPtr bounds = elem->getBounds();
            if (!bounds || bounds->isEmpty()) {
#if IGNORE_EMPTY_NODE
                return true;
#else
                return false;
#endif
            }
#if IGNORE_OUT_OF_BOUNDS_NODE
            Point center = bounds->center();
            if (!rootBounds->contains(center)) {
                return true;
            }
#endif
            if (elem->getChildren().empty() &&
                elem->getText().empty() &&
                elem->getContentDesc().empty() &&
                elem->getResourceID().empty()) {
#if IGNORE_EMPTY_NODE
                return true;
#endif
            }
#if EXCLUDE_EMPTY_CHILD
            if (!elem->getChildren().empty() &&
                elem->getText().empty() &&
                elem->getContentDesc().empty() &&
                elem->getResourceID().empty()) {
                if (elem->getChildren().size() == 1) {
                    auto child = elem->getChildren().front();
                    if (child && child->getBounds() && bounds && equals(child->getBounds(), bounds)) {
                        toLift.emplace_back(elem);
                    }
                }
            }
#endif
            return false;
        }, toDelete);
        for (const auto &elem : toDelete) {
            if (elem) {
                elem->deleteElement();
            }
        }
        for (const auto &elem : toLift) {
            if (elem) {
                elem->adoptChildrenToParent();
            }
        }
    }

    void Preference::patchGUITree(const ElementPtr &node) {
        if (!node || node->getChildren().empty()) {
            return;
        }
        if (node->isWebView()) {
            return;
        }
        RectPtr nodeBounds = node->getBounds();
        if (!nodeBounds || nodeBounds->isEmpty()) {
            return;
        }
        Rect childrenBounds;
        bool hasChild = false;
        for (const auto &child : node->getChildren()) {
            if (!child || !child->getBounds()) {
                continue;
            }
            RectPtr cb = child->getBounds();
            if (!hasChild) {
                childrenBounds = *cb;
                hasChild = true;
            } else {
                childrenBounds.left = std::min(childrenBounds.left, cb->left);
                childrenBounds.right = std::max(childrenBounds.right, cb->right);
                childrenBounds.top = std::min(childrenBounds.top, cb->top);
                childrenBounds.bottom = std::max(childrenBounds.bottom, cb->bottom);
            }
        }
        if (hasChild &&
            nodeBounds->left <= childrenBounds.left &&
            nodeBounds->right >= childrenBounds.right &&
            nodeBounds->top <= childrenBounds.top &&
            nodeBounds->bottom >= childrenBounds.bottom) {
            if (doPatchingChildren(node)) {
                auto children = node->getChildren();
                for (const auto &child : children) {
                    if (!child) {
                        continue;
                    }
                    if (!child->getClickable()) {
                        child->reSetClickable(true);
                        if (children.size() == 1) {
                            if (node->getIndex() != child->getIndex()) {
                                child->reSetIndex(node->getIndex());
                            }
                        }
                    }
                }
                Point center = nodeBounds->center();
                if (childrenBounds.left <= center.x &&
                    childrenBounds.right >= center.x &&
                    childrenBounds.top <= center.y &&
                    childrenBounds.bottom >= center.y) {
                    node->reSetClickable(false);
                }
            }
        }
        for (const auto &child : node->getChildren()) {
            patchGUITree(child);
        }
        if (!ALWAYS_IGNORE_WEBVIEW) {
            checkAndRemoveWebView(node);
        }
    }

    bool Preference::sameRow(const std::vector<ElementPtr> &children) const {
        int top = -1;
        int bottom = -1;
        for (const auto &child : children) {
            if (!child || !child->getBounds()) {
                continue;
            }
            RectPtr bounds = child->getBounds();
            if (top == -1) {
                top = bounds->top;
                bottom = bounds->bottom;
            } else if (top != bounds->top || bottom != bounds->bottom) {
                return false;
            }
        }
        return true;
    }

    bool Preference::sameColumn(const std::vector<ElementPtr> &children) const {
        int left = -1;
        int right = -1;
        for (const auto &child : children) {
            if (!child || !child->getBounds()) {
                continue;
            }
            RectPtr bounds = child->getBounds();
            if (left == -1) {
                left = bounds->left;
                right = bounds->right;
            } else if (left != bounds->left || right != bounds->right) {
                return false;
            }
        }
        return true;
    }

    bool Preference::doPatchingChildren(const ElementPtr &node) const {
        if (!node || !node->getClickable()) {
            return false;
        }
        if (node->getChildren().empty()) {
            return false;
        }
        if (node->getChildren().size() == 1) {
            return true;
        }
        if (sameRow(node->getChildren())) {
            return true;
        }
        if (sameColumn(node->getChildren())) {
            return true;
        }
        return false;
    }

    void Preference::ignoreActionsInWebView(const ElementPtr &node, bool ignore) {
        if (!node) {
            return;
        }
        if (ignore) {
            if (node->getClickable()) {
                node->reSetClickable(false);
            }
            if (node->getCheckable()) {
                node->reSetCheckable(false);
            }
            if (node->getLongClickable()) {
                node->reSetLongClickable(false);
            }
            if (node->getScrollable()) {
                node->reSetScrollable(false);
            }
        }
        if (node->isWebView()) {
            ignore = true;
        }
        if (node->getChildren().empty()) {
            return;
        }
        for (const auto &child : node->getChildren()) {
            ignoreActionsInWebView(child, ignore);
        }
    }

    void Preference::checkAndRemoveWebView(const ElementPtr &node) {
        if (!node || !node->isWebView()) {
            return;
        }
        int count = node->countDescendants();
        if (count > IGNORE_WEBVIEW_THRESHOLD) {
            node->clearChildren();
        }
    }

    void Preference::resolveBlackWidgets(const ElementPtr &rootXML, const std::string &activity) {
        // black widgets
        if (!this->_blackWidgetActions.empty()) {
            for (const CustomActionPtr &blackWidgetAction: this->_blackWidgetActions) {
                if (!activity.empty() && blackWidgetAction->activity != activity)
                    continue;
                XpathPtr xpath = blackWidgetAction->xpath;
                // read the bounds of black widget from the config
                std::vector<float> bounds = blackWidgetAction->bounds;
                bool hasBoundingBox = bounds.size() >= 4;
                if (nullptr == this->_rootScreenSize) {
                    BLOGE("black widget match failed %s", "No root node in current page");
                    return;
                }
                if (hasBoundingBox && bounds[1] <= 1.1 && bounds[3] <= 1.1) {
                    int rootWidth = this->_rootScreenSize->right;// - rootSize->left;
                    int rootHeight = this->_rootScreenSize->bottom;// - rootSize->top;
                    bounds[0] = bounds[0] * static_cast<float>(rootWidth);
                    bounds[1] = bounds[1] * static_cast<float>(rootHeight);
                    bounds[2] = bounds[2] * static_cast<float>(rootWidth);
                    bounds[3] = bounds[3] * static_cast<float>(rootHeight);
                }
                bool xpathExistsInPage;
                std::vector<ElementPtr> xpathElements;
                if (xpath) {
                    this->findMatchedElements(xpathElements, xpath, rootXML);
                    BDLOG("find black widget %s  %d", xpath->toString().c_str(),
                          (int) xpathElements.size());
                }
                xpathExistsInPage = xpath && !xpathElements.empty();
                std::vector<RectPtr> cachedRects;  // cache black widgets

                if (xpathExistsInPage && !hasBoundingBox) {
                    BLOG("black widget xpath %s, has no bounds matched %d nodes",
                         xpath->toString().c_str(), (int) xpathElements.size());
                    for (const auto &matchedElement: xpathElements) {
                        BLOG("black widget, delete node: %s depends xpath",
                             matchedElement->getResourceID().c_str());
                        cachedRects.push_back(matchedElement->getBounds());
                        matchedElement->deleteElement();
                    }
                }
                else if (xpathExistsInPage || (!xpath && hasBoundingBox)) {
                    RectPtr rejectRect = std::make_shared<Rect>(bounds[0], bounds[1], bounds[2],
                                                                bounds[3]);
                    cachedRects.push_back(rejectRect);
                    std::vector<ElementPtr> elementsInRejectRect;
                    rootXML->recursiveElements([&rejectRect](const ElementPtr &child) -> bool {
                        return rejectRect->contains(child->getBounds()->center());
                    }, elementsInRejectRect);
                    BLOG("black widget xpath %s, with bounds matched %d nodes",
                         xpath ? xpath->toString().c_str() : "none",
                         (int) elementsInRejectRect.size());
                    for (const auto &elementInRejectRect: elementsInRejectRect) {
                        if (elementInRejectRect) {
                            BLOG("black widget, delete node: %s depends xpath",
                                 elementInRejectRect->getResourceID().c_str());
                            elementInRejectRect->deleteElement();
                        }
                    }
                }
                this->_cachedBlackWidgetRects[activity] = cachedRects;
            }
        }
    }

    bool Preference::checkPointIsInBlackRects(const std::string &activity, int pointX, int pointY) {
        bool isInsideBlackList;
        auto iter = this->_cachedBlackWidgetRects.find(activity);
        isInsideBlackList = iter != this->_cachedBlackWidgetRects.end();
        if (isInsideBlackList) {
            const Point p(pointX, pointY);
            for (const auto &rect: iter->second) {
                if (rect->contains(p)) {
                    isInsideBlackList = true;
                    break;
                }
            }
        }
        BLOG("check point [%d, %d] is %s in black widgets", pointX, pointY,
             isInsideBlackList ? "" : "not");
        return isInsideBlackList;
    }

    void Preference::resolveTreePruning(const ElementPtr &elem, const std::string &activity) {
        if (!this->_treePrunings.empty()) {
            for (const auto &prun: this->_treePrunings) {
                if (prun->activity != activity)
                    continue;
                XpathPtr xpath = prun->xpath;
                std::vector<ElementPtr> xpathElemts;
                if (!xpath)
                    continue;
                if (elem->matchXpathSelector(xpath)) {
                    BLOG("pruning node %s for xpath: %s", elem->getResourceID().c_str(),
                         xpath->toString().c_str());
                    bool resetResid = 0 != InvalidProperty.compare(prun->resourceID);
                    bool resetContent = 0 != InvalidProperty.compare(prun->contentDescription);
                    bool resettext = 0 != InvalidProperty.compare(prun->text);
                    bool resetclassname = 0 != InvalidProperty.compare(prun->classname);

                    if (resetResid)
                        elem->reSetResourceID(prun->resourceID);
                    if (resetContent)
                        elem->reSetContentDesc(prun->contentDescription);
                    if (resettext)
                        elem->reSetText(prun->text);
                    if (resetclassname)
                        elem->reSetClassname(prun->classname);
                }
            }
        }
    }

    void Preference::pruningValidTexts(const ElementPtr &element) {
        if (!element || this->_validTexts.empty())
            return;
        bool valid;
        const std::string &originalTextOfElement = element->getText();
        valid = !originalTextOfElement.empty() &&
                this->_validTexts.find(originalTextOfElement) != this->_validTexts.end();
        if (valid) {
            element->validText = originalTextOfElement;
        } else {
            // if we could not find valid text from text, then try to find valid text from content description field.
            const std::string &contentDescription = element->getContentDesc();
            valid = !contentDescription.empty() &&
                    this->_validTexts.find(contentDescription) != this->_validTexts.end();
            if (valid) {
                element->validText = contentDescription;
            }
        }
        BDLOG("set valid Text: %s ", element->validText.c_str());
        // if we find valid text from text field or content description field,
        // and its parent is clickable, then set it as clickable.
        if (valid && !element->getParent().expired()
            && !element->getParent().lock()->getClickable()) {
            BDLOG("%s", "set valid Text  set clickable true");
            element->reSetClickable(true);
        }

        for (const auto &child: element->getChildren()) {
            pruningValidTexts(child);
        }
    }

/// According to the given xpath selector, match and return the satisfied elements inside UI page
/// \param outElements A vector storing matched elements
/// \param xpathSelector xpath selector describing the value of property that a matched element should have
/// \param elementXML Node from the XML tree of UI page
    void Preference::findMatchedElements(std::vector<ElementPtr> &outElements,
                                         const XpathPtr &xpathSelector,
                                         const ElementPtr &elementXML) {
        if (!elementXML) {
            BLOGE("%s", "xml node is null");
            return;
        }
        if (elementXML->matchXpathSelector(xpathSelector))
            outElements.push_back(elementXML);

        for (const auto &child: elementXML->getChildren()) {
            findMatchedElements(outElements, xpathSelector, child);
        }
    }

    void Preference::deMixResMapping(const ElementPtr &rootXML) {
        if (!rootXML || this->_resMixedMapping.empty())
            return;
        std::string stringOfResourceID = rootXML->getResourceID();
        if (!stringOfResourceID.empty()) {
            auto iterator = this->_resMixedMapping.find(stringOfResourceID);
            if (iterator != this->_resMixedMapping.end()) {
                rootXML->reSetResourceID((*iterator).second);
                BDLOG("de-mixed %s as %s", stringOfResourceID.c_str(), (*iterator).second.c_str());
            }
        }

        for (const auto &child: rootXML->getChildren()) {
            deMixResMapping(child);
        }
    }

    void Preference::loadMixResMapping(const std::string &resourceMappingPath) {
        BLOG("loading resource mapping : %s", resourceMappingPath.c_str());
        std::string content = loadFileContent(resourceMappingPath);
        if (content.empty())
            return;
        std::vector<std::string> lines;
        splitString(content, lines, '\n');
        for (std::string line: lines) {
            if (line.find(".R.id.") == std::string::npos)
                continue;
            size_t startPos = 0;
            if ((startPos = line.find("0x") != std::string::npos)
                && ((startPos = line.find(':')) != std::string::npos)) {
                line = line.substr(startPos + 1);
            }
            startPos = 0;
            stringReplaceAll(line, " ", "");
            stringReplaceAll(line, ".R.id.", ":id/");
            startPos = line.find("->");
            if (startPos == std::string::npos) {
                continue;
            }
            std::string resId = line.substr(0, startPos);
            std::string mixedResid = line.substr(startPos + 2);
            BDLOG("res id %s mixed to %s", resId.c_str(), mixedResid.c_str());
            this->_resMapping[resId] = mixedResid;
            this->_resMixedMapping[mixedResid] = resId;
        }
    }

    void Preference::loadValidTexts(const std::string &pathOfValidTexts) {
        std::string fileContent = loadFileContent(pathOfValidTexts);
        if (fileContent.empty())
            return;
        this->_validTexts.clear();
        std::vector<std::string> validStringLines;
        splitString(fileContent, validStringLines, '\n');
        for (auto &line: validStringLines) {
            auto iter = line.find(": ");
            if (iter != std::string::npos &&
                line.find("String #") != std::string::npos) {
                this->_validTexts.emplace(line.substr(iter + 2));
            } else {
                this->_validTexts.emplace(line);
            }
        }
        if (!this->_validTexts.empty())
            this->_pruningValidTexts = true;
    }

    void Preference::loadConfigs() {
#if defined(__ANDROID__) || defined(_DEBUG_)
        try {
            loadMixResMapping(DefaultResMappingFilePath); // loading mapping
            loadValidTexts(ValidTextFilePath);
            loadBaseConfig();
            loadBlackWidgets();
            loadActions();
            loadWhiteBlackList();
            loadTreePruning();
            loadInputTexts();
        }
        catch (std::exception &ex) {
            BLOGE("load configs Error! %s", ex.what());
        }
#endif
    }

#define MaxRandomPickSTR  "max.randomPickFromStringList"
#define InputFuzzSTR "max.doinputtextFuzzing"
#define ListenMode "max.listenMode"

    void Preference::loadBaseConfig() {
        LOGI("pref init checking curr packageName is offset: %s", Preference::PackageName.c_str());
        std::string configContent = loadFileContent(BaseConfigFilePath);
        BLOG("max.config:\n %s", configContent.c_str());
        std::vector<std::string> lines;
        splitString(configContent, lines, '\n');
        for (const std::string &line: lines) {
            std::vector<std::string> key_value;
            splitString(line, key_value, '=');
            if (key_value.size() < 2)
                continue;
            trimString(key_value[0]);
            trimString(key_value[1]);
            BDLOG("base config key:-%s- value:-%s-", key_value[0].c_str(), key_value[1].c_str());
            if (MaxRandomPickSTR == key_value[0]) {
                BDLOG("set %s", MaxRandomPickSTR);
                this->_randomInputText = ("true" == key_value[1]);
            } else if (InputFuzzSTR == key_value[0]) {
                BDLOG("set %s", InputFuzzSTR);
                this->_doInputFuzzing = ("true" == key_value[1]);
            } else if (ListenMode == key_value[0]) {
                BDLOG("set %s", ListenMode);
                this->setListenMode("true" == key_value[1]);
            }
        }
    }

#define PageTextsMaxCount 300

    void Preference::cachePageTexts(const ElementPtr &rootElement) {
        if (this->_pageTextsCache.size() > PageTextsMaxCount) {
            this->_pageTextsCache.erase(this->_pageTextsCache.begin(),
                                        this->_pageTextsCache.begin() + 20);
        }
        if (rootElement && !rootElement->getText().empty()) {
            this->_pageTextsCache.push_back(rootElement->getText());
        }
        for (const auto &childElement: rootElement->getChildren()) {
            this->cachePageTexts(childElement);
        }
    }


    void Preference::setListenMode(bool listen) {
        BDLOG("set %s", ListenMode);
        this->_skipAllActionsFromModel = listen;
        LOGI("fastbot native use a listen mode: %d !!!", this->_skipAllActionsFromModel);
    }

    void Preference::loadActions() {
        std::string fileContent = loadFileContent(ActionConfigFilePath);
        if (fileContent.empty())
            return;
        BLOG("loading actions  : %s", ActionConfigFilePath.c_str());
        try {
            ::nlohmann::json actionEvents = ::nlohmann::json::parse(fileContent);
            for (const ::nlohmann::json &actionEvent: actionEvents) {
                CustomEventPtr customEvent = std::make_shared<CustomEvent>();
                customEvent->prob = static_cast<float>(getJsonValue<float>(actionEvent, "prob", 1));
                customEvent->times = getJsonValue<int>(actionEvent, "times", 1);
                customEvent->activity = getJsonValue<std::string>(actionEvent, "activity", "");
                BLOG("loading event %s", customEvent->activity.c_str());
                ::nlohmann::json actions = getJsonValue<::nlohmann::json>(actionEvent, "actions",
                                                                          ::nlohmann::json());
                for (const ::nlohmann::json &action: actions) {
                    std::string actionTypeString = getJsonValue<std::string>(action, "action", "");
                    auto customAction = std::make_shared<CustomAction>(
                            stringToActionType(actionTypeString));
                    std::string xPathString = getJsonValue<std::string>(action, "xpath", "");
                    BLOG("loading action %s", xPathString.c_str());
                    customAction->xpath = std::make_shared<Xpath>(xPathString);
                    customAction->text = getJsonValue<std::string>(action, "text", "");
                    customAction->clearText = getJsonValue<bool>(action, "clearText", false);
                    customAction->throttle = getJsonValue<int>(action, "throttle", 1000);
                    customAction->waitTime = getJsonValue<int>(action, "wait", 0);
                    customAction->adbInput = getJsonValue<bool>(action, "useAdbInput", false);
                    customAction->allowFuzzing = false;
                    if (customAction->getActionType() == ActionType::SHELL_EVENT) {
                        customAction->command = getJsonValue<std::string>(actions, "command", "");
                    }
                    BLOG("loading action %s", xPathString.c_str());
                    customEvent->actions.push_back(customAction);
                }
                this->_customEvents.push_back(customEvent);
            }
        } catch (nlohmann::json::exception &ex) {
            BLOGE("parse actions error happened: id,%d: %s", ex.id, ex.what());
        }
    }

    void Preference::loadBlackWidgets() {
        std::string fileContent = fastbotx::Preference::loadFileContent(BlackWidgetFilePath);
        if (fileContent.empty())
            return;
        try {
            BLOG("loading black widgets  : %s", BlackWidgetFilePath.c_str());
            ::nlohmann::json actions = ::nlohmann::json::parse(fileContent);
            for (const ::nlohmann::json &action: actions) {
                CustomActionPtr act = std::make_shared<CustomAction>();
                std::string xpathstr = getJsonValue<std::string>(action, "xpath", "");
                if (!xpathstr.empty())
                    act->xpath = std::make_shared<Xpath>(xpathstr);
                BLOG("loading black widget %s", xpathstr.c_str());
                act->activity = getJsonValue<std::string>(action, "activity", "");
                this->_blackWidgetActions.push_back(act);
                std::string boundsstr = getJsonValue<std::string>(action, "bounds", "");
                if (!boundsstr.empty()) {
                    act->bounds.resize(4);
                    sscanf(boundsstr.c_str(), "[%f,%f][%f,%f]", &act->bounds[0], &act->bounds[1],
                           &act->bounds[2], &act->bounds[3]);
                    sscanf(boundsstr.c_str(), "%f,%f,%f,%f", &act->bounds[0], &act->bounds[1],
                           &act->bounds[2], &act->bounds[3]);
                } else {
                    act->bounds.clear();
                }
            }
        } catch (nlohmann::json::exception &ex) {
            BLOGE("parse black widgets error happend: id,%d: %s", ex.id, ex.what());
        }
    }

    void Preference::loadWhiteBlackList() {
        std::string contentBlack = fastbotx::Preference::loadFileContent(BlackListFilePath);
        if (contentBlack.empty())
            return;
        std::vector<std::string> texts;
        splitString(contentBlack, texts, '\n');
        this->_blackList.swap(texts);
        BLOG("blacklist :\n %s", contentBlack.c_str());
        std::string contentWhite = fastbotx::Preference::loadFileContent(WhiteListFilePath);
        std::vector<std::string> textsw;
        splitString(contentWhite, textsw, '\n');
        this->_whiteList.swap(textsw);
        BLOG("whitelist :\n %s", contentWhite.c_str());
    }

///Load texts for input from specified file of designed text or file of fuzzing text
    void Preference::loadInputTexts() {
        // load specified designed text by tester
        std::string content = fastbotx::Preference::loadFileContent(InputTextConfigFilePath);
        if (!content.empty()) {
            std::vector<std::string> texts;
            splitString(content, texts, '\n');
            this->_inputTexts.assign(texts.begin(), texts.end());
        }
        // load fuzzing texts
        std::string fuzzContent = fastbotx::Preference::loadFileContent(FuzzingTextsFilePath);
        if (!fuzzContent.empty()) {
            std::vector<std::string> fuzzTexts;
            splitString(fuzzContent, fuzzTexts, '\n');
            for (auto &line: fuzzTexts) {
                if (line.empty() || line[0] ==
                                    '#') // if a new line starts with #, means it is a comment. Overlook this line.
                    continue;
                this->_fuzzingTexts.push_back(line);
            }
        }
    }

    void Preference::loadTreePruning() {
        std::string fileContent = fastbotx::Preference::loadFileContent(TreePruningFilePath);
        if (fileContent.empty())
            return;
        try {
            ::nlohmann::json actions = ::nlohmann::json::parse(fileContent);
            for (const ::nlohmann::json &action: actions) {
                CustomActionPtr act = std::make_shared<CustomAction>();
                std::string xpathStr = getJsonValue<std::string>(action, "xpath", "");
                act->xpath = std::make_shared<Xpath>(xpathStr);
                act->activity = getJsonValue<std::string>(action, "activity", "");
                act->resourceID = getJsonValue<std::string>(action, "resourceid", InvalidProperty);
                act->text = getJsonValue<std::string>(action, "text", InvalidProperty);
                act->contentDescription = getJsonValue<std::string>(action, "contentdesc",
                                                                    InvalidProperty);
                act->classname = getJsonValue<std::string>(action, "classname", InvalidProperty);
                this->_treePrunings.push_back(act);
            }
        } catch (nlohmann::json::exception &ex) {
            BLOGE("parse tree pruning error happened: id,%d: %s", ex.id, ex.what());
        }
    }


    std::string Preference::loadFileContent(const std::string &fileAbsolutePath) {
        std::string retStr;
        std::ifstream fileStringReader(fileAbsolutePath);
        if (fileStringReader.good()) {
            retStr = std::string((std::istreambuf_iterator<char>(fileStringReader)),
                                 std::istreambuf_iterator<char>());
        } else {
            LOGW("load file %s not exists!!!", fileAbsolutePath.c_str());
        }
        return retStr;
    }

    std::string Preference::InvalidProperty = "-f0s^%a@d";
    // static configs for android
    std::string Preference::DefaultResMappingFilePath = "/sdcard/max.mapping";
    std::string Preference::BaseConfigFilePath = "/sdcard/max.config";
    std::string Preference::InputTextConfigFilePath = "/sdcard/max.strings";
    std::string Preference::ActionConfigFilePath = "/sdcard/max.xpath.actions";
    std::string Preference::WhiteListFilePath = "/sdcard/awl.strings";
    std::string Preference::BlackListFilePath = "/sdcard/abl.strings";
    std::string Preference::BlackWidgetFilePath = "/sdcard/max.widget.black";
    std::string Preference::TreePruningFilePath = "/sdcard/max.tree.pruning";
    std::string Preference::ValidTextFilePath = "/sdcard/max.valid.strings";
    std::string Preference::FuzzingTextsFilePath = "/sdcard/max.fuzzing.strings";
    std::string Preference::PackageName;

} // namespace fastbotx
