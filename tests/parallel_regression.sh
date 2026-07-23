#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 1 ]]; then
    echo "usage: $0 /path/to/rlife_llsss" >&2
    exit 2
fi

executable=$1
test_directory=$(mktemp -d)
trap 'rm -rf -- "$test_directory"' EXIT

compare_case() {
    local name=$1
    shift
    "$executable" llsss "$@" --threads 1 \
        >"$test_directory/$name.serial.out" \
        2>"$test_directory/$name.serial.err"
    "$executable" llsss "$@" --threads 2 \
        >"$test_directory/$name.parallel.out" \
        2>"$test_directory/$name.parallel.err"
    cmp "$test_directory/$name.serial.out" \
        "$test_directory/$name.parallel.out"
}

# This crosses the no-gate initialization, parent-of-leaf old gate, and
# current-leaf materialized gate cases.  BCAF's cached partial/completion
# reconstruction also checks path summaries and first-edge range ordering.
compare_case bcaf \
    --rule B3/S12 --filters '[bcaf]' \
    --partials every:1 --symmetry odd --halts w_pos:28 \
    3c7-f2b '@bg:6'

# The non-BCAF path builds and concatenates its relation tape independently,
# and reifies from the intersection of the two directional reachability tags.
compare_case non_bcaf \
    --rule B3/S12 --filters none --ends none \
    --partials every:1 --symmetry odd --halts w_pos:28 \
    3c7-f2b '@bg:8'
