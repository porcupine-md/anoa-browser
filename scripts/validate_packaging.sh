#!/usr/bin/env bash
# Validate the OBS packaging recipes before they are pushed.
#
# Same principle as validate_homebrew_templates.sh, and the same reason: the
# tap shipped a broken formula for nine releases because nothing parsed the
# rendered output before pushing it (issue #37). OBS fails later and louder —
# a bad spec is a build that runs for minutes on someone else's infrastructure
# before telling you the %files list named a path the install never created.
#
# The check that earns its keep is the last one: every path the spec claims in
# %files must be a path CMakeLists.txt actually installs. Those two lists live
# in different files, in different languages, and nothing but this connects
# them.
#
# Usage:
#   scripts/validate_packaging.sh            # render packaging/obs/* with dummy
#                                            # values, validate
#   scripts/validate_packaging.sh <dir>      # validate an already-rendered dir
#
# The first form is what CI runs on every push; the second is what the publish
# workflow runs on the rendered files right before osc commit — verify the
# artifact, not the working tree. Exits 0 when everything passes, 1 otherwise.

set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "${SCRIPT_DIR}/.." && pwd)
fail=0

note() { printf '  %s\n' "$1"; }
bad()  { printf '  FAIL  %s\n' "$1"; fail=1; }

# A leftover {{PLACEHOLDER}} means the workflow's sed list and these templates
# have drifted apart, and the pushed file would carry the literal braces.
# Braces are written as [{}] classes because a bare {{...}} pattern with a
# bracket expression behaves differently under BSD grep (macOS) and GNU grep
# (the CI runner) — the same portability trap the Homebrew validator hit.
check_no_placeholders() {
  local file="$1"
  if grep -qE '[{][{][A-Z_]+[}][}]' "$file"; then
    bad "$(basename "$file"): unrendered placeholder: $(grep -oE '[{][{][A-Z_]+[}][}]' "$file" | sort -u | tr '\n' ' ')"
  fi
}

# Every %files path must be one CMakeLists.txt installs. A spec that claims a
# file the install never produced fails the build with "No such file or
# directory" after everything else has already succeeded — and an unknown path
# must FAIL here rather than be waved through, which is the mistake the first
# version of this function made.
installed_paths() {
  # install(TARGETS ... RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}) -> bin/anoa
  grep -q 'RUNTIME DESTINATION' "${ROOT}/CMakeLists.txt" && echo "bin/anoa"
  # install(FILES <src> DESTINATION <dest>) -> <dest>/<basename src>
  awk '
    /install\(FILES/        { src = $2; getline; }
    /DESTINATION/ && src != "" {
      dest = $2
      sub(/\)$/, "", dest)
      gsub(/\$\{CMAKE_INSTALL_DATAROOTDIR\}/, "share", dest)
      n = split(src, parts, "/")
      print dest "/" parts[n]
      src = ""
    }
  ' "${ROOT}/CMakeLists.txt"
}

check_files_against_cmake() {
  local spec="$1" path rpmpath known
  known=$(installed_paths)

  while read -r path; do
    [ -n "$path" ] || continue
    rpmpath=$(printf '%s' "$path" \
      | sed -e 's|%{_bindir}|bin|' -e 's|%{_datadir}|share|')
    if ! printf '%s\n' "$known" | grep -qxF "$rpmpath"; then
      bad "spec claims $path, which CMakeLists.txt does not install"
      bad "  it installs: $(printf '%s' "$known" | tr '\n' ' ')"
    fi
  done < <(grep -oE '%\{_(bindir|datadir)\}[^[:space:]]*' "$spec" | sort -u)
}

# The Qt floor is declared in three places and they have to agree, or a distro
# with 6.4 builds a package whose binary needs 6.8 — found by a user, not by us.
check_qt_floor() {
  local spec="$1" control="$2" cmake_floor
  cmake_floor=$(sed -n 's/.*find_package(Qt6 \([0-9.]*\).*/\1/p' "${ROOT}/CMakeLists.txt" | head -1)
  [ -n "$cmake_floor" ] || { bad "could not read the Qt floor from CMakeLists.txt"; return; }

  grep -q ">= ${cmake_floor}" "$spec" \
    || bad "anoa.spec does not require Qt >= ${cmake_floor} (CMakeLists.txt does)"
  grep -q ">= ${cmake_floor}" "$control" \
    || bad "debian.control does not require Qt >= ${cmake_floor} (CMakeLists.txt does)"
  note "Qt floor ${cmake_floor}: spec and debian.control agree with CMakeLists.txt"
}

# PORTABLE_RPATH exists for packagers and is the one flag that must never be
# forgotten: left on, the installed binary names a directory no package ships
# and does not start at all.
check_portable_rpath_off() {
  local spec="$1" rules="$2"
  # Comments are stripped first: both files explain the flag in prose directly
  # above it, and grepping the whole file found the explanation rather than the
  # setting — so flipping the flag to ON still passed.
  uncommented() { sed -e 's/[[:space:]]*#.*$//' "$1"; }
  uncommented "$spec"  | grep -q -- '-DPORTABLE_RPATH=OFF' \
    || bad "anoa.spec does not set -DPORTABLE_RPATH=OFF"
  uncommented "$rules" | grep -q -- '-DPORTABLE_RPATH=OFF' \
    || bad "debian.rules does not set -DPORTABLE_RPATH=OFF"
  note "PORTABLE_RPATH=OFF in both recipes"
}

# Optional, like the Homebrew validator's ruby -c: a real parse where the tool
# exists, skipped where it does not, so the script runs on a developer's macOS.
check_spec_parses() {
  local spec="$1"
  if command -v rpmspec >/dev/null 2>&1; then
    if rpmspec --parse "$spec" >/dev/null 2>&1; then
      note "rpmspec --parse: ok"
    else
      bad "anoa.spec does not parse: $(rpmspec --parse "$spec" 2>&1 | tail -2 | tr '\n' ' ')"
    fi
  else
    note "rpmspec not installed — skipping the syntax parse"
  fi
}

validate_dir() {
  local dir="$1"
  local spec="${dir}/anoa.spec" control="${dir}/debian.control" rules="${dir}/debian.rules"

  for f in "$spec" "$control" "$rules" "${dir}/debian.changelog" "${dir}/anoa.dsc"; do
    [ -f "$f" ] || { bad "missing: $(basename "$f")"; return; }
    check_no_placeholders "$f"
  done

  check_qt_floor "$spec" "$control"
  check_portable_rpath_off "$spec" "$rules"
  check_files_against_cmake "$spec"
  check_spec_parses "$spec"
}

if [ $# -ge 1 ]; then
  echo "==> validating rendered packaging in $1"
  validate_dir "$1"
else
  echo "==> rendering packaging/obs with dummy values"
  tmp=$(mktemp -d)
  trap 'rm -rf "${tmp}"' EXIT
  # The same substitutions the publish workflow makes with real values. The
  # values do not matter here; the shape of the output does.
  for f in "${ROOT}"/packaging/obs/*; do
    sed -e 's/{{VERSION}}/0.0.0/g' \
        -e 's/{{DATE}}/Mon, 01 Jan 2029 00:00:00 +0000/g' \
        -e 's/{{DATE_RPM}}/Mon Jan 01 2029/g' \
        "$f" > "${tmp}/$(basename "$f")"
  done
  validate_dir "${tmp}"
fi

if [ "${fail}" -ne 0 ]; then
  echo "packaging: FAILED" >&2
  exit 1
fi
echo "packaging: OK"
