// ZIP backend over libzippp (third_party/libzippp on our in-tree libzip).
// Probe is magic-first; sessions implement the full ISession surface
// (list/info/read/extract + add/delete/rename/comment + passwords).
#include "Aae.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sys/stat.h>
#include <sys/types.h>

#ifdef AAE_HAVE_LIBZIPPP
#include <zip.h>

#include "libzippp.h"
#endif

namespace aae {
namespace zip {

std::string probeZip(const std::string& fsPath) {
#ifdef AAE_HAVE_LIBZIP
    // Magic-first: PK\x03\x04 (local header), PK\x05\x06 (empty),
    // PK\x07\x08 (spanned). Extension is only a tiebreak.
    FILE* f = std::fopen(fsPath.c_str(), "rb");
    if (f == nullptr) return "";
    unsigned char sig[4] = {0, 0, 0, 0};
    size_t n = std::fread(sig, 1, 4, f);
    std::fclose(f);
    if (n < 4 || sig[0] != 'P' || sig[1] != 'K') return "";
    if ((sig[2] == 0x03 && sig[3] == 0x04) ||
        (sig[2] == 0x05 && sig[3] == 0x06) ||
        (sig[2] == 0x07 && sig[3] == 0x08)) {
        return "zip";
    }
    return "";
#else
    (void)fsPath;
    return "";
#endif
}

#ifdef AAE_HAVE_LIBZIPPP

using libzippp::ZipArchive;

// Real ZIP_CM_* for AaeEntry.method (libzippp's own enum is positional).
int toZipMethod(libzippp::CompressionMethod m) {
    using CM = libzippp::CompressionMethod;
    switch (m) {
        case CM::STORE:
            return ZIP_CM_STORE;
        case CM::DEFLATE:
            return ZIP_CM_DEFLATE;
#ifdef LIBZIPPP_USE_BZIP2
        case CM::BZIP2:
            return ZIP_CM_BZIP2;
#endif
#ifdef LIBZIPPP_USE_XZ
        case CM::XZ:
            return ZIP_CM_XZ;
#endif
#ifdef LIBZIPPP_USE_ZSTD
        case CM::ZSTD:
            return ZIP_CM_ZSTD;
#endif
        case CM::DEFAULT:
        default:
            return ZIP_CM_DEFAULT;
    }
}

// AaeMethod wire int -> libzippp enum.
libzippp::CompressionMethod fromWireMethod(int wire) {
    using CM = libzippp::CompressionMethod;
    switch (wire) {
        case 1:
            return CM::STORE;
        case 2:
            return CM::DEFLATE;
#ifdef LIBZIPPP_USE_BZIP2
        case 3:
            return CM::BZIP2;
#endif
#ifdef LIBZIPPP_USE_XZ
        case 4:
            return CM::XZ;
#endif
#ifdef LIBZIPPP_USE_ZSTD
        case 5:
            return CM::ZSTD;
#endif
        default:
            return CM::DEFAULT;
    }
}

ZipArchive::Encryption fromWireEncryption(int wire) {
#ifdef LIBZIPPP_WITH_ENCRYPTION
    if (wire == 1) return ZipArchive::Encryption::Aes128;
    if (wire == 2) return ZipArchive::Encryption::Aes256;
#else
    (void)wire;
#endif
    return ZipArchive::Encryption::None;
}

void fillEntry(const libzippp::ZipEntry& e, Entry& out) {
    out.path = e.getName();
    out.isDir = e.isDirectory();
    out.size = static_cast<long long>(e.getSize());
    out.compressedSize = static_cast<long long>(e.getDeflatedSize());
    out.method = toZipMethod(e.getCompressionMethod());
    out.mtimeMillis = static_cast<long long>(e.getDate()) * 1000LL;
    out.crc = e.getCRC();
    out.encrypted = e.getEncryptionMethod() != ZIP_EM_NONE;
}

// Read/decrypt failure on an encrypted entry is a password problem, not
// generic IO. Callers pass the session password for the distinction.
void passwordAwareError(const libzippp::ZipEntry& e, const std::string& password,
                        const std::string& entry, Error* err) {
    if (err == nullptr) return;
    if (e.getEncryptionMethod() != ZIP_EM_NONE) {
        if (password.empty()) {
            err->set(Status::PasswordRequired, "password required: " + entry);
        } else {
            err->set(Status::PasswordWrong, "wrong password: " + entry);
        }
    } else {
        err->set(Status::Io, "cannot read entry: " + entry);
    }
}

class ZipSession : public ISession {
   public:
    ZipSession(ZipArchive* z, std::string path, bool writable, std::string password)
        : z_(z),
          path_(std::move(path)),
          writable_(writable),
          password_(std::move(password)) {}
    ~ZipSession() override {
        if (!closed_) {
            z_->discard();  // never commit from a leaked session
        }
        delete z_;
    }

