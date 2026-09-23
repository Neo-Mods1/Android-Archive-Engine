// Wide-range read backend over thirdparty/libarchive (RAR/RAR5, CAB,
// ISO9660). Enabled with AAE_HAVE_LIBARCHIVE=1 (off by default); without
// it this file registers a null opener so dispatch reports
// UNSUPPORTED_FORMAT instead of crashing.
//
// Design notes (libarchive reads are forward-only — there is no rewind):
// every list/info/read/extract opens a FRESH archive handle, walks it once
// and frees it. Slightly more parsing per op, but no stale-handle bugs and
// no cross-op state. Write ops stay UNSUPPORTED_OPERATION until the
// libarchive write phase (archive_write_*); the ZIP backend remains the
// only writer, so fresh-create dispatch is untouched.
//
// Name normalization: CAB stores backslash separators and some archives
// store leading "./" or "/". fillEntry normalizes ('\\' -> '/', strip
// leading '/' and "./" segments) so listed names always satisfy
// isSafeEntryName and round-trip through extractTo exactly.
// Symlinks are reported as (empty) files; extracting one materializes an
// empty file rather than a link — safe on Android, documented.
// Per-entry `encrypted` is always false: the backend exposes only the
// archive-level flag (see info()); wire-format per-entry crypto bits vary
// too much to report reliably here. A set session password IS honored via
// archive_read_add_passphrase, so encrypted RARs open when unlocked.
#include "Aae.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <vector>

#ifdef AAE_HAVE_LIBARCHIVE
extern "C" {
#include <archive.h>
#include <archive_entry.h>
}
#endif

namespace aae {
namespace archive {

namespace {

// Magic sniffing mirrors libarchive's own bid checks:
// - RAR4/RAR5 share "Rar!\x1A\x07" (rar5 adds 0x01 0x00 after).
// - CAB: "MSCF" at offset 0 (plain .cab; self-extractors match later and
//   are deliberately NOT claimed here).
// - ISO9660: "CD001" at 32769 (PVD/SVD standard identifier, ECMA-119).
bool readAt(const std::string& fsPath, long off, unsigned char* buf, size_t want) {
    FILE* f = std::fopen(fsPath.c_str(), "rb");
    if (f == nullptr) return false;
    bool ok = std::fseek(f, off, SEEK_SET) == 0 &&
              std::fread(buf, 1, want, f) == want;
    std::fclose(f);
    return ok;
}

}  // namespace

std::string probeArchive(const std::string& fsPath) {
#ifdef AAE_HAVE_LIBARCHIVE
    unsigned char sig[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    if (!readAt(fsPath, 0, sig, sizeof(sig))) return "";
    if (sig[0] == 'R' && sig[1] == 'a' && sig[2] == 'r' && sig[3] == '!' &&
        sig[4] == 0x1A && sig[5] == 0x07) {
        return "rar";
    }
    if (sig[0] == 'M' && sig[1] == 'S' && sig[2] == 'C' && sig[3] == 'F') {
        return "cab";
    }
    unsigned char iso[5] = {0, 0, 0, 0, 0};
    if (readAt(fsPath, 32769, iso, sizeof(iso)) && std::memcmp(iso, "CD001", 5) == 0) {
        return "iso";
    }
    return "";
#else
    (void)fsPath;
    return "";
#endif
}

#ifdef AAE_HAVE_LIBARCHIVE

namespace {

// libarchive signals password problems with "passphrase"/"password" in the
// error text (exact wording varies by format reader). Heuristic, but it is
// the only signal the API offers; open/read paths document it.
bool mentionsPassword(const std::string& m) {
    return m.find("passphrase") != std::string::npos ||
           m.find("Passphrase") != std::string::npos ||
           m.find("password") != std::string::npos ||
           m.find("Password") != std::string::npos;
}

// Some readers report encrypted content without naming passwords, e.g.
// "Reading encrypted data is not currently supported" (RAR builds without
// crypto). Treated as password-required only when no password was given;
// with a password set the backend truly cannot decrypt, so the original
// message is kept instead of misreporting PasswordWrong.
bool mentionsEncrypted(const std::string& m) {
    return m.find("encrypt") != std::string::npos ||
           m.find("Encrypt") != std::string::npos;
}

std::string archiveMessage(struct archive* a) {
    const char* e = (a != nullptr) ? archive_error_string(a) : nullptr;
    return (e != nullptr) ? e : "";
}

// Archive-relative write name: mirrors isSafeEntryName + CAB normalization.
std::string normalizeName(const char* raw) {
    std::string s = (raw != nullptr) ? raw : "";
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\') s[i] = '/';
    }
    while (!s.empty() && s[0] == '/') s.erase(0, 1);
    while (s.compare(0, 2, "./") == 0) s.erase(0, 2);
    return s;
}

bool mkdirs(const std::string& path) {
    if (path.empty()) return false;
    struct stat st {};
    if (::stat(path.c_str(), &st) == 0) return S_ISDIR(st.st_mode) != 0;
    size_t slash = path.find_last_of('/');
    if (slash != std::string::npos && slash > 0) {
        if (!mkdirs(path.substr(0, slash))) return false;
    }
    if (::mkdir(path.c_str(), 0755) != 0 && errno != EEXIST) return false;
    return ::stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode) != 0;
}

}  // namespace

class LibarchiveSession : public ISession {
   public:
    LibarchiveSession(std::string path, std::string format, std::string password)
        : path_(std::move(path)), format_(std::move(format)), password_(std::move(password)) {}

