/*
 * This code is licensed under the Fastbot license. You may obtain a copy of this license in the LICENSE.txt file in the root directory of this source tree.
 */
/**
 * Frontier-based exploration agent implementation.
 * Framework: MA-SLAM (structured map + global planner + local execution).
 * Scoring: FH-DRL (infoGain * exp(-beta * distance)).
 */

#ifndef FASTBOTX_FRONTIER_AGENT_CPP_
#define FASTBOTX_FRONTIER_AGENT_CPP_

#include "FrontierAgent.h"

#include "../utils.hpp"
#include "../desc/State.h"
#include "../desc/Action.h"
#include "../model/Model.h"

#include <cmath>

namespace fastbotx {

    FrontierAgent::FrontierAgent(const ModelPtr &model)
            : AbstractAgent(model) {
        this->_algorithmType = AlgorithmType::Frontier;
        BLOG("FrontierAgent: initialized (frontier-based exploration)");
    }

    void FrontierAgent::updateStrategy() {
        // No Q-values or reuse model; nothing to update.
    }

    void FrontierAgent::onStateAbstractionChanged() {
        _outEdges.clear();
        _pathToFrontier.clear();
        _pathIndex = 0;
        _cachedDistMap.clear();
        _cachedDistMapStartHash = 0;
        BDLOG("FrontierAgent: state abstraction changed, cleared outEdges and path");
    }

    void FrontierAgent::moveForward(StatePtr nextState) {
        // Record edge (from, action, to) before base updates _currentState / _newState
        StatePtr fromState = this->_newState;
        ActivityStateActionPtr actionTaken = this->_newAction;
        AbstractAgent::moveForward(nextState);
        if (fromState && nextState && actionTaken) {
            uintptr_t fromHash = fromState->hash();
            uintptr_t toHash = nextState->hash();
            _outEdges[fromHash].push_back({actionTaken, toHash});
            // Invalidate BFS cache: next step's "current" will be nextState
            _cachedDistMapStartHash = 0;
        }
        // Advance path index if we're following a path
        if (!_pathToFrontier.empty() && _pathIndex < _pathToFrontier.size()) {
            _pathIndex++;
            if (_pathIndex >= _pathToFrontier.size()) {
                _pathToFrontier.clear();
                _pathIndex = 0;
            }
        }
    }

    std::vector<FrontierCandidate> FrontierAgent::buildFrontierCandidatesLocal() const {
        std::vector<FrontierCandidate> candidates;
        StatePtr state = this->_newState;
        if (!state) {
            return candidates;
        }
        const ActivityStateActionPtrVec &actions = state->getActions();
        for (const ActivityStateActionPtr &action : actions) {
            if (!action || !action->isValid()) {
                continue;
            }
            if (_validateFilter && !_validateFilter->include(action)) {
                continue;
            }
            FrontierCandidate c;
            c.sourceState = state;
            c.action = action;
            c.distance = 0;
            c.infoGain = getInfoGain(action);
            candidates.push_back(c);
        }
        return candidates;
    }

    std::unordered_map<uintptr_t, int> FrontierAgent::bfsDistances(uintptr_t startHash) const {
        std::unordered_map<uintptr_t, int> dist;
        dist[startHash] = 0;
        std::deque<uintptr_t> q;
        q.push_back(startHash);
        while (!q.empty()) {
            uintptr_t u = q.front();
            q.pop_front();
            int d = dist[u];
            if (d >= kMaxBFSDistance) continue;
            auto it = _outEdges.find(u);
            if (it == _outEdges.end()) continue;
            for (const auto &edge : it->second) {
                uintptr_t v = edge.second;
                if (dist.find(v) == dist.end()) {
                    dist[v] = d + 1;
                    q.push_back(v);
                }
            }
        }
        return dist;
    }

    bool FrontierAgent::buildPathTo(uintptr_t startHash, uintptr_t targetHash,
                                    std::vector<std::pair<uintptr_t, ActivityStateActionPtr>> &path) const {
        path.clear();
        if (startHash == targetHash) return true;
        std::unordered_map<uintptr_t, std::pair<uintptr_t, ActivityStateActionPtr>> parent;
        parent[startHash] = {0, nullptr};
        std::deque<uintptr_t> q;
        q.push_back(startHash);
        while (!q.empty()) {
            uintptr_t u = q.front();
            q.pop_front();
            auto it = _outEdges.find(u);
            if (it == _outEdges.end()) continue;
            for (const auto &edge : it->second) {
                uintptr_t v = edge.second;
                if (parent.find(v) != parent.end()) continue;
                parent[v] = {u, edge.first};
                if (v == targetHash) {
                    std::vector<std::pair<uintptr_t, ActivityStateActionPtr>> rev;
                    for (uintptr_t cur = targetHash; cur != startHash; ) {
                        auto p = parent[cur];
                        rev.push_back({p.first, p.second});
                        cur = p.first;
                    }
                    path.assign(rev.rbegin(), rev.rend());
                    return true;
                }
                q.push_back(v);
            }
        }
        return false;
    }

