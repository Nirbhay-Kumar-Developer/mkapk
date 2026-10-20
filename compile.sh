#!/data/data/com.termux/files/usr/bin/bash
set -eo pipefail

# --- Argument Parsing (Release / Debug) ---
BUILD_MODE="debug"
if [[ "$1" == "release" ]]; then
    BUILD_MODE="release"
elif [[ "$1" == "debug" ]]; then
    BUILD_MODE="debug"
elif [[ -n "$1" ]]; then
    echo ">> Unknown argument: '$1'. Defaulting to debug."
fi
echo ">> Build Mode: $BUILD_MODE"

# --- Paths ---
PKG_NAME="mkapk-aarch64"
STORAGE_PATH="$(pwd)"
LOCAL_PATH="$HOME/mkapk_tmp_build"
CACHE_DIR="$LOCAL_PATH/.cache"
BUILD_DIR="$LOCAL_PATH/build/$BUILD_MODE"

PREFIX="${PREFIX:-/data/data/com.termux/files/usr}"
SDK_LIB="${SDK_ROOT:-$HOME/android-sdk/Sdk}/cmdline-tools/latest/lib"
JAVA_SHARE="$PREFIX/share/java"
KOTLIN_LIB="$PREFIX/opt/kotlin/lib"
MANIFEST_FILE="config/MANIFEST.MF"
POM_FILE="config/pom.xml"

for tool in xxhsum rsync javac jar make mvn python3 readelf dpkg-deb; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo ">> Error: '$tool' is required. Install missing dependencies." >&2
        exit 1
    fi
done

if [[ ! -f "$MANIFEST_FILE" || ! -f "$POM_FILE" ]]; then
    echo ">> Error: Required files '$MANIFEST_FILE' or '$POM_FILE' missing." >&2
    exit 1
fi

# --- Trap: Persist Build State Back to Storage on Exit ---
trap '
    mkdir -p "$STORAGE_PATH/build" "$STORAGE_PATH/.cache" "$STORAGE_PATH/config/target"
    rsync -a "$LOCAL_PATH/build/" "$STORAGE_PATH/build/" 2>/dev/null || true
    rsync -a "$LOCAL_PATH/.cache/" "$STORAGE_PATH/.cache/" 2>/dev/null || true
    rsync -a "$LOCAL_PATH/config/target/" "$STORAGE_PATH/config/target/" 2>/dev/null || true
    rsync -a "$LOCAL_PATH/$PKG_NAME/data/data/com.termux/files/usr/share/doc/mkapk/" \
             "$STORAGE_PATH/$PKG_NAME/data/data/com.termux/files/usr/share/doc/mkapk/" 2>/dev/null || true
' EXIT

hash_dir() {
    find "$1" -type f -exec xxhsum {} + 2>/dev/null | sort | xxhsum | cut -d' ' -f1
}

hash_files() {
    xxhsum "$@" 2>/dev/null | xxhsum | cut -d' ' -f1
}

# --- 0. Fast Incremental Sync to Persistent Local Storage ---
echo ">> Syncing changed sources from storage..."
mkdir -p "$LOCAL_PATH"

rsync -a --update --delete \
    --exclude='build/' \
    --exclude='.cache/' \
    --exclude='config/target/' \
    --exclude='*.deb' \
    "$STORAGE_PATH/" "$LOCAL_PATH/"

if [[ ! -d "$LOCAL_PATH/build" && -d "$STORAGE_PATH/build" ]]; then
    rsync -a "$STORAGE_PATH/build/" "$LOCAL_PATH/build/"
fi
if [[ ! -d "$LOCAL_PATH/.cache" && -d "$STORAGE_PATH/.cache" ]]; then
    rsync -a "$STORAGE_PATH/.cache/" "$LOCAL_PATH/.cache/"
fi
if [[ ! -d "$LOCAL_PATH/config/target" && -d "$STORAGE_PATH/config/target" ]]; then
    mkdir -p "$LOCAL_PATH/config/target"
    rsync -a "$STORAGE_PATH/config/target/" "$LOCAL_PATH/config/target/"
fi

