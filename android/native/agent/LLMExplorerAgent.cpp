/**
 * LLMExplorerAgent: knowledge-guided exploration with AIG.
 * Abstract states (rule-based), abstract actions with flags, app-wide selection, navigation.
 */
 /**
 * @authors Zhao Zhang
 */

#ifndef FASTBOTX_LLM_EXPLORER_AGENT_CPP_
#define FASTBOTX_LLM_EXPLORER_AGENT_CPP_

#include "LLMExplorerAgent.h"
#include "KnowledgeOrganizer.h"
#include "ContentAwareInputProvider.h"

#include "../utils.hpp"
#include "../desc/State.h"
#include "../desc/Action.h"
#include "../desc/Widget.h"
#include "../model/Model.h"
#include "../events/Preference.h"
#include "LLMTaskAgent.h"

#include <algorithm>
#include <cmath>
#include <sstream>
#include <cctype>

#include "../thirdpart/json/json.hpp"

namespace fastbotx {

    LLMExplorerAgent::LLMExplorerAgent(const ModelPtr &model)
            : AbstractAgent(model),
              _knowledgeOrganizer(std::make_shared<LlmKnowledgeOrganizer>()),
              _contentAwareInputProvider(std::make_shared<LlmContentAwareInputProvider>()) {
        this->_algorithmType = AlgorithmType::LLMExplorer;
        BLOG("LLMExplorerAgent: initialized (AIG, app-wide exploration)");
    }

    void LLMExplorerAgent::setKnowledgeOrganizer(const std::shared_ptr<IKnowledgeOrganizer> &organizer) {
        _knowledgeOrganizer = organizer;
    }

    void LLMExplorerAgent::setContentAwareInputProvider(const std::shared_ptr<IContentAwareInputProvider> &provider) {
        _contentAwareInputProvider = provider;
    }

    void LLMExplorerAgent::updateStrategy() {
        // Knowledge updates happen in moveForward (updateKnowledge).
    }

    void LLMExplorerAgent::onStateAbstractionChanged() {
        _abstractKeyToId.clear();
        _nextAbstractId = 1;
        _actionFlags.clear();
        _aigNextState.clear();
        _absStateToActionHashes.clear();
        _actionToGroup.clear();
        _groupToActionHashes.clear();
        _groupFunction.clear();
        _llmGroupingDoneForState.clear();
        _unexploredAbsIdActionPairs.clear();
        _navFailedActionKeys.clear();
        if (_contentAwareInputProvider) {
            _contentAwareInputProvider->onStateAbstractionChanged();
        }
        _navActionHashes.clear();
        _navTargetAbsId = 0;
        _navTargetActionHash = 0;
        _navRetryAfterRestart = false;
        _navFailedEdgeKey = 0;
        _navRetryRestartCount = 0;
        _navStepsInSession = 0;
        BDLOG("LLMExplorerAgent: state abstraction changed, cleared AIG and knowledge");
    }

    uintptr_t LLMExplorerAgent::computeAbstractStateKey(const StatePtr &state) const {
        if (!state) return 0;
        std::string activityStr = state->getActivityString() && state->getActivityString().get()
                                  ? *state->getActivityString() : "";
        WidgetKeyMask mask = DefaultWidgetKeyMask;
        ModelPtr model = _model.lock();
        if (model && !activityStr.empty()) {
            mask = model->getActivityKeyMask(activityStr);
            // Prefer structural key: if mask includes Text/ContentDesc, use Default for abstract match
            const auto textMask = static_cast<WidgetKeyMask>(WidgetKeyAttr::Text);
            const auto descMask = static_cast<WidgetKeyMask>(WidgetKeyAttr::ContentDesc);
            if ((mask & textMask) || (mask & descMask)) {
                mask = DefaultWidgetKeyMask;
            }
        }
        uintptr_t activityHash = (fastStringHash(activityStr) * 31U) << 5;
        const auto &widgets = state->getWidgets();
        std::vector<uintptr_t> widgetHashes;
        widgetHashes.reserve(widgets.size());
        for (const auto &w : widgets) {
            if (w) widgetHashes.push_back(w->hashWithMask(mask));
        }
        std::sort(widgetHashes.begin(), widgetHashes.end());
        uintptr_t mix = activityHash;
        for (size_t i = 0; i < widgetHashes.size(); ++i) {
            mix ^= (widgetHashes[i] * (31U + static_cast<uintptr_t>(i)));
            mix = (mix << 13) | (mix >> (sizeof(uintptr_t) * 8 - 13));
        }
        return mix != 0 ? mix : 1;
    }