    bool list(std::vector<Entry>& out, Error* err) override {
        if (!checkOpen(err)) return false;
        for (const auto& e : z_->getEntries()) {
            if (e.isNull()) continue;
            Entry o;
            fillEntry(e, o);
            out.push_back(o);
        }
        return true;
    }

    bool info(ArchiveInfo& out, Error* err) override {
        if (!checkOpen(err)) return false;
        std::vector<Entry> entries;
        if (!list(entries, err)) return false;
        out.format = "zip";
        out.entryCount = static_cast<int>(entries.size());
        out.comment = z_->getComment();
        for (const auto& e : entries) {
            if (!e.isDir) {
                out.totalSize += e.size;
                out.totalPacked += e.compressedSize;
            }
            out.hasEncrypted = out.hasEncrypted || e.encrypted;
        }
        return true;
    }

    bool readBytes(const std::string& entry, long long maxBytes, std::vector<char>& out,
                   Error* err) override {
        if (!checkOpen(err)) return false;
        if (!requireSafe(entry, err)) return false;
        libzippp::ZipEntry e = z_->getEntry(entry);
        if (e.isNull()) {
            if (err != nullptr) err->set(Status::NotFound, "no such entry: " + entry);
            return false;
        }
        if (e.isDirectory()) {
            if (err != nullptr) err->set(Status::Io, "not a file: " + entry);
            return false;
        }
        if (static_cast<long long>(e.getSize()) > maxBytes) {
            if (err != nullptr) {
                err->set(Status::TooLarge, "entry larger than limit: " + entry);
            }
            return false;
        }
        bool tooLarge = false;
        const int rc = z_->readEntry(
            e,
            [&](const void* data, libzippp_uint64 len) {
                if (static_cast<long long>(out.size() + len) > maxBytes) {
                    tooLarge = true;
                    return false;
                }
                const char* p = static_cast<const char*>(data);
                out.insert(out.end(), p, p + len);
                return true;
            });
        if (tooLarge) {
            if (err != nullptr) err->set(Status::TooLarge, "entry larger than limit: " + entry);
            return false;
        }
        if (rc != LIBZIPPP_OK) {
            passwordAwareError(e, password_, entry, err);
            return false;
        }
        return true;
    }

