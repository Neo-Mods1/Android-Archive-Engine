// Stream backends: gzip, bzip2, xz, zstd and lz4 single-file streams plus
// tar containers (plain .tar or layered over any of those streams, e.g.
// .tar.gz/.tgz). Everything is streaming file->temp-file on open; tar
// entries are served as slices afterwards. No password concept here
// (these formats have none) — a non-empty session password is ignored.
#include "Aae.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <vector>

#include <zlib.h>

#include "bzlib.h"
#include "lz4frame.h"
#include "zstd.h"
#include "lzma.h"

namespace aae {
namespace stream {

enum class Kind { RawTar, Gzip, Bzip2, Xz, Zstd, Lz4 };

namespace {

const size_t kChunk = 65536;

bool readMagic(const std::string& path, unsigned char* buf, size_t want, size_t& got) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (f == nullptr) {
        got = 0;
        return false;
    }
    got = std::fread(buf, 1, want, f);
    std::fclose(f);
    return true;
}

bool hasTarMagicAt(const unsigned char* p) {
    return p[257] == 'u' && p[258] == 's' && p[259] == 't' && p[260] == 'a' &&
           p[261] == 'r';
}

// A run of zero blocks is how tar represents an empty archive (GNU tar
// lists it as zero entries). Magic sniffing alone can never see it, so the
// probe and both open paths accept an all-zero first block as an empty tar
// (files under 512 bytes still need the magic).
bool isZeroBlock(const unsigned char* p, size_t n) {
    if (n < 512) return false;
    for (size_t i = 0; i < 512; ++i) {
        if (p[i] != 0) return false;
    }
    return true;
}

bool isTarImage(const unsigned char* head, size_t n) {
    return (n >= 262 && hasTarMagicAt(head)) || isZeroBlock(head, n);
}

// --- decompressors: FILE* -> FILE*, true on clean stream end ---

bool gunzip(FILE* in, FILE* out) {
    z_stream strm{};
    if (inflateInit2(&strm, 16 + MAX_WBITS) != Z_OK) return false;
    std::vector<unsigned char> inb(kChunk), outb(kChunk);
    int ret = Z_OK;
    bool ok = false;
    while (ret != Z_STREAM_END) {
        strm.avail_in = (uInt)std::fread(inb.data(), 1, inb.size(), in);
        if (std::ferror(in)) break;
        if (strm.avail_in == 0) break;
        strm.next_in = inb.data();
        do {
            strm.avail_out = (uInt)outb.size();
            strm.next_out = outb.data();
            ret = inflate(&strm, Z_NO_FLUSH);
            if (ret == Z_STREAM_ERROR || ret == Z_DATA_ERROR || ret == Z_MEM_ERROR) break;
            if (std::fwrite(outb.data(), 1, outb.size() - strm.avail_out, out) !=
                outb.size() - strm.avail_out) {
                ret = Z_STREAM_ERROR;
                break;
            }
        } while (strm.avail_out == 0);
        if (ret == Z_STREAM_ERROR || ret == Z_DATA_ERROR || ret == Z_MEM_ERROR) break;
    }
    ok = (ret == Z_STREAM_END);
    inflateEnd(&strm);
    return ok;
}

bool bunzip2(FILE* in, FILE* out) {
    int bzerr = 0;
    BZFILE* bz = BZ2_bzReadOpen(&bzerr, in, 0, 0, nullptr, 0);
    if (bz == nullptr) return false;
    std::vector<char> buf(kChunk);
    bool ok = false;
    while (true) {
        int n = BZ2_bzRead(&bzerr, bz, buf.data(), (int)buf.size());
        if (n > 0) {
            if (std::fwrite(buf.data(), 1, (size_t)n, out) != (size_t)n) break;
        }
        if (bzerr == BZ_STREAM_END) {
            ok = true;
            break;
        }
        if (bzerr != BZ_OK) break;
    }
    BZ2_bzReadClose(&bzerr, bz);
    return ok;
}

bool unxz(FILE* in, FILE* out) {
    lzma_stream strm = LZMA_STREAM_INIT;
    if (lzma_stream_decoder(&strm, UINT64_MAX, 0) != LZMA_OK) return false;
    std::vector<uint8_t> inb(kChunk), outb(kChunk);
    lzma_action action = LZMA_RUN;
    lzma_ret r = LZMA_OK;
    bool ok = false;
    while (true) {
        if (strm.avail_in == 0) {
            size_t n = std::fread(inb.data(), 1, inb.size(), in);
            if (std::ferror(in)) break;
            strm.next_in = inb.data();
            strm.avail_in = n;
            if (n == 0) action = LZMA_FINISH;
        }
        strm.next_out = outb.data();
        strm.avail_out = outb.size();
        r = lzma_code(&strm, action);
        size_t produced = outb.size() - strm.avail_out;
        if (produced > 0 && std::fwrite(outb.data(), 1, produced, out) != produced) break;
        if (r == LZMA_STREAM_END) {
            ok = true;
            break;
        }
        if (r != LZMA_OK) break;
    }
    lzma_end(&strm);
    return ok;
}

bool unzstd(FILE* in, FILE* out) {
    ZSTD_DStream* d = ZSTD_createDStream();
    if (d == nullptr) return false;
    size_t initRes = ZSTD_initDStream(d);
    if (ZSTD_isError(initRes)) {
        ZSTD_freeDStream(d);
        return false;
    }
    bool ok = false;
    std::vector<char> inb(ZSTD_DStreamInSize()), outb(ZSTD_DStreamOutSize());
    ZSTD_inBuffer zin{inb.data(), 0, 0};
    bool eof = false;
    while (!eof) {
        if (zin.pos == zin.size) {
            size_t n = std::fread(inb.data(), 1, inb.size(), in);
            if (std::ferror(in)) break;
            zin.src = inb.data();
            zin.size = n;
            zin.pos = 0;
            if (n == 0) eof = true;
        }
        ZSTD_outBuffer zout{outb.data(), outb.size(), 0};
        size_t r = ZSTD_decompressStream(d, &zout, &zin);
        if (ZSTD_isError(r)) break;
        if (zout.pos > 0 && std::fwrite(outb.data(), 1, zout.pos, out) != zout.pos) break;
        if (r == 0) {
            ok = true;
            break;
        }
    }
    ZSTD_freeDStream(d);
    return ok;
}

bool unlz4(FILE* in, FILE* out) {
    LZ4F_dctx* ctx = nullptr;
    if (LZ4F_createDecompressionContext(&ctx, LZ4F_VERSION) != 0) return false;
    std::vector<uint8_t> inb(kChunk), outb(kChunk);
    bool ok = false;
    bool first = true;
    while (true) {
        size_t n = std::fread(inb.data(), 1, inb.size(), in);
        if (std::ferror(in)) break;
        if (n == 0) break;  // truncated frame -> fail below
        size_t srcSize = n;
        size_t dstSize = outb.size();
        size_t r = LZ4F_decompress(ctx, outb.data(), &dstSize, inb.data(), &srcSize, nullptr);
        if (LZ4F_isError(r)) break;
        if (dstSize > 0 && std::fwrite(outb.data(), 1, dstSize, out) != dstSize) break;
        first = false;
        if (r == 0) {
            ok = true;
            break;
        }
        // Bytes not consumed stay for the next round (frames are wholes).
        if (srcSize < n) {
            long back = (long)(n - srcSize);
            if (std::fseek(in, -back, SEEK_CUR) != 0) break;
        }
    }
    LZ4F_freeDecompressionContext(ctx);
    return ok && !first;
}

// --- tar ---

long long octal(const unsigned char* p, size_t n) {
    char buf[32];
    if (n >= sizeof(buf)) n = sizeof(buf) - 1;
    size_t i = 0;
    while (i < n && (p[i] == ' ' || p[i] == '\0')) ++i;
    size_t j = 0;
    while (i < n && j + 1 < sizeof(buf) && p[i] >= '0' && p[i] <= '7') buf[j++] = (char)p[i++];
    buf[j] = '\0';
    if (j == 0) return 0;
    return std::strtoll(buf, nullptr, 8);
}

std::string cstr(const unsigned char* p, size_t n) {
    size_t len = 0;
    while (len < n && p[len] != '\0') ++len;
    return std::string((const char*)p, len);
}

struct RawEntry {
    std::string path;
    bool isDir = false;
    long long size = 0;
    long long mtime = 0;
    long long offset = 0;  // data start in the (decompressed) image
};

bool parseTar(FILE* f, std::vector<RawEntry>& out) {
    unsigned char blk[512];
    std::string pendingLong;
    while (true) {
        long hdrPos = std::ftell(f);
        if (hdrPos < 0) return false;
        size_t n = std::fread(blk, 1, sizeof(blk), f);
        if (n == 0 && std::feof(f)) break;
        if (n != sizeof(blk)) return false;
        bool zero = true;
        for (size_t i = 0; i < sizeof(blk); ++i) {
            if (blk[i] != 0) {
                zero = false;
                break;
            }
        }
        if (zero) break;  // end of archive (single zero block is enough)
        if (!hasTarMagicAt(blk)) return false;
        std::string name = cstr(blk, 100);
        std::string prefix = cstr(blk + 345, 155);
        if (!prefix.empty()) name = prefix + "/" + name;
        char type = (char)blk[156];
        long long size = octal(blk + 124, 12);
        long long mtime = octal(blk + 136, 12);
        long long dataPos = hdrPos + 512;
        if (type == 'L' || type == 'K') {
            // GNU long name/link: data block(s) hold the real name.
            std::string longName;
            longName.resize((size_t)size);
            if (size > 0 && std::fread(&longName[0], 1, (size_t)size, f) != (size_t)size) {
                return false;
            }
            while (!longName.empty() && longName.back() == '\0') longName.pop_back();
            if (type == 'L') pendingLong = longName;
            long long skip = (size + 511) / 512 * 512;
            if (std::fseek(f, hdrPos + 512 + skip, SEEK_SET) != 0) return false;
            continue;
        }
        if (type == 'x' || type == 'g') {
            // pax extended header: best effort, skip it.
            long long skip = (size + 511) / 512 * 512;
            if (std::fseek(f, hdrPos + 512 + skip, SEEK_SET) != 0) return false;
            continue;
        }
        RawEntry e;
        e.path = pendingLong.empty() ? name : pendingLong;
        pendingLong.clear();
        e.isDir = (type == '5') || (!e.path.empty() && e.path.back() == '/');
        e.size = (type == '5') ? 0 : size;
        e.mtime = mtime;
        e.offset = dataPos;
        if (e.path.empty()) {
            long long skip = (size + 511) / 512 * 512;
            if (std::fseek(f, hdrPos + 512 + skip, SEEK_SET) != 0) return false;
            continue;
        }
        out.push_back(e);
        long long skip = (size + 511) / 512 * 512;
        if (std::fseek(f, hdrPos + 512 + skip, SEEK_SET) != 0) return false;
    }
    return true;
}

std::string baseName(const std::string& path) {
    size_t s = path.find_last_of("/\\");
    return (s == std::string::npos) ? path : path.substr(s + 1);
}

std::string stripStreamSuffix(const std::string& name) {
    static const char* kSfx[] = {".tar.gz", ".tgz", ".tar.bz2", ".tbz2", ".tar.xz",
                                 ".txz",    ".tar.zst", ".tzst", ".tar.lz4", ".tlz4",
                                 ".gz",     ".bz2",     ".xz",   ".zst",    ".lz4",
                                 ".tar",    nullptr};
    for (int i = 0; kSfx[i] != nullptr; ++i) {
        std::string s = kSfx[i];
        if (name.size() > s.size() &&
            name.compare(name.size() - s.size(), s.size(), s) == 0) {
            return name.substr(0, name.size() - s.size());
        }
    }
    return name + ".out";
}

// --- session ---

// Fresh-create routing needs the same answer as the session: foo.tar.gz is
// a layered tar, foo.gz is a single stream. Case-insensitive suffix match.
bool hasLayeredTarSuffix(const std::string& name) {
    // Lowercase a copy; archive names are short, no need to be clever.
    std::string n = name;
    for (size_t i = 0; i < n.size(); ++i) {
        if (n[i] >= 'A' && n[i] <= 'Z') n[i] = (char)(n[i] - 'A' + 'a');
    }
    static const char* kSfx[] = {".tar.gz", ".tgz",         ".tar.bz2", ".tbz2",
                                 ".tar.xz", ".txz",         ".tar.zst", ".tzst",
                                 ".tar.lz4", ".tlz4", nullptr};
    for (int i = 0; kSfx[i] != nullptr; ++i) {
        std::string s = kSfx[i];
        if (n.size() > s.size() && n.compare(n.size() - s.size(), s.size(), s) == 0) {
            return true;
        }
    }
    return false;
}

// --- tar writer (ustar, MT-style: sorted entries, real mtimes, no extras) ---

bool putOctal(unsigned char* dst, size_t n, unsigned long long v) {
    // n includes the trailing NUL; false when the value does not fit
    // (u12 fields top out just under 8 GiB — same ceiling as GNU tar's
    // octal mode, plenty for an on-device writer).
    if (n == 0) return false;
    dst[n - 1] = '\0';
    for (size_t i = n - 1; i-- > 0;) {
        dst[i] = (unsigned char)('0' + (v & 7ULL));
        v >>= 3;
    }
    return v == 0;
}

bool emitTarHeader(FILE* out, const char* name, size_t nameLen, const char* prefix,
                   size_t prefixLen, char type, long long size, long long mtime, int mode) {
    unsigned char blk[512];
    std::memset(blk, 0, sizeof(blk));
    if (nameLen > 100 || prefixLen > 155) return false;
    std::memcpy(blk, name, nameLen);
    if (prefixLen > 0) std::memcpy(blk + 345, prefix, prefixLen);
    if (!putOctal(blk + 100, 8, (unsigned long long)mode)) return false;
    if (!putOctal(blk + 108, 8, 0)) return false;  // uid
    if (!putOctal(blk + 116, 8, 0)) return false;  // gid
    if (!putOctal(blk + 124, 12, (unsigned long long)size)) return false;
    if (!putOctal(blk + 136, 12, (unsigned long long)(mtime < 0 ? 0 : mtime))) return false;
    blk[156] = (unsigned char)type;
    std::memcpy(blk + 257, "ustar", 5);
    std::memcpy(blk + 263, "00", 2);
    std::memset(blk + 148, ' ', 8);
    unsigned sum = 0;
    for (size_t i = 0; i < sizeof(blk); ++i) sum += blk[i];
    if (!putOctal(blk + 148, 8, sum)) return false;
    return std::fwrite(blk, 1, sizeof(blk), out) == sizeof(blk);
}

bool writeOneTarHeader(FILE* out, const std::string& path, bool isDir, long long size,
                       long long mtime) {
    int mode = isDir ? 0755 : 0644;
    long long payload = isDir ? 0 : size;
    if (path.size() <= 100) {
        return emitTarHeader(out, path.c_str(), path.size(), "", 0, isDir ? '5' : '0',
                             payload, mtime, mode);
    }
    // Prefer a ustar name/prefix split; fall back to a GNU 'L' longname entry
    // (both are understood by parseTar above and by GNU tar).
    size_t best = std::string::npos;
    for (size_t i = 1; i < path.size(); ++i) {
        if (path[i] == '/' && i <= 155 && path.size() - i - 1 <= 100 && i > 0) best = i;
    }
    if (best != std::string::npos) {
        std::string prefix = path.substr(0, best);
        std::string name = path.substr(best + 1);
        return emitTarHeader(out, name.c_str(), name.size(), prefix.c_str(), prefix.size(),
                             isDir ? '5' : '0', payload, mtime, mode);
    }
    // GNU longname: full path + NUL as the 'L' payload; the following real
    // header carries a truncated name that readers must ignore in favour of
    // the pending long name (parseTar does exactly that).
    std::string full = path;
    full.push_back('\0');
    const char* kLong = "././@LongLink";
    if (!emitTarHeader(out, kLong, std::strlen(kLong), "", 0, 'L', (long long)full.size(),
                       mtime, 0)) {
        return false;
    }
    if (std::fwrite(full.data(), 1, full.size(), out) != full.size()) return false;
    unsigned char pad[512] = {0};
    size_t rest = (512 - (full.size() % 512)) % 512;
    if (rest > 0 && std::fwrite(pad, 1, rest, out) != rest) return false;
    std::string shortName = path.substr(0, 100);
    return emitTarHeader(out, shortName.c_str(), shortName.size(), "", 0, isDir ? '5' : '0',
                         payload, mtime, mode);
}

bool copyRange(FILE* in, long long off, long long size, FILE* out) {
    if (std::fseek(in, off, SEEK_SET) != 0) return false;
    std::vector<unsigned char> buf(kChunk);
    long long done = 0;
    while (done < size) {
        size_t want = (size_t)((size - done > (long long)buf.size()) ? buf.size()
                                                                     : (size_t)(size - done));
        size_t n = std::fread(buf.data(), 1, want, in);
        if (n == 0) return false;
        if (std::fwrite(buf.data(), 1, n, out) != n) return false;
        done += (long long)n;
    }
    return true;
}

struct TarStaged {
    std::string path;    // dirs end with '/'
    bool isDir = false;
    long long size = 0;
    long long mtime = 0;  // seconds
    // Data source: either a session-external file or a slice of origImage.
    std::string srcPath;
    bool fromOrig = false;
    long long origOff = 0;
};

bool writeTarImage(const std::vector<TarStaged>& entries, const std::string& origImage,
                   const std::string& outPath) {
    FILE* out = std::fopen(outPath.c_str(), "wb");
    if (out == nullptr) return false;
    FILE* orig = nullptr;
    if (!origImage.empty()) {
        orig = std::fopen(origImage.c_str(), "rb");
        if (orig == nullptr) {
            std::fclose(out);
            return false;
        }
    }
    bool ok = true;
    unsigned char pad[512] = {0};
    for (size_t i = 0; ok && i < entries.size(); ++i) {
        const TarStaged& e = entries[i];
        ok = writeOneTarHeader(out, e.path, e.isDir, e.size, e.mtime);
        if (ok && !e.isDir && e.size > 0) {
            if (e.fromOrig) {
                ok = orig != nullptr && copyRange(orig, e.origOff, e.size, out);
            } else {
                FILE* in = std::fopen(e.srcPath.c_str(), "rb");
                ok = in != nullptr && copyRange(in, 0, e.size, out);
                if (in != nullptr) std::fclose(in);
            }
        }
        if (ok && !e.isDir) {
            size_t rest = (size_t)((512 - (e.size % 512)) % 512);
            if (rest > 0) ok = std::fwrite(pad, 1, rest, out) == rest;
        }
    }
    if (ok) {
        // End-of-archive: two zero blocks (readers accept one; two is canonical).
        ok = std::fwrite(pad, 1, sizeof(pad), out) == sizeof(pad) &&
             std::fwrite(pad, 1, sizeof(pad), out) == sizeof(pad);
    }
    if (orig != nullptr) std::fclose(orig);
    if (std::fclose(out) != 0) ok = false;
    return ok;
}

// --- compressors: FILE* -> FILE*, true on clean stream end ---

bool gzipFile(FILE* in, FILE* out, int level) {
    if (level <= 0) level = 6;
    if (level > 9) level = 9;
    z_stream strm{};
    if (deflateInit2(&strm, level, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
        return false;
    }
    std::vector<unsigned char> inb(kChunk), outb(kChunk);
    int flush = Z_NO_FLUSH;
    bool ok = false;
    do {
        strm.avail_in = (uInt)std::fread(inb.data(), 1, inb.size(), in);
        if (std::ferror(in)) break;
        if (std::feof(in)) flush = Z_FINISH;
        strm.next_in = inb.data();
        do {
            strm.avail_out = (uInt)outb.size();
            strm.next_out = outb.data();
            int r = deflate(&strm, flush);
            if (r == Z_STREAM_ERROR) {
                deflateEnd(&strm);
                return false;
            }
            size_t have = outb.size() - strm.avail_out;
            if (have > 0 && std::fwrite(outb.data(), 1, have, out) != have) {
                deflateEnd(&strm);
                return false;
            }
        } while (strm.avail_out == 0);
    } while (flush != Z_FINISH);
    ok = true;
    deflateEnd(&strm);
    return ok;
}

bool bzip2File(FILE* in, FILE* out, int level) {
    if (level <= 0) level = 9;
    if (level > 9) level = 9;
    int bzerr = 0;
    BZFILE* bz = BZ2_bzWriteOpen(&bzerr, out, level, 0, 0);
    if (bz == nullptr) return false;
    std::vector<char> buf(kChunk);
    bool ok = true;
    while (true) {
        size_t n = std::fread(buf.data(), 1, buf.size(), in);
        if (std::ferror(in)) {
            ok = false;
            break;
        }
        if (n > 0) BZ2_bzWrite(&bzerr, bz, buf.data(), (int)n);
        if (bzerr != BZ_OK) {
            ok = false;
            break;
        }
        if (n == 0) break;
    }
    BZ2_bzWriteClose(&bzerr, bz, 0, nullptr, nullptr);
    return ok && bzerr == BZ_OK;
}

bool xzFile(FILE* in, FILE* out, int level) {
    if (level < 0) level = 6;
    if (level > 9) level = 9;
    lzma_stream strm = LZMA_STREAM_INIT;
    if (lzma_easy_encoder(&strm, (uint32_t)level, LZMA_CHECK_CRC64) != LZMA_OK) return false;
    std::vector<uint8_t> inb(kChunk), outb(kChunk);
    lzma_action action = LZMA_RUN;
    bool ok = false;
    while (true) {
        if (strm.avail_in == 0 && action == LZMA_RUN) {
            size_t n = std::fread(inb.data(), 1, inb.size(), in);
            if (std::ferror(in)) break;
            strm.next_in = inb.data();
            strm.avail_in = n;
            if (n == 0) action = LZMA_FINISH;
        }
        strm.next_out = outb.data();
        strm.avail_out = outb.size();
        lzma_ret r = lzma_code(&strm, action);
        size_t produced = outb.size() - strm.avail_out;
        if (produced > 0 && std::fwrite(outb.data(), 1, produced, out) != produced) break;
        if (r == LZMA_STREAM_END) {
            ok = true;
            break;
        }
        if (r != LZMA_OK) break;
    }
    lzma_end(&strm);
    return ok;
}

bool zstdFile(FILE* in, FILE* out, int level) {
    int maxLevel = (int)ZSTD_maxCLevel();
    if (level <= 0) level = 3;
    if (level > maxLevel) level = maxLevel;
    ZSTD_CStream* c = ZSTD_createCStream();
    if (c == nullptr) return false;
    bool ok = false;
    if (!ZSTD_isError(ZSTD_initCStream(c, level))) {
        std::vector<char> inb(ZSTD_CStreamInSize()), outb(ZSTD_CStreamOutSize());
        ZSTD_inBuffer zin{inb.data(), 0, 0};
        bool eof = false;
        ok = true;
        while (!eof && ok) {
            if (zin.pos == zin.size) {
                size_t n = std::fread(inb.data(), 1, inb.size(), in);
                if (std::ferror(in)) {
                    ok = false;
                    break;
                }
                zin.src = inb.data();
                zin.size = n;
                zin.pos = 0;
                if (n == 0) eof = true;
            }
            ZSTD_outBuffer zout{outb.data(), outb.size(), 0};
            size_t r = eof ? ZSTD_endStream(c, &zout)
                           : ZSTD_compressStream(c, &zout, &zin);
            if (ZSTD_isError(r)) {
                ok = false;
                break;
            }
            if (zout.pos > 0 && std::fwrite(outb.data(), 1, zout.pos, out) != zout.pos) {
                ok = false;
                break;
            }
            if (eof && r == 0) break;
        }
    }
    ZSTD_freeCStream(c);
    return ok;
}

bool lz4File(FILE* in, FILE* out, int level) {
    if (level < 0) level = 0;
    if (level > 12) level = 12;
    LZ4F_cctx* cctx = nullptr;
    if (LZ4F_createCompressionContext(&cctx, LZ4F_VERSION) != 0) return false;
    LZ4F_preferences_t prefs{};
    prefs.compressionLevel = level;
    std::vector<uint8_t> inb(kChunk), outb(LZ4F_compressBound(kChunk, &prefs));
    if (outb.empty()) {
        LZ4F_freeCompressionContext(cctx);
        return false;
    }
    bool ok = false;
    size_t n = LZ4F_compressBegin(cctx, outb.data(), outb.size(), &prefs);
    if (!LZ4F_isError(n) && (n == 0 || std::fwrite(outb.data(), 1, n, out) == n)) {
        ok = true;
        while (ok) {
            size_t rd = std::fread(inb.data(), 1, inb.size(), in);
            if (std::ferror(in)) {
                ok = false;
                break;
            }
            if (rd > 0) {
                n = LZ4F_compressUpdate(cctx, outb.data(), outb.size(), inb.data(), rd, nullptr);
                if (LZ4F_isError(n) || std::fwrite(outb.data(), 1, n, out) != n) {
                    ok = false;
                    break;
                }
            }
            if (rd == 0) break;
        }
    }
    if (ok) {
        n = LZ4F_compressEnd(cctx, outb.data(), outb.size(), nullptr);
        ok = !LZ4F_isError(n) && (n == 0 || std::fwrite(outb.data(), 1, n, out) == n);
    }
    LZ4F_freeCompressionContext(cctx);
    return ok;
}

bool compressKindToFile(Kind kind, const std::string& inPath, const std::string& outPath,
                        int level) {
    FILE* in = std::fopen(inPath.c_str(), "rb");
    if (in == nullptr) return false;
    FILE* out = std::fopen(outPath.c_str(), "wb");
    if (out == nullptr) {
        std::fclose(in);
        return false;
    }
    bool ok = false;
    switch (kind) {
        case Kind::Gzip:
            ok = gzipFile(in, out, level);
            break;
        case Kind::Bzip2:
            ok = bzip2File(in, out, level);
            break;
        case Kind::Xz:
            ok = xzFile(in, out, level);
            break;
        case Kind::Zstd:
            ok = zstdFile(in, out, level);
            break;
        case Kind::Lz4:
            ok = lz4File(in, out, level);
            break;
        case Kind::RawTar:
            ok = false;
            break;
    }
    std::fclose(in);
    if (std::fclose(out) != 0) ok = false;
    return ok;
}

bool decompressKindToFile(Kind kind, const std::string& inPath, const std::string& outPath) {
    FILE* in = std::fopen(inPath.c_str(), "rb");
    if (in == nullptr) return false;
    FILE* out = std::fopen(outPath.c_str(), "wb");
    if (out == nullptr) {
        std::fclose(in);
        return false;
    }
    bool ok = false;
    switch (kind) {
        case Kind::Gzip:
            ok = gunzip(in, out);
            break;
        case Kind::Bzip2:
            ok = bunzip2(in, out);
            break;
        case Kind::Xz:
            ok = unxz(in, out);
            break;
        case Kind::Zstd:
            ok = unzstd(in, out);
            break;
        case Kind::Lz4:
            ok = unlz4(in, out);
            break;
        case Kind::RawTar:
            ok = true;
            break;
    }
    std::fclose(in);
    if (std::fclose(out) != 0) ok = false;
    return ok;
}

}  // anonymous namespace (helpers above)

int gTempCounter = 0;

class StreamSession : public ISession {
   public:
    StreamSession(Kind kind, const std::string& path, const std::string& format)
        : kind_(kind), path_(path), format_(format) {}

