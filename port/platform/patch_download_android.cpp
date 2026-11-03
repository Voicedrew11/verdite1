#include <kf/platform/patch_archive.h>
#include <SDL3/SDL_system.h>
#include <jni.h>

namespace kf {
namespace {
constexpr const char *failed_message =
    "The download failed. Check your internet connection, or choose the patch file yourself.";

// A JNI local reference. Native code holds these until its thread ends, and the game's main thread
// does not end, so each is dropped here.
struct LocalRef {
    JNIEnv *env;
    jobject object;
    ~LocalRef() {
        if (env && object)
            env->DeleteLocalRef(object);
    }
};
} // namespace

std::optional<std::vector<u8>> patch_archive_download(const char *url, std::string *error) {
    auto *env = static_cast<JNIEnv *>(SDL_GetAndroidJNIEnv());
    const LocalRef activity{env, static_cast<jobject>(SDL_GetAndroidActivity())};
    *error = failed_message;
    if (!env || !activity.object)
        return std::nullopt;
    // Through the activity's class: FindClass on a thread not started by Java misses the app's
    // classes.
    const LocalRef type{env, env->GetObjectClass(activity.object)};
    jmethodID method = env->GetMethodID(static_cast<jclass>(type.object), "downloadFile",
                                              "(Ljava/lang/String;I)[B");
    const LocalRef address{env, method ? env->NewStringUTF(url) : nullptr};
    if (!address.object) {
        env->ExceptionClear();
        return std::nullopt;
    }
    const LocalRef result{env, env->CallObjectMethod(activity.object, method, address.object,
                                                     static_cast<jint>(patch_file_limit))};
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        return std::nullopt;
    }
    if (!result.object)
        return std::nullopt;
    const auto array = static_cast<jbyteArray>(result.object);
    std::vector<u8> bytes(static_cast<std::size_t>(env->GetArrayLength(array)));
    env->GetByteArrayRegion(array, 0, static_cast<jsize>(bytes.size()),
                            reinterpret_cast<jbyte *>(bytes.data()));
    error->clear();
    return bytes;
}
} // namespace kf
