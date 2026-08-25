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
#   * every public dispatcher command appears in the help syntax surface
#   * every public command is reachable through a wide dialog on the User menu
#   * the help's expression-function list and exprtrans.cpp agree both ways
#   * every parqit## reference in the help (jump or inline link) has a marker
#   * GUI and console selectors agree for both macOS architectures
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

# Stata assigns different package platform names to GUI and console sessions
# on both Mac architectures. Each pair executes the same binary; omitting a
# console alias makes the final `h parqit.plugin` line abort net install from
# stata-mp/stata-se launched in a terminal.
check_mac_pair() {
    local gui_code="$1" console_code="$2" gui console
    gui=$(awk -v code="$gui_code" \
              '$1 == "g" && $2 == code {print $3 " " $4}' \
              "$REPO/src/ado/p/parqit.pkg")
    console=$(awk -v code="$console_code" \
                  '$1 == "g" && $2 == code {print $3 " " $4}' \
                  "$REPO/src/ado/p/parqit.pkg")
    [ -n "$gui" ] || err "parqit.pkg has no $gui_code GUI plugin selector"
    [ -n "$console" ] || err "parqit.pkg has no $console_code console plugin selector"
    [ -n "$gui" ] && [ -n "$console" ] && [ "$gui" != "$console" ] && \
        err "$gui_code and $console_code must install the same source and destination"
}
check_mac_pair MACARM64 OSX.ARM64
check_mac_pair MACINTEL64 OSX.X8664

# --- README default net-install route follows the newest public release ------
# The installation command must not need a version edit after every release;
# users who deliberately pin an older tag can substitute download/vX.Y.Z.
latest_install='net install parqit, from("https://github.com/reisportela/parqit/releases/latest/download") replace'
grep -Fq "$latest_install" "$REPO/README.md" || \
    err "README default net install must use releases/latest/download"

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

# --- lazy-contract wording --------------------------------------------------
# Opening a view reads schema/metadata (and may sample/bridge a source), while
# no RESULT rows enter Stata. These old absolutes repeatedly drifted back into
# the README, ado mirror and dialog and contradict the executable contract.
lazy_overclaim=$(grep -niE \
    'nothing (is )?(read|materialised)|does not read the file|nothing executes until|nothing below materialises data|no observations (are )?loaded into Stata|before any observations enter Stata memory|Stata.?s? memory (is )?(never )?(un)?touched|without (ever )?touching (Stata.?s )?memory|never enters (Stata.?s )?memory|first contact with a 100-GB extract is not a 100-GB read|only the columns and rows your verbs need are' \
    "$REPO/README.md" \
    "$REPO/src/ado/p/parqit.ado" \
    "$REPO/src/ado/p/parqit.sthlp" \
    "$REPO"/src/ado/p/parqit_*.dlg \
    "$REPO/benchmarks/profile_parqit.ado" 2>/dev/null || true)
[ -z "$lazy_overclaim" ] || \
    err "stale absolute lazy-I/O claim(s); say schema probed/no result rows loaded: $(echo "$lazy_overclaim")"

