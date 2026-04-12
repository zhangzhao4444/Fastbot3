/*
 * This code is licensed under the Fastbot license. You may obtain a copy of this license in the LICENSE.txt file in the root directory of this source tree.
 */
/**
 * @authors Jianqiang Guo, Yuhui Su, Zhao Zhang, Tianming Liu, Chenxu Wang
 */

#ifndef AbstractAgent_CPP_
#define AbstractAgent_CPP_

#include "AbstractAgent.h"

#include "GPTAgent.h"
#include "CodeCoverageMonitor.h"
#include <utility>
#include "../model/Model.h"
#include "../desc/MergedState.h"
#include "../desc/reuse/ReuseState.h"
#include "../events/Preference.h"
#include "LLMTaskAgent.h"
#include "../llm/LlmJavaHttp.h"
#include <cinttypes>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <future>
#include "../storage/PreconditionModel_generated.h"

namespace fastbotx {

    namespace GuideConstants {
        constexpr double kMinCandidateScore = 0.3;
        constexpr double kMinPageScore = 0.1;
        constexpr double kMaxPageScore = 2.0;
        constexpr int kMaxTemplateRewardPerEpisode = 10;
        constexpr int kMaxTemplateSelectionPerEpisode = 10;
        constexpr int kMaxPageSelectionPerEpisode = 20;
        constexpr int kFailBlacklistThreshold = 3;
        constexpr int kNoProgressThreshold = 3;
        constexpr int kSystemFailStopThreshold = 20;
        constexpr size_t kRecentActionHistoryCap = 32;
    }

    enum class LlmdroidMode { EXPLORE, NAVIGATE, TEST_FUNCTION };

    struct LlmdroidAgentOverlay {
        GraphPtr graph;
        MergedStateGraphPtr mergedStateGraph;
        std::unique_ptr<GPTAgent> gptAgent;
        int totalMergedState{0};
        double startTime{0};
        double nextStageTime{0};
        double exploreWindowMs{120000.0};
        ActivityStateActionPtr mCurrentAction;
        ReuseStatePtr mCurrentState;

        /// same window/threshold defaults as LLMDroid AbstractAgent.
        CodeCoverageMonitor cvMonitor{80, 0.05, 1.0};
        static constexpr int kRateCapacity = 80;
        std::vector<double> growthRateWindow;
        double currentThreshold{0.05};
        bool shouldWait{false};
        size_t exploreWindowStartActivityCoverage{0};

        LlmdroidMode mode{LlmdroidMode::EXPLORE};
        Path currentPath;
        std::vector<Path> paths;
        int guideTarget{-1};
        int guideTime{0};
        int successGuideTime{0};
        int totalGuideTime{0};
        int executedSteps{0};
        float currentSimilarityCheck{0.6f};
        static constexpr float kMaxSimilarity = 0.6f;
        static constexpr float kMinSimilarity = 0.49f;
        ActivityStateActionPtr actionByGpt;
        std::future<int> futureInt;
        std::future<ActivityStateActionPtr> futureAction;
    };

    namespace {

        struct GuideChoice {
            ActionPtr action;
            uint64_t pageHash{0};
            int templateIndex{-1};
            int pos{-1};
            double score{-1.0};
        };

        inline bool isValidGuideActionHash(uint64_t h) {
            return h != 0;
        }

        inline void clampTemplateLength(int *len) {
            if (!len) return;
            if (*len < 0) *len = 0;
            if (*len > AbstractAgent::MAX_TEMPLATE_SEQUENCE_LEN) {
                *len = AbstractAgent::MAX_TEMPLATE_SEQUENCE_LEN;
            }
        }

        // Backward-compatible parser for legacy custom-binary .precond (PCTL v2)
        bool parseLegacyPreconditionBinaryV2(
                const std::vector<uint8_t> &buf,
                std::unordered_map<uint64_t, AbstractAgent::PreconditionInfo> &out) {
            auto readBytes = [&](size_t &off, void *dst, size_t n) -> bool {
                if (off + n > buf.size()) return false;
                std::memcpy(dst, buf.data() + off, n);
                off += n;
                return true;
            };

            size_t off = 0;
            char magic[4] = {0, 0, 0, 0};
            uint32_t version = 0;
            uint32_t entryCount = 0;
            if (!readBytes(off, magic, sizeof(magic))) return false;
            if (!readBytes(off, &version, sizeof(version))) return false;
            if (!readBytes(off, &entryCount, sizeof(entryCount))) return false;
            if (magic[0] != 'P' || magic[1] != 'C' || magic[2] != 'T' || magic[3] != 'L') return false;
            if (version != 2) return false;

            std::unordered_map<uint64_t, AbstractAgent::PreconditionInfo> loaded;
            for (uint32_t e = 0; e < entryCount; ++e) {
                uint64_t pageHash = 0;
                double score = 1.0;
                uint32_t tcount = 0;
                if (!readBytes(off, &pageHash, sizeof(pageHash))) return false;
                if (!readBytes(off, &score, sizeof(score))) return false;
                if (!readBytes(off, &tcount, sizeof(tcount))) return false;

                AbstractAgent::PreconditionInfo info;
                info.score = std::max(GuideConstants::kMinPageScore, std::min(score, GuideConstants::kMaxPageScore));
                info.templateCount = std::min(static_cast<int>(tcount), AbstractAgent::MAX_TEMPLATE_SEQUENCE_LEN);

                for (int i = 0; i < AbstractAgent::MAX_TEMPLATE_SEQUENCE_LEN; ++i) {
                    uint32_t tlen = 0;
                    AbstractAgent::GuidancePathTemplate tpl;
                    if (!readBytes(off, &tlen, sizeof(tlen))) return false;
                    if (!readBytes(off, tpl.sequence.data(), sizeof(uint64_t) * AbstractAgent::MAX_TEMPLATE_SEQUENCE_LEN)) return false;
                    if (!readBytes(off, tpl.reliability.data(), sizeof(double) * AbstractAgent::MAX_TEMPLATE_SEQUENCE_LEN)) return false;
                    tpl.length = static_cast<int>(tlen);
                    clampTemplateLength(&tpl.length);
                    for (int p = tpl.length; p < AbstractAgent::MAX_TEMPLATE_SEQUENCE_LEN; ++p) {
                        tpl.sequence[p] = 0;
                        tpl.reliability[p] = 0.0;
                    }
                    if (i < info.templateCount) {
                        info.templates[i] = tpl;
                    }
                }

                loaded[pageHash] = info;
            }

            out.swap(loaded);
            return true;
        }

        void llmdroidResetFuture(LlmdroidAgentOverlay &L) {
            if (!L.gptAgent) {
                return;
            }
            PromiseIntPtr promInt = std::make_shared<std::promise<int>>();
            PromiseActionPtr promAction = std::make_shared<std::promise<ActivityStateActionPtr>>();
            L.futureInt = promInt->get_future();
            L.futureAction = promAction->get_future();
            L.gptAgent->resetPromise(std::move(promInt), std::move(promAction));
        }

