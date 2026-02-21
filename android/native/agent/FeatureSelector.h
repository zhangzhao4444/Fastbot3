/*
 * This code is licensed under the Fastbot license. You may obtain a copy of this license in the LICENSE.txt file in the root directory of this source tree.
 */
/**
 * Runtime dynamic feature selection (FEATURE_ENGINEERING_OPTIMIZATION strategy 4).
 * Variance-based and correlation-based selectors for optional use with HandcraftedStateEncoder.
 */
#ifndef FASTBOTX_FEATURE_SELECTOR_H
#define FASTBOTX_FEATURE_SELECTOR_H

#include <vector>
#include <deque>
#include <cmath>
#include <algorithm>
#include <cstddef>

namespace fastbotx {

    /**
     * Variance-based feature selection: online Welford variance, mask out low-variance dimensions.
     * Output dimension is unchanged; disabled dimensions are zeroed so L2 renormalization can be applied.
     */
    class FeatureSelector {
    public:
        static constexpr double MIN_VARIANCE_THRESHOLD = 0.01;

        void updateStatistics(const std::vector<double> &features) {
            if (features.empty()) return;
            const size_t dim = features.size();
            if (_mean.empty()) {
                _mean.resize(dim, 0.0);
                _m2.resize(dim, 0.0);
                _mask.resize(dim, true);
            }
            if (dim != _mean.size()) return;
            ++_sampleCount;
            const double n = static_cast<double>(_sampleCount);
            for (size_t i = 0; i < dim; ++i) {
                const double delta = features[i] - _mean[i];
                _mean[i] += delta / n;
                const double delta2 = features[i] - _mean[i];
                _m2[i] += delta * delta2;
            }
            updateMaskFromVariance();
        }

        /** Returns variance for dimension i (sample variance). */
        double getVariance(size_t i) const {
            if (i >= _m2.size() || _sampleCount < 2) return 0.0;
            return _m2[i] / static_cast<double>(_sampleCount - 1);
        }

        const std::vector<bool> &getMask() const { return _mask; }

        /** Same-size output: disabled dimensions set to 0 (caller may L2-renormalize). */
        std::vector<double> applyMask(const std::vector<double> &features) const {
            std::vector<double> out = features;
            if (out.size() != _mask.size()) return out;
            for (size_t i = 0; i < out.size(); ++i)
                if (!_mask[i]) out[i] = 0.0;
            return out;
        }

        /** Subset of features (only enabled); dimension may change. */
        std::vector<double> selectFeatures(const std::vector<double> &features) const {
            std::vector<double> selected;
            selected.reserve(features.size());
            for (size_t i = 0; i < features.size(); ++i)
                if (i < _mask.size() && _mask[i])
                    selected.push_back(features[i]);
            return selected;
        }

        size_t getSampleCount() const { return _sampleCount; }

    private:
        std::vector<double> _mean;
        std::vector<double> _m2;
        std::vector<bool> _mask;
        size_t _sampleCount = 0;

        void updateMaskFromVariance() {
            if (_sampleCount < 2) return;
            for (size_t i = 0; i < _mask.size(); ++i) {
                double var = _m2[i] / static_cast<double>(_sampleCount - 1);
                _mask[i] = (var >= MIN_VARIANCE_THRESHOLD);
            }
        }
    };

    /**
     * Correlation-based feature selection: keep a sliding window of samples,
     * compute Pearson correlation matrix, drop one of each high-correlation pair (keep higher variance).
     */
    class CorrelationBasedSelector {
    public:
        static constexpr double CORRELATION_THRESHOLD = 0.7;
        static constexpr size_t DEFAULT_MAX_SAMPLES = 200;
        static constexpr size_t MIN_SAMPLES_FOR_CORRELATION = 30;

        explicit CorrelationBasedSelector(size_t maxSamples = DEFAULT_MAX_SAMPLES)
                : _maxSamples(maxSamples) {}

        void addSample(const std::vector<double> &features) {
            if (features.empty()) return;
            if (_samples.size() >= _maxSamples) _samples.pop_front();
            _samples.push_back(features);
        }

        /** Call after addSample(); recomputes correlation and mask when enough samples. */
        void computeCorrelation() {
            const size_t n = _samples.size();
            if (n < MIN_SAMPLES_FOR_CORRELATION) return;
            const size_t dim = _samples.front().size();
            for (const auto &row : _samples)
                if (row.size() != dim) return;

            _featureCount = dim;
            _means.assign(dim, 0.0);
            _variances.assign(dim, 0.0);
            _correlationMatrix.assign(dim, std::vector<double>(dim, 0.0));

            const double N = static_cast<double>(n);
            for (size_t i = 0; i < dim; ++i) {
                double sum = 0.0;
                for (const auto &s : _samples) sum += s[i];
                _means[i] = sum / N;
            }
            for (size_t i = 0; i < dim; ++i) {
                double m2 = 0.0;
                for (const auto &s : _samples) {
                    double d = s[i] - _means[i];
                    m2 += d * d;
                }
                _variances[i] = (n > 1) ? (m2 / (N - 1.0)) : 0.0;
            }
            for (size_t i = 0; i < dim; ++i) {
                for (size_t j = i; j < dim; ++j) {
                    double cov = 0.0;
                    for (const auto &s : _samples)
                        cov += (s[i] - _means[i]) * (s[j] - _means[j]);
                    cov = (n > 1) ? (cov / (N - 1.0)) : 0.0;
                    double denom = (std::sqrt(_variances[i] * _variances[j]) + 1e-12);
                    double r = (denom > 1e-12) ? (cov / denom) : 0.0;
                    if (r > 1.0) r = 1.0;
                    if (r < -1.0) r = -1.0;
                    _correlationMatrix[i][j] = r;
                    _correlationMatrix[j][i] = r;
                }
            }
            buildMask();
        }

        const std::vector<bool> &getMask() const { return _mask; }

        std::vector<double> applyMask(const std::vector<double> &features) const {
            std::vector<double> out = features;
            if (out.size() != _mask.size()) return out;
            for (size_t i = 0; i < out.size(); ++i)
                if (!_mask[i]) out[i] = 0.0;
            return out;
        }

        size_t getSampleCount() const { return _samples.size(); }

    private:
        size_t _maxSamples;
        size_t _featureCount = 0;
        std::deque<std::vector<double>> _samples;
        std::vector<double> _means;
        std::vector<double> _variances;
        std::vector<std::vector<double>> _correlationMatrix;
        std::vector<bool> _mask;

        void buildMask() {
            _mask.assign(_featureCount, true);
            for (size_t i = 0; i < _featureCount; ++i) {
                for (size_t j = i + 1; j < _featureCount; ++j) {
                    if (std::abs(_correlationMatrix[i][j]) > CORRELATION_THRESHOLD) {
                        if (_variances[i] < _variances[j])
                            _mask[i] = false;
                        else
                            _mask[j] = false;
                    }
                }
            }
        }
    };

}  // namespace fastbotx

#endif  // FASTBOTX_FEATURE_SELECTOR_H
