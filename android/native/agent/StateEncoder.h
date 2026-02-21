/*
 * This code is licensed under the Fastbot license. You may obtain a copy of this license in the LICENSE.txt file in the root directory of this source tree.
 */
/**
 * State encoder interface for pluggable embedding (e.g. DNN) in ICMAgent cluster novelty.
 * Implement this interface and set via ICMAgent::setStateEncoder() to replace hand-crafted embedding.
 */
#ifndef FASTBOTX_STATE_ENCODER_H
#define FASTBOTX_STATE_ENCODER_H

#include "../desc/State.h"
#include "../desc/Action.h"
#include "../Base.h"
#include "FeatureSelector.h"
#include <vector>
#include <memory>
#include <string>
#include <limits>
#include <cmath>
#include <random>
#include <cstddef>
#include <set>

namespace fastbotx {

    /**
     * Interface for state -> embedding. Used by ICMAgent when kEnableClusterNovelty is true.
     * getOutputDim() must match encode() result size; ICMAgent uses it for runtime cluster dimension.
     */
    class IStateEncoder {
    public:
        virtual ~IStateEncoder() = default;

        /** Return embedding for the given state; size must equal getOutputDim(). */
        virtual std::vector<double> encode(const StatePtr &state) = 0;

        /** Return embedding dimension (used for clustering); must match encode(state).size(). */
        virtual int getOutputDim() const = 0;
    };

    using IStateEncoderPtr = std::shared_ptr<IStateEncoder>;

    /** Strategy 5.1: normalization helpers (in-place). */
    inline void minMaxNormalize(std::vector<double> &features,
                                const std::vector<double> &mins,
                                const std::vector<double> &maxs) {
        const size_t n = std::min({features.size(), mins.size(), maxs.size()});
        for (size_t i = 0; i < n; ++i) {
            double range = maxs[i] - mins[i];
            if (range > 1e-10) features[i] = (features[i] - mins[i]) / range;
        }
    }
    inline void zScoreNormalize(std::vector<double> &features,
                               const std::vector<double> &means,
                               const std::vector<double> &stds) {
        const size_t n = std::min({features.size(), means.size(), stds.size()});
        for (size_t i = 0; i < n; ++i) {
            if (stds[i] > 1e-10) features[i] = (features[i] - means[i]) / stds[i];
        }
    }
    inline void robustNormalize(std::vector<double> &features,
                               const std::vector<double> &medians,
                               const std::vector<double> &iqrs) {
        const size_t n = std::min({features.size(), medians.size(), iqrs.size()});
        for (size_t i = 0; i < n; ++i) {
            if (iqrs[i] > 1e-10) features[i] = (features[i] - medians[i]) / iqrs[i];
        }
    }

    /**
     * Hand-crafted features: 16-dim [activityNorm, widgetDensity, interactionDensity, textDensity,
     * actionableWidgetRatio, depthAreaProduct, scrollActionRatio (binary), longClickRatio (binary),
     * avgWidgetArea, interactionComplexity, contentRichness, scrollMatchRatio,
     * buttonRatio, imageViewRatio, avgAspectRatio, edgeButtonRatio].
     * Combined features (3.1/3.2): widgetDensity, interactionDensity, depthAreaProduct, contentRichness, scrollMatchRatio, edgeButtonRatio.
     * Optimized per GUI_TREE_FEATURE_CLUSTERING_SOTA_SURVEY: merged high-correlation pairs to reduce redundancy
     * (actionTypeDiversity+backActionRatio -> interactionComplexity; targetActionRatio+resourceIDRatio -> actionableWidgetRatio).
     * Binary: scrollActionRatio, longClickRatio are 0.0 or 1.0.
     */
    class HandcraftedStateEncoder : public IStateEncoder {
    public:
        static constexpr int kHandcraftedDim = 16;

        int getOutputDim() const override { return kHandcraftedDim; }

