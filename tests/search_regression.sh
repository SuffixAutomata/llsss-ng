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
    --save none \
    "$geometry" "@bg($width)" \
    >"$stdout_file" 2>"$stderr_file"
}

run_tiny_edge() {
  local edge=$1
  local geometry=$2
  local halt=$3
  local stdout_file=$4
  local stderr_file=$5
  timeout "${RLIFE_TEST_TIMEOUT:-180}" "$binary" llsss \
    --rule B3578/S24678 --left-edge "$edge" --filters bcaf \
    --partials none --ends none --halts "w_pos:$halt" --save none \
    "$geometry" '@bg(3)' >"$stdout_file" 2>"$stderr_file"
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
    --rule B3578/S24678 --left-edge gse --filters bcaf \
    --partials final --partial-output "$test_tmp/gse.rle" \
    --ends none --halts w_pos:14 \
    --save final --savefile "$test_tmp/gse-checkpoint" \
    c6d-f2b '@bg:4' >"$test_tmp/gse.out" 2>"$test_tmp/gse.err"
  grep -Fq 'w_pos halt at 14[0]' "$test_tmp/gse.out"
  [[ $(grep -c 'cols:' "$test_tmp/gse.err") == 5 ]]
  gse_hash=$(normalize_hash "$test_tmp/gse.err")
  [[ $gse_hash == c97a1a13840c7c34d0f1b343a149e64fe919f0fa539877a2544aa0c33022de4a ]]
  grep -Fq '#C physical time phases 0..5 left-to-right; gap=16' "$test_tmp/gse.rle"
  grep -Eq '^x = [0-9]+, y = [0-9]+, rule = B3578/S24678$' "$test_tmp/gse.rle"
  tail -n +4 "$test_tmp/gse.rle" | grep -q 'o'
  test -s "$test_tmp/gse-checkpoint_28"
  "$binary" llsss --load "$test_tmp/gse-checkpoint_28" --partials none --save none \
    >"$test_tmp/gse-reload.out" 2>"$test_tmp/gse-reload.err"
  grep -Fq 'left_edge=gse' "$test_tmp/gse-reload.out"

  # An explicit partial request on an already-halted checkpoint prints the
  # current row without advancing it.  Ordinary reloads still avoid appending
  # a duplicate inherited partial.
  "$binary" llsss --load "$test_tmp/gse-checkpoint_28" --partials every:1 \
    --partial-output "$test_tmp/gse-reloaded.rle" --save none \
    >"$test_tmp/gse-print-current.out" 2>"$test_tmp/gse-print-current.err"
  cmp "$test_tmp/gse.rle" "$test_tmp/gse-reloaded.rle"
  [[ $(grep -c '^#C llsss halt ' "$test_tmp/gse-reloaded.rle") == 1 ]]
  if grep -Eq '^Row (29|[3-9][0-9])' "$test_tmp/gse-print-current.err"; then
    echo 'printing a loaded partial unexpectedly advanced the search' >&2
    exit 1
  fi

  timeout "${RLIFE_TEST_TIMEOUT:-180}" "$binary" llsss \
    --rule B3/S23 --left-edge bg --filters bcaf --partials none \
    --save final --savefile "$test_tmp/completion-checkpoint" \
    --partial-output "$test_tmp/completion.rle" c4d-f2b '@bg(6)' \
    >"$test_tmp/completion.out" 2>"$test_tmp/completion.err"
  grep -Fq 'completion at flattened depth 48 (w_pos 24[0])' "$test_tmp/completion.out"
  grep -Fq '#C physical time phases 0..3 left-to-right; gap=16' "$test_tmp/completion.rle"
  grep -Fq 'x = 94, y = 12, rule = B3/S23' "$test_tmp/completion.rle"
  [[ $(sha256sum "$test_tmp/completion.rle" | awk '{print $1}') == 8571b78b68c4f8db6d845fe68dd9ead52c589fa0c2c19d269d07a10543fcee40 ]]
  test -s "$test_tmp/completion-checkpoint_48"
  "$binary" llsss --load "$test_tmp/completion-checkpoint_48" --partials none --save none \
    >"$test_tmp/completion-reload.out" 2>"$test_tmp/completion-reload.err"
  grep -Fq 'checkpoint row already contains a halting completion' "$test_tmp/completion-reload.out"

  # The indexed implicit walk carries only the three-state completion class;
  # it must reproduce the serial exact-summary completion byte-for-byte.
  timeout "${RLIFE_TEST_TIMEOUT:-180}" "$binary" llsss \
    --rule B3/S23 --left-edge bg --filters bcaf --threads 4 \
    --partials final --partial-output "$test_tmp/completion-indexed.rle" \
    --save none c4d-f2b '@bg(6)' \
    >"$test_tmp/completion-indexed.out" 2>"$test_tmp/completion-indexed.err"
  grep -Fq 'completion at flattened depth 48 (w_pos 24[0])' "$test_tmp/completion-indexed.out"
  cmp "$test_tmp/completion.rle" "$test_tmp/completion-indexed.rle"

  # Capturing a scheduled partial inside the indexed left sweep must preserve
  # the serial DFS choice byte-for-byte.
  timeout "${RLIFE_TEST_TIMEOUT:-180}" "$binary" llsss \
    --rule B35678/S4678 --left-edge bg --filters bcaf --threads 1 \
    --partials every:1 --partial-output "$test_tmp/partials-serial.rle" \
    --save none c3d-f2b '@bg(7)' \
    >"$test_tmp/partials-serial.out" 2>"$test_tmp/partials-serial.err"
  timeout "${RLIFE_TEST_TIMEOUT:-180}" "$binary" llsss \
    --rule B35678/S4678 --left-edge bg --filters bcaf --threads 4 \
    --partials every:1 --partial-output "$test_tmp/partials-indexed.rle" \
    --save none c3d-f2b '@bg(7)' \
    >"$test_tmp/partials-indexed.out" 2>"$test_tmp/partials-indexed.err"
  [[ $(grep -c '^#C llsss partial ' "$test_tmp/partials-indexed.rle") == 3 ]]
  cmp "$test_tmp/partials-serial.rle" "$test_tmp/partials-indexed.rle"

  run_tiny_edge gse 2c4-f2b 8 "$test_tmp/orth-gse.out" "$test_tmp/orth-gse.err"
  [[ $(normalize_hash "$test_tmp/orth-gse.err") == 5574b6a953ab5ddf891961447b8b8f5a198d045561a28d636f52709dfe1bacaf ]]
  run_tiny_edge gso 2c4-f2b 8 "$test_tmp/orth-gso.out" "$test_tmp/orth-gso.err"
  [[ $(normalize_hash "$test_tmp/orth-gso.err") == ccf8a4bfd40d1e50f4d88201d47ad60dca64264c4a9a73eabb51e7cf9400d4be ]]
  run_tiny_edge odd c3d-f2b 8 "$test_tmp/diagonal-odd.out" "$test_tmp/diagonal-odd.err"
  [[ $(normalize_hash "$test_tmp/diagonal-odd.err") == b0e1f800f8131e14205af4f90b65680d9c72e78543ae5ec0632ff5da8e305e2c ]]
  run_tiny_edge gso 2c4d-f2b 6 "$test_tmp/diagonal-gso.out" "$test_tmp/diagonal-gso.err"
  [[ $(normalize_hash "$test_tmp/diagonal-gso.err") == 0161efcfd3ca9130bab60f32e5cbee80768f616d52787adc5761ffa385a4e944 ]]
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

if "$binary" llsss --left-edge even --partials none --ends none --save none c5d-f2b '@bg(5)' >"$test_tmp/edge.out" 2>"$test_tmp/edge.err"; then
  echo 'diagonal even edge unexpectedly succeeded' >&2
  exit 1
fi
grep -Fq 'diagonal even symmetry is incompatible with the lattice' "$test_tmp/edge.err"

if "$binary" llsss --left-edge gse --partials none --ends none --save none c5d-f2b '@bg(3)' >"$test_tmp/gse-odd.out" 2>"$test_tmp/gse-odd.err"; then
  echo 'odd-period diagonal glide-even edge unexpectedly succeeded' >&2
  exit 1
fi
grep -Fq 'diagonal glide-even symmetry requires an even period and odd displacement' "$test_tmp/gse-odd.err"

if "$binary" llsss --left-edge gso --partials none --ends none --save none c4-f2b '@bg(5)' >"$test_tmp/orth-glide.out" 2>"$test_tmp/orth-glide.err"; then
  echo 'incompatible primitive orthogonal glide unexpectedly succeeded' >&2
  exit 1
fi
grep -Fq 'orthogonal glide symmetry requires an even period and even displacement' "$test_tmp/orth-glide.err"

if "$binary" llsss --partials none --ends none --save none 2147483647c2147483647d-f2b '@bg(3)' >"$test_tmp/huge.out" 2>"$test_tmp/huge.err"; then
  echo 'oversized diagonal geometry unexpectedly succeeded' >&2
  exit 1
fi
grep -Fq 'geometry has too many lattice subtiles' "$test_tmp/huge.err"

# Checkpoint/resume must reproduce the uninterrupted row-boundary state.  The
# saved halt remains active unless explicitly replaced with a later one.
"$binary" llsss --rule B35678/S4678 --left-edge bg --filters bcaf \
  --partials none --ends none --halts w_pos:12 \
  --savefile "$test_tmp/checkpoint" c5-f2b '@bg(8)' \
  >"$test_tmp/checkpoint.out" 2>"$test_tmp/checkpoint.err"
test -s "$test_tmp/checkpoint_12"
grep -Fq "checkpoint saved: $test_tmp/checkpoint_12" "$test_tmp/checkpoint.out"

"$binary" llsss --load "$test_tmp/checkpoint_12" --partials none --save none \
  >"$test_tmp/reload.out" 2>"$test_tmp/reload.err"
grep -Fq 'checkpoint loaded:' "$test_tmp/reload.out"
if grep -Eq '^Row (1[3-9]|[2-9][0-9])' "$test_tmp/reload.err"; then
  echo 'checkpoint reload ignored its saved halt' >&2
  exit 1
fi

"$binary" llsss --load "$test_tmp/checkpoint_12" --partials none --save none --halts w_pos:16 \
  >"$test_tmp/resume.out" 2>"$test_tmp/resume.err"
"$binary" llsss --rule B35678/S4678 --left-edge bg --filters bcaf \
  --partials none --ends none --halts w_pos:16 --save none c5-f2b '@bg(8)' \
  >"$test_tmp/uninterrupted.out" 2>"$test_tmp/uninterrupted.err"
sed -n 's/^.*cols: //p' "$test_tmp/resume.err" >"$test_tmp/resume.cols"
sed -n 's/^.*cols: //p' "$test_tmp/uninterrupted.err" | tail -n 5 >"$test_tmp/uninterrupted.cols"
cmp "$test_tmp/resume.cols" "$test_tmp/uninterrupted.cols"

if "$binary" llsss --load "$test_tmp/checkpoint_12" --rule B3/S23 --save none \
  >"$test_tmp/checkpoint-mismatch.out" 2>"$test_tmp/checkpoint-mismatch.err"; then
  echo 'checkpoint search-tree option mismatch unexpectedly succeeded' >&2
  exit 1
fi
grep -Fq 'cannot alter checkpoint search-tree option rule' "$test_tmp/checkpoint-mismatch.err"

mkdir "$test_tmp/checkpoint-folder"
printf 'old checkpoint\n' >"$test_tmp/checkpoint-folder/save_12"
"$binary" llsss --rule B35678/S4678 --left-edge bg --filters bcaf \
  --partials none --ends none --halts w_pos:13 --save every:2 \
  --savefile "$test_tmp/checkpoint-folder" c5-f2b '@bg(8)' \
  >"$test_tmp/periodic.out" 2>"$test_tmp/periodic.err"
test -s "$test_tmp/checkpoint-folder/save_12"
test -s "$test_tmp/checkpoint-folder/save_13"
if grep -Fq 'old checkpoint' "$test_tmp/checkpoint-folder/save_12"; then
  echo 'periodic checkpoint did not overwrite its existing target' >&2
  exit 1
fi
grep -Fq "checkpoint saved: $test_tmp/checkpoint-folder/save_12" "$test_tmp/periodic.out"
grep -Fq "checkpoint saved: $test_tmp/checkpoint-folder/save_13" "$test_tmp/periodic.out"

echo "$mode diagonal search regression passed"
