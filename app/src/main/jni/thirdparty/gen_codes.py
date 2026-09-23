#!/usr/bin/env python3
"""Replicates libzip's cmake/GenerateZipErrorStrings.cmake without CMake.

Reads lib/zip.h (ZIP_ER_*) and lib/zipint.h (ZIP_ER_DETAIL_*) from the
vendored libzip tree and writes third-party/gen/zip_err_str.c, byte-
identical in structure to what the CMake build generates.

Usage: gen_zip_err_str.py <libzip-lib-dir> <out-file>
"""
import re
import sys

HEADER = """/*
  This file was generated automatically by gen_zip_err_str.py
  from zip.h and zipint.h; make changes there.
*/

#include "zipint.h"

#define L ZIP_ET_LIBZIP
#define N ZIP_ET_NONE
#define S ZIP_ET_SYS
#define Z ZIP_ET_ZLIB

#define E ZIP_DETAIL_ET_ENTRY
#define G ZIP_DETAIL_ET_GLOBAL

const struct _zip_err_info _zip_err_str[] = {
"""

MID = """};

const int _zip_err_str_count = sizeof(_zip_err_str)/sizeof(_zip_err_str[0]);

const struct _zip_err_info _zip_err_details[] = {
"""

FOOTER = """};

const int _zip_err_details_count = sizeof(_zip_err_details)/sizeof(_zip_err_details[0]);
"""

# Mirrors the CMake patterns character-for-character (note: CMake's
# [L|N|S|Z] classes also match a literal '|', kept here for fidelity).
LINE_RE = re.compile(
    r"#define ZIP_ER_([A-Z0-9_]+) ([0-9]+)[ \t]+/([-*0-9a-zA-Z, ']*)/"
)
DETAIL_LINE_RE = re.compile(
    r"#define ZIP_ER_DETAIL_([A-Z0-9_]+) ([0-9]+)[ \t]+/([-*0-9a-zA-Z, ']*)/"
)
INNER_RE = re.compile(r"([LNSZ|]+) ([-0-9a-zA-Z, ']*)")
DETAIL_INNER_RE = re.compile(r"([EG|]+) ([-0-9a-zA-Z, ']*)")


def emit(text, line_re, inner_re):
    out = []
    for m in line_re.finditer(text):
        comment = m.group(3)
        im = inner_re.search(comment)
        if not im:
            raise SystemExit("cannot parse error comment: %r" % comment)
        out.append("    { %s, \"%s\" },\n" % (im.group(1), im.group(2).strip()))
    return out


def main():
    lib_dir, out_file = sys.argv[1], sys.argv[2]
    with open(lib_dir + "/zip.h", encoding="utf-8") as f:
        zip_h = f.read()
    with open(lib_dir + "/zipint.h", encoding="utf-8") as f:
        zipint_h = f.read()
    parts = [HEADER]
    parts += emit(zip_h, LINE_RE, INNER_RE)
    parts.append(MID)
    parts += emit(zipint_h, DETAIL_LINE_RE, DETAIL_INNER_RE)
    parts.append(FOOTER)
    with open(out_file, "w", encoding="utf-8", newline="\n") as f:
        f.writelines(parts)
    print("wrote %s (%d+%d entries)" % (
        out_file, len(emit(zip_h, LINE_RE, INNER_RE)),
        len(emit(zipint_h, DETAIL_LINE_RE, DETAIL_INNER_RE))))


if __name__ == "__main__":
    main()
