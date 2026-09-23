LOCAL_PATH := $(call my-dir)

# Prebuilt mbedTLS 3.6.3 (arm64-v8a) — crypto backend for libzip.
include $(CLEAR_VARS)
LOCAL_MODULE := mbedcrypto
LOCAL_SRC_FILES := thirdparty/mbedtls/lib/libmbedcrypto.a
LOCAL_EXPORT_C_INCLUDES := $(LOCAL_PATH)/thirdparty/mbedtls/include
include $(PREBUILT_STATIC_LIBRARY)

# aae archive engine: core + providers + JNI, libzip, libzippp, bzip2.
# Source list mirrors aae/CMakeLists.txt + libzip's lib/CMakeLists.txt
# (base 111 + stdio/unix + bzip2 + mbedtls + winzip-aes + generated).
#
# Codec ownership (do NOT "deduplicate" by name):
# - thirdparty/sevenzip LzmaDec/Lzma2Dec serve the 7z reader (AaeSevenZ.cpp).
# - thirdparty/xz liblzma serves .xz streams (AaeStream.cpp) AND zip XZ
#   entries (libzip zip_algorithm_xz.c). Both are required.
# - thirdparty/lz4: all 3 files kept — lz4frame.c references lz4hc symbols
#   even for decompression-only builds.
# - thirdparty/zstd dictBuilder (cover/divsufsort/fastcover/zdict) is
#   dictionary training only; nothing references it, so it is excluded
#   (the reference Android port globs common+compress+decompress and
#   excludes dictBuilder/deprecated/legacy the same way).
include $(CLEAR_VARS)
LOCAL_MODULE := aae

LOCAL_CFLAGS := -w -s -Wno-error=format-security -fvisibility=hidden -fpermissive -fexceptions
LOCAL_CFLAGS += -DAAE_HAVE_LIBZIP=1 -DAAE_HAVE_LIBZIPPP=1 -DAAE_HAVE_MBEDTLS=1
# Single-threaded LZMA encoder only (see sevenzip note below).
LOCAL_CFLAGS += -D_7ZIP_ST=1
LOCAL_CPPFLAGS := -w -s -Wno-error=format-security -fvisibility=hidden -Werror -std=c++17
LOCAL_CPPFLAGS += -Wno-error=c++11-narrowing -fpermissive -Wall -fexceptions
LOCAL_CPPFLAGS += -DLIBZIPPP_WITH_ENCRYPTION
LOCAL_CFLAGS += -include $(LOCAL_PATH)/thirdparty/xz/xz_config.h \
	-DZSTD_MULTITHREAD=1
LOCAL_LDFLAGS += -Wl,--gc-sections,--strip-all,-llog
LOCAL_LDLIBS := -llog -landroid -lz

LOCAL_C_INCLUDES += $(LOCAL_PATH)/aae \
	$(LOCAL_PATH)/thirdparty/libzip \
	$(LOCAL_PATH)/thirdparty/gen \
	$(LOCAL_PATH)/thirdparty/libzippp \
	$(LOCAL_PATH)/thirdparty/bz2 \
	$(LOCAL_PATH)/thirdparty/lz4 \
	$(LOCAL_PATH)/thirdparty/sevenzip \
	$(LOCAL_PATH)/thirdparty/mbedtls/include \
	$(LOCAL_PATH)/thirdparty/xz/src/liblzma/api \
	$(LOCAL_PATH)/thirdparty/xz/src/liblzma/common \
	$(LOCAL_PATH)/thirdparty/xz/src/liblzma/check \
	$(LOCAL_PATH)/thirdparty/xz/src/liblzma/lz \
	$(LOCAL_PATH)/thirdparty/xz/src/liblzma/rangecoder \
	$(LOCAL_PATH)/thirdparty/xz/src/liblzma/lzma \
	$(LOCAL_PATH)/thirdparty/xz/src/liblzma/delta \
	$(LOCAL_PATH)/thirdparty/xz/src/liblzma/simple \
	$(LOCAL_PATH)/thirdparty/xz/src/common \
	$(LOCAL_PATH)/thirdparty/zstd \
	$(LOCAL_PATH)/thirdparty/zstd/common

