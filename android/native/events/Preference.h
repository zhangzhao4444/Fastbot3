/*
 * This code is licensed under the Fastbot license. You may obtain a copy of this license in the LICENSE.txt file in the root directory of this source tree.
 */
/**
 * @authors Jianqiang Guo, Yuhui Su, Zhao Zhang
 */
#ifndef Preference_H_
#define Preference_H_

#include <string>
#include <map>
#include <vector>
#include <deque>
#include <queue>
#include "Base.h"
#include "Action.h"
#include "DeviceOperateWrapper.h"
#include "Element.h"
#include "../llm/LlmTypes.h"


namespace fastbotx {

    /// The class for describing the actions that user specified in preference file
    class CustomAction : public Action {
    public:
        OperatePtr toOperate() const override;

        CustomAction();

        explicit CustomAction(ActionType act);

        XpathPtr xpath;
        std::string resourceID;
        std::string contentDescription;
        std::string text;
        std::string classname;
        std::string clickable;  ///< "true" | "false" | "" (skip); used by tree pruning only
        std::string activity;
        std::string command;
        std::vector<float> bounds;
        bool allowFuzzing{true};
        bool clearText{};
        int throttle{};
        int waitTime{};

        ~CustomAction() override = default;
    };

    typedef std::shared_ptr<CustomAction> CustomActionPtr;
    typedef std::vector<CustomActionPtr> CustomActionPtrVec;

    typedef std::map<std::string, std::vector<RectPtr>> StringRectsMap;

    /// Unified avoidance rule: avoid (delete + rect cache) or modify (property overrides only).
    struct AvoidRule {
        std::string activity;
        XpathPtr xpath;
        std::vector<float> bounds;  // size 4 when present
        enum class Action { Avoid, Modify };
        Action action{Action::Avoid};
        std::string resourceID;
        std::string text;
        std::string contentDescription;
        std::string classname;
        std::string clickable;
    };
    typedef std::shared_ptr<AvoidRule> AvoidRulePtr;
    typedef std::vector<AvoidRulePtr> AvoidRulePtrVec;

    /// One step in a max.xpath.actions case: xpath locator + action type + optional text (for CLICK input).
    struct XpathActionStep {
        std::string xpath;
        std::string actionType;  // CLICK, LONG_CLICK, BACK, SCROLL_TOP_DOWN, etc.
        std::string text;       // optional; for CLICK when input is needed
        bool clearText{false};
        int throttle{0};        // per-step override; 0 = use case throttle
        int waitTime{0};        // wait after this step (ms)
    };
    /// One case in max.xpath.actions: activity + prob + throttle + sequence of steps.
    struct XpathCase {
        std::string activity;
        float prob{1.0f};        // 0..1 or 1 = 100%
        int times{1};
        int throttle{0};
        std::vector<XpathActionStep> steps;
    };

    class Preference {
    public:
        Preference();

        static std::shared_ptr<Preference> inst();

        /**
         * Resolve the page: apply black widgets, tree pruning, valid texts, etc. to the element tree.
         * Call this before using the element for state/LLMTaskAgent when custom actions (max.xpath.actions) are not used.
         */
        void resolvePage(const std::string &activity, const ElementPtr &rootXML);

        //@brief patch operate: 1. fuzz input text 2. ..
        void patchOperate(const OperatePtr &opt);

        // load resource mapping file, override the mapings from default file max.mapping,
        void loadMixResMapping(const std::string &resourceMappingPath);

        // load label, text, button valid text dumped from apk
        void loadValidTexts(const std::string &pathOfValidTexts);

        bool checkPointIsInBlackRects(const std::string &activity, int pointX, int pointY);

        void setListenMode(bool listen);

        bool skipAllActionsFromModel() const { return this->_skipAllActionsFromModel; }

        /**
         * @brief Whether to use legacy static reuse state abstraction instead of dynamic abstraction.
         * Controlled via max.config key: max.staticStateAbstraction=true|false.
         */
        bool useStaticReuseAbstraction() const;

        bool isForceUseTextModel() const { return this->_forceUseTextModel; }

        int getForceMaxBlockStateTimes() const { return this->_forceMaxBlockStateTimes; }

        /**
         * Get runtime LLM configuration (OpenAI-compatible HTTP endpoint).
         */
        const LlmRuntimeConfig &getLlmRuntimeConfig() const { return this->_llmRuntimeConfig; }

        /**
         * Load LLM task configurations from external file.
         * This is typically called from loadConfigs().
         */
        void loadLlmTasks();

        /**
         * Match all configured LLM tasks for the current activity and page,
         * then randomly pick one whose checkpoint XPath matches the given root XML.
         *
         * @param activity Current activity name.
         * @param rootXML  Current page root element.
         * @return A matched LlmTaskConfigPtr or nullptr if no task matches.
         */
        LlmTaskConfigPtr matchLlmTask(const std::string &activity,
                                      const ElementPtr &rootXML);

