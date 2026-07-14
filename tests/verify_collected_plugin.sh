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
        exports="$(readelf --dyn-syms -W "$FILE_PATH" | \
            awk '$5 == "GLOBAL" && $7 != "UND" {sub(/@.*/, "", $8); print $8}' | \
            sort -u)"
        [ "$exports" = "$(printf '%s\n' pginit stata_call | sort)" ] || \
            die "export table must contain exactly pginit and stata_call (got: $(printf '%s' "$exports" | tr '\n' ' '))"
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
        # Apple nm's -g output includes undefined imports as well as symbols
        # defined by the plugin.  -U suppresses those imports, so this checks
        # the actual exported ABI surface rather than rejecting every normal
        # dependency on libc/libc++.
        symbols="$(nm -gjU "$FILE_PATH")"
        exports="$(printf '%s\n' "$symbols" | sort -u)"
        [ "$exports" = "$(printf '%s\n' _pginit _stata_call | sort)" ] || \
            die "export table must contain exactly _pginit and _stata_call (got: $(printf '%s' "$exports" | tr '\n' ' '))"
        ;;
    windows)
        command -v objdump >/dev/null 2>&1 || die "objdump is required"
        objdump -f "$FILE_PATH" | grep -Eqi 'pei-x86-64|pe-x86-64' || \
            die "artifact is not a 64-bit PE/COFF plugin"
        exports="$(LC_ALL=C objdump -p "$FILE_PATH" | \
            awk '/\[Ordinal\/Name Pointer\] Table/{inside=1; next} \
                 inside && $1 ~ /^\[/ {print $NF}' | sort -u)"
        [ "$exports" = "$(printf '%s\n' pginit stata_call | sort)" ] || \
            die "PE export table must contain exactly pginit and stata_call"
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
