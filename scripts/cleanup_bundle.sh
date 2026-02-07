#!/bin/bash
# cleanup_bundle.sh — Remove unnecessary frameworks from app bundle after macdeployqt
# This reduces the bundle from ~116MB to ~90MB by stripping unused dependencies.

set -euo pipefail

APP_PATH="$1"

if [ -z "$APP_PATH" ] || [ ! -d "$APP_PATH" ]; then
    echo "Usage: cleanup_bundle.sh /path/to/SoranaFlow.app"
    exit 1
fi

FRAMEWORKS="$APP_PATH/Contents/Frameworks"
PLUGINS="$APP_PATH/Contents/PlugIns"

echo "=== Bundle cleanup: $APP_PATH ==="
echo "Before: $(du -sh "$APP_PATH" | cut -f1)"

# ─────────────────────────────────────────────────────────────────────
# USER DATA CLEANUP (must run BEFORE signing)
# Remove files that belong in ~/Library, not inside the .app bundle.
# ─────────────────────────────────────────────────────────────────────

echo "[UserData] Removing database files..."
find "$APP_PATH" -type f \( -name "*.sqlite" -o -name "*.db" -o -name "*.sqlite-wal" -o -name "*.sqlite-shm" \) -delete -print

echo "[UserData] Removing QtWebEngine data directories..."
find "$APP_PATH/Contents" -type d \( -name "Cache" -o -name "GPUCache" -o -name "Local Storage" -o -name "IndexedDB" -o -name "Service Worker" \) ! -path "*/Frameworks/QtWebEngineCore.framework/*" -exec rm -rf {} + 2>/dev/null || true

echo "[UserData] Removing log files..."
find "$APP_PATH" -type f -name "*.log" -delete -print

echo "[UserData] Removing JSON state files..."
find "$APP_PATH" -type f \( -name "bookmarks.json" -o -name "queue.json" -o -name "state.json" -o -name "recent.json" \) -delete -print

echo "[UserData] Removing .p8 private keys..."
find "$APP_PATH" -type f -name "*.p8" -delete -print

echo "[UserData] Removing .DS_Store and ._* files..."
find "$APP_PATH" -type f \( -name ".DS_Store" -o -name "._*" \) -delete -print

echo "[UserData] Stripping extended attributes..."
xattr -cr "$APP_PATH" 2>/dev/null || true

# ── QtQuick/QML — KEPT for QtWebEngine (MusicKit JS playback) ──────
# QtWebEngineWidgets depends on QtQuick/QtQml at runtime.
# Only remove controls/layouts/templates that WebEngine doesn't need.
rm -rf "$FRAMEWORKS/QtQuickControls2.framework"
rm -rf "$FRAMEWORKS/QtQuickTemplates2.framework"
rm -rf "$FRAMEWORKS/QtQuickLayouts.framework"

# ── Video codec libs ───────────────────────────────────────────────
# NOTE: libavcodec links directly against libvpx, libdav1d, libSvtAv1Enc,
# libx264, and libx265. Removing ANY of them causes a dyld crash at launch.
# Keep all of them.

# ── Remove unused Qt frameworks ────────────────────────────────────
# QtOpenGL — KEPT for QtWebEngine (rendering dependency)
# QtDBus — KEPT because QtGui.framework depends on it via @rpath

# ── Remove QML/virtual keyboard plugins ────────────────────────────
rm -rf "$PLUGINS/qmltooling"
rm -rf "$PLUGINS/platforminputcontexts"

# ── Remove unused image format plugins (keep jpeg, png, gif, ico, svg) ─
rm -f "$PLUGINS/imageformats/libqtga.dylib"
rm -f "$PLUGINS/imageformats/libqtiff.dylib"
rm -f "$PLUGINS/imageformats/libqwbmp.dylib"
rm -f "$PLUGINS/imageformats/libqmng.dylib"
rm -f "$PLUGINS/imageformats/libqpdf.dylib"
rm -f "$PLUGINS/imageformats/libqjp2.dylib"
rm -f "$PLUGINS/imageformats/libqicns.dylib"
rm -f "$PLUGINS/imageformats/libqwebp.dylib"

# ── Remove unused plugins ─────────────────────────────────────────
rm -rf "$PLUGINS/networkinformation"

# ── Remove orphaned dylibs (only needed by removed image format plugins) ─
rm -f "$FRAMEWORKS/libwebp.7.dylib"
rm -f "$FRAMEWORKS/libwebpmux.3.dylib"
rm -f "$FRAMEWORKS/libwebpdemux.2.dylib"
rm -f "$FRAMEWORKS/libsharpyuv.0.dylib"
rm -f "$FRAMEWORKS/libmng.2.dylib"
rm -f "$FRAMEWORKS/libjasper.7.dylib"
rm -f "$FRAMEWORKS/libtiff.6.dylib"
rm -f "$FRAMEWORKS/liblcms2.2.dylib"

# NOTE: Homebrew Qt/FFmpeg libs are already stripped — no savings from strip -x.