# engine C++ (7 files)
LOCAL_SRC_FILES += aae/core/AaeRegistry.cpp \
	aae/providers/AaeZip.cpp \
	aae/providers/AaeArchive.cpp \
	aae/jni/AaeJni.cpp \
	aae/providers/AaeStream.cpp \
	aae/providers/AaeSevenZ.cpp \
	thirdparty/libzippp/libzippp.cpp

# libzip (121 files)
LOCAL_SRC_FILES += thirdparty/libzip/zip_add.c \
	thirdparty/libzip/zip_add_dir.c \
	thirdparty/libzip/zip_add_entry.c \
	thirdparty/libzip/zip_algorithm_deflate.c \
	thirdparty/libzip/zip_buffer.c \
	thirdparty/libzip/zip_close.c \
	thirdparty/libzip/zip_delete.c \
	thirdparty/libzip/zip_dir_add.c \
	thirdparty/libzip/zip_dirent.c \
	thirdparty/libzip/zip_discard.c \
	thirdparty/libzip/zip_entry.c \
	thirdparty/libzip/zip_error.c \
	thirdparty/libzip/zip_error_clear.c \
	thirdparty/libzip/zip_error_get.c \
	thirdparty/libzip/zip_error_get_sys_type.c \
	thirdparty/libzip/zip_error_strerror.c \
	thirdparty/libzip/zip_error_to_str.c \
	thirdparty/libzip/zip_extra_field.c \
	thirdparty/libzip/zip_extra_field_api.c \
	thirdparty/libzip/zip_fclose.c \
	thirdparty/libzip/zip_fdopen.c \
	thirdparty/libzip/zip_file_add.c \
	thirdparty/libzip/zip_file_error_clear.c \
	thirdparty/libzip/zip_file_error_get.c \
	thirdparty/libzip/zip_file_get_comment.c \
	thirdparty/libzip/zip_file_get_external_attributes.c \
	thirdparty/libzip/zip_file_get_offset.c \
	thirdparty/libzip/zip_file_rename.c \
	thirdparty/libzip/zip_file_replace.c \
	thirdparty/libzip/zip_file_set_comment.c \
	thirdparty/libzip/zip_file_set_encryption.c \
	thirdparty/libzip/zip_file_set_external_attributes.c \
	thirdparty/libzip/zip_file_set_mtime.c \
	thirdparty/libzip/zip_file_strerror.c \
	thirdparty/libzip/zip_fopen.c \
	thirdparty/libzip/zip_fopen_encrypted.c \
	thirdparty/libzip/zip_fopen_index.c \
	thirdparty/libzip/zip_fopen_index_encrypted.c \
	thirdparty/libzip/zip_fread.c \
	thirdparty/libzip/zip_fseek.c \
	thirdparty/libzip/zip_ftell.c \
	thirdparty/libzip/zip_get_archive_comment.c \
	thirdparty/libzip/zip_get_archive_flag.c \
	thirdparty/libzip/zip_get_encryption_implementation.c \
	thirdparty/libzip/zip_get_file_comment.c \
	thirdparty/libzip/zip_get_name.c \
	thirdparty/libzip/zip_get_num_entries.c \
	thirdparty/libzip/zip_get_num_files.c \
	thirdparty/libzip/zip_hash.c \
	thirdparty/libzip/zip_io_util.c \
	thirdparty/libzip/zip_libzip_version.c \
	thirdparty/libzip/zip_memdup.c \
	thirdparty/libzip/zip_name_locate.c \
	thirdparty/libzip/zip_new.c \
	thirdparty/libzip/zip_open.c \
	thirdparty/libzip/zip_pkware.c \
	thirdparty/libzip/zip_progress.c \
	thirdparty/libzip/zip_realloc.c \
	thirdparty/libzip/zip_rename.c \
	thirdparty/libzip/zip_replace.c \
	thirdparty/libzip/zip_set_archive_comment.c \
	thirdparty/libzip/zip_set_archive_flag.c \
	thirdparty/libzip/zip_set_default_password.c \
	thirdparty/libzip/zip_set_file_comment.c \
	thirdparty/libzip/zip_set_file_compression.c \
	thirdparty/libzip/zip_set_name.c \
	thirdparty/libzip/zip_source_accept_empty.c \
	thirdparty/libzip/zip_source_begin_write.c \
	thirdparty/libzip/zip_source_begin_write_cloning.c \
	thirdparty/libzip/zip_source_buffer.c \
	thirdparty/libzip/zip_source_call.c \
	thirdparty/libzip/zip_source_close.c \
	thirdparty/libzip/zip_source_commit_write.c \
	thirdparty/libzip/zip_source_compress.c \
	thirdparty/libzip/zip_source_crc.c \
	thirdparty/libzip/zip_source_error.c \
	thirdparty/libzip/zip_source_file_common.c \
	thirdparty/libzip/zip_source_file_stdio.c \
	thirdparty/libzip/zip_source_free.c \
	thirdparty/libzip/zip_source_function.c \
	thirdparty/libzip/zip_source_get_dostime.c \
	thirdparty/libzip/zip_source_get_file_attributes.c \
	thirdparty/libzip/zip_source_is_deleted.c \
	thirdparty/libzip/zip_source_layered.c \
	thirdparty/libzip/zip_source_open.c \
	thirdparty/libzip/zip_source_pass_to_lower_layer.c \
	thirdparty/libzip/zip_source_pkware_decode.c \
	thirdparty/libzip/zip_source_pkware_encode.c \
	thirdparty/libzip/zip_source_read.c \
	thirdparty/libzip/zip_source_remove.c \
	thirdparty/libzip/zip_source_rollback_write.c \
	thirdparty/libzip/zip_source_seek.c \
	thirdparty/libzip/zip_source_seek_write.c \
	thirdparty/libzip/zip_source_stat.c \
	thirdparty/libzip/zip_source_supports.c \
	thirdparty/libzip/zip_source_tell.c \
	thirdparty/libzip/zip_source_tell_write.c \
	thirdparty/libzip/zip_source_window.c \
	thirdparty/libzip/zip_source_write.c \
	thirdparty/libzip/zip_source_zip.c \
	thirdparty/libzip/zip_source_zip_new.c \
	thirdparty/libzip/zip_stat.c \
	thirdparty/libzip/zip_stat_index.c \
	thirdparty/libzip/zip_stat_init.c \
	thirdparty/libzip/zip_strerror.c \
	thirdparty/libzip/zip_string.c \
	thirdparty/libzip/zip_unchange.c \
	thirdparty/libzip/zip_unchange_all.c \
	thirdparty/libzip/zip_unchange_archive.c \
	thirdparty/libzip/zip_unchange_data.c \
	thirdparty/libzip/zip_utf-8.c \
	thirdparty/gen/zip_err_str.c \
	thirdparty/libzip/zip_source_file_stdio_named.c \
	thirdparty/libzip/zip_random_unix.c \
	thirdparty/libzip/zip_algorithm_bzip2.c \
	thirdparty/libzip/zip_algorithm_xz.c \
	thirdparty/libzip/zip_algorithm_zstd.c \
	thirdparty/libzip/zip_crypto_mbedtls.c \
	thirdparty/libzip/zip_winzip_aes.c \
	thirdparty/libzip/zip_source_winzip_aes_decode.c \
	thirdparty/libzip/zip_source_winzip_aes_encode.c

