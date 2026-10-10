#!/usr/bin/env bash
# Runs the real version.dll proxy inside Wine against a fake cs2.exe and checks the gate.
# Usage: tests/wine/run_wine_tests.sh <build-dir-with-version.dll-and-fake_cs2.exe>
set -u
BUILD="$(cd "${1:?usage: $0 <build-dir>}" && pwd)"
WINE="${WINE:-$(command -v wine64 || command -v wine || echo /usr/lib/wine/wine64)}"
export WINEPREFIX="${WINEPREFIX:-/tmp/wheat-wineprefix}"
export WINEDEBUG="+seh,+loaddll"   # +seh shows OutputDebugString text, +loaddll proves what got loaded
export WINEDLLOVERRIDES="version=n,b;mscoree,mshtml="   # the same override wheat-launch.sh sets
WORK="$(mktemp -d /tmp/wheat-wine.XXXXXX)"
pass=0; fail=0

ok()   { echo "  PASS  $1"; pass=$((pass+1)); }
bad()  { echo "  FAIL  $1"; fail=$((fail+1)); [ -n "${2:-}" ] && echo "$2" | sed 's/^/        | /' | head -15; }
expect() { # name, condition-exit-status, [detail]
  if [ "$2" -eq 0 ]; then ok "$1"; else bad "$1" "${3:-}"; fi
}

new_game() { # $1 = exe name -> prints dir
  local d="$WORK/$RANDOM$RANDOM"; mkdir -p "$d"
  cp "$BUILD/fake_cs2.exe" "$d/$1"; cp "$BUILD/version.dll" "$d/version.dll"
  echo "$d"
}
run_game() { # dir exe args...  (stdout+stderr captured to $dir/out.txt)
  local d="$1" exe="$2"; shift 2
  ( cd "$d" && timeout 90 "$WINE" "$exe" "$@" >"$d/out.txt" 2>&1 )
  echo $? >"$d/exit.txt"
}
add_mods() {
  local m="$1/wheat/mods"; mkdir -p "$m"/{hello,broken,runaway,spam,escape}
  cat >"$m/hello/main.lua" <<'LUA'
print('hello up, api ' .. wheat.api_version)
local n = 0
events.on('tick', function(dt)
  n = n + 1
  if n == 3 then print('ticks=' .. n .. ' dt_ok=' .. tostring(dt > 0)) end
end)
LUA
  echo "this is not lua !!!" >"$m/broken/main.lua"
  echo "events.on('tick', function() while true do end end)" >"$m/runaway/main.lua"
  echo "for i = 1, 5000 do print('spam ' .. i) end" >"$m/spam/main.lua"
  cat >"$m/escape/main.lua" <<'LUA'
assert(io == nil and os == nil and debug == nil and package == nil and dofile == nil and loadfile == nil)
assert(string.dump == nil and getmetatable('') == 'locked')
assert(not pcall(require, '../../../version'))
print('escape-check ok')
LUA
}
has_debug() { grep -q "$1" "$2/out.txt"; }
# Wine answers VerLanguageNameW with "English" (Windows says "English (United States)"); any non-empty answer proves forwarding.
forwarded() { grep -qE "VerLanguageNameW -> [1-9][0-9]* 'English" "$1/out.txt"; }
# Guards against vacuous passes: our native proxy was loaded from the game folder AND it loaded the real builtin version.dll.
proxy_loaded() { grep -qiE 'Loaded L"[^"]*[\\/]*VERSION\.dll" at [0-9A-F]+: native' "$1/out.txt" && grep -qiE 'system32.*version\.dll" at [0-9A-F]+: builtin' "$1/out.txt"; }

echo "== priming wine prefix (first run is slow) =="
"$WINE" wineboot -u >/dev/null 2>&1 || true

