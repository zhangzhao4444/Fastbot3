/**
 * @author Zhao Zhang
 */

#ifndef FASTBOTX_DFS_AGENT_CPP_
#define FASTBOTX_DFS_AGENT_CPP_

#include "DFSAgent.h"

#include "../utils.hpp"
#include "../desc/State.h"
#include "../desc/Action.h"

namespace fastbotx {

    DFSAgent::DFSAgent(const ModelPtr &model)
            : AbstractAgent(model),
              _stateBlockCounter(0),
              _lastStateHash(0),
              _activityBlockCounter(0) {
        this->_algorithmType = AlgorithmType::DFS;
        BLOG("DFSAgent: initialized (depth-first exploration)");
    }

    static std::uint64_t makeEdgeKey(uintptr_t stateHash, uintptr_t actionHash) {
        return (static_cast<std::uint64_t>(stateHash) << 32) ^
               static_cast<std::uint64_t>(actionHash);
    }

    void DFSAgent::updateStrategy() {
        // DFSAgent is purely structural: no Q-values or reuse model to update.
        // Visit counts are maintained by the graph/state itself.
    }

    void DFSAgent::moveForward(StatePtr nextState) {
        // Let base class update state and action history.
        AbstractAgent::moveForward(nextState);

        StatePtr cur = this->_currentState;
        StatePtr prev = this->_lastState;

        if (!cur) {
            _stack.clear();
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
            _selectCallCount = 0;
            return;
        }

        uintptr_t h = cur->hash();
        
        // Track state block counter: increment if same state hash, reset if different
        if (h == _lastStateHash) {
            _stateBlockCounter++;
            if (_stateBlockCounter >= 12) {
                BDLOG("DFSAgent: state hash unchanged for %d consecutive steps (hash=%lu)",
                      _stateBlockCounter, static_cast<unsigned long>(h));
            }
        } else {
            if (_stateBlockCounter > 0) {
                BDLOG("DFSAgent: state hash changed, reset block counter from %d to 0 (old=%lu, new=%lu)",
                      _stateBlockCounter, static_cast<unsigned long>(_lastStateHash),
                      static_cast<unsigned long>(h));
            }
            _stateBlockCounter = 0;
            _lastStateHash = h;
        }

        // Track activity-level coverage and activity block (only when "only BACK" on same activity, to avoid false positives)
        bool newActivity = false;
        stringPtr actPtr = cur->getActivityString();
        if (actPtr) {
            auto ins = _visitedActivities.insert(*actPtr);
            newActivity = ins.second;
            if (_rootActivity.empty()) {
                _rootActivity = *actPtr;
                BDLOG("DFSAgent: root activity set to %s", _rootActivity.c_str());
            }
            // Only count activity block when current state has no valid non-BACK action (real stuck), else reset
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
                        BDLOG("DFSAgent: same activity + only BACK for %d steps (activity=%s)",
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
            // VET-style tarpit: sliding window of recent activities
            _recentActivities.push_back(*actPtr);
            while (_recentActivities.size() > kRecentActivityWindowSize) {
                _recentActivities.pop_front();
            }
        } else {
            _activityBlockCounter = 0;
            _lastActivity.clear();
        }

        // Track edge statistics: (prevState, currentAction) -> next state / activity novelty.
        bool newState = _visitedStates.insert(h).second;
        if (prev && this->_currentAction) {
            uintptr_t prevHash = prev->hash();
            uintptr_t actHash = this->_currentAction->hash();
            std::uint64_t key = makeEdgeKey(prevHash, actHash);
            auto &stats = _edgeStats[key];
            stats.total += 1;
            if (newState) {
                stats.newStates += 1;
            }
            if (newActivity) {
                stats.newActivities += 1;
            }
        }

        // Target-state saturation: record (cur, action) -> nextState and coverage of nextState
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

        // If this state is already on the stack, trim to that position (backtracking).
        auto it = std::find_if(_stack.begin(), _stack.end(),
                               [h](const Frame &f) {
                                   return f.state && f.state->hash() == h;
                               });
        if (it != _stack.end()) {
            // reset nextIndex so we re-scan all actions when returning to this state
            // (e.g. X -> click A -> Y -> ... -> X: widget A can be chosen again via bestReusable)
            it->nextIndex = 0;
            _stack.erase(it + 1, _stack.end());
        } else {
            // New state: push as a new frame with shuffled action order.
            Frame f;
            f.state = cur;
            const ActivityStateActionPtrVec &actions = cur->getActions();
            f.order.resize(actions.size());
            for (size_t i = 0; i < actions.size(); ++i) {
                f.order[i] = i;
            }
            if (!f.order.empty()) {
                std::shuffle(f.order.begin(), f.order.end(), _rng);
            }
            f.nextIndex = 0;
            _stack.push_back(std::move(f));
        }
    }

    ActionPtr DFSAgent::selectNewAction() {
        _selectCallCount++;

        // Detect state/activity change so we don't keep escalating after we've left the stuck state.
        if (this->_newState) {
            if (this->_newState->hash() != _lastStateHash) {
                _stateBlockCounter = 0;
                _lastStateHash = this->_newState->hash();
                BDLOG("DFSAgent: state changed (hash=%lu), reset state block counter", static_cast<unsigned long>(_lastStateHash));
            }
            stringPtr actStr = this->_newState->getActivityString();
            if (actStr && *actStr != _lastActivity) {
                _activityBlockCounter = 0;
                _lastActivity = *actStr;
                BDLOG("DFSAgent: activity changed (%s), reset activity block counter", actStr->c_str());
            }
        }

        // Prefer stack top; if _newState is different from stack (new state after FUZZ/unstick), use _newState.
        StatePtr state = (!_stack.empty() && _stack.back().state) ? _stack.back().state : this->_newState;
        if (this->_newState && _stack.size() && _stack.back().state &&
            this->_newState->hash() != _stack.back().state->hash()) {
            state = this->_newState;  // new state from graph, explore from it
        }

        // Ensure root activity is set before any BACK/root check (first time on 首页, moveForward may not have run yet).
        if (_rootActivity.empty() && state) {
            stringPtr actStr = state->getActivityString();
            if (actStr && !actStr->empty()) {
                _rootActivity = *actStr;
                BDLOG("DFSAgent: root activity set in selectNewAction to %s", _rootActivity.c_str());
            }
        }

        if (!state) {
            BDLOG("DFSAgent: state is null, fallback to randomPickAction");
            if (this->_newState) {
                return this->_newState->randomPickAction(this->_validateFilter);
            }
            return nullptr;
        }

        // Block escalation: use max(state, activity) so we trigger when stuck on same activity even if state hash varies (e.g. timer/ad).
        constexpr int kBlockBackThreshold = 5;
        constexpr int kBlockFuzzThreshold = 10;
        constexpr int kBlockCleanRestartThreshold = 15;
        int blockTimes = std::max(_stateBlockCounter, _activityBlockCounter);

        if (blockTimes > kBlockCleanRestartThreshold) {
            _stateBlockCounter = 0;
            _lastStateHash = 0;
            _activityBlockCounter = 0;
            _lastActivity.clear();
            _stack.clear();
            _selectCallCount = 0;
            BDLOG("DFSAgent: blocked %d steps (>%d), CLEAN_RESTART (always)", blockTimes, kBlockCleanRestartThreshold);
            return Action::CLEAN_RESTART;
        }
        // DEEP_LINK when stuck (block > 10) or in exploration tarpit (VET-style: few distinct activities in recent window)
        bool tryDeepLink = (blockTimes > kBlockFuzzThreshold) ||
                           (inTarpit() && blockTimes > kBlockBackThreshold);
        if (tryDeepLink) {
            _selectCallCount = 0;
            _stateBlockCounter++;
            _activityBlockCounter++;  // so CLEAN_RESTART threshold is reachable if DEEP_LINK doesn't help
            BDLOG("DFSAgent: blocked %d steps (tarpit=%d), DEEP_LINK (try unexplored activity)", blockTimes, inTarpit() ? 1 : 0);
            return Action::DEEP_LINK;
        }
        // Coverage-driven DEEP_LINK (Delm-style): every N steps try DEEP_LINK to encourage new activity coverage.
        if (_selectCallCount >= kCoverageDrivenDeepLinkInterval) {
            _selectCallCount = 0;
            BDLOG("DFSAgent: coverage-driven DEEP_LINK (every %d steps)", kCoverageDrivenDeepLinkInterval);
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
                // At root: BACK would exit app and is often ignored → don't return BACK and reset,
                // so blockTimes keeps growing and we hit FUZZ (>10) / CLEAN_RESTART (>15).
                stringPtr actStr = state->getActivityString();
                bool isRootActivity = (actStr && !_rootActivity.empty() && *actStr == _rootActivity);
                bool isRootState = (_stack.size() == 1u && !_stack.empty() && _stack.back().state == state);
                if (isRootState && isRootActivity) {
                    BDLOG("DFSAgent: root + only BACK, skip return BACK (let blockTimes grow to FUZZ/CLEAN_RESTART)");
                } else {
                    _stateBlockCounter = 0;
                    _lastStateHash = 0;
                    _activityBlockCounter = 0;
                    _lastActivity.clear();
                    BDLOG("DFSAgent: blocked %d steps (>%d), BACK (only action)", blockTimes, kBlockBackThreshold);
                    if ((!_validateFilter || _validateFilter->include(backAction)))
                        return backAction;
                }
            }
        }

        // If we've been in the same state/activity for too many steps, prefer unvisited / any non-BACK (blockTimes already max of state/activity)
        {
            constexpr int kSelfRescueThreshold = 12;
            if (blockTimes >= kSelfRescueThreshold) {
                BDLOG("DFSAgent: state blocked for %d steps (hash=%lu), triggering self-rescue",
                      blockTimes, static_cast<unsigned long>(state ? state->hash() : 0));
                const ActivityStateActionPtrVec &actions = state->getActions();

                // Normal self-rescue policy:
                // 1. Prefer unvisited, non-BACK, non-NOP, valid actions.
                // 2. Then any non-BACK, valid action.
                ActivityStateActionPtr candidate = nullptr;

                // Pass 1: unvisited & non-BACK & valid
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

                // Pass 2: any non-BACK valid action (even if visited)
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
                    BDLOG("DFSAgent: self-rescue selected action %s", candidate->toString().c_str());
                    return candidate;
                }

                BDLOG("DFSAgent: self-rescue found no non-BACK candidate, continue with normal DFS");
            }
        }

