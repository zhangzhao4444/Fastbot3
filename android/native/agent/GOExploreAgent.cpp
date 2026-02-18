/*
 * This code is licensed under the Fastbot license. You may obtain a copy of this license in the LICENSE.txt file in the root directory of this source tree.
 */
/**
 * GOExploreAgent: standalone Go-Explore style exploration.
 * Archive of cells -> choose cell by weight -> return via BFS path -> explore N steps -> repeat.
 * See GO_EXPLORE_AGENT_DESIGN.md.
 */

#ifndef FASTBOTX_GO_EXPLORE_AGENT_CPP_
#define FASTBOTX_GO_EXPLORE_AGENT_CPP_

#include "GOExploreAgent.h"

#include "../utils.hpp"
#include "../desc/State.h"
#include "../desc/Action.h"
#include "../model/Model.h"

#include <cmath>

namespace fastbotx {

    GOExploreAgent::GOExploreAgent(const ModelPtr &model)
            : AbstractAgent(model) {
        this->_algorithmType = AlgorithmType::GoExplore;
        BLOG("GOExploreAgent: initialized (standalone Go-Explore style)");
    }

    void GOExploreAgent::updateStrategy() {
        // No Q-values or reuse model; archive updates happen in moveForward / selectNewAction.
    }

    void GOExploreAgent::onStateAbstractionChanged() {
        size_t edgeSize = _outEdges.size();
        size_t archiveSize = _archive.size();
        _outEdges.clear();
        _pathToCell.clear();
        _pathIndex = 0;
        _exploreTargetCell = 0;
        _exploreStepsLeft = 0;
        _archive.clear();
        BDLOG("GOExploreAgent: state abstraction changed, cleared edges=%zu archive=%zu",
              edgeSize, archiveSize);
    }

    void GOExploreAgent::ensureArchiveAndSeen(uintptr_t stateHash) {
        if (stateHash == 0) return;
        auto it = _archive.find(stateHash);
        if (it == _archive.end()) {
            _archive[stateHash] = CellMeta{1, 0};
            BDLOG("GOExploreAgent: new cell in archive hash=%llu seen=1 chosen=0 size=%zu",
                  (unsigned long long) stateHash, _archive.size());
        } else {
            it->second.seenTimes++;
        }
    }

    void GOExploreAgent::ensureInArchiveOnly(uintptr_t stateHash) {
        if (stateHash == 0) return;
        if (_archive.find(stateHash) == _archive.end()) {
            // Only建档，不增加seenTimes，保持seenTimes语义=“通过动作真实到达次数”
            _archive[stateHash] = CellMeta{0, 0};
            BDLOG("GOExploreAgent: ensureInArchiveOnly add cell hash=%llu size=%zu (seen=0 chosen=0)",
                  (unsigned long long) stateHash, _archive.size());
        }
    }

