/*
 * This code is licensed under the Fastbot license. You may obtain a copy of this license in the LICENSE.txt file in the root directory of this source tree.
 */
/**
 * @author Zhao Zhang
 */

#ifndef FASTBOTX_BFS_AGENT_CPP_
#define FASTBOTX_BFS_AGENT_CPP_

#include "BFSAgent.h"

#include "../utils.hpp"
#include "../desc/State.h"
#include "../desc/Action.h"

namespace fastbotx {

    BFSAgent::BFSAgent(const ModelPtr &model)
            : AbstractAgent(model),
              _stateBlockCounter(0),
              _lastStateHash(0),
              _activityBlockCounter(0) {
        this->_algorithmType = AlgorithmType::BFS;
        BLOG("BFSAgent: initialized (breadth-first exploration)");
    }

    static std::uint64_t makeEdgeKey(uintptr_t stateHash, uintptr_t actionHash) {
        return (static_cast<std::uint64_t>(stateHash) << 32) ^
               static_cast<std::uint64_t>(actionHash);
    }

    void BFSAgent::updateStrategy() {
        // BFSAgent is purely structural: no Q-values or reuse model to update.
    }

    void BFSAgent::moveForward(StatePtr nextState) {
        AbstractAgent::moveForward(nextState);

        StatePtr cur = this->_currentState;
        StatePtr prev = this->_lastState;

        if (!cur) {
            _queue.clear();
            _stateInQueue.clear();
            _stateDepth.clear();
            _currentDepth = 0;
            _activityInQueueCount.clear();
            _selectCallCount = 0;
            _visitedStates.clear();
            _visitedActivities.clear();
            _edgeStats.clear();
            _edgeToTarget.clear();
            _stateCoverage.clear();
            _rootActivity.clear();
            _stateBlockCounter = 0;
            _lastStateHash = 0;
            _activityBlockCounter = 0;
            _lastActivity.clear();
            _recentActivities.clear();
            return;
        }

        uintptr_t h = cur->hash();

        if (h == _lastStateHash) {
            _stateBlockCounter++;
            if (_stateBlockCounter >= 12) {
                BDLOG("BFSAgent: state hash unchanged for %d consecutive steps (hash=%lu)",
                      _stateBlockCounter, static_cast<unsigned long>(h));
            }
        } else {
            if (_stateBlockCounter > 0) {
                BDLOG("BFSAgent: state hash changed, reset block counter from %d to 0 (old=%lu, new=%lu)",
                      _stateBlockCounter, static_cast<unsigned long>(_lastStateHash),
                      static_cast<unsigned long>(h));
            }
            _stateBlockCounter = 0;
            _lastStateHash = h;
        }

        bool newActivity = false;
        stringPtr actPtr = cur->getActivityString();
        if (actPtr) {
            auto ins = _visitedActivities.insert(*actPtr);
            newActivity = ins.second;
            if (_rootActivity.empty()) {
                _rootActivity = *actPtr;
                BDLOG("BFSAgent: root activity set to %s", _rootActivity.c_str());
            }
            int validNonBack = 0;
            for (const auto &a : cur->getActions()) {
                if (!a || !a->isValid()) continue;
                if (a->isBack() || a->isNop()) continue;
                if (!_validateFilter || _validateFilter->include(a)) { validNonBack++; break; }
            }
            if (validNonBack == 0) {
                if (*actPtr == _lastActivity) {
                    _activityBlockCounter++;
                    if (_activityBlockCounter >= 10) {
                        BDLOG("BFSAgent: same activity + only BACK for %d steps (activity=%s)",
                              _activityBlockCounter, actPtr->c_str());
                    }
                } else {
                    _activityBlockCounter = 1;
                    _lastActivity = *actPtr;
                }
            } else {
                _activityBlockCounter = 0;
                _lastActivity = *actPtr;
            }
            _recentActivities.push_back(*actPtr);
            while (_recentActivities.size() > kRecentActivityWindowSize) {
                _recentActivities.pop_front();
            }
        } else {
            _activityBlockCounter = 0;
            _lastActivity.clear();
        }

        bool newState = _visitedStates.insert(h).second;
        if (prev && this->_currentAction) {
            uintptr_t prevHash = prev->hash();
            uintptr_t actHash = this->_currentAction->hash();
            std::uint64_t key = makeEdgeKey(prevHash, actHash);
            auto &stats = _edgeStats[key];
            stats.total += 1;
            if (newState) stats.newStates += 1;
            if (newActivity) stats.newActivities += 1;
        }

        if (this->_currentAction && this->_currentAction->requireTarget() && nextState) {
            std::uint64_t edgeKey = makeEdgeKey(cur->hash(), this->_currentAction->hash());
            _edgeToTarget[edgeKey] = nextState->hash();
            const ActivityStateActionPtrVec &targetActions = nextState->getActions();
            int total = 0;
            int visited = 0;
            for (const auto &a : targetActions) {
                if (!a) continue;
                if (a->requireTarget()) {
                    total++;
                    if (a->getVisitedCount() > 0) visited++;
                }
            }
            _stateCoverage[nextState->hash()] = std::make_pair(total, visited);
        }

        // Depth from root: cur's depth = prev's depth + 1 (or 0 if no prev).
        int parentDepth = -1;
        if (prev) {
            auto it = _stateDepth.find(prev->hash());
            if (it != _stateDepth.end()) parentDepth = it->second;
        }
        int curDepth = (parentDepth >= 0) ? (parentDepth + 1) : 0;
        _stateDepth[h] = curDepth;

        // BFS: enqueue new state at the back if not already in queue (no stack trim).
        // Activity-level dedup: skip enqueue if this activity already has a frame in queue (optional, reduces queue size).
        stringPtr actPtrForQueue = cur->getActivityString();
        std::string actKey = actPtrForQueue ? *actPtrForQueue : "";
        int actCount = 0;
        if (!actKey.empty()) {
            auto it = _activityInQueueCount.find(actKey);
            actCount = (it != _activityInQueueCount.end()) ? it->second : 0;
        }
        if (_stateInQueue.find(h) == _stateInQueue.end() && actCount == 0) {
            Frame f;
            f.state = cur;
            f.depth = curDepth;
            const ActivityStateActionPtrVec &actions = cur->getActions();
            f.order.resize(actions.size());
            for (size_t i = 0; i < actions.size(); ++i) {
                f.order[i] = i;
            }
            if (!f.order.empty()) {
                std::shuffle(f.order.begin(), f.order.end(), _rng);
            }
            f.nextIndex = 0;
            _queue.push_back(std::move(f));
            _stateInQueue.insert(h);
            if (!actKey.empty()) {
                _activityInQueueCount[actKey]++;
            }
        }
    }