        // If current state (_newState) is not on stack top (e.g. after DEEP_LINK/CLEAN_RESTART),
        // push a frame for it so the DFS loop selects actions for the actual current state.
        if (state && state == this->_newState && !_stack.empty() && _stack.back().state &&
            _stack.back().state->hash() != state->hash()) {
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
            _stack.push_back(std::move(f));
        }

        // DFS loop: walk frames from top down until we find an action or exhaust stack.
        while (!_stack.empty()) {
            Frame &frame = _stack.back();
            StatePtr s = frame.state;
            if (!s) {
                _stack.pop_back();
                continue;
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

            // Single pass over actions starting from nextIndex. Track best candidates per category.
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
                // visited but potentially reusable: not saturated by state, and not by target-state coverage
                if (!s->isSaturated(action) && !isActionSaturatedByTargetState(s, action)) {
                    double score = getEdgeNoveltyScore(s, action) * typeWeight;
                    if (score > bestReusableScore) {
                        bestReusableScore = score;
                        bestReusable = static_cast<ssize_t>(i);
                    }
                }
            }
            // Mark this frame as fully scanned.
            frame.nextIndex = actions.size();

            if (bestUnvisitedTarget >= 0) {
                const auto &action = actions[static_cast<size_t>(bestUnvisitedTarget)];
                BDLOG("DFSAgent: select unvisited target action %s", action->toString().c_str());
                return action;
            }
            if (bestUnvisited >= 0) {
                const auto &action = actions[static_cast<size_t>(bestUnvisited)];
                BDLOG("DFSAgent: select unvisited action %s", action->toString().c_str());
                return action;
            }
            if (bestReusable >= 0) {
                const auto &action = actions[static_cast<size_t>(bestReusable)];
                BDLOG("DFSAgent: reuse visited non-saturated action %s (noveltyScore=%.3f)",
                      action->toString().c_str(), bestReusableScore);
                return action;
            }

            // All actions for this frame are exhausted: try to backtrack via BACK, then pop frame.
            ActivityStateActionPtr backAction = s->getBackAction();
            
            // Check if this is the root state (last frame in stack) before popping
            bool isRootState = (_stack.size() == 1);
            
            // Also check if current state is the root activity (first visited activity)
            // This prevents BACK loop when stuck at MainActivity even if stack has multiple frames
            bool isRootActivity = false;
            stringPtr currentActivity = s->getActivityString();
            if (currentActivity && !_rootActivity.empty() && *currentActivity == _rootActivity) {
                isRootActivity = true;
            }
            
            _stack.pop_back();
            
            // Only allow BACK if:
            // 1. Not the last frame in stack (can backtrack to previous frame)
            // 2. Not the root activity (BACK would exit app, causing infinite loop)
            // Root state/activity BACK would exit the app, which may be rejected by the system,
            // causing infinite BACK loop on the same state.
            if (!isRootState && !isRootActivity && backAction && backAction->isValid() &&
                (!_validateFilter || _validateFilter->include(backAction))) {
                BDLOG("DFSAgent: all actions for state exhausted, backtrack via BACK");
                return backAction;
            }
            
            // Root state/activity: don't use BACK, continue to next frame or fallback to random
            if (isRootState || isRootActivity) {
                BDLOG("DFSAgent: root state/activity exhausted (isRootState=%d, isRootActivity=%d), skipping BACK to avoid infinite loop",
                      isRootState ? 1 : 0, isRootActivity ? 1 : 0);
            }

            // Continue with previous frame in the next loop iteration (deeper backtracking).
        }