        void llmdroidDebugMergedStates(LlmdroidAgentOverlay & /*L*/) {
            BLOG("LLMDroid: debugMergedStates (file dump skipped)");
        }

        void llmdroidPrepareBackToExplore(LlmdroidAgentOverlay &L, AbstractAgent & /*agent*/) {
            BLOG("LLMDroid: prepareBackToExplore");
            L.mode = LlmdroidMode::EXPLORE;
            L.nextStageTime = L.exploreWindowMs + currentStamp();
            if (L.graph) {
                L.exploreWindowStartActivityCoverage = L.graph->getVisitedActivities().size();
            }
            L.growthRateWindow.clear();
            L.shouldWait = false;
            L.guideTarget = -1;
            L.paths.clear();
            L.guideTime = 0;
            L.currentSimilarityCheck = LlmdroidAgentOverlay::kMaxSimilarity;
            L.executedSteps = 0;
            L.actionByGpt.reset();
            if (L.gptAgent) {
                L.gptAgent->addTestedFunction();
                L.gptAgent->clearExecutedEvents();
            }
            if (!L.mergedStateGraph || !L.gptAgent) {
                return;
            }
            for (const MergedStatePtr &ms : L.mergedStateGraph->getMergedStates()) {
                if (ms && ms->needReanalysed()) {
                    QuestionPayload qp;
                    qp.type = AskModel::REANALYSIS;
                    qp.from = ms;
                    L.gptAgent->pushStateToQueue(std::move(qp));
                }
            }
        }

        void llmdroidOnNavigationOver(LlmdroidAgentOverlay &L, bool success, AbstractAgent &agent) {
            if (success) {
                L.successGuideTime++;
                L.mode = LlmdroidMode::TEST_FUNCTION;
                BLOG("LLMDroid: navigation success -> TEST_FUNCTION");
            } else {
                llmdroidPrepareBackToExplore(L, agent);
            }
            BLOG("LLMDroid: guide stat %d/%d", L.successGuideTime, L.totalGuideTime);
            L.guideTarget = -1;
            L.paths.clear();
            L.guideTime = 0;
            L.currentSimilarityCheck = LlmdroidAgentOverlay::kMaxSimilarity;
        }

        void llmdroidPrepareForNavigation(LlmdroidAgentOverlay &L, const ModelPtr &model, AbstractAgent &agent);

        void llmdroidOnNavigationFailed(LlmdroidAgentOverlay &L, const ModelPtr &model, AbstractAgent &agent) {
            BLOG("LLMDroid: onNavigationFailed guideTime=%d", L.guideTime);
            if (L.guideTime > 1 && L.currentSimilarityCheck > LlmdroidAgentOverlay::kMinSimilarity) {
                L.currentSimilarityCheck -= 0.05f;
            }
            if (!L.paths.empty()) {
                L.currentPath = L.paths[0];
                L.paths.erase(L.paths.begin());
                return;
            }
            if (L.guideTime < 3) {
                if (L.gptAgent) {
                    L.gptAgent->addTestedFunction();
                }
                llmdroidPrepareForNavigation(L, model, agent);
                return;
            }
            llmdroidOnNavigationOver(L, false, agent);
        }

        void llmdroidPrepareForNavigation(LlmdroidAgentOverlay &L, const ModelPtr &model, AbstractAgent &agent) {
            (void)model;
            (void)agent;
            if (!L.graph || !L.gptAgent) {
                return;
            }
            L.mode = LlmdroidMode::NAVIGATE;
            L.gptAgent->waitUntilQueueEmpty();
            llmdroidDebugMergedStates(L);
            L.guideTime++;
            L.totalGuideTime++;
            llmdroidResetFuture(L);
            QuestionPayload qp;
            qp.type = AskModel::GUIDE;
            L.gptAgent->pushStateToQueue(std::move(qp));
            L.guideTarget = L.futureInt.get();
            BLOG("LLMDroid: guide target ReuseState id=%d", L.guideTarget);
            if (L.guideTarget < 0) {
                llmdroidOnNavigationFailed(L, model, agent);
                return;
            }
            L.paths = L.graph->findPath(L.guideTarget, true);
            if (L.paths.empty()) {
                BLOG("LLMDroid: no path to ReuseState %d", L.guideTarget);
                llmdroidOnNavigationFailed(L, model, agent);
            } else {
                L.currentPath = L.paths[0];
                L.paths.erase(L.paths.begin());
            }
        }

        int llmdroidGuideCheck(LlmdroidAgentOverlay &L) {
            bool isCorrect = false;
            int targetId = -1;
            ReuseStatePtr mcs = L.mCurrentState;
            if (!mcs) {
                return 3;
            }
            while (!L.currentPath.steps.empty()) {
                Step currentStep = L.currentPath.steps.front();
                targetId = currentStep.node;
                L.currentPath.steps.pop();
                if (mcs->getIdi() == targetId) {
                    isCorrect = true;
                    break;
                }
                if (currentStep.action && currentStep.action->getActionType() == ActionType::RESTART) {
                    if (L.currentPath.steps.empty()) {
                        isCorrect = true;
                        break;
                    }
                    ActionPtr replace = mcs->findSimilarAction(L.currentPath.steps.front().action);
                    if (replace) {
                        ActivityStateActionPtr tmp = std::dynamic_pointer_cast<ActivityStateAction>(replace);
                        L.currentPath.steps.front().action =
                            tmp ? std::static_pointer_cast<Action>(std::make_shared<ActivityStateAction>(*tmp)) : replace;
                        isCorrect = true;
                        break;
                    }
                } else if (L.graph) {
                    ReuseStatePtr targetState = L.graph->findReuseStateById(targetId);
                    const float sim = targetState ? mcs->computeSimilarity(targetState) : 0.f;
                    BLOG("LLMDroid guideCheck sim target R%d now R%d -> %f", targetId, mcs->getIdi(), sim);
                    if (sim > L.currentSimilarityCheck) {
                        if (L.currentPath.steps.empty()) {
                            isCorrect = true;
                            break;
                        }
                        ActionPtr replace = mcs->findSimilarAction(L.currentPath.steps.front().action);
                        if (replace) {
                            ActivityStateActionPtr tmp = std::dynamic_pointer_cast<ActivityStateAction>(replace);
                            L.currentPath.steps.front().action =
                                tmp ? std::static_pointer_cast<Action>(std::make_shared<ActivityStateAction>(*tmp))
                                    : replace;
                            isCorrect = true;
                            break;
                        }
                    }
                }
            }
            if (isCorrect) {
                return L.currentPath.steps.empty() ? 2 : 1;
            }
            BLOG("LLMDroid guideCheck failed target=%d now=%d", targetId, mcs->getIdi());
            return 3;
        }

        void llmdroidPrepareTestFunction(LlmdroidAgentOverlay &L) {
            if (!L.gptAgent || !L.mCurrentState) {
                return;
            }
            if (L.executedSteps < 5) {
                L.executedSteps++;
                llmdroidResetFuture(L);
                QuestionPayload qp;
                qp.type = AskModel::TEST_FUNCTION;
                qp.reuseState = L.mCurrentState;
                L.gptAgent->pushStateToQueue(std::move(qp));
                L.actionByGpt = L.futureAction.get();
            } else {
                L.actionByGpt.reset();
                BLOG("LLMDroid: TEST_FUNCTION step cap reached");
            }
        }