    ActionPtr BFSAgent::selectNewAction() {
        _selectCallCount++;

        if (this->_newState) {
            if (this->_newState->hash() != _lastStateHash) {
                _stateBlockCounter = 0;
                _lastStateHash = this->_newState->hash();
                BDLOG("BFSAgent: state changed (hash=%lu), reset state block counter", static_cast<unsigned long>(_lastStateHash));
            }
            stringPtr actStr = this->_newState->getActivityString();
            if (actStr && *actStr != _lastActivity) {
                _activityBlockCounter = 0;
                _lastActivity = *actStr;
                BDLOG("BFSAgent: activity changed (%s), reset activity block counter", actStr->c_str());
            }
        }

        StatePtr state = (!_queue.empty() && _queue.front().state) ? _queue.front().state : this->_newState;
        if (this->_newState && !_queue.empty() && _queue.front().state &&
            this->_newState->hash() != _queue.front().state->hash()) {
            state = this->_newState;
        }

        // Ensure root activity is set before any BACK/root check (first time on 首页, moveForward may not have run yet).
        if (_rootActivity.empty() && state) {
            stringPtr actStr = state->getActivityString();
            if (actStr && !actStr->empty()) {
                _rootActivity = *actStr;
                BDLOG("BFSAgent: root activity set in selectNewAction to %s", _rootActivity.c_str());
            }
        }

        if (!state) {
            BDLOG("BFSAgent: state is null, fallback to randomPickAction");
            if (this->_newState) {
                return this->_newState->randomPickAction(this->_validateFilter);
            }
            return nullptr;
        }

        constexpr int kBlockBackThreshold = 5;
        constexpr int kBlockFuzzThreshold = 10;
        constexpr int kBlockCleanRestartThreshold = 15;
        int blockTimes = std::max(_stateBlockCounter, _activityBlockCounter);

        if (blockTimes > kBlockCleanRestartThreshold) {
            _stateBlockCounter = 0;
            _lastStateHash = 0;
            _activityBlockCounter = 0;
            _lastActivity.clear();
            _queue.clear();
            _stateInQueue.clear();
            _stateDepth.clear();
            _currentDepth = 0;
            _activityInQueueCount.clear();
            _selectCallCount = 0;
            BDLOG("BFSAgent: blocked %d steps (>%d), CLEAN_RESTART (always)", blockTimes, kBlockCleanRestartThreshold);
            return Action::CLEAN_RESTART;
        }

        bool tryDeepLink = (blockTimes > kBlockFuzzThreshold) ||
                           (inTarpit() && blockTimes > kBlockBackThreshold);
        if (tryDeepLink) {
            _selectCallCount = 0;
            _stateBlockCounter++;
            _activityBlockCounter++;
            BDLOG("BFSAgent: blocked %d steps (tarpit=%d), DEEP_LINK (try unexplored activity)", blockTimes, inTarpit() ? 1 : 0);
            return Action::DEEP_LINK;
        }

        // Coverage-driven DEEP_LINK (Delm-style): every N steps try DEEP_LINK to encourage new activity coverage.
        if (_selectCallCount >= kCoverageDrivenDeepLinkInterval) {
            _selectCallCount = 0;
            BDLOG("BFSAgent: coverage-driven DEEP_LINK (every %d steps)", kCoverageDrivenDeepLinkInterval);
            return Action::DEEP_LINK;
        }

        if (blockTimes > kBlockBackThreshold) {
            const ActivityStateActionPtrVec &actions = state->getActions();
            int validNonBackActionCount = 0;
            ActivityStateActionPtr backAction = nullptr;
            for (const auto &a : actions) {
                if (!a || !a->isValid()) continue;
                if (a->isBack()) {
                    backAction = a;
                    continue;
                }
                if (a->isNop()) continue;
                if (!_validateFilter || _validateFilter->include(a)) {
                    validNonBackActionCount++;
                }
            }
            if (validNonBackActionCount == 0 && backAction) {
                stringPtr actStr = state->getActivityString();
                bool isRootActivity = (actStr && !_rootActivity.empty() && *actStr == _rootActivity);
                bool isRootState = (_queue.size() == 1u && !_queue.empty() && _queue.front().state == state);
                if (isRootState && isRootActivity) {
                    BDLOG("BFSAgent: root + only BACK, skip return BACK (let blockTimes grow to FUZZ/CLEAN_RESTART)");
                } else {
                    _stateBlockCounter = 0;
                    _lastStateHash = 0;
                    _activityBlockCounter = 0;
                    _lastActivity.clear();
                    BDLOG("BFSAgent: blocked %d steps (>%d), BACK (only action)", blockTimes, kBlockBackThreshold);
                    if ((!_validateFilter || _validateFilter->include(backAction)))
                        return backAction;
                }
            }
        }

        {
            constexpr int kSelfRescueThreshold = 12;
            if (blockTimes >= kSelfRescueThreshold) {
                BDLOG("BFSAgent: state blocked for %d steps (hash=%lu), triggering self-rescue",
                      blockTimes, static_cast<unsigned long>(state ? state->hash() : 0));
                const ActivityStateActionPtrVec &actions = state->getActions();
                ActivityStateActionPtr candidate = nullptr;

                for (const auto &a : actions) {
                    if (!a) continue;
                    if (!a->isValid()) continue;
                    if (a->isBack() || a->isNop()) continue;
                    if (!a->isVisited() &&
                        (!_validateFilter || _validateFilter->include(a))) {
                        candidate = a;
                        break;
                    }
                }

                if (!candidate) {
                    for (const auto &a : actions) {
                        if (!a) continue;
                        if (!a->isValid()) continue;
                        if (a->isBack() || a->isNop()) continue;
                        if (!_validateFilter || _validateFilter->include(a)) {
                            candidate = a;
                            break;
                        }
                    }
                }

                if (candidate) {
                    BDLOG("BFSAgent: self-rescue selected action %s", candidate->toString().c_str());
                    return candidate;
                }

                BDLOG("BFSAgent: self-rescue found no non-BACK candidate, continue with normal BFS");
            }
        }

        // If _newState is not at queue front (e.g. after DEEP_LINK/CLEAN_RESTART), make it front
        // so we select actions for the current UI state; otherwise we would pick from an old state
        // the device is no longer at (correctness bug).
        if (state && state == this->_newState && !_queue.empty() && _queue.front().state &&
            _queue.front().state->hash() != state->hash()) {
            uintptr_t sh = state->hash();
            stringPtr actPtrSync = state->getActivityString();
            std::string actKey = actPtrSync ? *actPtrSync : "";
            Frame f;
            f.state = state;
            const ActivityStateActionPtrVec &actions = state->getActions();
            f.order.resize(actions.size());
            for (size_t i = 0; i < actions.size(); ++i) {
                f.order[i] = i;
            }
            if (!f.order.empty()) {
                std::shuffle(f.order.begin(), f.order.end(), _rng);
            }
            f.nextIndex = 0;
            // Sync-to-front: treat as new root (e.g. after DEEP_LINK/CLEAN_RESTART).
            auto depthIt = _stateDepth.find(sh);
            f.depth = (depthIt != _stateDepth.end()) ? depthIt->second : 0;
            if (_stateInQueue.find(sh) != _stateInQueue.end()) {
                // State already in queue: remove one occurrence so we can put it at front
                // without duplicating; _stateInQueue unchanged (state still in queue).
                for (auto it = _queue.begin(); it != _queue.end(); ++it) {
                    if (it->state && it->state->hash() == sh) {
                        stringPtr remAct = it->state->getActivityString();
                        if (remAct && !remAct->empty()) {
                            auto acIt = _activityInQueueCount.find(*remAct);
                            if (acIt != _activityInQueueCount.end() && --acIt->second <= 0)
                                _activityInQueueCount.erase(acIt);
                        }
                        _queue.erase(it);
                        break;
                    }
                }
            } else {
                _stateInQueue.insert(sh);
            }
            _queue.push_front(std::move(f));
            if (!actKey.empty()) {
                _activityInQueueCount[actKey]++;
            }
        }

        // BFS loop: process front of queue; when frame exhausted, pop_front and continue.
        // Strict level-order: only process frames at _currentDepth; move deeper frames to back.
        while (!_queue.empty()) {
            Frame &frame = _queue.front();
            StatePtr s = frame.state;
            if (!s) {
                _queue.pop_front();
                continue;
            }
            if (frame.depth > _currentDepth) {
                _currentDepth = frame.depth;
                Frame f = std::move(_queue.front());
                _queue.pop_front();
                _queue.push_back(std::move(f));
                continue;
            }
            if (frame.depth < _currentDepth) {
                _currentDepth = frame.depth;
            }

            const ActivityStateActionPtrVec &actions = s->getActions();
            if (frame.order.size() != actions.size()) {
                frame.order.resize(actions.size());
                for (size_t i = 0; i < actions.size(); ++i) {
                    frame.order[i] = i;
                }
                if (!frame.order.empty()) {
                    std::shuffle(frame.order.begin(), frame.order.end(), _rng);
                }
                frame.nextIndex = 0;
            }

            ssize_t bestUnvisitedTarget = -1;
            ssize_t bestUnvisited = -1;
            ssize_t bestReusable = -1;
            double bestReusableScore = -1.0;

            for (size_t pos = frame.nextIndex; pos < frame.order.size(); ++pos) {
                size_t i = frame.order[pos];
                const auto &action = actions[i];
                if (!action) continue;
                if (_validateFilter && !_validateFilter->include(action)) continue;
                if (!action->isValid()) continue;

                bool isVisited = action->isVisited();
                bool requiresTarget = action->requireTarget();
                double typeWeight = getActionTypeWeight(action);

                if (!isVisited && requiresTarget) {
                    if (bestUnvisitedTarget < 0 ||
                        typeWeight > getActionTypeWeight(actions[static_cast<size_t>(bestUnvisitedTarget)])) {
                        bestUnvisitedTarget = static_cast<ssize_t>(i);
                    }
                    continue;
                }
                if (!isVisited) {
                    if (bestUnvisited < 0 ||
                        typeWeight > getActionTypeWeight(actions[static_cast<size_t>(bestUnvisited)])) {
                        bestUnvisited = static_cast<ssize_t>(i);
                    }
                    continue;
                }
                if (!s->isSaturated(action) && !isActionSaturatedByTargetState(s, action)) {
                    double score = getEdgeNoveltyScore(s, action) * typeWeight;
                    // Slight preference for shallower frames (level-order bias).
                    double depthWeight = 1.0 / (1.0 + 0.05 * static_cast<double>(frame.depth));
                    score *= depthWeight;
                    if (score > bestReusableScore) {
                        bestReusableScore = score;
                        bestReusable = static_cast<ssize_t>(i);
                    }
                }
            }
            frame.nextIndex = actions.size();

            if (bestUnvisitedTarget >= 0) {
                const auto &action = actions[static_cast<size_t>(bestUnvisitedTarget)];
                BDLOG("BFSAgent: select unvisited target action %s", action->toString().c_str());
                return action;
            }
            if (bestUnvisited >= 0) {
                const auto &action = actions[static_cast<size_t>(bestUnvisited)];
                BDLOG("BFSAgent: select unvisited action %s", action->toString().c_str());
                return action;
            }
            if (bestReusable >= 0) {
                const auto &action = actions[static_cast<size_t>(bestReusable)];
                BDLOG("BFSAgent: reuse visited non-saturated action %s (noveltyScore=%.3f)",
                      action->toString().c_str(), bestReusableScore);
                return action;
            }

            // Frame exhausted: optionally return BACK to parent layer (BFS layer switching, like DFS backtrack).
            ActivityStateActionPtr backAction = s->getBackAction();
            bool isRootState = (_queue.size() == 1u);
            bool isRootActivity = false;
            stringPtr currentActivity = s->getActivityString();
            if (currentActivity && !_rootActivity.empty() && *currentActivity == _rootActivity) {
                isRootActivity = true;
            }
            uintptr_t exhaustedHash = s->hash();
            _stateInQueue.erase(exhaustedHash);
            if (currentActivity && !currentActivity->empty()) {
                auto acIt = _activityInQueueCount.find(*currentActivity);
                if (acIt != _activityInQueueCount.end() && --acIt->second <= 0) {
                    _activityInQueueCount.erase(acIt);
                }
            }
            _queue.pop_front();
            if (!isRootState && !isRootActivity && backAction && backAction->isValid() &&
                (!_validateFilter || _validateFilter->include(backAction))) {
                BDLOG("BFSAgent: frame exhausted, return BACK to parent layer");
                return backAction;
            }
            if (isRootState || isRootActivity) {
                BDLOG("BFSAgent: root state/activity exhausted, skip BACK to avoid exit loop");
            }
        }

        BDLOG("BFSAgent: BFS queue exhausted, fallback to randomPickAction (preferring non-BACK)");

        // Use current UI state for fallback; state may be a previously-popped queue front.
        StatePtr fallbackState = this->_newState ? this->_newState : state;
        if (!fallbackState) {
            BDLOG("BFSAgent: no state for fallback, returning nullptr");
            return nullptr;
        }
        const int maxAttempts = 5;
        for (int attempt = 0; attempt < maxAttempts; ++attempt) {
            ActionPtr action = fallbackState->randomPickAction(this->_validateFilter);
            if (action && !action->isBack()) {
                BDLOG("BFSAgent: selected non-BACK action: %s", action->toString().c_str());
                return action;
            }
            if (action && action->isBack() && attempt < maxAttempts - 1) {
                BDLOG("BFSAgent: got BACK action, retrying (attempt %d/%d)", attempt + 1, maxAttempts);
                continue;
            }
            if (action) {
                BDLOG("BFSAgent: fallback to BACK action after %d attempts", attempt + 1);
                return action;
            }
        }

        BDLOG("BFSAgent: randomPickAction returned null, trying randomPickUnvisitedAction");
        ActionPtr unvisitedAction = fallbackState->randomPickUnvisitedAction();
        if (unvisitedAction) {
            return unvisitedAction;
        }

        BDLOG("BFSAgent: all fallback methods failed, returning nullptr");
        return nullptr;
    }