    bool open(Error* err) {
        struct archive* a = newReader(err);
        if (a == nullptr) return false;
        archive_read_close(a);
        archive_read_free(a);
        opened_ = true;
        return true;
    }

    bool list(std::vector<Entry>& out, Error* err) override {
        if (!checkOpen(err)) return false;
        struct archive* a = newReader(err);
        if (a == nullptr) return false;
        bool ok = walk(a, err, [&](struct archive_entry* e) {
            Entry o;
            if (!fillEntry(e, o)) return true;  // skip nameless entries
            out.push_back(o);
            return true;
        });
        archive_read_close(a);
        archive_read_free(a);
        return ok;
    }

    bool info(ArchiveInfo& out, Error* err) override {
        if (!checkOpen(err)) return false;
        struct archive* a = newReader(err);
        if (a == nullptr) return false;
        out.format = format_;
        bool ok = walk(a, err, [&](struct archive_entry* e) {
            Entry o;
            if (!fillEntry(e, o)) return true;
            ++out.entryCount;
            if (!o.isDir && o.size > 0) out.totalSize += o.size;
            return true;
        });
        if (ok) {
            out.totalPacked = -1;  // per-file packed size not reported
            out.hasEncrypted = archive_read_has_encrypted_entries(a) == 1;
        }
        archive_read_close(a);
        archive_read_free(a);
        return ok;
    }

    bool readBytes(const std::string& entry, long long maxBytes, std::vector<char>& out,
                   Error* err) override {
        if (!checkOpen(err)) return false;
        const std::string want = normalizeName(entry.c_str());
        if (!isSafeEntryName(want)) {
            if (err != nullptr) err->set(Status::UnsafeName, "unsafe entry name: " + entry);
            return false;
        }
        struct archive* a = newReader(err);
        if (a == nullptr) return false;
        struct archive_entry* e = nullptr;
        int r = ARCHIVE_OK;
        bool found = false, ok = false;
        while ((r = archive_read_next_header(a, &e)) == ARCHIVE_OK) {
            Entry o;
            if (!fillEntry(e, o)) {
                archive_read_data_skip(a);
                continue;
            }
            if (o.path != want) {
                archive_read_data_skip(a);
                continue;
            }
            found = true;
            if (o.isDir) {
                if (err != nullptr) err->set(Status::Io, "not a file: " + entry);
                break;
            }
            if (o.size > maxBytes) {
                if (err != nullptr) {
                    err->set(Status::TooLarge, "entry larger than limit: " + entry);
                }
                break;
            }
            ok = drain(a, out, maxBytes, nullptr, entry, err);
            break;
        }
        if (!found && r == ARCHIVE_EOF && err != nullptr) {
            err->set(Status::NotFound, "no such entry: " + entry);
        } else if (!found && (r != ARCHIVE_OK || !ok) && err != nullptr && err->ok()) {
            failArchive(a, err, Status::Io, "cannot read entry: " + entry);
        }
        archive_read_close(a);
        archive_read_free(a);
        return found && ok;
    }