    bool open(const WriteOptions& opt, bool writable, bool fresh, Error* err) {
        if (writable || fresh) return openForWrite(opt, writable, fresh, err);
        if (kind_ == Kind::RawTar) {
            image_ = path_;
        } else {
            char tmp[32];
            std::snprintf(tmp, sizeof(tmp), ".aaetmp.%d.tmp", ++gTempCounter);
            image_ = path_ + tmp;
            ownTemp_ = true;
            FILE* in = std::fopen(path_.c_str(), "rb");
            if (in == nullptr) {
                fail(err, Status::Io, "no such file: " + path_);
                return false;
            }
            FILE* out = std::fopen(image_.c_str(), "wb");
            if (out == nullptr) {
                std::fclose(in);
                fail(err, Status::Io, "cannot write temp: " + image_);
                return false;
            }
            bool ok = false;
            switch (kind_) {
                case Kind::Gzip:
                    ok = gunzip(in, out);
                    break;
                case Kind::Bzip2:
                    ok = bunzip2(in, out);
                    break;
                case Kind::Xz:
                    ok = unxz(in, out);
                    break;
                case Kind::Zstd:
                    ok = unzstd(in, out);
                    break;
                case Kind::Lz4:
                    ok = unlz4(in, out);
                    break;
                case Kind::RawTar:
                    ok = true;
                    break;
            }
            std::fclose(in);
            std::fclose(out);
            if (!ok) {
                std::remove(image_.c_str());
                fail(err, Status::NotArchive, "not a " + format_ + " stream: " + path_);
                return false;
            }
        }
        // Sniff tar: layered .tar.gz/.tgz/… report as tar from here on.
        FILE* f = std::fopen(image_.c_str(), "rb");
        if (f == nullptr) {
            fail(err, Status::Io, "cannot read: " + image_);
            cleanup();
            return false;
        }
        unsigned char head[512] = {0};
        size_t n = std::fread(head, 1, sizeof(head), f);
        if (isTarImage(head, n)) {
            isTar_ = true;
            format_ = "tar";
            // The sniff above leaves the cursor at offset 512; tar parsing
            // must start at 0 (found by device testing: every tar, plain or
            // pax, failed with "bad tar image" without this rewind).
            std::rewind(f);
            if (!parseTar(f, entries_)) {
                std::fclose(f);
                fail(err, Status::NotArchive, "bad tar image: " + path_);
                cleanup();
                return false;
            }
        } else if (kind_ == Kind::RawTar) {
            std::fclose(f);
            fail(err, Status::NotArchive, "not a tar archive: " + path_);
            cleanup();
            return false;
        } else {
            struct stat st {};
            if (::stat(image_.c_str(), &st) != 0) {
                std::fclose(f);
                fail(err, Status::Io, "cannot stat temp image");
                cleanup();
                return false;
            }
            singleSize_ = st.st_size;
            singleName_ = stripStreamSuffix(baseName(path_));
        }
        std::fclose(f);
        opened_ = true;
        return true;
    }