        /** True iff this task is still allowed to start (run count < max_times when max_times > 0). */
        bool canStartLlmTask(const LlmTaskConfigPtr &cfg) const;
        /** Call when an LLM session is actually started for this task (so max_times is counted per session start, not per match). */
        void incrementLlmTaskRunCount(const LlmTaskConfigPtr &cfg);

        /**
         * Get the next custom action from max.xpath.actions for the current activity and page.
         * When a case is in progress, returns the next step (element located by xpath); otherwise
         * may start a new case with probability prob. BACK/SCROLL do not require xpath match.
         */
        ActionPtr getCustomActionFromXpath(const std::string &activity,
                                          const ElementPtr &rootXML);

        ~Preference();

    protected:

        void deMixResMapping(const ElementPtr &rootXML);

        /// Single-pass resolve: avoid (delete + rect cache) + modify (property overrides), deMixResMapping, cachePageTexts, pruningValidTexts, then recurse.
        void resolveElementWithAvoid(const ElementPtr &element, const std::string &activity);

        void pruningValidTexts(const ElementPtr &element);

        // recursive
        void
        findMatchedElements(std::vector<ElementPtr> &outElements, const XpathPtr &xpathSelector,
                            const ElementPtr &elementXML);

        // Performance optimization: Find first matched element (early termination)
        // Returns first matching element or nullptr if none found
        ElementPtr findFirstMatchedElement(const XpathPtr &xpathSelector,
                                          const ElementPtr &elementXML);

        void cachePageTexts(const ElementPtr &rootElement);

        void loadConfigs();

        void loadBaseConfig();

        void loadAvoidRules();

        void loadXpathActions();

        void loadWhiteBlackList();

        void loadInputTexts();

    private:

        static std::shared_ptr<Preference> _preferenceInst;

        std::vector<std::string> _whiteList;
        std::vector<std::string> _blackList;

        std::vector<std::string> _inputTexts;
        std::vector<std::string> _fuzzingTexts;
        std::deque<std::string> _pageTextsCache;

        AvoidRulePtrVec _avoidRules;
        std::map<std::string, AvoidRulePtrVec> _avoidRulesByActivity;

        std::map<std::string, std::string> _resMapping;
        std::map<std::string, std::string> _resMixedMapping;

        bool _randomInputText;
        bool _doInputFuzzing;

        std::set<std::string> _validTexts;
        bool _pruningValidTexts;
        bool _skipAllActionsFromModel;
        bool _useStaticReuseAbstraction{};
        bool _forceUseTextModel{};
        int _forceMaxBlockStateTimes{};
        RectPtr _rootScreenSize;

        /// LLM task configurations loaded from external file.
        std::vector<LlmTaskConfigPtr> _llmTasks;
        /// Per-task run count (key = activity + "|" + checkpointXpathString) for maxTimes limit.
        std::map<std::string, int> _llmTaskRunCount;
        /// Last activity passed to matchLlmTask; used to detect activity switch and clear run counts when resetCount is true.
        std::string _lastLlmActivity;

        /// Runtime LLM HTTP configuration loaded from base config.
        LlmRuntimeConfig _llmRuntimeConfig;

        /// max.xpath.actions: cases and current execution state.
        std::vector<XpathCase> _xpathActionCases;
        /// Remaining trigger count per case (times in config); when 0 the case is not chosen.
        std::vector<int> _xpathCaseRemainingTimes;
        int _currentXpathCaseIdx{-1};
        int _currentXpathStepIdx{0};

        static std::string loadFileContent(const std::string &fileAbsolutePath);

        StringRectsMap _cachedBlackWidgetRects;
        /// Bounds-only avoid rects for current resolvePage(activity), used to delete elements whose center is inside.
        std::vector<RectPtr> _currentBoundsOnlyAvoidRects;

    public:
        static std::string InvalidProperty;
        // static configs for android
        static std::string DefaultResMappingFilePath;
        static std::string BaseConfigFilePath;      // /sdcard/max.config
        static std::string InputTextConfigFilePath; // /sdcard/max.strings
        static std::string LlmTaskConfigFilePath;   // /sdcard/max.llm.tasks
        static std::string WhiteListFilePath;       // /sdcard/awl.strings
        static std::string BlackListFilePath;       // /sdcard/abl.strings
        static std::string AvoidRulesFilePath;      // /sdcard/max.avoid.rules (unified black + pruning)
        static std::string XpathActionsFilePath;    // /sdcard/max.xpath.actions (custom event sequence)
        static std::string ValidTextFilePath;       // /sdcard/max.valid.strings
        static std::string FuzzingTextsFilePath;    // /sdcard/max.fuzzing.strings
        static std::string PackageName;
    };

    typedef std::shared_ptr<Preference> PreferencePtr;

};

#endif //Preference_H_