    std::vector<FrontierCandidate> FrontierAgent::buildFrontierCandidates() const {
        auto modelPtr = this->_model.lock();
        if (!modelPtr) return buildFrontierCandidatesLocal();
        const GraphPtr &graph = modelPtr->getGraph();
        if (!graph) return buildFrontierCandidatesLocal();
        // MA-SLAM structured map: states + observed edges -> frontier candidates with distance
        const StatePtrSet &states = graph->getStates();
        StatePtr current = this->_newState;
        if (!current || states.size() <= 1 || _outEdges.empty()) {
            return buildFrontierCandidatesLocal();
        }
        uintptr_t currentHash = current->hash();
        // Use cached BFS distances when current state unchanged (e.g. when following path we don't re-enter here)
        if (_cachedDistMapStartHash != currentHash) {
            _cachedDistMap = bfsDistances(currentHash);
            _cachedDistMapStartHash = currentHash;
        }
        const std::unordered_map<uintptr_t, int> &distMap = _cachedDistMap;
        size_t estCandidates = 0;
        for (const StatePtr &s : states) {
            if (s) estCandidates += s->getActions().size();
        }
        std::vector<FrontierCandidate> candidates;
        candidates.reserve(estCandidates > 0 ? estCandidates : 64);
        for (const StatePtr &state : states) {
            if (!state) continue;
            uintptr_t stateHash = state->hash();
            int distance = kMaxBFSDistance;
            auto dIt = distMap.find(stateHash);
            if (dIt != distMap.end()) distance = dIt->second;
            const ActivityStateActionPtrVec &actions = state->getActions();
            for (const ActivityStateActionPtr &action : actions) {
                if (!action || !action->isValid()) continue;
                if (_validateFilter && !_validateFilter->include(action)) continue;
                FrontierCandidate c;
                c.sourceState = state;
                c.action = action;
                c.distance = distance;
                c.infoGain = getInfoGain(action);
                candidates.push_back(c);
            }
        }
        if (candidates.empty()) return buildFrontierCandidatesLocal();
        return candidates;
    }

    double FrontierAgent::getInfoGain(const ActivityStateActionPtr &action) const {
        if (!action) {
            return 0.0;
        }
        int visitCount = action->getVisitedCount();
        double gain = kBaseInfoGain;
        if (visitCount <= 0) {
            // Unvisited: add type weight (DEEP_LINK/ACTIVATE etc. get higher)
            int typePriority = action->getPriorityByActionType();
            gain += static_cast<double>(typePriority) * 0.25;
        } else {
            // Visited: decay by visit count
            gain = kBaseInfoGain / (1.0 + static_cast<double>(visitCount));
        }
        return gain;
    }

    double FrontierAgent::getScore(double infoGain, int distance) {
        // FH-DRL exponential-hyperbolic: balance proximity vs information gain
        return infoGain * std::exp(-kBeta * static_cast<double>(distance));
    }

    ActionPtr FrontierAgent::fallbackPickAction() const {
        StatePtr state = this->_newState;
        if (!state) {
            return nullptr;
        }
        // Prefer unvisited then low-visit, avoid BACK when possible
        const ActivityStateActionPtrVec &actions = state->getActions();
        ActivityStateActionPtr best = nullptr;
        int bestVisitCount = -1;
        for (const ActivityStateActionPtr &a : actions) {
            if (!a || !a->isValid()) continue;
            if (a->isNop()) continue;
            if (_validateFilter && !_validateFilter->include(a)) continue;
            int v = a->getVisitedCount();
            if (best == nullptr || (v < bestVisitCount) ||
                (v == bestVisitCount && best->isBack() && !a->isBack())) {
                best = a;
                bestVisitCount = v;
            }
        }
        if (best) {
            return best;
        }
        ActionPtr action = state->randomPickAction(this->_validateFilter);
        if (action) return action;
        // Avoid returning null when state exists: try BACK so caller does not get NOP loop
        ActivityStateActionPtr backAction = state->getBackAction();
        if (backAction && (!_validateFilter || _validateFilter->include(backAction))) {
            return backAction;
        }
        return nullptr;
    }