        std::vector<double> encode(const StatePtr &state) override {
            std::vector<double> out;
            out.reserve(static_cast<size_t>(kHandcraftedDim));
            if (!state) return out;
            std::string activityStr = state->getActivityString() ? *state->getActivityString() : "";
            double activityNorm = 0.0;
            if (!activityStr.empty()) {
                size_t h = std::hash<std::string>()(activityStr);
                activityNorm = static_cast<double>(h) / (static_cast<double>(std::numeric_limits<size_t>::max()) + 1.0);
            }
            const auto &widgets = state->getWidgets();
            const auto &actions = state->getActions();
            size_t nWidgets = widgets.size();
            size_t nActions = actions.size();
            
            // Basic counts (normalized): log transform for long-tail distribution (FEATURE_ENGINEERING_OPTIMIZATION 2.1)
            double widgetNorm = std::log(1.0 + static_cast<double>(nWidgets)) / std::log(201.0);
            if (widgetNorm > 1.0) widgetNorm = 1.0;
            double actionNorm = std::log(1.0 + static_cast<double>(nActions)) / std::log(101.0);
            if (actionNorm > 1.0) actionNorm = 1.0;
            
            // Text density: widgets with non-empty text (sqrt for smoother ratio distribution, 2.2)
            size_t nTextWidgets = 0;
            size_t totalTextLength = 0;
            for (const auto &w : widgets) {
                if (w) {
                    std::string text = w->getText();
                    if (!text.empty()) {
                        ++nTextWidgets;
                        totalTextLength += text.length();
                    }
                }
            }
            double textDensity = (nWidgets > 0) ? std::sqrt(static_cast<double>(nTextWidgets) / static_cast<double>(nWidgets)) : 0.0;
            
            // Target action ratio: actions with target widgets (sqrt for ratio, 2.2)
            size_t nTargetActions = 0;
            if (nActions > 0) {
                ActivityStateActionPtrVec targetActs = state->targetActions();
                nTargetActions = targetActs.size();
            }
            double targetActionRatio = (nActions > 0) ? std::sqrt(static_cast<double>(nTargetActions) / static_cast<double>(nActions)) : 0.0;
            
            // Average widget depth (max parent chain length); power transform for better discrimination (2.3)
            double avgDepth = 0.0;
            if (nWidgets > 0) {
                size_t totalDepth = 0;
                for (const auto &w : widgets) {
                    if (w) {
                        size_t depth = 0;
                        auto parent = w->getParent();
                        while (parent) {
                            ++depth;
                            parent = parent->getParent();
                        }
                        totalDepth += depth;
                    }
                }
                double rawDepth = static_cast<double>(totalDepth) / static_cast<double>(nWidgets) / 10.0;
                avgDepth = std::pow(std::max(0.0, rawDepth), 0.7);
                if (avgDepth > 1.0) avgDepth = 1.0;
            }
            
            // Scroll action ratio: actions that are scroll types (SCROLL_TOP_DOWN, SCROLL_BOTTOM_UP, etc.)
            size_t nScrollActions = 0;
            size_t nLongClickActions = 0;
            size_t nBackActions = 0;
            std::set<ActionType> uniqueActionTypes;
            for (const auto &a : actions) {
                if (a && a->isValid()) {
                    ActionType at = a->getActionType();
                    uniqueActionTypes.insert(at);
                    if (at == ActionType::SCROLL_TOP_DOWN || at == ActionType::SCROLL_BOTTOM_UP ||
                        at == ActionType::SCROLL_LEFT_RIGHT || at == ActionType::SCROLL_RIGHT_LEFT ||
                        at == ActionType::SCROLL_BOTTOM_UP_N) {
                        ++nScrollActions;
                    }
                    if (at == ActionType::LONG_CLICK) ++nLongClickActions;
                    if (at == ActionType::BACK) ++nBackActions;
                }
            }
            // Binary features: scrollActionRatio and longClickRatio are binarized (0.0 or 1.0) to reduce sparsity
            double scrollActionRatio = (nActions > 0 && nScrollActions > 0) ? 1.0 : 0.0;
            double longClickRatio = (nActions > 0 && nLongClickActions > 0) ? 1.0 : 0.0;
            double backActionRatio = (nActions > 0) ? (static_cast<double>(nBackActions) / static_cast<double>(nActions)) : 0.0;
            double actionTypeDiversity = (nActions > 0) ? (static_cast<double>(uniqueActionTypes.size()) / static_cast<double>(nActions)) : 0.0;
            if (actionTypeDiversity > 1.0) actionTypeDiversity = 1.0;
            // Merged feature (survey: high correlation r~0.85) to reduce dimension
            double interactionComplexity = (actionTypeDiversity + backActionRatio) / 2.0;
            
            // Average widget area (normalized by screen area ~1344*2992)
            double avgWidgetArea = 0.0;
            double avgAspectRatio = 0.0;
            size_t nResourceIDWidgets = 0;
            size_t nContentDescWidgets = 0;
            size_t nScrollableWidgets = 0;
            size_t nButtonWidgets = 0;
            size_t nImageViewWidgets = 0;
            size_t nEdgeWidgets = 0;  // widgets near screen edges
            const double screenWidth = 1344.0;
            const double screenHeight = 2992.0;
            const double screenArea = screenWidth * screenHeight;
            const double edgeThreshold = 0.05;  // 5% of screen dimension
            
            if (nWidgets > 0) {
                std::vector<double> areas;
                std::vector<double> aspectRatios;
                areas.reserve(nWidgets);
                aspectRatios.reserve(nWidgets);
                
                for (const auto &w : widgets) {
                    if (w) {
                        // Resource ID and content description coverage
                        if (!w->getResourceID().empty()) ++nResourceIDWidgets;
                        if (!w->getContentDesc().empty()) ++nContentDescWidgets;
                        
                        // Widget type distribution (based on class name)
                        std::string clazz = w->getClassname();
                        if (!clazz.empty()) {
                            if (clazz.find("Button") != std::string::npos) ++nButtonWidgets;
                            if (clazz.find("ImageView") != std::string::npos) ++nImageViewWidgets;
                            if (clazz.find("ScrollView") != std::string::npos || 
                                clazz.find("RecyclerView") != std::string::npos ||
                                clazz.find("ListView") != std::string::npos ||
                                clazz.find("GridView") != std::string::npos ||
                                clazz.find("ViewPager") != std::string::npos) {
                                ++nScrollableWidgets;
                            }
                        }
                        // Also check via hasOperate for scrollable
                        if (w->hasOperate(OperateType::Scrollable)) ++nScrollableWidgets;
                        
                        // Widget bounds and geometry
                        auto bounds = w->getBounds();
                        if (bounds && !bounds->isEmpty()) {
                            int width = bounds->right - bounds->left;
                            int height = bounds->bottom - bounds->top;
                            double area = static_cast<double>(width * height);
                            areas.push_back(area / screenArea);
                            
                            // Aspect ratio (width/height, normalized)
                            if (height > 0) {
                                double aspect = static_cast<double>(width) / static_cast<double>(height);
                                if (aspect > 10.0) aspect = 10.0;  // cap extreme ratios
                                aspectRatios.push_back(aspect / 10.0);  // normalize to [0,1]
                            }
                            
                            // Edge widgets: near screen edges (within 5% of screen dimension)
                            double leftRatio = static_cast<double>(bounds->left) / screenWidth;
                            double rightRatio = static_cast<double>(bounds->right) / screenWidth;
                            double topRatio = static_cast<double>(bounds->top) / screenHeight;
                            double bottomRatio = static_cast<double>(bounds->bottom) / screenHeight;
                            if (leftRatio < edgeThreshold || rightRatio > (1.0 - edgeThreshold) ||
                                topRatio < edgeThreshold || bottomRatio > (1.0 - edgeThreshold)) {
                                ++nEdgeWidgets;
                            }
                        }
                    }
                }
                
                if (!areas.empty()) {
                    double sum = 0.0;
                    for (double a : areas) sum += a;
                    double avgArea = sum / static_cast<double>(areas.size());
                    avgWidgetArea = std::log(1.0 + avgArea) / std::log(2.0);  // log transform (2.1)
                    if (avgWidgetArea > 1.0) avgWidgetArea = 1.0;
                }
                
                if (!aspectRatios.empty()) {
                    double sumAspect = 0.0;
                    for (double ar : aspectRatios) sumAspect += ar;
                    avgAspectRatio = sumAspect / static_cast<double>(aspectRatios.size());
                }
            }
            
            double resourceIDRatio = (nWidgets > 0) ? (static_cast<double>(nResourceIDWidgets) / static_cast<double>(nWidgets)) : 0.0;
            // Merged feature (survey: high correlation r~0.73) to reduce dimension
            double actionableWidgetRatio = (targetActionRatio + resourceIDRatio) / 2.0;
            double contentDescRatio = (nWidgets > 0) ? (static_cast<double>(nContentDescWidgets) / static_cast<double>(nWidgets)) : 0.0;
            double scrollableWidgetRatio = (nWidgets > 0) ? (static_cast<double>(nScrollableWidgets) / static_cast<double>(nWidgets)) : 0.0;
            if (scrollableWidgetRatio > 1.0) scrollableWidgetRatio = 1.0;  // may count twice (class + hasOperate)
            double buttonRatio = (nWidgets > 0) ? (static_cast<double>(nButtonWidgets) / static_cast<double>(nWidgets)) : 0.0;
            double imageViewRatio = (nWidgets > 0) ? (static_cast<double>(nImageViewWidgets) / static_cast<double>(nWidgets)) : 0.0;
            double edgeWidgetRatio = (nWidgets > 0) ? (static_cast<double>(nEdgeWidgets) / static_cast<double>(nWidgets)) : 0.0;
            
            // Strategy 3: combined features (FEATURE_ENGINEERING_OPTIMIZATION 3.1 & 3.2), replace 6 base dims to keep 16
            const double epsilon = 1e-6;
            double widgetDensity = widgetNorm / (avgWidgetArea + epsilon);
            if (widgetDensity > 10.0) widgetDensity = 10.0;
            widgetDensity /= 10.0;
            double interactionDensity = actionNorm / (widgetNorm + epsilon);
            if (interactionDensity > 5.0) interactionDensity = 5.0;
            interactionDensity /= 5.0;
            double textActionRatio = textDensity * actionableWidgetRatio;
            double depthAreaProduct = avgDepth * avgWidgetArea;
            double scrollMatchRatio = scrollActionRatio * scrollableWidgetRatio;
            double edgeButtonRatio = edgeWidgetRatio * buttonRatio;
            double contentRichness = (textDensity + contentDescRatio + actionableWidgetRatio) / 3.0;
            
            out.push_back(activityNorm);
            out.push_back(widgetDensity);          // was widgetNorm (3.2)
            out.push_back(interactionDensity);    // was actionNorm (3.2)
            out.push_back(textDensity);
            out.push_back(actionableWidgetRatio); // merged: targetActionRatio + resourceIDRatio
            out.push_back(depthAreaProduct);      // was avgDepth (3.1)
            out.push_back(scrollActionRatio);     // binary: 0.0 or 1.0
            out.push_back(longClickRatio);        // binary: 0.0 or 1.0
            out.push_back(avgWidgetArea);
            out.push_back(interactionComplexity); // merged: actionTypeDiversity + backActionRatio
            out.push_back(contentRichness);       // was contentDescRatio (3.2)
            out.push_back(scrollMatchRatio);      // was scrollableWidgetRatio (3.1)
            out.push_back(buttonRatio);
            out.push_back(imageViewRatio);
            out.push_back(avgAspectRatio);
            out.push_back(edgeButtonRatio);       // was edgeWidgetRatio (3.1)

            // L2-normalize embedding so clustering depends on relative pattern
            // of features rather than absolute scale; this also keeps different
            // screens on a comparable magnitude.
            double normSq = 0.0;
            for (double v : out) normSq += v * v;
            if (normSq > 0.0) {
                double invNorm = 1.0 / std::sqrt(normSq);
                for (double &v : out) v *= invNorm;
            }
            return out;
        }
    };

