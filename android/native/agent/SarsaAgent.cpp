/*
 * This code is licensed under the Fastbot license. You may obtain a copy of this license in the LICENSE.txt file in the root directory of this source tree.
 */
/**
 * @authors Jianqiang Guo, Yuhui Su, Zhao Zhang
 */

#ifndef fastbotx_SarsaAgent_CPP_
#define fastbotx_SarsaAgent_CPP_

#include "SarsaAgent.h"
#include "ReuseDecisionTuning.h"
#include "Model.h"
#include "ModelStorageConstants.h"
#include "ContentAwareInputProvider.h"
#include "WidgetPriorityProvider.h"
#include "../storage/ReuseModel_generated.h"
#include "../events/Preference.h"
#include "../utils.hpp"
#include <cmath>
#include <fstream>
#include <limits>
#include <thread>
#include <chrono>
#include <cinttypes>
#include <cstdlib>

namespace fastbotx {

    std::string SarsaAgent::DefaultModelSavePath = "/sdcard/fastbot.model.fbm";

    SarsaAgent::SarsaAgent(const ModelPtr &model)
            : AbstractAgent(model),
              _alpha(kDefaultAlpha),
              _gamma(kDefaultGamma),
              _epsilon(kDefaultEpsilon),
              _modelSavePath(DefaultModelSavePath),
              _tmpSavePath(DefaultModelSavePath),
              _contentAwareInputProvider(std::make_shared<LlmContentAwareInputProvider>()),
              _widgetPriorityProvider(std::make_shared<LlmWidgetPriorityProvider>()) {
        this->_algorithmType = AlgorithmType::Sarsa;
        BLOG("SarsaAgent: initialized with alpha=%.4f, gamma=%.4f, epsilon=%.4f",
             _alpha, _gamma, _epsilon);
    }

    SarsaAgent::SarsaAgent()
            : AbstractAgent(),
              _alpha(kDefaultAlpha),
              _gamma(kDefaultGamma),
              _epsilon(kDefaultEpsilon),
              _modelSavePath(DefaultModelSavePath),
              _tmpSavePath(DefaultModelSavePath),
              _contentAwareInputProvider(std::make_shared<LlmContentAwareInputProvider>()),
              _widgetPriorityProvider(std::make_shared<LlmWidgetPriorityProvider>()) {
        this->_algorithmType = AlgorithmType::Sarsa;
    }

    SarsaAgent::~SarsaAgent() {
        BLOG("SarsaAgent: destructor called, saving model");
        this->saveReuseModel(this->_modelSavePath);
        {
            std::lock_guard<std::mutex> lock(this->_reuseModelLock);
            this->_reuseModel.clear();
        }
    }

    void SarsaAgent::setContentAwareInputProvider(const std::shared_ptr<IContentAwareInputProvider> &provider) {
        _contentAwareInputProvider = provider;
    }

    void SarsaAgent::setWidgetPriorityProvider(const std::shared_ptr<IWidgetPriorityProvider> &provider) {
        _widgetPriorityProvider = provider;
    }

    void SarsaAgent::moveForward(StatePtr nextState) {
        AbstractAgent::moveForward(nextState);
        _stepCount++;
    }

    bool SarsaAgent::isReuseDecisionTuningEnabled() {
        auto pref = Preference::inst();
        return pref && pref->isReuseDecisionTuningEnabled();
    }

    std::string SarsaAgent::getInputTextForAction(const StatePtr &state, const ActionPtr &action) const {
        if (!Preference::inst() || !Preference::inst()->getLlmRuntimeConfig().enabled
            || !Preference::inst()->isLlmContextAwareInputEnabled()) return "";
        if (!_contentAwareInputProvider) return "";

        // Throttle: at most one LLM content_aware_input call every kMinStepsBetweenContentAwareInputCalls steps.
        if (_lastContentAwareInputStep != 0 &&
            _stepCount - _lastContentAwareInputStep < kMinStepsBetweenContentAwareInputCalls) {
            return "";
        }
        // Optional probability: when allowed by step throttle, only call with kContentAwareInputCallProbability to reduce load.
        if (kContentAwareInputCallProbability < 1.0 &&
            static_cast<double>(randomInt(0, 100)) / 100.0 >= kContentAwareInputCallProbability) {
            return "";
        }

        ModelPtr model = _model.lock();
        std::string result = _contentAwareInputProvider->getInputTextForAction(state, action, model);
        _lastContentAwareInputStep = _stepCount;
        return result;
    }