    uintptr_t LLMExplorerAgent::getOrCreateAbstractStateId(uintptr_t key) {
        if (key == 0) return 0;
        auto it = _abstractKeyToId.find(key);
        if (it != _abstractKeyToId.end()) return it->second;
        uintptr_t id = _nextAbstractId++;
        _abstractKeyToId[key] = id;
        return id;
    }

    void LLMExplorerAgent::ensureAbstractActionsForState(uintptr_t absStateId, const StatePtr &state) {
        if (!state || absStateId == 0) return;
        for (const auto &a : state->getActions()) {
            if (!a || !a->isValid()) continue;
            uintptr_t actHash = a->hash();
            uintptr_t key = kActionFlagKey(absStateId, actHash);
            if (_actionFlags.find(key) == _actionFlags.end()) {
                _actionFlags[key] = LLMExplorerActionFlag::Unexplored;
                bool found = false;
                for (const auto &p : _unexploredAbsIdActionPairs) {
                    if (p.first == absStateId && p.second == actHash) { found = true; break; }
                }
                if (!found) _unexploredAbsIdActionPairs.push_back({absStateId, actHash});
            }
            _absStateToActionHashes[absStateId].insert(actHash);
        }
        if (_llmGroupingDoneForState.find(absStateId) == _llmGroupingDoneForState.end()) {
            tryLlmKnowledgeOrganization(absStateId, state);
        }
    }

    bool LLMExplorerAgent::tryLlmKnowledgeOrganization(uintptr_t absStateId, const StatePtr &state) {
        if (!state || absStateId == 0) return false;

        std::vector<ActivityStateActionPtr> validActions;
        for (const auto &a : state->getActions()) {
            if (a && a->isValid()) validActions.push_back(a);
        }
        if (validActions.empty()) {
            _llmGroupingDoneForState.insert(absStateId);
            BDLOG("LLMExplorerAgent: knowledge_org skip absStateId=%llu (no valid actions)",
                  (unsigned long long) absStateId);
            return true;
        }

        auto fallbackOneToOne = [this, absStateId](const std::vector<ActivityStateActionPtr> &actions) {
            for (const auto &a : actions) {
                uintptr_t actHash = a->hash();
                _actionToGroup[kActionFlagKey(absStateId, actHash)] = actHash;
                _groupToActionHashes[kGroupKey(absStateId, actHash)].insert(actHash);
            }
        };

        if (!_knowledgeOrganizer) {
            fallbackOneToOne(validActions);
            _llmGroupingDoneForState.insert(absStateId);
            BDLOG("LLMExplorerAgent: knowledge_org skip absStateId=%llu (no organizer), fallback 1:1",
                  (unsigned long long) absStateId);
            return true;
        }

        if (!Preference::inst() || !Preference::inst()->isLlmKnowledgeEnabled()) {
            fallbackOneToOne(validActions);
            _llmGroupingDoneForState.insert(absStateId);
            BDLOG("LLMExplorerAgent: knowledge_org skip absStateId=%llu (max.llm.knowledge=false), fallback 1:1",
                  (unsigned long long) absStateId);
            return true;
        }

        ModelPtr model = _model.lock();
        IKnowledgeOrganizer::Result res =
                _knowledgeOrganizer->organize(absStateId, validActions, model, kMinSameFunctionGroupSize);

        if (!res.success || res.groupIds.size() != validActions.size()) {
            fallbackOneToOne(validActions);
            _llmGroupingDoneForState.insert(absStateId);
            BDLOG("LLMExplorerAgent: knowledge_org organizer failed absStateId=%llu, fallback 1:1",
                  (unsigned long long) absStateId);
            return true;
        }

        std::unordered_set<uintptr_t> uniqueGroupIds;
        for (size_t i = 0; i < validActions.size(); ++i) {
            uintptr_t actHash = validActions[i]->hash();
            uintptr_t gid = res.groupIds[i];
            if (gid == 0) gid = actHash;
            _actionToGroup[kActionFlagKey(absStateId, actHash)] = gid;
            _groupToActionHashes[kGroupKey(absStateId, gid)].insert(actHash);
            uniqueGroupIds.insert(gid);
        }
        for (const auto &p : res.groupFunctions) {
            if (!p.second.empty()) {
                _groupFunction[kGroupKey(absStateId, p.first)] = p.second;
            }
        }

        BDLOG("LLMExplorerAgent: knowledge_org done absStateId=%llu groups=%zu",
              (unsigned long long) absStateId,
              static_cast<unsigned long long>(uniqueGroupIds.size()));
        _llmGroupingDoneForState.insert(absStateId);
        return true;
    }