    bool BFSAgent::inTarpit() const {
        if (_recentActivities.size() < kTarpitMinWindowSize)
            return false;
        std::unordered_set<std::string> distinct;
        for (const auto &a : _recentActivities)
            distinct.insert(a);
        return static_cast<int>(distinct.size()) <= kTarpitMaxDistinctActivities;
    }

    bool BFSAgent::isActionSaturatedByTargetState(const StatePtr &state,
                                                 const ActivityStateActionPtr &action) const {
        if (!state || !action || !action->requireTarget()) return false;
        std::uint64_t edgeKey = makeEdgeKey(state->hash(), action->hash());
        auto it = _edgeToTarget.find(edgeKey);
        if (it == _edgeToTarget.end()) return false;
        uintptr_t targetHash = it->second;
        auto cov = _stateCoverage.find(targetHash);
        if (cov == _stateCoverage.end()) return false;
        int total = cov->second.first;
        int visited = cov->second.second;
        if (total <= 0) return false;
        return visited >= total;
    }

    double BFSAgent::getEdgeNoveltyScore(const StatePtr &state,
                                        const ActivityStateActionPtr &action) const {
        if (!state || !action) return 0.0;
        uintptr_t sHash = state->hash();
        uintptr_t aHash = action->hash();
        std::uint64_t key = makeEdgeKey(sHash, aHash);
        auto it = _edgeStats.find(key);
        if (it == _edgeStats.end() || it->second.total == 0) return 0.0;
        const EdgeStats &st = it->second;
        double denom = static_cast<double>(st.total);
        double statePart = static_cast<double>(st.newStates) / denom;
        double activityPart = static_cast<double>(st.newActivities) / denom;
        // Favor edges that lead to new activities (Delm / coverage-oriented BFS).
        return statePart + 3.0 * activityPart;
    }

    double BFSAgent::getActionTypeWeight(const ActivityStateActionPtr &action) const {
        if (!action) return 1.0;
        ActionType t = action->getActionType();
        switch (t) {
            case ActionType::CLICK:
            case ActionType::LONG_CLICK:
                return 3.0;
            case ActionType::SCROLL_TOP_DOWN:
            case ActionType::SCROLL_BOTTOM_UP:
            case ActionType::SCROLL_LEFT_RIGHT:
            case ActionType::SCROLL_RIGHT_LEFT:
            case ActionType::SCROLL_BOTTOM_UP_N:
                return 2.0;
            case ActionType::BACK:
            case ActionType::NOP:
            case ActionType::SHELL_EVENT:
                return 0.5;
            default:
                return 1.0;
        }
    }

}  // namespace fastbotx

#endif // FASTBOTX_BFS_AGENT_CPP_