cd "$LOCAL_PATH"
mkdir -p "$CACHE_DIR" "$BUILD_DIR"
echo "$BUILD_MODE" > "$CACHE_DIR/build_mode"

# --- 1. Environment & Local SDK Classpath Discovery (Not Redistributed) ---
export JAVA_HOME="${JAVA_HOME:-"$PREFIX/lib/jvm/java-21-openjdk"}"
export PATH="$JAVA_HOME/bin:$PATH"

declare -A LOCAL_SDK_MAP=(
    ["java_share"]="$JAVA_SHARE/*.jar"
    ["kotlin_libs"]="$KOTLIN_LIB/*.jar"
    ["r8"]="$SDK_LIB/**/r8*.jar"
    ["manifest_merger"]="$SDK_LIB/**/*manifest-merger*.jar"
    ["common"]="$SDK_LIB/**/*common*.jar"
    ["sdklib"]="$SDK_LIB/**/*sdklib*.jar"
)

assemble_maven_extra_classpath() {
    local -a valid_entries=()
    local -A seen_paths=()
    local resolved_path

    shopt -s globstar nullglob
    for key in "${!LOCAL_SDK_MAP[@]}"; do
        local target="${LOCAL_SDK_MAP[$key]}"
        for resolved_path in $target; do
            if [[ -f "$resolved_path" && "$resolved_path" == *.jar ]]; then
                if [[ -z "${seen_paths[$resolved_path]:-}" ]]; then
                    seen_paths["$resolved_path"]=1
                    valid_entries+=("$resolved_path")
                fi
            fi
        done
    done
    shopt -u globstar nullglob

    # colon-delimited
    local IFS=":"
    echo "${valid_entries[*]}"
}

EXTRA_SDK_CLASSPATH="$(assemble_maven_extra_classpath)"

# --- 2. Compile C++ Native Engine ---
echo ">> Running Makefile (Incremental) in $BUILD_MODE mode..."
make -j"$(nproc)" BUILD_MODE="$BUILD_MODE"

# --- 3. Delegate Java Coordinator & License Extraction to Maven ---
JAVA_SRC_DIR="./java"
MAVEN_TARGET="$LOCAL_PATH/config/target"
JAR_NAME="mkapk-coordinator.jar"
JAR_PATH="$MAVEN_TARGET/$JAR_NAME"
DEPS_DIR="$MAVEN_TARGET/dependencies"

echo ">> Checking Java Coordinator & Maven Dependencies..."
CURRENT_JAVA_HASH="$(hash_dir "$JAVA_SRC_DIR")$(hash_files "$MANIFEST_FILE" "$POM_FILE")$(echo -n "$EXTRA_SDK_CLASSPATH" | xxhsum | cut -d' ' -f1)"
OLD_JAVA_HASH="$(cat "$CACHE_DIR/java_${BUILD_MODE}.hash" 2>/dev/null || true)"

if [[ "$CURRENT_JAVA_HASH" != "$OLD_JAVA_HASH" || ! -f "$JAR_PATH" || -z "$(ls -A "$DEPS_DIR"/*.jar 2>/dev/null)" ]]; then
    echo "   [MAVEN] Compiling Java sources, copying runtime deps, and extracting licenses..."
    rm -rf "$DEPS_DIR"
    mvn -f "$POM_FILE" package \
        -Dextra.classpath="$EXTRA_SDK_CLASSPATH" \
        -Dsilent=true

    cp "$JAR_PATH" "$BUILD_DIR/$JAR_NAME"
    echo "$CURRENT_JAVA_HASH" > "$CACHE_DIR/java_${BUILD_MODE}.hash"
    echo ">> Updated $JAR_PATH"
else
    cp "$JAR_PATH" "$BUILD_DIR/$JAR_NAME" 2>/dev/null || true
    echo "   [MAVEN] Up to date ($BUILD_MODE). Skipping Maven build."
fi

# --- 4. Auto-Generate Debian Copyright (C++ ELF + Third-Party + Maven) ---
python3 "$LOCAL_PATH/scripts/generate_copyright.py" "$BUILD_MODE"

