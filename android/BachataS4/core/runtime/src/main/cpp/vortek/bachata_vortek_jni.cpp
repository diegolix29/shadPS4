#include "bachata_vortek_server.h"
#include "bachata_vortek_logging.h"
#include "bachata_vk_context.h"

#include <jni.h>
#include <string>

namespace {

std::string jstring_to_std(JNIEnv* env, jstring value) {
    if (!value) return {};
    const char* chars = env->GetStringUTFChars(value, nullptr);
    if (!chars) return {};
    std::string out(chars);
    env->ReleaseStringUTFChars(value, chars);
    return out;
}

jobject make_result(JNIEnv* env, int code, const char* message) {
    jclass cls = env->FindClass("com/bachatas4/android/runtime/vortek/NativeVortekResult");
    if (!cls) return nullptr;
    jmethodID ctor = env->GetMethodID(cls, "<init>", "(ILjava/lang/String;)V");
    if (!ctor) return nullptr;
    jstring jmsg = env->NewStringUTF(message ? message : "");
    jobject obj = env->NewObject(cls, ctor, code, jmsg);
    if (jmsg) env->DeleteLocalRef(jmsg);
    return obj;
}

const char* error_name(int code) {
    switch (code) {
        case BACHATA_VORTEK_OK: return "ok";
        case BACHATA_VORTEK_ALREADY_RUNNING: return "already_running";
        case BACHATA_VORTEK_ERR_SOCKET_PATH: return "socket_path";
        case BACHATA_VORTEK_ERR_SOCKET_TOO_LONG: return "socket_path_too_long";
        case BACHATA_VORTEK_ERR_UNSAFE_EXISTING: return "unsafe_existing_socket_path";
        case BACHATA_VORTEK_ERR_PARENT_MISSING: return "parent_missing";
        case BACHATA_VORTEK_ERR_BIND: return "server_start_failed";
        case BACHATA_VORTEK_ERR_LOADER: return "loader_failed";
        case BACHATA_VORTEK_ERR_SYMBOL: return "symbol_missing";
        case BACHATA_VORTEK_ERR_LISTEN: return "listen_failed";
        case BACHATA_VORTEK_ERR_INTERNAL: return "internal";
        case BACHATA_VORTEK_ERR_NOT_RUNNING: return "not_running";
        case BACHATA_VORTEK_ERR_PROTOCOL: return "protocol_major_mismatch";
        case BACHATA_VORTEK_ERR_CLIENT_DISCONNECTED: return "client_disconnected";
        case BACHATA_VORTEK_ERR_CONTEXT: return "context_failed";
        default: return "unknown";
    }
}

} // namespace

#define BACHATA_JNI_EXPORT extern "C" __attribute__((visibility("default"))) JNIEXPORT

BACHATA_JNI_EXPORT jobject JNICALL
Java_com_bachatas4_android_runtime_vortek_VortekNativeBridge_nativeStartServer(
    JNIEnv* env,
    jclass,
    jstring socketPath,
    jstring expectedClientBuild,
    jstring serverBuild) {
    const std::string path = jstring_to_std(env, socketPath);
    const std::string client = jstring_to_std(env, expectedClientBuild);
    const std::string server = jstring_to_std(env, serverBuild);
    const int code = bachata_vortek_server_start(path.c_str(), client.c_str(), server.c_str());
    return make_result(env, code, error_name(code));
}

BACHATA_JNI_EXPORT jint JNICALL
Java_com_bachatas4_android_runtime_vortek_VortekNativeBridge_nativeGetState(
    JNIEnv*,
    jclass) {
    return bachata_vortek_server_state();
}

BACHATA_JNI_EXPORT jint JNICALL
Java_com_bachatas4_android_runtime_vortek_VortekNativeBridge_nativeLastError(
    JNIEnv*,
    jclass) {
    return bachata_vortek_server_last_error();
}

BACHATA_JNI_EXPORT jobject JNICALL
Java_com_bachatas4_android_runtime_vortek_VortekNativeBridge_nativeStopServer(
    JNIEnv* env,
    jclass) {
    const int code = bachata_vortek_server_stop();
    return make_result(env, code, error_name(code));
}

BACHATA_JNI_EXPORT jobject JNICALL
Java_com_bachatas4_android_runtime_vortek_VortekNativeBridge_nativeWaitSocketReady(
    JNIEnv* env,
    jclass,
    jint timeoutMs) {
    const int code = bachata_vortek_server_wait_socket_ready(timeoutMs);
    return make_result(env, code, error_name(code));
}

BACHATA_JNI_EXPORT jobject JNICALL
Java_com_bachatas4_android_runtime_vortek_VortekNativeBridge_nativeWaitContextReady(
    JNIEnv* env,
    jclass,
    jint timeoutMs) {
    const int code = bachata_vortek_server_wait_context_ready(timeoutMs);
    return make_result(env, code, error_name(code));
}

BACHATA_JNI_EXPORT jobject JNICALL
Java_com_bachatas4_android_runtime_vortek_VortekNativeBridge_nativeProtocolSelfTest(
    JNIEnv* env,
    jclass,
    jstring socketPath,
    jstring clientBuild) {
    const std::string path = jstring_to_std(env, socketPath);
    const std::string client = jstring_to_std(env, clientBuild);
    const int code = bachata_vortek_protocol_self_test(path.c_str(), client.c_str());
    return make_result(env, code, error_name(code));
}

BACHATA_JNI_EXPORT jstring JNICALL
Java_com_bachatas4_android_runtime_vortek_VortekNativeBridge_nativeHostApiVersion(
    JNIEnv* env,
    jclass) {
    return env->NewStringUTF(bachata_vortek_server_host_api_version());
}

BACHATA_JNI_EXPORT jobject JNICALL
Java_com_bachatas4_android_runtime_vortek_VortekNativeBridge_nativeSetWindowBridge(
    JNIEnv* env,
    jclass,
    jobject bridge) {
    const int code = bachata_vk_set_window_bridge(env, bridge);
    return make_result(env, code == 0 ? BACHATA_VORTEK_OK : BACHATA_VORTEK_ERR_INTERNAL,
                       code == 0 ? "ok" : "window_bridge");
}
