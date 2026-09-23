<div align="center">

# Android-Archive-Engine (AAE)

[![Typing SVG](https://readme-typing-svg.herokuapp.com?font=Fira+Code&weight=600&size=20&duration=3000&pause=800&color=3FB950&center=true&vCenter=true&width=700&lines=Native+archive+engine+for+Android;ZIP+%E2%80%A2+7-ZIP+%E2%80%A2+TAR+%E2%80%A2+GZ+%E2%80%A2+XZ+%E2%80%A2+ZSTD+%E2%80%A2+LZ4;Single+.so%2C+zero+permissions%2C+SAF-native;WIP+%E2%80%94+API+freezing%2C+formats+landing)](https://github.com/Neo-Mods1/Android-Archive-Engine)

[![WIP](https://img.shields.io/badge/status-WIP-orange?style=for-the-badge&logo=android)](https://github.com/Neo-Mods1/Android-Archive-Engine)
[![Platform](https://img.shields.io/badge/platform-Android_21%2B-3DDC84?style=for-the-badge&logo=android)](https://developer.android.com)
[![Language](https://img.shields.io/badge/Java_7_%2B_C%2B%2B17-blue?style=for-the-badge&logo=coffeescript)](.)
[![Native](https://img.shields.io/badge/native-libaae.so-ff69b4?style=for-the-badge&logo=c)](app/src/main/jni)
[![License](https://img.shields.io/badge/license-GPLv3-blue?style=for-the-badge&logo=gnu)](LICENSE)
[![ABI](https://img.shields.io/badge/ABI-arm64--v8a-purple?style=for-the-badge&logo=arm)](app/build.gradle)

**One JNI engine for everything archive on Android — create, list, extract, mutate — with no storage permissions.**

_No `java.util.zip` fallbacks. No silent behavior drift. If `libaae.so` is missing, writes fail loudly with `UNSUPPORTED_OPERATION` instead of producing a different file behind your back._

[Features](#features) • [Architecture](#architecture) • [Format matrix](#format-support-matrix) • [Quickstart](#quickstart) • [API](#api-reference) • [Build](#build-from-source) • [Roadmap](#roadmap-wip)

</div>

---

## What is this?

**AAE** is a native archive engine (`libaae.so`) + a thin Java 7 facade (`bin.nt.aae`) + a SAF-native demo app. It replaces the usual patchwork of `ZipFile` / Apache Commons / shell `tar` calls with one capability-driven backend:

```
Demo app (UI only)          Java facade (Java 7)         libaae.so (C++17)
-------------------         --------------------         -----------------
MainActivity                Aae                          AaeRegistry
  (tester + compressor)       (sessions + one-shots)       (probe + dispatch)
        |                     AaeCapabilities                    |
        v                     (write ground truth)               v
AaeRunner ------------------> AaeSession                   +-- AaeZip (libzip, AES)
  (worker thread)               (list/read/mutate)         +-- AaeStream (tar/streams)
        ^                                                    +-- AaeSevenZ (LZMA)
        |                                                    +-- AaeArchive (stub)
SafHelper
  (SAF to cache staging)
```

> **Design rule:** `AaeCapabilities` is the only truth about what can be written. The UI never guesses — it asks the engine, validates up front, and the native layer re-validates. Nothing reaches JNI that the backend would reject.

---

## Features

| Area | What you get |
|------|--------------|
| 13 writable formats | `zip`, `7z`, `tar`, `tar.gz/.bz2/.xz/.zst/.lz4`, `gz`, `bz2`, `xz`, `zst`, `lz4` |
| Real encryption | ZIP WinZip-AES-128/256 via mbedTLS 3.6.3. Streams/7z reject passwords loudly instead of dropping them silently |
| Session model | `open()` / `openWritable()` / `create()` + one-shot helpers. Writable sessions `close()` to commit, `discard()` to abandon |
| Secure by default | Zip-slip checked on every extract, `checkEntryName()` mirrors native `isSafeEntryName`, no `WRITE_EXTERNAL_STORAGE` — SAF only |
| SAF-native demo | `ACTION_OPEN_DOCUMENT` / `TREE` / `CREATE_DOCUMENT` with cache staging, atomic publish, per-file progress, MT-Manager-style level dialog |
| Vendored, reproducible | libzip, libzippp, bzip2, liblzma (xz), zstd, lz4, 7-Zip C-SDK + prebuilt mbedTLS — every source pinned with `VENDORED_FROM.txt` + SHA256 |
| Tester tab | Probe, list, read-bytes preview, extract, password matrix, round-trip matrix, diagnostics — all on a worker thread, never UI |
| One .so | `arm64-v8a`, `-Wl,--gc-sections --strip-all`, hidden visibility. No transitive Gradle deps |

---

## Format support matrix

Ground truth traced through `AaeCapabilities.java` → `Aae.h` → `providers/`:

| Format | Read | Write | Methods | Encryption | Notes |
|--------|------|-------|---------|------------|-------|
| `zip` | yes | yes | STORE, DEFLATE, BZIP2, XZ, ZSTD, DEFAULT | AES-128/256 | Full R/W via libzip |
| `7z` | yes | yes | STORE (Copy), LZMA 1..9 | no (rejected) | Non-solid, LZMA or Copy |
| `tar` | yes | yes | STORE | no (rejected) | Sorted entries, real mtimes |
| `tar.gz` / `gz` | yes | yes | DEFLATE 0..9 | no (rejected) | Tar-over-gzip / single stream |
| `tar.bz2` / `bz2` | yes | yes | BZIP2 1..9 | no (rejected) | Tar-over-bzip2 / single stream |
| `tar.xz` / `xz` | yes | yes | XZ preset 0..9 | no (rejected) | liblzma, default preset 6 |
| `tar.zst` / `zst` | yes | yes | ZSTD 1..22 | no (rejected) | `0` = zstd default |
| `tar.lz4` / `lz4` | yes | yes | LZ4 1..12 (3+ = HC) | no (rejected) | LZ4 frames |
| `rar` / `cab` / `iso` | opt-in | no | — | — | Stub `AaeArchive.cpp` until libarchive vendored (`AAE_HAVE_LIBARCHIVE=1`, v3.7.4) |

| Component | Files | Share |
|-----------|------:|-------|
| libzip | 121 | ######################################## (46%) |
| liblzma / xz | 79 | ############################## (30%) |
| zstd | 26 | ########## (10%) |
| 7-Zip C-SDK | 23 | ######### (9%) |
| bzip2 | 7 | ### (3%) |
| engine + libzippp | 7 | ### (3%) |
| lz4 | 3 | # (1%) |

### Level semantics are per-method

| Method | Allowed levels | Default |
|--------|---------------|---------|
| STORE | `0` only (no compression) | 0 |
| DEFAULT | `0` only (backend decides) | 0 |
| DEFLATE / BZIP2 | `0` (default) or `1..9` | 6 / 0 |
| XZ / LZMA | `0..9` (liblzma presets) | 6 (XZ), 5 (LZMA) |
| ZSTD | `0` (default) or `1..22` | 0 |
| LZ4 | `0` (fast) or `1..12` (`3+` uses LZ4HC) | 0 |

MT-Manager dialog mapping (`mtLevelsFor` / `mtOptions`): `Store`, `Fastest`, `Fast`, `Normal`, `Maximum`, `Ultra`, (+ `APK mode` for ZIP which maps to STORE). ZIP + password selects AES-256 automatically.

---

## Quickstart

**Requirements:** `minSdk 21`, `compileSdk 30`, NDK + ndk-build, `arm64-v8a` device. Java 7 source level (AIDE-safe).

### 1. Probe + list

```java
import bin.nt.aae.*;
import java.io.File;
import java.util.List;

File archive = new File("/sdcard/Download/sample.zip");

AaeFormat format = Aae.probe(archive);          // never throws, UNKNOWN if alien
String version = Aae.version();                 // e.g. "aae/0.1 (zip+mbedtls+no-archive)"
List<String> codecs = Aae.supportedFormats();   // e.g. [zip]

List<AaeEntry> entries = Aae.listEntries(archive, null);
for (AaeEntry e : entries) {
    // e.path, e.size, e.compressedSize, e.isDirectory, e.mtime, e.method, e.encrypted
}
```

### 2. Extract (progress + cancel)

```java
AaeSession s = Aae.open(archive, passwordOrNull);
try {
    s.extractAll(destDir, new AaeProgressListener() {
        @Override public boolean onProgress(int fi, int fc, long done, long total) {
            updateBar(fi, fc, done, total);
            return !cancelled; // return false to cancel -> AaeException(CANCELLED)
        }
    });
} finally {
    s.close(); // read-only close just releases
}
```

### 3. Create

```java
List<AaeInput> inputs = new ArrayList<AaeInput>();
inputs.add(new AaeInput("docs/", new File("/sdcard/Docs")));   // dirs recursed, sorted
inputs.add(new AaeInput("readme.txt", readmeFile));

AaeWriteOptions opts = new AaeWriteOptions(
    AaeMethod.ZSTD, 6, null, AaeEncryption.NONE);

File out = Aae.create(new File(cacheDir, "out.zip"), inputs, opts, AaeFormat.ZIP);
```

### 4. Mutate in place

```java
AaeSession s = Aae.openWritable(archive, null, AaeEncryption.NONE);
try {
    s.addFile("new/note.txt", noteFile, new AaeWriteOptions(AaeMethod.DEFLATE, 6, null, AaeEncryption.NONE));
    s.addDirectory("empty-dir");
    s.rename("old.txt", "renamed.txt");
    s.delete("junk.tmp");
    s.setComment("packed with AAE");
    s.close();   // commit atomically
} catch (AaeException e) {
    s.discard(); // abandon
    if (e.kind == AaeException.Kind.UNSAFE_NAME) { /* zip-slip attempt */ }
}
```

### 5. Encrypted ZIP (AES-256)

```java
AaeWriteOptions enc = new AaeWriteOptions(
    AaeMethod.DEFLATE, 9, "s3cret", AaeEncryption.AES_256);
Aae.create(outZip, inputs, enc, AaeFormat.ZIP);

// reading needs the password, wrong password -> PASSWORD_WRONG, missing -> PASSWORD_REQUIRED
Aae.listEntries(outZip, "s3cret");
```

---

## API reference

| Class | Role |
|-------|------|
| `Aae` | Entry point: `version()`, `supportedFormats()`, `probe()`, `open/create`, one-shots (`listEntries`, `info`, `readBytes`, `extractEntry/All`, `addEntries`, `deleteEntries`, `renameEntry`), `checkEntryName` / `resolveSafe` |
| `AaeSession` | Open handle: `list/info/readBytes/extractTo/extractAll/addFile/addDirectory/delete/rename/setComment/close/discard` — single-threaded, worker only |
| `AaeCapabilities` | `forFormat()`, `writableFormats()`, `readableFormats()`, `validateWrite()`, `mtLevelsFor()`, `mtOptions()`, `isSingleFileFormat()`, `isLayeredTarFormat()` |
| `AaeFormat` | `ZIP TAR TAR_GZ TAR_BZ2 TAR_XZ TAR_ZST TAR_LZ4 SEVEN_Z RAR GZ BZ2 XZ ZST CAB ISO LZ4 UNKNOWN` |
| `AaeMethod` | `DEFAULT STORE DEFLATE BZIP2 XZ ZSTD LZ4 LZMA` (wire ints to native, ordinals stable) |
| `AaeEncryption` | `NONE AES_128 AES_256` |
| `AaeWriteOptions` | `(method, level, password, encryption)` — fail-fast level validation per method |
| `AaeException.Kind` | `IO NOT_ARCHIVE UNSUPPORTED_FORMAT ENTRY_NOT_FOUND PASSWORD_REQUIRED PASSWORD_WRONG UNSAFE_NAME CANCELLED TOO_LARGE UNSUPPORTED_OPERATION` |

> Single-file formats (`gz/bz2/xz/zst/lz4`) hold exactly one file — `create()` with anything but one input fails with `UNSUPPORTED_OPERATION` instead of silently packing the wrong container.

---

## Project structure

```
AAE/
├── app/
│   ├── build.gradle                  # arm64-v8a, ndkBuild, sdk 21/30
│   └── src/main/
│       ├── AndroidManifest.xml       # zero storage permissions (SAF only)
│       ├── java/bin/nt/aae/         # engine facade — Aae, AaeSession, Caps, Formats...
│       ├── java/com/aae/androidlib/ # demo app — MainActivity, AaeRunner, SafHelper
│       └── jni/
│           ├── Android.mk            # ~266 sources, gc-sections, hidden visibility
│           ├── Application.mk
│           ├── aae/core/AaeRegistry.cpp
│           ├── aae/providers/        # AaeZip / AaeStream / AaeSevenZ / AaeArchive(stub)
│           ├── aae/jni/AaeJni.cpp
│           └── thirdparty/           # vendored: libzip libzippp bz2 xz zstd lz4 sevenzip mbedtls
├── .github/workflows/external-libs.yml  # fetch submodules to 7z artifact (manual dispatch)
├── .gitmodules                       # optional libarchive v3.7.4 (RAR/CAB/ISO read)
├── gradle/ gradlew* settings.gradle gradle.properties
└── LICENSE (GPLv3)
```

---

## Build from source

```bash
# 1. clone (plain clone works — thirdparty is vendored in-tree)
git clone https://github.com/Neo-Mods1/Android-Archive-Engine.git
cd Android-Archive-Engine

# 2a. Android Studio: open, let Gradle sync, Run (ndk-build runs automatically)
# 2b. CLI:
./gradlew :app:assembleDebug
# output: app/build/outputs/apk/debug/app-debug.apk  (arm64-v8a)

# 2c. AIDE (on-device): import project, build — Java 7 source level throughout
```

**Optional — RAR/CAB/ISO read backend (libarchive, off by default):**

```bash
# one-time (needs network, run once per clone):
git submodule add -b v3.7.4 https://github.com/libarchive/libarchive.git external/libarchive
git submodule update --init --recursive
# then build with AAE_HAVE_LIBARCHIVE=1 + hand-written
# thirdparty/gen-libarchive/config.h (see Android.mk header comment).
# Afterwards: git clone --recurse-submodules <your-fork-url>
```

> No network fetch at build time. No Maven deps (`implementation fileTree(libs)` only). The `.github/workflows/external-libs.yml` exists to pack `external/` into a version-pinned `external-libs.7z` artifact on demand.

---

## Vendored third-party

Each dir carries a `VENDORED_FROM.txt` with URL + SHA256 + kept/dropped file list:

| Dep | Version | License | Serves |
|-----|---------|---------|--------|
| libzip + libzippp | libzip tree + `LIBZIPPP_WITH_ENCRYPTION` | BSD-3 | ZIP R/W |
| mbedTLS | 3.6.3 prebuilt `libmbedcrypto.a` (arm64) | Apache-2.0 | WinZip-AES |
| xz / liblzma | 5.8.1 | 0BSD | `.xz` streams + ZIP-XZ entries |
| zstd | 1.5.7 (via zstd-jni bundle) | BSD-3 | `.zst` + ZIP-ZSTD |
| lz4 | 1.10.0 | BSD-2 | `.lz4` frames |
| bzip2 | 1.x | BSD-like | `.bz2` + ZIP-BZIP2 |
| 7-Zip C-SDK | LZMA SDK 24.x C tree (via 7-Zip-JBinding bundle) | Public domain | 7z reader + LZMA writer |
| libarchive (submodule, opt-in) | v3.7.4 | BSD-2 | RAR/RAR5/CAB/ISO read |

Note on codec ownership — do not deduplicate by name: `sevenzip/LzmaDec` serves the 7z reader, `xz/liblzma` serves `.xz` streams and ZIP-XZ entries — both are required. `lz4frame.c` resolves `xxhash.h` to `zstd/common/xxhash.h` (namespaced `ZSTD_`, intentional).

---

## Security model

- Zip-slip impossible by construction: `resolveSafe()` + native `isSafeEntryName` reject leading `/`, backslash, and `.`/`..` segments on write and extract.
- No permission creep: manifest requests zero storage permissions; all file access goes through SAF Uris with cache staging for the native engine.
- Passwords never silent: stream/7z formats have no password concept — a non-empty password is rejected (`UNSUPPORTED_OPERATION`), never dropped.
- Typed errors: every failure is `AaeException` with a stable `Kind` — branch on it, not on message strings.

---

## Roadmap (WIP)

- [x] ZIP full R/W (STORE/DEFLATE/BZIP2/XZ/ZSTD + AES-128/256)
- [x] TAR + layered `tar.*` + single-file streams R/W
- [x] 7z read (LZMA/LZMA2/PPMD/BCJ/Delta/Copy) + LZMA/Copy write, non-solid
- [x] Capability model + MT-style level dialog + SAF demo app
- [ ] RAR/RAR5/CAB/ISO read via libarchive submodule (stub ready, `AAE_HAVE_LIBARCHIVE`)
- [ ] 7z AES decrypt (needs C++ Crypto path — C decoder has none)
- [ ] Solid 7z, multi-volume, split-output parity in engine
- [ ] `armeabi-v7a`/`x86_64` mbedTLS prebuilts (currently arm64-only)
- [ ] Public Maven artifact (`bin.nt:aae`) + API freeze + CTS matrix
- [ ] Screenshots / demo recording (demo app is functional — capture pending)

Contributions welcome — open an issue before large native changes (codec ownership is subtle).

---

## License

GPLv3 — see [LICENSE](LICENSE). Vendored third-party code keeps its own licenses (BSD/0BSD/Apache-2.0/Public Domain — see each `VENDORED_FROM.txt`).

---

<div align="center">

![Progress](https://img.shields.io/badge/progress-70%25-yellow?style=flat-square)
![Engine](https://img.shields.io/badge/engine-libaae.so-blue?style=flat-square)
![SAF](https://img.shields.io/badge/IO-SAF_only-green?style=flat-square)

**Built with NDK. No cloud. No trackers. Just archives.**

Star it if you want libarchive + Maven Central to land next.

![Stats](https://github-readme-stats.vercel.app/api/pin/?username=Neo-Mods1&repo=Android-Archive-Engine&theme=dark)

</div>
