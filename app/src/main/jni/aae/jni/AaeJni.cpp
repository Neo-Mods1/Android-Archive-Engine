// JNI bridge for bin.nt.aae.Aae (statics) and bin.nt.aae.NativeAaeSession.
// Only talks to Aae.h — never to third-party headers directly.
// Sessions are pointers in jlongs; all callbacks are synchronous on the
// calling thread, so no global refs or thread attach/detach is needed.
//
// Failure contract: expected failures throw AaeException via throwAae() and
// return immediately (a return value with a pending exception is ignored by
// the JVM). The AAE_JNI_BEGIN/END barrier around every entry point converts
// anything else (std::bad_alloc, …) the same way — a C++ exception crossing
// the JNI boundary would otherwise terminate the process instead of
// arriving as a catchable exception. Signals (SEGV/ABRT) still can't be
// caught here; those mean a real native bug and belong in CrashActivity.
#include <jni.h>

#include <exception>
#include <string>
#include <vector>

#include "Aae.h"

#define AAE_JNI_BEGIN try {
#define AAE_JNI_CATCH(Env)                                                      \
    }                                                                           \
    catch (const std::exception& e) {                                           \
        aae::Error err;                                                         \
        err.set(aae::Status::Io, e.what());                                     \
        throwAae(Env, err);                                                     \
    }                                                                           \
    catch (...) {                                                               \
        aae::Error err;                                                         \
        err.set(aae::Status::Io, "native failure");                             \
        throwAae(Env, err);                                                     \
    }
#define AAE_JNI_END_VOID(Env) AAE_JNI_CATCH(Env)
#define AAE_JNI_END_VALUE(Env, Value)                                           \
    AAE_JNI_CATCH(Env)                                                          \
    return (Value);

namespace {

jstring toJString(JNIEnv* env, const std::string& s) { return env->NewStringUTF(s.c_str()); }

jobjectArray toJStringArray(JNIEnv* env, const std::vector<std::string>& v) {
    jclass cls = env->FindClass("java/lang/String");
    if (cls == nullptr) return nullptr;
    jobjectArray arr = env->NewObjectArray(static_cast<jsize>(v.size()), cls, nullptr);
    for (jsize i = 0; i < static_cast<jsize>(v.size()); ++i) {
        env->SetObjectArrayElement(arr, i, toJString(env, v[i]));
    }
    return arr;
}

std::string fromJString(JNIEnv* env, jstring j) {
    if (j == nullptr) return "";
    const char* c = env->GetStringUTFChars(j, nullptr);
    std::string s = c != nullptr ? c : "";
    if (c != nullptr) env->ReleaseStringUTFChars(j, c);
    return s;
}

aae::WriteOptions fromJWriteOptions(JNIEnv* env, jstring jPassword, jint encWire) {
    aae::WriteOptions opt;
    opt.password = fromJString(env, jPassword);
    opt.encryption = static_cast<int>(encWire);
    return opt;
}

void throwAae(JNIEnv* env, const aae::Error& err) {
    jclass excCls = env->FindClass("bin/nt/aae/AaeException");
    jclass kindCls = env->FindClass("bin/nt/aae/AaeException$Kind");
    if (excCls == nullptr || kindCls == nullptr) return;  // exception pending
    jmethodID fromWire =
        env->GetStaticMethodID(kindCls, "fromWire", "(I)Lbin/nt/aae/AaeException$Kind;");
    jmethodID ctor = env->GetMethodID(
        excCls, "<init>", "(Lbin/nt/aae/AaeException$Kind;Ljava/lang/String;)V");
    if (fromWire == nullptr || ctor == nullptr) return;
    jobject kind = env->CallStaticObjectMethod(kindCls, fromWire,
                                               static_cast<jint>(err.status));
    if (kind == nullptr) return;
    jstring msg = toJString(env, err.message);
    auto* exc = static_cast<jthrowable>(env->NewObject(excCls, ctor, kind, msg));
    if (exc != nullptr) env->Throw(exc);
}

aae::ISession* getSession(JNIEnv* env, jlong handle) {
    if (handle == 0) {
        aae::Error err;
        err.set(aae::Status::Io, "session is closed");
        throwAae(env, err);
        return nullptr;
    }
    return reinterpret_cast<aae::ISession*>(handle);
}

// Synchronous listener bridge: forwards chunk polls to
// AaeProgressListener.onProgress and converts false/exception to cancel.
class JniProgressSink : public aae::ProgressSink {
   public:
    JniProgressSink(JNIEnv* env, jobject listener, int fileIndex, int fileCount)
        : env_(env), listener_(listener), fileIndex_(fileIndex), fileCount_(fileCount) {
        jclass cls = env_->GetObjectClass(listener_);
        mid_ = env_->GetMethodID(cls, "onProgress", "(IIJJ)Z");
    }
    bool poll(int, int, long long bytesDone, long long bytesTotal) override {
        if (mid_ == nullptr) return false;
        jboolean cont = env_->CallBooleanMethod(listener_, mid_, static_cast<jint>(fileIndex_),
                                                static_cast<jint>(fileCount_),
                                                static_cast<jlong>(bytesDone),
                                                static_cast<jlong>(bytesTotal));
        if (env_->ExceptionCheck()) {
            env_->ExceptionClear();
            return false;
        }
        return cont == JNI_TRUE;
    }

