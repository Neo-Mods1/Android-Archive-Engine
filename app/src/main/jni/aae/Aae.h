// aae engine public API. Providers (one .cpp per backend under
// providers/) implement ISession; JNI only talks to this header.
// Status wire ints must mirror AaeException.Kind in Kotlin.
#pragma once

#include <string>
#include <vector>

namespace aae {

// Compiled-in backends. Order = probe priority.
enum class Backend : int {
    Zip = 0,      // thirdparty/libzip (+libzippp)
    Archive = 1,  // (reserved: libarchive tar/7z/rar/… later)
    Gzip = 2,     // stream backends (providers/AaeStream.cpp)
    Bzip2 = 3,
    Xz = 4,
    Zstd = 5,
    Lz4 = 6,
    Tar = 7,
    SevenZ = 8,   // thirdparty/sevenzip (LZMA SDK C codec, read+write)
};

enum class Status : int {
    Ok = 0,
    Io = 1,
    NotArchive = 2,
    UnsupportedFormat = 3,
    NotFound = 4,
    PasswordRequired = 5,
    PasswordWrong = 6,
    UnsafeName = 7,
    Cancelled = 8,
    TooLarge = 9,
    UnsupportedOperation = 10,
};

struct Error {
    Status status = Status::Ok;
    std::string message;
    bool ok() const { return status == Status::Ok; }
    void set(Status s, const std::string& m) {
        status = s;
        message = m;
    }
};

// One entry inside an archive (stable across backends).
struct Entry {
    std::string path;
    bool isDir = false;
    long long size = 0;
    long long compressedSize = 0;
    int method = -1;  // libzip ZIP_CM_* value, -1 = unknown
    long long mtimeMillis = 0;
    int crc = 0;
    bool encrypted = false;
};

struct ArchiveInfo {
    std::string format;  // "zip", …
    int entryCount = 0;
    long long totalSize = 0;
    long long totalPacked = 0;
    std::string comment;
    bool hasEncrypted = false;
};

// Write knobs. Method/encryption use the AaeMethod/AaeEncryption wire ints
// from Kotlin (0=DEFAULT/NONE …); each backend maps them to its own enums.
struct WriteOptions {
    int method = 0;
    int level = 0;  // 0 = backend default, else 1..9
    std::string password;
    int encryption = 0;  // 0=NONE, 1=AES_128, 2=AES_256
};

// Progress sink. Called synchronously on the calling thread, roughly once
// per 512 KiB chunk plus once at completion. Return false to cancel.
class ProgressSink {
   public:
    virtual ~ProgressSink() = default;
    virtual bool poll(int fileIndex, int fileCount, long long bytesDone,
                      long long bytesTotal) = 0;
};

// Open archive session. Implemented per backend; single-threaded.
class ISession {
   public:
    virtual ~ISession() = default;
    virtual bool list(std::vector<Entry>& out, Error* err) = 0;
    virtual bool info(ArchiveInfo& out, Error* err) = 0;
    virtual bool readBytes(const std::string& entry, long long maxBytes,
                           std::vector<char>& out, Error* err) = 0;
    virtual bool extractTo(const std::string& entry, const std::string& outPath,
                           ProgressSink* progress, Error* err) = 0;
    virtual bool addFile(const std::string& entryName, const std::string& srcPath,
                         const WriteOptions& opt, Error* err) = 0;
    virtual bool addDirectory(const std::string& entryName, Error* err) = 0;
    virtual bool remove(const std::string& entry, Error* err) = 0;
    virtual bool rename(const std::string& from, const std::string& to, Error* err) = 0;
    virtual bool setComment(const std::string& comment, Error* err) = 0;
    virtual bool close(bool discard, Error* err) = 0;  // commits unless discard
};

// Registry — implemented in core/AaeRegistry.cpp.
std::string version();
std::vector<std::string> supportedFormats();

// Probe by magic bytes. Returns "zip", "tar", … or "".
std::string probe(const std::string& fsPath);

// Opens a session, dispatching on probe(). fresh=true opens in New mode
// (create, truncating); writable=true opens Write (append/create).
// Non-writable opens ReadOnly.
ISession* openSession(const std::string& fsPath, const WriteOptions& opt, bool writable,
                      bool fresh, Error* err);

// Registration hook each provider .cpp calls into.
using ProbeFn = std::string (*)(const std::string& fsPath);
using OpenFn = ISession* (*)(const std::string& fsPath, const WriteOptions& opt, bool writable,
                             bool fresh, Error* err);
void registerBackend(Backend backend, const std::string& formatId, ProbeFn probe, OpenFn open);

// Shared: zip-slip guard. Rejects absolute paths, backslashes and any
// "." / ".." segment. Empty segments (double "//", dir trailing "/") pass.
bool isSafeEntryName(const std::string& name);

}  // namespace aae