# bzip2 (7 files)
LOCAL_SRC_FILES += thirdparty/bz2/blocksort.c \
	thirdparty/bz2/huffman.c \
	thirdparty/bz2/crctable.c \
	thirdparty/bz2/randtable.c \
	thirdparty/bz2/compress.c \
	thirdparty/bz2/decompress.c \
	thirdparty/bz2/bzlib.c

# liblzma (xz) (79 files)
LOCAL_SRC_FILES += thirdparty/xz/src/common/tuklib_cpucores.c \
	thirdparty/xz/src/common/tuklib_physmem.c \
	thirdparty/xz/src/liblzma/check/check.c \
	thirdparty/xz/src/liblzma/check/crc32_fast.c \
	thirdparty/xz/src/liblzma/check/crc64_fast.c \
	thirdparty/xz/src/liblzma/check/sha256.c \
	thirdparty/xz/src/liblzma/common/alone_decoder.c \
	thirdparty/xz/src/liblzma/common/alone_encoder.c \
	thirdparty/xz/src/liblzma/common/auto_decoder.c \
	thirdparty/xz/src/liblzma/common/block_buffer_decoder.c \
	thirdparty/xz/src/liblzma/common/block_buffer_encoder.c \
	thirdparty/xz/src/liblzma/common/block_decoder.c \
	thirdparty/xz/src/liblzma/common/block_encoder.c \
	thirdparty/xz/src/liblzma/common/block_header_decoder.c \
	thirdparty/xz/src/liblzma/common/block_header_encoder.c \
	thirdparty/xz/src/liblzma/common/block_util.c \
	thirdparty/xz/src/liblzma/common/common.c \
	thirdparty/xz/src/liblzma/common/easy_buffer_encoder.c \
	thirdparty/xz/src/liblzma/common/easy_decoder_memusage.c \
	thirdparty/xz/src/liblzma/common/easy_encoder.c \
	thirdparty/xz/src/liblzma/common/easy_encoder_memusage.c \
	thirdparty/xz/src/liblzma/common/easy_preset.c \
	thirdparty/xz/src/liblzma/common/file_info.c \
	thirdparty/xz/src/liblzma/common/filter_buffer_decoder.c \
	thirdparty/xz/src/liblzma/common/filter_buffer_encoder.c \
	thirdparty/xz/src/liblzma/common/filter_common.c \
	thirdparty/xz/src/liblzma/common/filter_decoder.c \
	thirdparty/xz/src/liblzma/common/filter_encoder.c \
	thirdparty/xz/src/liblzma/common/filter_flags_decoder.c \
	thirdparty/xz/src/liblzma/common/filter_flags_encoder.c \
	thirdparty/xz/src/liblzma/common/hardware_cputhreads.c \
	thirdparty/xz/src/liblzma/common/hardware_physmem.c \
	thirdparty/xz/src/liblzma/common/index.c \
	thirdparty/xz/src/liblzma/common/index_decoder.c \
	thirdparty/xz/src/liblzma/common/index_encoder.c \
	thirdparty/xz/src/liblzma/common/index_hash.c \
	thirdparty/xz/src/liblzma/common/lzip_decoder.c \
	thirdparty/xz/src/liblzma/common/microlzma_decoder.c \
	thirdparty/xz/src/liblzma/common/microlzma_encoder.c \
	thirdparty/xz/src/liblzma/common/outqueue.c \
	thirdparty/xz/src/liblzma/common/stream_buffer_decoder.c \
	thirdparty/xz/src/liblzma/common/stream_buffer_encoder.c \
	thirdparty/xz/src/liblzma/common/stream_decoder.c \
	thirdparty/xz/src/liblzma/common/stream_decoder_mt.c \
	thirdparty/xz/src/liblzma/common/stream_encoder.c \
	thirdparty/xz/src/liblzma/common/stream_encoder_mt.c \
	thirdparty/xz/src/liblzma/common/stream_flags_common.c \
	thirdparty/xz/src/liblzma/common/stream_flags_decoder.c \
	thirdparty/xz/src/liblzma/common/stream_flags_encoder.c \
	thirdparty/xz/src/liblzma/common/string_conversion.c \
	thirdparty/xz/src/liblzma/common/vli_decoder.c \
	thirdparty/xz/src/liblzma/common/vli_encoder.c \
	thirdparty/xz/src/liblzma/common/vli_size.c \
	thirdparty/xz/src/liblzma/delta/delta_common.c \
	thirdparty/xz/src/liblzma/delta/delta_decoder.c \
	thirdparty/xz/src/liblzma/delta/delta_encoder.c \
	thirdparty/xz/src/liblzma/lz/lz_decoder.c \
	thirdparty/xz/src/liblzma/lz/lz_encoder.c \
	thirdparty/xz/src/liblzma/lz/lz_encoder_mf.c \
	thirdparty/xz/src/liblzma/lzma/fastpos_table.c \
	thirdparty/xz/src/liblzma/lzma/lzma2_decoder.c \
	thirdparty/xz/src/liblzma/lzma/lzma2_encoder.c \
	thirdparty/xz/src/liblzma/lzma/lzma_decoder.c \
	thirdparty/xz/src/liblzma/lzma/lzma_encoder.c \
	thirdparty/xz/src/liblzma/lzma/lzma_encoder_optimum_fast.c \
	thirdparty/xz/src/liblzma/lzma/lzma_encoder_optimum_normal.c \
	thirdparty/xz/src/liblzma/lzma/lzma_encoder_presets.c \
	thirdparty/xz/src/liblzma/rangecoder/price_table.c \
	thirdparty/xz/src/liblzma/simple/arm.c \
	thirdparty/xz/src/liblzma/simple/arm64.c \
	thirdparty/xz/src/liblzma/simple/armthumb.c \
	thirdparty/xz/src/liblzma/simple/ia64.c \
	thirdparty/xz/src/liblzma/simple/powerpc.c \
	thirdparty/xz/src/liblzma/simple/riscv.c \
	thirdparty/xz/src/liblzma/simple/simple_coder.c \
	thirdparty/xz/src/liblzma/simple/simple_decoder.c \
	thirdparty/xz/src/liblzma/simple/simple_encoder.c \
	thirdparty/xz/src/liblzma/simple/sparc.c \
	thirdparty/xz/src/liblzma/simple/x86.c