    // Write mode: fresh create or in-place rewrite (add/remove/rename +
    // commit). Rewrite loads current content into staged_ (data stays in the
    // original image until commit); commit rebuilds to a temp file and
    // renames over the target, so a discarded session never touches it.
    bool openForWrite(const WriteOptions&, bool, bool fresh, Error* err) {
        struct stat pst {};
        if (::stat(path_.c_str(), &pst) != 0 && !fresh) {
            fail(err, Status::Io, "no such file: " + path_);
            return false;
        }
        if (fresh) {
            layered_ = (kind_ == Kind::RawTar) || hasLayeredTarSuffix(path_);
            if (!layered_) singleName_ = stripStreamSuffix(baseName(path_));
        } else {
            if (kind_ == Kind::RawTar) {
                image_ = path_;
            } else {
                char tmp[64];
                std::snprintf(tmp, sizeof(tmp), ".aaetmp.%d.tmp", ++gTempCounter);
                image_ = path_ + tmp;
                ownTemp_ = true;
                if (!decompressKindToFile(kind_, path_, image_)) {
                    std::remove(image_.c_str());
                    fail(err, Status::NotArchive, "not a " + format_ + " stream: " + path_);
                    return false;
                }
            }
            FILE* f = std::fopen(image_.c_str(), "rb");
            if (f == nullptr) {
                fail(err, Status::Io, "cannot read: " + image_);
                cleanup();
                return false;
            }
            unsigned char head[512] = {0};
            size_t n = std::fread(head, 1, sizeof(head), f);
            if (isTarImage(head, n)) {
                layered_ = true;
                std::rewind(f);
                if (!parseTar(f, entries_)) {
                    std::fclose(f);
                    fail(err, Status::NotArchive, "bad tar image: " + path_);
                    cleanup();
                    return false;
                }
                for (const auto& r : entries_) {
                    TarStaged s;
                    s.path = r.path;
                    s.isDir = r.isDir;
                    s.size = r.size;
                    s.mtime = r.mtime;
                    s.fromOrig = true;
                    s.origOff = r.offset;
                    staged_.push_back(s);
                }
            } else if (kind_ == Kind::RawTar) {
                std::fclose(f);
                fail(err, Status::NotArchive, "not a tar archive: " + path_);
                cleanup();
                return false;
            } else {
                layered_ = false;
                struct stat st {};
                if (::stat(image_.c_str(), &st) != 0) {
                    std::fclose(f);
                    fail(err, Status::Io, "cannot stat temp image");
                    cleanup();
                    return false;
                }
                singleSize_ = st.st_size;
                singleName_ = stripStreamSuffix(baseName(path_));
                TarStaged s;
                s.path = singleName_;
                s.size = singleSize_;
                s.mtime = st.st_mtime;
                s.fromOrig = true;
                staged_.push_back(s);
            }
            std::fclose(f);
            origImage_ = image_;
        }
        writeMode_ = true;
        fresh_ = fresh;
        opened_ = true;
        return true;
    }

