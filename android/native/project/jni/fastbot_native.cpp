/*
 * This code is licensed under the Fastbot license. You may obtain a copy of this license in the LICENSE.txt file in the root directory of this source tree.
 */
/**
 * @authors Jianqiang Guo, Yuhui Su
 */
#include "fastbot_native.h"
#include "Model.h"
#include "Element.h"
#include "DeviceOperateWrapper.h"
#include "DoubleSarsaAgent.h"
#include "utils.hpp"
#include "../llm/LlmJavaHttp.h"
#include "../thirdpart/json/json.hpp"
#include <random>
#include <chrono>
#include <cstring>
#include <jni.h>

#ifdef __cplusplus
extern "C" {
#endif

static fastbotx::ModelPtr _fastbot_model = nullptr;

// LLM HTTP via Java (when libcurl not available): image stays in Java, native passes prompt or payload
static JavaVM *g_jvm = nullptr;
static jobject g_llmHttpRunner = nullptr;
static jmethodID g_llmHttpDoPostFromPrompt = nullptr;
static jmethodID g_llmHttpDoPostFromPayload = nullptr;

// Fuzzer: RNG and one fuzz action JSON (performance §3.3)
static std::mt19937 &fuzzRng() {
    static std::mt19937 rng(static_cast<unsigned>(std::chrono::steady_clock::now().time_since_epoch().count()));
    return rng;
}

static std::string getNextFuzzActionJson(int displayWidth, int displayHeight, bool simplify) {
    auto &rng = fuzzRng();
    std::uniform_real_distribution<float> distW(0.f, static_cast<float>(displayWidth > 0 ? displayWidth : 1080));
    std::uniform_real_distribution<float> distH(0.f, static_cast<float>(displayHeight > 0 ? displayHeight : 1920));
    std::uniform_int_distribution<int> distRot(0, 3);
    const int rotations[] = {0, 90, 180, 270};
    nlohmann::json j;
    int typeChoice;
    if (simplify) {
        typeChoice = std::uniform_int_distribution<int>(0, 4)(rng);  // rotation, app_switch, drag, pinch, click
    } else {
        typeChoice = std::uniform_int_distribution<int>(0, 4)(rng);
    }
    switch (typeChoice) {
        case 0:  // rotation
            j["type"] = "rotation";
            j["degree"] = rotations[distRot(rng)];
            j["persist"] = false;
            break;
        case 1:  // app_switch
            j["type"] = "app_switch";
            j["home"] = (std::uniform_int_distribution<int>(0, 1)(rng) != 0);
            break;
        case 2:  // drag: 2-10 points, flat values [x1,y1,x2,y2,...]
            j["type"] = "drag";
            {
                int n = 2 + std::uniform_int_distribution<int>(0, 8)(rng);
                nlohmann::json arr = nlohmann::json::array();
                for (int i = 0; i < n; i++) {
                    arr.push_back(distW(rng));
                    arr.push_back(distH(rng));
                }
                j["values"] = arr;
            }
            break;
        case 3:  // pinch: 4+ points (pairs), flat values
            j["type"] = "pinch";
            {
                int n = 4 + std::uniform_int_distribution<int>(0, 6)(rng) * 2;
                nlohmann::json arr = nlohmann::json::array();
                for (int i = 0; i < n; i++) {
                    arr.push_back(distW(rng));
                    arr.push_back(distH(rng));
                }
                j["values"] = arr;
            }
            break;
        default:  // click
            j["type"] = "click";
            j["x"] = distW(rng);
            j["y"] = distH(rng);
            j["waitTime"] = static_cast<long>(std::uniform_int_distribution<int>(0, 1000)(rng));
            break;
    }
    return j.dump();
}

// getAction (XML string from Java - involves GetStringUTFChars copy)
jstring JNICALL Java_com_bytedance_fastbot_AiClient_getOperateJsonNative(JNIEnv *env, jobject, jstring activity,
                                                                         jstring xmlDescOfGuiTree) {
    if (nullptr == _fastbot_model) {
        _fastbot_model = fastbotx::Model::create();
    }
    const char *xmlDescriptionCString = env->GetStringUTFChars(xmlDescOfGuiTree, nullptr);
    const char *activityCString = env->GetStringUTFChars(activity, nullptr);
    std::string xmlString = std::string(xmlDescriptionCString);
    std::string activityString = std::string(activityCString);
    std::string operationString = _fastbot_model->getOperate(xmlString, activityString);
    LOGD("do action opt is : %s", operationString.c_str());
    env->ReleaseStringUTFChars(xmlDescOfGuiTree, xmlDescriptionCString);
    env->ReleaseStringUTFChars(activity, activityCString);
    return env->NewStringUTF(operationString.c_str());
}

// Helper: parse tree from buffer (binary "FB\0\1" or XML), return ElementPtr (opt1).
// byteLength must be the actual number of bytes written (Java buffer limit), not capacity,
// to avoid incomplete UTF-8 when building std::string (fixes type_error.316).
static fastbotx::ElementPtr parseTreeFromBuffer(const char *addr, size_t byteLength) {
    if (byteLength >= 4 && addr[0] == 'F' && addr[1] == 'B' && addr[2] == 0 && addr[3] == 1) {
        return fastbotx::Element::createFromBinary(addr, byteLength);
    }
    std::string xmlString(addr, byteLength);
    return fastbotx::Element::createFromXml(xmlString);
}

// getAction from Direct ByteBuffer (performance: avoid GetStringUTFChars copy, PERF §3.1; opt1 binary path).
// byteLength must be the actual bytes in the buffer (Java limit/remaining), not capacity.
// Image for LLM is obtained in Java on demand when native triggers HTTP (no screenshot param).
jstring JNICALL Java_com_bytedance_fastbot_AiClient_getActionFromBufferNative(JNIEnv *env, jobject,
                                                                              jstring activity,
                                                                              jobject xmlBuffer,
                                                                              jint byteLength) {
    if (nullptr == _fastbot_model || xmlBuffer == nullptr) {
        return env->NewStringUTF("");
    }
    void *addr = env->GetDirectBufferAddress(xmlBuffer);
    jlong capacity = env->GetDirectBufferCapacity(xmlBuffer);
    if (addr == nullptr || capacity <= 0) {
        return env->NewStringUTF("");
    }
    size_t len = static_cast<size_t>(byteLength > 0 ? byteLength : 0);
    if (len > static_cast<size_t>(capacity)) {
        len = static_cast<size_t>(capacity);
    }
    if (len == 0) {
        return env->NewStringUTF("");
    }
    const char *activityCString = env->GetStringUTFChars(activity, nullptr);
    std::string activityString = std::string(activityCString);
    fastbotx::ElementPtr elem = parseTreeFromBuffer(static_cast<const char *>(addr), len);
    std::string operationString;
    if (elem) {
        fastbotx::OperatePtr opt = _fastbot_model->getOperateOpt(elem, activityString, "");
        operationString = opt ? opt->toString() : "";
    }
    env->ReleaseStringUTFChars(activity, activityCString);
    return env->NewStringUTF(operationString.c_str());
}

// getAction structured: return OperateResult to avoid JSON parse (SECURITY_AND_OPTIMIZATION §7 opt4).
// byteLength must be the actual bytes in the buffer (Java limit/remaining), not capacity.
// Image for LLM is obtained in Java on demand when native triggers HTTP (no screenshot param).
jobject JNICALL Java_com_bytedance_fastbot_AiClient_getActionFromBufferNativeStructured(JNIEnv *env, jobject,
                                                                                        jstring activity,
                                                                                        jobject xmlBuffer,
                                                                                        jint byteLength) {
    if (nullptr == _fastbot_model || xmlBuffer == nullptr) return nullptr;
    void *addr = env->GetDirectBufferAddress(xmlBuffer);
    jlong capacity = env->GetDirectBufferCapacity(xmlBuffer);
    if (addr == nullptr || capacity <= 0) return nullptr;
    size_t len = static_cast<size_t>(byteLength > 0 ? byteLength : 0);
    if (len > static_cast<size_t>(capacity)) {
        len = static_cast<size_t>(capacity);
    }
    if (len == 0) return nullptr;
    const char *activityCString = env->GetStringUTFChars(activity, nullptr);
    std::string activityString = std::string(activityCString);
    env->ReleaseStringUTFChars(activity, activityCString);

    fastbotx::ElementPtr elem = parseTreeFromBuffer(static_cast<const char *>(addr), len);
    if (!elem) return nullptr;
    fastbotx::OperatePtr opt = _fastbot_model->getOperateOpt(elem, activityString, "");
    if (!opt || opt == fastbotx::DeviceOperateWrapper::OperateNop) return nullptr;

    jclass cls = env->FindClass("com/android/commands/monkey/fastbot/client/OperateResult");
    if (!cls) return nullptr;
    jobject result = env->AllocObject(cls);
    if (!result) return nullptr;

    env->SetIntField(result, env->GetFieldID(cls, "actOrdinal", "I"), static_cast<jint>(opt->act));
    jshortArray posArr = env->NewShortArray(4);
    if (posArr && opt->pos.left >= -32768 && opt->pos.left <= 32767) {
        jshort posData[4] = { static_cast<jshort>(opt->pos.left), static_cast<jshort>(opt->pos.top),
                             static_cast<jshort>(opt->pos.right), static_cast<jshort>(opt->pos.bottom) };
        env->SetShortArrayRegion(posArr, 0, 4, posData);
    }
    env->SetObjectField(result, env->GetFieldID(cls, "pos", "[S"), posArr);
    env->SetIntField(result, env->GetFieldID(cls, "throttle", "I"), static_cast<jint>(opt->throttle));
    env->SetLongField(result, env->GetFieldID(cls, "waitTime", "J"), static_cast<jlong>(opt->waitTime));
    env->SetObjectField(result, env->GetFieldID(cls, "text", "Ljava/lang/String;"),
                        opt->getText().empty() ? nullptr : env->NewStringUTF(opt->getText().c_str()));
    env->SetBooleanField(result, env->GetFieldID(cls, "clear", "Z"), opt->clear ? JNI_TRUE : JNI_FALSE);
    env->SetBooleanField(result, env->GetFieldID(cls, "rawInput", "Z"), opt->getRawInput() ? JNI_TRUE : JNI_FALSE);
    env->SetBooleanField(result, env->GetFieldID(cls, "allowFuzzing", "Z"), opt->allowFuzzing ? JNI_TRUE : JNI_FALSE);
    env->SetBooleanField(result, env->GetFieldID(cls, "editable", "Z"), opt->editable ? JNI_TRUE : JNI_FALSE);
    env->SetObjectField(result, env->GetFieldID(cls, "sid", "Ljava/lang/String;"),
                        opt->sid.empty() ? nullptr : env->NewStringUTF(opt->sid.c_str()));
    env->SetObjectField(result, env->GetFieldID(cls, "aid", "Ljava/lang/String;"),
                        opt->aid.empty() ? nullptr : env->NewStringUTF(opt->aid.c_str()));
    env->SetObjectField(result, env->GetFieldID(cls, "jAction", "Ljava/lang/String;"),
                        opt->getJAction().empty() ? nullptr : env->NewStringUTF(opt->getJAction().c_str()));
    env->SetObjectField(result, env->GetFieldID(cls, "widget", "Ljava/lang/String;"),
                        opt->widget.empty() ? nullptr : env->NewStringUTF(opt->widget.c_str()));

    env->DeleteLocalRef(cls);
    if (posArr) env->DeleteLocalRef(posArr);
    return result;
}

// InitAgent: for single device, just addAgent as empty device
void JNICALL Java_com_bytedance_fastbot_AiClient_initAgentNative(JNIEnv *env, jobject, jint agentType,
                                                                 jstring packageName, jint deviceType) {
    if (nullptr == _fastbot_model) {
        _fastbot_model = fastbotx::Model::create();
    }
    auto algorithmType = (fastbotx::AlgorithmType) agentType;
    auto agentPointer = _fastbot_model->addAgent("", algorithmType,
                                                 (fastbotx::DeviceType) deviceType);
    const char *packageNameCString = "";
    if (env)
        packageNameCString = env->GetStringUTFChars(packageName, nullptr);
    _fastbot_model->setPackageName(std::string(packageNameCString));

    BLOG("init agent with type %d, %s,  %d", agentType, packageNameCString, deviceType);
    auto doubleSarsaAgentPtr = std::dynamic_pointer_cast<fastbotx::DoubleSarsaAgent>(agentPointer);
    if (doubleSarsaAgentPtr) {
        doubleSarsaAgentPtr->loadReuseModel(std::string(packageNameCString));
    } else {
        BLOGE("Double SARSA: Failed to cast agent to DoubleSarsaAgent");
    }
    if (env)
        env->ReleaseStringUTFChars(packageName, packageNameCString);
}

// loadResMapping
void JNICALL
Java_com_bytedance_fastbot_AiClient_loadResMappingNative(JNIEnv *env, jobject, jstring resMappingFilepath) {
    if (nullptr == _fastbot_model) {
        _fastbot_model = fastbotx::Model::create();
    }
    const char *resourceMappingPath = env->GetStringUTFChars(resMappingFilepath, nullptr);
    auto preference = _fastbot_model->getPreference();
    if (preference) {
        preference->loadMixResMapping(std::string(resourceMappingPath));
    }
    env->ReleaseStringUTFChars(resMappingFilepath, resourceMappingPath);
}

// single-point shield check: whether (pointX, pointY) is in black widget area
jboolean JNICALL
Java_com_bytedance_fastbot_AiClient_checkPointInShieldNative(JNIEnv *env, jobject, jstring activity, jfloat pointX,
                                                             jfloat pointY) {
    bool isShield = false;
    if (nullptr == _fastbot_model) {
        BLOGE("%s", "model null, check point failed!");
        return isShield;
    }
    const char *activityStr = env->GetStringUTFChars(activity, nullptr);
    auto preference = _fastbot_model->getPreference();
    if (preference) {
        isShield = preference->checkPointIsInBlackRects(std::string(activityStr),
                                                        static_cast<int>(pointX),
                                                        static_cast<int>(pointY));
    }
    env->ReleaseStringUTFChars(activity, activityStr);
    return isShield;
}

// batch check: multiple points in one JNI call to reduce round-trips (performance optimization)
jbooleanArray JNICALL
Java_com_bytedance_fastbot_AiClient_checkPointsInShieldNative(JNIEnv *env, jobject, jstring activity,
                                                              jfloatArray xCoords, jfloatArray yCoords) {
    jbooleanArray result = nullptr;
    if (nullptr == _fastbot_model || xCoords == nullptr || yCoords == nullptr) {
        return result;
    }
    jsize len = env->GetArrayLength(xCoords);
    if (len != env->GetArrayLength(yCoords) || len <= 0) {
        return result;
    }
    result = env->NewBooleanArray(len);
    if (result == nullptr) {
        return nullptr;
    }
    const char *activityStr = env->GetStringUTFChars(activity, nullptr);
    jfloat *xElems = env->GetFloatArrayElements(xCoords, nullptr);
    jfloat *yElems = env->GetFloatArrayElements(yCoords, nullptr);
    if (activityStr == nullptr || xElems == nullptr || yElems == nullptr) {
        if (activityStr) env->ReleaseStringUTFChars(activity, activityStr);
        if (xElems) env->ReleaseFloatArrayElements(xCoords, xElems, JNI_ABORT);
        if (yElems) env->ReleaseFloatArrayElements(yCoords, yElems, JNI_ABORT);
        return result;
    }
    auto preference = _fastbot_model->getPreference();
    std::string activityCpp(activityStr);
    jboolean *out = new jboolean[len];
    for (jsize i = 0; i < len; i++) {
        bool isShield = false;
        if (preference) {
            isShield = preference->checkPointIsInBlackRects(activityCpp,
                                                            static_cast<int>(xElems[i]),
                                                            static_cast<int>(yElems[i]));
        }
        out[i] = isShield ? JNI_TRUE : JNI_FALSE;
    }
    env->ReleaseStringUTFChars(activity, activityStr);
    env->ReleaseFloatArrayElements(xCoords, xElems, JNI_ABORT);
    env->ReleaseFloatArrayElements(yCoords, yElems, JNI_ABORT);
    env->SetBooleanArrayRegion(result, 0, len, out);
    delete[] out;
    return result;
}

jstring JNICALL Java_com_bytedance_fastbot_AiClient_getNativeVersion(JNIEnv *env, jclass clazz) {
    return env->NewStringUTF(FASTBOT_VERSION);
}

// Coverage tracking: report activity (performance optimization §3.4)
void JNICALL Java_com_bytedance_fastbot_AiClient_reportActivityNative(JNIEnv *env, jobject, jstring activity) {
    if (nullptr == _fastbot_model || activity == nullptr) return;
    const char *activityStr = env->GetStringUTFChars(activity, nullptr);
    if (activityStr) {
        _fastbot_model->reportActivity(std::string(activityStr));
        env->ReleaseStringUTFChars(activity, activityStr);
    }
}

// Coverage tracking: get coverage JSON (performance optimization §3.4)
jstring JNICALL Java_com_bytedance_fastbot_AiClient_getCoverageJsonNative(JNIEnv *env, jobject) {
    if (nullptr == _fastbot_model) return env->NewStringUTF("{}");
    std::string json = _fastbot_model->getCoverageJson();
    return env->NewStringUTF(json.c_str());
}

// Fuzzing: get next fuzz action JSON from C++ (performance §3.3)
jstring JNICALL Java_com_bytedance_fastbot_AiClient_getNextFuzzActionNative(JNIEnv *env, jobject,
                                                                            jint displayWidth,
                                                                            jint displayHeight,
                                                                            jboolean simplify) {
    std::string json = getNextFuzzActionJson(displayWidth, displayHeight, simplify == JNI_TRUE);
    return env->NewStringUTF(json.c_str());
}

// Register Java object for LLM HTTP POST when libcurl is not available (see LlmJavaHttp.h).
// thiz is the AiClient instance (receiver of nativeRegisterLlmHttpRunner()).
JNIEXPORT void JNICALL Java_com_bytedance_fastbot_AiClient_nativeRegisterLlmHttpRunner(JNIEnv *env, jobject thiz) {
    if (env->GetJavaVM(&g_jvm) != JNI_OK) return;
    if (g_llmHttpRunner != nullptr) {
        env->DeleteGlobalRef(g_llmHttpRunner);
        g_llmHttpRunner = nullptr;
    }
    if (thiz == nullptr) return;
    g_llmHttpRunner = env->NewGlobalRef(thiz);
    jclass c = env->GetObjectClass(thiz);
    g_llmHttpDoPostFromPrompt = env->GetMethodID(c, "doLlmHttpPostFromPrompt",
        "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;I)Ljava/lang/String;");
    g_llmHttpDoPostFromPayload = env->GetMethodID(c, "doLlmHttpPostFromPayload",
        "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;I)Ljava/lang/String;");
    if (g_llmHttpDoPostFromPrompt == nullptr || g_llmHttpDoPostFromPayload == nullptr) {
        BLOGE("LLM Java HTTP: GetMethodID failed; LLM HTTP will fail until runner is registered");
        env->DeleteGlobalRef(g_llmHttpRunner);
        g_llmHttpRunner = nullptr;
    }
}

#ifdef __cplusplus
}
#endif

