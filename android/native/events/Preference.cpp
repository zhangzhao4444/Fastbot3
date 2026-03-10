/*
 * This code is licensed under the Fastbot license. You may obtain a copy of this license in the LICENSE.txt file in the root directory of this source tree.
 */
/**
 * @authors Jianqiang Guo, Yuhui Su, Zhao Zhang
 */
#include <fstream>
#include <sstream>
#include <algorithm>
#include <regex>
#include <mutex>
#include <cstring>
#include <cstdlib>
#include <climits>
#include "utils.hpp"
#include "Preference.h"
#include "../thirdpart/json/json.hpp"

// Performance optimization: Maximum number of page texts to cache
#define PageTextsMaxCount 300

namespace fastbotx {

    static void substituteEnvVars(std::string &s);

    /**
     * @brief Default constructor for CustomAction
     * 
     * Initializes a CustomAction with default values. XPath is set to nullptr.
     */
    CustomAction::CustomAction()
            : Action(), xpath(nullptr) {

    }

    /**
     * @brief Constructor for CustomAction with specified action type
     * 
     * @param act The action type for this custom action
     */
    CustomAction::CustomAction(ActionType act)
            : Action(act), xpath(nullptr) {

    }

    /**
     * @brief Convert CustomAction to Operate object
     * 
     * Converts this CustomAction to an Operate object that can be executed.
     * Sets all relevant fields including bounds, text, throttle, wait time, etc.
     * 
     * @return OperatePtr - Shared pointer to the created Operate object
     * 
     * @note Performance optimizations:
     *       - Avoids unnecessary setText() calls when text/command is empty
     *       - Checks action type first to determine which text to set
     *       - Caches bounds size check to avoid repeated vector operations
     */
    OperatePtr CustomAction::toOperate() const {
        OperatePtr opt = Action::toOperate();
        opt->sid = "customact";
        opt->aid = "customact";
        opt->editable = true;
        
        // Performance: Check action type first to avoid unnecessary setText calls
        // For SHELL_EVENT, use command; otherwise use text
        if (opt->act == ActionType::SHELL_EVENT) {
            // Only call setText if command is not empty
            if (!this->command.empty()) {
                opt->setText(this->command);
            }
        } else {
            // Only call setText if text is not empty
            if (!this->text.empty()) {
                opt->setText(this->text);
            }
        }
        
        // Performance: Cache bounds size check
        const size_t boundsSize = this->bounds.size();
        if (boundsSize >= 4) {
            opt->pos = Rect(static_cast<int>(this->bounds[0]), static_cast<int>(this->bounds[1]),
                            static_cast<int>(this->bounds[2]), static_cast<int>(this->bounds[3]));
        }
        
        opt->clear = this->clearText;
        opt->throttle = static_cast<float>(this->throttle);
        opt->waitTime = this->waitTime;
        opt->allowFuzzing = this->allowFuzzing;
        
        return opt;
    }

    /**
     * @brief Default constructor for Xpath
     * 
     * Initializes XPath selector with default values:
     * - index = -1 (ignore index)
     * - operationAND = false (OR mode)
     */
    Xpath::Xpath()
            : index(-1), operationAND(false) {}

    namespace {
        const std::regex &getResourceIDRegex() {
            static const std::regex r("resource-id='(.*?)'");
            return r;
        }
        const std::regex &getTextRegex() {
            static const std::regex r("text='(.*?)'");
            return r;
        }
        const std::regex &getIndexRegex() {
            static const std::regex r("index=(\\d+)");
            return r;
        }
        const std::regex &getContentRegex() {
            static const std::regex r("content-desc='(.*?)'");
            return r;
        }
        const std::regex &getClazzRegex() {
            static const std::regex r("class='(.*?)'");
            return r;
        }
    }

    /**
     * @brief Constructor for Xpath from string
     * 
     * Parses an XPath string and extracts matching criteria:
     * - resource-id: from pattern `resource-id='...'`
     * - text: from pattern `text='...'`
     * - content-desc: from pattern `content-desc='...'`
     * - class: from pattern `class='...'`
     * - index: from pattern `index=123`
     * - operationAND: true if string contains " and " (with spaces) and has multiple '=' signs
     * 
     * @param xpathString The XPath string to parse
     * 
     * @example
     * Xpath("resource-id='com.example:id/button' and text='Click Me'")
     * Xpath("class='android.widget.TextView'")
     * 
     * @note Performance optimizations:
     *       - Uses manual parsing instead of multiple regex searches (single pass)
     *       - More precise " and " detection (with spaces) to avoid false matches
     *       - Counts '=' signs during parsing to avoid separate traversal
     */
    Xpath::Xpath(const std::string &xpathString)
            : Xpath() {
        if (xpathString.empty())
            return;
        this->_xpathStr = xpathString;
        
        // Performance optimization: Manual parsing instead of multiple regex searches
        // This avoids 5 separate regex searches and reduces string operations
        const size_t len = xpathString.length();
        int equalsCount = 0;
        bool hasAndKeyword = false;
        
        // Helper lambda to extract quoted value: key='value'
        auto extractQuotedValue = [&xpathString](const std::string& key, std::string& out) -> bool {
            size_t keyPos = xpathString.find(key);
            if (keyPos == std::string::npos) return false;
            
            size_t quoteStart = keyPos + key.length();
            if (quoteStart >= xpathString.length() || xpathString[quoteStart] != '\'') return false;
            
            quoteStart++; // Skip opening quote
            size_t quoteEnd = xpathString.find('\'', quoteStart);
            if (quoteEnd == std::string::npos) return false;
            
            out = xpathString.substr(quoteStart, quoteEnd - quoteStart);
            return true;
        };
        
        // Extract resource-id='...'
        extractQuotedValue("resource-id='", this->resourceID);
        
        // Helper: extract value from contains(@attr,'val') or contains(@attr,"val") (handles optional space and Unicode quotes)
        auto extractContainsValue = [&xpathString](const char* prefix, size_t prefixLen, std::string& out) -> bool {
            size_t pos = xpathString.find(prefix);
            if (pos == std::string::npos) return false;
            size_t i = pos + prefixLen;
            while (i < xpathString.length() && (xpathString[i] == ' ' || xpathString[i] == ',')) ++i;
            if (i >= xpathString.length()) return false;
            char openQ = xpathString[i];
            if (openQ != '\'' && openQ != '"') {
                // Unicode single quotes (UTF-8: E2 80 98 / E2 80 99)
                if (i + 2 < xpathString.length() && (unsigned char)xpathString[i] == 0xE2
                    && (unsigned char)xpathString[i+1] == 0x80 && ((unsigned char)xpathString[i+2] == 0x98 || (unsigned char)xpathString[i+2] == 0x99)) {
                    openQ = '\''; // treat as single quote for matching
                    i += 3;
                } else
                    return false;
            } else {
                ++i;
            }
            size_t start = i;
            if (openQ == '\'') {
                while (i < xpathString.length() && xpathString[i] != '\'') {
                    if ((unsigned char)xpathString[i] == 0xE2 && i + 2 < xpathString.length()
                        && (unsigned char)xpathString[i+1] == 0x80 && (unsigned char)xpathString[i+2] == 0x99)
                        break; // Unicode right single quote
                    ++i;
                }
            } else {
                i = xpathString.find('"', i);
            }
            if (i > start) {
                out = xpathString.substr(start, i - start);
                return true;
            }
            return false;
        };
        
        // Helper: extract value between first two single quotes after a prefix (robust for any UTF-8 content)
        auto extractBetweenTwoSingleQuotes = [&xpathString](const char* prefix, std::string& out) -> bool {
            size_t pos = xpathString.find(prefix);
            if (pos == std::string::npos) return false;
            size_t after = pos + strlen(prefix);
            size_t q1 = xpathString.find('\'', after);
            if (q1 == std::string::npos) return false;
            size_t q2 = xpathString.find('\'', q1 + 1);
            if (q2 == std::string::npos || q2 <= q1 + 1) return false;
            out = xpathString.substr(q1 + 1, q2 - q1 - 1);
            return true;
        };
        
        // Extract text='...' (exact) or contains(@text,'...') / contains(@text,"...")
        extractQuotedValue("text='", this->text);
        if (this->text.empty()) {
            if (!extractContainsValue("contains(@text", 13, this->text)
                && !extractBetweenTwoSingleQuotes("contains(@text", this->text)) {
                size_t pos = xpathString.find("contains(@text,'");
                if (pos != std::string::npos) {
                    size_t start = pos + 15;
                    size_t end = xpathString.find('\'', start);
                    if (end != std::string::npos) this->text = xpathString.substr(start, end - start);
                }
            }
            if (this->text.empty()) {
                size_t pos = xpathString.find("contains(@text,\"");
                if (pos != std::string::npos) {
                    size_t start = pos + 16;
                    size_t end = xpathString.find('"', start);
                    if (end != std::string::npos) this->text = xpathString.substr(start, end - start);
                }
            }
        }
        
        // Extract content-desc='...' (exact) or contains(@content-desc,'...') / contains(@content-desc,"...")
        extractQuotedValue("content-desc='", this->contentDescription);
        if (this->contentDescription.empty()) {
            if (!extractContainsValue("contains(@content-desc", 22, this->contentDescription)
                && !extractBetweenTwoSingleQuotes("contains(@content-desc", this->contentDescription)) {
                size_t pos = xpathString.find("contains(@content-desc,'");
                if (pos != std::string::npos) {
                    size_t start = pos + 23;
                    size_t end = xpathString.find('\'', start);
                    if (end != std::string::npos) this->contentDescription = xpathString.substr(start, end - start);
                }
            }
            if (this->contentDescription.empty()) {
                size_t pos = xpathString.find("contains(@content-desc,\"");
                if (pos != std::string::npos) {
                    size_t start = pos + 24;
                    size_t end = xpathString.find('"', start);
                    if (end != std::string::npos) this->contentDescription = xpathString.substr(start, end - start);
                }
            }
        }
        
        // Extract class='...'
        extractQuotedValue("class='", this->clazz);
        
        // Extract index=123 (no quotes)
        size_t indexPos = xpathString.find("index=");
        if (indexPos != std::string::npos) {
            size_t numStart = indexPos + 6; // Skip "index="
            if (numStart < len && xpathString[numStart] >= '0' && xpathString[numStart] <= '9') {
                try {
                    size_t numEnd = numStart;
                    while (numEnd < len && xpathString[numEnd] >= '0' && xpathString[numEnd] <= '9') {
                        numEnd++;
                    }
                    long parsedIndex = std::stol(xpathString.substr(numStart, numEnd - numStart));
                    if (parsedIndex >= 0 && parsedIndex <= INT_MAX) {
                        this->index = static_cast<int>(parsedIndex);
                    }
                } catch (...) {
                    // Ignore parsing errors, keep default index = -1
                }
            }
        }
        
        // Performance: Count '=' and detect " and " in single pass
        for (size_t i = 0; i < len; ++i) {
            if (xpathString[i] == '=') {
                equalsCount++;
            }
            // Check for " and " (with spaces) to avoid false matches like "android"
            if (i + 4 < len && 
                xpathString[i] == ' ' && 
                xpathString[i+1] == 'a' && xpathString[i+2] == 'n' && xpathString[i+3] == 'd' && 
                xpathString[i+4] == ' ') {
                hasAndKeyword = true;
            }
        }
        
        // Set operationAND if both conditions are met
        if (hasAndKeyword && equalsCount > 1) {
            this->operationAND = true;
        }
        
        // Human-friendly xpath parse log for debug
        BDLOG("    xpath parsed: resource-id=\"%s\", text=\"%s\", content-desc=\"%s\", index=%d, useAND=%s",
              this->resourceID.c_str(),
              this->text.c_str(),
              this->contentDescription.c_str(),
              this->index,
              this->operationAND ? "true" : "false");
        if (this->text.empty() && this->contentDescription.empty()
            && xpathString.find("contains(") != std::string::npos) {
            size_t maxLen = (xpathString.length() < 120) ? xpathString.length() : 120;
            BDLOG(" xpath contains() but parsed text/content empty; first %zu chars: %s",
                  maxLen, xpathString.substr(0, maxLen).c_str());
        }
    }


