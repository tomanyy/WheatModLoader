#!/usr/bin/env bash
set -u

hasInsecure=0
hasSecure=0
for arg in "$@"; do
  case "${arg,,}" in
    -insecure) hasInsecure=1 ;;
    -secure)   hasSecure=1 ;;
  esac
done

if [ "$hasInsecure" -eq 1 ] && [ "$hasSecure" -eq 0 ]; then
  export WINEDLLOVERRIDES="winmm=n,b${WINEDLLOVERRIDES:+;$WINEDLLOVERRIDES}"
  echo "[wheat] -insecure detected: mod loader enabled." >&2
else
  echo "[wheat] -insecure NOT present: starting the game WITHOUT the mod loader." >&2
fi

echo "[$(date +%T)] override=${WINEDLLOVERRIDES-unset}" >> "$HOME/wheatLaunch.log"
exec "$@"