namespace fastbotx {

bool llmHttpPostViaJavaWithPrompt(const char *url,
                                  const char *apiKey,
                                  const char *prompt,
                                  const char *model,
                                  int maxTokens,
                                  std::string *outResponse) {
    if (!g_jvm || !g_llmHttpRunner || !g_llmHttpDoPostFromPrompt || !outResponse) {
        BLOGE("LLM Java HTTP: runner not registered (g_jvm=%d g_runner=%d g_method=%d)", !!g_jvm, !!g_llmHttpRunner, !!g_llmHttpDoPostFromPrompt);
        return false;
    }
    JNIEnv *env = nullptr;
    jint attach = g_jvm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6);
    if (attach == JNI_EDETACHED)
        g_jvm->AttachCurrentThread(&env, nullptr);
    if (!env) {
        BLOGE("LLM Java HTTP: GetEnv/AttachCurrentThread failed");
        return false;
    }
    jstring jUrl = env->NewStringUTF(url ? url : "");
    jstring jKey = env->NewStringUTF(apiKey ? apiKey : "");
    jstring jPrompt = env->NewStringUTF(prompt ? prompt : "");
    jstring jModel = env->NewStringUTF(model ? model : "");
    jstring jResult = (jstring) env->CallObjectMethod(g_llmHttpRunner, g_llmHttpDoPostFromPrompt,
                                                       jUrl, jKey, jPrompt, jModel, static_cast<jint>(maxTokens));
    env->DeleteLocalRef(jUrl);
    env->DeleteLocalRef(jKey);
    env->DeleteLocalRef(jPrompt);
    env->DeleteLocalRef(jModel);
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        if (attach == JNI_EDETACHED) g_jvm->DetachCurrentThread();
        BLOGE("LLM Java HTTP: Java exception in doLlmHttpPostFromPrompt");
        return false;
    }
    if (!jResult) {
        if (attach == JNI_EDETACHED) g_jvm->DetachCurrentThread();
        BLOGE("LLM Java HTTP: Java returned null (HTTP non-2xx or network/API error)");
        return false;
    }
    const char *utf = env->GetStringUTFChars(jResult, nullptr);
    if (utf) {
        *outResponse = utf;
        env->ReleaseStringUTFChars(jResult, utf);
    }
    env->DeleteLocalRef(jResult);
    if (attach == JNI_EDETACHED) g_jvm->DetachCurrentThread();
    return true;
}

