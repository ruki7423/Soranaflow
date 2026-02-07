#!/bin/bash
# adhoc_sign.sh — Ad-hoc codesign for development builds
# Signs inside-out to avoid "nested code is modified or invalid" errors
set -e

APP_BUNDLE="$1"
if [ -z "$APP_BUNDLE" ] || [ ! -d "$APP_BUNDLE" ]; then
    echo "Usage: adhoc_sign.sh /path/to/SoranaFlow.app"
    exit 1
fi

FW="$APP_BUNDLE/Contents/Frameworks"

# ── Phase 1: Sign dylibs inside frameworks ──────────────────────────
find "$FW" -name "*.framework" -type d -maxdepth 1 | while read -r fwdir; do
    find "$fwdir" -name "*.dylib" -type f 2>/dev/null | while read -r dylib; do
        codesign --force --sign - "$dylib" 2>/dev/null || true
    done
done

# ── Phase 2: Sign QtWebEngineCore internals ──────────────────────────
QTWE_FW="$FW/QtWebEngineCore.framework"
if [ -d "$QTWE_FW" ]; then
    find "$QTWE_FW" -type f \( -name "*.dylib" -o -perm +111 \) ! -name "QtWebEngineCore" 2>/dev/null | while read -r f; do
        file "$f" | grep -q "Mach-O" && \
            codesign --force --sign - "$f" 2>/dev/null || true
    done

    WEBENGINE_HELPER="$QTWE_FW/Versions/A/Helpers/QtWebEngineProcess.app"
    [ -d "$WEBENGINE_HELPER" ] && codesign --force --sign - "$WEBENGINE_HELPER" 2>/dev/null || true
fi

# ── Phase 3: Sign Sparkle internals (Versions/B for 2.x) ────────────
SPARKLE_FW="$FW/Sparkle.framework"
if [ -d "$SPARKLE_FW" ]; then
    if [ -d "$SPARKLE_FW/Versions/B" ]; then
        SPARKLE_VER="$SPARKLE_FW/Versions/B"
    else
        SPARKLE_VER="$SPARKLE_FW/Versions/A"
    fi

    find "$SPARKLE_VER/XPCServices" -name '*.xpc' -type d 2>/dev/null | while read -r xpc; do
        codesign --force --sign - "$xpc" 2>/dev/null || true
    done

    [ -f "$SPARKLE_VER/Autoupdate" ] && codesign --force --sign - "$SPARKLE_VER/Autoupdate" 2>/dev/null || true
    [ -d "$SPARKLE_VER/Updater.app" ] && codesign --force --sign - "$SPARKLE_VER/Updater.app" 2>/dev/null || true
fi

# ── Phase 4: Sign all framework bundles ──────────────────────────────
find "$FW" -maxdepth 1 -name '*.framework' -type d | while read -r fw; do
    codesign --force --sign - "$fw" 2>/dev/null || true
done

# ── Phase 5: Sign standalone dylibs ──────────────────────────────────
find "$FW" -maxdepth 1 -name '*.dylib' -type f | while read -r dylib; do
    codesign --force --sign - "$dylib" 2>/dev/null || true
done

# ── Phase 6: Sign plugins ────────────────────────────────────────────
find "$APP_BUNDLE/Contents/PlugIns" -name '*.dylib' -type f 2>/dev/null | while read -r plugin; do
    codesign --force --sign - "$plugin" 2>/dev/null || true
done

# ── Phase 7: Sign main app bundle last ───────────────────────────────
codesign --force --sign - "$APP_BUNDLE"