    ActionPtr FrontierAgent::selectNewAction() {
        StatePtr state = this->_newState;
        if (!state) {
            BDLOG("FrontierAgent: no state, fallback");
            return fallbackPickAction();
        }
        uintptr_t currentHash = state->hash();
        int blockTimes = getCurrentStateBlockTimes();

        // ---------- Anti-stuck: escape after consecutive same-state steps ----------
        if (blockTimes > kBlockCleanRestartThreshold) {
            _pathToFrontier.clear();
            _pathIndex = 0;
            BDLOG("FrontierAgent: blocked %d steps (>%d), CLEAN_RESTART", blockTimes, kBlockCleanRestartThreshold);
            return Action::CLEAN_RESTART;
        }
        if (blockTimes > kBlockDeepLinkThreshold) {
            _pathToFrontier.clear();
            _pathIndex = 0;
            BDLOG("FrontierAgent: blocked %d steps (>%d), DEEP_LINK", blockTimes, kBlockDeepLinkThreshold);
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
                _pathToFrontier.clear();
                _pathIndex = 0;
                BDLOG("FrontierAgent: blocked %d steps (>%d), BACK (only action)", blockTimes, kBlockBackThreshold);
                return backAction;
            }
        }

        // ---------- MA-SLAM local execution: follow planned path if any ----------
        if (!_pathToFrontier.empty() && _pathIndex < _pathToFrontier.size()) {
            if (_pathToFrontier[_pathIndex].first == currentHash) {
                ActivityStateActionPtr pathAction = _pathToFrontier[_pathIndex].second;
                BDLOG("FrontierAgent: path step %zu/%zu action %s",
                      _pathIndex + 1, _pathToFrontier.size(), pathAction->toString().c_str());
                return pathAction;
            }
            _pathToFrontier.clear();
            _pathIndex = 0;
        }

        // ---------- MA-SLAM global planner: structured map -> frontier candidates -> best target ----------
        std::vector<FrontierCandidate> candidates = buildFrontierCandidates();
        if (candidates.empty()) {
            BDLOG("FrontierAgent: no frontier candidates, fallback");
            return fallbackPickAction();
        }

        // FH-DRL score = infoGain * exp(-beta * distance); break ties by type priority then random
        double bestScore = -1.0;
        std::vector<size_t> bestIndices;
        bestIndices.reserve(32);
        for (size_t i = 0; i < candidates.size(); ++i) {
            double s = getScore(candidates[i].infoGain, candidates[i].distance);
            if (s > bestScore) {
                bestScore = s;
                bestIndices.clear();
                bestIndices.push_back(i);
            } else if (s >= bestScore - 1e-9) {
                bestIndices.push_back(i);
            }
        }

        size_t chosen;
        if (bestIndices.size() == 1) {
            chosen = bestIndices[0];
        } else {
            int bestTypePri = -1;
            std::vector<size_t> typeBest;
            typeBest.reserve(bestIndices.size());
            for (size_t i : bestIndices) {
                int tp = candidates[i].action->getPriorityByActionType();
                if (tp > bestTypePri) {
                    bestTypePri = tp;
                    typeBest.clear();
                    typeBest.push_back(i);
                } else if (tp == bestTypePri) {
                    typeBest.push_back(i);
                }
            }
            std::uniform_int_distribution<size_t> dist(0, typeBest.size() - 1);
            chosen = typeBest[dist(_rng)];
        }

        const FrontierCandidate &best = candidates[chosen];
        uintptr_t targetHash = best.sourceState->hash();

        if (targetHash == currentHash) {
            BDLOG("FrontierAgent: select action %s (score=%.3f, infoGain=%.3f)",
                  best.action->toString().c_str(), bestScore, best.infoGain);
            return best.action;
        }

        // MA-SLAM local execution: navigate to target state then execute frontier action
        // Build BFS path and return first step
        std::vector<std::pair<uintptr_t, ActivityStateActionPtr>> path;
        if (buildPathTo(currentHash, targetHash, path)) {
            path.push_back({targetHash, best.action});
            _pathToFrontier = std::move(path);
            _pathIndex = 0;
            ActivityStateActionPtr firstAction = _pathToFrontier[0].second;
            BDLOG("FrontierAgent: navigate to frontier (%zu steps), first action %s",
                  _pathToFrontier.size(), firstAction->toString().c_str());
            return firstAction;
        }

        // No path to target: best.action belongs to another state; must not execute from current.
        // Use best-scoring candidate at current state, else fallback.
        BDLOG("FrontierAgent: no path to target hash %lu, use best local candidate", (unsigned long)targetHash);
        double bestLocalScore = -1.0;
        ActivityStateActionPtr bestLocalAction = nullptr;
        for (size_t i = 0; i < candidates.size(); ++i) {
            if (candidates[i].sourceState->hash() != currentHash) continue;
            double s = getScore(candidates[i].infoGain, candidates[i].distance);
            if (s > bestLocalScore) {
                bestLocalScore = s;
                bestLocalAction = candidates[i].action;
            }
        }
        if (bestLocalAction) return bestLocalAction;
        return fallbackPickAction();
    }

}  // namespace fastbotx

#endif  // FASTBOTX_FRONTIER_AGENT_CPP_
