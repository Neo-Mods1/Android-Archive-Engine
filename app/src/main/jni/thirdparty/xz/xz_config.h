/* xz_config.h — hand-written for Android (bionic, LP64 arm64, API 21+).
 * Force-included via `-include xz_config.h` (never named config.h: that
 * would clash with libzip's config.h on the shared include path, and xz's
 * sysdefs.h works fine without HAVE_CONFIG_H).
 * Full codec set (all encoders/decoders/checks), POSIX threads.
 */
#pragma once

#define HAVE_STDBOOL_H 1
#define HAVE_INTTYPES_H 1
#define HAVE_STDINT_H 1
#define SIZEOF_SIZE_T 8

/* Threading: bionic pthreads (mythread.h picks this up). */
#define MYTHREAD_POSIX 1

/* Integrity checks. */
#define HAVE_CHECK_CRC32 1
#define HAVE_CHECK_CRC64 1
#define HAVE_CHECK_SHA256 1

/* Clocks for the threaded outqueue (all present in bionic, API 21+). */
#define HAVE_CLOCK_GETTIME 1
#define HAVE_CLOCK_MONOTONIC 1
#define HAVE_PTHREAD_CONDATTR_SETCLOCK 1

/* Full filter set, encoders + decoders. */
#define HAVE_ENCODER_X86 1
#define HAVE_DECODER_X86 1
#define HAVE_ENCODER_POWERPC 1
#define HAVE_DECODER_POWERPC 1
#define HAVE_ENCODER_IA64 1
#define HAVE_DECODER_IA64 1
#define HAVE_ENCODER_ARM 1
#define HAVE_DECODER_ARM 1
#define HAVE_ENCODER_ARMTHUMB 1
#define HAVE_DECODER_ARMTHUMB 1
#define HAVE_ENCODER_ARM64 1
#define HAVE_DECODER_ARM64 1
#define HAVE_ENCODER_SPARC 1
#define HAVE_DECODER_SPARC 1
#define HAVE_ENCODER_RISCV 1
#define HAVE_DECODER_RISCV 1
#define HAVE_ENCODER_DELTA 1
#define HAVE_DECODER_DELTA 1
#define HAVE_ENCODER_LZMA1 1
#define HAVE_DECODER_LZMA1 1
#define HAVE_ENCODER_LZMA2 1
#define HAVE_DECODER_LZMA2 1

/* LZMA match finders (lz_encoder.c / lz_encoder_mf.c test these; without
 * them the encoder silently loses finder choices). */
#define HAVE_MF_BT2 1
#define HAVE_MF_BT3 1
#define HAVE_MF_BT4 1
#define HAVE_MF_HC3 1
#define HAVE_MF_HC4 1

/* lzip (.lz) decoder in auto_decoder (tiny, no extra deps). */
#define HAVE_LZIP_DECODER 1

/* arm64 tolerates unaligned access; lets rangecoder/fastpos use fast paths. */
#define TUKLIB_FAST_UNALIGNED_ACCESS 1

/* Core count / physical memory via sysconf (bionic has both since API 21;
 * used by the threaded encoder sizing). */
#define TUKLIB_CPUCORES_SYSCONF 1
#define TUKLIB_PHYSMEM_SYSCONF 1

/* Byteswap (bionic byteswap.h) + compiler builtins (NDK clang). */
#define HAVE_BSWAP_16 1
#define HAVE_BSWAP_32 1
#define HAVE_BSWAP_64 1
#define HAVE_BYTESWAP_H 1
#define HAVE___BUILTIN_BSWAPXX 1
#define HAVE___BUILTIN_ASSUME_ALIGNED 1

/* bionic headers (tuklib_integer.h / common sources test these). The
 * SYS_BYTEORDER branch is dead code behind HAVE_BYTESWAP_H; defined only
 * to match the proven upstream Android configuration. */
#define HAVE_SYS_ENDIAN_H 1
#define HAVE_SYS_PARAM_H 1
#define HAVE_SYS_BYTEORDER_H 1

/* __attribute__((constructor)) for internal one-time init (NDK clang). */
#define HAVE_FUNC_ATTRIBUTE_CONSTRUCTOR 1
#define HAVE__BOOL 1

/* arm64 hardware CRC32: OFF on this toolchain. crc32_arm64.h enables it
 * via __attribute__((__target__("+crc"))), which this NDK's clang 7 does
 * not implement for AArch64 (it silently treats __crc32b/__crc32h as
 * external calls -> undefined references at link). Portable CRC is used
 * instead. Re-enable HAVE_ARM64_CRC32 (+ HAVE_GETAUXVAL and HWCAP_CRC32
 * =(1<<7), which API-21-era bionic lacks) once the toolchain moves to a
 * clang with AArch64 function-multiversioning support. */

/* Deliberately OFF (verified against these 5.8.1 sources, not just copied):
 * - HAVE_DECODERS/HAVE_ENCODERS, HAVE_HWCAP_CRC32, HAVE_MBRTOWC,
 *   HAVE_WCWIDTH: not tested anywhere in this tree (older-xz or CLI-only
 *   macros) — defining them would be cargo-cult.
 * - HAVE__MM_MOVEMASK_EPI8, HAVE_CPUID_H, HAVE_IMMINTRIN_H,
 *   HAVE_USABLE_CLMUL, x86 ASM files: x86-only; this build is arm64-only.
 * - HAVE_VISIBILITY: would export lzma_* from libaae.so; the static link
 *   is intentionally hidden (see Android.mk -fvisibility=hidden).
 * - NDEBUG: kept off so liblzma internal asserts stay live in diagnostics.
 * - _FILE_OFFSET_BITS: no-op on LP64 (off_t is already 64-bit).
 * - TUKLIB_SYMBOL_PREFIX/HAVE_CONFIG_H: static link needs neither; this
 *   header is force-included instead of a generated config.h. */