    /**
     * @brief Constructor for Preference
     * 
     * Initializes Preference with default values and loads all configuration files.
     * 
     * Default values:
     * - _randomInputText: false
     * - _doInputFuzzing: true
     * - _pruningValidTexts: false
     * - _skipAllActionsFromModel: false
     * - _rootScreenSize: nullptr
     * 
     * Automatically calls loadConfigs() to load all configuration files.
     */
    Preference::Preference()
            : _randomInputText(false), _doInputFuzzing(true), _pruningValidTexts(false),
              _skipAllActionsFromModel(false), _useStaticReuseAbstraction(false),
              _rootScreenSize(nullptr) {
        loadConfigs();
    }

    /**
     * @brief Get the singleton instance of Preference (thread-safe)
     * 
     * Uses std::call_once to ensure thread-safe initialization.
     * The singleton is created on first call and reused for subsequent calls.
     * 
     * @return PreferencePtr - Shared pointer to the singleton Preference instance
     */
    PreferencePtr Preference::inst() {
        static std::once_flag once;
        std::call_once(once, []() { _preferenceInst = std::make_shared<Preference>(); });
        return _preferenceInst;
    }

    PreferencePtr Preference::_preferenceInst = nullptr;

    /**
     * @brief Destructor for Preference
     * 
     * Cleans up all configuration data and cached information.
     */
    Preference::~Preference() {
        this->_resMixedMapping.clear();
        this->_resMapping.clear();
        this->_avoidRules.clear();
        this->_avoidRulesByActivity.clear();
        this->_inputTexts.clear();
        this->_blackList.clear();
        this->_validTexts.clear();
    }

    /**
     * @brief Patch operate object with input text fuzzing
     * 
     * For editable widgets with empty text, fills in text based on configuration:
     * 1. If _randomInputText is true: use user preset strings (_inputTexts)
     * 2. Otherwise: 50% probability use fuzzing texts (_fuzzingTexts)
     * 3. Otherwise: 35% probability use page texts (_pageTextsCache)
     * 
     * Only applies to CLICK and LONG_CLICK actions on editable widgets.
     * 
     * @param opt The operate object to patch
     * 
     * @note This function is called before executing an action to provide input text
     *       for text input fields.
     * 
     * @note Performance optimizations:
     *       - Early exit for non-editable or non-empty text cases
     *       - Caches text and action type to avoid repeated calls
     *       - Caches vector sizes to avoid repeated size() calls
     *       - Uses string literals instead of strcpy for better performance
     *       - Only generates random numbers when needed
     */
    void Preference::patchOperate(const OperatePtr &opt) {
        // Substitute ${VAR} in action input text from env (e.g. LLM returns "${LLM_LOGIN_ACCOUNT}";
        // replace here so secrets are not sent to the LLM, only when executing the action).
        std::string text = opt->getText();
        if (!text.empty()) {
            substituteEnvVars(text);
            opt->setText(text);
        }

        // Performance: Early exit if input fuzzing is disabled
        if (!this->_doInputFuzzing) {
            return;
        }

        // Performance: Early exit - check action type first (fastest check)
        ActionType act = opt->act;
        if (act != ActionType::CLICK && act != ActionType::LONG_CLICK) {
            return;
        }

        // Performance: Early exit - check editable flag
        if (!opt->editable) {
            return;
        }

        // Performance: Cache getText() result to avoid repeated calls
        const std::string &currentText = opt->getText();
        if (!currentText.empty()) {
            return; // Text already set, no need to patch
        }

        // Performance: Use const char* instead of strcpy for string literals
        const char* prelog = nullptr;
        bool textSet = false;

        // Priority 1: Use preset strings if enabled and available
        if (this->_randomInputText && !this->_inputTexts.empty()) {
            // Performance: Cache size to avoid repeated size() calls
            const int n = static_cast<int>(this->_inputTexts.size());
            if (n > 0) {
                int randIdx = randomInt(0, n);
                opt->setText(this->_inputTexts[randIdx]);
                prelog = "user preset strings";
                textSet = true;
            }
        } else {
            // Priority 2 & 3: Use fuzzing texts or page texts based on probability
            // Performance: Only generate random number when needed
            int rate = randomInt(0, 100);
            
            // 50% probability: Use fuzzing texts
            if (rate < 50 && !this->_fuzzingTexts.empty()) {
                const int n = static_cast<int>(this->_fuzzingTexts.size());
                if (n > 0) {
                    int randIdx = randomInt(0, n);
                    opt->setText(this->_fuzzingTexts[randIdx]);
                    prelog = "fuzzing text";
                    textSet = true;
                }
            }
            // 35% probability (rate 50-84): Use page texts cache
            else if (rate < 85 && !this->_pageTextsCache.empty()) {
                const int n = static_cast<int>(this->_pageTextsCache.size());
                if (n > 0) {
                    int randIdx = randomInt(0, n);
                    opt->setText(this->_pageTextsCache[randIdx]);
                    prelog = "page text";
                    textSet = true;
                }
            }
        }

        // Performance: Only log when text was actually set
        if (textSet && prelog != nullptr) {
            BLOG("patch %s input text: %s", prelog, opt->getText().c_str());
        }
    }

    /**
     * @brief Resolve page: preprocess UI tree before exploration
     * 
     * Main entry point for page preprocessing. Performs the following operations:
     * 1. Gets and caches root screen size
     * 2. Resolves black widgets (deletes blacklisted controls/regions)
     * 3. Resolves element tree (resource mapping, tree pruning, valid texts)
     * 
     * This function is called once per page before the UI tree is used for exploration.
     * 
     * @param activity Current activity name
     * @param rootXML Root Element of the UI tree
     * 
     * @note Performance: Tree traversal reduced from 3 passes to 2 passes
     *       (resolveBlackWidgets + resolveElement which includes deMixResMapping)
     * 
     * @note Performance optimizations:
     *       - Early validation of rootXML
     *       - Simplified root size caching logic
     *       - Reduced redundant bounds checks
     *       - Cached children reference
     */
    void Preference::resolvePage(const std::string &activity, const ElementPtr &rootXML) {
        // Performance: Early validation
        if (nullptr == rootXML) {
            return;
        }

        BDLOG("preference resolve page: %s avoid rules %zu", activity.c_str(), this->_avoidRules.size());

        // Performance: Get and cache root size only if not already cached or if cached size is invalid
        if (nullptr == this->_rootScreenSize || this->_rootScreenSize->isEmpty()) {
            RectPtr rootSize = rootXML->getBounds();
            if (!rootSize || rootSize->isEmpty()) {
                const auto &children = rootXML->getChildren();
                if (!children.empty()) {
                    rootSize = children[0]->getBounds();
                }
            }
            this->_rootScreenSize = rootSize;
            if (!this->_rootScreenSize || this->_rootScreenSize->isEmpty()) {
                BLOGE("%s", "No root size in current page");
            }
        }

        // Clear cache for this activity and collect bounds-only avoid rects (single-pass unified avoidance)
        this->_cachedBlackWidgetRects[activity].clear();
        this->_currentBoundsOnlyAvoidRects.clear();
        int rootW = this->_rootScreenSize ? this->_rootScreenSize->right : 0;
        int rootH = this->_rootScreenSize ? this->_rootScreenSize->bottom : 0;
        auto addRulesForActivity = [this, rootW, rootH, activity](const std::string &act) {
            auto it = this->_avoidRulesByActivity.find(act);
            if (it == this->_avoidRulesByActivity.end()) return;
            for (const AvoidRulePtr &rule : it->second) {
                if (rule->action != AvoidRule::Action::Avoid || rule->bounds.size() != 4) continue;
                if (rule->xpath) continue; // xpath-based avoid: rects come from deleted elements
                std::vector<float> b = rule->bounds;
                bool rel = (b[0] >= 0.0f && b[0] <= 1.1f && b[1] >= 0.0f && b[1] <= 1.1f &&
                            b[2] >= 0.0f && b[2] <= 1.1f && b[3] >= 0.0f && b[3] <= 1.1f);
                if (rel && rootW > 0 && rootH > 0) {
                    b[0] *= static_cast<float>(rootW);
                    b[1] *= static_cast<float>(rootH);
                    b[2] *= static_cast<float>(rootW);
                    b[3] *= static_cast<float>(rootH);
                }
                RectPtr r = std::make_shared<Rect>(b[0], b[1], b[2], b[3]);
                this->_cachedBlackWidgetRects[activity].push_back(r);
                this->_currentBoundsOnlyAvoidRects.push_back(r);
            }
        };
        addRulesForActivity(activity);
        addRulesForActivity("");

        this->resolveElementWithAvoid(rootXML, activity);
    }