    double SarsaAgent::getNewReward() {
        double reward = 0.0;
        if (nullptr != this->_newState) {
            auto modelPtr = this->_model.lock();
            if (!modelPtr) {
                BLOGE("SarsaAgent: model expired in getNewReward");
                return reward;
            }
            const GraphPtr &graphRef = modelPtr->getGraph();
            auto visitedActivities = graphRef->getVisitedActivities();
            long totalVisitCount = graphRef->getTotalDistri();

            double movingAlpha = 0.5;
            if (totalVisitCount > 20000) movingAlpha -= 0.1;
            if (totalVisitCount > 50000) movingAlpha -= 0.1;
            if (totalVisitCount > 100000) movingAlpha -= 0.1;
            if (totalVisitCount > 250000) movingAlpha -= 0.1;
            // Keep a minimum learning rate consistent with the legacy default.
            this->_alpha = std::max(kDefaultAlpha, movingAlpha);

            if (!this->_previousActions.empty()) {
                auto lastSelectedAct = std::dynamic_pointer_cast<ActivityStateAction>(this->_previousActions.back());
                if (lastSelectedAct) {
                    // NOTE: Legacy ReuseAgent contained a logic bug here:
                    // it effectively never used the return value of getReuseActionValue
                    // and always degraded to reward += 1.0. We fix this by only
                    // falling back to 1.0 when reuseValue is numerically close to 0.
                    double reuseValue = this->getReuseActionValue(lastSelectedAct, visitedActivities);
                    if (std::abs(reuseValue - 0.0) < 0.0001) {
                        reuseValue = 1.0;
                    }

                    if (isReuseDecisionTuningEnabled()) {
                        stringPtr curActivity = this->_newState->getActivityString();
                        uint64_t lastHash = static_cast<uint64_t>(lastSelectedAct->hash());
                        double loopBias = this->computeLoopBias(lastHash, curActivity);
                        double diversity = this->computeCoverageDiversity(lastHash);
                        double reusePrior = ReuseDecisionTuning::computeReusePrior(reuseValue, loopBias, diversity);
                        reward += ReuseDecisionTuning::kBetaReuse * (reusePrior /
                                               std::sqrt(lastSelectedAct->getVisitedCount() + 1.0));
                    } else {
                        reward += (reuseValue / std::sqrt(lastSelectedAct->getVisitedCount() + 1.0));
                    }
                }
            }

            reward += (this->getStateActionValue(this->_newState, visitedActivities) /
                       std::sqrt(this->_newState->getVisitedCount() + 1.0));

            BLOG("SarsaAgent: total visited activities count is %zu", visitedActivities.size());
            BDLOG("SarsaAgent: getNewReward alpha=%.4f gamma=%.4f totalVisit=%ld reward=%.4f",
                  this->_alpha, this->_gamma, totalVisitCount, reward);
        }

        BDLOG("reuse-cov-opti action reward=%f", reward);
        this->_rewardHistory.emplace_back(reward);
        if (this->_rewardHistory.size() > static_cast<size_t>(kMaxHistorySteps)) {
            this->_rewardHistory.erase(this->_rewardHistory.begin());
        }

        return reward;
    }

