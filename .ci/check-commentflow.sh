#!/usr/bin/env bash

# Reflow comments to the column limit, or check that they already are.
#
# clang-format breaks a comment line that runs past ColumnLimit but never
# refills a short-wrapped one, so comment width is the one part of the house
# style no other gate settles. commentflow reads the same ColumnLimit from
# .clang-format, which keeps comment width and code width on one number.
#
# The selection below is shared by --write and the default check, so what gets
# rewritten is what gets checked.
#
# Exit codes: 0 clean, 1 files would change, 2 the check could not run.

set -u -o pipefail

cd "$(dirname "${BASH_SOURCE[0]}")/.." || exit 2

case "${1:-}" in
    "" | --check) write=0 ;;
    --write) write=1 ;;
    *)
        echo "usage: $0 [--check | --write]" >&2
        exit 2
        ;;
esac

COMMENTFLOW="${COMMENTFLOW:-commentflow}"

if ! command -v "$COMMENTFLOW" > /dev/null 2>&1; then
    echo "Error: $COMMENTFLOW not found" >&2
    echo "Install it from https://github.com/sysprog21/commentflow" >&2
    exit 2
fi

# Untracked but non-ignored files are included, so a new file is covered before
# it is staged. A while-read loop because macOS ships bash 3.2 (no mapfile).
# Enumerated before the loop so a failing git is an error, not an empty list.
list=$(git ls-files -z --cached --others --exclude-standard \
    -- '*.c' '*.h' '*.sh' | tr '\0' '\n') || exit 2
files=()
while IFS= read -r f; do
    [ -n "$f" ] && files+=("$f")
done <<< "$list"

if [ "${#files[@]}" -eq 0 ]; then
    echo "Error: no files selected; the pathspec no longer matches the tree" >&2
    exit 2
fi

printf '  CFLOW   %d files\n' "${#files[@]}"

if [ "$write" -eq 1 ]; then
    exec "$COMMENTFLOW" "${files[@]}"
fi

status=0
"$COMMENTFLOW" --check "${files[@]}" || status=$?

if [ "$status" -eq 0 ]; then
    echo "Comment reflow clean."
    exit 0
fi

# Exit 1 means "these files would change"; anything else is the tool failing to
# read them, which must not be reported as a style violation.
if [ "$status" -ne 1 ]; then
    echo "Error: $COMMENTFLOW exited $status (I/O or parse error)" >&2
    exit "$status"
fi

echo "Run '$0 --write' to reflow, or '$COMMENTFLOW --diff <file>' to see one." >&2
exit 1