        MergedStatePtr findMostSimilarReuse(LlmdroidAgentOverlay &L, const ReuseStatePtr &state) {
            constexpr float kThreshold = 0.6f;
            MergedStatePtr origin = state->getMergedState();
            if (origin) {
                return origin;
            }
            MergedStatePtr current = L.mergedStateGraph->getCurrentNode();
            if (!current) {
                return nullptr;
            }
            ReuseStatePtr rootState = current->getRootState();
            if (!rootState) {
                return nullptr;
            }
            const float similarity = rootState->computeSimilarityForMergedState(state);
            if (similarity < kThreshold) {
                float maxS = 0.f;
                MergedStatePtr best;
                for (const MergedStatePtr &ms : L.mergedStateGraph->getMergedStates()) {
                    if (!ms) {
                        continue;
                    }
                    ReuseStatePtr r = ms->getRootState();
                    if (!r) {
                        continue;
                    }
                    const float s = r->computeSimilarityForMergedState(state);
                    if (s > kThreshold && s > maxS) {
                        maxS = s;
                        best = ms;
                    }
                }
                return best;
            }
            return current;
        }

        void llmdroidSwitchMode(LlmdroidAgentOverlay &L, const ModelPtr &model, AbstractAgent &agent) {
            const double now = currentStamp();

            if (L.mode == LlmdroidMode::EXPLORE) {
                const bool useCoverageMode = isLlmdroidExternalCoverageEnabledFromJava();
                if (useCoverageMode) {
                    const double javaCoverageMetric = getLlmdroidCoverageFromJava();
                    const auto res = L.cvMonitor.update(javaCoverageMetric);
                    L.currentThreshold = res.second;
                    L.growthRateWindow.push_back(res.first);
                    if (static_cast<int>(L.growthRateWindow.size()) > LlmdroidAgentOverlay::kRateCapacity) {
                        L.growthRateWindow.erase(L.growthRateWindow.begin());
                    }

                    if (static_cast<int>(L.growthRateWindow.size()) == LlmdroidAgentOverlay::kRateCapacity) {
                        bool pass = false;
                        for (double d : L.growthRateWindow) {
                            if (d > L.currentThreshold) {
                                pass = true;
                                break;
                            }
                        }
                        if (!pass) {
                            L.shouldWait = true;
                            BLOG("LLMDroid: stagnation by coverage window (threshold=%f)", L.currentThreshold);
                        }
                    }
                } else {
                    // Time mode: switch only when activity coverage has not increased within one explore window.
                    size_t currentActivityCoverage = 0;
                    if (L.graph) {
                        currentActivityCoverage = L.graph->getVisitedActivities().size();
                    }
                    if (currentActivityCoverage > L.exploreWindowStartActivityCoverage) {
                        BLOG("LLMDroid: time window coverage increased %zu -> %zu, extend explore window",
                             L.exploreWindowStartActivityCoverage, currentActivityCoverage);
                        L.exploreWindowStartActivityCoverage = currentActivityCoverage;
                        L.nextStageTime = now + L.exploreWindowMs;
                    } else if (now > L.nextStageTime) {
                        L.shouldWait = true;
                        BLOG("LLMDroid: switch by time window (activity coverage no increase: %zu)",
                             currentActivityCoverage);
                    }
                }

                if (L.shouldWait) {
                    L.shouldWait = false;
                    llmdroidPrepareForNavigation(L, model, agent);
                    L.nextStageTime = now + L.exploreWindowMs;
                    if (L.graph) {
                        L.exploreWindowStartActivityCoverage = L.graph->getVisitedActivities().size();
                    }
                    L.growthRateWindow.clear();
                    return;
                }
            }

            if (L.mode == LlmdroidMode::NAVIGATE) {
                const int st = llmdroidGuideCheck(L);
                if (st == 1) {
                    return;
                }
                if (st == 2) {
                    llmdroidOnNavigationOver(L, true, agent);
                    return;
                }
                else{
                    llmdroidOnNavigationFailed(L, model, agent);
                    return;
                }
            }

            if (L.mode == LlmdroidMode::TEST_FUNCTION) {
                llmdroidPrepareTestFunction(L);
            }
        }

    } // namespace

    /**
     * @brief Default constructor
     * 
     * Initializes all member variables to default values:
     * - Uses validDatePriorityFilter as default filter
     * - All counters initialized to 0
     * - Boolean flags initialized to false
     * - Algorithm type defaults to Random
     */
    AbstractAgent::AbstractAgent()
            : _validateFilter(validDatePriorityFilter), _graphStableCounter(0),
              _stateStableCounter(0), _activityStableCounter(0), _disableFuzz(false),
              _requestRestart(false), _currentStateBlockTimes(0),
              _algorithmType(AlgorithmType::Random) {

    }

    /**
     * @brief Constructor with model parameter
     * 
     * First calls default constructor for initialization, then sets model pointer.
     * 
     * @param model Model smart pointer
     */
    AbstractAgent::AbstractAgent(const ModelPtr &model)
            : AbstractAgent() {
        this->_model = model;
        // Avoid virtual call from constructor; initialize guide runtime counters directly.
        _coveredThisEpisode.clear();
        _consecutiveFails.clear();
        _noProgressCount.clear();
        _lastPosition.clear();
        _templateRewardCounts.clear();
        _pageSelectionCounts.clear();
        _templateSelectionCounts.clear();
        _guideRecentActionHashes.clear();
        _hasPendingGuideCheck = false;
        _pendingGuideActionHash = 0;
        _pendingGuideTargetPage = 0;
        _pendingGuideTemplateIndex = -1;
        _pendingGuidePosition = -1;
        _preconditionReachedSinceLastGuide = false;
        _pendingGuideRewardApplied = false;
        _guideSystemFailureCount = 0;
    }

    /**
     * @brief Destructor
     * 
     * Explicitly resets all smart pointers to ensure proper resource cleanup.
     * Note: Smart pointers automatically manage memory, explicit reset here is for code clarity.
     */
    AbstractAgent::~AbstractAgent() {
        _llmdroid.reset();
        this->_model.reset();
        this->_lastState.reset();
        this->_currentState.reset();
        this->_newState.reset();
        this->_lastAction.reset();
        this->_currentAction.reset();
        this->_newAction.reset();
        this->_validateFilter.reset();
    }

