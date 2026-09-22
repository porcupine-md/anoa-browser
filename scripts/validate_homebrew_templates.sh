#!/usr/bin/env bash
# Validate the Homebrew tap files against the two rules Homebrew enforces:
#
#   1. A formula's class name is the CamelCase form of its file base name
#      (Formula/anoa-linux.rb must declare class AnoaLinux).
#   2. A cask's token equals its file base name (Casks/anoa.rb -> cask "anoa").
#
# One file breaking either rule makes the WHOLE tap un-tappable ("Cannot tap
# ...: invalid syntax in tap!") — every install through the tap fails, not
# just the broken one. v0.7.3 shipped exactly that and it survived nine
# releases because nothing parsed the rendered output before it was pushed
# (issue #37): class AnoaBrowserLinux sat in Formula/anoa-linux.rb.
#
# Usage:
#   scripts/validate_homebrew_templates.sh             # render .github/homebrew/*.tpl
#                                                      # with dummy values, validate
#   scripts/validate_homebrew_templates.sh <tap-dir>   # validate a rendered tap
#                                                      # checkout (Formula/, Casks/)
#
# The first form is what CI runs on every push; the second is what the tap
# workflows run on the rendered checkout right before git push — verify the
# artifact, not the working tree. Exits 0 when everything passes, 1 otherwise.

set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
fail=0

# anoa-linux -> AnoaLinux, matching how Homebrew derives the class name.
camel_case() {
  printf '%s' "$1" | awk -F'[-_]' '{
    out = ""
    for (i = 1; i <= NF; i++) out = out toupper(substr($i, 1, 1)) substr($i, 2)
    print out
  }'
}

check_file() {
  local file="$1" base expected class token
  base=$(basename "$file" .rb)

  # A leftover {{PLACEHOLDER}} means the sed substitutions in the workflow and
  # this renderer have drifted apart — the pushed file would carry garbage.
  # Braces are written as [{}] classes: a bare {{...}} pattern with a bracket
  # expression inside does not match under BSD grep/sed (macOS) though it does
  # under GNU (Linux/CI), and the script runs on both.
  if grep -q '[{][{]' "$file"; then
    echo "FAIL $file: unreplaced placeholder(s): $(grep -o '[{][{][A-Za-z_]*[}][}]' "$file" | tr '\n' ' ')"
    fail=1
  fi

  if grep -q '^cask "' "$file"; then
    token=$(awk -F'"' '/^cask "/{print $2; exit}' "$file")
    if [ -z "$token" ]; then
      echo "FAIL $file: no \"cask \\\"...\\\" do\" declaration found"
      fail=1
    elif [ "$token" != "$base" ]; then
      echo "FAIL $file: cask \"$token\" does not match file name (expected \"$base\")"
      fail=1
    fi
  else
    expected=$(camel_case "$base")
    class=$(awk '/^class /{print $2; exit}' "$file")
    if [ -z "$class" ]; then
      echo "FAIL $file: no class declaration found"
      fail=1
    elif [ "$class" != "$expected" ]; then
      echo "FAIL $file: class $class does not match file name (Homebrew expects $expected)"
      fail=1
    fi
  fi

  # Syntax-only parse; no Homebrew DSL needed. Optional so the script also
  # runs where ruby is not installed.
  if command -v ruby >/dev/null 2>&1; then
    if ! ruby -c "$file" >/dev/null 2>&1; then
      echo "FAIL $file: ruby syntax error:"
      ruby -c "$file" 2>&1 | sed 's/^/  /'
      fail=1
    fi
  fi
}

validate_tap_dir() {
  local dir="$1" file found=0
  for file in "$dir"/Formula/*.rb "$dir"/Casks/*.rb; do
    [ -e "$file" ] || continue
    found=1
    check_file "$file"
  done
  if [ "$found" -eq 0 ]; then
    echo "FAIL $dir: no .rb files found under Formula/ or Casks/"
    fail=1
  fi
}

if [ $# -ge 2 ]; then
  echo "usage: $0 [tap-dir]" >&2
  exit 2
fi

if [ $# -eq 1 ]; then
  validate_tap_dir "$1"
else
  tpl_dir="$SCRIPT_DIR/../.github/homebrew"
  tmp=$(mktemp -d)
  trap 'rm -rf "$tmp"' EXIT
  # Same substitutions the tap workflows make with real values; the values
  # themselves do not matter, the shape of the output does.
  for tpl in "$tpl_dir"/*.tpl; do
    out="$tmp/$(basename "$tpl" .tpl)"
    sed -e 's/[{][{]VERSION[}][}]/0.0.0-test/' \
        -e 's/[{][{]MACOS_SHA256[}][}]/0000000000000000000000000000000000000000000000000000000000000000/' \
        -e 's/[{][{]LINUX_X86_64_SHA256[}][}]/0000000000000000000000000000000000000000000000000000000000000000/' \
        -e 's/[{][{]LINUX_AARCH64_SHA256[}][}]/0000000000000000000000000000000000000000000000000000000000000000/' \
        "$tpl" > "$out"
    check_file "$out"
  done
fi

if [ "$fail" -ne 0 ]; then
  echo "Homebrew tap validation FAILED (one invalid file blocks the whole tap)"
  exit 1
fi
echo "Homebrew tap validation passed"
