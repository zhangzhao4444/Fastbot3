/*
 * This code is licensed under the Fastbot license. You may obtain a copy of this license in the LICENSE.txt file in the root directory of this source tree.
 */
/**
 * ICMAgent: curiosity-driven exploration (WebRLED-aligned dual novelty + ε-greedy).
 */

#ifndef FASTBOTX_ICM_AGENT_CPP_
#define FASTBOTX_ICM_AGENT_CPP_

#include "ICMAgent.h"

#include "../utils.hpp"
#include "../desc/State.h"
#include "../desc/Action.h"

#include <cmath>

namespace fastbotx {

    ICMAgent::ICMAgent(const ModelPtr &model)
            : AbstractAgent(model) {
        this->_algorithmType = AlgorithmType::ICM;
        BLOG("ICMAgent: initialized (curiosity-driven, WebRLED-style dual novelty)");
    }

    void ICMAgent::updateStrategy() {
        // No Q-values or reuse model; curiosity is purely count-based.
    }

    void ICMAgent::onStateAbstractionChanged() {
        _episodeStateCount.clear();
        _episodeSteps = 0;
        _globalStateCount.clear();
        _inDegree.clear();
        _outDegree.clear();
        _succStats.clear();
        _selfLoopCount.clear();
        _recentStates.clear();
        _pathCount.clear();
        BDLOG("ICMAgent: state abstraction changed, cleared episode and global counts");
    }

    void ICMAgent::moveForward(StatePtr nextState) {
        StatePtr fromState = this->_newState;
        ActivityStateActionPtr actionTaken = this->_newAction;
        AbstractAgent::moveForward(nextState);
        // Reset episode on CLEAN_RESTART (new "episode" in WebRLED terms)
        if (actionTaken && actionTaken->getActionType() == ActionType::CLEAN_RESTART) {
            _episodeStateCount.clear();
            _episodeSteps = 0;
            BDLOG("ICMAgent: moveForward CLEAN_RESTART, reset episode");
        }
        // Finite-length episode: reset episode counts after kMaxEpisodeSteps (align with WebRLED §3.6)
        _episodeSteps++;
        if (_episodeSteps >= kMaxEpisodeSteps) {
            _episodeStateCount.clear();
            _episodeSteps = 0;
            BDLOG("ICMAgent: episode step limit %d reached, reset episode", kMaxEpisodeSteps);
        }
        if (nextState) {
            uintptr_t toHash = nextState->hash();
            _episodeStateCount[toHash] = _episodeStateCount[toHash] + 1;
            _globalStateCount[toHash] = _globalStateCount[toHash] + 1;
            BDLOG("ICMAgent: moveForward toHash=0x%zx episodeSteps=%d episodeCount[to]=%d globalCount[to]=%d #states=%zu",
                  (size_t)toHash, _episodeSteps, _episodeStateCount[toHash], _globalStateCount[toHash], _episodeStateCount.size());
            if (_episodeStateCount.size() > kMaxEpisodeStateCount) {
                _episodeStateCount.clear();
                _episodeSteps = 0;
                BDLOG("ICMAgent: episode state count limit %zu reached, reset episode", kMaxEpisodeStateCount);
            }

            // Record successor statistics (fromState, action) -> nextState.
            // Key by action->hash() which already encodes (actionType, fromStateHash, targetHash).
            if (fromState && actionTaken && actionTaken->isValid() && !actionTaken->isNop()) {
                ActionType t = actionTaken->getActionType();
                // Skip global-jump / reset actions to avoid polluting the local navigation model.
                if (t != ActionType::CLEAN_RESTART &&
                    t != ActionType::RESTART &&
                    t != ActionType::START &&
                    t != ActionType::ACTIVATE &&
                    t != ActionType::DEEP_LINK &&
                    t != ActionType::FUZZ) {
                    uintptr_t fromHash = fromState->hash();
                    uintptr_t actionHash = actionTaken->hash();

                    // Update in/out degree for bottleneck estimation.
                    _outDegree[fromHash] = _outDegree[fromHash] + 1;
                    _inDegree[toHash] = _inDegree[toHash] + 1;

                    // Update successor top-K counts.
                    SuccessorStats &st = _succStats[actionHash];
                    st.total++;
                    bool found = false;
                    for (auto &kv : st.topNext) {
                        if (kv.first == toHash) {
                            kv.second++;
                            found = true;
                            break;
                        }
                    }
                    if (!found) {
                        if (static_cast<int>(st.topNext.size()) < kSuccessorTopK) {
                            st.topNext.push_back({toHash, 1});
                        } else {
                            // Replace the least frequent entry (space-saving approximation).
                            size_t minIdx = 0;
                            for (size_t i = 1; i < st.topNext.size(); ++i) {
                                if (st.topNext[i].second < st.topNext[minIdx].second) minIdx = i;
                            }
                            st.topNext[minIdx] = {toHash, 1};
                        }
                    }

                    // Track self-loops per action hash.
                    if (fromHash == toHash) {
                        _selfLoopCount[actionHash] = _selfLoopCount[actionHash] + 1;
                    }
                }
            }

            // Update rolling path signature for path diversity.
            _recentStates.push_back(toHash);
            if (static_cast<int>(_recentStates.size()) > kPathWindowSize) {
                _recentStates.pop_front();
            }
            if (!_recentStates.empty()) {
                // Simple FNV-like hash over the window.
                std::uint64_t h = 1469598103934665603ULL;
                const std::uint64_t prime = 1099511628211ULL;
                for (uintptr_t sh : _recentStates) {
                    std::uint64_t x = static_cast<std::uint64_t>(sh);
                    h ^= x + 0x9e3779b97f4a7c15ULL + (h << 6U) + (h >> 2U);
                    h *= prime;
                }
                _pathCount[h] = _pathCount[h] + 1;
            }
        }
    }