    void LLMExplorerAgent::updateKnowledge(const StatePtr &srcState,
                                          const ActivityStateActionPtr &actionTaken,
                                          const StatePtr &tgtState) {
        if (!srcState || !actionTaken || !tgtState) return;
        uintptr_t srcKey = computeAbstractStateKey(srcState);
        uintptr_t tgtKey = computeAbstractStateKey(tgtState);
        uintptr_t srcAbsId = getOrCreateAbstractStateId(srcKey);
        uintptr_t tgtAbsId = getOrCreateAbstractStateId(tgtKey);

        ensureAbstractActionsForState(srcAbsId, srcState);
        // Defer ensure for target: only ensure when we are at that state in selectNewAction to reduce LLM calls.

        uintptr_t actHash = actionTaken->hash();
        uintptr_t flagKey = kActionFlagKey(srcAbsId, actHash);
        LLMExplorerActionFlag newFlag = (srcAbsId == tgtAbsId)
                                        ? LLMExplorerActionFlag::Ineffective
                                        : LLMExplorerActionFlag::Explored;
        _actionFlags[flagKey] = newFlag;
        removeUnexploredPair(srcAbsId, actHash);
        size_t groupSize = 1;
        uintptr_t groupId = actHash;
        auto itGroup = _actionToGroup.find(flagKey);
        if (itGroup != _actionToGroup.end()) groupId = itGroup->second;
        uintptr_t groupKey = kGroupKey(srcAbsId, groupId);
        auto itMembers = _groupToActionHashes.find(groupKey);
        if (itMembers != _groupToActionHashes.end()) {
            groupSize = itMembers->second.size();
            for (uintptr_t h : itMembers->second) {
                uintptr_t k = kActionFlagKey(srcAbsId, h);
                _actionFlags[k] = newFlag;
                removeUnexploredPair(srcAbsId, h);
            }
        }

        uintptr_t edgeKey = kAigEdgeKey(srcAbsId, actHash);
        bool isNewEdge = (_aigNextState.find(edgeKey) == _aigNextState.end());
        if (isNewEdge) {
            _aigNextState[edgeKey] = tgtAbsId;
        }
        BDLOG("LLMExplorerAgent: updateKnowledge src=%llu tgt=%llu actHash=0x%llx flag=%s newEdge=%d groupSize=%zu",
              (unsigned long long) srcAbsId, (unsigned long long) tgtAbsId, (unsigned long long) actHash,
              (newFlag == LLMExplorerActionFlag::Ineffective ? "Ineffective" : "Explored"), isNewEdge ? 1 : 0, groupSize);
    }