    bool list(std::vector<Entry>& out, Error* err) override {
        if (!checkOpen(err)) return false;
        if (writeMode_) {
            if (!layered_) {
                // Single stream: at most one logical entry (name follows the
                // archive file name, exactly like the read path derives it).
                if (!staged_.empty()) {
                    Entry e;
                    e.path = singleName_;
                    e.size = staged_[0].size;
                    e.mtimeMillis = staged_[0].mtime * 1000LL;
                    out.push_back(e);
                }
            } else {
                for (const auto& s : staged_) {
                    Entry e;
                    e.path = s.path;
                    e.isDir = s.isDir;
                    e.size = s.size;
                    e.mtimeMillis = s.mtime * 1000LL;
                    out.push_back(e);
                }
            }
            return true;
        }
        if (isTar_) {
            for (const auto& r : entries_) {
                Entry e;
                e.path = r.path;
                e.isDir = r.isDir;
                e.size = r.size;
                e.mtimeMillis = r.mtime * 1000LL;
                out.push_back(e);
            }
        } else {
            Entry e;
            e.path = singleName_;
            e.size = singleSize_;
            out.push_back(e);
        }
        return true;
    }

    bool info(ArchiveInfo& out, Error* err) override {
        if (!checkOpen(err)) return false;
        std::vector<Entry> entries;
        if (!list(entries, err)) return false;
        out.format = (writeMode_ && layered_) ? "tar" : format_;
        out.entryCount = (int)entries.size();
        for (const auto& e : entries) {
            if (!e.isDir) {
                out.totalSize += e.size;
                out.totalPacked += e.size;  // stream already decompressed
            }
        }
        return true;
    }

