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
DEPS_DIR="$LOCAL_PATH/dependencies"

PREFIX="${PREFIX:-/data/data/com.termux/files/usr}"
SDK_LIB="${SDK_ROOT:-$HOME/android-sdk/Sdk}/cmdline-tools/latest/lib"
JAVA_SHARE="$PREFIX/share/java"
MKAPK_SHARE="$PREFIX/share/mkapk"
KOTLIN_LIB="$PREFIX/opt/kotlin/lib"
MANIFEST_FILE="config/MANIFEST.MF"
POM_FILE="config/pom.xml"

# Ensure required tools are installed
for tool in xxhsum rsync javac jar make mvn; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo ">> Error: '$tool' is required. Install missing dependencies." >&2
        exit 1
    fi
done

# Validate required configuration files
if [[ ! -f "$MANIFEST_FILE" ]]; then
    echo ">> Error: Required manifest '$MANIFEST_FILE' not found." >&2
    exit 1
fi

if [[ ! -f "$POM_FILE" ]]; then
    echo ">> Error: Required dependency POM '$POM_FILE' not found." >&2
    exit 1
fi

# --- Trap: Persist Build State Back to Storage on Exit ---
trap '
    mkdir -p "$STORAGE_PATH/build" "$STORAGE_PATH/.cache" "$STORAGE_PATH/dependencies"
    rsync -a "$LOCAL_PATH/build/" "$STORAGE_PATH/build/" 2>/dev/null || true
    rsync -a "$LOCAL_PATH/.cache/" "$STORAGE_PATH/.cache/" 2>/dev/null || true
    rsync -a "$LOCAL_PATH/dependencies/" "$STORAGE_PATH/dependencies/" 2>/dev/null || true
' EXIT

# --- Helper Hash Functions ---
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
    --exclude='*.deb' \
    "$STORAGE_PATH/" "$LOCAL_PATH/"

if [[ ! -d "$LOCAL_PATH/build" && -d "$STORAGE_PATH/build" ]]; then
    rsync -a "$STORAGE_PATH/build/" "$LOCAL_PATH/build/"
fi
if [[ ! -d "$LOCAL_PATH/.cache" && -d "$STORAGE_PATH/.cache" ]]; then
    rsync -a "$STORAGE_PATH/.cache/" "$LOCAL_PATH/.cache/"
fi
if [[ ! -d "$LOCAL_PATH/dependencies" && -d "$STORAGE_PATH/dependencies" ]]; then
    rsync -a "$STORAGE_PATH/dependencies/" "$LOCAL_PATH/dependencies/"
fi

cd "$LOCAL_PATH"
mkdir -p "$CACHE_DIR" "$BUILD_DIR" "$DEPS_DIR"

echo "$BUILD_MODE" > "$CACHE_DIR/build_mode"

# --- 1. Dependency Resolution & Caching ---
CURRENT_POM_HASH="$(hash_files "$POM_FILE")"
OLD_POM_HASH="$(cat "$CACHE_DIR/pom.hash" 2>/dev/null || true)"

# Re-resolve if pom.xml changed or dependencies folder has no JARs
if [[ "$CURRENT_POM_HASH" != "$OLD_POM_HASH" || -z "$(ls -A "$DEPS_DIR"/*.jar 2>/dev/null)" ]]; then
    echo ">> Resolving and downloading dependencies from $POM_FILE..."
    rm -rf "$DEPS_DIR" "$STORAGE_PATH/dependencies"
    mvn -f "$POM_FILE" dependency:copy-dependencies \
        -DoutputDirectory="$DEPS_DIR" \
        -DincludeScope=runtime \
        -Dsilent=true

    echo "$CURRENT_POM_HASH" > "$CACHE_DIR/pom.hash"
    echo ">> Dependencies cached in $DEPS_DIR"
else
    echo ">> Dependencies up to date (cached in $DEPS_DIR)."
fi

# --- Classpath Configuration Dictionary ---
# Recursively targets ManifestMerger, common (ILogger), sdklib, R8, Kotlin, and dependencies
declare -A CLASSPATH_MAP=(
    ["downloaded_deps"]="$DEPS_DIR/*.jar"
    ["mkapk_share"]="$MKAPK_SHARE/*.jar"
    ["java_share"]="$JAVA_SHARE/*.jar"
    ["kotlin_libs"]="$KOTLIN_LIB/*.jar"
    ["r8"]="$SDK_LIB/**/r8*.jar"
    ["manifest_merger"]="$SDK_LIB/**/*manifest-merger*.jar"
    ["common"]="$SDK_LIB/**/*common*.jar"
    ["sdklib"]="$SDK_LIB/**/*sdklib*.jar"
)