    /**
     * Wrapper encoder that applies runtime dynamic feature selection (strategy 4).
     * Uses variance-based and/or correlation-based selector; output dimension equals inner encoder.
     * Disabled dimensions are zeroed then L2-renormalized so clustering remains consistent.
     */
    class SelectiveStateEncoder : public IStateEncoder {
    public:
        SelectiveStateEncoder(const IStateEncoderPtr &inner,
                             FeatureSelector *varianceSelector = nullptr,
                             CorrelationBasedSelector *correlationSelector = nullptr)
                : _inner(inner), _varianceSelector(varianceSelector), _correlationSelector(correlationSelector) {}

        int getOutputDim() const override { return _inner ? _inner->getOutputDim() : 0; }

        std::vector<double> encode(const StatePtr &state) override {
            if (!_inner) return {};
            std::vector<double> x = _inner->encode(state);
            if (x.empty()) return x;
            const size_t dim = x.size();

            if (_varianceSelector) _varianceSelector->updateStatistics(x);
            if (_correlationSelector) {
                _correlationSelector->addSample(x);
                _correlationSelector->computeCorrelation();
            }

            std::vector<bool> mask(dim, true);
            if (_varianceSelector && _varianceSelector->getMask().size() == dim) {
                const auto &vm = _varianceSelector->getMask();
                for (size_t i = 0; i < dim; ++i) mask[i] = mask[i] && vm[i];
            }
            if (_correlationSelector && _correlationSelector->getMask().size() == dim) {
                const auto &cm = _correlationSelector->getMask();
                for (size_t i = 0; i < dim; ++i) mask[i] = mask[i] && cm[i];
            }

            for (size_t i = 0; i < dim; ++i)
                if (!mask[i]) x[i] = 0.0;

            double normSq = 0.0;
            for (double v : x) normSq += v * v;
            if (normSq > 0.0) {
                double invNorm = 1.0 / std::sqrt(normSq);
                for (double &v : x) v *= invNorm;
            }
            return x;
        }

