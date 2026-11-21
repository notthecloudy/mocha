#!/usr/bin/env bash
# Patch the Unity test APK with Mocha, install it on a device, push the example
# mods, launch the app, and collect the real on-device artifacts.
#
# Required:
#   testapp/Build/mocha-testapp.apk   Unity IL2CPP arm64-v8a test app
#   build/android/native/*.so         from scripts/build_android.sh
#
# Usage:
#   scripts/run_emulator_test.sh [--serial SERIAL] [--wait SECONDS] [--no-build]
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$root"

serial="${ANDROID_SERIAL:-emulator-5554}"
package="dev.mocha.testapp"
activity="com.unity3d.player.UnityPlayerGameActivity"
timeout_seconds=90
build=1

while [[ $# -gt 0 ]]; do
    case "$1" in
        --serial) serial="$2"; shift 2 ;;
        --wait) timeout_seconds="$2"; shift 2 ;;
        --no-build) build=0; shift ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done

adb=(adb -s "$serial")

if [[ "$build" == "1" ]]; then
    scripts/build_android.sh
fi

apk="testapp/Build/mocha-testapp.apk"
shim="build/android/native/libmain.so"
mocha="build/android/native/libmocha.so"
patched="build/mocha-testapp-patched.apk"

[[ -f "$apk" ]] || { echo "error: $apk missing" >&2; exit 1; }
[[ -f "$shim" && -f "$mocha" ]] || { echo "error: build android artifacts first" >&2; exit 1; }

python3 tools/mocha_patch.py --apk "$apk" --out "$patched" --shim "$shim" --mocha "$mocha"

"${adb[@]}" get-state >/dev/null
abi_list="$("${adb[@]}" shell getprop ro.product.cpu.abilist | tr -d '\r')"
echo "device ABIs: $abi_list"
if [[ "$abi_list" != *"arm64-v8a"* ]]; then
    echo "error: device does not report arm64-v8a support" >&2
    exit 1
fi

echo "installing $patched"
"${adb[@]}" uninstall "$package" >/dev/null 2>&1 || true
"${adb[@]}" install -r "$patched"

external="/sdcard/Android/data/$package/files"
mods="$external/mocha/mods"
internal="/data/user/0/$package/files"
internal_mods="$internal/mocha/mods"

# The loader prefers external app storage when it can create and write
# <files>/mocha, and otherwise falls back to internal storage. Both roots are
# staged so the mods are found whichever one the loader selects.
device_uid="$("${adb[@]}" shell id -u | tr -d '\r')"
if [[ "$device_uid" != "0" ]]; then
    echo "error: adb shell is not root; cannot prepare app-owned mod directories" >&2
    echo "       run 'adb root' (userdebug builds) and retry" >&2
    exit 1
fi

app_uid="$("${adb[@]}" shell "cmd package list packages -U $package" | sed -n 's/.*uid:\([0-9][0-9]*\).*/\1/p' | head -n1 | tr -d '\r')"
app_gid="$("${adb[@]}" shell "stat -c '%g' /sdcard/Android/data" | tr -d '\r')"
owner="$app_uid:$app_gid"
if [[ ! "$owner" =~ ^[0-9]+:[0-9]+$ ]]; then
    echo "error: cannot determine the app UID (got '$owner')" >&2
    exit 1
fi
app_uid="${owner%%:*}"

"${adb[@]}" shell "rm -rf '$external/mocha' '$internal/mocha'"
"${adb[@]}" shell "mkdir -p '$mods' '$internal_mods'"
"${adb[@]}" shell "chown -R '$owner' '$external'"

