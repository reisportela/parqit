#!/usr/bin/env bash
# release_lint.sh — guards against the C03 hazard: user-visible version/date
# surfaces drifting out of sync (CMake says one version, the help banner
# another, the package date a third). It reads only text files, so CI can run
# it on every platform without a build or a Stata license.
#
#   bash tests/release_lint.sh
#
# Checks, all of which must hold before tagging a release:
#   * project version == ado/help/dialog banners == README/CLAUDE == CITATION.cff
#   * release dates agree across banners, package manifest and CITATION.cff
#   * CHANGELOG has exactly one "## [Unreleased]" heading
#   * the newest dated CHANGELOG section matches the project version
#   * no CHANGELOG section repeats the same "### <Type>" heading
set -u

REPO="$(cd "$(dirname "$0")/.." && pwd)"
fail=0
err() { printf 'release-lint FAIL: %s\n' "$*" >&2; fail=1; }

semver='[0-9]+\.[0-9]+\.[0-9]+'
banner_date='[0-9]{1,2}[a-z]{3}[0-9]{4}'   # e.g. 14jun2026

# --- gather every version/date surface ---------------------------------------
cmake_v=$(grep -oE "project\(parqit VERSION $semver" "$REPO/CMakeLists.txt" \
            | grep -oE "$semver" | head -1)

ado_line=$(sed -n '1p' "$REPO/src/ado/p/parqit.ado")
ado_v=$(printf '%s' "$ado_line" | grep -oE "version $semver" | grep -oE "$semver")
ado_d=$(printf '%s' "$ado_line" | grep -oE "$banner_date")

sthlp_line=$(grep -m1 -E "version $semver" "$REPO/src/ado/p/parqit.sthlp")
sthlp_v=$(printf '%s' "$sthlp_line" | grep -oE "version $semver" | grep -oE "$semver")
sthlp_d=$(printf '%s' "$sthlp_line" | grep -oE "$banner_date")

readme_v=$(grep -oE "Status:\*\* v$semver" "$REPO/README.md" | grep -oE "$semver" | head -1)
claude_v=$(grep -oE "Current state: \*\*v$semver" "$REPO/CLAUDE.md" \
              | grep -oE "$semver" | head -1)

pkg_d=$(grep -oE 'Distribution-Date: [0-9]{8}' "$REPO/src/ado/p/parqit.pkg" \
          | grep -oE '[0-9]{8}' | head -1)

cff_v=$(grep -m1 -oE '^version: "[0-9]+\.[0-9]+\.[0-9]+"' "$REPO/CITATION.cff" \
          | grep -oE "$semver")
cff_d=$(grep -m1 -oE '^date-released: [0-9]{4}-[0-9]{2}-[0-9]{2}' "$REPO/CITATION.cff" \
          | grep -oE '[0-9]{4}-[0-9]{2}-[0-9]{2}')

# DDmonYYYY -> YYYYMMDD so the banner date can be compared to the pkg date.
banner_to_iso() {
    local d="$1" dd mon yyyy num
    dd=$(printf '%s' "$d" | sed -E "s/^([0-9]{1,2})[a-z]{3}[0-9]{4}$/\1/")
    mon=$(printf '%s' "$d" | sed -E "s/^[0-9]{1,2}([a-z]{3})[0-9]{4}$/\1/")
    yyyy=$(printf '%s' "$d" | sed -E "s/^[0-9]{1,2}[a-z]{3}([0-9]{4})$/\1/")
    case "$mon" in
        jan) num=01;; feb) num=02;; mar) num=03;; apr) num=04;;
        may) num=05;; jun) num=06;; jul) num=07;; aug) num=08;;
        sep) num=09;; oct) num=10;; nov) num=11;; dec) num=12;;
        *) num=00;;
    esac
    printf '%s%s%02d' "$yyyy" "$num" "$dd"
}

# --- versions agree ----------------------------------------------------------
[ -n "$cmake_v" ]  || err "could not read project(parqit VERSION) from CMakeLists.txt"
[ -n "$ado_v" ]    || err "could not read version from parqit.ado banner"
[ -n "$sthlp_v" ]  || err "could not read version from parqit.sthlp banner"
[ -n "$readme_v" ] || err "could not read **Status:** vX.Y.Z from README.md"
[ -n "$claude_v" ] || err "could not read Current state: **vX.Y.Z from CLAUDE.md"