    /**
     * @brief Single-pass resolve: avoid (delete + rect cache) + modify (property overrides), deMixResMapping, cachePageTexts, pruningValidTexts, then recurse.
     * Replaces former resolveBlackWidgets + resolveElement + resolveTreePruning.
     */
    void Preference::resolveElementWithAvoid(const ElementPtr &element, const std::string &activity) {
        if (!element) return;

        if (!this->_resMixedMapping.empty()) {
            std::string stringOfResourceID = element->getResourceID();
            if (!stringOfResourceID.empty()) {
                auto it = this->_resMixedMapping.find(stringOfResourceID);
                if (it != this->_resMixedMapping.end()) {
                    element->reSetResourceID(it->second);
                    BDLOG("de-mixed %s as %s", stringOfResourceID.c_str(), it->second.c_str());
                }
            }
        }

        if (!element->getText().empty()) {
            if (this->_pageTextsCache.size() > PageTextsMaxCount) {
                for (int i = 0; i < 20 && !this->_pageTextsCache.empty(); i++)
                    this->_pageTextsCache.pop_front();
            }
            this->_pageTextsCache.push_back(element->getText());
        }

        int rootW = this->_rootScreenSize ? this->_rootScreenSize->right : 0;
        int rootH = this->_rootScreenSize ? this->_rootScreenSize->bottom : 0;
        auto boundsToRect = [rootW, rootH](const std::vector<float> &b) -> RectPtr {
            if (b.size() != 4) return nullptr;
            std::vector<float> abs = b;
            bool rel = (b[0] >= 0.0f && b[0] <= 1.1f && b[1] >= 0.0f && b[1] <= 1.1f &&
                        b[2] >= 0.0f && b[2] <= 1.1f && b[3] >= 0.0f && b[3] <= 1.1f);
            if (rel && rootW > 0 && rootH > 0) {
                abs[0] *= static_cast<float>(rootW);
                abs[1] *= static_cast<float>(rootH);
                abs[2] *= static_cast<float>(rootW);
                abs[3] *= static_cast<float>(rootH);
            }
            return std::make_shared<Rect>(abs[0], abs[1], abs[2], abs[3]);
        };

        for (const std::string &actKey : {activity, std::string("")}) {
            auto it = this->_avoidRulesByActivity.find(actKey);
            if (it == this->_avoidRulesByActivity.end()) continue;
            for (const AvoidRulePtr &rule : it->second) {
                if (rule->action == AvoidRule::Action::Avoid) {
                    if (rule->xpath && element->matchXpathSelector(rule->xpath)) {
                        if (rule->bounds.size() == 4) {
                            RectPtr rejectRect = boundsToRect(rule->bounds);
                            RectPtr elBounds = element->getBounds();
                            if (!rejectRect || !elBounds || !rejectRect->contains(elBounds->center()))
                                continue;
                        }
                        RectPtr bounds = element->getBounds();
                        if (bounds) this->_cachedBlackWidgetRects[activity].push_back(bounds);
                        BLOG("avoid rule, delete node: %s", element->getResourceID().c_str());
                        element->deleteElement();
                        return;
                    }
                    if (!rule->xpath && rule->bounds.size() == 4) {
                        RectPtr elBounds = element->getBounds();
                        if (!elBounds) continue;
                        Point c = elBounds->center();
                        for (const RectPtr &rect : this->_currentBoundsOnlyAvoidRects) {
                            if (rect && rect->contains(c)) {
                                BLOG("avoid rule (bounds), delete node: %s", element->getResourceID().c_str());
                                element->deleteElement();
                                return;
                            }
                        }
                    }
                } else if (rule->action == AvoidRule::Action::Modify && rule->xpath && element->matchXpathSelector(rule->xpath)) {
                    if (rule->resourceID != InvalidProperty) element->reSetResourceID(rule->resourceID);
                    if (rule->contentDescription != InvalidProperty) element->reSetContentDesc(rule->contentDescription);
                    if (rule->text != InvalidProperty) element->reSetText(rule->text);
                    if (rule->classname != InvalidProperty) element->reSetClassname(rule->classname);
                    if (!rule->clickable.empty()) element->reSetClickable(rule->clickable == "true");
                }
            }
        }

        if (this->_pruningValidTexts) this->pruningValidTexts(element);

        for (const auto &child : element->getChildren()) {
            this->resolveElementWithAvoid(child, activity);
        }
    }

    /**
     * @brief Check if a point is inside blacklisted rectangles (unified avoid rects from max.avoid.rules)
     * 
     * Checks if the given coordinate point falls within any of the cached black widget
     * rectangles for the specified activity. This is used to prevent clicking on
     * blacklisted regions (e.g., logout button).
     * 
     * @param activity Current activity name
     * @param pointX X coordinate of the point
     * @param pointY Y coordinate of the point
     * 
     * @return bool - true if point is inside any black rect, false otherwise
     * 
     * @note Performance optimization:
     *       - Early return if no cached rects
     *       - Inline coordinate comparison (avoids Point object creation)
     *       - Early exit on first match
     *       - Logging controlled by FASTBOT_LOG_BLACK_RECT_CHECK macro
     * 
     * @note This function is called frequently (before every click action),
     *       so performance is critical.
     */
    bool Preference::checkPointIsInBlackRects(const std::string &activity, int pointX, int pointY) {
        // Performance optimization: Early return if no cached rects for this activity
        auto iter = this->_cachedBlackWidgetRects.find(activity);
        if (iter == this->_cachedBlackWidgetRects.end() || iter->second.empty()) {
            return false;
        }
        
        // Performance optimization: Direct point comparison instead of creating Point object
        // Rect::contains checks: point.x >= left && point.x <= right && point.y >= top && point.y <= bottom
        // We can inline this check to avoid Point object creation
        bool isInsideBlackList = false;
        const std::vector<RectPtr> &rects = iter->second;
        
        // Performance: Use range-based for loop with early exit
        // Most points will not be in black rects, so early exit is common
        for (const auto &rect : rects) {
            if (rect && 
                pointX >= rect->left && pointX <= rect->right &&
                pointY >= rect->top && pointY <= rect->bottom) {
                isInsideBlackList = true;
                break;
            }
        }
        
        // Performance optimization: Only log when enabled (this function is called frequently)
#if FASTBOT_LOG_BLACK_RECT_CHECK
        BLOG("check point [%d, %d] is %s in black widgets", pointX, pointY,
             isInsideBlackList ? "" : "not");
#endif
        return isInsideBlackList;
    }

    /**
     * @brief Prune valid texts: mark valid texts and set clickable
     * 
     * Recursively processes elements to find valid texts (texts that appear in _validTexts set).
     * If a valid text is found:
     * 1. Sets element->validText to the found text
     * 2. If parent is not clickable, sets current element as clickable
     * 
     * Valid texts are typically extracted from APK resources and represent legitimate
     * UI text that should be clickable.
     * 
     * @param element Current Element to process
     * 
     * @note Performance optimization:
     *       - Checks text first, then contentDescription (most elements have text)
     *       - Caches parent lock result to avoid repeated weak_ptr operations
     * 
     * @note Only processes if _pruningValidTexts flag is enabled
     */
    void Preference::pruningValidTexts(const ElementPtr &element) {
        if (!element || this->_validTexts.empty()) {
            return;
        }
        
        // Performance optimization: Check text first, then content description
        // Most elements have text, so checking text first is more efficient
        bool valid = false;
        const std::string &originalTextOfElement = element->getText();
        if (!originalTextOfElement.empty()) {
            // Performance: Use find() which returns iterator, more efficient than count()
            auto textIt = this->_validTexts.find(originalTextOfElement);
            if (textIt != this->_validTexts.end()) {
                element->validText = originalTextOfElement;
                valid = true;
            }
        }
        
        // If text is not valid, try content description
        if (!valid) {
            const std::string &contentDescription = element->getContentDesc();
            if (!contentDescription.empty()) {
                auto contentIt = this->_validTexts.find(contentDescription);
                if (contentIt != this->_validTexts.end()) {
                    element->validText = contentDescription;
                    valid = true;
                }
            }
        }
        
        // BDLOG("set valid Text: %s ", element->validText.c_str());
        
        // Performance optimization: Cache parent lock result to avoid repeated weak_ptr operations
        // if we find valid text from text field or content description field,
        // and its parent is clickable, then set it as clickable.
        if (valid) {
            auto parentWeak = element->getParent();
            if (!parentWeak.expired()) {
                auto parentLocked = parentWeak.lock();
                if (parentLocked && !parentLocked->getClickable()) {
                    BDLOG("%s", "set valid Text  set clickable true");
                    element->reSetClickable(true);
                }
            }
        }

        // Recursively process children
        for (const auto &child: element->getChildren()) {
            pruningValidTexts(child);
        }
    }