for manifest in examples/manifests/*.json; do
    stem="$(basename "$manifest" .json)"
    library="build/android/examples/$stem.so"
    [[ -f "$library" ]] || { echo "error: $library missing" >&2; exit 1; }
    "${adb[@]}" push "$manifest" "$mods/$stem.json" >/dev/null
    "${adb[@]}" push "$library" "$mods/$stem.so" >/dev/null
    "${adb[@]}" push "$manifest" "$internal_mods/$stem.json" >/dev/null
    "${adb[@]}" push "$library" "$internal_mods/$stem.so" >/dev/null
done

"${adb[@]}" shell "chown -R '$owner' '$external/mocha'"
"${adb[@]}" shell "chown -R '$owner' '$internal'"
"${adb[@]}" shell "chmod 0770 '$external/mocha' '$mods' '$internal/mocha' '$internal_mods'"

# FUSE can briefly report stale attributes after a chown; verify and retry.
for _ in 1 2 3; do
    if [[ "$("${adb[@]}" shell stat -c '%u' "$mods" | tr -d '\r')" == "$app_uid" &&
          "$("${adb[@]}" shell stat -c '%u' "$internal_mods" | tr -d '\r')" == "$app_uid" ]]; then
        break
    fi
    "${adb[@]}" shell "chown -R '$owner' '$external/mocha' '$internal'"
    sleep 1
done
for dir in "$mods" "$internal_mods"; do
    actual="$("${adb[@]}" shell stat -c '%u' "$dir" | tr -d '\r')"
    if [[ "$actual" != "$app_uid" ]]; then
        echo "error: $dir is owned by UID $actual, expected $app_uid" >&2
        exit 1
    fi
done

"${adb[@]}" logcat -c
"${adb[@]}" shell am start -W -n "$package/$activity" >/dev/null

echo "waiting up to ${timeout_seconds}s for loader-summary.txt"
summary=""
deadline=$((SECONDS + timeout_seconds))
while (( SECONDS < deadline )); do
    for dir in "$external/mocha" "$internal/mocha"; do
        if "${adb[@]}" shell "test -f '$dir/loader-summary.txt'" 2>/dev/null; then
            summary="$(mktemp)"
            "${adb[@]}" pull "$dir/loader-summary.txt" "$summary" >/dev/null
            if grep -q '^loaded=' "$summary"; then
                break 2
            fi
        fi
    done
    sleep 2
done

stamp="$(date +%Y%m%d-%H%M%S)"
artifacts="artifacts/emulator-$stamp"
mkdir -p "$artifacts"

for file in loader-summary.txt mocha.log hello.result dependent.result failing.result missing-dep.result; do
    for dir in "$external/mocha" "$internal/mocha"; do
        if "${adb[@]}" shell "test -f '$dir/$file'" 2>/dev/null; then
            "${adb[@]}" pull "$dir/$file" "$artifacts/$file" >/dev/null
            break
        fi
    done
done
"${adb[@]}" logcat -d -v threadtime > "$artifacts/logcat.txt"
"${adb[@]}" shell dumpsys package "$package" > "$artifacts/package.txt" 2>/dev/null || true

echo
echo "== loader summary =="
if [[ -f "$artifacts/loader-summary.txt" ]]; then
    cat "$artifacts/loader-summary.txt"
else
    echo "(missing)"
fi

echo
echo "== results =="
for name in hello dependent failing missing-dep; do
    if [[ -f "$artifacts/$name.result" ]]; then
        echo "-- $name.result"; cat "$artifacts/$name.result"; echo
    else
        echo "-- $name.result: absent"
    fi
done

echo
echo "== Mocha log lines =="
grep -E "Mocha|mocha" "$artifacts/logcat.txt" | tail -n 80 || true

echo
echo "artifacts: $artifacts"

if [[ ! -f "$artifacts/loader-summary.txt" ]]; then
    echo "error: loader did not write a summary" >&2
    exit 1
fi
if ! grep -qx 'loaded=2' "$artifacts/loader-summary.txt"; then
    echo "error: expected exactly 2 loaded mods" >&2
    exit 1
fi
if ! grep -qx 'failed=1' "$artifacts/loader-summary.txt"; then
    echo "error: expected exactly 1 failed mod" >&2
    exit 1
fi
if grep -q '^mod.mocha.example.missing-dep=' "$artifacts/loader-summary.txt"; then
    echo "error: missing-dep mod must not be loaded" >&2
    exit 1
fi
if [[ -f "$artifacts/missing-dep.result" ]]; then
    echo "error: missing-dep mod executed" >&2
    exit 1
fi
echo "PASS"
