#!/system/bin/sh
set -e

# --- Paths ---
PKG_NAME="mkapk-aarch64"
STORAGE_PATH="/storage/emulated/0/Programming/mkapk"
LOCAL_PATH="$HOME/mkapk_tmp_build"
CLASS_PATH="$(cat scripts/classpath.txt)"

# Clean start
trap 'rm -rf "$LOCAL_PATH"' EXIT 

# --- 0. Sync to Local (Faster I/O) ---
echo ">> Syncing to local storage..."
mkdir -p "$LOCAL_PATH"
cp -r "$STORAGE_PATH/." "$LOCAL_PATH/"
cd "$LOCAL_PATH"

# --- 1. Environment Setup ---
export JAVA_HOME=${JAVA_HOME:-"$PREFIX/lib/jvm/java-21-openjdk"}
export PATH="$JAVA_HOME/bin:$PATH"

# --- 2. Compile C++ Native Engine ---
echo ">> Running Makefile..."
make clean
make -j"$(nproc)"

# --- 3. Compile Java Coordinator ---
echo ">> Compiling Java Coordinator..."
JAVA_SRC_DIR="./java"
JAVA_BIN_DIR="./build/java_out"
JAR_NAME="mkapk-coordinator.jar"

mkdir -p "$JAVA_BIN_DIR"

# Find all .java files and compile them
find "$JAVA_SRC_DIR" -name "*.java" > scripts/sources.txt
javac -d "$JAVA_BIN_DIR" @scripts/sources.txt -cp $CLASS_PATH

# Create the JAR file
jar cvf "build/$JAR_NAME" -C "$JAVA_BIN_DIR" .
echo ">> Created build/$JAR_NAME"

# --- 4. Package Assembly ---
echo ">> Assembling Debian Package..."
DEB_ROOT="$LOCAL_PATH/$PKG_NAME"
PREFIX_PATH="$DEB_ROOT/data/data/com.termux/files/usr"

# Create destinations
BIN_DEST="$PREFIX_PATH/bin"
SHARE_DEST="$PREFIX_PATH/share/mkapk"

mkdir -p "$BIN_DEST"
mkdir -p "$SHARE_DEST"

strip build/mkapk
# A. Copy Primary Binary to /usr/bin
cp build/mkapk "$BIN_DEST/"

# B. Copy JAR to /usr/share/mkapk (where C++ expects it)
cp "build/$JAR_NAME" "$SHARE_DEST/"

# --- 5. Permissions & Build ---
echo ">> Setting Permissions..."
find "$DEB_ROOT" -type d -exec chmod 755 {} +
# Avoid chmoding DEBIAN folder if it doesn't exist in the local build
if [ -d "$DEB_ROOT/DEBIAN" ]; then
    find "$DEB_ROOT/DEBIAN" -type f -exec chmod 644 {} +
    [ -f "$DEB_ROOT/DEBIAN/postinst" ] && chmod 755 "$DEB_ROOT/DEBIAN/postinst"
fi

chmod +x "$BIN_DEST/mkapk"

echo ">> Building .deb..."
dpkg-deb --build "$PKG_NAME"

# --- 6. Deployment ---
echo ">> Installing locally..."

# Copy the finished .deb back to your Programming folder
cp "${PKG_NAME}.deb" "$STORAGE_PATH/"
dpkg --install mkapk-aarch64.deb

echo "🚀 Success!"
echo "Binary: $PREFIX/usr/bin/mkapk"
echo "Library: $PREFIX/share/mkapk/$JAR_NAME"