# zstd (26 files; dictBuilder excluded — training only, unreferenced)
LOCAL_SRC_FILES += thirdparty/zstd/common/debug.c \
	thirdparty/zstd/common/entropy_common.c \
	thirdparty/zstd/common/error_private.c \
	thirdparty/zstd/common/fse_decompress.c \
	thirdparty/zstd/common/pool.c \
	thirdparty/zstd/common/threading.c \
	thirdparty/zstd/common/xxhash.c \
	thirdparty/zstd/common/zstd_common.c \
	thirdparty/zstd/compress/fse_compress.c \
	thirdparty/zstd/compress/hist.c \
	thirdparty/zstd/compress/huf_compress.c \
	thirdparty/zstd/compress/zstd_compress.c \
	thirdparty/zstd/compress/zstd_compress_literals.c \
	thirdparty/zstd/compress/zstd_compress_sequences.c \
	thirdparty/zstd/compress/zstd_compress_superblock.c \
	thirdparty/zstd/compress/zstd_double_fast.c \
	thirdparty/zstd/compress/zstd_fast.c \
	thirdparty/zstd/compress/zstd_lazy.c \
	thirdparty/zstd/compress/zstd_ldm.c \
	thirdparty/zstd/compress/zstd_opt.c \
	thirdparty/zstd/compress/zstd_preSplit.c \
	thirdparty/zstd/compress/zstdmt_compress.c \
	thirdparty/zstd/decompress/huf_decompress.c \
	thirdparty/zstd/decompress/zstd_ddict.c \
	thirdparty/zstd/decompress/zstd_decompress.c \
	thirdparty/zstd/decompress/zstd_decompress_block.c