    bool extractTo(const std::string& entry, const std::string& outPath,
                   ProgressSink* progress, Error* err) override {
        if (!checkOpen(err)) return false;
        const std::string want = normalizeName(entry.c_str());
        if (!isSafeEntryName(want)) {
            if (err != nullptr) err->set(Status::UnsafeName, "unsafe entry name: " + entry);
            return false;
        }
        struct archive* a = newReader(err);
        if (a == nullptr) return false;
        struct archive_entry* e = nullptr;
        int r = ARCHIVE_OK;
        bool found = false, ok = false;
        while ((r = archive_read_next_header(a, &e)) == ARCHIVE_OK) {
            Entry o;
            if (!fillEntry(e, o)) {
                archive_read_data_skip(a);
                continue;
            }
            if (o.path != want) {
                archive_read_data_skip(a);
                continue;
            }
            found = true;
            if (o.isDir) {
                ok = mkdirs(outPath);
                if (!ok && err != nullptr) {
                    err->set(Status::Io, "cannot create dir: " + outPath + ": " +
                                             std::strerror(errno));
                }
                break;
            }
            FILE* out = std::fopen(outPath.c_str(), "wb");
            if (out == nullptr) {
                if (err != nullptr) err->set(Status::Io, "cannot write: " + outPath);
                break;
            }
            ok = pump(a, out, o.size, progress, entry, err);
            std::fclose(out);
            if (!ok) std::remove(outPath.c_str());  // no partial files
            break;
        }
        if (!found && r == ARCHIVE_EOF && err != nullptr) {
            err->set(Status::NotFound, "no such entry: " + entry);
        } else if (!found && err != nullptr && err->ok()) {
            failArchive(a, err, Status::Io, "extract failed: " + entry);
        }
        archive_read_close(a);
        archive_read_free(a);
        return found && ok;
    }

    bool addFile(const std::string&, const std::string&, const WriteOptions&,
                 Error* err) override {
        return readonly(err);
    }
    bool addDirectory(const std::string&, Error* err) override { return readonly(err); }
    bool remove(const std::string&, Error* err) override { return readonly(err); }
    bool rename(const std::string&, const std::string&, Error* err) override {
        return readonly(err);
    }
    bool setComment(const std::string&, Error* err) override { return readonly(err); }

    bool close(bool, Error*) override {
        opened_ = false;  // read-only: no persistent handle, nothing to commit
        return true;
    }

   private:
    // Opens a fresh handle positioned before the first header. Forward-only:
    // callers walk it once, then close+free it.
    struct archive* newReader(Error* err) {
        struct archive* a = archive_read_new();
        if (a == nullptr) {
            if (err != nullptr) err->set(Status::Io, "out of memory");
            return nullptr;
        }
        archive_read_support_filter_all(a);
        archive_read_support_format_rar(a);
        archive_read_support_format_rar5(a);
        archive_read_support_format_cab(a);
        archive_read_support_format_iso9660(a);
        if (!password_.empty()) archive_read_add_passphrase(a, password_.c_str());
        if (archive_read_open_filename(a, path_.c_str(), 10240) != ARCHIVE_OK) {
            failArchive(a, err, Status::Io, "cannot open: " + path_);
            archive_read_free(a);  // free closes a failed-open handle
            return nullptr;
        }
        return a;
    }

    void failArchive(struct archive* a, Error* err, Status def, const std::string& ctx) {
        if (err == nullptr) return;
        std::string m = archiveMessage(a);
        if (mentionsPassword(m)) {
            if (password_.empty()) {
                err->set(Status::PasswordRequired, "password required: " + ctx);
            } else {
                err->set(Status::PasswordWrong, "wrong password: " + ctx);
            }
            return;
        }
        if (password_.empty() && mentionsEncrypted(m)) {
            err->set(Status::PasswordRequired, "password required: " + ctx);
            return;
        }
        err->set(def, m.empty() ? ctx : ctx + " (" + m + ")");
    }

    // Walk every header; handler returns false to abort early (still OK).
    template <typename Fn>
    bool walk(struct archive* a, Error* err, Fn fn) {
        struct archive_entry* e = nullptr;
        int r = ARCHIVE_OK;
        while ((r = archive_read_next_header(a, &e)) == ARCHIVE_OK) {
            if (!fn(e)) return true;
        }
        if (r == ARCHIVE_EOF || r == ARCHIVE_WARN) return true;
        failArchive(a, err, Status::Io, "cannot list: " + path_);
        return false;
    }

    static bool fillEntry(struct archive_entry* e, Entry& out) {
        const char* name = archive_entry_pathname_utf8(e);
        if (name == nullptr) name = archive_entry_pathname(e);
        out.path = normalizeName(name);
        if (out.path.empty() || !isSafeEntryName(out.path)) return false;
        __LA_MODE_T ft = archive_entry_filetype(e);
        out.isDir = (ft == AE_IFDIR);
        long long size = static_cast<long long>(archive_entry_size(e));
        out.size = (size >= 0) ? size : 0;
        out.compressedSize = -1;
        out.method = -1;
        time_t mt = archive_entry_mtime(e);
        out.mtimeMillis = (mt > 0) ? static_cast<long long>(mt) * 1000LL : 0;
        out.crc = 0;
        out.encrypted = false;  // see file header note
        return true;
    }