    /**
     * @brief Find all elements matching the xpath selector
     * 
     * Recursively traverses the UI tree and collects all elements that match
     * the given xpath selector. Results are appended to the output vector.
     * 
     * @param outElements Output parameter: vector to store matched elements
     * @param xpathSelector XPath selector describing matching criteria
     * @param elementXML Root Element to start searching from
     * 
     * @note This function collects ALL matching elements. For better performance
     *       when only the first match is needed, use findFirstMatchedElement().
     */
    /**
     * @brief Find all elements matching the xpath selector
     * 
     * Recursively traverses the UI tree and collects all elements that match
     * the given xpath selector. Results are appended to the output vector.
     * 
     * @param outElements Output parameter: vector to store matched elements
     * @param xpathSelector XPath selector describing matching criteria
     * @param elementXML Root Element to start searching from
     * 
     * @note This function collects ALL matching elements. For better performance
     *       when only the first match is needed, use findFirstMatchedElement().
     * 
     * @note Performance optimizations:
     *       - Early validation of inputs
     *       - Cached children reference to avoid repeated getChildren() calls
     *       - Reduced logging overhead in recursive calls
     */
    void Preference::findMatchedElements(std::vector<ElementPtr> &outElements,
                                         const XpathPtr &xpathSelector,
                                         const ElementPtr &elementXML) {
        // Performance: Early validation - check both inputs
        if (!elementXML || !xpathSelector) {
            // Performance: Skip logging in recursive calls (caller should validate root)
            // Only log if this is the root call (outElements is empty)
            if (outElements.empty()) {
                BLOGE("findMatchedElements: elementXML or xpathSelector is null");
            }
            return;
        }
        
        // Check if current element matches
        if (elementXML->matchXpathSelector(xpathSelector)) {
            // Performance: Use emplace_back for in-place construction (though shared_ptr copy is cheap)
            outElements.push_back(elementXML);
        }

        // Performance: Cache children reference to avoid repeated getChildren() calls
        const auto &children = elementXML->getChildren();
        // Performance: Early exit if no children (common case for leaf nodes)
        if (children.empty()) {
            return;
        }
        
        // Recursively process children
        for (const auto &child: children) {
            findMatchedElements(outElements, xpathSelector, child);
        }
    }

    /**
     * @brief Find the first element matching the xpath selector (early termination)
     * 
     * Recursively searches the UI tree and returns the first element that matches
     * the xpath selector. Stops searching immediately after finding the first match.
     * 
     * @param xpathSelector XPath selector describing matching criteria
     * @param elementXML Root Element to start searching from
     * 
     * @return ElementPtr - First matching element, or nullptr if none found
     * 
     * @note Performance optimization: Early termination avoids unnecessary tree traversal
     *       when only the first match is needed (e.g., patchActionBounds).
     * 
     * @note Performance optimizations:
     *       - Early validation of inputs
     *       - Cached children reference to avoid repeated getChildren() calls
     *       - Early exit for leaf nodes (no children)
     *       - Direct return instead of storing in temporary variable
     */
    ElementPtr Preference::findFirstMatchedElement(const XpathPtr &xpathSelector,
                                                   const ElementPtr &elementXML) {
        // Performance: Early validation - check both inputs
        if (!elementXML || !xpathSelector) {
            return nullptr;
        }
        
        // Check current element first (depth-first search)
        if (elementXML->matchXpathSelector(xpathSelector)) {
            return elementXML;
        }
        
        // Performance: Cache children reference to avoid repeated getChildren() calls
        const auto &children = elementXML->getChildren();
        // Performance: Early exit if no children (common case for leaf nodes)
        if (children.empty()) {
            return nullptr;
        }
        
        // Recursively check children (early termination on first match)
        for (const auto &child: children) {
            // Performance: Direct return instead of storing in temporary variable
            // This avoids an extra assignment and allows compiler optimization
            ElementPtr result = findFirstMatchedElement(xpathSelector, child);
            if (result != nullptr) {
                return result;
            }
        }
        
        return nullptr;
    }

    /**
     * @brief De-mix resource ID mapping: map obfuscated resource IDs back to original
     * 
     * Recursively processes elements to replace obfuscated resource IDs with their
     * original values using the _resMixedMapping lookup table.
     * 
     * @param rootXML Root Element to process
     * 
     * @note This function is now merged into resolveElement() to reduce tree traversals.
     *       It's kept as a separate function for backward compatibility but is no longer
     *       called directly from resolvePage().
     * 
     * @deprecated Use resolveElement() instead, which includes this functionality.
     * 
     * @note Performance optimizations:
     *       - Uses const reference for getResourceID() to avoid string copy
     *       - Cached children reference to avoid repeated getChildren() calls
     *       - Early exit for leaf nodes (no children)
     *       - Optimized iterator access using -> instead of (*)
     */
    void Preference::deMixResMapping(const ElementPtr &rootXML) {
        // Performance: Early validation
        if (!rootXML || this->_resMixedMapping.empty()) {
            return;
        }
        
        // Performance: Use const reference instead of copy to avoid string allocation
        const std::string &stringOfResourceID = rootXML->getResourceID();
        if (!stringOfResourceID.empty()) {
            auto iterator = this->_resMixedMapping.find(stringOfResourceID);
            if (iterator != this->_resMixedMapping.end()) {
                // Performance: Use -> instead of (*) for iterator access
                rootXML->reSetResourceID(iterator->second);
                BDLOG("de-mixed %s as %s", stringOfResourceID.c_str(), iterator->second.c_str());
            }
        }

        // Performance: Cache children reference to avoid repeated getChildren() calls
        const auto &children = rootXML->getChildren();
        // Performance: Early exit if no children (common case for leaf nodes)
        if (children.empty()) {
            return;
        }
        
        // Recursively process children
        for (const auto &child: children) {
            deMixResMapping(child);
        }
    }

    /**
     * @brief Load resource ID mapping file
     * 
     * Loads a resource ID mapping file that maps obfuscated resource IDs back to
     * their original values. This is used for handling code obfuscation.
     * 
     * @param resourceMappingPath Path to the mapping file
     * 
     * @note File format:
     *       - Lines containing ".R.id." are processed
     *       - Format: "0x7f0a0012: id/foo -> :id/bar"
     *       - Creates bidirectional mapping (_resMapping and _resMixedMapping)
     */
    /**
     * @brief Load resource ID mapping file
     * 
     * Loads a resource ID mapping file that maps obfuscated resource IDs back to
     * their original values. This is used for handling code obfuscation.
     * 
     * @param resourceMappingPath Path to the mapping file
     * 
     * @note File format:
     *       - Lines containing ".R.id." are processed
     *       - Format: "0x7f0a0012: id/foo -> :id/bar"
     *       - Creates bidirectional mapping (_resMapping and _resMixedMapping)
     * 
     * @note Performance optimizations:
     *       - Uses reference instead of copy for line iteration
     *       - Early exit for lines without ".R.id."
     *       - Manual string processing to avoid multiple passes
     *       - Reduced string allocations
     */
    void Preference::loadMixResMapping(const std::string &resourceMappingPath) {
        BLOG("loading resource mapping : %s", resourceMappingPath.c_str());
        std::string content = loadFileContent(resourceMappingPath);
        if (content.empty()) {
            return;
        }
        
        std::vector<std::string> lines;
        splitString(content, lines, '\n');
        
        // Performance: Use reference instead of copy to avoid string allocation per iteration
        for (auto &line: lines) {
            // Performance: Early exit for lines that don't contain ".R.id."
            if (line.find(".R.id.") == std::string::npos) {
                continue;
            }
            
            // Parse format like "0x7f0a0012: id/foo -> :id/bar": take substring after first colon after "0x"
            size_t pos0x = line.find("0x");
            if (pos0x != std::string::npos) {
                size_t posColon = line.find(':', pos0x);
                if (posColon != std::string::npos) {
                    // Performance: Modify line in-place instead of creating new string
                    line = line.substr(posColon + 1);
                }
            }
            
            // Performance: Remove spaces and replace ".R.id." with ":id/"
            // Using stringReplaceAll is efficient enough for this use case
            stringReplaceAll(line, " ", "");
            stringReplaceAll(line, ".R.id.", ":id/");
            
            // Find "->" separator
            size_t startPos = line.find("->");
            if (startPos == std::string::npos || startPos + 2 >= line.length()) {
                continue; // Invalid format or no value after "->"
            }
            
            // Performance: Extract substrings directly
            std::string resId = line.substr(0, startPos);
            std::string mixedResid = line.substr(startPos + 2);
            
            // Security: Skip empty mappings to avoid invalid map entries
            if (resId.empty() || mixedResid.empty()) {
                continue;
            }
            
            BDLOG("res id %s mixed to %s", resId.c_str(), mixedResid.c_str());
            this->_resMapping[resId] = mixedResid;
            this->_resMixedMapping[mixedResid] = resId;
        }
    }