    double ICMAgent::getCuriosityScore(const ActivityStateActionPtr &action, double episodeMod, double stateFactor) const {
        if (!action || !action->isValid()) return -1.0;
        int visitCount = action->getVisitedCount();
        double globalNovelty = 1.0 / (1.0 + static_cast<double>(visitCount));
        if (episodeMod < 1e-6) episodeMod = 1e-6;
        if (stateFactor < 1.0) stateFactor = 1.0;
        if (stateFactor > kRewardCap) stateFactor = kRewardCap;
        double raw = globalNovelty * episodeMod * stateFactor;
        return (raw > kRewardCap) ? kRewardCap : raw;
    }

    ActionPtr ICMAgent::fallbackPickAction() const {
        StatePtr state = this->_newState;
        if (!state) return nullptr;
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
        if (best) return best;
        ActionPtr action = state->randomPickAction(this->_validateFilter);
        if (action) return action;
        ActivityStateActionPtr backAction = state->getBackAction();
        if (backAction && (!_validateFilter || _validateFilter->include(backAction)))
            return backAction;
        return nullptr;
    }

    ActionPtr ICMAgent::selectNewAction() {
        StatePtr state = this->_newState;
        if (!state) {
            BDLOG("ICMAgent: no state, fallback");
            return fallbackPickAction();
        }
        uintptr_t currentHash = state->hash();
        int blockTimes = getCurrentStateBlockTimes();

        // ---------- Anti-stuck (align with Frontier/BFS/DFS) ----------
        if (blockTimes > kBlockCleanRestartThreshold) {
            BDLOG("ICMAgent: blocked %d steps (>%d), CLEAN_RESTART", blockTimes, kBlockCleanRestartThreshold);
            return Action::CLEAN_RESTART;
        }
        if (blockTimes > kBlockDeepLinkThreshold) {
            BDLOG("ICMAgent: blocked %d steps (>%d), DEEP_LINK", blockTimes, kBlockDeepLinkThreshold);
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
                BDLOG("ICMAgent: blocked %d steps (>%d), BACK (only action)", blockTimes, kBlockBackThreshold);
                return backAction;
            }
        }