    private:
        IStateEncoderPtr _inner;
        FeatureSelector *_varianceSelector = nullptr;
        CorrelationBasedSelector *_correlationSelector = nullptr;
    };

    /**
     * Strategy 5.2: hybrid normalization wrapper — Z-score then L2.
     * Maintains running mean and variance (Welford) over inner encoder output; applies Z-score when
     * sample_count >= 2, then L2-normalizes. Output dimension equals inner encoder.
     */
    class ZScoreL2StateEncoder : public IStateEncoder {
    public:
        static constexpr size_t kMinSamplesForZScore = 2;
        static constexpr double kStdEpsilon = 1e-9;

        explicit ZScoreL2StateEncoder(const IStateEncoderPtr &inner) : _inner(inner) {}

        int getOutputDim() const override { return _inner ? _inner->getOutputDim() : 0; }

        std::vector<double> encode(const StatePtr &state) override {
            if (!_inner) return {};
            std::vector<double> x = _inner->encode(state);
            if (x.empty()) return x;
            const size_t dim = x.size();

            if (_mean.empty()) {
                _mean.resize(dim, 0.0);
                _m2.resize(dim, 0.0);
            }
            if (dim != _mean.size()) return l2Normalize(x);

            ++_sampleCount;
            const double n = static_cast<double>(_sampleCount);
            for (size_t i = 0; i < dim; ++i) {
                const double delta = x[i] - _mean[i];
                _mean[i] += delta / n;
                const double delta2 = x[i] - _mean[i];
                _m2[i] += delta * delta2;
            }

            if (_sampleCount >= kMinSamplesForZScore) {
                for (size_t i = 0; i < dim; ++i) {
                    double var = _m2[i] / static_cast<double>(_sampleCount - 1);
                    double stdVal = std::sqrt(var) + kStdEpsilon;
                    x[i] = (x[i] - _mean[i]) / stdVal;
                }
            }
            return l2Normalize(x);
        }