        // Stack exhausted: last resort, use random pick on the current UI state.
        // Use _newState for fallback; state may be a previously-popped stack top.
        BDLOG("DFSAgent: DFS stack exhausted, fallback to randomPickAction (preferring non-BACK)");
        StatePtr fallbackState = this->_newState ? this->_newState : state;
        if (!fallbackState) {
            BDLOG("DFSAgent: no state for fallback, returning nullptr");
            return nullptr;
        }
        const int maxAttempts = 5;
        for (int attempt = 0; attempt < maxAttempts; ++attempt) {
            ActionPtr action = fallbackState->randomPickAction(this->_validateFilter);
            if (action && !action->isBack()) {
                BDLOG("DFSAgent: selected non-BACK action: %s", action->toString().c_str());
                return action;
            }
            if (action && action->isBack() && attempt < maxAttempts - 1) {
                BDLOG("DFSAgent: got BACK action, retrying (attempt %d/%d)", attempt + 1, maxAttempts);
                continue;
            }
            if (action) {
                BDLOG("DFSAgent: fallback to BACK action after %d attempts", attempt + 1);
                return action;
            }
        }
        BDLOG("DFSAgent: randomPickAction returned null, trying randomPickUnvisitedAction");
        ActionPtr unvisitedAction = fallbackState->randomPickUnvisitedAction();
        if (unvisitedAction) {
            return unvisitedAction;
        }
        BDLOG("DFSAgent: all fallback methods failed, returning nullptr");
        return nullptr;
    }

    bool DFSAgent::inTarpit() const {
        if (_recentActivities.size() < kTarpitMinWindowSize)
            return false;
        std::unordered_set<std::string> distinct;
        for (const auto &a : _recentActivities)
            distinct.insert(a);
        return static_cast<int>(distinct.size()) <= kTarpitMaxDistinctActivities;
    }

    bool DFSAgent::isActionSaturatedByTargetState(const StatePtr &state,
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

    double DFSAgent::getEdgeNoveltyScore(const StatePtr &state,
                                         const ActivityStateActionPtr &action) const {
        if (!state || !action) return 0.0;
        uintptr_t sHash = state->hash();
        uintptr_t aHash = action->hash();
        std::uint64_t key = makeEdgeKey(sHash, aHash);
        auto it = _edgeStats.find(key);
        if (it == _edgeStats.end() || it->second.total == 0) return 0.0;
        const EdgeStats &st = it->second;
        double denom = static_cast<double>(st.total);
        // Combine state-level and activity-level novelty; activity hits权重大一些。
        double statePart = static_cast<double>(st.newStates) / denom;
        double activityPart = static_cast<double>(st.newActivities) / denom;
        return statePart + 2.0 * activityPart;
    }

    double DFSAgent::getActionTypeWeight(const ActivityStateActionPtr &action) const {
        if (!action) return 1.0;
        ActionType t = action->getActionType();
        switch (t) {
            case ActionType::CLICK:
            case ActionType::LONG_CLICK:
                return 3.0;  // 高优先级：最可能切屏
            case ActionType::SCROLL_TOP_DOWN:
            case ActionType::SCROLL_BOTTOM_UP:
            case ActionType::SCROLL_LEFT_RIGHT:
            case ActionType::SCROLL_RIGHT_LEFT:
            case ActionType::SCROLL_BOTTOM_UP_N:
                return 2.0;  // 中优先级：有助于发现新内容
            case ActionType::BACK:
            case ActionType::NOP:
            case ActionType::SHELL_EVENT:
                return 0.5;  // 较低优先级
            default:
                return 1.0;
        }
    }

}  // namespace fastbotx

#endif // FASTBOTX_DFS_AGENT_CPP_

