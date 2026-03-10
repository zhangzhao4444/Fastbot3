/**
 * @authors Zhao Zhang
 */
/**
 * CuriosityAgent: curiosity-driven exploration (WebRLED-aligned dual novelty + ε-greedy).
 */

#ifndef FASTBOTX_CURIOSITY_AGENT_CPP_
#define FASTBOTX_CURIOSITY_AGENT_CPP_

#include "CuriosityAgent.h"

#include "../utils.hpp"
#include "../desc/State.h"
#include "../desc/Action.h"
#include "../model/Model.h"

#include <cmath>
#include <unordered_set>
#include <string>
#include <sstream>
#include <iomanip>

namespace fastbotx {

    namespace {
        inline std::string embeddingSummary(const std::vector<double> &emb, int headK) {
            if (emb.empty()) return "empty";
            double sum = 0.0;
            double sumSq = 0.0;
            for (double v : emb) {
                sum += v;
                sumSq += v * v;
            }
            double mean = sum / static_cast<double>(emb.size());
            double var = (sumSq / static_cast<double>(emb.size())) - mean * mean;
            if (var < 0.0) var = 0.0;
            double l2 = std::sqrt(sumSq);

            std::stringstream ss;
            ss.setf(std::ios::fixed);
            ss << "l2=" << std::setprecision(4) << l2
               << " mean=" << std::setprecision(4) << mean
               << " var=" << std::setprecision(6) << var
               << " head=[";
            int k = headK;
            if (k < 0) k = 0;
            if (k > static_cast<int>(emb.size())) k = static_cast<int>(emb.size());
            for (int i = 0; i < k; ++i) {
                if (i) ss << ",";
                ss << std::setprecision(4) << emb[static_cast<size_t>(i)];
            }
            ss << "]";
            return ss.str();
        }
    } // namespace

    CuriosityAgent::CuriosityAgent(const ModelPtr &model)
            : AbstractAgent(model) {
        this->_algorithmType = AlgorithmType::Curiosity;
        _clusterDim = 16;  // handcrafted default (HandcraftedStateEncoder::kHandcraftedDim)
        BLOG("CuriosityAgent: initialized (curiosity-driven, WebRLED-style dual novelty)");
    }

    void CuriosityAgent::setStateEncoder(const IStateEncoderPtr &encoder) {
        _stateEncoder = encoder;
        _clusterDim = encoder ? encoder->getOutputDim() : 16;
    }

    void CuriosityAgent::updateStrategy() {
        // No Q-values or reuse model; curiosity is purely count-based.
    }

    void CuriosityAgent::onStateAbstractionChanged() {
        _episodeStateCount.clear();
        _episodeSteps = 0;
        _globalStateCount.clear();
        _smoothedGlobalStateCount.clear();
        _succStats.clear();
        _selfLoopCount.clear();
        _stateOutDegree.clear();
        _recentStates.clear();
        _pathCount.clear();
        _lastPathSignatureValid = false;
        _stateClusterIndex.clear();
        _clusterCount.clear();
        _clusterCentroids.clear();
        BDLOG("CuriosityAgent: state abstraction changed, cleared episode and global counts");
    }

