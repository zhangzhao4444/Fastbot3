#ifndef FASTBOTX_MODEL_STORAGE_CONSTANTS_H_
#define FASTBOTX_MODEL_STORAGE_CONSTANTS_H_

#include <cstddef>

// Shared constants for reuse-model storage on disk (DoubleSarsaAgent, SarsaAgent).
namespace ModelStorageConstants {
#ifdef __ANDROID__
inline constexpr const char *StoragePrefix = "/sdcard/fastbot_";
#else
inline constexpr const char *StoragePrefix = "";
#endif
inline constexpr const char *ModelFileExtension = ".fbm";
inline constexpr const char *TempModelFileExtension = ".tmp.fbm";
/// Max length for activity name when serializing (security: prevent unbounded string).
inline constexpr std::size_t MaxActivityNameLength = 4096;
}  // namespace ModelStorageConstants

#endif  // FASTBOTX_MODEL_STORAGE_CONSTANTS_H_