    bool extractTo(const std::string& entry, const std::string& outPath,
                   ProgressSink* progress, Error* err) override {
        if (!checkOpen(err)) return false;
        if (!requireSafe(entry, err)) return false;
        libzippp::ZipEntry e = z_->getEntry(entry);
        if (e.isNull()) {
            if (err != nullptr) err->set(Status::NotFound, "no such entry: " + entry);
            return false;
        }
        if (e.isDirectory()) {
            // mkdir -p: entries arrive in archive order, so parents may not
            // exist yet (and neither may the dest root on odd filesystems).
            if (!mkdirs(outPath)) {
                if (err != nullptr) {
                    err->set(Status::Io,
                             "cannot create dir: " + outPath + ": " + std::strerror(errno));
                }
                return false;
            }
            return true;
        }
        std::ofstream out(outPath, std::ios::binary | std::ios::trunc);
        if (!out) {
            if (err != nullptr) err->set(Status::Io, "cannot write: " + outPath);
            return false;
        }
        const long long total = static_cast<long long>(e.getSize());
        long long done = 0;
        bool cancelled = false;
        const int rc = z_->readEntry(
            e,
            [&](const void* data, libzippp_uint64 len) {
                out.write(static_cast<const char*>(data), static_cast<std::streamsize>(len));
                if (!out) return false;
                done += static_cast<long long>(len);
                if (progress != nullptr && !progress->poll(0, 1, done, total)) {
                    cancelled = true;
                    return false;
                }
                return true;
            });
        out.close();
        if (cancelled) {
            std::remove(outPath.c_str());
            if (err != nullptr) err->set(Status::Cancelled, "cancelled: " + entry);
            return false;
        }
        if (rc != LIBZIPPP_OK || out.fail()) {
            std::remove(outPath.c_str());  // don't leave partial files
            passwordAwareError(e, password_, entry, err);
            if (err != nullptr && err->ok()) {
                err->set(Status::Io, "extract failed: " + entry);
            }
            return false;
        }
        if (progress != nullptr) progress->poll(0, 1, total, total);
        return true;
    }