    std::pair<uintptr_t, uintptr_t> LLMExplorerAgent::selectExploreAction(const StatePtr &state,
                                                                          uintptr_t currentAbsId) {
        if (!state || currentAbsId == 0) return {0, 0};

        std::vector<uintptr_t> currentUnexplored;
        for (const auto &a : state->getActions()) {
            if (!a || !a->isValid()) continue;
            if (_validateFilter && !_validateFilter->include(a)) continue;
            uintptr_t key = kActionFlagKey(currentAbsId, a->hash());
            auto it = _actionFlags.find(key);
            if (it != _actionFlags.end() && it->second == LLMExplorerActionFlag::Unexplored) {
                currentUnexplored.push_back(a->hash());
            }
        }
        BDLOG("LLMExplorerAgent: explore current absId=%llu collected %zu unexplored",
              (unsigned long long) currentAbsId, currentUnexplored.size());
        if (!currentUnexplored.empty()) {
            std::uniform_int_distribution<size_t> dist(0, currentUnexplored.size() - 1);
            uintptr_t ah = currentUnexplored[dist(_rng)];
            BDLOG("LLMExplorerAgent: explore from_current absId=%llu n=%zu chosenActionHash=0x%llx",
                  (unsigned long long) currentAbsId, currentUnexplored.size(), (unsigned long long) ah);
            return {currentAbsId, ah};
        }

        BDLOG("LLMExplorerAgent: explore current absId=%llu has no unexplored, try app_wide", (unsigned long long) currentAbsId);
        std::vector<std::pair<uintptr_t, uintptr_t>> candidates;
        for (const auto &p : _unexploredAbsIdActionPairs) {
            uintptr_t key = kActionFlagKey(p.first, p.second);
            if (_navFailedActionKeys.find(key) != _navFailedActionKeys.end()) continue;
            candidates.push_back(p);
        }
        if (candidates.empty()) {
            BDLOG("LLMExplorerAgent: explore app_wide empty (or all nav-failed), no unexplored -> fallbackPickAction");
            return {0, 0};
        }
        std::uniform_int_distribution<size_t> dist(0, candidates.size() - 1);
        size_t idx = dist(_rng);
        uintptr_t targetAbsId = candidates[idx].first;
        uintptr_t chosenActionHash = candidates[idx].second;
        BDLOG("LLMExplorerAgent: explore from_app_wide currentAbsId=%llu targetAbsId=%llu n=%zu chosenActionHash=0x%llx",
              (unsigned long long) currentAbsId, (unsigned long long) targetAbsId, candidates.size(), (unsigned long long) chosenActionHash);
        return {targetAbsId, chosenActionHash};
    }

    bool LLMExplorerAgent::findNavigatePath(uintptr_t currentAbsId, uintptr_t targetAbsId,
                                            std::vector<uintptr_t> &outActionHashes,
                                            uintptr_t excludeEdgeKey) {
        outActionHashes.clear();
        if (currentAbsId == targetAbsId) return true;
        std::unordered_map<uintptr_t, std::pair<uintptr_t, uintptr_t>> parent;
        parent[currentAbsId] = {0, 0};
        std::deque<uintptr_t> q;
        q.push_back(currentAbsId);
        while (!q.empty()) {
            if (parent.size() > kMaxBFSDepth) return false;
            uintptr_t u = q.front();
            q.pop_front();
            auto itActions = _absStateToActionHashes.find(u);
            if (itActions == _absStateToActionHashes.end()) continue;
            for (uintptr_t actHash : itActions->second) {
                uintptr_t edgeKey = kAigEdgeKey(u, actHash);
                if (excludeEdgeKey != 0 && edgeKey == excludeEdgeKey) continue;
                auto itNext = _aigNextState.find(edgeKey);
                if (itNext == _aigNextState.end()) continue;
                uintptr_t v = itNext->second;
                if (parent.find(v) != parent.end()) continue;
                parent[v] = {u, actHash};
                if (v == targetAbsId) {
                    std::vector<uintptr_t> rev;
                    for (uintptr_t cur = targetAbsId; cur != currentAbsId;) {
                        auto p = parent[cur];
                        rev.push_back(p.second);
                        cur = p.first;
                    }
                    outActionHashes.assign(rev.rbegin(), rev.rend());
                    return true;
                }
                q.push_back(v);
            }
        }
        return false;
    }

    ActivityStateActionPtr LLMExplorerAgent::findActionByHash(const StatePtr &state,
                                                              uintptr_t actionHash) const {
        if (!state) return nullptr;
        for (const auto &a : state->getActions()) {
            if (a && a->hash() == actionHash) return a;
        }
        return nullptr;
    }

    void LLMExplorerAgent::removeUnexploredPair(uintptr_t absId, uintptr_t actHash) {
        auto it = std::remove_if(_unexploredAbsIdActionPairs.begin(), _unexploredAbsIdActionPairs.end(),
                                 [absId, actHash](const std::pair<uintptr_t, uintptr_t> &p) {
                                     return p.first == absId && p.second == actHash;
                                 });
        if (it != _unexploredAbsIdActionPairs.end())
            _unexploredAbsIdActionPairs.erase(it, _unexploredAbsIdActionPairs.end());
    }

