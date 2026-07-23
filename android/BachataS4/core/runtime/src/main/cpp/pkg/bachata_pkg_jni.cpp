#include "pkg_extractor.h"
#include "pkg_rsa_bridge.h"

#include <android/log.h>
#include <jni.h>

#include <cstring>
#include <mutex>
#include <string>

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "BachataPkg", __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "BachataPkg", __VA_ARGS__)

namespace {

JavaVM* g_vm = nullptr;
std::mutex g_rsa_mu;
jclass g_pkg_rsa_cls = nullptr; // global ref
jmethodID g_pkg_rsa_decrypt = nullptr;

struct ProgressCtx {
    JavaVM* jvm = nullptr;
    jobject listener = nullptr; // global ref
    jmethodID method = nullptr;
};

void progress_trampoline(void* ctx, uint64_t done, uint64_t total, const char* file) {
    auto* p = static_cast<ProgressCtx*>(ctx);
    if (!p || !p->jvm || !p->listener || !p->method) return;
    JNIEnv* env = nullptr;
    bool attached = false;
    if (p->jvm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK) {
        if (p->jvm->AttachCurrentThread(&env, nullptr) != 0) return;
        attached = true;
    }
    jstring jfile = env->NewStringUTF(file ? file : "");
    env->CallVoidMethod(p->listener, p->method, static_cast<jlong>(done), static_cast<jlong>(total),
                        jfile);
    env->DeleteLocalRef(jfile);
    if (attached) p->jvm->DetachCurrentThread();
}

jobject make_probe_result(JNIEnv* env, const BachataPkgProbe& probe) {
    jclass cls = env->FindClass("com/bachatas4/android/runtime/pkg/PkgProbeResult");
    jmethodID ctor = env->GetMethodID(
        cls, "<init>",
        "(Ljava/lang/String;JJLjava/lang/String;Lcom/bachatas4/android/runtime/pkg/PkgStatus;"
        "Ljava/lang/String;)V");
    jclass statusCls = env->FindClass("com/bachatas4/android/runtime/pkg/PkgStatus");
    const char* statusName = "ERROR";
    if (probe.status == 0) statusName = "OK";
    else if (probe.status == 1) statusName = "NEED_PASSCODE";
    else if (probe.status == 2) statusName = "CANCELLED";
    jfieldID fid = env->GetStaticFieldID(statusCls, statusName,
                                         "Lcom/bachatas4/android/runtime/pkg/PkgStatus;");
    jobject status = env->GetStaticObjectField(statusCls, fid);
    jstring cid = env->NewStringUTF(probe.content_id);
    jstring hint = env->NewStringUTF(probe.title_hint);
    jstring msg = probe.message[0] ? env->NewStringUTF(probe.message) : nullptr;
    jobject obj = env->NewObject(cls, ctor, cid, static_cast<jlong>(probe.package_size),
                                 static_cast<jlong>(probe.pfs_image_size), hint, status, msg);
    return obj;
}

jobject make_extract_result(JNIEnv* env, int status, const char* message, const char* content_id) {
    jclass cls = env->FindClass("com/bachatas4/android/runtime/pkg/PkgExtractResult");
    jmethodID ctor = env->GetMethodID(
        cls, "<init>",
        "(Lcom/bachatas4/android/runtime/pkg/PkgStatus;Ljava/lang/String;Ljava/lang/String;)V");
    jclass statusCls = env->FindClass("com/bachatas4/android/runtime/pkg/PkgStatus");
    const char* statusName = "ERROR";
    if (status == 0) statusName = "OK";
    else if (status == 1) statusName = "NEED_PASSCODE";
    else if (status == 2) statusName = "CANCELLED";
    jfieldID fid = env->GetStaticFieldID(statusCls, statusName,
                                         "Lcom/bachatas4/android/runtime/pkg/PkgStatus;");
    jobject st = env->GetStaticObjectField(statusCls, fid);
    jstring msg = message ? env->NewStringUTF(message) : nullptr;
    jstring cid = content_id ? env->NewStringUTF(content_id) : nullptr;
    return env->NewObject(cls, ctor, st, msg, cid);
}

bool ensure_rsa_method(JNIEnv* env) {
    if (g_pkg_rsa_cls && g_pkg_rsa_decrypt) return true;
    jclass local = env->FindClass("com/bachatas4/android/runtime/pkg/PkgRsa");
    if (!local) {
        LOGE("FindClass PkgRsa failed");
        if (env->ExceptionCheck()) env->ExceptionDescribe();
        env->ExceptionClear();
        return false;
    }
    g_pkg_rsa_cls = static_cast<jclass>(env->NewGlobalRef(local));
    env->DeleteLocalRef(local);
    g_pkg_rsa_decrypt = env->GetStaticMethodID(g_pkg_rsa_cls, "decrypt", "([BZ)[B");
    if (!g_pkg_rsa_decrypt) {
        LOGE("GetStaticMethodID PkgRsa.decrypt failed");
        if (env->ExceptionCheck()) env->ExceptionDescribe();
        env->ExceptionClear();
        return false;
    }
    return true;
}

} // namespace