    bool addFile(const std::string& entryName, const std::string& srcPath,
                 const WriteOptions& opt, Error* err) override {
        if (!checkWritable(err)) return false;
        if (!requireSafe(entryName, err)) return false;
        if (!checkWriteOptions(opt, err)) return false;
        struct stat st {};
        if (::stat(srcPath.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) {
            if (err != nullptr) err->set(Status::Io, "no such file: " + srcPath);
            return false;
        }
        if (!z_->addFile(entryName, srcPath)) {
            if (err != nullptr) err->set(Status::Io, "cannot add: " + entryName);
            return false;
        }
        if (opt.method != 0 || opt.level != 0) {
            libzippp::ZipEntry e = z_->getEntry(entryName);
            if (!e.isNull() &&
                !z_->setEntryCompressionConfig(e, fromWireMethod(opt.method),
                                               static_cast<libzippp_uint32>(opt.level))) {
                if (err != nullptr) err->set(Status::Io, "cannot set method: " + entryName);
                return false;
            }
        }
        return true;
    }

    bool addDirectory(const std::string& entryName, Error* err) override {
        if (!checkWritable(err)) return false;
        if (!requireSafe(entryName, err)) return false;
        std::string dir = entryName;
        if (dir.back() != '/') dir += '/';
        if (!z_->addEntry(dir)) {
            if (err != nullptr) err->set(Status::Io, "cannot add dir: " + entryName);
            return false;
        }
        return true;
    }

    bool remove(const std::string& entry, Error* err) override {
        if (!checkWritable(err)) return false;
        if (!z_->hasEntry(entry, /*excludeDirectories=*/false, /*caseSensitive=*/true)) {
            if (err != nullptr) err->set(Status::NotFound, "no such entry: " + entry);
            return false;
        }
        // deleteEntry returns the COUNT removed (1 for a file, N for a dir
        // tree), negative on error — not LIBZIPPP_OK. Comparing against OK
        // misreported every successful delete as a failure.
        if (z_->deleteEntry(entry) <= 0) {
            if (err != nullptr) err->set(Status::Io, "cannot delete: " + entry);
            return false;
        }
        return true;
    }

    bool rename(const std::string& from, const std::string& to, Error* err) override {
        if (!checkWritable(err)) return false;
        if (!requireSafe(to, err)) return false;
        // Same count-vs-OK contract as deleteEntry above.
        if (z_->renameEntry(from, to) <= 0) {
            if (err != nullptr) err->set(Status::Io, "cannot rename: " + from);
            return false;
        }
        return true;
    }

    bool setComment(const std::string& comment, Error* err) override {
        if (!checkWritable(err)) return false;
        if (!z_->setComment(comment)) {
            if (err != nullptr) err->set(Status::Io, "cannot set comment");
            return false;
        }
        comment_ = comment;
        commentSet_ = true;
        return true;
    }

    bool close(bool discard, Error* err) override {
        if (closed_) return true;
        closed_ = true;
        if (discard) {
            z_->discard();
            return true;
        }
        if (!commentSet_) comment_ = z_->getComment();
        if (z_->close() != LIBZIPPP_OK) {
            if (err != nullptr) err->set(Status::Io, "cannot write archive");
            // The handle is poisoned after a failed commit (~ZipArchive would
            // retry zip_close on it -> double-close crash). Discard it here
            // so destruction is always safe.
            z_->discard();
            return false;
        }
        // libzip deletes the file when a commit leaves zero entries
        // (zip_close.c: "don't create zip files with no entries"). An emptied
        // archive must stay a valid empty zip, so recreate it, preserving the
        // comment. Same for a fresh session committed with no entries.
        // Found by device testing: delete-last-entry + commit made m.zip
        // vanish, and the reopen failed with "not a supported archive".
        struct stat st {};
        if (::stat(path_.c_str(), &st) != 0) {
            if (!writeEmptyZip(path_, comment_, err)) return false;
        }
        return true;
    }

   private:
    bool checkOpen(Error* err) {
        if (closed_) {
            if (err != nullptr) err->set(Status::Io, "session is closed");
            return false;
        }
        return true;
    }
    bool checkWritable(Error* err) {
        if (!checkOpen(err)) return false;
        if (!writable_) {
            if (err != nullptr) {
                err->set(Status::UnsupportedOperation, "session is read-only");
            }
            return false;
        }
        return true;
    }
    static bool requireSafe(const std::string& name, Error* err) {
        if (!isSafeEntryName(name)) {
            if (err != nullptr) err->set(Status::UnsafeName, "unsafe entry name: " + name);
            return false;
        }
        return true;
    }

    // Mirrors bin.nt.aae.AaeCapabilities.validateWrite: never trust Java.
    // Ranges come from libzip's algorithm files (deflate/bzip2/xz/zstd).
    static bool checkWriteOptions(const WriteOptions& opt, Error* err) {
        auto fail = [&](const std::string& m) {
            if (err != nullptr) err->set(Status::UnsupportedOperation, m);
            return false;
        };
        switch (opt.method) {
            case 0:  // DEFAULT
                if (opt.level != 0) return fail("DEFAULT supports only level 0");
                break;
            case 1:  // STORE
                if (opt.level != 0) return fail("STORE supports only level 0 (no compression)");
                break;
            case 2:  // DEFLATE
                if (opt.level < 0 || opt.level > 9)
                    return fail("DEFLATE level must be 0..9");
                break;
            case 3:  // BZIP2
#ifndef LIBZIPPP_USE_BZIP2
                return fail("BZIP2 not compiled in");
#else
                if (opt.level < 0 || opt.level > 9)
                    return fail("BZIP2 level must be 0..9");
                break;
#endif
            case 4:  // XZ
#ifndef LIBZIPPP_USE_XZ
                return fail("XZ not compiled in");
#else
                if (opt.level < 0 || opt.level > 9)
                    return fail("XZ level must be 0..9 (liblzma preset)");
                break;
#endif
            case 5:  // ZSTD
#ifndef LIBZIPPP_USE_ZSTD
                return fail("ZSTD not compiled in");
#else
                if (opt.level < 0 || opt.level > 22)
                    return fail("ZSTD level must be 0..22 (0 = default)");
                break;
#endif
            default:
                return fail("unsupported compression method");
        }
        if (opt.encryption < 0 || opt.encryption > 2)
            return fail("unsupported encryption slot");
#ifndef LIBZIPPP_WITH_ENCRYPTION
        if (opt.encryption != 0) return fail("encryption not compiled in (mbedtls slot empty)");
#endif
        return true;
    }

    // mkdir -p. Returns true if path exists as a dir afterwards.
    static bool mkdirs(const std::string& path) {
        if (path.empty()) return false;
        struct stat st {};
        if (::stat(path.c_str(), &st) == 0) return S_ISDIR(st.st_mode);
        const size_t slash = path.find_last_of('/');
        if (slash != std::string::npos && slash > 0) {
            if (!mkdirs(path.substr(0, slash))) return false;
        }
        if (::mkdir(path.c_str(), 0755) != 0 && errno != EEXIST) return false;
        return ::stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
    }

    // Writes a valid empty zip (EOCD only, optional comment). Used when a
    // commit leaves zero entries, which libzip answers by deleting the file.
    static bool writeEmptyZip(const std::string& path, const std::string& comment, Error* err) {
        const std::string c = comment.size() > 65535 ? comment.substr(0, 65535) : comment;
        unsigned char eocd[22] = {0x50, 0x4b, 0x05, 0x06};
        eocd[20] = static_cast<unsigned char>(c.size() & 0xFF);
        eocd[21] = static_cast<unsigned char>((c.size() >> 8) & 0xFF);
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if (!out) {
            if (err != nullptr) err->set(Status::Io, "cannot write empty archive: " + path);
            return false;
        }
        out.write(reinterpret_cast<const char*>(eocd), sizeof(eocd));
        if (!c.empty()) out.write(c.data(), static_cast<std::streamsize>(c.size()));
        out.close();
        if (!out) {
            if (err != nullptr) err->set(Status::Io, "cannot write empty archive: " + path);
            return false;
        }
        return true;
    }

    ZipArchive* z_;
    std::string path_;
    bool writable_;
    bool closed_ = false;
    std::string password_;
    std::string comment_;
    bool commentSet_ = false;
};

ISession* zipOpen(const std::string& fsPath, const WriteOptions& opt, bool writable, bool fresh,
                  Error* err) {
    if (!fresh) {
        struct stat st {};
        if (::stat(fsPath.c_str(), &st) != 0) {
            if (err != nullptr) err->set(Status::Io, "no such file: " + fsPath);
            return nullptr;
        }
    }
    ZipArchive* z = new ZipArchive(fsPath, opt.password, fromWireEncryption(opt.encryption));
    const ZipArchive::OpenMode mode =
        fresh ? ZipArchive::New : (writable ? ZipArchive::Write : ZipArchive::ReadOnly);
    if (!z->open(mode)) {
        delete z;
        if (err != nullptr) {
            if (fresh) {
                err->set(Status::Io, "cannot create archive: " + fsPath);
            } else {
                err->set(Status::NotArchive, "not a zip archive: " + fsPath);
            }
        }
        return nullptr;
    }
    return new ZipSession(z, fsPath, writable || fresh, opt.password);
}

struct Registrar {
    Registrar() { registerBackend(Backend::Zip, "zip", probeZip, zipOpen); }
};
Registrar sRegistrar;  // NOLINT: self-registration on load

#else  // !AAE_HAVE_LIBZIPPP

ISession* zipOpen(const std::string& fsPath, const WriteOptions& opt, bool writable, bool fresh,
                  Error* err) {
    (void)fsPath;
    (void)opt;
    (void)writable;
    (void)fresh;
    if (err != nullptr) {
        err->set(Status::UnsupportedFormat, "zip backend not compiled in (libzippp slot empty)");
    }
    return nullptr;
}

struct Registrar {
    Registrar() { registerBackend(Backend::Zip, "zip", probeZip, zipOpen); }
};
Registrar sRegistrar;  // NOLINT: self-registration on load

#endif  // AAE_HAVE_LIBZIPPP
}  // namespace zip
}  // namespace aae

// NOTE: libzip includes stay inside this file so the core never depends
// on third-party headers.