    double SarsaAgent::getReuseActionValue(const ActivityStateActionPtr &action,
                                           const stringPtrSet &visitedActivities) const {
        double value = 0.0;
        int total = 0;
        int unvisited = 0;
        auto iter = this->_reuseModel.find(action->hash());
        if (iter != this->_reuseModel.end()) {
            for (const auto &entry : iter->second) {
                total += entry.second;
                stringPtr activity = entry.first;
                if (visitedActivities.find(activity) == visitedActivities.end()) {
                    unvisited += entry.second;
                }
            }
            if (total > 0) {
                // Cap the effective sample size to avoid very old experience dominating forever.
                static const int kMaxEffectiveSamplesPerAction = 100000;
                int cappedTotal = total;
                int cappedUnvisited = unvisited;
                if (total > kMaxEffectiveSamplesPerAction) {
                    const double scale = static_cast<double>(kMaxEffectiveSamplesPerAction) /
                                         static_cast<double>(total);
                    cappedTotal = kMaxEffectiveSamplesPerAction;
                    cappedUnvisited = static_cast<int>(std::round(unvisited * scale));
                }

                // Use a simple Beta prior over "unvisited vs visited" rather than raw ratio,
                // to smooth early noise and keep very large histories from fully overwhelming the prior.
                const double alphaPrior = 1.0;  // prior pseudo-count for "unvisited"
                const double betaPrior = 1.0;   // prior pseudo-count for "visited"
                const double totalEffective = static_cast<double>(cappedTotal);
                const double unvisitedEffective = static_cast<double>(cappedUnvisited);

                value = (unvisitedEffective + alphaPrior) /
                        (totalEffective + alphaPrior + betaPrior);
            }
        }
        return value;
    }

    double SarsaAgent::computeLoopBias(uint64_t actionHash, const stringPtr &currentActivity) const {
        std::lock_guard<std::mutex> lock(this->_reuseModelLock);
        auto it = this->_reuseModel.find(actionHash);
        if (it == this->_reuseModel.end()) return 0.0;
        return ReuseDecisionTuning::computeLoopBiasFromEntry(it->second, currentActivity);
    }

    double SarsaAgent::computeCoverageDiversity(uint64_t actionHash) const {
        std::lock_guard<std::mutex> lock(this->_reuseModelLock);
        auto it = this->_reuseModel.find(actionHash);
        if (it == this->_reuseModel.end()) return 0.0;
        return ReuseDecisionTuning::computeCoverageDiversityFromEntry(it->second);
    }

    double SarsaAgent::getStateActionValue(const StatePtr &state,
                                           const stringPtrSet &visitedActivities) const {
        double value = 0.0;
        for (const auto &action : state->getActions()) {
            uintptr_t actHash = action->hash();
            auto it = this->_reuseModel.find(actHash);
            if (it == this->_reuseModel.end()) {
                value += 1.0;
            } else if (action->getVisitedCount() >= 1) {
                value += 0.5;
            }

            if (action->getTarget() != nullptr) {
                auto actPtr = std::dynamic_pointer_cast<ActivityStateAction>(action);
                if (actPtr) {
                    value += getReuseActionValue(actPtr, visitedActivities);
                }
            }
        }
        return value;
    }

    void SarsaAgent::updateStrategy() {
        if (nullptr == this->_newAction) {
            return;
        }

        if (!this->_previousActions.empty()) {
            this->getNewReward();
            this->updateReuseModel();

            double value = this->_newAction->getQValue();
            const int historySize = static_cast<int>(this->_previousActions.size());
            const int rewardSize = static_cast<int>(this->_rewardHistory.size());
            const int limit = std::min(historySize, rewardSize);
            BLOG("SarsaAgent: updateStrategy(history=%d,reward=%d,limit=%d,alpha=%.4f,gamma=%.4f)",
                 historySize, rewardSize, limit, this->_alpha, this->_gamma);
            for (int i = limit - 1; i >= 0; --i) {
                auto act = std::dynamic_pointer_cast<ActivityStateAction>(this->_previousActions[i]);
                if (!act) continue;
                double curV = act->getQValue();
                double curR = this->_rewardHistory[i];
                value = curR + _gamma * value;
                act->setQValue(curV + _alpha * (value - curV));
                BDLOG("SarsaAgent: Q-update i=%d curR=%.4f backup=%.4f oldQ=%.4f newQ=%.4f",
                      i, curR, value, curV, act->getQValue());
            }
        } else {
            BDLOG("%s", "SarsaAgent: get action value failed (empty history)");
        }

        this->_previousActions.emplace_back(this->_newAction);
        if (this->_previousActions.size() > static_cast<size_t>(kMaxHistorySteps)) {
            this->_previousActions.erase(this->_previousActions.begin());
        }
    }