        // ---------- Collect valid actions and curiosity scores (WebRLED §3.5: episodic = 1/√(1+n)) ----------
        auto it = _episodeStateCount.find(currentHash);
        int episodeCount = (it != _episodeStateCount.end()) ? it->second : 0;
        int n = (episodeCount < kEpisodeCap) ? episodeCount : kEpisodeCap;
        double episodeMod = 1.0 / std::sqrt(1.0 + static_cast<double>(n));
        auto git = _globalStateCount.find(currentHash);
        int globalCount = (git != _globalStateCount.end()) ? git->second : 0;
        int gcap = (globalCount < kGlobalStateCap) ? globalCount : kGlobalStateCap;
        double stateFactor = 1.0 + kGlobalStateBonus * static_cast<double>(gcap);

        // Lightweight bottleneck bonus: prefer structural "junction" states (high in/out degree, low globalCount).
        auto itIn = _inDegree.find(currentHash);
        auto itOut = _outDegree.find(currentHash);
        int inDeg = (itIn != _inDegree.end()) ? itIn->second : 0;
        int outDeg = (itOut != _outDegree.end()) ? itOut->second : 0;
        if (inDeg > 0 || outDeg > 0) {
            double b = std::log(1.0 + static_cast<double>(inDeg)) *
                       std::log(1.0 + static_cast<double>(outDeg)) /
                       (1.0 + static_cast<double>(globalCount));
            if (b > 0.0) {
                stateFactor *= (1.0 + kBottleneckBeta * b);
            }
        }

        BDLOG("ICMAgent: select stateHash=0x%zx episodeCount=%d n=%d episodeMod=%.4f globalCount=%d stateFactor=%.4f",
              (size_t)currentHash, episodeCount, n, episodeMod, globalCount, stateFactor);
        // Path diversity: compute hash over recent window to detect over-used trajectories.
        bool pathOverused = false;
        if (!_recentStates.empty()) {
            std::uint64_t h = 1469598103934665603ULL;
            const std::uint64_t prime = 1099511628211ULL;
            for (uintptr_t sh : _recentStates) {
                std::uint64_t x = static_cast<std::uint64_t>(sh);
                h ^= x + 0x9e3779b97f4a7c15ULL + (h << 6U) + (h >> 2U);
                h *= prime;
            }
            auto itPath = _pathCount.find(h);
            if (itPath != _pathCount.end() && itPath->second >= kPathPenaltyThreshold) {
                pathOverused = true;
            }
        }

        const ActivityStateActionPtrVec &actions = state->getActions();
        std::vector<std::pair<ActivityStateActionPtr, double>> scored;
        scored.reserve(actions.size());
        for (const ActivityStateActionPtr &a : actions) {
            if (!a || !a->isValid()) continue;
            // Skip NOP in the main policy as well to avoid treating "do nothing" actions as exploration candidates.
            if (a->isNop()) continue;
            if (_validateFilter && !_validateFilter->include(a)) continue;
            double s = getCuriosityScore(a, episodeMod, stateFactor);
            if (s > 0.0) {
                // Apply self-loop penalty: if this (state, action) has repeatedly resulted in the same state
                // hash, down-weight its score so that ICMAgent is less likely to keep trying it.
                uintptr_t actionHash = a->hash();
                auto itSelf = _selfLoopCount.find(actionHash);
                if (itSelf != _selfLoopCount.end() && itSelf->second >= kSelfLoopPenaltyThreshold) {
                    s *= kSelfLoopPenaltyFactor;
                }

                // Successor-state novelty: prefer actions that tend to lead to globally less-visited states.
                auto itSucc = _succStats.find(actionHash);
                if (itSucc != _succStats.end() && !itSucc->second.topNext.empty()) {
                    const SuccessorStats &st = itSucc->second;
                    double weightedGlobal = 0.0;
                    int used = 0;
                    for (const auto &kv : st.topNext) {
                        uintptr_t nextHash = kv.first;
                        int cnt = kv.second;
                        auto git2 = _globalStateCount.find(nextHash);
                        int gc = (git2 != _globalStateCount.end()) ? git2->second : 0;
                        weightedGlobal += static_cast<double>(cnt) * static_cast<double>(gc);
                        used += cnt;
                    }
                    if (used > 0) {
                        double expectedGlobal = weightedGlobal / static_cast<double>(used);
                        double succFactor = 1.0 / (1.0 + expectedGlobal);
                        if (succFactor < kSuccessorMinFactor) succFactor = kSuccessorMinFactor;
                        // Exponentiate to control strength.
                        if (kSuccessorAlpha != 1.0) succFactor = std::pow(succFactor, kSuccessorAlpha);
                        s *= succFactor;
                    }
                }

                // Path-level penalty: if the recent trajectory pattern has been seen many times,
                // slightly down-weight all actions at this state to encourage leaving this path.
                if (pathOverused) {
                    s *= kPathPenaltyFactor;
                }
            }
            if (s >= 0.0) scored.push_back({a, s});
        }
        if (scored.empty()) {
            BDLOG("ICMAgent: no valid actions, fallback");
            return fallbackPickAction();
        }

