#!/bin/bash
# DriverCheckTest.java on a desktop JVM: the app's Java sources and the test compiled against android.jar (as build_apk.py does: javac -source 8 -target 8), run with
#   android.jar's stubs on the class path (the tested code calls nothing of Android).
#   Environment: ANDROID_JAR (default: the Android SDK's android-34 platform), JAVAC / JAVA (default: from PATH)
set -u
HERE=$(cd "$(dirname "$0")" && pwd); APP=$HERE/../../app/java/com/nxnative
JAR=${ANDROID_JAR:-${LOCALAPPDATA:-$HOME}/Android/Sdk/platforms/android-34/android.jar}
[ -f "$JAR" ] || { echo "no android.jar at $JAR (ANDROID_JAR=...)"; exit 2; }
OUT=$(mktemp -d); CP="$OUT:$JAR"
case "$(uname -s)" in MINGW*|MSYS*|CYGWIN*) CP="$(cygpath -w "$OUT");$(cygpath -w "$JAR")";; esac   # (a Windows JDK: Windows paths, ';' between them)
"${JAVAC:-javac}" -encoding UTF-8 -source 8 -target 8 -nowarn -cp "$JAR" -d "$OUT" "$APP"/*.java "$HERE/DriverCheckTest.java" || { rm -rf "$OUT"; exit 2; }
"${JAVA:-java}" -cp "$CP" com.nxnative.DriverCheckTest "$@"; rc=$?
rm -rf "$OUT"; exit $rc
