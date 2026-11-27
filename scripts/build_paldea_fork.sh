#!/bin/bash
# FORK: the Paldea fork (Pokémon Violet 4.0.0 on the same engine and fork changes as the Galar fork): its own app next to Paldea Native and the Galar Fork,
# its own data folder in shared storage, setup from the player's dump + keys on the phone. Everything else is scripts/build_fork.sh / build_apk_pc.sh.
#
#   bash scripts/build_paldea_fork.sh        → ~/psa-build/PaldeaFork.apk
#
# PALDEA_FORK_VERSION: this app's own versionCode (raise it for every build; the app re-unpacks its scripts only on a new version)
set -euo pipefail
PALDEA_FORK_VERSION=${PALDEA_FORK_VERSION:-1}
export GAME=${GAME:-01008F6008C5E000}   # Pokémon Violet (Scarlet's code stays in place, untested)
export FORK_PACKAGE=${FORK_PACKAGE:-com.nxnative.paldeafork}
export FORK_NAME=${FORK_NAME:-Paldea Fork}
export FORK_DATA_DIR=${FORK_DATA_DIR:-PaldeaFork}
export FORK_ICON=${FORK_ICON:-8a3fd1 1a1030}
export FORK_ONLY_GAME=${FORK_ONLY_GAME-01008F6008C5E000}
export FORK_VERSION=$PALDEA_FORK_VERSION
source "$(dirname "$0")/build_fork.sh"
