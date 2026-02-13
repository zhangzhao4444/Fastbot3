/*
 * This code is licensed under the Fastbot license. You may obtain a copy of this license in the LICENSE.txt file in the root directory of this source tree.
 */
/**
 * @authors Jianqiang Guo, Yuhui Su
 */
#include <jni.h>

#ifndef __Fastbot_Native_Jni_H__
#define __Fastbot_Native_Jni_H__
#ifdef __cplusplus
extern "C" {
#endif

// getAction (returns operate as JSON string)
JNIEXPORT jstring JNICALL
Java_com_bytedance_fastbot_AiClient_getOperateJsonNative(JNIEnv *env, jobject, jstring, jstring);

// InitAgent
JNIEXPORT void JNICALL
Java_com_bytedance_fastbot_AiClient_initAgentNative(JNIEnv *env, jobject, jint, jstring, jint);

// loadResMapping
JNIEXPORT void JNICALL
Java_com_bytedance_fastbot_AiClient_loadResMappingNative(JNIEnv *env, jobject, jstring);

// single-point shield check
JNIEXPORT jboolean JNICALL
Java_com_bytedance_fastbot_AiClient_checkPointInShieldNative(JNIEnv *env, jobject, jstring activity, jfloat pointX,
                                                             jfloat pointY);
JNIEXPORT jbooleanArray JNICALL
Java_com_bytedance_fastbot_AiClient_checkPointsInShieldNative(JNIEnv *env, jobject, jstring activity,
                                                              jfloatArray xCoords, jfloatArray yCoords);
JNIEXPORT jstring JNICALL
Java_com_bytedance_fastbot_AiClient_getActionFromBufferNative(JNIEnv *env, jobject, jstring activity,
                                                              jobject xmlBuffer, jint byteLength);
JNIEXPORT jobject JNICALL
Java_com_bytedance_fastbot_AiClient_getActionFromBufferNativeStructured(JNIEnv *env, jobject, jstring activity,
                                                                         jobject xmlBuffer, jint byteLength);
JNIEXPORT jstring JNICALL
Java_com_bytedance_fastbot_AiClient_getNativeVersion(JNIEnv *env, jclass clazz);
JNIEXPORT void JNICALL
Java_com_bytedance_fastbot_AiClient_reportActivityNative(JNIEnv *env, jobject, jstring activity);
JNIEXPORT jstring JNICALL
Java_com_bytedance_fastbot_AiClient_getCoverageJsonNative(JNIEnv *env, jobject);
JNIEXPORT jstring JNICALL
Java_com_bytedance_fastbot_AiClient_getNextFuzzActionNative(JNIEnv *env, jobject, jint displayWidth,
                                                            jint displayHeight, jboolean simplify);

/** Register AiClient instance as LLM HTTP runner for native when libcurl is not available. */
JNIEXPORT void JNICALL
Java_com_bytedance_fastbot_AiClient_nativeRegisterLlmHttpRunner(JNIEnv *env, jobject thiz);

#ifdef __cplusplus
}
#endif

#endif //__Fastbot_Native_Jni_H__