    ActionPtr LLMExplorerAgent::fallbackPickAction() const {
        StatePtr state = this->_newState;
        if (!state) return nullptr;
        const ActivityStateActionPtrVec &actions = state->getActions();
        ActivityStateActionPtr backAction = state->getBackAction();
        std::vector<ActivityStateActionPtr> valid;
        for (const auto &a : actions) {
            if (!a || !a->isValid()) continue;
            if (a->isBack() || a->isNop()) continue;
            if (_validateFilter && !_validateFilter->include(a)) continue;
            valid.push_back(a);
        }
        if (valid.empty() && backAction && (!_validateFilter || _validateFilter->include(backAction))) {
            return backAction;
        }
        if (valid.empty()) return nullptr;
        std::uniform_int_distribution<size_t> dist(0, valid.size() - 1);
        return valid[dist(_rng)];
    }

    void LLMExplorerAgent::moveForward(StatePtr nextState) {
        StatePtr fromState = this->_newState;
        ActivityStateActionPtr actionTaken = this->_newAction;
        bool wasNavStep = actionTaken && !_navActionHashes.empty();
        uintptr_t expectedHash = wasNavStep ? _navActionHashes.front() : 0;

        AbstractAgent::moveForward(nextState);

        if (fromState && actionTaken && nextState) {
            updateKnowledge(fromState, actionTaken, nextState);
        }

        if (actionTaken && !_navActionHashes.empty()) {
            if (actionTaken->hash() == _navActionHashes.front()) {
                _navActionHashes.pop_front();
                _navStepsInSession++;
            }
        }

        // UpdateNavigatePath (paper Algorithm 2 step 5): if we were in nav and ended off-path, replan from current.
        if (_navTargetAbsId != 0 && fromState && actionTaken && nextState) {
            uintptr_t srcAbsId = getOrCreateAbstractStateId(computeAbstractStateKey(fromState));
            uintptr_t currentAbsId = getOrCreateAbstractStateId(computeAbstractStateKey(nextState));
            uintptr_t edgeKey = kAigEdgeKey(srcAbsId, actionTaken->hash());
            bool executedExpectedNav = wasNavStep && (actionTaken->hash() == expectedHash);
            uintptr_t expectedTgt = 0;
            auto itEdge = _aigNextState.find(edgeKey);
            if (itEdge != _aigNextState.end()) expectedTgt = itEdge->second;
            bool offPath = !executedExpectedNav || (executedExpectedNav && currentAbsId != expectedTgt);
            if (offPath) {
                std::vector<uintptr_t> newPath;
                if (findNavigatePath(currentAbsId, _navTargetAbsId, newPath) && !newPath.empty()) {
                    _navActionHashes.assign(newPath.begin(), newPath.end());
                    BDLOG("LLMExplorerAgent: UpdateNavigatePath replan ok, steps=%zu", newPath.size());
                } else {
                    // Fault-tolerant: try alternative path (exclude the edge that led here).
                    if (findNavigatePath(currentAbsId, _navTargetAbsId, newPath, edgeKey) && !newPath.empty()) {
                        _navActionHashes.assign(newPath.begin(), newPath.end());
                        BDLOG("LLMExplorerAgent: alternative path ok, steps=%zu", newPath.size());
                    } else {
                        _navActionHashes.clear();
                        _navRetryAfterRestart = true;
                        _navFailedEdgeKey = edgeKey;
                        BDLOG("LLMExplorerAgent: nav failed, will retry after restart (target=%llu)", (unsigned long long) _navTargetAbsId);
                    }
                }
            }
        }
    }