    private:
        IStateEncoderPtr _inner;
        std::vector<double> _mean;
        std::vector<double> _m2;
        size_t _sampleCount = 0;

        static std::vector<double> l2Normalize(std::vector<double> x) {
            double normSq = 0.0;
            for (double v : x) normSq += v * v;
            if (normSq > 0.0) {
                double invNorm = 1.0 / std::sqrt(normSq);
                for (double &v : x) v *= invNorm;
            }
            return x;
        }
    };

    /**
     * DNN state encoder: small MLP (16 -> 16 -> 8) in-process, no TFLite dependency.
     * Input = HandcraftedStateEncoder 16-dim; output = 8-dim embedding for clustering.
     * To use a real trained model (e.g. TFLite): replace forwardMlp() with your inference call.
     */
    class DnnStateEncoder : public IStateEncoder {
    public:
        static constexpr int kInputDim = 16;  // matches HandcraftedStateEncoder::kHandcraftedDim
        static constexpr int kHiddenDim = 16;
        static constexpr int kOutputDim = 8;

        DnnStateEncoder() { initWeights(42); }

        explicit DnnStateEncoder(unsigned seed) { initWeights(seed); }

        int getOutputDim() const override { return kOutputDim; }

        std::vector<double> encode(const StatePtr &state) override {
            std::vector<double> x = HandcraftedStateEncoder().encode(state);
            if (x.size() != static_cast<size_t>(kInputDim)) return x;
            return forwardMlp(x);
        }

