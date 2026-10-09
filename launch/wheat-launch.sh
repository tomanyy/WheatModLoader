#!/usr/bin/env bash
# Steam launch options (CS2 -> Properties -> Launch Options):
#   /full/path/to/wheat-launch.sh %command% -insecure
#
# Layer 1 of the gate. The loader DLL override is ONLY set when -insecure is present
# (and -secure is not). On any other launch the game starts untouched, so Wine never
# loads the loader DLL into a VAC-protected process.
set -u

has_insecure=0
has_secure=0
for arg in "$@"; do
  case "${arg,,}" in
    -insecure) has_insecure=1 ;;
    -secure)   has_secure=1 ;;
  esac
done

if [ "$has_insecure" -eq 1 ] && [ "$has_secure" -eq 0 ]; then
  export WINEDLLOVERRIDES="version=n,b${WINEDLLOVERRIDES:+;$WINEDLLOVERRIDES}"
  echo "[wheat] -insecure detected: mod loader enabled." >&2
else
  echo "[wheat] -insecure NOT present: starting the game WITHOUT the mod loader." >&2
fi

exec "$@"
