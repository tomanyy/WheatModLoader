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
  decision="ENABLED"
else
  decision="DISABLED (insecure=$hasInsecure secure=$hasSecure)"
fi

echo "[wheat] loader $decision" >&2
echo "[$(date +%T)] $decision override=${WINEDLLOVERRIDES-unset}" >> "$HOME/wheatLaunch.log"
exec "$@"