    void AbstractAgent::ensureLlmdroidRuntime() {
        if (_llmdroid) {
            return;
        }
        const PreferencePtr pref = Preference::inst();
        if (!pref || !pref->isLlmdroidEnabled()) {
            return;
        }
        const ModelPtr model = this->_model.lock();
        if (!model) {
            return;
        }
        _llmdroid = std::make_unique<LlmdroidAgentOverlay>();
        _llmdroid->graph = model->getGraph();
        _llmdroid->mergedStateGraph = std::make_shared<MergedStateGraph>(_llmdroid->graph);
        std::shared_ptr<LlmClient> llm = model->getLlmClient();
        std::string startPrompt = "I'm testing an Android app.\n";
        _llmdroid->gptAgent =
                std::make_unique<GPTAgent>(_llmdroid->mergedStateGraph, std::move(llm), std::move(startPrompt));
        _llmdroid->startTime = currentStamp();
        const int exploreWindowSec = pref->getLlmdroidExploreWindowSec();
        _llmdroid->exploreWindowMs = static_cast<double>(exploreWindowSec) * 1000.0;
        _llmdroid->nextStageTime = _llmdroid->startTime + _llmdroid->exploreWindowMs;
        _llmdroid->exploreWindowStartActivityCoverage =
                _llmdroid->graph ? _llmdroid->graph->getVisitedActivities().size() : 0;
        BLOG("LLMDroid: mode source=%s",
             isLlmdroidExternalCoverageEnabledFromJava() ? "external-coverage(jacoco/androlog)" : "time-mode");
        BLOG("LLMDroid: time-mode explore window configured to %d sec", exploreWindowSec);
    }

    void AbstractAgent::processState(const ReuseStatePtr &state) {
        if (!state) {
            return;
        }
        const auto pref = Preference::inst();
        if (!pref || !pref->isLlmdroidEnabled()) {
            return;
        }
        const ModelPtr model = this->_model.lock();
        if (!model) {
            return;
        }

        ensureLlmdroidRuntime();
        if (!_llmdroid || !_llmdroid->mergedStateGraph || !_llmdroid->gptAgent) {
            return;
        }

        LlmdroidAgentOverlay &L = *_llmdroid;
        L.mCurrentState = state;
        L.mCurrentAction = _currentAction;

        MergedStatePtr mergedState = findMostSimilarReuse(L, state);
        if (mergedState) {
            state->setMergedState(mergedState);
            if (mergedState == L.mergedStateGraph->getCurrentNode()) {
                mergedState->addState(state, L.mCurrentAction, false, false);
            } else {
                MergedStatePtr preState = L.mergedStateGraph->getCurrentNode();
                if (preState) {
                    preState->addState(state, L.mCurrentAction, false, true);
                }
                mergedState->addState(state, L.mCurrentAction, true, false);
            }
        } else {
            MergedStatePtr preState = L.mergedStateGraph->getCurrentNode();
            if (preState) {
                preState->addState(state, L.mCurrentAction, false, true);
            }
            mergedState = std::make_shared<MergedState>(state, L.totalMergedState);
            L.totalMergedState++;
            state->setMergedState(mergedState);
            const stringPtr actStr = state->getActivityString();
            if (actStr && actStr.get() && actStr->find("com.android.") == std::string::npos) {
                QuestionPayload qp;
                qp.type = AskModel::STATE_OVERVIEW;
                qp.from = mergedState;
                L.gptAgent->pushStateToQueue(std::move(qp));
            }
        }

        L.mergedStateGraph->addNode(mergedState, L.mCurrentAction, false);
        llmdroidSwitchMode(L, model, *this);
    }

    /**
     * @brief Callback when a new node is added to the state graph
     * 
     * Implements GraphListener interface. Called when Graph adds a new state.
     * 
     * Functionality:
     * 1. Updates _newState to the newly added node
     * 2. If state blocking detection is enabled (BLOCK_STATE_TIME_RESTART != -1),
     *    detects if same state is reached consecutively, increments block counter if so
     * 
     * @param node Newly added state node
     */
    void AbstractAgent::onAddNode(StatePtr node) {
        _newState = node;

        // If state blocking detection is enabled, check if stuck in loop
#if BLOCK_STATE_TIME_RESTART != -1
            if (equals(_newState, _currentState)) {
                // Consecutively reached same state, increment block count
                this->_currentStateBlockTimes++;
            } else {
                // Reached new state, reset block count
                this->_currentStateBlockTimes = 0;
            }
#endif
    }

    /**
     * @brief Move forward in state machine
     * 
     * Updates state and action history, implementing state machine state transition.
     * 
     * State update flow:
     * - _lastState = _currentState (save previous state)
     * - _currentState = _newState (current state updated to new state)
     * - _newState = nextState (new state updated to next state)
     * 
     * Action update flow:
     * - _lastAction = _currentAction (save previous action)
     * - _currentAction = _newAction (current action updated to newly selected action)
     * - _newAction = nullptr (clear new action, wait for next selection)
     * 
     * @param nextState Next state, uses move semantics to avoid unnecessary copies
     */
    void AbstractAgent::moveForward(StatePtr nextState) {
        // Update state history
        _lastState = _currentState;
        _currentState = _newState;
        _newState = std::move(nextState);  // Use move to avoid copy
        
        // Update action history
        _lastAction = _currentAction;
        _currentAction = _newAction;
        _newAction = nullptr;  // Clear new action, wait for next selection

        // [GUIDE] Record executed action hash for template generation (old -> new order).
        if (_currentAction) {
            const uint64_t h = static_cast<uint64_t>(_currentAction->hash());
            if (isValidGuideActionHash(h)) {
                _guideRecentActionHashes.push_back(h);
                if (_guideRecentActionHashes.size() > GuideConstants::kRecentActionHistoryCap) {
                    _guideRecentActionHashes.erase(_guideRecentActionHashes.begin());
                }
            }
            if (_currentAction->getActionType() == ActionType::RESTART) {
                BLOG("[GUIDE] beginNewEpisode trigger: RESTART action");
                beginNewEpisode();
            }
        }
    }

    void AbstractAgent::beginNewEpisode() {
        _coveredThisEpisode.clear();
        _consecutiveFails.clear();
        _noProgressCount.clear();
        _lastPosition.clear();
        _templateRewardCounts.clear();
        _pageSelectionCounts.clear();
        _templateSelectionCounts.clear();
        _guideRecentActionHashes.clear();
        _hasPendingGuideCheck = false;
        _pendingGuideActionHash = 0;
        _pendingGuideTargetPage = 0;
        _pendingGuideTemplateIndex = -1;
        _pendingGuidePosition = -1;
        _preconditionReachedSinceLastGuide = false;
        _pendingGuideRewardApplied = false;
        _guideSystemFailureCount = 0;
        BLOG("[GUIDE] beginNewEpisode: runtime counters cleared");
    }