extern "C" int bachata_pkg_rsa_decrypt(const uint8_t* ciphertext256, uint8_t* out32, int is_dk3) {
    if (!g_vm || !ciphertext256 || !out32) return -1;
    std::lock_guard<std::mutex> lock(g_rsa_mu);
    JNIEnv* env = nullptr;
    bool attached = false;
    if (g_vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK) {
        if (g_vm->AttachCurrentThread(&env, nullptr) != 0) {
            LOGE("RSA AttachCurrentThread failed");
            return -2;
        }
        attached = true;
    }
    if (!ensure_rsa_method(env)) {
        if (attached) g_vm->DetachCurrentThread();
        return -3;
    }
    jbyteArray jin = env->NewByteArray(256);
    env->SetByteArrayRegion(jin, 0, 256, reinterpret_cast<const jbyte*>(ciphertext256));
    auto jout = static_cast<jbyteArray>(
        env->CallStaticObjectMethod(g_pkg_rsa_cls, g_pkg_rsa_decrypt, jin, is_dk3 ? JNI_TRUE : JNI_FALSE));
    env->DeleteLocalRef(jin);
    if (env->ExceptionCheck()) {
        LOGE("PkgRsa.decrypt threw");
        env->ExceptionDescribe();
        env->ExceptionClear();
        if (attached) g_vm->DetachCurrentThread();
        return -4;
    }
    if (!jout) {
        LOGE("PkgRsa.decrypt returned null");
        if (attached) g_vm->DetachCurrentThread();
        return -5;
    }
    const jsize n = env->GetArrayLength(jout);
    if (n < 32) {
        LOGE("PkgRsa.decrypt short output len=%d", static_cast<int>(n));
        env->DeleteLocalRef(jout);
        if (attached) g_vm->DetachCurrentThread();
        return -6;
    }
    env->GetByteArrayRegion(jout, 0, 32, reinterpret_cast<jbyte*>(out32));
    env->DeleteLocalRef(jout);
    if (attached) g_vm->DetachCurrentThread();
    LOGI("RSA decrypt ok isDk3=%d", is_dk3);
    return 0;
}

extern "C" JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void*) {
    g_vm = vm;
    JNIEnv* env = nullptr;
    if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK) {
        return JNI_ERR;
    }
    // Eager-resolve PkgRsa so first probe is not missing classloader edge cases.
    ensure_rsa_method(env);
    LOGI("JNI_OnLoad bachata_pkg ok");
    return JNI_VERSION_1_6;
}

extern "C" JNIEXPORT jobject JNICALL
Java_com_bachatas4_android_runtime_pkg_PkgExtractor_nativeProbe(JNIEnv* env, jclass, jint fd) {
    BachataPkgProbe probe{};
    bachata_pkg_probe(fd, &probe);
    return make_probe_result(env, probe);
}

extern "C" JNIEXPORT jobject JNICALL
Java_com_bachatas4_android_runtime_pkg_PkgExtractor_nativeExtract(
    JNIEnv* env, jclass, jint fd, jstring outPath, jstring passcode, jobject listener) {
    const char* path = env->GetStringUTFChars(outPath, nullptr);
    const char* pass = passcode ? env->GetStringUTFChars(passcode, nullptr) : nullptr;

    ProgressCtx ctx;
    if (listener) {
        env->GetJavaVM(&ctx.jvm);
        ctx.listener = env->NewGlobalRef(listener);
        jclass lcls = env->GetObjectClass(listener);
        ctx.method = env->GetMethodID(lcls, "onProgress", "(JJLjava/lang/String;)V");
    }

    const int status = bachata_pkg_extract(
        fd, path, pass, listener ? progress_trampoline : nullptr, listener ? &ctx : nullptr);

    if (path) env->ReleaseStringUTFChars(outPath, path);
    if (pass) env->ReleaseStringUTFChars(passcode, pass);
    if (ctx.listener) env->DeleteGlobalRef(ctx.listener);

    const char* msg = nullptr;
    if (status == 1) msg = "Passcode required";
    else if (status == 2) msg = "Cancelled";
    else if (status == 3) msg = "Extract failed";
    return make_extract_result(env, status, msg, nullptr);
}

extern "C" JNIEXPORT void JNICALL
Java_com_bachatas4_android_runtime_pkg_PkgExtractor_nativeCancel(JNIEnv*, jclass) {
    bachata_pkg_cancel();
}
