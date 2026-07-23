#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
SDK=${ANDROID_SDK_ROOT:-/Users/ivory/Library/Android/sdk}
NDK=${ANDROID_NDK_HOME:-$SDK/ndk/29.0.14206865}
BUILD_TOOLS=${ANDROID_BUILD_TOOLS:-$SDK/build-tools/35.0.0}
OUT="$ROOT/build/lab-app"
STAGE="$OUT/stage"
KEYSTORE="$ROOT/lab-app/debug.keystore"
ANDROID_JAR="$SDK/platforms/android-34/android.jar"

rm -rf "$OUT"
mkdir -p "$OUT/classes" "$OUT/dex" "$STAGE"

javac -source 8 -target 8 -bootclasspath "$ANDROID_JAR" -d "$OUT/classes" \
  "$ROOT/lab-app/src/main/java/dev/r0hook/lab/MainActivity.java"
jar cf "$OUT/classes.jar" -C "$OUT/classes" .
"$BUILD_TOOLS/d8" --lib "$ANDROID_JAR" --output "$OUT/dex" "$OUT/classes.jar"
"$NDK/toolchains/llvm/prebuilt/darwin-x86_64/bin/aarch64-linux-android23-clang" \
  -shared -fPIC -O2 -Wall -Wextra -Werror \
  -o "$OUT/liblabprobe.so" "$ROOT/lab-app/src/main/cpp/labprobe.c" -ldl

"$BUILD_TOOLS/aapt2" link --manifest "$ROOT/lab-app/AndroidManifest.xml" \
  --min-sdk-version 23 --target-sdk-version 34 -I "$ANDROID_JAR" -o "$OUT/unsigned.apk"
unzip -q "$OUT/unsigned.apk" -d "$STAGE"
cp "$OUT/dex/classes.dex" "$STAGE/classes.dex"
mkdir -p "$STAGE/lib/arm64-v8a"
cp "$OUT/liblabprobe.so" "$STAGE/lib/arm64-v8a/liblabprobe.so"
rm "$OUT/unsigned.apk"
(cd "$STAGE" && zip -0 -qr "$OUT/unsigned.apk" .)

if [ ! -f "$KEYSTORE" ]; then
  keytool -genkeypair -keystore "$KEYSTORE" -storepass android -keypass android \
    -alias androiddebugkey -dname "CN=Android Debug,O=Android,C=US" -keyalg RSA -keysize 2048 \
    -validity 10000 >/dev/null 2>&1
fi
"$BUILD_TOOLS/zipalign" -f 4 "$OUT/unsigned.apk" "$OUT/aligned.apk"
"$BUILD_TOOLS/apksigner" sign --ks "$KEYSTORE" --ks-pass pass:android --key-pass pass:android \
  --out "$OUT/r0lab-debug.apk" "$OUT/aligned.apk"
"$BUILD_TOOLS/apksigner" verify --verbose "$OUT/r0lab-debug.apk"
echo "$OUT/r0lab-debug.apk"