# ============================================================
# FIX: Copy non-Qt dylibs that macdeployqt misses
# These are linked via find_library() in CMakeLists.txt,
# so macdeployqt doesn't know about them.
# ============================================================
echo "=== Copying missing @rpath dylibs ==="

MAIN_BINARY="$APP_PATH/Contents/MacOS/SoranaFlow"
otool -L "$MAIN_BINARY" 2>/dev/null | grep '@rpath/' | awk '{print $1}' | sed 's|@rpath/||' | while read -r lib; do
    [ -z "$lib" ] && continue
    # Skip frameworks (contain /)
    [[ "$lib" == */* ]] && continue
    if [ ! -e "$FRAMEWORKS/$lib" ]; then
        # Search common library paths
        for search_dir in /usr/local/lib /opt/homebrew/lib; do
            if [ -f "$search_dir/$lib" ]; then
                cp "$search_dir/$lib" "$FRAMEWORKS/"
                chmod u+w "$FRAMEWORKS/$lib" 2>/dev/null || true
                echo "    Copied $lib from $search_dir"
                break
            fi
        done
        if [ ! -e "$FRAMEWORKS/$lib" ]; then
            echo "    WARNING: $lib not found on system!"
        fi
    fi
done

echo "=== Missing dylib copy complete ==="

# ============================================================
# FIX: QtWebEngineProcess helper setup
# The helper resolves Qt frameworks via @loader_path rpaths
# (set by macdeployqt). Do NOT create symlinks in Contents/Frameworks
# — symlinks pointing outside the bundle boundary break codesigning
# ("invalid destination for symbolic link in bundle").
# ============================================================
WEBENGINE_HELPER="$APP_PATH/Contents/Frameworks/QtWebEngineCore.framework/Versions/A/Helpers/QtWebEngineProcess.app"

if [ -d "$WEBENGINE_HELPER" ]; then
    echo "=== Setting up QtWebEngineProcess helper ==="

    # Remove any previously created Contents/Frameworks symlinks (legacy)
    rm -rf "$WEBENGINE_HELPER/Contents/Frameworks"

    # Create qt.conf for the helper process to find resources
    # This fixes "QCoreApplication::applicationDirPath: Please instantiate the QApplication object first"
    HELPER_RESOURCES="$WEBENGINE_HELPER/Contents/Resources"
    mkdir -p "$HELPER_RESOURCES"
    # Prefix is relative to the helper's MacOS dir
    # MacOS -> Contents -> QtWebEngineProcess.app -> Helpers -> A -> Versions -> QtWebEngineCore.framework -> Frameworks -> Contents
    cat > "$HELPER_RESOURCES/qt.conf" << 'QTCONF'
[Paths]
Prefix = ../../../../../../../../
QTCONF

    echo "  Created qt.conf for helper"
    echo "=== Helper setup complete ==="
else
    echo "WARNING: QtWebEngineProcess helper not found!"
fi

# ============================================================
# FIX: Remove homebrew RPATHs from all bundled binaries
# Prevents loading homebrew Qt alongside bundled Qt (class conflicts → SIGSEGV)
# ============================================================
echo "=== Removing homebrew RPATHs from bundled binaries ==="

remove_homebrew_rpaths() {
    local binary="$1"
    local rpaths
    rpaths=$(otool -l "$binary" 2>/dev/null | grep -A2 "LC_RPATH" | grep "path /opt/homebrew" | awk '{print $2}') || true
    for rpath in $rpaths; do
        echo "    Removing rpath $rpath from $(basename "$binary")"
        install_name_tool -delete_rpath "$rpath" "$binary" 2>/dev/null || true
    done
    # Ensure @executable_path/../Frameworks rpath exists (needed for bundled frameworks)
    local has_exec_rpath
    has_exec_rpath=$(otool -l "$binary" 2>/dev/null | grep -A2 "LC_RPATH" | grep "@executable_path/../Frameworks" || true)
    if [ -z "$has_exec_rpath" ] && [[ "$(basename "$binary")" == "SoranaFlow" ]]; then
        echo "    Adding @executable_path/../Frameworks rpath to $(basename "$binary")"
        install_name_tool -add_rpath "@executable_path/../Frameworks" "$binary" 2>/dev/null || true
    fi
}

# Main executable
remove_homebrew_rpaths "$APP_PATH/Contents/MacOS/SoranaFlow"

# QtWebEngineProcess helper
HELPER_BINARY="$WEBENGINE_HELPER/Contents/MacOS/QtWebEngineProcess"
if [ -f "$HELPER_BINARY" ]; then
    remove_homebrew_rpaths "$HELPER_BINARY"
fi

# All framework binaries
for fw in "$FRAMEWORKS"/*.framework; do
    if [ -d "$fw" ]; then
        fwname=$(basename "$fw" .framework)
        fwbinary="$fw/Versions/A/$fwname"
        if [ -f "$fwbinary" ]; then
            remove_homebrew_rpaths "$fwbinary"
        fi
    fi
done

# All dylibs
for dylib in "$FRAMEWORKS"/*.dylib; do
    if [ -f "$dylib" ]; then
        remove_homebrew_rpaths "$dylib"
    fi
done

echo "=== Homebrew RPATH removal complete ==="

# ============================================================
# FIX: Rewrite absolute homebrew install names in helper binary
# macdeployqt fixes the main binary but misses the helper.
# The helper has entries like:
#   /opt/homebrew/opt/qtbase/lib/QtCore.framework/Versions/A/QtCore
# These must become:
#   @rpath/QtCore.framework/Versions/A/QtCore
# ============================================================
HELPER_BINARY="$WEBENGINE_HELPER/Contents/MacOS/QtWebEngineProcess"
if [ -f "$HELPER_BINARY" ]; then
    echo "=== Rewriting helper install names ==="
    chmod u+w "$HELPER_BINARY" 2>/dev/null || true
    # Find all absolute homebrew load paths and rewrite to @rpath
    otool -L "$HELPER_BINARY" | { grep "/opt/homebrew" || true; } | awk '{print $1}' | while read -r old_path; do
        # Extract the framework-relative portion: e.g. QtCore.framework/Versions/A/QtCore
        fw_relative=$(echo "$old_path" | sed 's|.*/\(Qt[^/]*\.framework/.*\)|\1|')
        new_path="@rpath/$fw_relative"
        echo "    $old_path -> $new_path"
        install_name_tool -change "$old_path" "$new_path" "$HELPER_BINARY" 2>/dev/null || true
    done
    echo "=== Helper install names rewritten ==="
fi

# ============================================================
# FIX: Rewrite ALL @executable_path references to @rpath
# macdeployqt sets dylib install names to @executable_path/../Frameworks/
# which works for the main binary but fails for QtWebEngineProcess
# because its @executable_path points to the helper's MacOS dir.
# Using @rpath instead lets both binaries resolve libs via their rpaths.
# ============================================================
echo "=== Rewriting @executable_path -> @rpath for all bundled libraries ==="

# Step 1: Rewrite install name IDs for all dylibs
for dylib in "$FRAMEWORKS"/*.dylib; do
    [ -f "$dylib" ] || continue
    libname=$(basename "$dylib")
    current_id=$(otool -D "$dylib" 2>/dev/null | tail -1)
    if [[ "$current_id" == *"@executable_path"* ]]; then
        chmod u+w "$dylib" 2>/dev/null || true
        install_name_tool -id "@rpath/$libname" "$dylib" 2>/dev/null || true
        echo "    ID: $libname"
    fi
done

# Step 1b: Rewrite install name IDs for all framework binaries
for fw in "$FRAMEWORKS"/*.framework; do
    [ -d "$fw" ] || continue
    fwname=$(basename "$fw" .framework)
    fwbin="$fw/Versions/A/$fwname"
    [ -f "$fwbin" ] || continue
    current_id=$(otool -D "$fwbin" 2>/dev/null | tail -1)
    if [[ "$current_id" == *"@executable_path"* ]]; then
        # Extract framework-relative portion: e.g. QtCore.framework/Versions/A/QtCore
        fw_rel="${current_id#@executable_path/../Frameworks/}"
        chmod u+w "$fwbin" 2>/dev/null || true
        install_name_tool -id "@rpath/$fw_rel" "$fwbin" 2>/dev/null || true
        echo "    ID: $fw_rel"
    fi
done

# Helper: rewrite all @executable_path references in a binary
rewrite_exec_paths() {
    local binary="$1"
    local label="$2"
    chmod u+w "$binary" 2>/dev/null || true
    # Collect refs first (|| true to avoid set -e abort on no matches)
    local refs
    refs=$(otool -L "$binary" 2>/dev/null | grep "@executable_path/../Frameworks/" | awk '{print $1}' || true)
    [ -z "$refs" ] && return 0
    while IFS= read -r ref; do
        [ -z "$ref" ] && continue
        # Strip @executable_path/../Frameworks/ prefix, keep the rest
        local rel="${ref#@executable_path/../Frameworks/}"
        install_name_tool -change "$ref" "@rpath/$rel" "$binary" 2>/dev/null || true
        echo "    $label: $rel"
    done <<< "$refs"
    return 0
}

# Step 2: Rewrite all @executable_path load commands in dylibs
for dylib in "$FRAMEWORKS"/*.dylib; do
    [ -f "$dylib" ] || continue
    rewrite_exec_paths "$dylib" "$(basename "$dylib")"
done

# Step 3: Rewrite all @executable_path load commands in frameworks
for fw in "$FRAMEWORKS"/*.framework; do
    [ -d "$fw" ] || continue
    fwname=$(basename "$fw" .framework)
    fwbin="$fw/Versions/A/$fwname"
    [ -f "$fwbin" ] || continue
    rewrite_exec_paths "$fwbin" "$fwname"
done

# Step 4: Rewrite @executable_path load commands in main executable
rewrite_exec_paths "$APP_PATH/Contents/MacOS/SoranaFlow" "SoranaFlow"

echo "=== @executable_path rewrite complete ==="

echo "After:  $(du -sh "$APP_PATH" | cut -f1)"
echo "=== Cleanup complete ==="