    void SarsaAgent::updateReuseModel() {
        if (this->_previousActions.empty()) return;
        ActionPtr lastAction = this->_previousActions.back();
        auto modelAction = std::dynamic_pointer_cast<ActivityStateAction>(lastAction);
        if (!modelAction || !this->_newState) return;

        uint64_t hash = static_cast<uint64_t>(modelAction->hash());
        stringPtr activity = this->_newState->getActivityString();
        if (!activity) return;

        std::lock_guard<std::mutex> lock(this->_reuseModelLock);
        auto iter = this->_reuseModel.find(hash);
        if (iter == this->_reuseModel.end()) {
            BDLOG("SarsaAgent: can not find action %s in reuse map", modelAction->getId().c_str());
            ReuseEntryM entrym;
            entrym.emplace(activity, 1);
            this->_reuseModel[hash] = std::move(entrym);
        } else {
            ((*iter).second)[activity] += 1;
        }
    }

    ActivityStateActionPtr SarsaAgent::selectNewActionEpsilonGreedyRandomly() const {
        if (this->eGreedy()) {
            BDLOG("%s", "SarsaAgent: Try to select the max value action");
            return this->_newState->greedyPickMaxQValue(enableValidValuePriorityFilter);
        }
        BDLOG("%s", "SarsaAgent: Try to randomly select a value action.");
        return this->_newState->randomPickAction(enableValidValuePriorityFilter);
    }

    bool SarsaAgent::eGreedy() const {
        // Use thread-local RNG via randomInt; epsilon is small (explore with prob epsilon)
        double r = static_cast<double>(randomInt(0, 100)) / 100.0L;
        return !(r < this->_epsilon);
    }

    ActionPtr SarsaAgent::selectActionNotInModel() const {
        ActionPtr chosen = nullptr;
        double totalWeight = 0.0;
        uintptr_t stateHash = this->_newState ? this->_newState->hash() : 0;

        for (const auto &action : this->_newState->getActions()) {
            const bool matched = action->isModelAct()
                                 && (this->_reuseModel.find(action->hash()) == this->_reuseModel.end())
                                 && action->getVisitedCount() <= 0;
            if (!matched) continue;

            const int p = action->getPriority();
            if (p <= 0) continue;

            double w = static_cast<double>(p);
            if (Preference::inst() && Preference::inst()->getLlmRuntimeConfig().enabled
                && Preference::inst()->isLlmKnowledgeEnabled()) {
                w *= this->getWidgetPriority(stateHash, action->hash());
            }
            const double newTotal = totalWeight + w;
            const double r = static_cast<double>(randomInt(0, 10000)) / 10000.0 * newTotal;
            if (r < w) chosen = action;
            totalWeight = newTotal;
        }
        if (totalWeight > 0.0 && chosen) return chosen;

        BDLOG("%s", "SarsaAgent: no unvisited not-in-model (fall back to next strategy)");
        return nullptr;
    }

    namespace {
        inline double sampleGumbelNoise() {
            // Sample a value in (0,1) and transform to standard Gumbel(0,1).
            // Avoid exact 0/1 to keep log well-defined.
            const int r = randomInt(1, 10000);
            const double u = static_cast<double>(r) / 10001.0;
            return -std::log(-std::log(u));
        }
    }

    ActionPtr SarsaAgent::selectActionInModel(const stringPtrSet &visitedActivities) const {
        float maxValue = -MAXFLOAT;
        ActionPtr retAct = nullptr;
        for (const auto &action : this->_newState->targetActions()) {
            uintptr_t actHash = action->hash();
            if (this->_reuseModel.find(actHash) != this->_reuseModel.end()) {
                if (action->getVisitedCount() > 0) {
                    BDLOG("%s", "SarsaAgent: action has been visited");
                    continue;
                }
                auto actPtr = std::dynamic_pointer_cast<ActivityStateAction>(action);
                if (!actPtr) continue;
                float qv = static_cast<float>(this->getReuseActionValue(actPtr, visitedActivities));
                if (qv > 1e-4f) {
                    qv = 10.0f * qv;
                    qv -= static_cast<float>(sampleGumbelNoise());
                    if (qv > maxValue) {
                        maxValue = qv;
                        retAct = action;
                    }
                }
            }
        }
        return retAct;
    }

