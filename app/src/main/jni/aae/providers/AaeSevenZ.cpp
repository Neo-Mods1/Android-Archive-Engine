// 7z backend over the LZMA SDK C code (thirdparty/sevenzip, from the
// 7-Zip-JBinding tree). Read: list/info/read/extract via the C decoder
// (SevenZSession). Write: fresh create + in-place rewrite via the C LZMA
// encoder (LzmaEnc, single-threaded) and a minimal container writer
// (SevenZWriteSession below): non-solid, one folder per file, LZMA or Copy
// coders, unpacked header, real mtimes + attribs — the same shape p7zip
// emits for such archives (verified byte-for-byte patterns against 7za).
// No AES anywhere (the C tree has none): passwords are rejected up front,
// encrypted archives fail cleanly with a password status.
#include "Aae.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <vector>

extern "C" {
#include "7z.h"
#include "7zAlloc.h"
#include "7zCrc.h"
#include "7zFile.h"
#include "LzmaEnc.h"
}

namespace aae {
namespace sevenz {

namespace {

// SzAlloc/SzFree instances shared by all sessions (stateless).
ISzAlloc g_alloc = {SzAlloc, SzFree};
ISzAlloc g_allocTemp = {SzAllocTemp, SzFreeTemp};

// UTF-16LE (7z names) -> UTF-8. Ill-formed units become U+FFFD.
void utf16ToUtf8(const UInt16* src, size_t len, std::string& out) {
    static const char kRepl[] = "\xEF\xBF\xBD";
    out.clear();
    out.reserve(len);
    size_t i = 0;
    while (i < len) {
        UInt32 cp;
        UInt16 w = src[i++];
        if (w >= 0xD800 && w <= 0xDBFF && i < len) {
            UInt16 lo = src[i];
            if (lo >= 0xDC00 && lo <= 0xDFFF) {
                ++i;
                cp = 0x10000u + ((UInt32)(w - 0xD800) << 10) + (UInt32)(lo - 0xDC00);
            } else {
                cp = 0xFFFDu;
            }
        } else if (w >= 0xDC00 && w <= 0xDFFF) {
            cp = 0xFFFDu;
        } else {
            cp = w;
        }
        if (cp < 0x80) {
            out.push_back((char)cp);
        } else if (cp < 0x800) {
            out.push_back((char)(0xC0 | (cp >> 6)));
            out.push_back((char)(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out.push_back((char)(0xE0 | (cp >> 12)));
            out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back((char)(0x80 | (cp & 0x3F)));
        } else {
            out.push_back((char)(0xF0 | (cp >> 18)));
            out.push_back((char)(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back((char)(0x80 | (cp & 0x3F)));
        }
    }
    (void)kRepl;
}

const char* sresText(SRes r) {
    switch (r) {
        case SZ_OK:
            return "ok";
        case SZ_ERROR_DATA:
            return "data error";
        case SZ_ERROR_MEM:
            return "out of memory";
        case SZ_ERROR_CRC:
            return "crc error";
        case SZ_ERROR_UNSUPPORTED:
            return "unsupported feature";
        case SZ_ERROR_PARAM:
            return "bad parameter";
        case SZ_ERROR_INPUT_EOF:
            return "unexpected end of file";
        case SZ_ERROR_OUTPUT_EOF:
            return "output full";
        case SZ_ERROR_READ:
            return "read error";
        case SZ_ERROR_PROGRESS:
            return "interrupted";
        case SZ_ERROR_FAIL:
            return "failed";
        case SZ_ERROR_THREAD:
            return "thread error";
        case SZ_ERROR_ARCHIVE:
            return "archive error";
        case SZ_ERROR_NO_ARCHIVE:
            return "not an archive";
        default:
            return "unknown error";
    }
}

}  // namespace

class SevenZSession : public ISession {
   public:
    SevenZSession(const std::string& path, const std::string& password)
        : path_(path), password_(password) {
        SzArEx_Init(&db_);
    }

    bool open(Error* err) {
        CrcGenerateTable();
        FileInStream_CreateVTable(&stream_);
        if (InFile_Open(&stream_.file, path_.c_str()) != 0) {
            fail(err, Status::Io, "no such file: " + path_);
            return false;
        }
        fileOpen_ = true;
        LookToRead_CreateVTable(&look_, False);
        look_.realStream = &stream_.s;
        LookToRead_Init(&look_);
        SRes r = SzArEx_Open(&db_, &look_.s, &g_alloc, &g_allocTemp);
        if (r != SZ_OK) {
            // Encrypted headers surface as UNSUPPORTED here (the C decoder
            // has no AES); anything else is a broken/non archive.
            if (r == SZ_ERROR_UNSUPPORTED && err != nullptr) {
                if (password_.empty()) {
                    err->set(Status::PasswordRequired,
                             "password required (or unsupported 7z feature): " + path_);
                } else {
                    err->set(Status::PasswordWrong,
                             "cannot use password: encrypted 7z not supported yet: " + path_);
                }
            } else if (err != nullptr) {
                err->set(Status::NotArchive,
                         "not a 7z archive: " + path_ + " (" + sresText(r) + ")");
            }
            cleanup();
            return false;
        }
        opened_ = true;
        return true;
    }

    bool list(std::vector<Entry>& out, Error* err) override {
        if (!checkOpen(err)) return false;
        for (UInt32 i = 0; i < db_.NumFiles; ++i) {
            Entry e;
            if (!fillEntry(i, e, err)) return false;
            out.push_back(e);
        }
        return true;
    }

    bool info(ArchiveInfo& out, Error* err) override {
        if (!checkOpen(err)) return false;
        std::vector<Entry> entries;
        if (!list(entries, err)) return false;
        out.format = "7z";
        out.entryCount = (int)entries.size();
        for (const auto& e : entries) {
            if (!e.isDir) out.totalSize += e.size;
        }
        out.totalPacked = -1;  // solid blocks: per-file packed size unknown
        return true;
    }

    bool readBytes(const std::string& entry, long long maxBytes, std::vector<char>& out,
                   Error* err) override {
        if (!checkOpen(err)) return false;
        UInt32 index = 0;
        long long size = 0;
        if (!find(entry, &index, &size, err)) return false;
        if (size > maxBytes) {
            fail(err, Status::TooLarge, "entry larger than limit: " + entry);
            return false;
        }
        std::vector<Byte> buf;
        size_t got = 0;
        if (!extractFile(index, buf, &got, nullptr, err)) return false;
        out.assign((const char*)buf.data(), (const char*)buf.data() + got);
        return true;
    }

    bool extractTo(const std::string& entry, const std::string& outPath,
                   ProgressSink* progress, Error* err) override {
        if (!checkOpen(err)) return false;
        if (!isSafeEntryName(entry)) {
            fail(err, Status::UnsafeName, "unsafe entry name: " + entry);
            return false;
        }
        UInt32 index = 0;
        long long size = 0;
        bool isDir = false;
        if (!find(entry, &index, &size, err, &isDir)) return false;
        if (isDir) {
            if (!mkdirs(outPath)) {
                fail(err, Status::Io,
                     "cannot create dir: " + outPath + ": " + std::strerror(errno));
                return false;
            }
            return true;
        }
        if (progress != nullptr) {
            // Pre-flight poll only: solid-block decode is one native call
            // and cannot be interrupted mid-way, but a cancelled operation
            // must not start pointless work (found by device testing).
            if (!progress->poll(0, 1, 0, size)) {
                fail(err, Status::Cancelled, "cancelled: " + entry);
                return false;
            }
        }
        std::vector<Byte> buf;
        size_t got = 0;
        if (!extractFile(index, buf, &got, nullptr, err)) return false;
        FILE* f = std::fopen(outPath.c_str(), "wb");
        if (f == nullptr) {
            fail(err, Status::Io, "cannot write: " + outPath);
            return false;
        }
        bool ok = got == 0 || std::fwrite(buf.data(), 1, got, f) == got;
        std::fclose(f);
        if (!ok) {
            std::remove(outPath.c_str());
            fail(err, Status::Io, "extract failed: " + entry);
            return false;
        }
        if (progress != nullptr) progress->poll(0, 1, (long long)got, size);
        return true;
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
        opened_ = false;
        cleanup();
        return true;
    }

    ~SevenZSession() override { cleanup(); }

   private:
    static void fail(Error* err, Status s, const std::string& m) {
        if (err != nullptr) err->set(s, m);
    }
    void cleanup() {
        if (outBuffer_ != nullptr) {
            g_alloc.Free(&g_alloc, outBuffer_);
            outBuffer_ = nullptr;
            outBufferSize_ = 0;
        }
        if (opened_) {
            SzArEx_Free(&db_, &g_alloc);
            opened_ = false;
        }
        if (fileOpen_) {
            File_Close(&stream_.file);
            fileOpen_ = false;
        }
    }
    bool checkOpen(Error* err) {
        if (!opened_) {
            fail(err, Status::Io, "session is closed");
            return false;
        }
        return true;
    }
    bool readonly(Error* err) {
        fail(err, Status::UnsupportedOperation, "7z is read-only (for now)");
        return false;
    }
    bool fillEntry(UInt32 i, Entry& e, Error* err) {
        size_t nameLen = SzArEx_GetFileNameUtf16(&db_, i, nullptr);
        std::vector<UInt16> uname(nameLen);
        if (nameLen > 0) SzArEx_GetFileNameUtf16(&db_, i, uname.data());
        // Stored length excludes the trailing NUL.
        size_t actual = nameLen;
        while (actual > 0 && uname[actual - 1] == 0) --actual;
        utf16ToUtf8(uname.data(), actual, e.path);
        e.isDir = SzArEx_IsDir(&db_, i) != 0;
        e.size = e.isDir ? 0 : (long long)SzArEx_GetFileSize(&db_, i);
        // MTime when the archive stores one (lets rewrite keep real mtimes
        // instead of stamping everything with "now").
        e.mtimeMillis = 0;
        if (db_.MTime.Vals != nullptr && SzBitWithVals_Check(&db_.MTime, i)) {
            UInt64 ft = ((UInt64)db_.MTime.Vals[i].High << 32) | db_.MTime.Vals[i].Low;
            const UInt64 kUnixBias = 11644473600ULL;
            if (ft / 10000000ULL > kUnixBias) {
                e.mtimeMillis = (long long)(ft / 10000ULL - kUnixBias * 1000ULL);
            }
        }
        (void)err;
        return true;
    }
    bool find(const std::string& entry, UInt32* index, long long* size, Error* err,
              bool* isDir = nullptr) {
        for (UInt32 i = 0; i < db_.NumFiles; ++i) {
            Entry e;
            if (!fillEntry(i, e, err)) return false;
            if (e.path == entry) {
                *index = i;
                *size = e.size;
                if (isDir != nullptr) *isDir = e.isDir;
                return true;
            }
        }
        fail(err, Status::NotFound, "no such entry: " + entry);
        return false;
    }
    // Whole-file extract with solid-block cache.
    bool extractFile(UInt32 index, std::vector<Byte>& buf, size_t* got, ProgressSink*,
                     Error* err) {
        size_t offset = 0, processed = 0;
        SRes r = SzArEx_Extract(&db_, &look_.s, index, &blockIndex_, &outBuffer_,
                                &outBufferSize_, &offset, &processed, &g_alloc, &g_allocTemp);
        if (r == SZ_ERROR_UNSUPPORTED) {
            // Per-file AES (or exotic method): header opened fine, this file
            // needs crypto the C decoder lacks.
            if (err != nullptr) {
                if (password_.empty()) {
                    err->set(Status::PasswordRequired, "password required: encrypted entry");
                } else {
                    err->set(Status::PasswordWrong,
                             "cannot use password: encrypted 7z not supported yet");
                }
            }
            return false;
        }
        if (r != SZ_OK) {
            fail(err, Status::Io, std::string("extract failed (") + sresText(r) + ")");
            return false;
        }
        buf.assign(outBuffer_ + offset, outBuffer_ + offset + processed);
        *got = processed;
        return true;
    }
    static bool mkdirs(const std::string& path) {
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

    std::string path_;
    std::string password_;
    CSzArEx db_;
    CFileInStream stream_;
    CLookToRead look_;
    bool opened_ = false;
    bool fileOpen_ = false;
    UInt32 blockIndex_ = 0xFFFFFFFF;
    Byte* outBuffer_ = nullptr;
    size_t outBufferSize_ = 0;
};

// ---- 7z writer: fresh create + rewrite (LZMA / Copy, unpacked header) ----
// Container shape (non-solid, one folder per data file) mirrors what p7zip
// emits for the same content — byte patterns verified against real 7za
// output (see reference dumps in scratch, not in-tree). Deviations are
// deliberate simplifications, all reader-optional: no kDummy padding, no
// folder CRCs, MTime/attrib always fully defined, per-stream CRCs always
// present, empty archives omit kMainStreamsInfo (like 7za itself).
namespace szw {

int gTemp7z = 0;

void putNum(std::vector<Byte>& o, UInt64 v) {
    unsigned n = 0;
    UInt64 t = v >> 7;
    while (t != 0) {
        t >>= 8;
        ++n;
    }
    Byte first = (Byte)(n >= 8 ? 0xFF : (0xFF << (8 - n)) & 0xFF);
    if (n < 8) first |= (Byte)((v >> (8 * n)) & ((1u << (7 - n)) - 1));
    o.push_back(first);
    for (unsigned i = n; i-- > 0;) o.push_back((Byte)(v >> (8 * i)));
}

void putU32(std::vector<Byte>& o, UInt32 v) {
    for (int i = 0; i < 4; ++i) o.push_back((Byte)(v >> (8 * i)));
}

void putU64(std::vector<Byte>& o, UInt64 v) {
    for (int i = 0; i < 8; ++i) o.push_back((Byte)(v >> (8 * i)));
}

void putUtf16Char(std::vector<UInt16>& o, UInt32 cp) {
    if (cp < 0x10000) {
        o.push_back((UInt16)cp);
        return;
    }
    cp -= 0x10000;
    o.push_back((UInt16)(0xD800 + (cp >> 10)));
    o.push_back((UInt16)(0xDC00 + (cp & 0x3FF)));
}

// UTF-8 -> UTF-16 units (no NUL). Ill-formed input becomes U+FFFD;
// archive names arriving from Java are always valid UTF-8 anyway.
void utf8ToUtf16(const char* s, size_t n, std::vector<UInt16>& out) {
    out.clear();
    size_t i = 0;
    while (i < n) {
        unsigned char c = (unsigned char)s[i];
        UInt32 cp;
        size_t need;
        if (c < 0x80) {
            cp = c;
            need = 1;
        } else if ((c & 0xE0) == 0xC0) {
            cp = c & 0x1F;
            need = 2;
        } else if ((c & 0xF0) == 0xE0) {
            cp = c & 0x0F;
            need = 3;
        } else if ((c & 0xF8) == 0xF0) {
            cp = c & 0x07;
            need = 4;
        } else {
            putUtf16Char(out, 0xFFFD);
            ++i;
            continue;
        }
        bool ok = i + need <= n;
        for (size_t k = 1; ok && k < need; ++k) {
            unsigned char d = (unsigned char)s[i + k];
            if ((d & 0xC0) != 0x80) {
                ok = false;
                break;
            }
            cp = (cp << 6) | (d & 0x3F);
        }
        UInt32 lo = need == 1 ? 0 : need == 2 ? 0x80 : need == 3 ? 0x800 : 0x10000;
        if (!ok || cp < lo || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
            putUtf16Char(out, 0xFFFD);
            ++i;
            continue;
        }
        putUtf16Char(out, cp);
        i += need;
    }
}

UInt64 unixToFileTime(long long t) {
    if (t < 0) t = 0;
    return ((UInt64)t + 11644473600ULL) * 10000000ULL;
}

void ensureCrcTable() {
    static bool done = false;
    if (!done) {
        CrcGenerateTable();
        done = true;
    }
}

struct InSeq {
    ISeqInStream s;
    FILE* f;
    UInt32 crc;
    UInt64 size;
    bool readErr;
};

SRes inSeqRead(void* p, void* buf, size_t* size) {
    InSeq* s = (InSeq*)p;
    size_t n = std::fread(buf, 1, *size, s->f);
    if (n == 0 && std::ferror(s->f)) {
        s->readErr = true;
        return SZ_ERROR_READ;
    }
    s->crc = CrcUpdate(s->crc, buf, n);
    s->size += n;
    *size = n;
    return SZ_OK;
}

struct OutSeq {
    ISeqOutStream s;
    FILE* f;
    UInt64 size;
    bool ok;
};

size_t outSeqWrite(void* p, const void* buf, size_t size) {
    OutSeq* s = (OutSeq*)p;
    if (size == 0) return 0;
    size_t n = std::fwrite(buf, 1, size, s->f);
    if (n != size) s->ok = false;
    s->size += n;
    return n;
}

// LZMA-compress inPath, appended to out. No end marker (unpack size is
// stored in the container, like p7zip emits).
bool lzmaEncode(const std::string& inPath, FILE* out, int level, std::vector<Byte>& props,
                UInt32& crc, UInt64& unpackSize) {
    FILE* in = std::fopen(inPath.c_str(), "rb");
    if (in == nullptr) return false;
    if (level < 0) level = 5;
    if (level > 9) level = 9;
    CLzmaEncProps ep;
    LzmaEncProps_Init(&ep);
    ep.level = level;
    ep.writeEndMark = 0;
    ep.numThreads = 1;
    bool ok = false;
    crc = CRC_GET_DIGEST(CRC_INIT_VAL);
    unpackSize = 0;
    CLzmaEncHandle enc = LzmaEnc_Create(&g_alloc);
    if (enc != nullptr) {
        if (LzmaEnc_SetProps(enc, &ep) == SZ_OK) {
            Byte pb[5];
            SizeT pbSize = 5;
            if (LzmaEnc_WriteProperties(enc, pb, &pbSize) == SZ_OK && pbSize == 5) {
                props.assign(pb, pb + 5);
                InSeq iseq;
                iseq.s.Read = inSeqRead;
                iseq.f = in;
                iseq.crc = CRC_INIT_VAL;
                iseq.size = 0;
                iseq.readErr = false;
                OutSeq oseq;
                oseq.s.Write = outSeqWrite;
                oseq.f = out;
                oseq.size = 0;
                oseq.ok = true;
                SRes r = LzmaEnc_Encode(enc, &oseq.s, &iseq.s, nullptr, &g_alloc, &g_alloc);
                ok = (r == SZ_OK) && !iseq.readErr && oseq.ok;
                crc = CRC_GET_DIGEST(iseq.crc);
                unpackSize = iseq.size;
            }
        }
        LzmaEnc_Destroy(enc, &g_alloc, &g_alloc);
    }
    std::fclose(in);
    return ok;
}

bool copyWithCrc(const std::string& inPath, FILE* out, UInt32& crc, UInt64& size) {
    FILE* in = std::fopen(inPath.c_str(), "rb");
    if (in == nullptr) return false;
    crc = CRC_INIT_VAL;
    size = 0;
    Byte buf[65536];
    bool ok = true;
    while (true) {
        size_t n = std::fread(buf, 1, sizeof(buf), in);
        if (n == 0) {
            if (std::ferror(in)) ok = false;
            break;
        }
        if (std::fwrite(buf, 1, n, out) != n) {
            ok = false;
            break;
        }
        crc = CrcUpdate(crc, buf, n);
        size += n;
    }
    std::fclose(in);
    crc = CRC_GET_DIGEST(crc);
    return ok;
}

}  // namespace szw

std::string probe7z(const std::string& p) {    FILE* f = std::fopen(p.c_str(), "rb");
    if (f == nullptr) return "";
    unsigned char sig[6] = {0, 0, 0, 0, 0, 0};
    size_t n = std::fread(sig, 1, sizeof(sig), f);
    std::fclose(f);
    if (n < 6) return "";
    return (sig[0] == 0x37 && sig[1] == 0x7A && sig[2] == 0xBC && sig[3] == 0xAF &&
            sig[4] == 0x27 && sig[5] == 0x1C)
               ? "7z"
               : "";
}

struct Staged7z {
    std::string path;  // 7z names: '/' separators, never a trailing slash
    bool isDir = false;
    long long size = 0;
    long long mtime = 0;  // unix seconds
    std::string srcPath;  // files only (caller-owned, except rewrite temps)
    int method = 1;       // 0 = Copy/STORE, 1 = LZMA
};

struct Coded7z {
    size_t stagedIdx = 0;
    std::vector<Byte> props;  // LZMA coder props (empty for Copy)
    UInt64 packSize = 0;
    UInt32 crc = 0;
    UInt64 unpackSize = 0;
};

class SevenZWriteSession : public ISession {
   public:
    explicit SevenZWriteSession(const std::string& path) : path_(path) {}

    bool openForWrite(bool fresh, Error* err) {
        struct stat pst {};
        if (::stat(path_.c_str(), &pst) != 0 && !fresh) {
            fail(err, Status::Io, "no such file: " + path_);
            return false;
        }
        if (!fresh) {
            // Rewrite: decode everything to session temps up front (the C
            // decoder only extracts whole files), then rebuild on commit.
            // Files the user never touches re-encode identically enough;
            // an untouched session commits byte-identically (see close).
            SevenZSession r(path_, "");
            if (!r.open(err)) return false;
            std::vector<Entry> entries;
            if (!r.list(entries, err)) {
                r.close(true, nullptr);
                return false;
            }
            char tmp[64];
            std::snprintf(tmp, sizeof(tmp), ".aae7zr.%d", ++szw::gTemp7z);
            loadTmpDir_ = path_ + tmp;
            if (::mkdir(loadTmpDir_.c_str(), 0755) != 0) {
                r.close(true, nullptr);
                fail(err, Status::Io, "cannot write temp: " + loadTmpDir_);
                return false;
            }
            for (size_t i = 0; i < entries.size(); ++i) {
                const Entry& e = entries[i];
                Staged7z s;
                s.path = e.path;
                s.isDir = e.isDir;
                s.size = e.size;
                s.mtime = e.mtimeMillis / 1000;
                if (!e.isDir) {
                    char fn[32];
                    std::snprintf(fn, sizeof(fn), "f%u", (unsigned)i);
                    s.srcPath = loadTmpDir_ + "/" + fn;
                    if (!r.extractTo(e.path, s.srcPath, nullptr, err)) {
                        r.close(true, nullptr);
                        cleanup();
                        return false;
                    }
                }
                staged_.push_back(s);
            }
            r.close(true, nullptr);
        }
        fresh_ = fresh;
        opened_ = true;
        return true;
    }

    bool list(std::vector<Entry>& out, Error* err) override {
        if (!checkOpen(err)) return false;
        for (const auto& s : staged_) {
            Entry e;
            e.path = s.path;
            e.isDir = s.isDir;
            e.size = s.size;
            e.mtimeMillis = s.mtime * 1000LL;
            out.push_back(e);
        }
        return true;
    }

    bool info(ArchiveInfo& out, Error* err) override {
        if (!checkOpen(err)) return false;
        out.format = "7z";
        out.entryCount = (int)staged_.size();
        for (const auto& s : staged_) {
            if (!s.isDir) {
                out.totalSize += s.size;
                out.totalPacked += s.size;
            }
        }
        return true;
    }

    bool readBytes(const std::string& entry, long long maxBytes, std::vector<char>& out,
                   Error* err) override {
        if (!checkOpen(err)) return false;
        if (!isSafeEntryName(entry)) {
            fail(err, Status::UnsafeName, "unsafe entry name: " + entry);
            return false;
        }
        for (const auto& s : staged_) {
            if (!s.isDir && s.path == entry) {
                if (s.size > maxBytes) {
                    fail(err, Status::TooLarge, "entry larger than limit: " + entry);
                    return false;
                }
                FILE* f = std::fopen(s.srcPath.c_str(), "rb");
                if (f == nullptr) {
                    fail(err, Status::Io, "cannot read: " + s.srcPath);
                    return false;
                }
                out.resize((size_t)s.size);
                bool ok = s.size == 0 ||
                          std::fread(out.data(), 1, (size_t)s.size, f) == (size_t)s.size;
                std::fclose(f);
                if (!ok) fail(err, Status::Io, "cannot read entry: " + entry);
                return ok;
            }
        }
        fail(err, Status::NotFound, "no such entry: " + entry);
        return false;
    }

    bool extractTo(const std::string& entry, const std::string& outPath,
                   ProgressSink* progress, Error* err) override {
        if (!checkOpen(err)) return false;
        if (!isSafeEntryName(entry)) {
            fail(err, Status::UnsafeName, "unsafe entry name: " + entry);
            return false;
        }
        for (const auto& s : staged_) {
            if (s.path != entry && s.path != entry + "/") continue;
            if (s.isDir) {
                if (!mkdirs(outPath)) {
                    fail(err, Status::Io,
                         "cannot create dir: " + outPath + ": " + std::strerror(errno));
                    return false;
                }
                return true;
            }
            FILE* in = std::fopen(s.srcPath.c_str(), "rb");
            if (in == nullptr) {
                fail(err, Status::Io, "cannot read: " + s.srcPath);
                return false;
            }
            FILE* out = std::fopen(outPath.c_str(), "wb");
            if (out == nullptr) {
                std::fclose(in);
                fail(err, Status::Io, "cannot write: " + outPath);
                return false;
            }
            Byte buf[65536];
            long long done = 0;
            bool cancelled = false, ioErr = false;
            while (done < s.size) {
                size_t want = (size_t)((s.size - done > (long long)sizeof(buf))
                                           ? sizeof(buf)
                                           : (size_t)(s.size - done));
                size_t n = std::fread(buf, 1, want, in);
                if (n == 0) {
                    if (std::ferror(in)) ioErr = true;
                    break;
                }
                if (std::fwrite(buf, 1, n, out) != n) {
                    ioErr = true;
                    break;
                }
                done += (long long)n;
                if (progress != nullptr && !progress->poll(0, 1, done, s.size)) {
                    cancelled = true;
                    break;
                }
            }
            std::fclose(in);
            std::fclose(out);
            if (cancelled) {
                std::remove(outPath.c_str());
                fail(err, Status::Cancelled, "cancelled: " + entry);
                return false;
            }
            if (ioErr || done != s.size) {
                std::remove(outPath.c_str());
                fail(err, Status::Io, "extract failed: " + entry);
                return false;
            }
            if (progress != nullptr) progress->poll(0, 1, s.size, s.size);
            return true;
        }
        fail(err, Status::NotFound, "no such entry: " + entry);
        return false;
    }

    bool addFile(const std::string& entryName, const std::string& srcPath,
                 const WriteOptions& opt, Error* err) override {
        if (!checkOpen(err)) return false;
        std::string clean = entryName;
        while (!clean.empty() && clean.back() == '/') clean.pop_back();
        if (!isSafeEntryName(clean)) {
            fail(err, Status::UnsafeName, "unsafe entry name: " + entryName);
            return false;
        }
        if (!levelSet_) {
            if (!checkLevel(opt, err)) return false;
            level_ = opt.level;
            levelSet_ = true;
        }
        struct stat st {};
        if (::stat(srcPath.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) {
            fail(err, Status::Io, "cannot read input: " + srcPath);
            return false;
        }
        Staged7z s;
        s.path = clean;
        s.size = st.st_size;
        s.mtime = st.st_mtime;
        s.srcPath = srcPath;
        s.method = (opt.method == 1 /*STORE*/) ? 0 : 1;
        stageReplace(s);
        dirty_ = true;
        return true;
    }

    bool addDirectory(const std::string& entryName, Error* err) override {
        if (!checkOpen(err)) return false;
        std::string clean = entryName;
        while (!clean.empty() && clean.back() == '/') clean.pop_back();
        if (!isSafeEntryName(clean)) {
            fail(err, Status::UnsafeName, "unsafe entry name: " + entryName);
            return false;
        }
        Staged7z s;
        s.path = clean;
        s.isDir = true;
        s.mtime = std::time(nullptr);
        stageReplace(s);
        dirty_ = true;
        return true;
    }

    bool remove(const std::string& entry, Error* err) override {
        if (!checkOpen(err)) return false;
        std::string base = entry;
        while (!base.empty() && base.back() == '/') base.pop_back();
        size_t before = staged_.size();
        for (size_t i = 0; i < staged_.size();) {
            const std::string& p = staged_[i].path;
            if (p == entry || p == base ||
                (p.size() > base.size() + 1 && p.compare(0, base.size() + 1, base + "/") == 0)) {
                staged_.erase(staged_.begin() + (ptrdiff_t)i);
            } else {
                ++i;
            }
        }
        if (staged_.size() == before) {
            fail(err, Status::NotFound, "no such entry: " + entry);
            return false;
        }
        dirty_ = true;
        return true;
    }

    bool rename(const std::string& from, const std::string& to, Error* err) override {
        if (!checkOpen(err)) return false;
        std::string f = from, t = to;
        while (!f.empty() && f.back() == '/') f.pop_back();
        while (!t.empty() && t.back() == '/') t.pop_back();
        if (!isSafeEntryName(t) || f.empty() || t.empty()) {
            fail(err, Status::UnsafeName, "unsafe entry name: " + to);
            return false;
        }
        auto exists = [&](const std::string& p) {
            for (const auto& s : staged_) {
                if (s.path == p) return true;
            }
            return false;
        };
        for (size_t i = 0; i < staged_.size(); ++i) {
            if (!staged_[i].isDir && staged_[i].path == f) {
                if (exists(t)) {
                    fail(err, Status::Io, "already exists: " + to);
                    return false;
                }
                Staged7z s = staged_[i];
                staged_.erase(staged_.begin() + (ptrdiff_t)i);
                s.path = t;
                stageReplace(s);
                dirty_ = true;
                return true;
            }
        }
        std::string fd = f + "/", td = t + "/";
        bool any = false;
        for (const auto& s : staged_) {
            if (s.path == f ||
                (s.path.size() > fd.size() && s.path.compare(0, fd.size(), fd) == 0)) {
                any = true;
                break;
            }
        }
        if (!any) {
            fail(err, Status::NotFound, "no such entry: " + from);
            return false;
        }
        for (const auto& s : staged_) {
            std::string np;
            if (s.path == f) {
                np = t;
            } else if (s.path.size() > fd.size() && s.path.compare(0, fd.size(), fd) == 0) {
                np = td + s.path.substr(fd.size());
            } else {
                continue;
            }
            if (exists(np) && np != s.path) {
                fail(err, Status::Io, "already exists: " + np);
                return false;
            }
        }
        for (auto& s : staged_) {
            if (s.path == f) {
                s.path = t;
            } else if (s.path.size() > fd.size() && s.path.compare(0, fd.size(), fd) == 0) {
                s.path = td + s.path.substr(fd.size());
            }
        }
        std::sort(staged_.begin(), staged_.end(),
                  [](const Staged7z& a, const Staged7z& b) { return a.path < b.path; });
        dirty_ = true;
        return true;
    }

    bool setComment(const std::string&, Error* err) override {
        fail(err, Status::UnsupportedOperation, "7z has no archive comment");
        return false;
    }

    bool close(bool discard, Error* err) override {
        if (discard) {
            opened_ = false;
            cleanup();
            return true;
        }
        if (!fresh_ && !dirty_) {
            opened_ = false;
            cleanup();
            return true;
        }
        bool ok = commit(err);
        opened_ = false;
        cleanup();
        return ok;
    }

    ~SevenZWriteSession() override { cleanup(); }

   private:
    static void fail(Error* err, Status s, const std::string& m) {
        if (err != nullptr) err->set(s, m);
    }
    void cleanup() {
        if (!loadTmpDir_.empty()) deleteRecursive(loadTmpDir_);
        loadTmpDir_.clear();
        if (!buildTmp_.empty()) std::remove(buildTmp_.c_str());
        buildTmp_.clear();
    }
    // Removes a session-owned rewrite temp dir (flat f0..fN files only).
    // NOTE: built with std::string, not a fixed char buffer — a truncated
    // buffer once collapsed every probe to the same existing directory
    // prefix and spun forever on deep paths (e.g. app-private dirs).
    static void deleteRecursive(const std::string& path) {
        for (unsigned i = 0;; ++i) {
            std::string fn = path + "/f" + std::to_string(i);
            struct stat st {};
            if (::stat(fn.c_str(), &st) != 0) break;
            std::remove(fn.c_str());
        }
        ::rmdir(path.c_str());
    }
    bool checkOpen(Error* err) {
        if (!opened_) {
            fail(err, Status::Io, "session is closed");
            return false;
        }
        return true;
    }
    // STORE must be level 0; LZMA takes presets 0..9 (Java sends 1/3/5/7/9).
    // The method wire int is Java-validated; only the level reaches the C
    // encoder (Copy ignores it).
    static bool checkLevel(const WriteOptions& opt, Error* err) {
        bool ok = opt.method == 1 /*STORE*/ ? opt.level == 0
                                            : (opt.level >= 0 && opt.level <= 9);
        if (!ok && err != nullptr) {
            err->set(Status::UnsupportedOperation,
                     "level " + std::to_string(opt.level) + " not supported by 7z");
        }
        return ok;
    }
    void stageReplace(Staged7z s) {
        for (auto& e : staged_) {
            if (e.path == s.path) {
                e = s;
                return;
            }
        }
        auto it = std::lower_bound(staged_.begin(), staged_.end(), s.path,
                                   [](const Staged7z& e, const std::string& v) {
                                       return e.path < v;
                                   });
        staged_.insert(it, s);
    }
    static bool anyOf(const std::vector<Byte>& v) {
        for (size_t i = 0; i < v.size(); ++i) {
            if (v[i] != 0) return true;
        }
        return false;
    }
    bool commit(Error* err) {
        using namespace szw;
        ensureCrcTable();
        char tmp[64];
        std::snprintf(tmp, sizeof(tmp), ".aae7zb.%d", ++gTemp7z);
        buildTmp_ = path_ + tmp;
        FILE* f = std::fopen(buildTmp_.c_str(), "wb");
        if (f == nullptr) {
            fail(err, Status::Io, "cannot write temp: " + buildTmp_);
            return false;
        }
        Byte zero[32] = {0};
        bool ok = std::fwrite(zero, 1, sizeof(zero), f) == sizeof(zero);
        std::vector<Coded7z> coded;
        if (ok) {
            for (size_t i = 0; ok && i < staged_.size(); ++i) {
                const Staged7z& s = staged_[i];
                if (s.isDir || s.size == 0) continue;  // empty streams need no folder
                Coded7z c;
                c.stagedIdx = i;
                long long start = std::ftell(f);
                if (start < 0) {
                    ok = false;
                    break;
                }
                if (s.method == 0) {
                    ok = copyWithCrc(s.srcPath, f, c.crc, c.unpackSize);
                } else {
                    ok = lzmaEncode(s.srcPath, f, level_, c.props, c.crc, c.unpackSize);
                }
                if (!ok) break;
                long long end = std::ftell(f);
                if (end < start) {
                    ok = false;
                    break;
                }
                c.packSize = (UInt64)(end - start);
                if (c.packSize == 0) {
                    ok = false;  // a coder that emits nothing is broken, not empty
                    break;
                }
                coded.push_back(c);
            }
        }
        UInt64 dataSize = 0;
        if (ok) {
            long long end = std::ftell(f);
            ok = end >= 32;
            dataSize = ok ? (UInt64)(end - 32) : 0;
        }
        std::vector<Byte> hdr;
        if (ok) {
            buildHeader(hdr, coded);
            ok = std::fwrite(hdr.data(), 1, hdr.size(), f) == hdr.size();
        }
        if (ok) {
            // Patch the signature header now that both sizes are known.
            Byte sig[32] = {0x37, 0x7A, 0xBC, 0xAF, 0x27, 0x1C, 0x00, 0x04};
            for (int i = 0; i < 8; ++i) sig[12 + i] = (Byte)(dataSize >> (8 * i));
            for (size_t i = 0; i < 8; ++i) sig[20 + i] = (Byte)(((UInt64)hdr.size()) >> (8 * i));
            // NOTE: StartHeaderCRC covers bytes 12..31 *including* the
            // NextHeaderCRC field, so the header CRC must be placed first.
            UInt32 hcrc = CrcCalc(hdr.data(), hdr.size());
            for (int i = 0; i < 4; ++i) sig[28 + i] = (Byte)(hcrc >> (8 * i));
            UInt32 scrc = CrcCalc(sig + 12, 20);
            for (int i = 0; i < 4; ++i) sig[8 + i] = (Byte)(scrc >> (8 * i));
            ok = std::fseek(f, 0, SEEK_SET) == 0 && std::fwrite(sig, 1, sizeof(sig), f) ==
                                                              sizeof(sig);
        }
        if (std::fclose(f) != 0) ok = false;
        if (!ok) {
            fail(err, Status::Io, "cannot build 7z image: " + path_);
            return false;
        }
        if (std::rename(buildTmp_.c_str(), path_.c_str()) != 0) {
            fail(err, Status::Io, "cannot replace: " + path_ + ": " + std::strerror(errno));
            return false;
        }
        buildTmp_.clear();
        return true;
    }
    // Container header. dataFiles order == folder/pack-stream order; the
    // FilesInfo lists every entry (dirs + empties + files) sorted together.
    void buildHeader(std::vector<Byte>& h, const std::vector<Coded7z>& coded) {
        using namespace szw;
        h.push_back(0x01);  // kHeader
        if (!coded.empty()) {
            h.push_back(0x04);  // kMainStreamsInfo
            h.push_back(0x06);  // kPackInfo
            putNum(h, 0);       // PackPos: data follows the signature header
            putNum(h, coded.size());
            h.push_back(0x09);  // kSize
            for (size_t i = 0; i < coded.size(); ++i) putNum(h, coded[i].packSize);
            h.push_back(0x00);
            h.push_back(0x07);  // kUnpackInfo
            h.push_back(0x0B);  // kFolder
            putNum(h, coded.size());
            h.push_back(0x00);  // external = folders inline
            for (size_t i = 0; i < coded.size(); ++i) {
                const Staged7z& s = staged_[coded[i].stagedIdx];
                h.push_back(0x01);  // NumCoders
                if (s.method == 0) {
                    h.push_back(0x01);  // flags: 1-byte id
                    h.push_back(0x00);  // Copy
                } else {
                    h.push_back(0x23);  // flags: 3-byte id + props
                    h.push_back(0x03);
                    h.push_back(0x01);
                    h.push_back(0x01);  // LZMA
                    putNum(h, coded[i].props.size());
                    h.insert(h.end(), coded[i].props.begin(), coded[i].props.end());
                }
                // Single-coder folders imply 0 binds + packed index == order.
            }
            h.push_back(0x0C);  // kCodersUnpackSize
            for (size_t i = 0; i < coded.size(); ++i) putNum(h, coded[i].unpackSize);
            h.push_back(0x00);
            h.push_back(0x08);  // kSubStreamsInfo (unpack CRCs only; every
            h.push_back(0x0A);  // folder holds exactly one stream)
            h.push_back(0x01);  // all CRCs defined
            for (size_t i = 0; i < coded.size(); ++i) putU32(h, coded[i].crc);
            h.push_back(0x00);
            h.push_back(0x00);
        }
        h.push_back(0x05);  // kFilesInfo
        putNum(h, staged_.size());
        std::vector<Byte> es(staged_.size(), 0), ef;
        for (size_t i = 0; i < staged_.size(); ++i) {
            if (staged_[i].isDir || staged_[i].size == 0) {
                es[i] = 1;
                ef.push_back(staged_[i].isDir ? 0 : 1);
            }
        }
        if (anyOf(es)) {
            h.push_back(0x0E);
            putNum(h, (es.size() + 7) / 8);
            for (size_t i = 0; i < (es.size() + 7) / 8; ++i) {
                Byte m = 0;
                for (unsigned b = 0; b < 8; ++b) {
                    size_t k = i * 8 + b;
                    if (k < es.size() && es[k]) m |= (Byte)(0x80 >> b);
                }
                h.push_back(m);
            }
        }
        if (anyOf(ef)) {
            h.push_back(0x0F);
            putNum(h, (ef.size() + 7) / 8);
            for (size_t i = 0; i < (ef.size() + 7) / 8; ++i) {
                Byte m = 0;
                for (unsigned b = 0; b < 8; ++b) {
                    size_t k = i * 8 + b;
                    if (k < ef.size() && ef[k]) m |= (Byte)(0x80 >> b);
                }
                h.push_back(m);
            }
        }
        h.push_back(0x11);  // kName
        std::vector<Byte> nb;
        for (size_t i = 0; i < staged_.size(); ++i) {
            std::vector<UInt16> u;
            utf8ToUtf16(staged_[i].path.c_str(), staged_[i].path.size(), u);
            for (size_t k = 0; k < u.size(); ++k) {
                nb.push_back((Byte)u[k]);
                nb.push_back((Byte)(u[k] >> 8));
            }
            nb.push_back(0);
            nb.push_back(0);
        }
        putNum(h, 1 + nb.size());
        h.push_back(0x00);  // external = names inline
        h.insert(h.end(), nb.begin(), nb.end());
        h.push_back(0x14);  // kMTime (fully defined, inline)
        putNum(h, 1 + 1 + 8 * staged_.size());
        h.push_back(0x01);
        h.push_back(0x00);
        for (size_t i = 0; i < staged_.size(); ++i) {
            putU64(h, unixToFileTime(staged_[i].mtime));
        }
        h.push_back(0x15);  // kWinAttrib (fully defined, inline)
        putNum(h, 1 + 1 + 4 * staged_.size());
        h.push_back(0x01);
        h.push_back(0x00);
        for (size_t i = 0; i < staged_.size(); ++i) {
            putU32(h, staged_[i].isDir ? ((UInt32)0x41ED << 16 | 0x10)
                                       : ((UInt32)0x81A4 << 16 | 0x20));
        }
        h.push_back(0x00);
        h.push_back(0x00);
    }
    static bool mkdirs(const std::string& path) {
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

    std::string path_;
    std::vector<Staged7z> staged_;
    std::string loadTmpDir_;
    std::string buildTmp_;
    bool opened_ = false;
    bool fresh_ = false;
    bool dirty_ = false;
    bool levelSet_ = false;
    int level_ = 5;  // 7z default; first addFile overrides
};

ISession* open7z(const std::string& p, const WriteOptions& opt, bool writable, bool fresh,
                 Error* e) {
    if (writable || fresh) {
        SevenZWriteSession* s = new SevenZWriteSession(p);
        if (!s->openForWrite(fresh, e)) {
            delete s;
            return nullptr;
        }
        return s;
    }
    SevenZSession* s = new SevenZSession(p, opt.password);
    if (!s->open(e)) {
        delete s;
        return nullptr;
    }
    return s;
}

struct Registrar {
    Registrar() { registerBackend(Backend::SevenZ, "7z", probe7z, open7z); }
};
Registrar sRegistrar;  // NOLINT: self-registration on load

}  // namespace sevenz
}  // namespace aae