    ActionPtr LLMExplorerAgent::selectNewAction() {
        StatePtr state = this->_newState;
        if (!state) {
            BDLOG("LLMExplorerAgent: selectNewAction no state, fallback");
            return fallbackPickAction();
        }

        int blockTimes = getCurrentStateBlockTimes();
        if (blockTimes > kBlockCleanRestartThreshold) {
            _navActionHashes.clear();
            _navTargetAbsId = 0;
            _navTargetActionHash = 0;
            _navRetryAfterRestart = false;
            _navFailedEdgeKey = 0;
            _navRetryRestartCount = 0;
            _navStepsInSession = 0;
            BDLOG("LLMExplorerAgent: blocked %d steps, CLEAN_RESTART", blockTimes);
            return Action::CLEAN_RESTART;
        }
        if (blockTimes > kBlockDeepLinkThreshold) {
            _navActionHashes.clear();
            _navTargetAbsId = 0;
            _navTargetActionHash = 0;
            _navRetryAfterRestart = false;
            _navFailedEdgeKey = 0;
            _navRetryRestartCount = 0;
            _navStepsInSession = 0;
            BDLOG("LLMExplorerAgent: blocked %d steps, DEEP_LINK", blockTimes);
            return Action::DEEP_LINK;
        }
        if (blockTimes > kBlockBackThreshold) {
            const ActivityStateActionPtrVec &actions = state->getActions();
            ActivityStateActionPtr backAction = state->getBackAction();
            int validNonBack = 0;
            for (const auto &a : actions) {
                if (!a || !a->isValid()) continue;
                if (a->isBack() || a->isNop()) continue;
                if (!_validateFilter || _validateFilter->include(a)) { validNonBack++; break; }
            }
            if (validNonBack == 0 && backAction && (!_validateFilter || _validateFilter->include(backAction))) {
                _navActionHashes.clear();
                _navTargetAbsId = 0;
                _navTargetActionHash = 0;
                _navRetryAfterRestart = false;
                _navFailedEdgeKey = 0;
                _navRetryRestartCount = 0;
                _navStepsInSession = 0;
                BDLOG("LLMExplorerAgent: blocked %d steps, BACK", blockTimes);
                return backAction;
            }
        }

        uintptr_t currentKey = computeAbstractStateKey(state);
        uintptr_t currentAbsId = getOrCreateAbstractStateId(currentKey);
        ensureAbstractActionsForState(currentAbsId, state);

        std::unordered_map<uintptr_t, ActivityStateActionPtr> stateActionByHash;
        for (const auto &a : state->getActions()) {
            if (a && a->isValid()) stateActionByHash[a->hash()] = a;
        }
        auto findInState = [&stateActionByHash](uintptr_t hash) -> ActivityStateActionPtr {
            auto it = stateActionByHash.find(hash);
            return it != stateActionByHash.end() ? it->second : nullptr;
        };

        // Fault-tolerant: after nav failure we may retry from current (or after restart).
        if (_navRetryAfterRestart && _navTargetAbsId != 0) {
            std::vector<uintptr_t> pathHashes;
            if (findNavigatePath(currentAbsId, _navTargetAbsId, pathHashes) && !pathHashes.empty()) {
                _navActionHashes.assign(pathHashes.begin(), pathHashes.end());
                _navRetryAfterRestart = false;
                _navFailedEdgeKey = 0;
                _navRetryRestartCount = 0;
                ActivityStateActionPtr firstAction = findInState(pathHashes.front());
                if (firstAction && (!_validateFilter || _validateFilter->include(firstAction))) {
                    BDLOG("LLMExplorerAgent: nav retry ok after restart, steps=%zu", pathHashes.size());
                    return firstAction;
                }
            }
            if (_navRetryRestartCount >= kMaxNavRetryRestarts) {
                if (_navFailedEdgeKey != 0) {
                    _aigNextState.erase(_navFailedEdgeKey);
                    BDLOG("LLMExplorerAgent: remove failed edge from AIG, give up nav target=%llu", (unsigned long long) _navTargetAbsId);
                }
                _navTargetAbsId = 0;
                _navTargetActionHash = 0;
                _navRetryAfterRestart = false;
                _navFailedEdgeKey = 0;
                _navRetryRestartCount = 0;
                _navStepsInSession = 0;
            } else {
                _navRetryRestartCount++;
                BDLOG("LLMExplorerAgent: nav unreachable, CLEAN_RESTART retry %d/%d", _navRetryRestartCount, kMaxNavRetryRestarts);
                return Action::CLEAN_RESTART;
            }
        }

        if (!_navActionHashes.empty()) {
            if (_navStepsInSession >= kMaxNavStepsPerSession) {
                if (_navTargetAbsId != 0 && _navTargetActionHash != 0) {
                    _navFailedActionKeys.insert(kActionFlagKey(_navTargetAbsId, _navTargetActionHash));
                    BDLOG("LLMExplorerAgent: nav steps exceeded %d, mark target failed and clear nav", kMaxNavStepsPerSession);
                }
                _navActionHashes.clear();
                _navTargetAbsId = 0;
                _navTargetActionHash = 0;
                _navStepsInSession = 0;
            } else {
                uintptr_t nextActionHash = _navActionHashes.front();
                ActivityStateActionPtr a = findInState(nextActionHash);
                if (a && (!_validateFilter || _validateFilter->include(a))) {
                    BDLOG("LLMExplorerAgent: nav step (remaining %zu)", _navActionHashes.size());
                    return a;
                }
                if (_navTargetAbsId != 0 && _navTargetActionHash != 0) {
                    _navFailedActionKeys.insert(kActionFlagKey(_navTargetAbsId, _navTargetActionHash));
                    BDLOG("LLMExplorerAgent: nav step action not in state, mark target failed");
                }
                BDLOG("LLMExplorerAgent: nav step action not in state (remaining %zu), clear nav", _navActionHashes.size());
                _navActionHashes.clear();
                _navTargetAbsId = 0;
                _navTargetActionHash = 0;
                _navRetryAfterRestart = false;
                _navFailedEdgeKey = 0;
                _navRetryRestartCount = 0;
                _navStepsInSession = 0;
            }
        }

        std::pair<uintptr_t, uintptr_t> exploreChoice = selectExploreAction(state, currentAbsId);
        uintptr_t targetAbsId = exploreChoice.first;
        uintptr_t chosenActionHash = exploreChoice.second;
        if (chosenActionHash == 0) {
            BDLOG("LLMExplorerAgent: explore no unexplored, fallback");
            return fallbackPickAction();
        }

        ActivityStateActionPtr chosenAction = findInState(chosenActionHash);
        if (chosenAction && (!_validateFilter || _validateFilter->include(chosenAction))) {
            BDLOG("LLMExplorerAgent: explore action in current state");
            return chosenAction;
        }

        if (targetAbsId != 0 && targetAbsId != currentAbsId) {
            std::vector<uintptr_t> pathHashes;
            if (findNavigatePath(currentAbsId, targetAbsId, pathHashes) && !pathHashes.empty()) {
                _navActionHashes.assign(pathHashes.begin(), pathHashes.end());
                _navTargetAbsId = targetAbsId;
                _navTargetActionHash = chosenActionHash;
                _navRetryRestartCount = 0;
                _navStepsInSession = 0;
                uintptr_t firstHash = _navActionHashes.front();
                ActivityStateActionPtr firstAction = findInState(firstHash);
                if (firstAction && (!_validateFilter || _validateFilter->include(firstAction))) {
                    BDLOG("LLMExplorerAgent: start nav to absState=%llu steps=%zu",
                          (unsigned long long) targetAbsId, pathHashes.size());
                    return firstAction;
                }
                _navActionHashes.clear();
                _navTargetAbsId = 0;
                _navTargetActionHash = 0;
                BDLOG("LLMExplorerAgent: start nav first action invalid, clear nav");
            } else {
                _navActionHashes.clear();
                _navTargetAbsId = 0;
                _navTargetActionHash = 0;
                BDLOG("LLMExplorerAgent: findNavigatePath failed current=%llu target=%llu, fallback",
                      (unsigned long long) currentAbsId, (unsigned long long) targetAbsId);
            }
        }

        BDLOG("LLMExplorerAgent: fallback (explore/nav not applicable)");
        return fallbackPickAction();
    }

    std::string LLMExplorerAgent::getInputTextForAction(const StatePtr &state, const ActionPtr &action) const {
        if (!Preference::inst() || !Preference::inst()->isLlmContextAwareInputEnabled()) return "";
        if (!_contentAwareInputProvider) return "";
        ModelPtr model = _model.lock();
        return _contentAwareInputProvider->getInputTextForAction(state, action, model);
    }

}  // namespace fastbotx

#endif  // FASTBOTX_LLM_EXPLORER_AGENT_CPP_
