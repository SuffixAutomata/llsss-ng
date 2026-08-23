#!/usr/bin/env bash
set -euo pipefail

binary=${1:?usage: search_regression.sh /path/to/rlife_llsss [smoke|full]}
mode=${2:-full}
if [[ $mode != smoke && $mode != full ]]; then
  echo 'test mode must be smoke or full' >&2
  exit 2
fi
test_tmp=$(mktemp -d "${TMPDIR:-/tmp}/rlife-regression.XXXXXX")
trap 'rm -rf "$test_tmp"' EXIT

normalize_hash() {
  sed -n 's/^.*cols: //p' "$1" \
    | sed 's/[^0-9][^0-9]*/ /g;s/^ //;s/ $//' \
    | sha256sum \
    | awk '{print $1}'
}

run_search() {
  local geometry=$1
  local width=$2
  local stdout_file=$3
  local stderr_file=$4
  timeout "${RLIFE_TEST_TIMEOUT:-180}" "$binary" llsss \
    --rule 'B35678/S4678' \
    --left-edge bg \
    --filters bcaf \
    "$geometry" "@bg($width)" \
    >"$stdout_file" 2>"$stderr_file"
}

if [[ $mode == smoke ]]; then
  run_search c3d-f2b 7 "$test_tmp/diagonal.out" "$test_tmp/diagonal.err"
  grep -Fq 'search exhausted at flattened depth 16 (w_pos 8[0])' "$test_tmp/diagonal.out"
  [[ $(grep -c 'cols:' "$test_tmp/diagonal.err") == 4 ]]
  diagonal_hash=$(normalize_hash "$test_tmp/diagonal.err")
  [[ $diagonal_hash == 913d9681e52fedd3e8bd8a0ca32ef758e32a8fc40af00454b515ee2368956aff ]]

  run_search 2c4d-f2b 7 "$test_tmp/four-subtile.out" "$test_tmp/four-subtile.err"
  grep -Fq 'search exhausted at flattened depth 20 (w_pos 5[0])' "$test_tmp/four-subtile.out"
  [[ $(grep -c 'cols:' "$test_tmp/four-subtile.err") == 4 ]]
  four_subtile_hash=$(normalize_hash "$test_tmp/four-subtile.err")
  [[ $four_subtile_hash == 92692a524225d9f5d02253fc1e12459e06f958e4b7a876dfc6335d08be7bbc99 ]]

  timeout "${RLIFE_TEST_TIMEOUT:-180}" "$binary" llsss \
    --rule B3/S23 --left-edge bg --filters bcaf --partials none \
    --partial-output "$test_tmp/completion.rle" c4d-f2b '@bg(6)' \
    >"$test_tmp/completion.out" 2>"$test_tmp/completion.err"
  grep -Fq 'completion at flattened depth 48 (w_pos 24[0])' "$test_tmp/completion.out"
  grep -Fq '#C physical time phases 0..3 left-to-right; gap=16' "$test_tmp/completion.rle"
  grep -Fq 'x = 94, y = 12, rule = B3/S23' "$test_tmp/completion.rle"
  [[ $(sha256sum "$test_tmp/completion.rle" | awk '{print $1}') == 8571b78b68c4f8db6d845fe68dd9ead52c589fa0c2c19d269d07a10543fcee40 ]]
else
  run_search c5-f2b 11 "$test_tmp/orth.out" "$test_tmp/orth.err"
  grep -Fq 'search exhausted at height 37' "$test_tmp/orth.out"
  [[ $(grep -c 'cols:' "$test_tmp/orth.err") == 27 ]]
  orth_hash=$(normalize_hash "$test_tmp/orth.err")
  [[ $orth_hash == c4771a773ab5a77a7727a92c9116bd689d2d56e345154489abdbf3bd2cb1f387 ]]

  run_search c5d-f2b 11 "$test_tmp/diagonal.out" "$test_tmp/diagonal.err"
  grep -Fq 'search exhausted at flattened depth 134 (w_pos 67[0])' "$test_tmp/diagonal.out"
  [[ $(grep -c 'cols:' "$test_tmp/diagonal.err") == 114 ]]
  diagonal_hash=$(normalize_hash "$test_tmp/diagonal.err")
  [[ $diagonal_hash == e24f646987b57b50b3c16f0d066a33f48338f1c17b46a503abd98ad3679a6a28 ]]
fi

if "$binary" llsss --left-edge odd --partials none --ends none c5d-f2b '@bg(5)' >"$test_tmp/edge.out" 2>"$test_tmp/edge.err"; then
  echo 'diagonal odd edge unexpectedly succeeded' >&2
  exit 1
fi
grep -Fq 'diagonal geometries currently support background edges only' "$test_tmp/edge.err"

if "$binary" llsss --partials none --ends none 2c4-f2b '@bg(5)' >"$test_tmp/nonprimitive.out" 2>"$test_tmp/nonprimitive.err"; then
  echo 'non-primitive orthogonal geometry unexpectedly succeeded' >&2
  exit 1
fi
grep -Fq 'non-coprime orthogonal periods are not supported' "$test_tmp/nonprimitive.err"

if "$binary" llsss --partials none --ends none 2147483647c2147483647d-f2b '@bg(3)' >"$test_tmp/huge.out" 2>"$test_tmp/huge.err"; then
  echo 'oversized diagonal geometry unexpectedly succeeded' >&2
  exit 1
fi
grep -Fq 'diagonal geometry has too many lattice subtiles' "$test_tmp/huge.err"

echo "$mode diagonal search regression passed"