    // Shared entry lookup for readBytes/extractTo, in both read and write
    // (rewrite) modes. Write-mode data comes from freshly added files or
    // from slices of the original image. Directory hits report wantDir
    // (readBytes turns those into NotFound, like the old lookup that only
    // matched files).
    bool resolveEntry(const std::string& entry, bool& wantDir, std::string& file,
                      long long& off, long long& size, Error* err) {
        if (!isSafeEntryName(entry)) {
            fail(err, Status::UnsafeName, "unsafe entry name: " + entry);
            return false;
        }
        wantDir = false;
        file = image_;
        off = 0;
        size = 0;
        if (writeMode_) {
            if (!layered_) {
                if (entry != singleName_ || staged_.empty()) {
                    fail(err, Status::NotFound, "no such entry: " + entry);
                    return false;
                }
                const TarStaged& s = staged_[0];
                file = s.fromOrig ? origImage_ : s.srcPath;
                off = s.fromOrig ? s.origOff : 0;
                size = s.size;
                return true;
            }
            for (const auto& s : staged_) {
                if (!s.isDir && s.path == entry) {
                    file = s.fromOrig ? origImage_ : s.srcPath;
                    off = s.fromOrig ? s.origOff : 0;
                    size = s.size;
                    return true;
                }
            }
            for (const auto& s : staged_) {
                if (s.isDir && (s.path == entry || s.path == entry + "/")) {
                    wantDir = true;
                    return true;
                }
            }
            fail(err, Status::NotFound, "no such entry: " + entry);
            return false;
        }
        if (isTar_) {
            for (const auto& r : entries_) {
                if (!r.isDir && r.path == entry) {
                    off = r.offset;
                    size = r.size;
                    return true;
                }
            }
            for (const auto& r : entries_) {
                if (r.isDir && r.path == entry) {
                    wantDir = true;
                    return true;
                }
            }
            fail(err, Status::NotFound, "no such entry: " + entry);
            return false;
        }
        if (entry != singleName_) {
            fail(err, Status::NotFound, "no such entry: " + entry);
            return false;
        }
        size = singleSize_;
        return true;
    }