   private:
    JNIEnv* env_;
    jobject listener_;
    jmethodID mid_ = nullptr;
    int fileIndex_;
    int fileCount_;
};

jobject toJEntry(JNIEnv* env, jclass entryCls, jmethodID ctor, const aae::Entry& e) {
    jstring name = toJString(env, e.path);
    if (name == nullptr) return nullptr;
    jobject obj = env->NewObject(entryCls, ctor, name, static_cast<jboolean>(e.isDir),
                                 static_cast<jlong>(e.size),
                                 static_cast<jlong>(e.compressedSize),
                                 static_cast<jint>(e.method),
                                 static_cast<jlong>(e.mtimeMillis), static_cast<jint>(e.crc),
                                 static_cast<jboolean>(e.encrypted));
    env->DeleteLocalRef(name);
    return obj;
}

jobject toJInfo(JNIEnv* env, const aae::ArchiveInfo& info) {
    jclass formatCls = env->FindClass("bin/nt/aae/AaeFormat");
    jclass infoCls = env->FindClass("bin/nt/aae/AaeArchiveInfo");
    if (formatCls == nullptr || infoCls == nullptr) return nullptr;
    jmethodID fromId =
        env->GetStaticMethodID(formatCls, "fromId", "(Ljava/lang/String;)Lbin/nt/aae/AaeFormat;");
    jmethodID ctor = env->GetMethodID(
        infoCls, "<init>",
        "(Lbin/nt/aae/AaeFormat;IJJLjava/lang/String;Z)V");
    if (fromId == nullptr || ctor == nullptr) return nullptr;
    jstring id = toJString(env, info.format);
    jobject format = env->CallStaticObjectMethod(formatCls, fromId, id);
    env->DeleteLocalRef(id);
    if (format == nullptr) return nullptr;
    jstring comment = toJString(env, info.comment);
    jobject obj = env->NewObject(infoCls, ctor, format, static_cast<jint>(info.entryCount),
                                 static_cast<jlong>(info.totalSize),
                                 static_cast<jlong>(info.totalPacked), comment,
                                 static_cast<jboolean>(info.hasEncrypted));
    env->DeleteLocalRef(comment);
    env->DeleteLocalRef(format);
    return obj;
}

}  // namespace

