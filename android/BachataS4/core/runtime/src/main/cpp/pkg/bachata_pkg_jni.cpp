#include "pkg_extractor.h"

#include <jni.h>

#include <cstring>
#include <string>

namespace {

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

} // namespace

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