    void AbstractAgent::checkPendingGuideResult() {
        if (!_hasPendingGuideCheck) {
            return;
        }

        BLOG("[GUIDE] pending check: action=%" PRIu64 " page=%" PRIu64 " reached=%d",
             _pendingGuideActionHash, _pendingGuideTargetPage, _preconditionReachedSinceLastGuide ? 1 : 0);

        auto pageIt = _preconditionPages.find(_pendingGuideTargetPage);
        if (_preconditionReachedSinceLastGuide) {
            _consecutiveFails[_pendingGuideTargetPage][_pendingGuideActionHash] = 0;
            _noProgressCount[_pendingGuideTargetPage] = 0;
            _guideSystemFailureCount = 0;
            BLOG("[GUIDE] settle success: reset fail/noProgress for page=%" PRIu64 " action=%" PRIu64,
                 _pendingGuideTargetPage, _pendingGuideActionHash);
        } else {
            if (pageIt != _preconditionPages.end()) {
                PreconditionInfo &info = pageIt->second;
                // Action-level decay on matched slots.
                for (int t = 0; t < info.templateCount; ++t) {
                    GuidancePathTemplate &tpl = info.templates[t];
                    clampTemplateLength(&tpl.length);
                    for (int i = 0; i < tpl.length; ++i) {
                        if (tpl.sequence[i] == _pendingGuideActionHash && tpl.reliability[i] > 0.0) {
                            const double oldR = tpl.reliability[i];
                            tpl.reliability[i] = oldR * 0.5;
                            BLOG("[GUIDE] punish hit slot: page=%" PRIu64 " tpl=%d pos=%d action=%" PRIu64 " rel %.4f->%.4f",
                                 _pendingGuideTargetPage, t, i, _pendingGuideActionHash, oldR, tpl.reliability[i]);
                        }
                    }
                }
                // Page-level global decay.
                for (int t = 0; t < info.templateCount; ++t) {
                    GuidancePathTemplate &tpl = info.templates[t];
                    clampTemplateLength(&tpl.length);
                    for (int i = 0; i < tpl.length; ++i) {
                        tpl.reliability[i] *= 0.95;
                    }
                }
            }

            int &fails = _consecutiveFails[_pendingGuideTargetPage][_pendingGuideActionHash];
            fails += 1;
            int &noProgress = _noProgressCount[_pendingGuideTargetPage];
            noProgress += 1;
            _guideSystemFailureCount += 1;
            BLOG("[GUIDE] settle fail: page=%" PRIu64 " action=%" PRIu64 " fails=%d noProgress=%d systemFail=%d",
                 _pendingGuideTargetPage, _pendingGuideActionHash, fails, noProgress, _guideSystemFailureCount);

            if (pageIt != _preconditionPages.end()) {
                PreconditionInfo &info = pageIt->second;
                if (noProgress >= GuideConstants::kNoProgressThreshold) {
                    const double oldScore = info.score;
                    info.score = std::max(info.score * 0.3, GuideConstants::kMinPageScore);
                    _coveredThisEpisode.insert(_pendingGuideTargetPage);
                    BLOG("[GUIDE] page stop-loss: page=%" PRIu64 " score %.4f->%.4f, marked covered",
                         _pendingGuideTargetPage, oldScore, info.score);
                }
                if (fails >= GuideConstants::kFailBlacklistThreshold) {
                    for (int t = 0; t < info.templateCount; ++t) {
                        GuidancePathTemplate &tpl = info.templates[t];
                        clampTemplateLength(&tpl.length);
                        for (int i = 0; i < tpl.length; ++i) {
                            if (tpl.sequence[i] == _pendingGuideActionHash) {
                                tpl.reliability[i] = 0.0;
                            }
                        }
                    }
                    BLOG("[GUIDE] action blacklisted by fail threshold: page=%" PRIu64 " action=%" PRIu64,
                         _pendingGuideTargetPage, _pendingGuideActionHash);
                }
            }
        }

        BLOG("[GUIDE] pending cleared: action=%" PRIu64 " page=%" PRIu64,
             _pendingGuideActionHash, _pendingGuideTargetPage);
        _hasPendingGuideCheck = false;
        _pendingGuideActionHash = 0;
        _pendingGuideTargetPage = 0;
        _pendingGuideTemplateIndex = -1;
        _pendingGuidePosition = -1;
        _preconditionReachedSinceLastGuide = false;
        _pendingGuideRewardApplied = false;
    }

    ActionPtr AbstractAgent::trySelectGuideAction() {
        if (!_newState || _preconditionPages.empty()) {
            return nullptr;
        }
        if (_guideSystemFailureCount >= GuideConstants::kSystemFailStopThreshold) {
            BLOG("[GUIDE] skip by system stop-loss: systemFail=%d", _guideSystemFailureCount);
            return nullptr;
        }

        std::unordered_map<uint64_t, ActionPtr> currentActions;
        for (const auto &a : _newState->getActions()) {
            if (!a) continue;
            const uint64_t h = static_cast<uint64_t>(a->hash());
            if (!isValidGuideActionHash(h)) continue;
            currentActions[h] = a;
        }
        if (currentActions.empty()) {
            BLOG("[GUIDE] no valid current action hash on page=%" PRIu64,
                 static_cast<uint64_t>(_newState->hash()));
            return nullptr;
        }

        GuideChoice best;
        for (auto &kv : _preconditionPages) {
            const uint64_t pageHash = kv.first;
            PreconditionInfo &info = kv.second;
            if (_coveredThisEpisode.count(pageHash) > 0) {
                BLOG("[GUIDE] candidate page filtered: page=%" PRIu64 " reason=covered", pageHash);
                continue;
            }
            if (info.score < GuideConstants::kMinCandidateScore) {
                BLOG("[GUIDE] candidate page filtered: page=%" PRIu64 " reason=low-score score=%.4f", pageHash, info.score);
                continue;
            }
            if (info.templateCount <= 0) {
                BLOG("[GUIDE] candidate page filtered: page=%" PRIu64 " reason=no-template", pageHash);
                continue;
            }
            if (_pageSelectionCounts[pageHash] >= GuideConstants::kMaxPageSelectionPerEpisode) {
                BLOG("[GUIDE] candidate page filtered: page=%" PRIu64 " reason=page-select-limit cnt=%d", pageHash, _pageSelectionCounts[pageHash]);
                continue;
            }

            for (int t = 0; t < info.templateCount; ++t) {
                if (_templateSelectionCounts[pageHash][t] >= GuideConstants::kMaxTemplateSelectionPerEpisode) {
                    BLOG("[GUIDE] candidate template filtered: page=%" PRIu64 " tpl=%d reason=template-select-limit cnt=%d",
                         pageHash, t, _templateSelectionCounts[pageHash][t]);
                    continue;
                }
                GuidancePathTemplate &tpl = info.templates[t];
                clampTemplateLength(&tpl.length);
                if (tpl.length <= 0) {
                    continue;
                }
                for (int pos = tpl.length - 1; pos >= 0; --pos) {
                    const uint64_t actionHash = tpl.sequence[pos];
                    if (!isValidGuideActionHash(actionHash)) {
                        continue;
                    }
                    const int failCount = _consecutiveFails[pageHash][actionHash];
                    if (failCount >= GuideConstants::kFailBlacklistThreshold) {
                        tpl.reliability[pos] = 0.0;
                        BLOG("[GUIDE] template slot filtered: page=%" PRIu64 " tpl=%d pos=%d action=%" PRIu64 " reason=fail-blacklist",
                             pageHash, t, pos, actionHash);
                        continue;
                    }
                    auto actIt = currentActions.find(actionHash);
                    if (actIt == currentActions.end()) {
                        continue;
                    }
                    const double p = (tpl.length == 1) ? 1.0 : (static_cast<double>(pos) / static_cast<double>(tpl.length - 1));
                    const double r = tpl.reliability[pos];
                    const double c = info.score / 2.0;
                    const double score = 0.5 * p + 0.3 * r + 0.2 * c;
                    BLOG("[GUIDE] template match: page=%" PRIu64 " tpl=%d pos=%d action=%" PRIu64 " P=%.4f R=%.4f C=%.4f S=%.4f",
                         pageHash, t, pos, actionHash, p, r, c, score);
                    if (score > best.score) {
                        best.action = actIt->second;
                        best.pageHash = pageHash;
                        best.templateIndex = t;
                        best.pos = pos;
                        best.score = score;
                    }
                    break; // terminal-first: first executable slot from tail wins this template
                }
            }
        }

        if (!best.action) {
            BLOG("[GUIDE] no guide action selected this step");
            return nullptr;
        }

        const uint64_t actionHash = static_cast<uint64_t>(best.action->hash());
        _preconditionReachedSinceLastGuide = false;
        _hasPendingGuideCheck = true;
        _pendingGuideActionHash = actionHash;
        _pendingGuideTargetPage = best.pageHash;
        _pendingGuideTemplateIndex = best.templateIndex;
        _pendingGuidePosition = best.pos;
        _pendingGuideRewardApplied = false;
        _lastPosition[best.pageHash] = best.pos;
        _pageSelectionCounts[best.pageHash] += 1;
        _templateSelectionCounts[best.pageHash][best.templateIndex] += 1;
        BLOG("[GUIDE] selected action: page=%" PRIu64 " tpl=%d pos=%d action=%" PRIu64 " bestScore=%.4f",
             best.pageHash, best.templateIndex, best.pos, actionHash, best.score);
        BLOG("[GUIDE] pending set: has=1 action=%" PRIu64 " page=%" PRIu64,
             _pendingGuideActionHash, _pendingGuideTargetPage);
        return best.action;
    }