    bool readBytes(const std::string& entry, long long maxBytes, std::vector<char>& out,
                   Error* err) override {
        if (!checkOpen(err)) return false;
        bool wantDir = false;
        std::string file;
        long long off = 0, size = 0;
        if (!resolveEntry(entry, wantDir, file, off, size, err)) return false;
        if (wantDir) {
            if (err != nullptr) err->set(Status::NotFound, "no such entry: " + entry);
            return false;
        }
        if (size > maxBytes) {
            if (err != nullptr) err->set(Status::TooLarge, "entry larger than limit: " + entry);
            return false;
        }
        FILE* f = std::fopen(file.c_str(), "rb");
        if (f == nullptr || std::fseek(f, off, SEEK_SET) != 0) {
            if (f != nullptr) std::fclose(f);
            if (err != nullptr) err->set(Status::Io, "cannot read image");
            return false;
        }
        out.resize((size_t)size);
        bool ok = size == 0 ||
                  std::fread(out.data(), 1, (size_t)size, f) == (size_t)size;
        std::fclose(f);
        if (!ok && err != nullptr) err->set(Status::Io, "cannot read entry: " + entry);
        return ok;
    }

    bool extractTo(const std::string& entry, const std::string& outPath,
                   ProgressSink* progress, Error* err) override {
        if (!checkOpen(err)) return false;
        bool wantDir = false;
        std::string file;
        long long off = 0, size = 0;
        if (!resolveEntry(entry, wantDir, file, off, size, err)) return false;
        if (wantDir) {
            if (!mkdirs(outPath)) {
                if (err != nullptr) {
                    err->set(Status::Io,
                             "cannot create dir: " + outPath + ": " + std::strerror(errno));
                }
                return false;
            }
            return true;
        }
        FILE* in = std::fopen(file.c_str(), "rb");
        if (in == nullptr || std::fseek(in, off, SEEK_SET) != 0) {
            if (in != nullptr) std::fclose(in);
            if (err != nullptr) err->set(Status::Io, "cannot read image");
            return false;
        }
        FILE* out = std::fopen(outPath.c_str(), "wb");
        if (out == nullptr) {
            std::fclose(in);
            if (err != nullptr) err->set(Status::Io, "cannot write: " + outPath);
            return false;
        }
        std::vector<char> buf(kChunk);
        long long done = 0;
        bool cancelled = false, ioErr = false;
        while (done < size) {
            size_t want = (size_t)((size - done > (long long)buf.size()) ? buf.size()
                                                                         : (size_t)(size - done));
            size_t n = std::fread(buf.data(), 1, want, in);
            if (n == 0) {
                if (std::ferror(in)) ioErr = true;
                break;
            }
            if (std::fwrite(buf.data(), 1, n, out) != n) {
                ioErr = true;
                break;
            }
            done += (long long)n;
            if (progress != nullptr && !progress->poll(0, 1, done, size)) {
                cancelled = true;
                break;
            }
        }
        std::fclose(in);
        std::fclose(out);
        if (cancelled) {
            std::remove(outPath.c_str());
            if (err != nullptr) err->set(Status::Cancelled, "cancelled: " + entry);
            return false;
        }
        if (ioErr || done != size) {
            std::remove(outPath.c_str());
            if (err != nullptr) err->set(Status::Io, "extract failed: " + entry);
            return false;
        }
        if (progress != nullptr) progress->poll(0, 1, size, size);
        return true;
    }

