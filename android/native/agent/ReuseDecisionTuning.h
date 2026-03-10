/**
 * @file ReuseDecisionTuning.h
 * @brief Shared helpers for reuse decision tuning (loop bias, coverage diversity, prior).
 * Used by SarsaAgent and DoubleSarsaAgent when max.reuse.decisionTuning is enabled.
 */
 /**
 * @authors Zhao Zhang
 */

#ifndef ReuseDecisionTuning_H_
#define ReuseDecisionTuning_H_

#include "Base.h"
#include <unordered_map>
#include <cmath>

namespace fastbotx {

/// Reuse entry map type: activity -> visit count (same as in SarsaAgent / DoubleSarsaAgent).
using ReuseEntryM = std::unordered_map<stringPtr, int>;

namespace ReuseDecisionTuning {

    /// Weight for loop penalty: prior *= exp(-kAlphaLoop * loopBias).
    constexpr double kAlphaLoop = 2.0;
    /// Scale for reuse prior reward contribution (e.g. reward += kBetaReuse * normalizedPrior).
    constexpr double kBetaReuse = 0.1;

    /**
     * Compute loop bias for an action's activity distribution: ratio of transitions
     * that stay in currentActivity (self-loop). Caller must hold lock on the reuse model.
     * @param actMap  activity -> count for this action
     * @param currentActivity  current activity pointer (may be null)
     * @return self-loop ratio in [0,1], or 0 if no data or null activity
     */
    inline double computeLoopBiasFromEntry(const ReuseEntryM &actMap,
                                           const stringPtr &currentActivity) {
        if (!currentActivity) return 0.0;
        int total = 0;
        int selfCount = 0;
        for (const auto &entry : actMap) {
            int c = entry.second;
            total += c;
            if (entry.first && *(entry.first) == *currentActivity)
                selfCount += c;
        }
        if (total <= 0) return 0.0;
        return static_cast<double>(selfCount) / static_cast<double>(total);
    }

    /**
     * Compute coverage diversity for an action: log1p(number of distinct target activities).
     * Caller must hold lock on the reuse model.
     */
    inline double computeCoverageDiversityFromEntry(const ReuseEntryM &actMap) {
        size_t k = actMap.size();
        if (k == 0) return 0.0;
        return std::log1p(static_cast<double>(k));
    }

    /**
     * Compute reuse prior from base reuse value, loop bias, and diversity.
     * prior = reuseValue * exp(-kAlphaLoop * loopBias) + diversity
     */
    inline double computeReusePrior(double reuseValue, double loopBias, double diversity) {
        double loopFactor = std::exp(-kAlphaLoop * loopBias);
        return reuseValue * loopFactor + diversity;
    }
}
}

#endif /* ReuseDecisionTuning_H_ */
