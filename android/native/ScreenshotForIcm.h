/*
 * This code is licensed under the Fastbot license.
 */
/**
 * Screenshot / image features for ICM/UISCGD encoder.
 * - On-demand path (preferred): C++ calls getImageFeaturesForIcmFromJava() during encode();
 *   Java computes 256-dim features (UISCGD/Deformable DETR d_model aligned), no image over JNI.
 * - Fallback: setScreenshotForIcm/getScreenshotForIcm + imageFeaturesFromBytes in C++ (e.g. tests).
 */
#ifndef FASTBOTX_SCREENSHOT_FOR_ICM_H
#define FASTBOTX_SCREENSHOT_FOR_ICM_H

#include <cstddef>
#include <vector>

namespace fastbotx {

/** Set screenshot bytes (e.g. from test). Called from JNI or test code. */
void setScreenshotForIcm(const unsigned char *data, size_t len);

/** Get current thread's screenshot bytes (fallback when Java provider not used). */
const std::vector<unsigned char> &getScreenshotForIcm();

/**
 * Request UISCGD image features (256-dim) from Java. Used by UISCGDStateEncoder::encode().
 * @param out  On success, set to 256 doubles; cleared on failure.
 * @return true if Java returned 256-dim vector; false otherwise.
 */
bool getImageFeaturesForIcmFromJava(std::vector<double> *out);

}  // namespace fastbotx

#endif  // FASTBOTX_SCREENSHOT_FOR_ICM_H