    void CuriosityAgent::moveForward(StatePtr nextState) {
        StatePtr fromState = this->_newState;
        ActivityStateActionPtr actionTaken = this->_newAction;
        AbstractAgent::moveForward(nextState);
        _moveForwardCount++;
        // Reset episode on CLEAN_RESTART (new "episode" in WebRLED terms)
        if (actionTaken && actionTaken->getActionType() == ActionType::CLEAN_RESTART) {
            _episodeStateCount.clear();
            _episodeSteps = 0;
            _recentStates.clear();
            _pathCount.clear();
            _lastPathSignatureValid = false;
            BDLOG("CuriosityAgent: moveForward CLEAN_RESTART, reset episode");
        }
        // Finite-length episode: reset episode counts after kMaxEpisodeSteps (align with WebRLED §3.6)
        _episodeSteps++;
        if (_episodeSteps >= kMaxEpisodeSteps) {
            _episodeStateCount.clear();
            _episodeSteps = 0;
            BDLOG("CuriosityAgent: episode step limit %d reached, reset episode", kMaxEpisodeSteps);
        }
        if (nextState) {
            uintptr_t toHash = nextState->hash();
            _episodeStateCount[toHash] = _episodeStateCount[toHash] + 1;
            _globalStateCount[toHash] = _globalStateCount[toHash] + 1;
            if (kEnableCountSmoothing) {
                double prev = 0.0;
                auto itS = _smoothedGlobalStateCount.find(toHash);
                if (itS != _smoothedGlobalStateCount.end()) prev = itS->second;
                _smoothedGlobalStateCount[toHash] = (1.0 - kSmoothBeta) * prev + kSmoothBeta * static_cast<double>(_globalStateCount[toHash]);
            }
            // Bottleneck (3.1): record out-degree of state (number of actions) for hub/leaf scoring.
            if (kEnableBottleneckDiversity) {
                int outDeg = static_cast<int>(nextState->getActions().size());
                auto itOd = _stateOutDegree.find(toHash);
                if (itOd == _stateOutDegree.end())
                    _stateOutDegree[toHash] = outDeg;
                else if (outDeg > itOd->second)
                    itOd->second = outDeg;
            }
            // Cluster novelty (learned embedding + online clustering): compute embedding, assign to nearest cluster, update centroid.
            if (kEnableClusterNovelty) {
                std::vector<double> emb = computeStateEmbedding(nextState);
                if (emb.size() == static_cast<size_t>(_clusterDim)) {
                    int cidx = assignStateToCluster(toHash, emb);
                    if (cidx >= 0 && cidx < static_cast<int>(_clusterCount.size()))
                        _clusterCount[cidx] = _clusterCount[cidx] + 1;
                    if (kEnableEmbeddingLog && (kEmbeddingLogEvery > 0) && (_moveForwardCount % kEmbeddingLogEvery == 0)) {
                        BDLOG("CuriosityAgent: embedding stateHash=0x%zx dim=%zu cluster=%d %s",
                              (size_t)toHash, emb.size(), cidx, embeddingSummary(emb, kEmbeddingLogHead).c_str());
                    }
                }
            }
            size_t graphStates = 0;
            {
                auto modelPtr = _model.lock();
                if (modelPtr) {
                    const GraphPtr &g = modelPtr->getGraph();
                    if (g) graphStates = g->getStates().size();
                }
            }
            size_t widgetsInState = nextState->getWidgets().size();
            BDLOG("CuriosityAgent: moveForward toHash=0x%zx episodeSteps=%d episodeCount[to]=%d globalCount[to]=%d #episode_states=%zu #global_states=%zu #graph_states=%zu #widgets=%zu",
                  (size_t)toHash, _episodeSteps, _episodeStateCount[toHash], _globalStateCount[toHash], _episodeStateCount.size(), _globalStateCount.size(), graphStates, widgetsInState);
            if (_episodeStateCount.size() > kMaxEpisodeStateCount) {
                _episodeStateCount.clear();
                _episodeSteps = 0;
                BDLOG("CuriosityAgent: episode state count limit %zu reached, reset episode", kMaxEpisodeStateCount);
            }

            // Update sliding window of recent state hashes for path diversity (optional; off by default).
            if (kEnablePathDiversity) {
                _recentStates.push_back(toHash);
                if (_recentStates.size() > static_cast<size_t>(kPathWindow)) {
                    _recentStates.pop_front();
                }
                if (_recentStates.size() >= static_cast<size_t>(kPathMinLengthForPenalty)) {
                    std::uint64_t h = 1469598103934665603ULL; // FNV offset basis
                    for (uintptr_t sh : _recentStates) {
                        std::uint64_t v = static_cast<std::uint64_t>(sh);
                        h ^= v + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
                    }
                    _pathCount[h] = _pathCount[h] + 1;
                    _lastPathSignature = h;
                    _lastPathSignatureValid = true;
                }
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
        }
    }

    double CuriosityAgent::getCuriosityScore(const ActivityStateActionPtr &action, double episodeMod, double stateFactor) const {
        if (!action || !action->isValid()) return -1.0;
        int visitCount = action->getVisitedCount();
        double globalNovelty = 1.0 / (1.0 + static_cast<double>(visitCount));
        if (episodeMod < 1e-6) episodeMod = 1e-6;
        if (stateFactor < 1.0) stateFactor = 1.0;
        if (stateFactor > kRewardCap) stateFactor = kRewardCap;
        double raw = globalNovelty * episodeMod * stateFactor;
        return (raw > kRewardCap) ? kRewardCap : raw;
    }

    ActionPtr CuriosityAgent::fallbackPickAction() const {
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

    ActionPtr CuriosityAgent::selectNewAction() {
        StatePtr state = this->_newState;
        if (!state) {
            BDLOG("CuriosityAgent: no state, fallback");
            return fallbackPickAction();
        }
        uintptr_t currentHash = state->hash();
        int blockTimes = getCurrentStateBlockTimes();

        // ---------- Anti-stuck (align with Frontier/BFS/DFS) ----------
        if (blockTimes > kBlockCleanRestartThreshold) {
            BDLOG("CuriosityAgent: blocked %d steps (>%d), CLEAN_RESTART", blockTimes, kBlockCleanRestartThreshold);
            return Action::CLEAN_RESTART;
        }
        if (blockTimes > kBlockDeepLinkThreshold) {
            BDLOG("CuriosityAgent: blocked %d steps (>%d), DEEP_LINK", blockTimes, kBlockDeepLinkThreshold);
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
                BDLOG("CuriosityAgent: blocked %d steps (>%d), BACK (only action)", blockTimes, kBlockBackThreshold);
                return backAction;
            }
        }

        // ---------- Collect valid actions and curiosity scores (WebRLED §3.5: episodic = 1/√(1+n)) ----------
        auto it = _episodeStateCount.find(currentHash);
        int episodeCount = (it != _episodeStateCount.end()) ? it->second : 0;
        int n = (episodeCount < kEpisodeCap) ? episodeCount : kEpisodeCap;
        // Performance optimization: use precomputed lookup table instead of std::sqrt
        double episodeMod = kEpisodeModTable[n];
        int globalCount;
        if (kEnableCountSmoothing) {
            auto itS = _smoothedGlobalStateCount.find(currentHash);
            globalCount = (itS != _smoothedGlobalStateCount.end()) ? static_cast<int>(std::round(itS->second)) : 0;
        } else {
            auto git = _globalStateCount.find(currentHash);
            globalCount = (git != _globalStateCount.end()) ? git->second : 0;
        }
        int gcap = (globalCount < kGlobalStateCap) ? globalCount : kGlobalStateCap;
        double stateFactor = 1.0 + kGlobalStateBonus * static_cast<double>(gcap);
        if (kEnableCurriculum) {
            double progress = std::min(1.0, static_cast<double>(_selectCount) / static_cast<double>(kCurriculumSteps));
            episodeMod = episodeMod + (1.0 - episodeMod) * (1.0 - progress) * kCurriculumEarlyBlend;
            if (episodeMod < 1e-6) episodeMod = 1e-6;
            if (episodeMod > 1.0) episodeMod = 1.0;
        }
        BDLOG("CuriosityAgent: select stateHash=0x%zx episodeCount=%d n=%d episodeMod=%.4f globalCount=%d stateFactor=%.4f",
              (size_t)currentHash, episodeCount, n, episodeMod, globalCount, stateFactor);

        // Long-horizon novelty: frontier = states that have at least one unvisited action (lightweight, no BFS).
        std::unordered_set<uintptr_t> frontierHashes;
        if (kEnableLongHorizonNovelty) {
            auto modelPtr = _model.lock();
            if (modelPtr) {
                const GraphPtr &graph = modelPtr->getGraph();
                if (graph) {
                    for (const StatePtr &s : graph->getStates()) {
                        if (!s) continue;
                        for (const ActivityStateActionPtr &ac : s->getActions()) {
                            if (ac && ac->isValid() && ac->getVisitedCount() <= 0) {
                                frontierHashes.insert(s->hash());
                                break;
                            }
                        }
                    }
                }
            }
        }

        const ActivityStateActionPtrVec &actions = state->getActions();
        std::vector<std::pair<ActivityStateActionPtr, double>> scored;
        scored.reserve(actions.size());
        // Path-level diversity factor (optional; use cached signature from last moveForward when valid).
        double pathFactor = 1.0;
        if (kEnablePathDiversity && _recentStates.size() >= static_cast<size_t>(kPathMinLengthForPenalty)) {
            std::uint64_t pathSig = _lastPathSignatureValid ? _lastPathSignature : 0;
            if (!_lastPathSignatureValid) {
                pathSig = 1469598103934665603ULL;
                for (uintptr_t sh : _recentStates) {
                    std::uint64_t v = static_cast<std::uint64_t>(sh);
                    pathSig ^= v + 0x9e3779b97f4a7c15ULL + (pathSig << 6) + (pathSig >> 2);
                }
            }
            auto itPath = _pathCount.find(pathSig);
            int pathVisited = (itPath != _pathCount.end()) ? itPath->second : 0;
            pathFactor = 1.0 / (1.0 + static_cast<double>(pathVisited));
            if (pathFactor < kPathMinFactor) pathFactor = kPathMinFactor;
            if (kPathAlpha != 1.0) pathFactor = std::pow(pathFactor, kPathAlpha);
        }

        for (const ActivityStateActionPtr &a : actions) {
            if (!a || !a->isValid()) continue;
            // Skip NOP in the main policy as well to avoid treating "do nothing" actions as exploration candidates.
            if (a->isNop()) continue;
            if (_validateFilter && !_validateFilter->include(a)) continue;
            double s = getCuriosityScore(a, episodeMod, stateFactor);
            if (s > 0.0) {
                // Apply self-loop penalty: if this (state, action) has repeatedly resulted in the same state
                // hash, down-weight its score so that CuriosityAgent is less likely to keep trying it.
                uintptr_t actionHash = a->hash();
                auto itSelf = _selfLoopCount.find(actionHash);
                if (itSelf != _selfLoopCount.end() && itSelf->second >= kSelfLoopPenaltyThreshold) {
                    s *= kSelfLoopPenaltyFactor;
                }

                // Single lookup for successor stats; reuse for successor/bottleneck/long-horizon/cluster factors.
                auto itSucc = _succStats.find(actionHash);
                if (itSucc != _succStats.end() && !itSucc->second.topNext.empty()) {
                    const SuccessorStats &st = itSucc->second;

                    // Successor-state novelty: prefer actions that tend to lead to globally less-visited states.
                    double weightedGlobal = 0.0;
                    int used = 0;
                    for (const auto &kv : st.topNext) {
                        uintptr_t nextHash = kv.first;
                        int cnt = kv.second;
                        int gc;
                        if (kEnableCountSmoothing) {
                            auto itS2 = _smoothedGlobalStateCount.find(nextHash);
                            gc = (itS2 != _smoothedGlobalStateCount.end()) ? static_cast<int>(std::round(itS2->second)) : 0;
                        } else {
                            auto git2 = _globalStateCount.find(nextHash);
                            gc = (git2 != _globalStateCount.end()) ? git2->second : 0;
                        }
                        weightedGlobal += static_cast<double>(cnt) * static_cast<double>(gc);
                        used += cnt;
                    }
                    if (used > 0) {
                        double expectedGlobal = weightedGlobal / static_cast<double>(used);
                        double succFactor = 1.0 / (1.0 + expectedGlobal);
                        if (succFactor < kSuccessorMinFactor) succFactor = kSuccessorMinFactor;
                        if (kSuccessorAlpha != 1.0) succFactor = std::pow(succFactor, kSuccessorAlpha);
                        s *= succFactor;
                    }

                    // Bottleneck (3.1): down-weight actions that tend to lead to hub states (high out-degree).
                    if (kEnableBottleneckDiversity) {
                        double weightedOutDeg = 0.0;
                        int usedB = 0;
                        for (const auto &kv : st.topNext) {
                            uintptr_t nextHash = kv.first;
                            int cnt = kv.second;
                            auto itOd = _stateOutDegree.find(nextHash);
                            int od = (itOd != _stateOutDegree.end()) ? itOd->second : 0;
                            if (od > kBottleneckOutDegreeCap) od = kBottleneckOutDegreeCap;
                            weightedOutDeg += static_cast<double>(cnt) * static_cast<double>(od);
                            usedB += cnt;
                        }
                        if (usedB > 0) {
                            double expectedOutDeg = weightedOutDeg / static_cast<double>(usedB);
                            double bottleneckFactor = 1.0 / (1.0 + expectedOutDeg);
                            if (bottleneckFactor < kBottleneckMinFactor) bottleneckFactor = kBottleneckMinFactor;
                            s *= bottleneckFactor;
                        }
                    }

                    // Long-horizon novelty: boost actions whose successors often lie in frontier.
                    if (kEnableLongHorizonNovelty && !frontierHashes.empty()) {
                        int frontierCount = 0;
                        for (const auto &kv : st.topNext) {
                            if (frontierHashes.count(kv.first)) frontierCount++;
                        }
                        if (frontierCount > 0) {
                            int cap = (frontierCount < kLongHorizonFrontierCap) ? frontierCount : kLongHorizonFrontierCap;
                            double longHorizonFactor = 1.0 + kLongHorizonFrontierBonus * static_cast<double>(cap);
                            s *= longHorizonFactor;
                        }
                    }

                    // Cluster novelty: prefer actions leading to rarely-visited clusters.
                    if (kEnableClusterNovelty) {
                        double weightedCluster = 0.0;
                        int usedC = 0;
                        for (const auto &kv : st.topNext) {
                            uintptr_t nextHash = kv.first;
                            int cnt = kv.second;
                            auto itCi = _stateClusterIndex.find(nextHash);
                            if (itCi != _stateClusterIndex.end()) {
                                int cidx = itCi->second;
                                if (cidx >= 0 && cidx < static_cast<int>(_clusterCount.size())) {
                                    weightedCluster += static_cast<double>(cnt) * static_cast<double>(_clusterCount[static_cast<size_t>(cidx)]);
                                    usedC += cnt;
                                }
                            }
                        }
                        if (usedC > 0) {
                            double expectedCluster = weightedCluster / static_cast<double>(usedC);
                            double clusterFactor = 1.0 / (1.0 + expectedCluster);
                            if (clusterFactor < kClusterMinFactor) clusterFactor = kClusterMinFactor;
                            s *= clusterFactor;
                        }
                    }
                }

                if (kEnablePathDiversity) s *= pathFactor;
            }
            if (s >= 0.0) scored.push_back({a, s});
        }
        if (scored.empty()) {
            BDLOG("CuriosityAgent: no valid actions, fallback");
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
                // Should not happen in practice; fall back to uniform random.
                std::uniform_int_distribution<size_t> idxDist(0, scored.size() - 1);
                size_t i = idxDist(_rng);
                BDLOG("CuriosityAgent: random(uniform-fallback) u=%.3f epsilon=%.3f selectCount=%d action=%s score=%.3f",
                      u, epsilon, _selectCount, scored[i].first->toString().c_str(), scored[i].second);
                return scored[i].first;
            }
            std::uniform_real_distribution<double> wdist(0.0, sumWeight);
            double r = wdist(_rng);
            size_t chosenIdx = scored.size() - 1;  // fallback if r never <= 0 due to float rounding
            for (size_t i = 0; i < scored.size(); ++i) {
                r -= scored[i].second;
                if (r <= 0.0) {
                    chosenIdx = i;
                    break;
                }
            }
            BDLOG("CuriosityAgent: random(weighted) u=%.3f epsilon=%.3f selectCount=%d action=%s score=%.3f",
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
        BDLOG("CuriosityAgent: greedy epsilon=%.3f selectCount=%d action=%s score=%.3f",
              epsilon, _selectCount, scored[chosen].first->toString().c_str(), scored[chosen].second);
        return scored[chosen].first;
    }

    std::vector<double> CuriosityAgent::computeStateEmbedding(const StatePtr &state) const {
        if (_stateEncoder) {
            std::vector<double> enc = _stateEncoder->encode(state);
            if (enc.size() == static_cast<size_t>(_clusterDim)) return enc;
        }
        return computeStateEmbeddingHandcrafted(state);
    }

    std::vector<double> CuriosityAgent::computeStateEmbeddingHandcrafted(const StatePtr &state) {
        return HandcraftedStateEncoder().encode(state);
    }

    int CuriosityAgent::assignStateToCluster(uintptr_t stateHash, const std::vector<double> &embedding) {
        if (embedding.size() != static_cast<size_t>(_clusterDim)) return -1;
        // Bootstrap: use first kNumClusters distinct states as initial centroids.
        if (_clusterCentroids.size() < static_cast<size_t>(kNumClusters)) {
            size_t idx = _clusterCentroids.size();
            _clusterCentroids.push_back(embedding);
            _clusterCount.push_back(0);
            _stateClusterIndex[stateHash] = static_cast<int>(idx);
            if (kEnableEmbeddingLog) {
                BDLOG("CuriosityAgent: cluster_init idx=%zu stateHash=0x%zx dim=%zu %s",
                      idx, (size_t)stateHash, embedding.size(), embeddingSummary(embedding, kEmbeddingLogHead).c_str());
            }
            return static_cast<int>(idx);
        }
        // Find nearest cluster by L2 distance.
        int best = 0;
        double bestDist = 1e30;
        // Performance optimization: early exit if distance is very small (embedding matches centroid closely)
        static constexpr double kClusterEarlyExitThreshold = 1e-6;
        for (size_t i = 0; i < _clusterCentroids.size(); ++i) {
            double d = 0.0;
            for (int j = 0; j < _clusterDim; ++j) {
                size_t jj = static_cast<size_t>(j);
                double diff = embedding[jj] - _clusterCentroids[i][jj];
                d += diff * diff;
            }
            if (d < bestDist) {
                bestDist = d;
                best = static_cast<int>(i);
                // Early exit if distance is very small (exact or near-exact match)
                if (bestDist < kClusterEarlyExitThreshold) break;
            }
        }
        _stateClusterIndex[stateHash] = best;
        // Online centroid update: moving average.
        double alpha = kClusterCentroidAlpha;
        for (int j = 0; j < _clusterDim; ++j) {
            size_t jj = static_cast<size_t>(j);
            _clusterCentroids[static_cast<size_t>(best)][jj] += alpha * (embedding[jj] - _clusterCentroids[static_cast<size_t>(best)][jj]);
        }
        if (kEnableEmbeddingLog && (kEmbeddingLogEvery > 0) && (_moveForwardCount % kEmbeddingLogEvery == 0)) {
            BDLOG("CuriosityAgent: cluster_assign stateHash=0x%zx best=%d dist2=%.6f count=%d",
                  (size_t)stateHash, best, bestDist,
                  (best >= 0 && best < static_cast<int>(_clusterCount.size())) ? _clusterCount[static_cast<size_t>(best)] : -1);
        }
        return best;
    }

}  // namespace fastbotx

#endif  // FASTBOTX_CURIOSITY_AGENT_CPP_