# --- 5. Package Assembly & Deployment ---
DEB_ROOT="$LOCAL_PATH/$PKG_NAME"
PREFIX_PATH="$DEB_ROOT/data/data/com.termux/files/usr"
BIN_DEST="$PREFIX_PATH/bin"
SHARE_DEST="$PREFIX_PATH/share/mkapk"
LIB_DEST="$SHARE_DEST/lib"
COPYRIGHT_PATH="$PREFIX_PATH/share/doc/mkapk/copyright"

# --- Comprehensive Debian Input Hash ---
# 1. Native C++ binary & build mode
DEB_INPUT_COMPONENTS=(
    "$BUILD_DIR/mkapk"
    "$CACHE_DIR/build_mode"
)

# 2. Packaging control scripts & metadata (DEBIAN/control, postinst, etc.)
DEB_CONTROL_HASH=""
if [[ -d "$DEB_ROOT/DEBIAN" ]]; then
    DEB_CONTROL_HASH="$(hash_dir "$DEB_ROOT/DEBIAN")"
fi

# 3. Copyright file & its generator script (ensures changes in licensing script trigger rebuild)
COPYRIGHT_INPUT_HASH="$(hash_files "$COPYRIGHT_PATH" "$LOCAL_PATH/scripts/generate_copyright.py")"

# 4. Java Coordinator JAR & Maven runtime dependencies
JAVA_INPUT_HASH="$(hash_files "$JAR_PATH")$(hash_dir "$DEPS_DIR")"

# 5. Local vendored JARs (e.g. AndResGuard in libs/), if present
VENDORED_JAR_HASH=""
if [[ -d "$LOCAL_PATH/libs" ]]; then
    VENDORED_JAR_HASH="$(hash_dir "$LOCAL_PATH/libs")"
fi

# Combine all components into a single deterministic fingerprint
DEB_INPUT_HASH="$(hash_files "${DEB_INPUT_COMPONENTS[@]}")${DEB_CONTROL_HASH}${COPYRIGHT_INPUT_HASH}${JAVA_INPUT_HASH}${VENDORED_JAR_HASH}"
OLD_DEB_HASH="$(cat "$CACHE_DIR/deb.hash" 2>/dev/null || true)"

if [[ "$DEB_INPUT_HASH" != "$OLD_DEB_HASH" || ! -f "${PKG_NAME}.deb" ]]; then
    echo ">> Assembling Debian Package ($BUILD_MODE)..."
    mkdir -p "$BIN_DEST" "$SHARE_DEST" "$LIB_DEST"

    cp "$BUILD_DIR/mkapk" "$BIN_DEST/"
    if [[ "$BUILD_MODE" == "release" ]]; then
        echo ">> Stripping binary for release..."
        strip "$BIN_DEST/mkapk" 2>/dev/null || true
    fi

    cp "$JAR_PATH" "$SHARE_DEST/"

    # Clean old jars before copying fresh Maven runtime dependencies
    rm -f "$LIB_DEST"/*.jar
    cp "$DEPS_DIR"/*.jar "$LIB_DEST/"
    
    # Copy vendored jars
    if compgen -G "$LOCAL_PATH/libs/*.jar" > /dev/null; then
        cp "$LOCAL_PATH/libs"/*.jar "$LIB_DEST/"
    fi

    echo ">> Setting Permissions..."
    find "$DEB_ROOT" -type d -exec chmod 755 {} +
    if [[ -d "$DEB_ROOT/DEBIAN" ]]; then
        find "$DEB_ROOT/DEBIAN" -type f -exec chmod 644 {} +
        [[ -f "$DEB_ROOT/DEBIAN/postinst" ]] && chmod 755 "$DEB_ROOT/DEBIAN/postinst"
    fi
    chmod +x "$BIN_DEST/mkapk"

    echo ">> Building .deb..."
    dpkg-deb --build "$PKG_NAME"

    cp "${PKG_NAME}.deb" "$STORAGE_PATH/"
    dpkg --install "${PKG_NAME}.deb"

    echo "$DEB_INPUT_HASH" > "$CACHE_DIR/deb.hash"
    echo "✨ Package updated and installed."
else
    echo "✨ Everything is up to date! Skipping .deb rebuild and installation."
fi
