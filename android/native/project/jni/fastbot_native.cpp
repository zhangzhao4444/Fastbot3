/*
 * This code is licensed under the Fastbot license. You may obtain a copy of this license in the LICENSE.txt file in the root directory of this source tree.
 */
/**
 * @authors Jianqiang Guo, Yuhui Su
 */
#include "fastbot_native.h"
#include "Model.h"
// #include "ModelReusableAgent.h"  // Temporarily disabled for DoubleSarsa testing
#include "DoubleSarsaAgent.h"
#include "utils.hpp"
#include <mutex>

#ifdef __cplusplus
extern "C" {
#endif

static fastbotx::ModelPtr _fastbot_model = nullptr;
static std::mutex _fastbot_model_mutex;

namespace {
    class JStringUTFChars {
    public:
        JStringUTFChars(JNIEnv *env, jstring str)
                : _env(env), _str(str), _chars(nullptr) {
            if (_env && _str) {
                _chars = _env->GetStringUTFChars(_str, nullptr);
            }
        }

        ~JStringUTFChars() {
            if (_env && _str && _chars) {
                _env->ReleaseStringUTFChars(_str, _chars);
            }
        }

        JStringUTFChars(const JStringUTFChars &) = delete;
        JStringUTFChars &operator=(const JStringUTFChars &) = delete;

        const char *c_str() const { return _chars ? _chars : ""; }

    private:
        JNIEnv *_env;
        jstring _str;
        const char *_chars;
    };
} // namespace

//getAction
jstring JNICALL Java_com_bytedance_fastbot_AiClient_b0bhkadf(JNIEnv *env, jobject, jstring activity,
                                                             jstring xmlDescOfGuiTree) {
    JStringUTFChars xmlDescriptionCString(env, xmlDescOfGuiTree);
    JStringUTFChars activityCString(env, activity);

    std::string operationString;
    {
        std::lock_guard<std::mutex> lock(_fastbot_model_mutex);
        if (_fastbot_model == nullptr) {
            _fastbot_model = fastbotx::Model::create();
        }
        operationString = _fastbot_model->getOperate(xmlDescriptionCString.c_str(), activityCString.c_str());
    }
    LOGD("do action opt is : %s", operationString.c_str());
    return env->NewStringUTF(operationString.c_str());
}

// for single device, just addAgent as empty device //InitAgent
void JNICALL Java_com_bytedance_fastbot_AiClient_fgdsaf5d(JNIEnv *env, jobject, jint agentType,
                                                          jstring packageName, jint deviceType) {
    JStringUTFChars packageNameCString(env, packageName);

    std::lock_guard<std::mutex> lock(_fastbot_model_mutex);
    if (_fastbot_model == nullptr) {
        _fastbot_model = fastbotx::Model::create();
    }
    auto algorithmType = static_cast<fastbotx::AlgorithmType>(agentType);
    auto agentPointer = _fastbot_model->addAgent("", algorithmType,
                                                 static_cast<fastbotx::DeviceType>(deviceType));

    _fastbot_model->setPackageName(std::string(packageNameCString.c_str()));

    BLOG("init agent with type %d, %s,  %d", agentType, packageNameCString.c_str(), deviceType);
    // Temporarily: Always use DoubleSarsaAgent for testing
    // ModelReusableAgent has been disabled
    auto doubleSarsaAgentPtr = std::dynamic_pointer_cast<fastbotx::DoubleSarsaAgent>(agentPointer);
    if (doubleSarsaAgentPtr) {
        doubleSarsaAgentPtr->loadReuseModel(std::string(packageNameCString.c_str()));
    } else {
        BLOGE("Double SARSA: Failed to cast agent to DoubleSarsaAgent");
    }
}

// load ResMapping
void JNICALL
Java_com_bytedance_fastbot_AiClient_jdasdbil(JNIEnv *env, jobject, jstring resMappingFilepath) {
    JStringUTFChars resourceMappingPath(env, resMappingFilepath);

    std::lock_guard<std::mutex> lock(_fastbot_model_mutex);
    if (_fastbot_model == nullptr) {
        _fastbot_model = fastbotx::Model::create();
    }
    auto preference = _fastbot_model->getPreference();
    if (preference) {
        preference->loadMixResMapping(std::string(resourceMappingPath.c_str()));
    }
}

// to check if a point is in black widget area
jboolean JNICALL
Java_com_bytedance_fastbot_AiClient_nkksdhdk(JNIEnv *env, jobject, jstring activity, jfloat pointX,
                                             jfloat pointY) {
    bool isShield = false;
    JStringUTFChars activityStr(env, activity);

    std::lock_guard<std::mutex> lock(_fastbot_model_mutex);
    if (_fastbot_model == nullptr) {
        BLOGE("%s", "model null, check point failed!");
        return isShield;
    }
    auto preference = _fastbot_model->getPreference();
    if (preference) {
        isShield = preference->checkPointIsInBlackRects(std::string(activityStr.c_str()),
                                                        static_cast<int>(pointX),
                                                        static_cast<int>(pointY));
    }
    return isShield;
}

jstring JNICALL Java_com_bytedance_fastbot_AiClient_getNativeVersion(JNIEnv *env, jclass clazz) {
    return env->NewStringUTF(FASTBOT_VERSION);
}

#ifdef __cplusplus
}
#endif