    std::unordered_map<uintptr_t, int> GOExploreAgent::bfsDistances(uintptr_t startHash) const {
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

    bool GOExploreAgent::buildPathTo(uintptr_t startHash, uintptr_t targetHash,
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
                    for (uintptr_t cur = targetHash; cur != startHash;) {
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

    uintptr_t GOExploreAgent::chooseCellFromArchive(const std::unordered_map<uintptr_t, int> *distToCurrent) const {
        if (_archive.empty()) return 0;
        // Weight = 1/(1+seen) + 1/(1+chosen); weighted random sampling for diversity
        std::vector<std::pair<uintptr_t, double>> weighted;
        double sumWeight = 0.0;
        for (const auto &kv : _archive) {
            const CellMeta &m = kv.second;
            double w = 1.0 / (1.0 + m.seenTimes) + 1.0 / (1.0 + m.chosenTimes);
            // Optional distance bias: prefer occasionally选择距离当前较远的cell，鼓励深度探索
            if (distToCurrent) {
                auto itDist = distToCurrent->find(kv.first);
                if (itDist != distToCurrent->end()) {
                    int d = itDist->second;
                    if (d > 0) {
                        double clipped = std::min(d, kMaxDistanceForWeight);
                        w *= (1.0 + kDistanceAlpha * clipped);
                    }
                }
            }
            if (w <= 0.0) continue;
            weighted.push_back({kv.first, w});
            sumWeight += w;
        }
        if (weighted.empty() || sumWeight <= 0.0) return 0;
        std::uniform_real_distribution<double> dist(0.0, sumWeight);
        double r = dist(_rng);
        for (const auto &p : weighted) {
            r -= p.second;
            if (r <= 0.0) return p.first;
        }
        return weighted.back().first;
    }

    ActivityStateActionPtr GOExploreAgent::pickExploreAction() const {
        StatePtr state = this->_newState;
        if (!state) return nullptr;
        const ActivityStateActionPtrVec &actions = state->getActions();
        std::vector<ActivityStateActionPtr> valid;
        for (const ActivityStateActionPtr &a : actions) {
            if (!a || !a->isValid()) continue;
            if (a->isNop()) continue;
            if (_validateFilter && !_validateFilter->include(a)) continue;
            valid.push_back(a);
        }
        if (valid.empty()) {
            ActivityStateActionPtr backAction = state->getBackAction();
            if (backAction && (!_validateFilter || _validateFilter->include(backAction)))
                return backAction;
            return nullptr;
        }
        // With probability kExploreRandomEpsilon pick random for diversity; else prefer unvisited/low-visit
        std::uniform_real_distribution<double> u01(0.0, 1.0);
        if (u01(_rng) < kExploreRandomEpsilon) {
            std::uniform_int_distribution<size_t> idx(0, valid.size() - 1);
            return valid[idx(_rng)];
        }
        ActivityStateActionPtr best = nullptr;
        int bestVisit = -1;
        for (const ActivityStateActionPtr &a : valid) {
            int v = a->getVisitedCount();
            if (best == nullptr || v < bestVisit) {
                best = a;
                bestVisit = v;
            }
        }
        return best;
    }

    ActionPtr GOExploreAgent::fallbackPickAction() const {
        ActivityStateActionPtr a = pickExploreAction();
        return a;
    }

    void GOExploreAgent::moveForward(StatePtr nextState) {
        StatePtr fromState = this->_newState;
        ActivityStateActionPtr actionTaken = this->_newAction;
        AbstractAgent::moveForward(nextState);
        if (fromState && nextState && actionTaken) {
            // 过滤掉全局跳转/重启类操作，避免在BFS图中产生跨应用/跨会话的噪声边
            ActionType t = actionTaken->getActionType();
            if (t == ActionType::CLEAN_RESTART ||
                t == ActionType::RESTART ||
                t == ActionType::START ||
                t == ActionType::ACTIVATE ||
                t == ActionType::DEEP_LINK ||
                t == ActionType::FUZZ) {
                // 这些动作通常意味着「上下文重置/跳出当前导航图」，不记录为可复用导航边
            } else {
            uintptr_t fromHash = fromState->hash();
            uintptr_t toHash = nextState->hash();
            auto &edges = _outEdges[fromHash];
            edges.push_back({actionTaken, toHash});
            BDLOG("GOExploreAgent: record edge %llu -> %llu (degree=%zu)",
                  (unsigned long long) fromHash,
                  (unsigned long long) toHash,
                  edges.size());
            }
        }
        ensureArchiveAndSeen(nextState ? nextState->hash() : 0);

        if (!_pathToCell.empty() && _pathIndex < _pathToCell.size()) {
            _pathIndex++;
            if (_pathIndex >= _pathToCell.size()) {
                uintptr_t arrivedHash = nextState ? nextState->hash() : 0;
                _pathToCell.clear();
                _pathIndex = 0;
                _exploreTargetCell = arrivedHash;
                _exploreStepsLeft = kExploreStepsPerCell;
                BDLOG("GOExploreAgent: arrived at target cell hash=%llu, start explore (%d steps)",
                      (unsigned long long) arrivedHash, kExploreStepsPerCell);
            }
        } else if (_exploreStepsLeft > 0) {
            _exploreStepsLeft--;
        }
    }

    ActionPtr GOExploreAgent::selectNewAction() {
        StatePtr state = this->_newState;
        if (!state) return fallbackPickAction();
        uintptr_t currentHash = state->hash();
        int blockTimes = getCurrentStateBlockTimes();

        if (blockTimes > kBlockCleanRestartThreshold) {
            _pathToCell.clear();
            _pathIndex = 0;
            _exploreStepsLeft = 0;
            BDLOG("GOExploreAgent: blocked %d steps, CLEAN_RESTART", blockTimes);
            return Action::CLEAN_RESTART;
        }
        if (blockTimes > kBlockDeepLinkThreshold) {
            _pathToCell.clear();
            _pathIndex = 0;
            _exploreStepsLeft = 0;
            BDLOG("GOExploreAgent: blocked %d steps, DEEP_LINK", blockTimes);
            return Action::DEEP_LINK;
        }
        if (blockTimes > kBlockBackThreshold) {
            const ActivityStateActionPtrVec &actions = state->getActions();
            ActivityStateActionPtr backAction = state->getBackAction();
            int validNonBack = 0;
            for (const auto &a : actions) {
                if (!a || !a->isValid()) continue;
                if (a->isBack() || a->isNop()) continue;
                if (!_validateFilter || _validateFilter->include(a)) {
                    validNonBack++;
                    break;
                }
            }
            if (validNonBack == 0 && backAction && (!_validateFilter || _validateFilter->include(backAction))) {
                _pathToCell.clear();
                _pathIndex = 0;
                _exploreStepsLeft = 0;
                BDLOG("GOExploreAgent: blocked %d steps, BACK", blockTimes);
                return backAction;
            }
        }

        // 1) Following path to cell
        if (!_pathToCell.empty() && _pathIndex < _pathToCell.size()) {
            if (_pathToCell[_pathIndex].first == currentHash) {
                ActivityStateActionPtr pathAction = _pathToCell[_pathIndex].second;
                BDLOG("GOExploreAgent: path step %zu/%zu", _pathIndex + 1, _pathToCell.size());
                return pathAction;
            }
            _pathToCell.clear();
            _pathIndex = 0;
        }

        // 2) Exploring from cell (N steps at target cell)
        if (_exploreStepsLeft > 0) {
            if (currentHash == _exploreTargetCell) {
                ActivityStateActionPtr exploreAction = pickExploreAction();
                if (exploreAction) {
                    BDLOG("GOExploreAgent: explore step (left=%d)", _exploreStepsLeft);
                    return exploreAction;
                }
            } else {
                // State hash changed (e.g. abstraction) or we left the cell; clear explore phase
                _exploreStepsLeft = 0;
                _exploreTargetCell = 0;
            }
        }

        // 3) Choose new cell: ensure current in archive (no extra seen count), then pick target
        ensureInArchiveOnly(currentHash);
        // Use BFS distance as额外信号来轻微偏置「距离当前较远」的cell
        std::unordered_map<uintptr_t, int> dist = bfsDistances(currentHash);
        uintptr_t targetCell = chooseCellFromArchive(&dist);
        if (targetCell == 0) {
            BDLOG("GOExploreAgent: chooseCellFromArchive got 0, fallback action");
            return fallbackPickAction();
        }

        auto it = _archive.find(targetCell);
        int seen = -1;
        int chosen = -1;
        if (it != _archive.end()) {
            seen = it->second.seenTimes;
            chosen = it->second.chosenTimes;
            it->second.chosenTimes++;
        }

        BDLOG("GOExploreAgent: choose cell from archive cur=%llu target=%llu seen=%d chosen(before)=%d archiveSize=%zu",
              (unsigned long long) currentHash,
              (unsigned long long) targetCell,
              seen, chosen,
              _archive.size());

        if (targetCell == currentHash) {
            _exploreTargetCell = currentHash;
            _exploreStepsLeft = kExploreStepsPerCell;
            ActivityStateActionPtr exploreAction = pickExploreAction();
            if (exploreAction) {
                BDLOG("GOExploreAgent: start explore at current cell hash=%llu (%d steps)",
                      (unsigned long long) currentHash, kExploreStepsPerCell);
                return exploreAction;
            }
            return fallbackPickAction();
        }

        std::vector<std::pair<uintptr_t, ActivityStateActionPtr>> path;
        if (buildPathTo(currentHash, targetCell, path)) {
            _pathToCell = std::move(path);
            _pathIndex = 0;
            ActivityStateActionPtr firstAction = _pathToCell[0].second;
            BDLOG("GOExploreAgent: navigate to cell from=%llu to=%llu steps=%zu",
                  (unsigned long long) currentHash,
                  (unsigned long long) targetCell,
                  _pathToCell.size());
            return firstAction;
        }

        _exploreTargetCell = currentHash;
        _exploreStepsLeft = kExploreStepsPerCell;
        BDLOG("GOExploreAgent: no path from=%llu to target=%llu, explore from current (%d steps)",
              (unsigned long long) currentHash,
              (unsigned long long) targetCell,
              kExploreStepsPerCell);
        ActivityStateActionPtr a = pickExploreAction();
        return a ? a : fallbackPickAction();
    }

}  // namespace fastbotx

#endif  // FASTBOTX_GO_EXPLORE_AGENT_CPP_
