#!/bin/bash
# Builds the shader translator as an Android arm64 executable with .NET NativeAOT. Linux only (NativeAOT does not cross-compile across OSes; WSL works).
# Needs: .NET 10 SDK (DOTNET, default ~/.dotnet/dotnet) and the Android NDK r27c (NDK, default ~/aot/android-ndk-r27c).
# GAME=<title ID> (default 01008DB008C2C000): the translator of that game's shader chain generation (dm/gfx/chains/chain_tree.py, the profile's xlate_chain):
#   the live sources, or a tree with the generation's frozen Program.cs / Ryujinx files laid over them (Arceus Native, Paldea Native: the translator before FMZ)
# Output: out/libdmsh_xlate.so - an executable, named like a library so Android installs it executable in the app's native library folder
set -e
export DOTNET_SYSTEM_GLOBALIZATION_INVARIANT=1 DOTNET_CLI_TELEMETRY_OPTOUT=1   # the SDK itself runs without ICU (WSL has none by default)
HERE=$(cd "$(dirname "$0")" && pwd); DM=$(cd "$HERE/../.." && pwd)
DOTNET=${DOTNET:-$HOME/.dotnet/dotnet}; NDK=${NDK:-$HOME/aot/android-ndk-r27c}; API=${API:-31}
TC=$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin
GAME=${GAME:-01008DB008C2C000}
GEN=$(python3 "$DM/gfx/chains/chain_tree.py" "$GAME" --gen)
PROJ="$HERE/dmsh_xlate_aot.csproj"
if [ -n "$GEN" ]; then   # the generation's own sources: a tool tree laid out as under dm/, so the project's relative paths find them
    TREE=${TREE:-$HOME/aot/xlate-tree-$GEN}; rm -rf "$TREE"; python3 "$DM/gfx/chains/chain_tree.py" "$GAME" "$TREE" > /dev/null
    PROJ="$TREE/android/xlate_aot/dmsh_xlate_aot.csproj"; echo "translator: chain generation $GEN ($TREE)" >&2
fi
PUB=${PUB:-$HOME/aot/xlate/publish${GEN:+-$GEN}}
# the NDK's clang wrapper for the target carries the target triple and the sysroot; NativeAOT links with it
"$DOTNET" publish "$PROJ" -c Release -o "$PUB" -nologo -p:CppCompilerAndLinker="$TC/aarch64-linux-android$API-clang" -p:ObjCopyName="$TC/llvm-objcopy"
mkdir -p "$HERE/out"
cp "$PUB/dmsh_xlate" "$HERE/out/libdmsh_xlate.so"
echo "${GEN:-live}" > "$HERE/out/libdmsh_xlate.chain"   # (which generation the translator in out/ is)
ls -l "$HERE/out/libdmsh_xlate.so"
