#!/system/bin/sh
set -e

# --- Paths ---
PKG_NAME="mkapk-aarch64"
STORAGE_PATH="/storage/emulated/0/Programming/mkapk"
LOCAL_PATH="$HOME/mkapk_tmp_build"
CACHE_DIR="$LOCAL_PATH/.cache"
CLASS_PATH="$(cat scripts/classpath.txt 2>/dev/null || echo "")"

# Ensure required tools are installed
for tool in xxhsum rsync; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo ">> Error: '$tool' is required. Install it via: pkg install xxhash rsync" >&2
        exit 1
    fi
done

# --- Trap: Persist Build State Back to Storage on Exit ---
# NOTE: $LOCAL_PATH is kept persistent for instant incremental rebuilds
trap '
    mkdir -p "$STORAGE_PATH/build" "$STORAGE_PATH/.cache"
    rsync -a "$LOCAL_PATH/build/" "$STORAGE_PATH/build/" 2>/dev/null || true
    rsync -a "$LOCAL_PATH/.cache/" "$STORAGE_PATH/.cache/" 2>/dev/null || true
' EXIT

# --- Helper Hash Functions ---
hash_dir() {
    find "$1" -type f 2>/dev/null | sort | xxhsum | xxhsum | cut -d' ' -f1
}

hash_files() {
    xxhsum "$@" 2>/dev/null | xxhsum | cut -d' ' -f1
}

# --- 0. Fast Incremental Sync to Persistent Local Storage ---
echo ">> Syncing changed sources from storage..."
mkdir -p "$LOCAL_PATH"

# Pull only modified/new/deleted files from storage to local workspace
rsync -a --update --delete \
    --exclude='build/' \
    --exclude='.cache/' \
    --exclude='*.deb' \
    "$STORAGE_PATH/" "$LOCAL_PATH/"

# Restore build/cache from storage if running on a fresh/cleared local workspace
if [ ! -d "$LOCAL_PATH/build" ] && [ -d "$STORAGE_PATH/build" ]; then
    rsync -a "$STORAGE_PATH/build/" "$LOCAL_PATH/build/"
fi
if [ ! -d "$LOCAL_PATH/.cache" ] && [ -d "$STORAGE_PATH/.cache" ]; then
    rsync -a "$STORAGE_PATH/.cache/" "$LOCAL_PATH/.cache/"
fi

cd "$LOCAL_PATH"
mkdir -p "$CACHE_DIR"

# --- 1. Environment Setup ---
export JAVA_HOME="${JAVA_HOME:-"$PREFIX/lib/jvm/java-21-openjdk"}"
export PATH="$JAVA_HOME/bin:$PATH"

# --- 2. Compile C++ Native Engine ---
echo ">> Running Makefile (Incremental)..."
make -j"$(nproc)"

# --- 3. Compile Java Coordinator ---
JAVA_SRC_DIR="./java"
JAVA_BIN_DIR="./build/java_out"
JAR_NAME="mkapk-coordinator.jar"
JAR_PATH="build/$JAR_NAME"

echo ">> Checking Java Coordinator..."
CURRENT_JAVA_HASH=$(hash_dir "$JAVA_SRC_DIR")$(echo "$CLASS_PATH" | xxhsum | cut -d' ' -f1)
OLD_JAVA_HASH=$(cat "$CACHE_DIR/java.hash" 2>/dev/null || echo "")

if [ "$CURRENT_JAVA_HASH" != "$OLD_JAVA_HASH" ] || [ ! -f "$JAR_PATH" ]; then
    echo "   [JAVA] Changes detected. Compiling Java sources..."
    mkdir -p "$JAVA_BIN_DIR" "scripts"
    
    find "$JAVA_SRC_DIR" -name "*.java" > scripts/sources.txt
    javac -d "$JAVA_BIN_DIR" @scripts/sources.txt -cp "$CLASS_PATH"
    
    jar cvf "$JAR_PATH" -C "$JAVA_BIN_DIR" .
    echo "$CURRENT_JAVA_HASH" > "$CACHE_DIR/java.hash"
    echo ">> Updated $JAR_PATH"
else
    echo "   [JAVA] Up to date. Skipping compilation."
fi

# --- 4. Package Assembly & Deployment ---
DEB_ROOT="$LOCAL_PATH/$PKG_NAME"
PREFIX_PATH="$DEB_ROOT/data/data/com.termux/files/usr"
BIN_DEST="$PREFIX_PATH/bin"
SHARE_DEST="$PREFIX_PATH/share/mkapk"

# Calculate hash of package contents to decide if rebuilding .deb is necessary
DEB_INPUT_HASH=$(hash_files "build/mkapk" "$JAR_PATH")
if [ -d "$DEB_ROOT/DEBIAN" ]; then
    DEB_INPUT_HASH="${DEB_INPUT_HASH}$(hash_dir "$DEB_ROOT/DEBIAN")"
fi
OLD_DEB_HASH=$(cat "$CACHE_DIR/deb.hash" 2>/dev/null || echo "")

if [ "$DEB_INPUT_HASH" != "$OLD_DEB_HASH" ] || [ ! -f "${PKG_NAME}.deb" ]; then
    echo ">> Assembling Debian Package..."
    mkdir -p "$BIN_DEST" "$SHARE_DEST"

    # Copy files & strip binary in dest to avoid invalidating make targets
    cp build/mkapk "$BIN_DEST/"
    strip "$BIN_DEST/mkapk" 2>/dev/null || true
    cp "$JAR_PATH" "$SHARE_DEST/"

    echo ">> Setting Permissions..."
    find "$DEB_ROOT" -type d -exec chmod 755 {} +
    if [ -d "$DEB_ROOT/DEBIAN" ]; then
        find "$DEB_ROOT/DEBIAN" -type f -exec chmod 644 {} +
        [ -f "$DEB_ROOT/DEBIAN/postinst" ] && chmod 755 "$DEB_ROOT/DEBIAN/postinst"
    fi
    chmod +x "$BIN_DEST/mkapk"

    echo ">> Building .deb..."
    dpkg-deb --build "$PKG_NAME"

    # Copy package to storage and install locally
    cp "${PKG_NAME}.deb" "$STORAGE_PATH/"
    dpkg --install "${PKG_NAME}.deb"

    echo "$DEB_INPUT_HASH" > "$CACHE_DIR/deb.hash"
    echo "🚀 Success! Package updated and installed."
else
    echo "⚡ Everything is up to date! Skipping .deb rebuild and installation."
fi

echo "Binary:  $PREFIX/usr/bin/mkapk"
echo "Library: $PREFIX/share/mkapk/$JAR_NAME"