    private:
        std::vector<double> _W1, _b1, _W2, _b2;

        void initWeights(unsigned seed) {
            std::mt19937 rng(seed);
            // Glorot/Xavier-style init for better balanced embeddings:
            // weights ~ U(-sqrt(6/(fan_in+fan_out)), sqrt(6/(fan_in+fan_out))), biases = 0.
            const double limit1 = std::sqrt(6.0 / static_cast<double>(kInputDim + kHiddenDim));
            const double limit2 = std::sqrt(6.0 / static_cast<double>(kHiddenDim + kOutputDim));
            std::uniform_real_distribution<double> u1(-limit1, limit1);
            std::uniform_real_distribution<double> u2(-limit2, limit2);

            _W1.resize(static_cast<size_t>(kInputDim * kHiddenDim));
            _b1.resize(static_cast<size_t>(kHiddenDim));
            _W2.resize(static_cast<size_t>(kHiddenDim * kOutputDim));
            _b2.resize(static_cast<size_t>(kOutputDim));

            for (double &v : _W1) v = u1(rng);
            for (double &v : _W2) v = u2(rng);
            std::fill(_b1.begin(), _b1.end(), 0.0);
            std::fill(_b2.begin(), _b2.end(), 0.0);
        }

        static double relu(double x) { return x > 0 ? x : 0; }

        std::vector<double> forwardMlp(const std::vector<double> &x) const {
            std::vector<double> h(static_cast<size_t>(kHiddenDim));
            for (int i = 0; i < kHiddenDim; ++i) {
                double s = _b1[static_cast<size_t>(i)];
                for (int j = 0; j < kInputDim; ++j)
                    s += _W1[static_cast<size_t>(i * kInputDim + j)] * x[static_cast<size_t>(j)];
                h[static_cast<size_t>(i)] = relu(s);
            }
            std::vector<double> out(static_cast<size_t>(kOutputDim));
            for (int i = 0; i < kOutputDim; ++i) {
                double s = _b2[static_cast<size_t>(i)];
                for (int j = 0; j < kHiddenDim; ++j)
                    s += _W2[static_cast<size_t>(i * kHiddenDim + j)] * h[static_cast<size_t>(j)];
                out[static_cast<size_t>(i)] = std::tanh(s);
            }
            return out;
        }
    };

}  // namespace fastbotx

#endif  // FASTBOTX_STATE_ENCODER_H