    // Accumulates entry bytes (readBytes path). Caps at maxBytes.
    bool drain(struct archive* a, std::vector<char>& out, long long maxBytes,
               ProgressSink*, const std::string& entry, Error* err) {
        long long done = 0;
        while (true) {
            const void* buff = nullptr;
            size_t size = 0;
            la_int64_t offset = 0;
            int r = archive_read_data_block(a, &buff, &size, &offset);
            if (r == ARCHIVE_EOF) return true;
            if (r != ARCHIVE_OK) {
                failArchive(a, err, Status::Io, "cannot read entry: " + entry);
                return false;
            }
            if (done + static_cast<long long>(size) > maxBytes) {
                if (err != nullptr) {
                    err->set(Status::TooLarge, "entry larger than limit: " + entry);
                }
                return false;
            }
            const char* p = static_cast<const char*>(buff);
            out.insert(out.end(), p, p + size);
            done += static_cast<long long>(size);
        }
    }

    // Streams entry bytes to disk (extract path) with progress + cancel.
    bool pump(struct archive* a, FILE* out, long long total, ProgressSink* progress,
              const std::string& entry, Error* err) {
        long long done = 0;
        while (true) {
            const void* buff = nullptr;
            size_t size = 0;
            la_int64_t offset = 0;
            int r = archive_read_data_block(a, &buff, &size, &offset);
            if (r == ARCHIVE_EOF) break;
            if (r != ARCHIVE_OK) {
                failArchive(a, err, Status::Io, "extract failed: " + entry);
                return false;
            }
            if (size > 0 && std::fwrite(buff, 1, size, out) != size) {
                if (err != nullptr) err->set(Status::Io, "cannot write: " + entry);
                return false;
            }
            done += static_cast<long long>(size);
            if (progress != nullptr && !progress->poll(0, 1, done, total)) {
                if (err != nullptr) err->set(Status::Cancelled, "cancelled: " + entry);
                return false;
            }
        }
        if (progress != nullptr) progress->poll(0, 1, done, total > 0 ? total : done);
        return true;
    }

    bool checkOpen(Error* err) {
        if (!opened_) {
            if (err != nullptr) err->set(Status::Io, "session is closed");
            return false;
        }
        return true;
    }

    bool readonly(Error* err) {
        if (err != nullptr) {
            err->set(Status::UnsupportedOperation,
                     format_ + " is read-only (libarchive write lands in the next phase)");
        }
        return false;
    }

    std::string path_;
    std::string format_;
    std::string password_;
    bool opened_ = false;
};

ISession* archiveOpen(const std::string& fsPath, const WriteOptions& opt, bool, bool fresh,
                      Error* err) {
    if (fresh) {
        if (err != nullptr) {
            err->set(Status::UnsupportedFormat, "fresh-create goes to the ZIP backend");
        }
        return nullptr;  // registry never routes fresh here, but stay total
    }
    struct stat st {};
    if (::stat(fsPath.c_str(), &st) != 0) {
        if (err != nullptr) err->set(Status::Io, "no such file: " + fsPath);
        return nullptr;
    }
    LibarchiveSession* s = new LibarchiveSession(fsPath, probeArchive(fsPath), opt.password);
    if (!s->open(err)) {
        delete s;
        return nullptr;
    }
    return s;
}

struct Registrar {
    Registrar() { registerBackend(Backend::Archive, "rar", probeArchive, archiveOpen); }
};
Registrar sRegistrar;  // NOLINT: self-registration on load

#else  // !AAE_HAVE_LIBARCHIVE

ISession* archiveOpen(const std::string& fsPath, const WriteOptions& opt, bool, bool,
                      Error* err) {
    (void)fsPath;
    (void)opt;
    if (err != nullptr) {
        err->set(Status::UnsupportedFormat, "libarchive backend not compiled in");
    }
    return nullptr;
}

struct Registrar {
    Registrar() { registerBackend(Backend::Archive, "rar", probeArchive, archiveOpen); }
};
Registrar sRegistrar;  // NOLINT: self-registration on load

#endif  // AAE_HAVE_LIBARCHIVE
}  // namespace archive
}  // namespace aae