echo "== scenario 1: cs2.exe -insecure  (loader must start) =="
g=$(new_game cs2.exe); add_mods "$g"; run_game "$g" cs2.exe -insecure -novid
L="$g/wheat/loader.log"
expect "forwarding works: VerLanguageNameW returns the real answer" $(forwarded "$g"; echo $?) "$(grep -a VerLanguage "$g/out.txt" | head -3)"
expect "proxy DLL loaded from game folder; real builtin version.dll loaded by path" $(proxy_loaded "$g"; echo $?) "$(grep -a loaddll "$g/out.txt" | head -4)"
expect "process exited normally (DLL load did not break the host)" $(grep -qx 0 "$g/exit.txt"; echo $?) "$(cat "$g/exit.txt")"
expect "loader started and logged" $(grep -q "wheat loader started" "$L" 2>/dev/null; echo $?) "$(ls -R "$g" | head; head -5 "$g/out.txt")"
expect "good mod loaded and its output was logged" $(grep -q "\[hello\] INFO  hello up, api 1" "$L"; echo $?)
expect "tick events are delivered to mods" $(grep -q "ticks=3 dt_ok=true" "$L"; echo $?) "$(tail -5 "$L")"
expect "mod with a syntax error was dropped, others unaffected" $(grep -q "mod 'broken' failed to load" "$L"; echo $?)
expect "runaway mod was struck and disabled" $(grep -q "mod disabled" "$L"; echo $?) "$(grep -i "runaway" "$L" | head -3)"
expect "sandbox escape attempts fail inside the real DLL" $(grep -q "escape-check ok" "$L"; echo $?) "$(grep escape "$L" | head -3)"
spam_lines=$(grep -c "\[spam\] INFO  spam " "$L"); 
expect "log flood is rate limited (got $spam_lines of 5000 lines)" $([ "$spam_lines" -le 60 ] && [ "$spam_lines" -ge 1 ]; echo $?)
expect "dropped-lines notice was written" $(grep -q "log lines dropped" "$L"; echo $?)

echo "== scenario 2: cs2.exe, NO -insecure (loader must stay dormant) =="
g=$(new_game cs2.exe); add_mods "$g"; run_game "$g" cs2.exe -novid
expect "DLL was loaded and forwards (not vacuous)" $(proxy_loaded "$g" && forwarded "$g"; echo $?)
expect "gate refused: -insecure missing" $(has_debug "refused: -insecure not on the command line" "$g"; echo $?)
expect "no wheat/ directory or log was created" $([ ! -e "$g/wheat/loader.log" ] && [ ! -e "$g/wheat/mods/../loader.log" ]; echo $?)

echo "== scenario 3: -insecure AND -secure =="
g=$(new_game cs2.exe); add_mods "$g"; run_game "$g" cs2.exe -insecure -secure
expect "DLL was loaded (not vacuous)" $(proxy_loaded "$g"; echo $?)
expect "gate refused: -secure present" $(has_debug "refused: -secure present" "$g"; echo $?)
expect "no log created" $([ ! -e "$g/wheat/loader.log" ]; echo $?)

echo "== scenario 4: -insecure but the process is not cs2.exe =="
g=$(new_game notcs2.exe); add_mods "$g"; run_game "$g" notcs2.exe -insecure
expect "DLL was loaded (not vacuous)" $(proxy_loaded "$g"; echo $?)
expect "gate refused: wrong host process" $(has_debug "refused: host process is not cs2.exe" "$g"; echo $?)
expect "no log created" $([ ! -e "$g/wheat/loader.log" ]; echo $?)

echo "== scenario 5: quoted \"-insecure\" (spoof attempt) =="
g=$(new_game cs2.exe); add_mods "$g"; run_game "$g" cs2.exe -x '"-insecure"'
expect "DLL was loaded (not vacuous)" $(proxy_loaded "$g"; echo $?)
expect "gate refused: quoted flag does not count" $(has_debug "refused: -insecure not on the command line" "$g"; echo $?)
expect "no log created" $([ ! -e "$g/wheat/loader.log" ]; echo $?)

rm -rf "$WORK"
echo; echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
