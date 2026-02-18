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
        BDLOG("ICMAgent: select stateHash=0x%zx episodeCount=%d n=%d episodeMod=%.4f globalCount=%d stateFactor=%.4f",
              (size_t)currentHash, episodeCount, n, episodeMod, globalCount, stateFactor);
        const ActivityStateActionPtrVec &actions = state->getActions();
        std::vector<std::pair<ActivityStateActionPtr, double>> scored;
        scored.reserve(actions.size());
        for (const ActivityStateActionPtr &a : actions) {
            if (!a || !a->isValid()) continue;
            if (_validateFilter && !_validateFilter->include(a)) continue;
            double s = getCuriosityScore(a, episodeMod, stateFactor);
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
            std::uniform_int_distribution<size_t> idxDist(0, scored.size() - 1);
            size_t i = idxDist(_rng);
            BDLOG("ICMAgent: random u=%.3f epsilon=%.3f selectCount=%d action=%s score=%.3f",
                  u, epsilon, _selectCount, scored[i].first->toString().c_str(), scored[i].second);
            return scored[i].first;
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