    void AbstractAgent::addCurrentPageAsPrecondition(const StatePtr &state) {
        if (!state) {
            return;
        }
        const uint64_t pageHash = static_cast<uint64_t>(state->hash());
        PreconditionInfo &info = _preconditionPages[pageHash];

        BLOG("[GUIDE] addCurrentPageAsPrecondition: page=%" PRIu64 " pending=%d pendingAction=%" PRIu64 " pendingPage=%" PRIu64,
             pageHash, _hasPendingGuideCheck ? 1 : 0, _pendingGuideActionHash, _pendingGuideTargetPage);

        // Refresh actionList from current page actions.
        info.actionList.clear();
        for (const auto &a : state->getActions()) {
            if (!a) continue;
            const uint64_t h = static_cast<uint64_t>(a->hash());
            if (isValidGuideActionHash(h)) {
                info.actionList.insert(h);
            }
        }

        // Delayed settlement: if a guide action is pending, reaching any precondition page marks success.
        // Reward the pending template once, then disable this template for the rest of current episode.
        if (_hasPendingGuideCheck) {
            _preconditionReachedSinceLastGuide = true;
            BLOG("[GUIDE] pending settled as success by precondition hit: reachedPage=%" PRIu64 " pendingPage=%" PRIu64,
                 pageHash, _pendingGuideTargetPage);

            auto pit = _preconditionPages.find(_pendingGuideTargetPage);
            if (!_pendingGuideRewardApplied && pit != _preconditionPages.end()) {
                PreconditionInfo &targetInfo = pit->second;
                const int tIdx = _pendingGuideTemplateIndex;
                bool rewardApplied = false;
                if (tIdx >= 0 && tIdx < targetInfo.templateCount) {
                    if (_templateRewardCounts[_pendingGuideTargetPage][tIdx] < GuideConstants::kMaxTemplateRewardPerEpisode) {
                        GuidancePathTemplate &tpl = targetInfo.templates[tIdx];
                        clampTemplateLength(&tpl.length);
                        for (int i = 0; i < tpl.length; ++i) {
                            if (tpl.sequence[i] != 0 && tpl.reliability[i] > 0.0) {
                                const double oldR = tpl.reliability[i];
                                tpl.reliability[i] = std::min(oldR * 1.2, 1.0);
                                BLOG("[GUIDE] reward slot: page=%" PRIu64 " tpl=%d pos=%d action=%" PRIu64 " rel %.4f->%.4f",
                                     _pendingGuideTargetPage, tIdx, i, _pendingGuideActionHash, oldR, tpl.reliability[i]);
                            }
                        }
                        _templateRewardCounts[_pendingGuideTargetPage][tIdx] += 1;
                        rewardApplied = true;
                    } else {
                        BLOG("[GUIDE] reward skipped by template cap: page=%" PRIu64 " tpl=%d",
                             _pendingGuideTargetPage, tIdx);
                    }
                    // This template has already been executed successfully in this episode.
                    // Lock it for the rest of this episode; reward remains persisted for next rounds.
                    _templateSelectionCounts[_pendingGuideTargetPage][tIdx] = GuideConstants::kMaxTemplateSelectionPerEpisode;
                    BLOG("[GUIDE] template locked for this episode: page=%" PRIu64 " tpl=%d",
                         _pendingGuideTargetPage, tIdx);
                }
                if (rewardApplied) {
                    const double oldScore = targetInfo.score;
                    targetInfo.score = std::min(oldScore * 1.5, GuideConstants::kMaxPageScore);
                    BLOG("[GUIDE] reward page score: page=%" PRIu64 " %.4f->%.4f",
                         _pendingGuideTargetPage, oldScore, targetInfo.score);
                }
                _pendingGuideRewardApplied = true;
            } else if (_pendingGuideRewardApplied) {
                BLOG("[GUIDE] reward skipped: already applied for current pending guide action");
            } else {
                BLOG("[GUIDE] reward skipped: pending target page not found");
            }
        }

        // Keep episode behavior consistent with legacy guide agent: once a precondition page is reached,
        // do not guide to the same page repeatedly within this episode.
        _coveredThisEpisode.insert(pageHash);

        // Build template from recent action history (old -> new), keep <= MAX_TEMPLATE_SEQUENCE_LEN non-zero hashes.
        std::array<uint64_t, MAX_TEMPLATE_SEQUENCE_LEN> seq{};
        int seqLen = 0;
        const size_t hs = _guideRecentActionHashes.size();
        const size_t start = (hs > static_cast<size_t>(MAX_TEMPLATE_SEQUENCE_LEN))
                             ? (hs - static_cast<size_t>(MAX_TEMPLATE_SEQUENCE_LEN))
                             : 0;
        for (size_t i = start; i < hs; ++i) {
            const uint64_t h = _guideRecentActionHashes[i];
            if (!isValidGuideActionHash(h)) {
                continue;
            }
            if (seqLen < MAX_TEMPLATE_SEQUENCE_LEN) {
                seq[seqLen++] = h;
            }
        }

        if (seqLen <= 0) {
            BLOG("[GUIDE] template skip: page=%" PRIu64 " reason=empty-sequence", pageHash);
            return;
        }

        auto templateEquals = [&](const GuidancePathTemplate &tpl) -> bool {
            if (tpl.length != seqLen) {
                return false;
            }
            for (int i = 0; i < seqLen; ++i) {
                if (tpl.sequence[i] != seq[i]) {
                    return false;
                }
            }
            return true;
        };

        for (int t = 0; t < info.templateCount; ++t) {
            if (templateEquals(info.templates[t])) {
                BLOG("[GUIDE] template exists: page=%" PRIu64 " tpl=%d len=%d", pageHash, t, seqLen);
                return;
            }
        }

        // FIFO insert at head, max 5 templates.
        const int maxSlot = MAX_TEMPLATE_SEQUENCE_LEN;
        const int oldCount = info.templateCount;
        const int newCount = std::min(oldCount + 1, maxSlot);
        for (int i = newCount - 1; i >= 1; --i) {
            info.templates[i] = info.templates[i - 1];
        }
        GuidancePathTemplate newTpl;
        newTpl.length = seqLen;
        for (int i = 0; i < seqLen; ++i) {
            newTpl.sequence[i] = seq[i];
            newTpl.reliability[i] = 1.0;
        }
        info.templates[0] = newTpl;
        info.templateCount = newCount;
        BLOG("[GUIDE] template inserted: page=%" PRIu64 " len=%d templateCount=%d", pageHash, seqLen, info.templateCount);
    }