    ActionPtr SarsaAgent::selectActionByQValue(const stringPtrSet &visitedActivities) const {
        ActionPtr retAct = nullptr;
        float maxQ = -MAXFLOAT;
        for (const auto &action : this->_newState->getActions()) {
            double qv = 0.0;
            uintptr_t actHash = action->hash();
            if (action->getVisitedCount() <= 0) {
                auto iter = this->_reuseModel.find(actHash);
                if (iter != this->_reuseModel.end()) {
                    auto actPtr = std::dynamic_pointer_cast<ActivityStateAction>(action);
                    if (actPtr) {
                        qv += this->getReuseActionValue(actPtr, visitedActivities);
                    }
                } else {
                    BDLOG("SarsaAgent: qvalue pick return a action: %s", action->toString().c_str());
                    return action;
                }
            }
            qv += action->getQValue();
            qv /= kEntropyAlpha;
            qv -= sampleGumbelNoise();
            if (qv > maxQ) {
                maxQ = static_cast<float>(qv);
                retAct = action;
            }
        }
        return retAct;
    }

    void SarsaAgent::adjustActions() {
        AbstractAgent::adjustActions();
    }

    void SarsaAgent::ensureWidgetPrioritiesForState(const StatePtr &state) {
        if (!state || !_widgetPriorityProvider) {
            if (state && !_widgetPriorityProvider) BDLOG("SarsaAgent: widget_priority skip (no provider)");
            return;
        }
        if (!Preference::inst()) {
            BDLOG("SarsaAgent: widget_priority skip (no Preference)");
            return;
        }
        if (!Preference::inst()->getLlmRuntimeConfig().enabled) {
            BDLOG("SarsaAgent: widget_priority skip (max.llm.enabled=false)");
            return;
        }
        if (!Preference::inst()->isLlmKnowledgeEnabled()) {
            BDLOG("SarsaAgent: widget_priority skip (max.llm.widgetpriority/knowledge=false)");
            return;
        }

        uintptr_t stateHash = state->hash();
        if (_stateWidgetPrioritiesRequested.find(stateHash) != _stateWidgetPrioritiesRequested.end()) {
            return;
        }

        std::vector<ActivityStateActionPtr> validActions;
        for (const auto &a : state->getActions()) {
            if (a && a->isValid()) validActions.push_back(a);
        }
        if (validActions.size() < 2) {
            _stateWidgetPrioritiesRequested.insert(stateHash);
            BDLOG("SarsaAgent: widget_priority skip stateHash=0x%llx (validActions=%zu < 2)",
                  (unsigned long long) stateHash, validActions.size());
            return;
        }

        ModelPtr model = _model.lock();
        if (!model) {
            BDLOG("SarsaAgent: widget_priority skip stateHash=0x%llx (model expired)", (unsigned long long) stateHash);
            _stateWidgetPrioritiesRequested.insert(stateHash);
            return;
        }
        BDLOG("SarsaAgent: widget_priority request stateHash=0x%llx n=%zu",
              (unsigned long long) stateHash, validActions.size());
        IWidgetPriorityProvider::Result res = _widgetPriorityProvider->organize(0, validActions, model);

        if (res.success && res.widgetPriorities.size() == validActions.size()) {
            for (size_t i = 0; i < validActions.size(); ++i) {
                uintptr_t actHash = validActions[i]->hash();
                double p = res.widgetPriorities[i];
                if (p < 0.0) p = 0.0;
                _actionPriority[kActionPriorityKey(stateHash, actHash)] = p;
            }
            BDLOG("SarsaAgent: widget_priority stateHash=0x%llx n=%zu",
                  (unsigned long long) stateHash, validActions.size());
            for (size_t i = 0; i < validActions.size(); ++i) {
                const auto &a = validActions[i];
                WidgetPtr w = a ? a->getTarget() : nullptr;
                const char *resId = w ? w->getResourceID().c_str() : "";
                const char *clazz = w ? w->getClassname().c_str() : "";
                std::string text = w ? w->getText() : "";
                if (text.size() > 40) text = text.substr(0, 37) + "...";
                for (char &c : text) if (c == '\n' || c == '\r') c = ' ';
                double p = res.widgetPriorities[i];
                if (p < 0.0) p = 0.0;
                BDLOG("SarsaAgent: widget_priority [%zu] hash=0x%llx resource_id=%s class=%s text=%s priority=%.3f",
                      i, (unsigned long long) a->hash(), resId[0] ? resId : "(none)", clazz[0] ? clazz : "(none)",
                      text.c_str(), p);
            }
            _stateWidgetPrioritiesRequested.insert(stateHash);
        } else {
            BDLOG("SarsaAgent: widget_priority stateHash=0x%llx organize failed or empty (success=%d priorities=%zu), will retry next time",
                  (unsigned long long) stateHash, res.success ? 1 : 0, res.widgetPriorities.size());
            // Do not insert into _stateWidgetPrioritiesRequested so we retry on next visit (e.g. after client becomes available)
        }
    }