    /**
     * @brief Load valid texts from file
     * 
     * Loads a list of valid texts (typically extracted from APK resources).
     * These texts are used by pruningValidTexts() to identify legitimate UI text.
     * 
     * @param pathOfValidTexts Path to the valid texts file
     * 
     * @note File format:
     *       - Regular text: one text per line
     *       - String resources: "String #123: actual_text" format
     *       - Automatically enables _pruningValidTexts flag if file is loaded
     */
    /**
     * @brief Load valid texts from file
     * 
     * Loads a list of valid texts (typically extracted from APK resources).
     * These texts are used by pruningValidTexts() to identify legitimate UI text.
     * 
     * @param pathOfValidTexts Path to the valid texts file
     * 
     * @note File format:
     *       - Regular text: one text per line
     *       - String resources: "String #123: actual_text" format
     *       - Automatically enables _pruningValidTexts flag if file is loaded
     * 
     * @note Performance optimizations:
     *       - Early exit for empty lines
     *       - Check "String #" first to avoid unnecessary find(": ") calls
     *       - Use const reference for line iteration
     *       - Skip empty strings before inserting
     */
    void Preference::loadValidTexts(const std::string &pathOfValidTexts) {
        std::string fileContent = loadFileContent(pathOfValidTexts);
        if (fileContent.empty()) {
            return;
        }
        
        this->_validTexts.clear();
        std::vector<std::string> validStringLines;
        splitString(fileContent, validStringLines, '\n');
        
        // Performance: Use const reference to avoid unnecessary string operations
        for (const auto &line: validStringLines) {
            // Performance: Skip empty lines early
            if (line.empty()) {
                continue;
            }
            
            // Performance: Check "String #" first (more specific pattern)
            // Most lines won't contain "String #", so this avoids unnecessary find(": ") calls
            size_t stringHashPos = line.find("String #");
            if (stringHashPos != std::string::npos) {
                // String resource format: "String #123: actual_text"
                // Find ": " after "String #"
                size_t colonPos = line.find(": ", stringHashPos);
                if (colonPos != std::string::npos && colonPos + 2 < line.length()) {
                    std::string extractedText = line.substr(colonPos + 2);
                    // Performance: Only insert non-empty strings
                    if (!extractedText.empty()) {
                        this->_validTexts.emplace(std::move(extractedText));
                    }
                }
            } else {
                // Regular text format: use the whole line
                this->_validTexts.emplace(line);
            }
        }
        
        // Performance: Set flag only if we actually loaded texts
        if (!this->_validTexts.empty()) {
            this->_pruningValidTexts = true;
        }
    }

    /**
     * @brief Load all configuration files
     * 
     * Main configuration loading function. Loads all configuration files in order:
     * 1. Resource mapping (max.mapping)
     * 2. Valid texts (max.valid.strings)
     * 3. Base config (max.config)
     * 4. Unified avoidance rules (max.avoid.rules; replaces max.widget.black + max.tree.pruning)
     * 5. LLM tasks (max.llm.tasks)
     * 6. White/black lists (awl.strings, abl.strings)
     * 8. Input texts (max.strings, max.fuzzing.strings)
     * 
     * @note Only loads on Android or in debug mode
     * 
     * @note Performance optimizations:
     *       - Individual error handling for each config to prevent one failure from blocking others
     *       - More detailed error logging to help identify which config failed
     *       - Optimized loading order: critical configs first
     */
    void Preference::loadConfigs() {
#if defined(__ANDROID__) || defined(_DEBUG_)
        // Performance: Individual error handling for each config
        // This ensures that if one config fails to load, others can still be loaded
        // Critical configs are loaded first
        
        // 1. Resource mapping (used for de-obfuscation)
        try {
            loadMixResMapping(DefaultResMappingFilePath);
        } catch (const std::exception &ex) {
            BLOGE("Failed to load resource mapping: %s", ex.what());
        }
        
        // 2. Valid texts (used for text pruning)
        try {
            loadValidTexts(ValidTextFilePath);
        } catch (const std::exception &ex) {
            BLOGE("Failed to load valid texts: %s", ex.what());
        }
        
        // 3. Base config (contains critical settings like input fuzzing flags)
        try {
            loadBaseConfig();
        } catch (const std::exception &ex) {
            BLOGE("Failed to load base config: %s", ex.what());
        }
        
        // 4. Unified avoidance rules (replaces max.widget.black + max.tree.pruning)
        try {
            loadAvoidRules();
        } catch (const std::exception &ex) {
            BLOGE("Failed to load avoid rules: %s", ex.what());
        }
        
        // 4.1 Custom event sequence (max.xpath.actions)
        try {
            loadXpathActions();
        } catch (const std::exception &ex) {
            BLOGE("Failed to load xpath actions: %s", ex.what());
        }
        
        // 5.1 LLM tasks (LLMTaskAgent tasks)
        try {
            loadLlmTasks();
        } catch (const std::exception &ex) {
            BLOGE("Failed to load LLM tasks: %s", ex.what());
        }
        
        // 6. White/black lists (currently not actively used)
        try {
            loadWhiteBlackList();
        } catch (const std::exception &ex) {
            BLOGE("Failed to load white/black lists: %s", ex.what());
        }
        
        // 8. Input texts (used for input fuzzing)
        try {
            loadInputTexts();
        } catch (const std::exception &ex) {
            BLOGE("Failed to load input texts: %s", ex.what());
        }
#endif
    }

#define MaxRandomPickSTR          "max.randomPickFromStringList"
#define InputFuzzSTR              "max.doinputtextFuzzing"
#define ListenMode                "max.listenMode"
#define StaticStateAbstractionSTR "max.staticStateAbstraction"
#define LlmEnabledSTR             "max.llm.enabled"
#define LlmKnowledgeSTR           "max.llm.knowledge"
#define LlmWidgetPrioritySTR      "max.llm.widgetpriority"
#define LlmContextAwareInputSTR   "max.llm.contextAwareInput"
#define LlmApiUrlSTR              "max.llm.apiUrl"
#define LlmApiKeySTR              "max.llm.apiKey"
#define LlmModelSTR               "max.llm.model"
#define LlmMaxTokensSTR           "max.llm.maxTokens"
#define LlmTimeoutMsSTR           "max.llm.timeoutMs"
#define ReuseDecisionTuningSTR    "max.reuse.decisionTuning"

    /**
     * @brief Load base configuration file
     * 
     * Loads the base configuration file (/sdcard/max.config) which contains
     * key-value pairs for various settings.
     * 
     * Supported configuration keys:
     * - max.randomPickFromStringList: Use random preset strings for input
     * - max.doinputtextFuzzing: Enable input text fuzzing
     * - max.listenMode: Enable listen mode (skip all model actions)
     * 
     * @note File format: key=value, one per line
     */
    /**
     * @brief Load base configuration file
     * 
     * Loads the base configuration file (/sdcard/max.config) which contains
     * key-value pairs for various settings.
     * 
     * Supported configuration keys:
     * - max.randomPickFromStringList: Use random preset strings for input
     * - max.doinputtextFuzzing: Enable input text fuzzing
     * - max.listenMode: Enable listen mode (skip all model actions)
     * 
     * @note File format: key=value, one per line
     * 
     * @note Performance optimizations:
     *       - Early exit for empty lines
     *       - Manual key-value parsing to avoid multiple splitString calls
     *       - String comparison optimization
     *       - Reduced logging overhead
     */
    void Preference::loadBaseConfig() {
        // LOGI("pref init checking curr packageName is offset: %s", Preference::PackageName.c_str());
        std::string configContent = loadFileContent(BaseConfigFilePath);
        if (configContent.empty()) {
            return;
        }
        
        // Pretty-print max.config without extra blank line, and indent each entry
        BLOG("max.config:");
        std::vector<std::string> lines;
        splitString(configContent, lines, '\n');
        
        for (const std::string &line: lines) {
            // Performance: Skip empty or whitespace-only lines early
            std::string trimmedLine = line;
            trimString(trimmedLine);
            if (trimmedLine.empty()) {
                continue;
            }
            
            // Log raw (trimmed) line with indentation for readability; keep comments
            BLOG("  %s", trimmedLine.c_str());
            
            // Performance: Manual key-value parsing to avoid splitString overhead
            size_t eqPos = trimmedLine.find('=');
            if (eqPos == std::string::npos || eqPos == 0 || eqPos == line.length() - 1) {
                continue; // No '=' or '=' at start/end
            }
            
            // Extract key and value
            std::string key = trimmedLine.substr(0, eqPos);
            std::string value = trimmedLine.substr(eqPos + 1);
            
            // Trim whitespace
            trimString(key);
            trimString(value);
            
            // Skip if key is empty; allow empty value for env-backed keys (max.llm.apiUrl, max.llm.apiKey)
            if (key.empty()) {
                continue;
            }
            if (value.empty() && key != LlmApiUrlSTR && key != LlmApiKeySTR) {
                continue;
            }
            
            // Human-friendly config log: max.config parsed: key=value
            //BDLOG("max.config parsed: %s=%s", key.c_str(), value.c_str());
            
            // Performance: Use string comparison with early exit
            // Check most common keys first (if we know the distribution)
            if (key == MaxRandomPickSTR) {
                BDLOG("set %s", MaxRandomPickSTR);
                this->_randomInputText = (value == "true");
            } else if (key == InputFuzzSTR) {
                BDLOG("set %s", InputFuzzSTR);
                this->_doInputFuzzing = (value == "true");
            } else if (key == ListenMode) {
                BDLOG("set %s", ListenMode);
                this->setListenMode(value == "true");
            } else if (key == StaticStateAbstractionSTR) {
                // max.staticStateAbstraction=true  -> legacy static reuse abstraction
                // max.staticStateAbstraction=false -> dynamic abstraction
                this->_useStaticReuseAbstraction = (value == "true");
                if (this->_useStaticReuseAbstraction) {
                    BLOG("state abstraction: static (legacy static reuse state abstraction enabled)");
                } else {
                    BLOG("state abstraction: dynamic (runtime refinement/coarsening enabled)");
                }
            } else if (key == LlmEnabledSTR) {
                this->_llmRuntimeConfig.enabled = (value == "true");
            } else if (key == LlmKnowledgeSTR) {
                this->_llmKnowledge = (value == "true");
                if (this->_llmKnowledge) BDLOG("set %s (LLM knowledge_org enabled)", LlmKnowledgeSTR);
            } else if (key == LlmWidgetPrioritySTR) {
                this->_llmKnowledge = (value == "true");
                if (this->_llmKnowledge) BDLOG("set %s (LLM widget_priority enabled)", LlmWidgetPrioritySTR);
            } else if (key == LlmContextAwareInputSTR) {
                this->_llmContextAwareInput = (value == "true");
                if (this->_llmContextAwareInput) BDLOG("set %s (LLM content_aware_input enabled)", LlmContextAwareInputSTR);
            } else if (key == LlmApiUrlSTR) {
                if (value.size() >= 4 && value[0] == '$' && value[1] == '{' && value.back() == '}') {
                    std::string varName = value.substr(2, value.size() - 3);
                    const char *envVal = std::getenv(varName.c_str());
                    this->_llmRuntimeConfig.apiUrl = envVal ? envVal : value;
                } else if (value.empty()) {
                    const char *envVal = std::getenv("MAX_LLM_API_URL");
                    if (envVal) this->_llmRuntimeConfig.apiUrl = envVal;
                } else {
                    this->_llmRuntimeConfig.apiUrl = value;
                }
            } else if (key == LlmApiKeySTR) {
                if (value.size() >= 4 && value[0] == '$' && value[1] == '{' && value.back() == '}') {
                    std::string varName = value.substr(2, value.size() - 3);
                    const char *envVal = std::getenv(varName.c_str());
                    this->_llmRuntimeConfig.apiKey = envVal ? envVal : value;
                } else if (value.empty()) {
                    const char *envVal = std::getenv("MAX_LLM_API_KEY");
                    if (envVal) this->_llmRuntimeConfig.apiKey = envVal;
                } else {
                    this->_llmRuntimeConfig.apiKey = value;
                }
            } else if (key == LlmModelSTR) {
                this->_llmRuntimeConfig.model = value;
            } else if (key == LlmMaxTokensSTR) {
                try {
                    this->_llmRuntimeConfig.maxTokens = std::stoi(value);
                } catch (...) {
                    BLOGE("invalid max.llm.maxTokens value: %s", value.c_str());
                }
            } else if (key == LlmTimeoutMsSTR) {
                try {
                    this->_llmRuntimeConfig.timeoutMs = std::stoi(value);
                } catch (...) {
                    BLOGE("invalid max.llm.timeoutMs value: %s", value.c_str());
                }
            } else if (key == ReuseDecisionTuningSTR) {
                this->_reuseDecisionTuning = (value == "true");
            }
        }
    }

