#!/usr/bin/env bash
# Verify the exact plugin file that packaging/upload will consume.
# Usage: tests/verify_collected_plugin.sh FILE linux|macos|windows
set -eu

FILE_PATH="${1:-}"
PLATFORM="${2:-}"

die() { printf 'collected-plugin FAIL: %s\n' "$*" >&2; exit 1; }
[ -n "$FILE_PATH" ] && [ -f "$FILE_PATH" ] && [ -s "$FILE_PATH" ] || \
    die "missing or empty artifact: $FILE_PATH"

case "$PLATFORM" in
    linux)
        command -v readelf >/dev/null 2>&1 || die "readelf is required"
        file "$FILE_PATH" | grep -q 'ELF 64-bit' || die "artifact is not a 64-bit ELF plugin"
        sections="$(readelf -S -W "$FILE_PATH")"
        printf '%s\n' "$sections" | grep -Eq '[[:space:]]\.symtab[[:space:]]' && \
            die "ordinary .symtab is present (artifact was not stripped)"
        printf '%s\n' "$sections" | grep -Eq '[[:space:]]\.debug(_|[[:space:]])' && \
            die "debug sections are present (artifact was not stripped)"
        symbols="$(readelf -Ws -W "$FILE_PATH")"
        printf '%s\n' "$symbols" | grep -Eq '[[:space:]]stata_call$' || \
            die "required exported symbol stata_call is missing"
        printf '%s\n' "$symbols" | grep -Eq '[[:space:]]pginit$' || \
            die "required exported symbol pginit is missing"
        if readelf -d -W "$FILE_PATH" | grep -Eq 'NEEDED.*(libstdc\+\+|libgcc_s)'; then
            die "Linux artifact dynamically links libstdc++ or libgcc_s"
        fi
        ;;
    macos)
        command -v nm >/dev/null 2>&1 || die "nm is required"
        file "$FILE_PATH" | grep -q 'Mach-O 64-bit' || die "artifact is not a 64-bit Mach-O plugin"
        # CMake uses `strip -x`: local symbols disappear, while the two globals
        # Stata needs remain.  Mach-O keeps an export symbol table by design, so
        # the Linux .symtab rule is intentionally not asserted here.
        symbols="$(nm -gj "$FILE_PATH")"
        printf '%s\n' "$symbols" | grep -qx '_stata_call' || \
            die "required exported symbol _stata_call is missing"
        printf '%s\n' "$symbols" | grep -qx '_pginit' || \
            die "required exported symbol _pginit is missing"
        ;;
    windows)
        command -v objdump >/dev/null 2>&1 || die "objdump is required"
        objdump -f "$FILE_PATH" | grep -Eqi 'pei-x86-64|pe-x86-64' || \
            die "artifact is not a 64-bit PE/COFF plugin"
        exports="$(objdump -p "$FILE_PATH")"
        printf '%s\n' "$exports" | grep -Eq '[[:space:]]stata_call$' || \
            die "required exported symbol stata_call is missing"
        printf '%s\n' "$exports" | grep -Eq '[[:space:]]pginit$' || \
            die "required exported symbol pginit is missing"
        # MSVC Release output is the distributable binary; there is no Unix
        # strip step or ELF section contract to apply on this platform.
        ;;
    *)
        die "platform must be linux, macos or windows"
        ;;
esac

if stat -c %s "$FILE_PATH" >/dev/null 2>&1; then
    bytes="$(stat -c %s "$FILE_PATH")"
else
    bytes="$(stat -f %z "$FILE_PATH")"
fi
printf 'collected-plugin OK: %s (%s bytes; %s)\n' "$FILE_PATH" "$bytes" "$PLATFORM"