    void SarsaAgent::clearReuseModelOnLoadFailure() {
        std::lock_guard<std::mutex> lock(this->_reuseModelLock);
        this->_reuseModel.clear();
    }

    double SarsaAgent::getWidgetPriority(uintptr_t stateHash, uintptr_t actionHash) const {
        uintptr_t key = kActionPriorityKey(stateHash, actionHash);
        auto it = _actionPriority.find(key);
        if (it != _actionPriority.end() && it->second > 0.0) return it->second;
        return 1.0;
    }

    ActionPtr SarsaAgent::selectNewAction() {
        ActionPtr action = nullptr;

        if (this->_newState) {
            ensureWidgetPrioritiesForState(this->_newState);
        }

        action = this->selectActionNotInModel();
        if (action) {
            BLOG("%s", "SarsaAgent: select action not in reuse model");
            return action;
        }

        // Reuse-based policies require visited-activity statistics from the graph.
        stringPtrSet visitedActivities;
        if (auto modelPtr = this->_model.lock()) {
            const GraphPtr &graphRef = modelPtr->getGraph();
            visitedActivities = graphRef->getVisitedActivities();
        }

        if (!visitedActivities.empty()) {
            action = this->selectActionInModel(visitedActivities);
            if (action) {
                BLOG("%s", "SarsaAgent: select action in reuse model");
                return action;
            }
        }

        action = this->_newState->randomPickUnvisitedAction();
        if (action) {
            BLOG("%s", "SarsaAgent: select action in unvisited action");
            return action;
        }

        if (!visitedActivities.empty()) {
            action = this->selectActionByQValue(visitedActivities);
            if (action) {
                BLOG("%s", "SarsaAgent: select action by qvalue");
                return action;
            }
        }

        auto actPtr = this->selectNewActionEpsilonGreedyRandomly();
        if (actPtr) {
            BLOG("%s", "SarsaAgent: select action by EpsilonGreedyRandom");
            return actPtr;
        }

        BLOGE("SarsaAgent: null action happened, handle null action");
        return handleNullAction();
    }