    /**
     * @brief Cache page texts recursively
     * 
     * Recursively collects all non-empty text from elements and caches them
     * in _pageTextsCache. If cache exceeds PageTextsMaxCount, removes oldest entries.
     * 
     * @param rootElement Root Element to start caching from
     * 
     * @note This function is now merged into resolveElement() to reduce tree traversals.
     *       It's kept as a separate function for backward compatibility but is no longer
     *       called directly from resolvePage().
     * 
     * @deprecated Use resolveElement() instead, which includes this functionality.
     */
    /**
     * @brief Cache page texts recursively
     * 
     * Recursively collects all non-empty text from elements and caches them
     * in _pageTextsCache. If cache exceeds PageTextsMaxCount, removes oldest entries.
     * 
     * @param rootElement Root Element to start caching from
     * 
     * @note This function is now merged into resolveElement() to reduce tree traversals.
     *       It's kept as a separate function for backward compatibility but is no longer
     *       called directly from resolvePage().
     * 
     * @deprecated Use resolveElement() instead, which includes this functionality.
     * 
     * @note Performance optimizations:
     *       - Cached children reference to avoid repeated getChildren() calls
     *       - Early exit for empty children
     *       - Cache text reference to avoid repeated getText() calls
     */
    void Preference::cachePageTexts(const ElementPtr &rootElement) {
        if (!rootElement) {
            return;
        }
        
        // Performance: Check cache size and trim if needed
        if (this->_pageTextsCache.size() > PageTextsMaxCount) {
            for (int i = 0; i < 20 && !this->_pageTextsCache.empty(); i++) {
                this->_pageTextsCache.pop_front();
            }
        }
        
        // Performance: Cache text reference to avoid repeated getText() calls
        const std::string &text = rootElement->getText();
        if (!text.empty()) {
            this->_pageTextsCache.push_back(text);
        }
        
        // Performance: Cache children reference to avoid repeated getChildren() calls
        const auto &children = rootElement->getChildren();
        // Performance: Early exit if no children (common case for leaf nodes)
        if (children.empty()) {
            return;
        }
        
        // Recursively process children
        for (const auto &childElement: children) {
            this->cachePageTexts(childElement);
        }
    }


    /**
     * @brief Set listen mode
     * 
     * Enables or disables listen mode. In listen mode, all actions from the model
     * are skipped, allowing the system to listen to user interactions only.
     * 
     * @param listen true to enable listen mode, false to disable
     */
    void Preference::setListenMode(bool listen) {
        BDLOG("set %s", ListenMode);
        this->_skipAllActionsFromModel = listen;
        LOGI("fastbot native use a listen mode: %d !!!", this->_skipAllActionsFromModel);
    }

    /**
     * @brief Load unified avoidance rules from /sdcard/max.avoid.rules (replaces max.widget.black + max.tree.pruning).
     * Each rule: activity, xpath (optional), bounds (optional), action ("avoid" | "modify"), and for modify: resourceid, text, contentdesc, classname, clickable.
     */
    void Preference::loadAvoidRules() {
        std::string fileContent = fastbotx::Preference::loadFileContent(AvoidRulesFilePath);
        if (fileContent.empty()) return;
        try {
            BLOG("loading avoid rules: %s", AvoidRulesFilePath.c_str());
            ::nlohmann::json arr = ::nlohmann::json::parse(fileContent);
            if (!arr.is_array()) {
                BLOGE("avoid rules is not a JSON array");
                return;
            }
            this->_avoidRulesByActivity.clear();
            this->_avoidRules.clear();
            this->_avoidRules.reserve(arr.size());
            for (const ::nlohmann::json &obj : arr) {
                AvoidRulePtr r = std::make_shared<AvoidRule>();
                r->activity = getJsonValue<std::string>(obj, "activity", "");
                std::string xpathStr = getJsonValue<std::string>(obj, "xpath", "");
                if (!xpathStr.empty()) r->xpath = std::make_shared<Xpath>(xpathStr);
                std::string actionStr = getJsonValue<std::string>(obj, "action", "avoid");
                r->action = (actionStr == "modify") ? AvoidRule::Action::Modify : AvoidRule::Action::Avoid;
                std::string boundsStr = getJsonValue<std::string>(obj, "bounds", "");
                if (!boundsStr.empty()) {
                    r->bounds.resize(4);
                    int parsed = (boundsStr.find('[') != std::string::npos)
                        ? sscanf(boundsStr.c_str(), "[%f,%f][%f,%f]", &r->bounds[0], &r->bounds[1], &r->bounds[2], &r->bounds[3])
                        : sscanf(boundsStr.c_str(), "%f,%f,%f,%f", &r->bounds[0], &r->bounds[1], &r->bounds[2], &r->bounds[3]);
                    if (parsed != 4) {
                        BLOGE("Failed to parse avoid rule bounds: %s", boundsStr.c_str());
                        r->bounds.clear();
                    }
                }
                r->resourceID = getJsonValue<std::string>(obj, "resourceid", InvalidProperty);
                r->text = getJsonValue<std::string>(obj, "text", InvalidProperty);
                r->contentDescription = getJsonValue<std::string>(obj, "contentdesc", InvalidProperty);
                r->classname = getJsonValue<std::string>(obj, "classname", InvalidProperty);
                r->clickable = getJsonValue<std::string>(obj, "clickable", "");
                this->_avoidRules.push_back(r);
                this->_avoidRulesByActivity[r->activity].push_back(r);
            }
        } catch (::nlohmann::json::exception &ex) {
            BLOGE("parse avoid rules error: id,%d: %s", ex.id, ex.what());
        }
    }

    /**
     * @brief Load custom event sequence from /sdcard/max.xpath.actions.
     * Format: JSON array of cases. Each case: activity, prob (0-1 or 1=100%), times, throttle (ms), actions: [ { xpath?, action, text? } ].
     * Action types: CLICK, LONG_CLICK, BACK, SCROLL_TOP_DOWN, SCROLL_BOTTOM_UP, SCROLL_LEFT_RIGHT, SCROLL_RIGHT_LEFT.
     */
    void Preference::loadXpathActions() {
        std::string fileContent = loadFileContent(XpathActionsFilePath);
        if (fileContent.empty()) return;
        try {
            BLOG("loading xpath actions: %s", XpathActionsFilePath.c_str());
            ::nlohmann::json arr = ::nlohmann::json::parse(fileContent);
            if (!arr.is_array()) {
                BLOGE("max.xpath.actions is not a JSON array");
                return;
            }
            _xpathActionCases.clear();
            _xpathCaseRemainingTimes.clear();
            _currentXpathCaseIdx = -1;
            _currentXpathStepIdx = 0;
            for (const ::nlohmann::json &obj : arr) {
                XpathCase c;
                c.activity = getJsonValue<std::string>(obj, "activity", "");
                double p = getJsonValue<double>(obj, "prob", 1.0);
                c.prob = (p > 1.0) ? static_cast<float>(p / 100.0) : static_cast<float>(p);  // support 1 or 100 for 100%
                c.times = getJsonValue<int>(obj, "times", 1);
                c.throttle = getJsonValue<int>(obj, "throttle", 0);
                if (!obj.contains("actions") || !obj["actions"].is_array()) continue;
                for (const ::nlohmann::json &a : obj["actions"]) {
                    XpathActionStep step;
                    step.xpath = getJsonValue<std::string>(a, "xpath", "");
                    step.actionType = getJsonValue<std::string>(a, "action", "");
                    step.text = getJsonValue<std::string>(a, "text", "");
                    step.clearText = getJsonValue<bool>(a, "clearText", false);
                    step.throttle = getJsonValue<int>(a, "throttle", 0);
                    step.waitTime = getJsonValue<int>(a, "wait", 0);
                    if (!step.actionType.empty()) c.steps.push_back(step);
                }
                if (!c.steps.empty()) {
                    _xpathActionCases.push_back(c);
                    _xpathCaseRemainingTimes.push_back(c.times);

                    // Pretty, human-friendly summary for each xpath action case
                    BLOG("\nxpath action case:\n  activity=%s\n  prob=%.2f\n  times=%d",
                         c.activity.c_str(), c.prob, c.times);
                    for (const auto &step : c.steps) {
                        BLOG("    action=%s xpath=%s text=%s",
                             step.actionType.c_str(),
                             step.xpath.c_str(),
                             step.text.c_str());
                    }
                }
            }
            BLOG("loaded %zu xpath action cases", _xpathActionCases.size());
        } catch (::nlohmann::json::exception &ex) {
            BLOGE("parse max.xpath.actions error: id,%d: %s", ex.id, ex.what());
        }
    }