        // ---------- ε-greedy: decay from kEpsilonInitial to kEpsilonMin over kEpsilonDecaySteps ----------
        _selectCount++;
        double decayProgress = static_cast<double>(_selectCount < kEpsilonDecaySteps ? _selectCount : kEpsilonDecaySteps) / static_cast<double>(kEpsilonDecaySteps);
        double epsilon = kEpsilonInitial - (kEpsilonInitial - kEpsilonMin) * decayProgress;
        if (epsilon < kEpsilonMin) epsilon = kEpsilonMin;
        std::uniform_real_distribution<double> unif(0.0, 1.0);
        double u = unif(_rng);
        if (u < epsilon) {
            // Exploration branch: sample actions proportionally to curiosity score (weighted random)
            // instead of uniform random, to reduce the probability of clearly low-value actions.
            double sumWeight = 0.0;
            for (const auto &p : scored) {
                // Use curiosity score as weight; score ∈ (0, kRewardCap].
                sumWeight += p.second;
            }
            if (sumWeight <= 0.0) {
                // 理论上不应发生，退化为均匀随机
                std::uniform_int_distribution<size_t> idxDist(0, scored.size() - 1);
                size_t i = idxDist(_rng);
                BDLOG("ICMAgent: random(uniform-fallback) u=%.3f epsilon=%.3f selectCount=%d action=%s score=%.3f",
                      u, epsilon, _selectCount, scored[i].first->toString().c_str(), scored[i].second);
                return scored[i].first;
            }
            std::uniform_real_distribution<double> wdist(0.0, sumWeight);
            double r = wdist(_rng);
            size_t chosenIdx = scored.size() - 1;
            for (size_t i = 0; i < scored.size(); ++i) {
                r -= scored[i].second;
                if (r <= 0.0) {
                    chosenIdx = i;
                    break;
                }
            }
            BDLOG("ICMAgent: random(weighted) u=%.3f epsilon=%.3f selectCount=%d action=%s score=%.3f",
                  u, epsilon, _selectCount, scored[chosenIdx].first->toString().c_str(), scored[chosenIdx].second);
            return scored[chosenIdx].first;
        }

        // Greedy: max curiosity score; tie-break by type priority then random
        double bestScore = -1.0;
        std::vector<size_t> bestIndices;
        bestIndices.reserve(32);
        for (size_t i = 0; i < scored.size(); ++i) {
            if (scored[i].second > bestScore) {
                bestScore = scored[i].second;
                bestIndices.clear();
                bestIndices.push_back(i);
            } else if (scored[i].second >= bestScore - 1e-9) {
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
                int tp = scored[i].first->getPriorityByActionType();
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
        BDLOG("ICMAgent: greedy epsilon=%.3f selectCount=%d action=%s score=%.3f",
              epsilon, _selectCount, scored[chosen].first->toString().c_str(), scored[chosen].second);
        return scored[chosen].first;
    }

}  // namespace fastbotx

#endif  // FASTBOTX_ICM_AGENT_CPP_