    bool addFile(const std::string& entryName, const std::string& srcPath,
                 const WriteOptions& opt, Error* err) override {
        if (!requireWrite(err)) return false;
        if (!isSafeEntryName(entryName)) {
            fail(err, Status::UnsafeName, "unsafe entry name: " + entryName);
            return false;
        }
        // The session level comes from the first add (Java passes identical
        // options for every file of one job; JNI open carries no level).
        if (!levelSet_) {
            if (!checkLevel(kind_, opt.level, format_, err)) return false;
            level_ = opt.level;
            levelSet_ = true;
        }
        struct stat st {};
        if (::stat(srcPath.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) {
            fail(err, Status::Io, "cannot read input: " + srcPath);
            return false;
        }
        if (!layered_) {
            // Single-file stream: adding replaces the content (at most one).
            staged_.clear();
            TarStaged s;
            s.path = singleName_;
            s.size = st.st_size;
            s.mtime = st.st_mtime;
            s.srcPath = srcPath;
            staged_.push_back(s);
        } else {
            TarStaged s;
            s.path = entryName;
            s.size = st.st_size;
            s.mtime = st.st_mtime;
            s.srcPath = srcPath;
            stageReplace(s);
        }
        dirty_ = true;
        return true;
    }

    bool addDirectory(const std::string& entryName, Error* err) override {
        if (!requireWrite(err)) return false;
        if (!layered_) {
            fail(err, Status::UnsupportedOperation,
                 format_ + " holds a single file (no directories)");
            return false;
        }
        std::string clean = entryName;
        while (!clean.empty() && clean.back() == '/') clean.pop_back();
        if (!isSafeEntryName(clean)) {
            fail(err, Status::UnsafeName, "unsafe entry name: " + entryName);
            return false;
        }
        TarStaged s;
        s.path = clean + "/";
        s.isDir = true;
        s.mtime = std::time(nullptr);
        stageReplace(s);
        dirty_ = true;
        return true;
    }

    bool remove(const std::string& entry, Error* err) override {
        if (!requireWrite(err)) return false;
        if (!layered_) {
            if (staged_.empty()) {
                fail(err, Status::NotFound, "no such entry: " + entry);
                return false;
            }
            staged_.clear();
            dirty_ = true;
            return true;
        }
        std::string base = entry;
        while (!base.empty() && base.back() == '/') base.pop_back();
        size_t before = staged_.size();
        for (size_t i = 0; i < staged_.size();) {
            const std::string& p = staged_[i].path;
            std::string pb = p;
            while (!pb.empty() && pb.back() == '/') pb.pop_back();
            if (p == entry || p == entry + "/" || pb == base ||
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
        if (!requireWrite(err)) return false;
        if (!isSafeEntryName(to)) {
            fail(err, Status::UnsafeName, "unsafe entry name: " + to);
            return false;
        }
        if (!layered_) {
            fail(err, Status::UnsupportedOperation,
                 "cannot rename inside a single-file stream "
                 "(the name follows the archive file name)");
            return false;
        }
        auto exists = [&](const std::string& p) {
            for (const auto& s : staged_) {
                if (s.path == p) return true;
            }
            return false;
        };
        // Plain file rename first.
        for (size_t i = 0; i < staged_.size(); ++i) {
            if (!staged_[i].isDir && staged_[i].path == from) {
                if (exists(to)) {
                    fail(err, Status::Io, "already exists: " + to);
                    return false;
                }
                TarStaged s = staged_[i];
                staged_.erase(staged_.begin() + (ptrdiff_t)i);
                s.path = to;
                stageReplace(s);
                dirty_ = true;
                return true;
            }
        }
        // Directory subtree rename ("docs" renames "docs/..." too).
        std::string fd = from;
        while (!fd.empty() && fd.back() == '/') fd.pop_back();
        std::string td = to;
        while (!td.empty() && td.back() == '/') td.pop_back();
        if (fd.empty() || td.empty()) {
            fail(err, Status::UnsafeName, "unsafe entry name: " + to);
            return false;
        }
        fd += "/";
        td += "/";
        bool any = false;
        for (const auto& s : staged_) {
            if (s.path == fd || (s.path.size() > fd.size() &&
                                 s.path.compare(0, fd.size(), fd) == 0)) {
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
            if (s.path == fd) {
                np = td;
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
            if (s.path == fd) {
                s.path = td;
            } else if (s.path.size() > fd.size() && s.path.compare(0, fd.size(), fd) == 0) {
                s.path = td + s.path.substr(fd.size());
            }
        }
        std::sort(staged_.begin(), staged_.end(),
                  [](const TarStaged& a, const TarStaged& b) { return a.path < b.path; });
        dirty_ = true;
        return true;
    }

    bool setComment(const std::string&, Error* err) override {
        fail(err, Status::UnsupportedOperation, format_ + " has no archive comment");
        return false;
    }

    bool close(bool discard, Error* err) override {
        if (discard) {
            opened_ = false;
            cleanup();
            return true;
        }
        if (!writeMode_) {
            opened_ = false;
            cleanup();
            return true;
        }
        // Rewrite with no changes: leave the file byte-identical.
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

    ~StreamSession() override { cleanup(); }

   private:
    static void fail(Error* err, Status s, const std::string& m) {
        if (err != nullptr) err->set(s, m);
    }
    void cleanup() {
        if (ownTemp_ && !image_.empty()) std::remove(image_.c_str());
        ownTemp_ = false;
        if (!buildTmpA_.empty()) std::remove(buildTmpA_.c_str());
        if (!buildTmpB_.empty()) std::remove(buildTmpB_.c_str());
        buildTmpA_.clear();
        buildTmpB_.clear();
    }
    bool checkOpen(Error* err) {
        if (!opened_) {
            fail(err, Status::Io, "session is closed");
            return false;
        }
        return true;
    }
    bool requireWrite(Error* err) {
        if (!checkOpen(err)) return false;
        if (!writeMode_) {
            fail(err, Status::UnsupportedOperation, "session is read-only");
            return false;
        }
        return true;
    }
    // Level ranges per codec. The method wire int is Java-validated and
    // ignored here; only the numeric level reaches the encoder.
    static bool checkLevel(Kind kind, int lvl, const std::string& format, Error* err) {
        bool ok = false;
        switch (kind) {
            case Kind::RawTar:
                ok = (lvl == 0);
                break;
            case Kind::Gzip:
            case Kind::Bzip2:
            case Kind::Xz:
                ok = (lvl >= 0 && lvl <= 9);
                break;
            case Kind::Zstd:
                ok = (lvl >= 0 && lvl <= 22);
                break;
            case Kind::Lz4:
                ok = (lvl >= 0 && lvl <= 12);
                break;
        }
        if (!ok && err != nullptr) {
            err->set(Status::UnsupportedOperation,
                     "level " + std::to_string(lvl) + " not supported by " + format);
        }
        return ok;
    }
    void stageReplace(TarStaged s) {
        for (auto& e : staged_) {
            if (e.path == s.path) {
                e = s;
                return;
            }
        }
        auto it = std::lower_bound(staged_.begin(), staged_.end(), s.path,
                                   [](const TarStaged& e, const std::string& v) {
                                       return e.path < v;
                                   });
        staged_.insert(it, s);
    }
    std::string freshTemp(char tag) {
        char tmp[64];
        std::snprintf(tmp, sizeof(tmp), ".aaebuild.%c.%d.tmp", tag, ++gTempCounter);
        return path_ + tmp;
    }
    // Rebuilds the archive into a temp file and renames over the target, so
    // a failed commit keeps the previous file intact. Never deletes
    // caller-owned input files (staged srcPaths) — only session temps.
    bool commit(Error* err) {
        if (!layered_) {
            // Staged-empty (everything removed) compresses an empty input:
            // every codec has a valid empty stream, so an emptied single
            // archive stays openable with one zero-size entry.
            std::string slice;
            std::string src;
            long long srcOff = 0;
            long long srcSize = 0;
            if (staged_.empty()) {
                slice = freshTemp('q');
                FILE* ps = std::fopen(slice.c_str(), "wb");
                if (ps == nullptr) {
                    fail(err, Status::Io, "cannot write temp: " + slice);
                    return false;
                }
                std::fclose(ps);
                src = slice;
            } else {
                const TarStaged& s = staged_[0];
                src = s.fromOrig ? origImage_ : s.srcPath;
                srcOff = s.fromOrig ? s.origOff : 0;
                srcSize = s.size;
            }
            // Fast path: a whole input file compresses directly. Otherwise
            // (a slice of a rewritten image — defensive, normally
            // unreachable since a dirty single session carries a fresh
            // srcPath) materialize the slice first.
            struct stat st {};
            bool whole =
                srcOff == 0 && ::stat(src.c_str(), &st) == 0 && st.st_size == srcSize;
            if (!whole) {
                FILE* pin = std::fopen(src.c_str(), "rb");
                if (pin == nullptr) {
                    fail(err, Status::Io, "cannot read: " + src);
                    return false;
                }
                slice = freshTemp('q');
                FILE* ps = std::fopen(slice.c_str(), "wb");
                bool ok = ps != nullptr && copyRange(pin, srcOff, srcSize, ps);
                if (ps != nullptr) std::fclose(ps);
                std::fclose(pin);
                if (!ok) {
                    std::remove(slice.c_str());
                    fail(err, Status::Io, "cannot read: " + src);
                    return false;
                }
                src = slice;
            }
            buildTmpA_ = freshTemp('s');
            bool ok = compressKindToFile(kind_, src, buildTmpA_, level_);
            if (!slice.empty()) std::remove(slice.c_str());
            if (!ok) {
                fail(err, Status::Io, "cannot compress: " + path_);
                return false;
            }
            if (std::rename(buildTmpA_.c_str(), path_.c_str()) != 0) {
                fail(err, Status::Io,
                     "cannot replace: " + path_ + ": " + std::strerror(errno));
                return false;
            }
            buildTmpA_.clear();
            return true;
        }
        // Tar container (plain or layered over a codec).
        buildTmpA_ = freshTemp('t');
        if (!writeTarImage(staged_, origImage_, buildTmpA_)) {
            fail(err, Status::Io, "cannot build tar image: " + path_);
            return false;
        }
        std::string finalTmp = buildTmpA_;
        if (kind_ != Kind::RawTar) {
            buildTmpB_ = freshTemp('c');
            if (!compressKindToFile(kind_, buildTmpA_, buildTmpB_, level_)) {
                fail(err, Status::Io, "cannot compress: " + path_);
                return false;
            }
            finalTmp = buildTmpB_;
        }
        if (std::rename(finalTmp.c_str(), path_.c_str()) != 0) {
            fail(err, Status::Io, "cannot replace: " + path_ + ": " + std::strerror(errno));
            return false;
        }
        // Layered: finalTmp is the compressed image (B); the uncompressed
        // tar image (A) would otherwise leak as "<target>.aaebuild.t.*.tmp"
        // next to the archive (found by CLI round-trip testing).
        if (finalTmp == buildTmpB_) std::remove(buildTmpA_.c_str());
        buildTmpA_.clear();
        buildTmpB_.clear();
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

    Kind kind_;
    std::string path_;
    std::string format_;
    std::string image_;
    bool ownTemp_ = false;
    bool opened_ = false;
    bool isTar_ = false;
    std::vector<RawEntry> entries_;
    std::string singleName_;
    long long singleSize_ = 0;
    // Write-mode state (fresh create or rewrite; see openForWrite/commit).
    bool writeMode_ = false;
    bool fresh_ = false;
    bool layered_ = false;  // tar container (vs single-file stream)
    bool dirty_ = false;
    bool levelSet_ = false;
    int level_ = 0;
    std::vector<TarStaged> staged_;
    std::string origImage_;
    std::string buildTmpA_;
    std::string buildTmpB_;
};

ISession* streamOpen(Kind kind, const std::string& format, const std::string& fsPath,
                     const WriteOptions& opt, bool writable, bool fresh, Error* err) {
    StreamSession* s = new StreamSession(kind, fsPath, format);
    if (!s->open(opt, writable, fresh, err)) {
        delete s;
        return nullptr;
    }
    return s;
}

// --- probes (magic-first) ---

std::string probeGzip(const std::string& p) {
    unsigned char b[2];
    size_t n = 0;
    if (!readMagic(p, b, sizeof(b), n) || n < 2) return "";
    return (b[0] == 0x1F && b[1] == 0x8B) ? "gz" : "";
}
std::string probeBzip2(const std::string& p) {
    unsigned char b[4];
    size_t n = 0;
    if (!readMagic(p, b, sizeof(b), n) || n < 4) return "";
    return (b[0] == 'B' && b[1] == 'Z' && b[2] == 'h' && b[3] >= '1' && b[3] <= '9') ? "bz2"
                                                                                    : "";
}
std::string probeXz(const std::string& p) {
    unsigned char b[6];
    size_t n = 0;
    if (!readMagic(p, b, sizeof(b), n) || n < 6) return "";
    return (b[0] == 0xFD && b[1] == '7' && b[2] == 'z' && b[3] == 'X' && b[4] == 'Z' &&
            b[5] == 0x00)
               ? "xz"
               : "";
}
std::string probeZstd(const std::string& p) {
    unsigned char b[4];
    size_t n = 0;
    if (!readMagic(p, b, sizeof(b), n) || n < 4) return "";
    return (b[0] == 0x28 && b[1] == 0xB5 && b[2] == 0x2F && b[3] == 0xFD) ? "zst" : "";
}
std::string probeLz4(const std::string& p) {
    unsigned char b[4];
    size_t n = 0;
    if (!readMagic(p, b, sizeof(b), n) || n < 4) return "";
    return (b[0] == 0x04 && b[1] == 0x22 && b[2] == 0x4D && b[3] == 0x18) ? "lz4" : "";
}
std::string probeTar(const std::string& p) {
    unsigned char b[512];
    size_t n = 0;
    if (!readMagic(p, b, sizeof(b), n) || n < 262) return "";
    return isTarImage(b, n) ? "tar" : "";
}

ISession* openGzip(const std::string& p, const WriteOptions& o, bool w, bool f, Error* e) {
    return streamOpen(Kind::Gzip, "gz", p, o, w, f, e);
}
ISession* openBzip2(const std::string& p, const WriteOptions& o, bool w, bool f, Error* e) {
    return streamOpen(Kind::Bzip2, "bz2", p, o, w, f, e);
}
ISession* openXz(const std::string& p, const WriteOptions& o, bool w, bool f, Error* e) {
    return streamOpen(Kind::Xz, "xz", p, o, w, f, e);
}
ISession* openZstd(const std::string& p, const WriteOptions& o, bool w, bool f, Error* e) {
    return streamOpen(Kind::Zstd, "zst", p, o, w, f, e);
}
ISession* openLz4(const std::string& p, const WriteOptions& o, bool w, bool f, Error* e) {
    return streamOpen(Kind::Lz4, "lz4", p, o, w, f, e);
}
ISession* openTar(const std::string& p, const WriteOptions& o, bool w, bool f, Error* e) {
    return streamOpen(Kind::RawTar, "tar", p, o, w, f, e);
}

struct Registrars {
    Registrars() {
        registerBackend(Backend::Gzip, "gz", probeGzip, openGzip);
        registerBackend(Backend::Bzip2, "bz2", probeBzip2, openBzip2);
        registerBackend(Backend::Xz, "xz", probeXz, openXz);
        registerBackend(Backend::Zstd, "zst", probeZstd, openZstd);
        registerBackend(Backend::Lz4, "lz4", probeLz4, openLz4);
        registerBackend(Backend::Tar, "tar", probeTar, openTar);
    }
};
Registrars sRegistrars;  // NOLINT: self-registration on load

}  // namespace stream
}  // namespace aae