    bool AbstractAgent::savePreconditionPagesToFile(const std::string &filepath) const {
        if (filepath.empty()) {
            BLOGE("[GUIDE] save .precond failed: empty path");
            return false;
        }
        flatbuffers::FlatBufferBuilder builder;
        std::vector<flatbuffers::Offset<fastbotx::PreconditionPage>> pageOffsets;

        size_t templateTotal = 0;
        for (const auto &kv : _preconditionPages) {
            const uint64_t pageHash = kv.first;
            const PreconditionInfo &info = kv.second;
            const int tcount = std::max(0, std::min(info.templateCount, MAX_TEMPLATE_SEQUENCE_LEN));

            std::vector<flatbuffers::Offset<fastbotx::PreconditionTemplate>> templateOffsets;
            for (int i = 0; i < tcount; ++i) {
                GuidancePathTemplate tpl = info.templates[i];
                clampTemplateLength(&tpl.length);

                auto seq = builder.CreateVector(tpl.sequence.data(), MAX_TEMPLATE_SEQUENCE_LEN);
                auto rel = builder.CreateVector(tpl.reliability.data(), MAX_TEMPLATE_SEQUENCE_LEN);
                templateOffsets.emplace_back(fastbotx::CreatePreconditionTemplate(
                        builder, tpl.length, seq, rel));
                templateTotal++;
            }

            auto templatesOffset = builder.CreateVector(templateOffsets);
            pageOffsets.emplace_back(fastbotx::CreatePreconditionPage(
                    builder, pageHash, info.score, templatesOffset));
        }

        auto pagesOffset = builder.CreateVector(pageOffsets);
        auto root = fastbotx::CreatePreconditionModel(builder, 1, pagesOffset);
        fastbotx::FinishPreconditionModelBuffer(builder, root);

        std::ofstream out(filepath, std::ios::binary | std::ios::trunc);
        if (!out.is_open()) {
            BLOGE("[GUIDE] save .precond failed: cannot open %s", filepath.c_str());
            return false;
        }
        out.write(reinterpret_cast<const char *>(builder.GetBufferPointer()),
                  static_cast<std::streamsize>(builder.GetSize()));
        if (out.fail()) {
            BLOGE("[GUIDE] save .precond failed while writing: %s", filepath.c_str());
            return false;
        }
        BLOG("[GUIDE] save .precond(flatbuffers-strict) ok: path=%s pages=%zu templates=%zu",
             filepath.c_str(), _preconditionPages.size(), templateTotal);
        return true;
    }

    bool AbstractAgent::loadPreconditionPagesFromFile(const std::string &filepath) {
        _preconditionPages.clear();
        if (filepath.empty()) {
            BLOGE("[GUIDE] load .precond failed: empty path");
            return false;
        }
        std::ifstream in(filepath, std::ios::binary | std::ios::in);
        if (!in.is_open()) {
            BLOG("[GUIDE] load .precond: file not found (first run): %s", filepath.c_str());
            return false;
        }

        in.seekg(0, std::ios::end);
        const std::streamsize sz = in.tellg();
        in.seekg(0, std::ios::beg);
        if (sz <= 0) {
            BLOGE("[GUIDE] load .precond failed: empty file %s", filepath.c_str());
            return false;
        }
        std::vector<uint8_t> buf(static_cast<size_t>(sz));
        in.read(reinterpret_cast<char *>(buf.data()), sz);
        if (in.fail()) {
            BLOGE("[GUIDE] load .precond failed: read error %s", filepath.c_str());
            return false;
        }

        flatbuffers::Verifier verifier(buf.data(), buf.size());
        if (!fastbotx::VerifyPreconditionModelBuffer(verifier)) {
            std::unordered_map<uint64_t, PreconditionInfo> legacyPages;
            if (parseLegacyPreconditionBinaryV2(buf, legacyPages)) {
                _preconditionPages.swap(legacyPages);
                BLOG("[GUIDE] load .precond legacy(v2) ok: path=%s pages=%zu", filepath.c_str(), _preconditionPages.size());
                return true;
            }
            BLOGE("[GUIDE] load .precond failed: invalid flatbuffer and legacy parse failed %s", filepath.c_str());
            return false;
        }
        const fastbotx::PreconditionModel *model = fastbotx::GetPreconditionModel(buf.data());
        if (model == nullptr) {
            BLOGE("[GUIDE] load .precond failed: null root %s", filepath.c_str());
            return false;
        }

        const int version = model->version();
        if (version != 1) {
            BLOGE("[GUIDE] load .precond failed: unsupported version=%d path=%s", version, filepath.c_str());
            return false;
        }

        auto pages = model->pages();
        if (pages == nullptr) {
            BLOGE("[GUIDE] load .precond failed: pages missing path=%s", filepath.c_str());
            return false;
        }

        size_t templateTotal = 0;
        for (size_t e = 0; e < pages->size(); ++e) {
            auto page = pages->Get(static_cast<flatbuffers::uoffset_t>(e));
            if (page == nullptr) continue;

            const uint64_t pageHash = page->hashcode();
            const double score = page->score();
            auto templates = page->templates();

            PreconditionInfo info;
            info.score = std::max(GuideConstants::kMinPageScore, std::min(score, GuideConstants::kMaxPageScore));
            const int templateSize = templates ? static_cast<int>(templates->size()) : 0;
            info.templateCount = std::min(templateSize, MAX_TEMPLATE_SEQUENCE_LEN);

            for (int i = 0; i < info.templateCount; ++i) {
                auto templ = templates->Get(i);
                if (templ == nullptr) continue;
                GuidancePathTemplate tpl;

                tpl.length = templ->length();
                clampTemplateLength(&tpl.length);

                auto seq = templ->sequence();
                auto rel = templ->reliability();
                for (int j = 0; j < MAX_TEMPLATE_SEQUENCE_LEN; ++j) {
                    tpl.sequence[j] = (seq && j < static_cast<int>(seq->size())) ? seq->Get(j) : 0;
                    tpl.reliability[j] = (rel && j < static_cast<int>(rel->size())) ? rel->Get(j) : 0.0;
                }
                for (int p = tpl.length; p < MAX_TEMPLATE_SEQUENCE_LEN; ++p) {
                    tpl.sequence[p] = 0;
                    tpl.reliability[p] = 0.0;
                }

                info.templates[i] = tpl;
                templateTotal++;
            }
            _preconditionPages[pageHash] = info;
        }

        BLOG("[GUIDE] load .precond(flatbuffers-strict) ok: path=%s pages=%zu templates=%zu",
             filepath.c_str(), _preconditionPages.size(), templateTotal);
        return true;
    }