# lz4 (3 files; lz4frame.c #includes "xxhash.h", which is NOT vendored
# under thirdparty/lz4 and resolves to thirdparty/zstd/common/xxhash.h.
# Sound: zstd's header sets XXH_NAMESPACE=ZSTD_, so lz4frame calls the
# ZSTD_-prefixed symbols implemented by zstd's xxhash.c — no collisions,
# no missing symbols. Do not "fix" by deleting either xxhash.)
LOCAL_SRC_FILES += thirdparty/lz4/lz4.c \
	thirdparty/lz4/lz4frame.c \
	thirdparty/lz4/lz4hc.c

# sevenzip C decoder (21 files; decode-only: 7z read path in AaeSevenZ.cpp)
# plus single-threaded LZMA encoder (2 files from the same bundle: 7z write
# path). _7ZIP_ST strips LzmaEnc's multi-thread code so LzFindMt/Threads are
# neither compiled nor needed; nothing else in the tree reads that macro.
LOCAL_SRC_FILES += thirdparty/sevenzip/7zArcIn.c \
	thirdparty/sevenzip/7zDec.c \
	thirdparty/sevenzip/7zBuf.c \
	thirdparty/sevenzip/7zCrc.c \
	thirdparty/sevenzip/7zCrcOpt.c \
	thirdparty/sevenzip/7zStream.c \
	thirdparty/sevenzip/7zAlloc.c \
	thirdparty/sevenzip/7zFile.c \
	thirdparty/sevenzip/Alloc.c \
	thirdparty/sevenzip/CpuArch.c \
	thirdparty/sevenzip/LzmaDec.c \
	thirdparty/sevenzip/Lzma2Dec.c \
	thirdparty/sevenzip/Bra.c \
	thirdparty/sevenzip/Bra86.c \
	thirdparty/sevenzip/BraIA64.c \
	thirdparty/sevenzip/Bcj2.c \
	thirdparty/sevenzip/Delta.c \
	thirdparty/sevenzip/Ppmd7.c \
	thirdparty/sevenzip/Ppmd7Dec.c \
	thirdparty/sevenzip/Ppmd8.c \
	thirdparty/sevenzip/Ppmd8Dec.c \
	thirdparty/sevenzip/LzmaEnc.c \
	thirdparty/sevenzip/LzFind.c