# --- public command/help contract -------------------------------------------
# Derive the list from the dispatcher's `local cmds` declaration so adding a
# public subcommand without updating the help is a release-blocking failure.
public_cmds=$(awk '
    /^[[:space:]]*local cmds[[:space:]]/ {
        sub(/^[[:space:]]*local cmds[[:space:]]+/, "")
        active=1
    }
    active {
        continued=($0 ~ /\/\/\/[[:space:]]*$/)
        sub(/[[:space:]]*\/\/\/[[:space:]]*$/, "")
        print
        if (!continued) exit
    }
' "$REPO/src/ado/p/parqit.ado")

for cmd in $public_cmds; do
    [ "$cmd" = "_dlgvars" ] && continue
    awk -v needle="{cmd:parqit $cmd" '
        /\{marker syntax\}/       { inside=1 }
        /\{marker description\}/  { inside=0 }
        inside && index($0, needle) { found=1 }
        END { exit(found ? 0 : 1) }
    ' "$REPO/src/ado/p/parqit.sthlp" || \
        err "public subcommand '$cmd' is absent from the help syntax section"
done

# The help carries one delimited block listing the expression functions. Both
# directions are release-blocking: an implemented function missing from the
# block (undocumented surface) and a name in the block that the translator does
# not implement (a promise parqit cannot keep).
expr_fns=$(grep -oE 'fname == "[A-Za-z0-9_]+"' \
               "$REPO/src/engine/exprtrans.cpp" \
           | sed -E 's/.*"([A-Za-z0-9_]+)"/\1/' | sort -u)
[ -n "$expr_fns" ] || err "could not derive expression functions from exprtrans.cpp"

help_fns=$(awk '
    /parqit-lint: expression-function-list begin/ { inside=1; next }
    /parqit-lint: expression-function-list end/   { inside=0 }
    inside {
        line=$0
        # keep only what is inside {cmd:...} groups: prose between the groups
        # ("and the date literals") must not be read as function names
        out=""
        while (match(line, /\{cmd:[^}]*\}/)) {
            out = out " " substr(line, RSTART + 5, RLENGTH - 6)
            line = substr(line, RSTART + RLENGTH)
        }
        gsub(/[^[:alnum:]_]/, " ", out)
        print out
    }
' "$REPO/src/ado/p/parqit.sthlp" | tr ' ' '\n' | grep -v '^$' | sort -u)
[ -n "$help_fns" ] || \
    err "could not read the delimited expression-function list from parqit.sthlp"

for fn in $expr_fns; do
    printf '%s\n' "$help_fns" | grep -qx "$fn" || \
        err "expression function '$fn()' is absent from the help expression-function list"
done
for fn in $help_fns; do
    printf '%s\n' "$expr_fns" | grep -qx "$fn" || \
        err "help lists expression function '$fn()', which exprtrans.cpp does not implement"
done

# Every internal reference — viewerjumpto targets and inline {help parqit##x:…}
# links alike — must resolve to a marker in the same file.
jump_targets=$(grep -oE 'parqit##[A-Za-z0-9_]+' \
                   "$REPO/src/ado/p/parqit.sthlp" \
               | sed 's/^parqit##//' | sort -u)
[ -n "$jump_targets" ] || err "no parqit## targets found in parqit.sthlp"
for target in $jump_targets; do
    grep -Fq "{marker $target}" "$REPO/src/ado/p/parqit.sthlp" || \
        err "help jump target 'parqit##$target' has no marker"
done

# Inline SMCL directives cannot span physical lines: Stata otherwise prints
# the opening token literally (for example "{bf:") instead of styling it.
smcl_open=$(grep -nE '\{(bf|it|cmd|opt):[^}]*$' \
                "$REPO/src/ado/p/parqit.sthlp" || true)
[ -z "$smcl_open" ] || err "unterminated inline SMCL directive(s): $(echo "$smcl_open")"

# --- menu/dialog command coverage and geometry ------------------------------
# Parse the dialog language rather than relying only on grep-shaped coverage:
# control references must resolve, LIST triplets must align, optionarg targets
# must declare option(), FILE controls must use smartquote, and the Populate
# helper must expose the complete and semantically correct variable source.
if ! python3 "$REPO/tests/dialog_lint.py" "$REPO"; then
    err "static dialog-language contract failed"
fi

# The menu is an entrypoint into the dialog family. Derive both sides from the
# live dispatcher/dialogs so a future public verb cannot ship as command-line
# only by accident. `menu` installs the surface and `_dlgvars` is private dialog
# glue, so neither should itself be an action generated by a dialog.
menu_public_cmds=$(awk '
    /^[[:space:]]*local cmds[[:space:]]/ {
        sub(/^[[:space:]]*local cmds[[:space:]]+/, "")
        active=1
    }
    active {
        continued=($0 ~ /\/\/\/[[:space:]]*$/)
        sub(/[[:space:]]*\/\/\/[[:space:]]*$/, "")
        print
        if (!continued) exit
    }
' "$REPO/src/ado/p/parqit.ado")

gui_cmds=$(grep -hoE 'put "parqit [A-Za-z0-9_]+' \
               "$REPO"/src/ado/p/parqit_*.dlg \
           | sed -E 's/^put "parqit //' | sort -u)
[ -n "$gui_cmds" ] || err "could not derive parqit commands from dialogs"

for cmd in $menu_public_cmds; do
    [ "$cmd" = "menu" ] && continue
    [ "$cmd" = "_dlgvars" ] && continue
    printf '%s\n' "$gui_cmds" | grep -qx "$cmd" || \
        err "public subcommand '$cmd' is not exposed by a parqit dialog"
done

# A single geometry policy prevents controls designed for the 600-unit canvas
# from silently being rendered in Stata's 420-unit medium dialog. Every shipped
# dialog must also be reachable from the idempotent User-menu installer.
for dlg in "$REPO"/src/ado/p/parqit_*.dlg; do
    base=$(basename "$dlg" .dlg)
    nwide=$(grep -cE '^INCLUDE _std_wide$' "$dlg")
    [ "$nwide" = "1" ] || err "$base.dlg must include _std_wide exactly once"
    grep -Fq "\"db $base\"" "$REPO/src/ado/p/parqit.ado" || \
        err "$base.dlg is not linked from parqit menu"
done

menu_dlgs=$(awk '
    /^program define _parqit_menu/ { inside=1 }
    inside && /window menu append item/ { print }
    inside && /^end$/ { exit }
' "$REPO/src/ado/p/parqit.ado" | grep -oE 'db parqit_[A-Za-z0-9_]+' \
  | sed 's/^db //' | sort -u)
for base in $menu_dlgs; do
    [ -f "$REPO/src/ado/p/$base.dlg" ] || \
        err "parqit menu links missing dialog $base.dlg"
done

# --- report ------------------------------------------------------------------
if [ "$fail" -eq 0 ]; then
    printf 'release-lint OK: v%s (%s / pkg %s); CHANGELOG top [%s]\n' \
        "$cmake_v" "$ado_d" "$pkg_d" "$chg_top"
fi
exit "$fail"
