#!/usr/bin/env bash
# console_check.sh -- `spinel console` sessions against their transcripts.
#
#   tools/console_check.sh [SPINEL]
#
# Each test/console/NAME.in is typed into `spinel console` (with NAME.rb as
# the program when there is one); stdout and stderr together must match
# NAME.expected. NAME.args holds console flags. SPINEL_CONSOLE_UPDATE=1 rewrites the transcripts instead.
set -u
spinel=${1:-bin/spinel}
dir=test/console
tmp=$(mktemp -d "${TMPDIR:-/tmp}/spinel-console-check.XXXXXX")
trap 'rm -rf "$tmp"' 0
ok=1
for in in "$dir"/*.in; do
  name=$(basename "$in" .in)
  prog=()
  [ -f "$dir/$name.rb" ] && prog=("$dir/$name.rb")
  [ -f "$dir/$name.args" ] && read -r -a args <"$dir/$name.args" && prog+=("${args[@]}")
  "$spinel" console "${prog[@]}" <"$in" >"$tmp/$name.out" 2>&1
  if [ -n "${SPINEL_CONSOLE_UPDATE:-}" ]; then
    cp "$tmp/$name.out" "$dir/$name.expected"
  elif ! cmp -s "$tmp/$name.out" "$dir/$name.expected"; then
    echo "console-test: FAIL ($name)"
    diff "$dir/$name.expected" "$tmp/$name.out" | sed -n 1,20p
    ok=0
  fi
done
if [ $ok -eq 1 ]; then echo "console-test: pass"; else exit 1; fi
