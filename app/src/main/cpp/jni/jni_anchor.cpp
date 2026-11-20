// Diamond Engine — JNI_OnLoad / JNI_OnUnload.
//
// This is the only source of libengine.so: the rest of the object is the three
// static libraries linked together. Keeping JNI_OnLoad here means the bridge
// code stays free of loader-lifecycle bookkeeping.
#include <jni.h>

#include <android/log.h>

namespace {

constexpr const char *kTag = "DiamondJNI";

} // namespace

extern "C" JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM *vm, void *) {
    JNIEnv *env = nullptr;
    if (vm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6) != JNI_OK) {
        __android_log_print(ANDROID_LOG_ERROR, kTag, "JNI_OnLoad: unsupported VM version");
        return JNI_ERR;
    }
    __android_log_print(ANDROID_LOG_INFO, kTag, "libengine loaded");
    return JNI_VERSION_1_6;
}

extern "C" JNIEXPORT void JNICALL JNI_OnUnload(JavaVM *, void *) {
    __android_log_print(ANDROID_LOG_INFO, kTag, "libengine unloaded");
}