extern "C" {

// --- bin.nt.aae.Aae statics ---

JNIEXPORT jstring JNICALL Java_bin_nt_aae_Aae_nativeVersion(JNIEnv* env, jclass) {
    AAE_JNI_BEGIN
    return toJString(env, aae::version());
    AAE_JNI_END_VALUE(env, nullptr)
}

JNIEXPORT jobjectArray JNICALL Java_bin_nt_aae_Aae_nativeSupportedFormats(JNIEnv* env, jclass) {
    AAE_JNI_BEGIN
    return toJStringArray(env, aae::supportedFormats());
    AAE_JNI_END_VALUE(env, nullptr)
}

JNIEXPORT jstring JNICALL Java_bin_nt_aae_Aae_nativeProbe(JNIEnv* env, jclass, jstring path) {
    AAE_JNI_BEGIN
    return toJString(env, aae::probe(fromJString(env, path)));
    AAE_JNI_END_VALUE(env, nullptr)
}

// --- bin.nt.aae.NativeAaeSession statics (jlong session pointers) ---

JNIEXPORT jlong JNICALL Java_bin_nt_aae_NativeAaeSession_nativeOpen(
    JNIEnv* env, jclass, jstring path, jstring password, jint encWire, jboolean writable,
    jboolean fresh) {
    AAE_JNI_BEGIN
    aae::Error err;
    aae::WriteOptions opt = fromJWriteOptions(env, password, encWire);
    aae::ISession* s =
        aae::openSession(fromJString(env, path), opt, writable != JNI_FALSE, fresh != JNI_FALSE,
                         &err);
    if (s == nullptr) {
        throwAae(env, err);
        return 0;
    }
    return reinterpret_cast<jlong>(s);
    AAE_JNI_END_VALUE(env, 0)
}

JNIEXPORT void JNICALL Java_bin_nt_aae_NativeAaeSession_nativeClose(JNIEnv* env, jclass,
                                                                    jlong handle,
                                                                    jboolean discard) {
    AAE_JNI_BEGIN
    aae::ISession* s = getSession(env, handle);
    if (s == nullptr) return;
    aae::Error err;
    const bool ok = s->close(discard != JNI_FALSE, &err);
    delete s;
    if (!ok) throwAae(env, err);
    AAE_JNI_END_VOID(env)
}

JNIEXPORT jobjectArray JNICALL Java_bin_nt_aae_NativeAaeSession_nativeList(JNIEnv* env, jclass,
                                                                           jlong handle) {
    AAE_JNI_BEGIN
    aae::ISession* s = getSession(env, handle);
    if (s == nullptr) return nullptr;
    std::vector<aae::Entry> entries;
    aae::Error err;
    if (!s->list(entries, &err)) {
        throwAae(env, err);
        return nullptr;
    }
    jclass entryCls = env->FindClass("bin/nt/aae/AaeEntry");
    if (entryCls == nullptr) return nullptr;
    jmethodID ctor = env->GetMethodID(entryCls, "<init>", "(Ljava/lang/String;ZJJIJIZ)V");
    if (ctor == nullptr) return nullptr;
    jobjectArray arr = env->NewObjectArray(static_cast<jsize>(entries.size()), entryCls, nullptr);
    if (arr == nullptr) return nullptr;
    for (jsize i = 0; i < static_cast<jsize>(entries.size()); ++i) {
        jobject obj = toJEntry(env, entryCls, ctor, entries[i]);
        if (obj == nullptr) return nullptr;
        env->SetObjectArrayElement(arr, i, obj);
        env->DeleteLocalRef(obj);
    }
    return arr;
    AAE_JNI_END_VALUE(env, nullptr)
}

JNIEXPORT jobject JNICALL Java_bin_nt_aae_NativeAaeSession_nativeInfo(JNIEnv* env, jclass,
                                                                      jlong handle) {
    AAE_JNI_BEGIN
    aae::ISession* s = getSession(env, handle);
    if (s == nullptr) return nullptr;
    aae::ArchiveInfo info;
    aae::Error err;
    if (!s->info(info, &err)) {
        throwAae(env, err);
        return nullptr;
    }
    return toJInfo(env, info);
    AAE_JNI_END_VALUE(env, nullptr)
}

JNIEXPORT jbyteArray JNICALL Java_bin_nt_aae_NativeAaeSession_nativeReadBytes(
    JNIEnv* env, jclass, jlong handle, jstring entry, jint maxBytes) {
    AAE_JNI_BEGIN
    aae::ISession* s = getSession(env, handle);
    if (s == nullptr) return nullptr;
    std::vector<char> out;
    aae::Error err;
    if (!s->readBytes(fromJString(env, entry), static_cast<long long>(maxBytes), out, &err)) {
        throwAae(env, err);
        return nullptr;
    }
    jbyteArray arr = env->NewByteArray(static_cast<jsize>(out.size()));
    if (arr == nullptr) return nullptr;
    env->SetByteArrayRegion(arr, 0, static_cast<jsize>(out.size()),
                            reinterpret_cast<const jbyte*>(out.data()));
    return arr;
    AAE_JNI_END_VALUE(env, nullptr)
}

JNIEXPORT void JNICALL Java_bin_nt_aae_NativeAaeSession_nativeExtractTo(
    JNIEnv* env, jclass, jlong handle, jstring entry, jstring outPath, jobject listener) {
    AAE_JNI_BEGIN
    aae::ISession* s = getSession(env, handle);
    if (s == nullptr) return;
    JniProgressSink* sink = nullptr;
    JniProgressSink owned(env, listener, 0, 1);
    if (listener != nullptr) sink = &owned;
    aae::Error err;
    if (!s->extractTo(fromJString(env, entry), fromJString(env, outPath), sink, &err)) {
        throwAae(env, err);
    }
    AAE_JNI_END_VOID(env)
}

JNIEXPORT void JNICALL Java_bin_nt_aae_NativeAaeSession_nativeAddFile(
    JNIEnv* env, jclass, jlong handle, jstring entry, jstring srcPath, jint methodWire,
    jint level) {
    AAE_JNI_BEGIN
    aae::ISession* s = getSession(env, handle);
    if (s == nullptr) return;
    aae::WriteOptions opt;
    opt.method = static_cast<int>(methodWire);
    opt.level = static_cast<int>(level);
    aae::Error err;
    if (!s->addFile(fromJString(env, entry), fromJString(env, srcPath), opt, &err)) {
        throwAae(env, err);
    }
    AAE_JNI_END_VOID(env)
}

JNIEXPORT void JNICALL Java_bin_nt_aae_NativeAaeSession_nativeAddDirectory(JNIEnv* env, jclass,
                                                                           jlong handle,
                                                                           jstring entry) {
    AAE_JNI_BEGIN
    aae::ISession* s = getSession(env, handle);
    if (s == nullptr) return;
    aae::Error err;
    if (!s->addDirectory(fromJString(env, entry), &err)) throwAae(env, err);
    AAE_JNI_END_VOID(env)
}

JNIEXPORT void JNICALL Java_bin_nt_aae_NativeAaeSession_nativeDelete(JNIEnv* env, jclass,
                                                                     jlong handle, jstring entry) {
    AAE_JNI_BEGIN
    aae::ISession* s = getSession(env, handle);
    if (s == nullptr) return;
    aae::Error err;
    if (!s->remove(fromJString(env, entry), &err)) throwAae(env, err);
    AAE_JNI_END_VOID(env)
}

JNIEXPORT void JNICALL Java_bin_nt_aae_NativeAaeSession_nativeRename(JNIEnv* env, jclass,
                                                                     jlong handle, jstring from,
                                                                     jstring to) {
    AAE_JNI_BEGIN
    aae::ISession* s = getSession(env, handle);
    if (s == nullptr) return;
    aae::Error err;
    if (!s->rename(fromJString(env, from), fromJString(env, to), &err)) throwAae(env, err);
    AAE_JNI_END_VOID(env)
}

JNIEXPORT void JNICALL Java_bin_nt_aae_NativeAaeSession_nativeSetComment(JNIEnv* env, jclass,
                                                                         jlong handle,
                                                                         jstring comment) {
    AAE_JNI_BEGIN
    aae::ISession* s = getSession(env, handle);
    if (s == nullptr) return;
    aae::Error err;
    if (!s->setComment(fromJString(env, comment), &err)) throwAae(env, err);
    AAE_JNI_END_VOID(env)
}

}  // extern "C"