    void SarsaAgent::loadReuseModel(const std::string &packageName) {
        const bool useStatic = Preference::inst() && Preference::inst()->useStaticReuseAbstraction();
        std::string basePath = std::string(ModelStorageConstants::StoragePrefix) + packageName;
        std::string modelFilePath = useStatic
                                    ? (basePath + ".static" + ModelStorageConstants::ModelFileExtension)
                                    : (basePath + ModelStorageConstants::ModelFileExtension);

        this->_modelSavePath = modelFilePath;
        if (!this->_modelSavePath.empty()) {
            this->_tmpSavePath = useStatic
                                 ? (basePath + ".static" + ModelStorageConstants::TempModelFileExtension)
                                 : (basePath + ModelStorageConstants::TempModelFileExtension);
        }

        BLOG("SarsaAgent: begin load model: %s", this->_modelSavePath.c_str());

        std::ifstream modelFile(modelFilePath, std::ios::binary | std::ios::in);
        if (!modelFile.is_open()) {
            BLOGE("SarsaAgent: Failed to open model file: %s", modelFilePath.c_str());
            clearReuseModelOnLoadFailure();
            return;
        }

        std::filebuf *fileBuffer = modelFile.rdbuf();
        std::size_t filesize = fileBuffer->pubseekoff(0, modelFile.end, modelFile.in);
        fileBuffer->pubseekpos(0, modelFile.in);

        if (filesize <= 0) {
            BLOGE("SarsaAgent: Invalid model file size: %zu", filesize);
            clearReuseModelOnLoadFailure();
            return;
        }

        std::unique_ptr<char[]> modelFileData(new char[filesize]);
        std::streamsize bytesRead = fileBuffer->sgetn(modelFileData.get(), static_cast<int>(filesize));
        if (bytesRead != static_cast<std::streamsize>(filesize)) {
            BLOGE("SarsaAgent: Failed to read complete model file: read %lld bytes, expected %zu bytes",
                  static_cast<long long>(bytesRead), filesize);
            clearReuseModelOnLoadFailure();
            return;
        }

        flatbuffers::Verifier verifier(reinterpret_cast<const uint8_t *>(modelFileData.get()), filesize);
        if (!VerifyReuseModelBuffer(verifier)) {
            BLOGE("SarsaAgent: Invalid or corrupted model buffer");
            clearReuseModelOnLoadFailure();
            return;
        }
        auto reuseFBModel = GetReuseModel(modelFileData.get());
        if (!reuseFBModel) {
            BLOGE("SarsaAgent: GetReuseModel returned null");
            clearReuseModelOnLoadFailure();
            return;
        }

        auto modelDataPtr = reuseFBModel->model();
        if (!modelDataPtr) {
            BLOG("SarsaAgent: model data is null");
            clearReuseModelOnLoadFailure();
            return;
        }

        // Build a fresh model map off-lock, then swap in under mutex. Merge by activity name
        // so duplicate (hash, activity) rows in file sum to one entry (fixes duplicate keys from stringPtr).
        ReuseEntryIntMap newModel;
        for (flatbuffers::uoffset_t i = 0; i < modelDataPtr->size(); ++i) {
            auto modelEntry = modelDataPtr->Get(i);
            uint64_t actionHash = modelEntry->action();
            auto activityEntries = modelEntry->targets();
            std::unordered_map<std::string, int> mergedByName;
            for (flatbuffers::uoffset_t j = 0; j < activityEntries->size(); ++j) {
                auto targetEntry = activityEntries->Get(j);
                std::string actStr = targetEntry->activity()->str();
                mergedByName[actStr] += static_cast<int>(targetEntry->times());
            }
            ReuseEntryM entryMap;
            for (const auto &kv : mergedByName) {
                entryMap.emplace(std::make_shared<std::string>(kv.first), kv.second);
                BDLOG("SarsaAgent: load model hash: %" PRIu64 " %s %d", actionHash, kv.first.c_str(), kv.second);
            }
            if (!entryMap.empty()) {
                newModel.emplace(actionHash, std::move(entryMap));
            }
        }

        {
            std::lock_guard<std::mutex> lock(this->_reuseModelLock);
            this->_reuseModel.swap(newModel);
        }

        BLOG("SarsaAgent: loaded model contains %zu actions", this->_reuseModel.size());
    }

