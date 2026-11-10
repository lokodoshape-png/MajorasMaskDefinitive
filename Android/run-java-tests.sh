#!/usr/bin/env bash
# Runs the unit tests of the app's plain-Java classes (install, migration, zip limits, startup gate) on the host's
# JDK, without the Android SDK: the CI's "Java unit tests" job and a quick local check. The same tests also run
# with ./gradlew testDebugUnitTest.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
libs="${JUNIT_LIBS:-$here/build/junit-libs}"
mkdir -p "$libs"
fetch() {
    local file="$libs/$(basename "$1")" sum="$2"
    if [ ! -f "$file" ]; then
        curl -sSfL --retry 4 -o "$file.part" "https://repo1.maven.org/maven2/$1"
        mv "$file.part" "$file"
    fi
    echo "$sum  $file" | sha256sum -c --quiet -
}
fetch junit/junit/4.13.2/junit-4.13.2.jar 8e495b634469d64fb8acfa3495a065cbacc8a0fff55ce1e31007be4c16dc57d3
fetch org/hamcrest/hamcrest-core/1.3/hamcrest-core-1.3.jar 66fdef91e9739348df7a096aa384a5685f4e875584cce89386a7a47251c4d8e9
cp="$libs/junit-4.13.2.jar:$libs/hamcrest-core-1.3.jar"
out="$(mktemp -d)"
trap 'rm -rf "$out"' EXIT
main="$here/app/src/main/java/com/dishii/mm"
sources=()
for name in FileOps PortInstall SafeZip StagedFiles DataMigration SetupGate; do
    [ -f "$main/$name.java" ] && sources+=("$main/$name.java")
done
javac -encoding UTF-8 -source 8 -target 8 -Xlint:-options -cp "$cp" -d "$out" "${sources[@]}" \
    $(find "$here/app/src/test/java" -name '*.java')
tests=$(cd "$out" && find . -name '*Test.class' | sed 's|^\./||; s|\.class$||; s|/|.|g' | sort)
java -cp "$out:$cp" org.junit.runner.JUnitCore $tests
