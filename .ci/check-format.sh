#!/usr/bin/env bash

# Verify clang-format conformance for all tracked C/H files. The repository's
# .clang-format is calibrated against clang-format-20; other versions produce
# different output and are rejected to keep CI deterministic.
#
# Exit codes, matching .ci/check-commentflow.sh: 0 clean, 1 files differ from
# the style, 2 the check could not run.

set -u -o pipefail

cd "$(dirname "${BASH_SOURCE[0]}")/.." || exit 2

if [ -z "${CLANG_FORMAT:-}" ]; then
    if command -v clang-format-20 > /dev/null 2>&1; then
        CLANG_FORMAT="clang-format-20"
    else
        CLANG_FORMAT="clang-format"
    fi
fi

# The pin is checked on whatever binary was resolved, an explicit CLANG_FORMAT
# included, so it cannot be bypassed by naming one.
if ! "$CLANG_FORMAT" --version 2> /dev/null | grep -qE 'version 20\.'; then
    echo "Error: clang-format 20 is required (other versions differ in style)" >&2
    exit 2
fi

expected=$(mktemp)
trap 'rm -f "$expected"' EXIT

# Enumerate first: a failing git inside a process substitution would be
# indistinguishable from "no files" and pass the gate.
sources=$(git ls-files -- '*.c' '*.h') || exit 2
if [ -z "$sources" ]; then
    echo "Error: no C sources tracked; the pathspec no longer matches" >&2
    exit 2
fi

# A tool failure (2) outranks a style difference (1).
rc=0
for file in $sources; do
    if ! "$CLANG_FORMAT" "$file" > "$expected"; then
        echo "Error: $CLANG_FORMAT failed on $file" >&2
        rc=2
        continue
    fi
    st=0
    diff -u -p --label="$file" --label="expected coding style" \
        "$file" "$expected" || st=$?
    if [ "$st" -gt 1 ]; then
        rc=2
    elif [ "$st" -eq 1 ] && [ "$rc" -eq 0 ]; then
        rc=1
    fi
done

exit "$rc"