    void SarsaAgent::saveReuseModel(const std::string &modelFilepath) {
        flatbuffers::FlatBufferBuilder builder;
        std::vector<flatbuffers::Offset<fastbotx::ReuseEntry>> entries;

        // Take a snapshot of the reuse model under lock, then serialize off-lock.
        ReuseEntryIntMap snapshot;
        {
            std::lock_guard<std::mutex> lock(this->_reuseModelLock);
            snapshot = this->_reuseModel;
        }

        // Apply a simple exponential decay to long-lived counts so that very old
        // experience does not dominate forever. This keeps the effective history
        // window bounded without changing the on-disk schema.
        static const double kDecayFactor = 0.99; // Decay ~1% per save.
        if (kDecayFactor > 0.0 && kDecayFactor < 1.0) {
            for (auto it = snapshot.begin(); it != snapshot.end(); ) {
                ReuseEntryM &actMap = it->second;
                for (auto actIt = actMap.begin(); actIt != actMap.end(); ) {
                    int count = actIt->second;
                    int decayed = static_cast<int>(std::floor(count * kDecayFactor));
                    if (decayed <= 0) {
                        actIt = actMap.erase(actIt);
                    } else {
                        actIt->second = decayed;
                        ++actIt;
                    }
                }
                if (actMap.empty()) {
                    it = snapshot.erase(it);
                } else {
                    ++it;
                }
            }
        }

        // Merge by activity name so we write one row per activity (same string under different stringPtrs are merged).
        for (const auto &iter : snapshot) {
            uint64_t actionHash = iter.first;
            const ReuseEntryM &actMap = iter.second;
            std::unordered_map<std::string, int> byName;
            for (const auto &entry : actMap) {
                byName[*(entry.first)] += entry.second;
            }
            std::vector<flatbuffers::Offset<fastbotx::ActivityTimes>> targets;
            targets.reserve(byName.size());
            for (const auto &kv : byName) {
                auto actTimes = CreateActivityTimes(builder,
                                                    builder.CreateString(kv.first),
                                                    kv.second);
                targets.push_back(actTimes);
            }
            auto reuseEntry = CreateReuseEntry(builder,
                                               actionHash,
                                               builder.CreateVector(targets.data(), targets.size()));
            entries.push_back(reuseEntry);
        }

        auto modelBuf = CreateReuseModel(builder,
                                         builder.CreateVector(entries.data(), entries.size()));
        builder.Finish(modelBuf);

        // Determine final output path: use provided path if non-empty, otherwise use tmpSavePath.
        std::string finalPath = modelFilepath.empty() ? this->_tmpSavePath : modelFilepath;
        if (finalPath.empty()) {
            BLOGE("SarsaAgent: Cannot save model: output file path is empty");
            return;
        }

        // Write to a temporary file first, then atomically rename to final path for better robustness.
        std::string tempFilePath = finalPath + ".tmp";
        BLOG("SarsaAgent: save model to temporary path: %s (entries=%zu)", tempFilePath.c_str(), snapshot.size());

        std::ofstream out(tempFilePath, std::ios::binary);
        if (!out.is_open()) {
            BLOGE("SarsaAgent: Failed to open temporary file for writing: %s", tempFilePath.c_str());
            return;
        }
        out.write(reinterpret_cast<const char *>(builder.GetBufferPointer()),
                  static_cast<std::streamsize>(builder.GetSize()));
        out.close();

        if (out.fail()) {
            BLOGE("SarsaAgent: Failed to write model to temporary file: %s", tempFilePath.c_str());
            std::remove(tempFilePath.c_str());
            return;
        }

        if (std::rename(tempFilePath.c_str(), finalPath.c_str()) != 0) {
            BLOGE("SarsaAgent: Failed to rename temporary file to final file: %s -> %s",
                  tempFilePath.c_str(), finalPath.c_str());
            std::remove(tempFilePath.c_str());
            return;
        }

        BLOG("SarsaAgent: Model saved successfully to: %s (entries=%zu)", finalPath.c_str(), snapshot.size());
    }

    void SarsaAgent::saveReuseModelNow() {
        if (!_modelSavePath.empty()) {
            saveReuseModel(_modelSavePath);
        }
    }

    void SarsaAgent::threadModelStorage(const std::weak_ptr<SarsaAgent> &agent) {
        int saveIntervalMs = 1000 * 60 * 10; // 10 minutes
        // Sleep one full interval before first save, so we do not overwrite a good
        // on-disk model with an empty/small snapshot from the first few seconds.
        std::this_thread::sleep_for(std::chrono::milliseconds(saveIntervalMs));
        while (true) {
            auto locked = agent.lock();
            if (!locked) {
                break;
            }
            locked->saveReuseModel(locked->_modelSavePath);
            std::this_thread::sleep_for(std::chrono::milliseconds(saveIntervalMs));
        }
    }

} // namespace fastbotx

#endif // fastbotx_SarsaAgent_CPP_