# Dynamically construct the classpath string in memory
assemble_classpath() {
    local -a valid_entries=()
    local -A seen_paths=()
    local resolved_path

    # Enable globstar and nullglob so ** searches recursively and unmatched patterns expand to nothing
    shopt -s globstar nullglob
    for key in "${!CLASSPATH_MAP[@]}"; do
        local target="${CLASSPATH_MAP[$key]}"

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

    local IFS=":"
    echo "${valid_entries[*]}"
}

CLASS_PATH="$(assemble_classpath)"

# Clean up deprecated classpath.txt if present from previous builds
rm -f scripts/classpath.txt

# --- 2. Environment Setup ---
export JAVA_HOME="${JAVA_HOME:-"$PREFIX/lib/jvm/java-21-openjdk"}"
export PATH="$JAVA_HOME/bin:$PATH"

# --- 3. Compile C++ Native Engine ---
echo ">> Running Makefile (Incremental) in $BUILD_MODE mode..."
make -j"$(nproc)" BUILD_MODE="$BUILD_MODE"

# --- 4. Compile Java Coordinator ---
JAVA_SRC_DIR="./java"
JAVA_BIN_DIR="$BUILD_DIR/java_out"
JAR_NAME="mkapk-coordinator.jar"
JAR_PATH="$BUILD_DIR/$JAR_NAME"

echo ">> Checking Java Coordinator..."
CURRENT_JAVA_HASH="$(hash_dir "$JAVA_SRC_DIR")$(hash_files "$MANIFEST_FILE")$(echo -n "$CLASS_PATH" | xxhsum | cut -d' ' -f1)"
OLD_JAVA_HASH="$(cat "$CACHE_DIR/java_${BUILD_MODE}.hash" 2>/dev/null || true)"

if [[ "$CURRENT_JAVA_HASH" != "$OLD_JAVA_HASH" || ! -f "$JAR_PATH" ]]; then
    echo "   [JAVA] Changes detected ($BUILD_MODE). Compiling Java sources..."
    mkdir -p "$JAVA_BIN_DIR" "scripts"
    
    find "$JAVA_SRC_DIR" -name "*.java" > scripts/sources.txt
    
    # -proc:none disables annotation processors bundled inside Maven/Plexus dependency jars
    javac -proc:none -d "$JAVA_BIN_DIR" @scripts/sources.txt -cp "$CLASS_PATH"
    
    jar cvfm "$JAR_PATH" "$MANIFEST_FILE" -C "$JAVA_BIN_DIR" .
    echo "$CURRENT_JAVA_HASH" > "$CACHE_DIR/java_${BUILD_MODE}.hash"
    echo ">> Updated $JAR_PATH"
else
    echo "   [JAVA] Up to date ($BUILD_MODE). Skipping compilation."
fi

# --- 5. Package Assembly & Deployment ---
DEB_ROOT="$LOCAL_PATH/$PKG_NAME"
PREFIX_PATH="$DEB_ROOT/data/data/com.termux/files/usr"
BIN_DEST="$PREFIX_PATH/bin"
SHARE_DEST="$PREFIX_PATH/share/mkapk"
LIB_DEST="$SHARE_DEST/lib"

DEB_INPUT_HASH="$(hash_files "$BUILD_DIR/mkapk" "$JAR_PATH" "$CACHE_DIR/build_mode")$(hash_dir "$DEPS_DIR")"
if [[ -d "$DEB_ROOT/DEBIAN" ]]; then
    DEB_INPUT_HASH+="$(hash_dir "$DEB_ROOT/DEBIAN")"
fi
OLD_DEB_HASH="$(cat "$CACHE_DIR/deb.hash" 2>/dev/null || true)"

if [[ "$DEB_INPUT_HASH" != "$OLD_DEB_HASH" || ! -f "${PKG_NAME}.deb" ]]; then
    echo ">> Assembling Debian Package ($BUILD_MODE)..."
    mkdir -p "$BIN_DEST" "$SHARE_DEST" "$LIB_DEST"

    cp "$BUILD_DIR/mkapk" "$BIN_DEST/"
    
    if [[ "$BUILD_MODE" == "release" ]]; then
        echo ">> Stripping binary for release..."
        strip "$BIN_DEST/mkapk" 2>/dev/null || true
    else
        echo ">> Preserving debug symbols in package..."
    fi
    
    cp "$JAR_PATH" "$SHARE_DEST/"

    # Copy cached dependencies into debian payload under $PREFIX/share/mkapk/lib/
    echo ">> Packaging dependencies into $LIB_DEST..."
    cp "$DEPS_DIR"/*.jar "$LIB_DEST/"

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

echo "Binary:  $PREFIX/bin/mkapk"
echo "Library: $PREFIX/share/mkapk/$JAR_NAME"
echo "Deps:    $PREFIX/share/mkapk/lib/"