for pair in "ado=$ado_v" "sthlp=$sthlp_v" "readme=$readme_v" \
            "claude=$claude_v" "citation=$cff_v"; do
    name=${pair%%=*}; val=${pair#*=}
    [ "$val" = "$cmake_v" ] || err "$name version $val != project version $cmake_v"
done

# --- dates agree -------------------------------------------------------------
[ -n "$ado_d" ]   || err "could not read date from parqit.ado banner"
[ -n "$sthlp_d" ] || err "could not read date from parqit.sthlp banner"
[ -n "$pkg_d" ]   || err "could not read Distribution-Date from parqit.pkg"
[ -n "$cff_d" ]   || err "could not read date-released from CITATION.cff"
[ "$ado_d" = "$sthlp_d" ] || err "ado banner date $ado_d != sthlp banner date $sthlp_d"
if [ -n "$ado_d" ]; then
    iso=$(banner_to_iso "$ado_d")
    [ "$iso" = "$pkg_d" ] || err "banner date $ado_d ($iso) != parqit.pkg Distribution-Date $pkg_d"
    cff_iso=$(printf '%s' "$cff_d" | tr -d '-')
    [ "$iso" = "$cff_iso" ] || err "banner date $ado_d ($iso) != CITATION.cff date-released $cff_d"
fi

# --- dialogs carry the same version/date as the ado banner -------------------
# (the .dlg banners are a fifth synchronised surface; they drifted to a stale
# 0.1.15 while the package shipped 0.1.16, and nothing caught it — now gated.)
for dlg in "$REPO"/src/ado/p/parqit_*.dlg; do
    [ -e "$dlg" ] || continue
    base=$(basename "$dlg")
    dl=$(grep -m1 -E '^\*!  *VERSION ' "$dlg")
    dv=$(printf '%s' "$dl" | grep -oE "$semver")
    dd=$(printf '%s' "$dl" | grep -oE "$banner_date")
    [ -n "$dv" ] || err "$base has no '*! VERSION X.Y.Z DDmonYYYY' banner"
    [ -n "$dv" ] && [ "$dv" != "$cmake_v" ] && err "$base version $dv != project version $cmake_v"
    [ -n "$dd" ] && [ -n "$ado_d" ] && [ "$dd" != "$ado_d" ] && \
        err "$base date $dd != ado banner date $ado_d"
done

# --- parqit.pkg manifest is coherent (a net install reads it line by line) ----
# every 'f <file>' the package ships must exist in the source ado dir (a missing
# one aborts net install on the target — the historical .dlg-not-shipped bug).
while read -r _ fn _; do
    [ -n "$fn" ] || continue
    [ -f "$REPO/src/ado/p/$fn" ] || \
        err "parqit.pkg ships '$fn' but src/ado/p/$fn does not exist"
done < <(grep -E '^f ' "$REPO/src/ado/p/parqit.pkg")

# every 'g <PLAT> <binary> ...' must name a per-OS binary the release workflow
# actually builds — the manifest promised MACINTEL64 that CI never produced.
built=$(grep -oE 'parqit_[A-Za-z0-9]+\.plugin' "$REPO/.github/workflows/build.yml" | sort -u)
while read -r _ _ gbin _; do
    [ -n "$gbin" ] || continue
    printf '%s\n' "$built" | grep -qx "$gbin" || \
        err "parqit.pkg declares platform binary '$gbin' the release workflow never builds"
done < <(grep -E '^g ' "$REPO/src/ado/p/parqit.pkg")

# --- README net-install example pins the current version ---------------------
rd_v=$(grep -oE 'releases/download/v'"$semver" "$REPO/README.md" | grep -oE "$semver" | head -1)
[ -n "$rd_v" ] || err "README.md has no 'net install ... releases/download/vX.Y.Z' example"
[ -n "$rd_v" ] && [ "$rd_v" != "$cmake_v" ] && \
    err "README net install example v$rd_v != project version $cmake_v"

# --- CHANGELOG sectioning ----------------------------------------------------
unrel=$(grep -cE '^## \[Unreleased\]' "$REPO/CHANGELOG.md")
[ "$unrel" = "1" ] || err "CHANGELOG.md must have exactly one '## [Unreleased]' (found $unrel)"

chg_top=$(grep -oE "^## \[$semver\]" "$REPO/CHANGELOG.md" | head -1 | grep -oE "$semver")
[ -n "$chg_top" ] || err "CHANGELOG.md has no dated release section"
[ -n "$chg_top" ] && [ "$chg_top" != "$cmake_v" ] && \
    err "newest CHANGELOG release [$chg_top] != project version $cmake_v"

# No duplicate "### <Type>" heading inside any one section (a
# Keep-a-Changelog malformation the section-level checks above cannot see).
dups=$(awk '/^## \[/{sec=$0; next} /^### /{print sec "\t" $0}' \
        "$REPO/CHANGELOG.md" | sort | uniq -d)
[ -z "$dups" ] || err "CHANGELOG has duplicate heading(s) inside one section: $(echo "$dups")"

# --- no private / home-absolute paths in shipping/source/test/benchmark files -
# (REL-2b: a leaked username or third-party data path must never reach VCS;
# historical .md audit reports are exempt — they cite paths as evidence.)
leak=$(git -C "$REPO" grep -lE '/home/[^/ ]+/|/Users/[^/ ]+/' -- \
        '*.do' '*.ado' '*.sthlp' '*.pkg' '*.cpp' '*.hpp' '*.h' '*.c' \
        '*.sh' '*.yml' '*.yaml' '*.cmake' 'CMakeLists.txt' \
        ':!tests/release_lint.sh' 2>/dev/null || true)
[ -z "$leak" ] || err "private/home-absolute path in tracked source file(s): $(echo $leak)"

# --- report ------------------------------------------------------------------
if [ "$fail" -eq 0 ]; then
    printf 'release-lint OK: v%s (%s / pkg %s); CHANGELOG top [%s]\n' \
        "$cmake_v" "$ado_d" "$pkg_d" "$chg_top"
fi
exit "$fail"