# libarchive read backend (RAR/RAR5/CAB/ISO). OFF by default: set
# AAE_HAVE_LIBARCHIVE=1 and provide thirdparty/libarchive at v3.7.4 plus a
# hand-written config dir thirdparty/gen-libarchive/config.h (same pattern
# as thirdparty/gen/config.h; upstream generates it from
# build/cmake/config.h.in via LibarchiveChecks.cmake). File list mirrors
# libarchive-android's CMake selection adapted for v3.7.4 renames
# (archive_getdate.c instead of archive_parse_date.c; no archive_time.c).
# Windows/darwin/freebsd/sunos-only files excluded, same as upstream.
ifeq ($(AAE_HAVE_LIBARCHIVE),1)
LOCAL_CFLAGS += -DAAE_HAVE_LIBARCHIVE=1
LOCAL_C_INCLUDES += $(LOCAL_PATH)/thirdparty/libarchive/libarchive \
	$(LOCAL_PATH)/thirdparty/gen-libarchive
LOCAL_SRC_FILES +=  \
	thirdparty/libarchive/libarchive/archive_acl.c \
	thirdparty/libarchive/libarchive/archive_blake2s_ref.c \
	thirdparty/libarchive/libarchive/archive_blake2sp_ref.c \
	thirdparty/libarchive/libarchive/archive_check_magic.c \
	thirdparty/libarchive/libarchive/archive_cmdline.c \
	thirdparty/libarchive/libarchive/archive_cryptor.c \
	thirdparty/libarchive/libarchive/archive_digest.c \
	thirdparty/libarchive/libarchive/archive_entry.c \
	thirdparty/libarchive/libarchive/archive_entry_copy_stat.c \
	thirdparty/libarchive/libarchive/archive_entry_link_resolver.c \
	thirdparty/libarchive/libarchive/archive_entry_sparse.c \
	thirdparty/libarchive/libarchive/archive_entry_stat.c \
	thirdparty/libarchive/libarchive/archive_entry_strmode.c \
	thirdparty/libarchive/libarchive/archive_entry_xattr.c \
	thirdparty/libarchive/libarchive/archive_getdate.c \
	thirdparty/libarchive/libarchive/archive_hmac.c \
	thirdparty/libarchive/libarchive/archive_match.c \
	thirdparty/libarchive/libarchive/archive_options.c \
	thirdparty/libarchive/libarchive/archive_pack_dev.c \
	thirdparty/libarchive/libarchive/archive_pathmatch.c \
	thirdparty/libarchive/libarchive/archive_ppmd7.c \
	thirdparty/libarchive/libarchive/archive_ppmd8.c \
	thirdparty/libarchive/libarchive/archive_random.c \
	thirdparty/libarchive/libarchive/archive_rb.c \
	thirdparty/libarchive/libarchive/archive_read.c \
	thirdparty/libarchive/libarchive/archive_read_add_passphrase.c \
	thirdparty/libarchive/libarchive/archive_read_append_filter.c \
	thirdparty/libarchive/libarchive/archive_read_data_into_fd.c \
	thirdparty/libarchive/libarchive/archive_read_disk_entry_from_file.c \
	thirdparty/libarchive/libarchive/archive_read_disk_posix.c \
	thirdparty/libarchive/libarchive/archive_read_disk_set_standard_lookup.c \
	thirdparty/libarchive/libarchive/archive_read_extract.c \
	thirdparty/libarchive/libarchive/archive_read_extract2.c \
	thirdparty/libarchive/libarchive/archive_read_open_fd.c \
	thirdparty/libarchive/libarchive/archive_read_open_file.c \
	thirdparty/libarchive/libarchive/archive_read_open_filename.c \
	thirdparty/libarchive/libarchive/archive_read_open_memory.c \
	thirdparty/libarchive/libarchive/archive_read_set_format.c \
	thirdparty/libarchive/libarchive/archive_read_set_options.c \
	thirdparty/libarchive/libarchive/archive_read_support_filter_all.c \
	thirdparty/libarchive/libarchive/archive_read_support_filter_by_code.c \
	thirdparty/libarchive/libarchive/archive_read_support_filter_bzip2.c \
	thirdparty/libarchive/libarchive/archive_read_support_filter_compress.c \
	thirdparty/libarchive/libarchive/archive_read_support_filter_grzip.c \
	thirdparty/libarchive/libarchive/archive_read_support_filter_gzip.c \
	thirdparty/libarchive/libarchive/archive_read_support_filter_lrzip.c \
	thirdparty/libarchive/libarchive/archive_read_support_filter_lz4.c \
	thirdparty/libarchive/libarchive/archive_read_support_filter_lzop.c \
	thirdparty/libarchive/libarchive/archive_read_support_filter_none.c \
	thirdparty/libarchive/libarchive/archive_read_support_filter_program.c \
	thirdparty/libarchive/libarchive/archive_read_support_filter_rpm.c \
	thirdparty/libarchive/libarchive/archive_read_support_filter_uu.c \
	thirdparty/libarchive/libarchive/archive_read_support_filter_xz.c \
	thirdparty/libarchive/libarchive/archive_read_support_filter_zstd.c \
	thirdparty/libarchive/libarchive/archive_read_support_format_7zip.c \
	thirdparty/libarchive/libarchive/archive_read_support_format_all.c \
	thirdparty/libarchive/libarchive/archive_read_support_format_ar.c \
	thirdparty/libarchive/libarchive/archive_read_support_format_by_code.c \
	thirdparty/libarchive/libarchive/archive_read_support_format_cab.c \
	thirdparty/libarchive/libarchive/archive_read_support_format_cpio.c \
	thirdparty/libarchive/libarchive/archive_read_support_format_empty.c \
	thirdparty/libarchive/libarchive/archive_read_support_format_iso9660.c \
	thirdparty/libarchive/libarchive/archive_read_support_format_lha.c \
	thirdparty/libarchive/libarchive/archive_read_support_format_mtree.c \
	thirdparty/libarchive/libarchive/archive_read_support_format_rar.c \
	thirdparty/libarchive/libarchive/archive_read_support_format_rar5.c \
	thirdparty/libarchive/libarchive/archive_read_support_format_raw.c \
	thirdparty/libarchive/libarchive/archive_read_support_format_tar.c \
	thirdparty/libarchive/libarchive/archive_read_support_format_warc.c \
	thirdparty/libarchive/libarchive/archive_read_support_format_xar.c \
	thirdparty/libarchive/libarchive/archive_read_support_format_zip.c \
	thirdparty/libarchive/libarchive/archive_string.c \
	thirdparty/libarchive/libarchive/archive_string_sprintf.c \
	thirdparty/libarchive/libarchive/archive_util.c \
	thirdparty/libarchive/libarchive/archive_version_details.c \
	thirdparty/libarchive/libarchive/archive_virtual.c \
	thirdparty/libarchive/libarchive/archive_write.c \
	thirdparty/libarchive/libarchive/archive_write_add_filter.c \
	thirdparty/libarchive/libarchive/archive_write_add_filter_b64encode.c \
	thirdparty/libarchive/libarchive/archive_write_add_filter_by_name.c \
	thirdparty/libarchive/libarchive/archive_write_add_filter_bzip2.c \
	thirdparty/libarchive/libarchive/archive_write_add_filter_compress.c \
	thirdparty/libarchive/libarchive/archive_write_add_filter_grzip.c \
	thirdparty/libarchive/libarchive/archive_write_add_filter_gzip.c \
	thirdparty/libarchive/libarchive/archive_write_add_filter_lrzip.c \
	thirdparty/libarchive/libarchive/archive_write_add_filter_lz4.c \
	thirdparty/libarchive/libarchive/archive_write_add_filter_lzop.c \
	thirdparty/libarchive/libarchive/archive_write_add_filter_none.c \
	thirdparty/libarchive/libarchive/archive_write_add_filter_program.c \
	thirdparty/libarchive/libarchive/archive_write_add_filter_uuencode.c \
	thirdparty/libarchive/libarchive/archive_write_add_filter_xz.c \
	thirdparty/libarchive/libarchive/archive_write_add_filter_zstd.c \
	thirdparty/libarchive/libarchive/archive_write_disk_posix.c \
	thirdparty/libarchive/libarchive/archive_write_disk_set_standard_lookup.c \
	thirdparty/libarchive/libarchive/archive_write_open_fd.c \
	thirdparty/libarchive/libarchive/archive_write_open_file.c \
	thirdparty/libarchive/libarchive/archive_write_open_filename.c \
	thirdparty/libarchive/libarchive/archive_write_open_memory.c \
	thirdparty/libarchive/libarchive/archive_write_set_format.c \
	thirdparty/libarchive/libarchive/archive_write_set_format_7zip.c \
	thirdparty/libarchive/libarchive/archive_write_set_format_ar.c \
	thirdparty/libarchive/libarchive/archive_write_set_format_by_name.c \
	thirdparty/libarchive/libarchive/archive_write_set_format_cpio.c \
	thirdparty/libarchive/libarchive/archive_write_set_format_cpio_binary.c \
	thirdparty/libarchive/libarchive/archive_write_set_format_cpio_newc.c \
	thirdparty/libarchive/libarchive/archive_write_set_format_cpio_odc.c \
	thirdparty/libarchive/libarchive/archive_write_set_format_filter_by_ext.c \
	thirdparty/libarchive/libarchive/archive_write_set_format_gnutar.c \
	thirdparty/libarchive/libarchive/archive_write_set_format_iso9660.c \
	thirdparty/libarchive/libarchive/archive_write_set_format_mtree.c \
	thirdparty/libarchive/libarchive/archive_write_set_format_pax.c \
	thirdparty/libarchive/libarchive/archive_write_set_format_raw.c \
	thirdparty/libarchive/libarchive/archive_write_set_format_shar.c \
	thirdparty/libarchive/libarchive/archive_write_set_format_ustar.c \
	thirdparty/libarchive/libarchive/archive_write_set_format_v7tar.c \
	thirdparty/libarchive/libarchive/archive_write_set_format_warc.c \
	thirdparty/libarchive/libarchive/archive_write_set_format_xar.c \
	thirdparty/libarchive/libarchive/archive_write_set_format_zip.c \
	thirdparty/libarchive/libarchive/archive_write_set_options.c \
	thirdparty/libarchive/libarchive/archive_write_set_passphrase.c \
	thirdparty/libarchive/libarchive/filter_fork_posix.c \
	thirdparty/libarchive/libarchive/xxhash.c
endif

LOCAL_STATIC_LIBRARIES := mbedcrypto

include $(BUILD_SHARED_LIBRARY)