bool llmHttpPostViaJavaWithPayload(const char *url,
                                    const char *apiKey,
                                    const char *promptType,
                                    const char *payloadJson,
                                    const char *model,
                                    int maxTokens,
                                    std::string *outResponse) {
    if (!g_jvm || !g_llmHttpRunner || !g_llmHttpDoPostFromPayload || !outResponse) {
        BLOGE("LLM Java HTTP: runner not registered");
        return false;
    }
    JNIEnv *env = nullptr;
    jint attach = g_jvm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6);
    if (attach == JNI_EDETACHED)
        g_jvm->AttachCurrentThread(&env, nullptr);
    if (!env) {
        BLOGE("LLM Java HTTP: GetEnv/AttachCurrentThread failed");
        return false;
    }
    jstring jUrl = env->NewStringUTF(url ? url : "");
    jstring jKey = env->NewStringUTF(apiKey ? apiKey : "");
    jstring jPromptType = env->NewStringUTF(promptType ? promptType : "");
    jstring jPayload = env->NewStringUTF(payloadJson ? payloadJson : "{}");
    jstring jModel = env->NewStringUTF(model ? model : "");
    jstring jResult = (jstring) env->CallObjectMethod(g_llmHttpRunner, g_llmHttpDoPostFromPayload,
                                                     jUrl, jKey, jPromptType, jPayload, jModel, static_cast<jint>(maxTokens));
    env->DeleteLocalRef(jUrl);
    env->DeleteLocalRef(jKey);
    env->DeleteLocalRef(jPromptType);
    env->DeleteLocalRef(jPayload);
    env->DeleteLocalRef(jModel);
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        if (attach == JNI_EDETACHED) g_jvm->DetachCurrentThread();
        BLOGE("LLM Java HTTP: Java exception in doLlmHttpPostFromPayload");
        return false;
    }
    if (!jResult) {
        if (attach == JNI_EDETACHED) g_jvm->DetachCurrentThread();
        BLOGE("LLM Java HTTP: Java returned null");
        return false;
    }
    const char *utf = env->GetStringUTFChars(jResult, nullptr);
    if (utf) {
        *outResponse = utf;
        env->ReleaseStringUTFChars(jResult, utf);
    }
    env->DeleteLocalRef(jResult);
    if (attach == JNI_EDETACHED) g_jvm->DetachCurrentThread();
    return true;
}

} // namespace fastbotx