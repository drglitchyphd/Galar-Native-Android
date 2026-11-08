#!/bin/bash
# FORK: builds the fork's APK. It installs next to the main Galar Native app (its own package, name and icon) and keeps its game files, saves and
# settings in a folder on the phone's shared storage instead of Android/data. Everything else is scripts/build_apk_pc.sh (same needs, same ~/psa-build).
#
#   bash scripts/build_fork.sh                         → ~/psa-build/GalarFork.apk
#   FORK_DATA_DIR=GalarFork-test bash scripts/build_fork.sh   another data folder (a second test install, say)
#
# Settings (environment variables):
#   FORK_PACKAGE   Android package name             default com.nxnative.galarfork  (keep it the same between builds, or Android treats it as a new app)
#   FORK_NAME      app name under the icon          default "Galar Fork"
#   FORK_DATA_DIR  folder in /storage/emulated/0    default GalarFork  (setup writes the ~15 GB game here; it survives uninstalling)
#   FORK_ICON      icon colours "top ground" (hex)  default "2a6fdb 182040"
#   FORK_VERSION   versionCode                      default 13 (raise it for each build you install over an older one)
set -euo pipefail
FORK_PACKAGE=${FORK_PACKAGE:-com.nxnative.galarfork}
FORK_NAME=${FORK_NAME:-Galar Fork}
FORK_DATA_DIR=${FORK_DATA_DIR:-GalarFork}
FORK_ICON=${FORK_ICON:-2a6fdb 182040}
FORK_VERSION=${FORK_VERSION:-13}
APK_ARGS=(--package "$FORK_PACKAGE" --name "$FORK_NAME" --data-dir "$FORK_DATA_DIR" --icon "$FORK_ICON" --version "$FORK_VERSION")
APK_OUT="${WORK:-$HOME/psa-build}/$(echo "$FORK_NAME" | tr -d ' ').apk"
echo "[FORK] package $FORK_PACKAGE, name \"$FORK_NAME\", data folder /storage/emulated/0/$FORK_DATA_DIR"
source "$(dirname "$0")/build_apk_pc.sh"
