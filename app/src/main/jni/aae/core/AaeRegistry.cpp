// Format registry + session dispatch. No third-party includes here —
// providers self-register via aae::registerBackend().
#include "Aae.h"

#include <cstring>
#include <map>

namespace aae {
namespace {

struct BackendOps {
    std::string formatId;
    ProbeFn probe = nullptr;
    OpenFn open = nullptr;
};

std::map<int, BackendOps>& backends() {
    static std::map<int, BackendOps> sBackends;
    return sBackends;
}

bool endsWithFold(const std::string& name, const char* sfx) {
    size_t nl = name.size(), sl = std::strlen(sfx);
    if (nl <= sl) return false;
    for (size_t i = 0; i < sl; ++i) {
        char a = name[nl - sl + i], b = sfx[i];
        if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if (a != b) return false;
    }
    return true;
}

// Fresh-create routing by target name. Probing is impossible (the file does
// not exist yet), so the extension picks the backend; layered tar names
// (.tar.gz/.tgz/…) land on their codec backend, which sniffs layered-vs-
// single itself at open. Unknown extensions keep the historical behavior
// (ZIP) so extension-less temp files still work.
Backend backendForFreshName(const std::string& fsPath) {
    if (endsWithFold(fsPath, ".tar")) return Backend::Tar;
    if (endsWithFold(fsPath, ".tar.gz") || endsWithFold(fsPath, ".tgz")) return Backend::Gzip;
    if (endsWithFold(fsPath, ".tar.bz2") || endsWithFold(fsPath, ".tbz2")) return Backend::Bzip2;
    if (endsWithFold(fsPath, ".tar.xz") || endsWithFold(fsPath, ".txz")) return Backend::Xz;
    if (endsWithFold(fsPath, ".tar.zst") || endsWithFold(fsPath, ".tzst")) return Backend::Zstd;
    if (endsWithFold(fsPath, ".tar.lz4") || endsWithFold(fsPath, ".tlz4")) return Backend::Lz4;
    if (endsWithFold(fsPath, ".gz")) return Backend::Gzip;
    if (endsWithFold(fsPath, ".bz2")) return Backend::Bzip2;
    if (endsWithFold(fsPath, ".xz")) return Backend::Xz;
    if (endsWithFold(fsPath, ".zst")) return Backend::Zstd;
    if (endsWithFold(fsPath, ".lz4")) return Backend::Lz4;
    if (endsWithFold(fsPath, ".7z")) return Backend::SevenZ;
    return Backend::Zip;
}

}  // namespace

void registerBackend(Backend backend, const std::string& formatId, ProbeFn probe, OpenFn open) {
    BackendOps ops;
    ops.formatId = formatId;
    ops.probe = probe;
    ops.open = open;
    backends()[static_cast<int>(backend)] = ops;
}

std::string version() {
    std::string v = "aae/0.1 (";
#ifdef AAE_HAVE_LIBZIP
    v += "zip";
#else
    v += "no-zip";
#endif
#ifdef AAE_HAVE_MBEDTLS
    v += "+mbedtls";  // WinZip-AES + PKWARE crypto actually linked in
#else
    v += "+no-crypto";
#endif
#ifdef AAE_HAVE_LIBARCHIVE
    v += "+archive";
#else
    v += "+no-archive";
#endif
    v += "+stream";  // gzip/bzip2/xz/zstd/lz4/tar (AaeStream.cpp, always on)
    v += "+7zrw";     // 7z read+write (AaeSevenZ.cpp, always on)
    v += ")";
    return v;
}

std::vector<std::string> supportedFormats() {
    std::vector<std::string> out;
#ifdef AAE_HAVE_LIBZIP
    out.emplace_back("zip");
#endif
#ifdef AAE_HAVE_LIBARCHIVE
    // Read-only rar/cab/iso (AaeArchive.cpp). 7z is intentionally NOT here:
    // it stays with the sevenzip decoder. ar/xar/warc readers exist in-tree
    // but have no probe yet, so they stay unadvertised.
    out.emplace_back("rar");
    out.emplace_back("cab");
    out.emplace_back("iso");
#endif
    // Stream backends are unconditional in this build (zlib from the NDK,
    // everything else vendored): tar + single-file gz/bz2/xz/zst/lz4.
    out.emplace_back("tar");
    out.emplace_back("gz");
    out.emplace_back("bz2");
    out.emplace_back("xz");
    out.emplace_back("zst");
    out.emplace_back("lz4");
    // 7z read+write codec (AaeSevenZ.cpp over thirdparty/sevenzip).
    // Listed once: the earlier revision pushed "7z" twice (archive stub +
    // sevenz), which confused capability discovery.
    out.emplace_back("7z");
    return out;
}

std::string probe(const std::string& fsPath) {
    for (const auto& [key, ops] : backends()) {
        if (ops.probe == nullptr) continue;
        std::string hit = ops.probe(fsPath);
        if (!hit.empty()) return hit;
    }
    return "";
}

ISession* openSession(const std::string& fsPath, const WriteOptions& opt, bool writable,
                      bool fresh, Error* err) {
    const std::string hit = probe(fsPath);
    // Fresh-create skips probing (target may not exist yet) and routes by
    // target extension: zip/tar/7z/single-stream/layered-tar backends all
    // support creation now.
    if (fresh) {
        auto it = backends().find(static_cast<int>(backendForFreshName(fsPath)));
        if (it != backends().end() && it->second.open != nullptr) {
            return it->second.open(fsPath, opt, writable, fresh, err);
        }
    } else {
        for (const auto& [key, ops] : backends()) {
            if (ops.probe == nullptr || ops.open == nullptr) continue;
            if (ops.probe(fsPath) == hit && !hit.empty()) {
                return ops.open(fsPath, opt, writable, fresh, err);
            }
        }
    }
    if (err != nullptr) {
        if (hit.empty()) {
            err->set(Status::NotArchive, "not a supported archive: " + fsPath);
        } else {
            err->set(Status::UnsupportedFormat, "no backend for format: " + hit);
        }
    }
    return nullptr;
}

bool isSafeEntryName(const std::string& name) {
    if (name.empty() || name[0] == '/' || name[0] == '\\') return false;
    if (name.find('\\') != std::string::npos) return false;
    size_t start = 0;
    while (start <= name.size()) {
        size_t end = name.find('/', start);
        if (end == std::string::npos) end = name.size();
        std::string seg = name.substr(start, end - start);
        if (seg == "." || seg == "..") return false;
        if (end == name.size()) break;
        start = end + 1;
    }
    return true;
}

}  // namespace aae