    /**
     * @brief Get next custom action from max.xpath.actions. When a case is in progress returns next step; else may start a case by prob.
     * When max.llm.tasks exists and LLM url/apikey are configured, xpath custom events are disabled (return nullptr).
     */
    ActionPtr Preference::getCustomActionFromXpath(const std::string &activity,
                                                  const ElementPtr &rootXML) {
        if (!rootXML || _xpathActionCases.empty()) return nullptr;

        // If LLM tasks are loaded and LLM url/apikey are configured, max.xpath.actions custom events are ineffective.
        if (!_llmTasks.empty() && !_llmRuntimeConfig.apiUrl.empty() && !_llmRuntimeConfig.apiKey.empty())
            return nullptr;

        // In progress: emit next step of current case
        if (_currentXpathCaseIdx >= 0 && static_cast<size_t>(_currentXpathCaseIdx) < _xpathActionCases.size()) {
            XpathCase &c = _xpathActionCases[static_cast<size_t>(_currentXpathCaseIdx)];
            if (!c.activity.empty() && c.activity != activity) {
                _currentXpathCaseIdx = -1;
                _currentXpathStepIdx = 0;
                return nullptr;
            }
            if (_currentXpathStepIdx < 0 || static_cast<size_t>(_currentXpathStepIdx) >= c.steps.size()) {
                _currentXpathCaseIdx = -1;
                _currentXpathStepIdx = 0;
                return nullptr;
            }
            const XpathActionStep &step = c.steps[static_cast<size_t>(_currentXpathStepIdx)];
            ActionType at = stringToActionType(step.actionType);
            if (at == ActionType::ActTypeSize) {
                _currentXpathStepIdx++;
                return getCustomActionFromXpath(activity, rootXML);
            }
            auto customAction = std::make_shared<CustomAction>(at);
            customAction->activity = activity;
            customAction->throttle = (step.throttle > 0) ? step.throttle : c.throttle;
            customAction->waitTime = step.waitTime;
            customAction->allowFuzzing = false;
            if (at == ActionType::BACK) {
                _currentXpathStepIdx++;
                if (static_cast<size_t>(_currentXpathStepIdx) >= c.steps.size()) {
                    _currentXpathCaseIdx = -1;
                    _currentXpathStepIdx = 0;
                }
                return customAction;
            }
            if (step.xpath.empty()) {
                _currentXpathStepIdx++;
                return getCustomActionFromXpath(activity, rootXML);
            }
            XpathPtr xpath = std::make_shared<Xpath>(step.xpath);
            ElementPtr elem = findFirstMatchedElement(xpath, rootXML);
            if (!elem) {
                // Align with original: discard this step, return nullptr so this frame RL runs; next frame continues from next step.
                _currentXpathStepIdx++;
                if (static_cast<size_t>(_currentXpathStepIdx) >= c.steps.size()) {
                    _currentXpathCaseIdx = -1;
                    _currentXpathStepIdx = 0;
                }
                return nullptr;
            }
            RectPtr bounds = elem->getBounds();
            if (bounds && !bounds->isEmpty()) {
                customAction->bounds.resize(4);
                customAction->bounds[0] = static_cast<float>(bounds->left);
                customAction->bounds[1] = static_cast<float>(bounds->top);
                customAction->bounds[2] = static_cast<float>(bounds->right);
                customAction->bounds[3] = static_cast<float>(bounds->bottom);
            }
            if (!step.text.empty()) {
                customAction->text = step.text;
                customAction->clearText = step.clearText;
            }
            _currentXpathStepIdx++;
            if (static_cast<size_t>(_currentXpathStepIdx) >= c.steps.size()) {
                _currentXpathCaseIdx = -1;
                _currentXpathStepIdx = 0;
            }
            return customAction;
        }

        // Not in progress: first-match by config order — first case that matches activity, has remaining times > 0, and passes prob.
        for (size_t i = 0; i < _xpathActionCases.size(); i++) {
            if (static_cast<size_t>(i) >= _xpathCaseRemainingTimes.size() || _xpathCaseRemainingTimes[i] <= 0)
                continue;
            const XpathCase &c = _xpathActionCases[i];
            if (!c.activity.empty() && c.activity != activity) continue;
            float eventRate = randomInt(0, 100) / 100.0f;
            if (eventRate >= c.prob) continue;
            _currentXpathCaseIdx = static_cast<int>(i);
            _currentXpathStepIdx = 0;
            _xpathCaseRemainingTimes[i]--;
            return getCustomActionFromXpath(activity, rootXML);
        }
        return nullptr;
    }

    /**
     * @brief Load white list and black list text files
     * 
     * Loads white list and black list from text files:
     * - White list: /sdcard/awl.strings
     * - Black list: /sdcard/abl.strings
     * 
     * File format: One text per line
     * 
     * @note Currently loaded but not actively used in the codebase.
     *       Kept for future use or backward compatibility.
     */
    /**
     * @brief Load white list and black list text files
     * 
     * Loads white list and black list from text files:
     * - White list: /sdcard/awl.strings
     * - Black list: /sdcard/abl.strings
     * 
     * File format: One text per line
     * 
     * @note Currently loaded but not actively used in the codebase.
     *       Kept for future use or backward compatibility.
     * 
     * @note Performance optimizations:
     *       - Early exit if black list is empty (most common case)
     *       - Reduced logging overhead (only log if content exists)
     */
    void Preference::loadWhiteBlackList() {
        std::string contentBlack = fastbotx::Preference::loadFileContent(BlackListFilePath);
        if (!contentBlack.empty()) {
            std::vector<std::string> texts;
            splitString(contentBlack, texts, '\n');
            this->_blackList.swap(texts);
            BLOG("blacklist:");
            for (const auto &t : this->_blackList) {
                if (!t.empty()) {
                    BLOG("    %s", t.c_str());
                }
            }
        }
        
        std::string contentWhite = fastbotx::Preference::loadFileContent(WhiteListFilePath);
        if (!contentWhite.empty()) {
            std::vector<std::string> textsw;
            splitString(contentWhite, textsw, '\n');
            this->_whiteList.swap(textsw);
            BLOG("whitelist:");
            for (const auto &t : this->_whiteList) {
                if (!t.empty()) {
                    BLOG("    %s", t.c_str());
                }
            }
        }
    }

    /**
     * @brief Load input texts for fuzzing
     * 
     * Loads two types of input texts:
     * 1. Preset texts: /sdcard/max.strings - User-designed input texts
     * 2. Fuzzing texts: /sdcard/max.fuzzing.strings - Random fuzzing texts
     * 
     * File format:
     * - Preset texts: One text per line
     * - Fuzzing texts: One text per line (lines starting with '#' are comments)
     * 
     * These texts are used by patchOperate() to fill in input fields during exploration.
     */
    /**
     * @brief Load input texts for fuzzing
     * 
     * Loads two types of input texts:
     * 1. Preset texts: /sdcard/max.strings - User-designed input texts
     * 2. Fuzzing texts: /sdcard/max.fuzzing.strings - Random fuzzing texts
     * 
     * File format:
     * - Preset texts: One text per line
     * - Fuzzing texts: One text per line (lines starting with '#' are comments)
     * 
     * These texts are used by patchOperate() to fill in input fields during exploration.
     * 
     * @note Performance optimizations:
     *       - Use swap instead of assign for better performance
     *       - Use const reference for line iteration
     *       - Early exit for empty lines and comments
     *       - Pre-filter during split to reduce iterations
     */
    void Preference::loadInputTexts() {
        // Load specified designed text by tester
        std::string content = fastbotx::Preference::loadFileContent(InputTextConfigFilePath);
        if (!content.empty()) {
            std::vector<std::string> texts;
            splitString(content, texts, '\n');
            // Performance: Use swap instead of assign for better performance
            this->_inputTexts.swap(texts);
        }
        
        // Load fuzzing texts
        std::string fuzzContent = fastbotx::Preference::loadFileContent(FuzzingTextsFilePath);
        if (!fuzzContent.empty()) {
            std::vector<std::string> fuzzTexts;
            splitString(fuzzContent, fuzzTexts, '\n');
            
            // Performance: Pre-allocate capacity if we can estimate size
            // Most lines will be valid (not comments), so reserve approximate size
            this->_fuzzingTexts.reserve(fuzzTexts.size());
            
            // Performance: Use const reference to avoid unnecessary string operations
            for (const auto &line: fuzzTexts) {
                // Performance: Early exit for empty lines and comments
                if (line.empty() || line[0] == '#') {
                    continue; // Comment line, skip
                }
                this->_fuzzingTexts.push_back(line);
            }
        }
    }