    /**
     * @brief Adjust action priorities
     * 
     * Dynamically adjusts priority of each action based on visit status, type,
     * saturation status, etc.
     * 
     * Priority adjustment rules:
     * 
     * 1. Base priority: Get base priority from action type
     * 
     * 2. No-target actions:
     *    - If unvisited, add NoTargetUnvisitedBonus (5)
     *    - Skip subsequent processing
     * 
     * 3. Target actions:
     *    - If invalid, skip (no priority adjustment)
     *    - If unvisited, add UnvisitedActionBonus (20)
     *    - If new action (state not saturated), add NewActionMultiplier (5) * base priority
     *    - Ensure priority is not less than 0
     * 
     * 4. Calculate total state priority:
     *    - Accumulate (adjusted priority - base priority) for all actions
     *    - Set state priority to total priority
     * 
     * Performance optimization:
     * - Uses references to avoid unnecessary copies
     * - Early continue to skip invalid actions
     * 
     * @note Time complexity: O(n), where n is the number of actions in the state
     */
    void AbstractAgent::adjustActions() {
        using namespace ActionPriorityConstants;
        
        // Accumulate priority increments for all actions (for calculating total state priority)
        double totalPriority = 0;
        
        // Iterate through all actions in state and adjust priorities
        for (const ActivityStateActionPtr &action: _newState->getActions()) {
            // Get and set base priority
            int basePriority = action->getPriorityByActionType();
            action->setPriority(basePriority);
            
            // Handle no-target actions (e.g., BACK, FEED system actions)
            if (!action->requireTarget()) {
                if (!action->isVisited()) {
                    // Unvisited no-target action, add bonus
                    int priority = action->getPriority();
                    priority += NoTargetUnvisitedBonus;
                    action->setPriority(priority);
                }
                continue;  // No-target action processing complete, skip subsequent logic
            }
            
            // Target actions must be valid
            if (!action->isValid()) {
                continue;  // Skip invalid actions
            }
            
            // Calculate priority for target actions
            int priority = action->getPriority();
            
            // Unvisited actions get bonus, encouraging exploration
            if (!action->isVisited()) {
                priority += UnvisitedActionBonus;
            }
            
            // If new action (state not saturated), significantly increase priority
            if (!this->_newState->isSaturated(action)) {
                priority += NewActionMultiplier * action->getPriorityByActionType();
            }

            // Ensure priority is not negative
            if (priority <= 0) {
                priority = 0;
            }

            // Set adjusted priority
            action->setPriority(priority);
            
            // Accumulate priority increment (for calculating total state priority)
            totalPriority += (priority - basePriority);
        }
        
        // Set total state priority
        _newState->setPriority(static_cast<int>(totalPriority));
    }

    /**
     * @brief Resolve and select a new action
     * 
     * Main entry point for action selection. Execution flow:
     * 1. Call adjustActions() to adjust priorities of all candidate actions
     * 2. Call subclass's selectNewAction() to select specific action (strategy pattern)
     * 3. Convert selected action to ActivityStateAction type and save to _newAction
     * 
     * @return Pointer to selected action, or nullptr if selection fails
     */
    ActionPtr AbstractAgent::resolveNewAction() {
        this->adjustActions();

        const PreferencePtr pref = Preference::inst();
        if (pref && pref->isLlmdroidEnabled() && _llmdroid) {
            LlmdroidAgentOverlay &L = *_llmdroid;
            if (L.mode == LlmdroidMode::NAVIGATE) {
                if (!L.currentPath.steps.empty()) {
                    ActionPtr nextAction = L.currentPath.steps.front().action;
                    ActivityStateActionPtr tmp = std::dynamic_pointer_cast<ActivityStateAction>(nextAction);
                    ActionPtr action =
                        (tmp && L.mCurrentState) ? L.mCurrentState->findSimilarAction(nextAction) : nextAction;
                    if (!action) {
                        BLOGE("LLMDroid NAVIGATE: findSimilarAction failed");
                    } else {
                        _newAction = std::dynamic_pointer_cast<ActivityStateAction>(action);
                        return action;
                    }
                }
                BLOGE("LLMDroid NAVIGATE: currentPath is empty, fallback to RL selectNewAction. "
                      "state=%d guideTarget=%d pendingPaths=%zu guideTime=%d similarity=%.3f",
                      L.mCurrentState ? L.mCurrentState->getIdi() : -1,
                      L.guideTarget,
                      L.paths.size(),
                      L.guideTime,
                      static_cast<double>(L.currentSimilarityCheck));
            } else if (L.mode == LlmdroidMode::TEST_FUNCTION) {
                if (L.actionByGpt) {
                    _newAction = L.actionByGpt;
                    return std::static_pointer_cast<Action>(L.actionByGpt);
                }
                llmdroidPrepareBackToExplore(L, *this);
            }
        }

        ActionPtr action = this->selectNewAction();
        _newAction = std::dynamic_pointer_cast<ActivityStateAction>(action);
        return action;
    }

    /**
     * @brief Handle null action situation
     * 
     * When no valid action can be selected (selectNewAction returns nullptr),
     * attempts to randomly select a valid action from current state as fallback.
     * 
     * Handling flow:
     * 1. Randomly select an action from _newState that passes validation filter
     * 2. If action found, attempt to resolve it (resolveAt)
     * 3. If resolution succeeds, return resolved action
     * 4. If all steps fail, log error and return nullptr
     * 
     * @return Pointer to handled action, or nullptr on failure
     */
    ActivityStateActionPtr AbstractAgent::handleNullAction() const {
        // Attempt to randomly select a valid action
        ActivityStateActionPtr action = this->_newState->randomPickAction(this->_validateFilter);
        
        if (nullptr != action) {
            // Get model pointer (use weak_ptr to avoid circular references)
            auto modelPtr = this->_model.lock();
            if (!modelPtr) {
                BDLOGE("Model has been destroyed, cannot handle null action");
                return nullptr;
            }
            
            // Resolve action (resolve action target, etc. based on timestamp)
            ActivityStateActionPtr resolved = this->_newState->resolveAt(action,
                                                                         modelPtr->getGraph()->getTimestamp());
            if (nullptr != resolved) {
                return resolved;
            }
        }
        
        // All attempts failed, log error
        BDLOGE("handle null action error!!!!!");
        return nullptr;
    }
}

#endif //AbstractAgent_CPP_
