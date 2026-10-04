#!/usr/bin/env bash
#
# Line-coverage gate for the unit-testable sources.
#
# Scope, and why it is what it is: this measures the three Qt6::Core-only
# libraries the unit targets link (config.cpp, frame_bytes.cpp, terminal_ui.cpp).
# The browser, http, cdp and pdf subsystems are exercised only by the vitest and
# e2e suites, which drive a *separate* anoa process — an uninstrumented
# binary reports nothing back, so folding those files in would report 0% for code
# that is in fact well covered. Widening this gate means instrumenting the main
# binary and teaching the integration suites to preserve its .gcda, which is a
# different piece of work.
#
#   bash tests/coverage.sh
#   COVERAGE_MIN=90 bash tests/coverage.sh
#   QT_PREFIX=/opt/Qt/6.7.3/gcc_64 bash tests/coverage.sh
#
# Reports per-file source-line coverage and fails if any file is under
# COVERAGE_MIN (default 80).
#
# On the number gcov prints: gcov's own "File ... Lines executed:N% of M"
# summary inflates M with Qt template instantiations (QArrayDataPointer<QString>
# and friends) that land in the translation unit but are not lines of the source
# file. This script counts the per-line listing in the .gcov file instead, which
# is the actual source. For config.cpp the two differ by 14 lines.

set -eu

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-${ROOT}/build-coverage}"
COVERAGE_MIN="${COVERAGE_MIN:-80}"

if [ -z "${QT_PREFIX:-}" ]; then
  case "$(uname -s)" in
    Darwin) QT_PREFIX=/opt/homebrew ;;
    *)      QT_PREFIX=/usr ;;
  esac
fi

if ! command -v gcov >/dev/null 2>&1; then
  echo "coverage: gcov not found — install gcc (or lcov) and retry" >&2
  exit 2
fi

# The instrumented sources, paired with the CMake object directory holding
# their .gcda. Kept explicit rather than globbed so a new source silently
# joining a target cannot slip past the gate unnoticed.
TARGETS="
anoa-config-lib.dir/__/__/src/config/config.cpp
anoa-terminal-bytes-lib.dir/__/__/src/terminal/frame_bytes.cpp
anoa-terminal-ui-lib.dir/__/__/src/terminal/terminal_ui.cpp
anoa-tab-ids-lib.dir/__/__/src/browser/tab_ids.cpp
anoa-mcp-tools-lib.dir/__/__/src/mcp/mcp_tools.cpp
"

echo "==> configuring ${BUILD_DIR} (Qt: ${QT_PREFIX})"
cmake -B "${BUILD_DIR}" -S "${ROOT}" \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_PREFIX_PATH="${QT_PREFIX}" \
  -DBUILD_TESTS=ON \
  -DANOA_TEST_COVERAGE=ON >/dev/null

echo "==> building the unit targets"
cmake --build "${BUILD_DIR}" \
  --target anoa-test-config \
           anoa-test-frame-bytes \
           anoa-test-terminal-ui \
           anoa-test-tab-ids \
           anoa-test-mcp-tools \
  -- -j"$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)" >/dev/null

# Stale counters from a previous run merge into this one and make the result a
# function of how often the suite has been run. Always start from zero.
find "${BUILD_DIR}" -name '*.gcda' -delete

echo "==> running the unit suite"
( cd "${BUILD_DIR}" && ctest --output-on-failure )

WORK="${BUILD_DIR}/.coverage-report"
rm -rf "${WORK}"
mkdir -p "${WORK}"

echo
printf '%-22s %10s %10s %9s\n' FILE COVERED LINES COVERAGE
printf '%-22s %10s %10s %9s\n' "----" "-------" "-----" "--------"

total_hit=0
total_lines=0
failed=0

for rel in ${TARGETS}; do
  gcda="${BUILD_DIR}/tests/unit/CMakeFiles/${rel}.gcda"
  base="$(basename "${rel}")"

  if [ ! -f "${gcda}" ]; then
    echo "coverage: no .gcda for ${base} — the suite did not exercise it at all" >&2
    failed=1
    continue
  fi

  ( cd "${WORK}" && gcov "${gcda}" >/dev/null 2>&1 )

  report="${WORK}/${base}.gcov"
  if [ ! -f "${report}" ]; then
    echo "coverage: gcov produced no listing for ${base}" >&2
    failed=1
    continue
  fi

  # Count the per-line listing: "#####" is an unexecuted line, a leading digit
  # is an executed one, and "-" marks a line with no code on it.
  read -r hit lines < <(awk -F':' '
    { c = $1; gsub(/ /, "", c)
      if (c == "#####") miss++
      else if (c ~ /^[0-9]/) hit++ }
    END { printf "%d %d\n", hit, hit + miss }
  ' "${report}")

  if [ "${lines}" -eq 0 ]; then
    echo "coverage: ${base} reported zero executable lines" >&2
    failed=1
    continue
  fi

  pct="$(awk -v h="${hit}" -v l="${lines}" 'BEGIN { printf "%.2f", 100 * h / l }')"
  total_hit=$((total_hit + hit))
  total_lines=$((total_lines + lines))

  mark=""
  if awk -v p="${pct}" -v m="${COVERAGE_MIN}" 'BEGIN { exit !(p < m) }'; then
    mark="  << under ${COVERAGE_MIN}%"
    failed=1
  fi
  printf '%-22s %10s %10s %8s%%%s\n' "${base}" "${hit}" "${lines}" "${pct}" "${mark}"
done

if [ "${total_lines}" -eq 0 ]; then
  echo "coverage: nothing was measured" >&2
  exit 1
fi

total_pct="$(awk -v h="${total_hit}" -v l="${total_lines}" 'BEGIN { printf "%.2f", 100 * h / l }')"
printf '%-22s %10s %10s %8s%%\n' TOTAL "${total_hit}" "${total_lines}" "${total_pct}"
echo
echo "Per-line listings: ${WORK}"

if [ "${failed}" -ne 0 ]; then
  echo "coverage: FAILED — threshold is ${COVERAGE_MIN}%" >&2
  exit 1
fi

echo "coverage: OK — every measured file is at or above ${COVERAGE_MIN}%"