    /**
     * @brief Load file content into string
     * 
     * Utility function to read a file's entire content into a string.
     * 
     * @param fileAbsolutePath Absolute path to the file
     * 
     * @return std::string - File content, or empty string if file doesn't exist
     * 
     * @note Used by all configuration loading functions
     * @note Logs a warning if file doesn't exist
     */
    /**
     * @brief Load file content into string
     * 
     * Utility function to read a file's entire content into a string.
     * 
     * @param fileAbsolutePath Absolute path to the file
     * 
     * @return std::string - File content, or empty string if file doesn't exist
     * 
     * @note Used by all configuration loading functions
     * @note Logs a warning if file doesn't exist
     * 
     * @note Performance optimizations:
     *       - Uses file size hint to pre-allocate string capacity
     *       - More efficient file reading with size estimation
     */
    std::string Preference::loadFileContent(const std::string &fileAbsolutePath) {
        std::ifstream fileStringReader(fileAbsolutePath, std::ios::binary | std::ios::ate);
        if (!fileStringReader.good()) {
            LOGW("%s not exists!!!", fileAbsolutePath.c_str());
            return std::string();
        }
        
        // Performance: Get file size and pre-allocate string capacity
        // Security: Check for tellg() errors (-1 indicates error)
        std::streamsize fileSize = fileStringReader.tellg();
        if (fileSize <= 0 || fileSize == std::streamsize(-1)) {
            LOGW("Failed to get file size for %s (size: %ld)", fileAbsolutePath.c_str(), 
                 static_cast<long>(fileSize));
            return std::string();
        }
        
        fileStringReader.seekg(0, std::ios::beg);
        // Security: Verify seekg succeeded
        if (!fileStringReader.good()) {
            LOGW("Failed to seek to beginning of file %s", fileAbsolutePath.c_str());
            return std::string();
        }
        std::string retStr;
        retStr.reserve(static_cast<size_t>(fileSize)); // Pre-allocate to avoid reallocations
        
        retStr.assign((std::istreambuf_iterator<char>(fileStringReader)),
                      std::istreambuf_iterator<char>());
        
        return retStr;
    }

    std::string Preference::InvalidProperty = "-f0s^%a@d";
    // static configs for android
    std::string Preference::DefaultResMappingFilePath = "/sdcard/max.mapping";
    std::string Preference::BaseConfigFilePath = "/sdcard/max.config";
    std::string Preference::InputTextConfigFilePath = "/sdcard/max.strings";
    std::string Preference::WhiteListFilePath = "/sdcard/awl.strings";
    std::string Preference::BlackListFilePath = "/sdcard/abl.strings";
    std::string Preference::AvoidRulesFilePath = "/sdcard/max.avoid.rules";
    std::string Preference::XpathActionsFilePath = "/sdcard/max.xpath.actions";
    std::string Preference::ValidTextFilePath = "/sdcard/max.valid.strings";
    std::string Preference::FuzzingTextsFilePath = "/sdcard/max.fuzzing.strings";
    std::string Preference::LlmTaskConfigFilePath = "/sdcard/max.llm.tasks";
    std::string Preference::PackageName;

    /**
     * Replaces ${VAR_NAME} in s with getenv("VAR_NAME"). Unset vars are replaced by empty string.
     * Used when executing an action: LLM may return input text like "${LLM_LOGIN_ACCOUNT}";
     * we substitute in patchOperate() so secrets are never sent to the LLM, only filled at execution.
     */
    static void substituteEnvVars(std::string &s) {
        if (s.empty()) return;
        std::string out;
        out.reserve(s.size());
        for (size_t i = 0; i < s.size(); ) {
            if (s[i] == '$' && i + 2 < s.size() && s[i + 1] == '{') {
                size_t end = s.find('}', i + 2);
                if (end != std::string::npos) {
                    std::string varName = s.substr(i + 2, end - (i + 2));
                    const char *val = std::getenv(varName.c_str());
                    if (val) out.append(val);
                    i = end + 1;
                    continue;
                }
            }
            out.push_back(s[i]);
            ++i;
        }
        s = std::move(out);
    }

    void Preference::loadLlmTasks() {
        BLOG("loading max.llm.tasks: %s", LlmTaskConfigFilePath.c_str());
        std::string fileContent = fastbotx::Preference::loadFileContent(LlmTaskConfigFilePath);
        if (fileContent.empty()) {
            BLOG("max.llm.tasks not found or empty: %s", LlmTaskConfigFilePath.c_str());
            return;
        }

        try {
            ::nlohmann::json tasks = ::nlohmann::json::parse(fileContent);
            _llmTasks.clear();
            _llmTaskRunCount.clear();
            _lastLlmActivity.clear();

            if (!tasks.is_array()) {
                BLOGE("LLM task config is not an array");
                return;
            }

            for (const ::nlohmann::json &task : tasks) {
                auto cfg = std::make_shared<LlmTaskConfig>();
                cfg->activity = getJsonValue<std::string>(task, "activity", "");
                cfg->checkpointXpathString = getJsonValue<std::string>(task, "checkpoint_xpath", "");
                cfg->taskDescription = getJsonValue<std::string>(task, "task_description", "");
                cfg->maxSteps = getJsonValue<int>(task, "max_steps", 10);
                cfg->maxDurationMs = getJsonValue<int>(task, "max_duration_ms", 30000);
                cfg->safeMode = getJsonValue<bool>(task, "safe_mode", false);
                cfg->useLlmForStepSummary = getJsonValue<bool>(task, "use_llm_step_summary", false);
                cfg->usePlannerLayer = getJsonValue<bool>(task, "use_planner", true);
                cfg->maxTimes = getJsonValue<int>(task, "max_times", 0);
                cfg->resetCount = getJsonValue<bool>(task, "reset_count", false);

                ::nlohmann::json forbidden = getJsonValue<::nlohmann::json>(task, "forbidden_texts",
                                                                            ::nlohmann::json::array());
                if (forbidden.is_array()) {
                    for (const auto &v : forbidden) {
                        if (v.is_string()) {
                            cfg->forbiddenTexts.push_back(v.get<std::string>());
                        }
                    }
                }
                
                if (!cfg->checkpointXpathString.empty()) {
                    cfg->checkpointXpath = std::make_shared<Xpath>(cfg->checkpointXpathString);
                }
                
                _llmTasks.push_back(cfg);

                // Pretty, human-friendly summary for each LLM task
                BLOG("\n  LLM task loaded:\n  activity=%s\n  checkpoint_xpath=%s\n  task=%s",
                     cfg->activity.c_str(),
                     cfg->checkpointXpathString.c_str(),
                     cfg->taskDescription.c_str());
            }
            BLOG("loaded max.llm.tasks: %zu tasks", _llmTasks.size());
        } catch (const ::nlohmann::json::exception &ex) {
            BLOGE("parse LLM tasks error happened: id,%d: %s", ex.id, ex.what());
        }
    }

    LlmTaskConfigPtr Preference::matchLlmTask(const std::string &activity,
                                              const ElementPtr &rootXML) {
        if (!rootXML) {
            return nullptr;
        }
        if (_llmTasks.empty()) {
            return nullptr;
        }

        // When activity switched to a different one: clear run counts for tasks (of the previous activity) that have reset_count.
        if (!_lastLlmActivity.empty() && _lastLlmActivity != activity) {
            for (const auto &cfg : _llmTasks) {
                if (!cfg || cfg->activity != _lastLlmActivity || !cfg->resetCount) continue;
                std::string taskKey = cfg->activity + "|" + cfg->checkpointXpathString;
                _llmTaskRunCount[taskKey] = 0;
            }
            BLOG("LLM task run counts cleared for previous activity %s (reset_count)", _lastLlmActivity.c_str());
        }
        _lastLlmActivity = activity;

        // 1) Filter by activity first (no tree walk); only then run checkpoint match on rootXML.
        // Also skip tasks that have already been run maxTimes (when times > 0).
        std::vector<LlmTaskConfigPtr> activityFiltered;
        activityFiltered.reserve(_llmTasks.size());
        for (const auto &cfg : _llmTasks) {
            if (!cfg || !cfg->checkpointXpath) continue;
            if (!cfg->activity.empty() && cfg->activity != activity) continue;
            if (cfg->maxTimes > 0) {
                std::string taskKey = cfg->activity + "|" + cfg->checkpointXpathString;
                int runCount = 0;
                auto it = _llmTaskRunCount.find(taskKey);
                if (it != _llmTaskRunCount.end()) runCount = it->second;
                if (runCount >= cfg->maxTimes) {
                    BDLOG("LLM task skipped (max_times): activity=%s run_count=%d max_times=%d", cfg->activity.c_str(), runCount, cfg->maxTimes);
                    continue;
                }
            }
            activityFiltered.push_back(cfg);
        }
        if (activityFiltered.empty()) {
            return nullptr;
        }

        std::vector<LlmTaskConfigPtr> candidates;
        candidates.reserve(activityFiltered.size());
        for (const auto &cfg : activityFiltered) {
            ElementPtr matched = findFirstMatchedElement(cfg->checkpointXpath, rootXML);
            if (matched) {
                candidates.push_back(cfg);
            }
        }

        if (candidates.empty()) {
            BDLOG("LLM task no match: activity=%s (no checkpoint_xpath matched on current page)", activity.c_str());
            return nullptr;
        }

        int idx = randomInt(0, static_cast<int>(candidates.size()));
        if (idx < 0 || static_cast<size_t>(idx) >= candidates.size()) {
            idx = 0;
        }
        LlmTaskConfigPtr chosen = candidates[static_cast<size_t>(idx)];
        BLOG("LLM task matched: activity=%s checkpoint_xpath=%s", chosen->activity.c_str(), chosen->checkpointXpathString.c_str());
        return chosen;
    }

    bool Preference::canStartLlmTask(const LlmTaskConfigPtr &cfg) const {
        if (!cfg) return false;
        if (cfg->maxTimes <= 0) return true;
        std::string taskKey = cfg->activity + "|" + cfg->checkpointXpathString;
        auto it = _llmTaskRunCount.find(taskKey);
        int count = (it != _llmTaskRunCount.end()) ? it->second : 0;
        return count < cfg->maxTimes;
    }

    void Preference::incrementLlmTaskRunCount(const LlmTaskConfigPtr &cfg) {
        if (!cfg) return;
        std::string taskKey = cfg->activity + "|" + cfg->checkpointXpathString;
        _llmTaskRunCount[taskKey]++;
        BLOG("LLM task session started: activity=%s checkpoint_xpath=%s (run %d, max_times %d)", cfg->activity.c_str(), cfg->checkpointXpathString.c_str(), _llmTaskRunCount[taskKey], cfg->maxTimes);
    }

    bool Preference::useStaticReuseAbstraction() const {
        return _useStaticReuseAbstraction;
    }

} // namespace fastbotx
