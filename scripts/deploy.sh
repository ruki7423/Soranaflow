#!/bin/bash
# deploy.sh — Build, Sign, DMG, Notarize, Staple for Sorana Flow
# Usage:
#   ./scripts/deploy.sh              — full pipeline (build + sign + DMG + notarize)
#   ./scripts/deploy.sh --sign-only  — sign existing build only
#   ./scripts/deploy.sh --dmg-only   — create DMG from existing signed build
#   ./scripts/deploy.sh --skip-notarize — build + sign + DMG, skip notarization
#
# Prerequisites:
#   1. Developer ID signing identity installed in Keychain
#   2. App-specific password stored:
#      xcrun notarytool store-credentials "SoranaFlow-notary" \
#        --apple-id "haruki7423@gmail.com" \
#        --team-id "W5JMPJXB5H" \
#        --password "<app-specific-password>"
#   3. Sparkle EdDSA key generated:
#      /opt/homebrew/Caskroom/sparkle/2.8.1/bin/generate_keys

set -euo pipefail

# ── Configuration ────────────────────────────────────────────────────
APP_NAME="Sorana Flow"
BUNDLE_NAME="SoranaFlow"
VERSION="1.7.3"
TEAM_ID="W5JMPJXB5H"
SIGNING_IDENTITY="Developer ID Application: HAESEONG CHOI (W5JMPJXB5H)"
BUNDLE_ID="com.soranaflow.app"
NOTARY_PROFILE="SoranaFlow-notary"

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="$PROJECT_DIR/build"
APP_BUNDLE="$BUILD_DIR/${BUNDLE_NAME}.app"
ENTITLEMENTS="$PROJECT_DIR/packaging/macos/SoranaFlow.entitlements"
DMG_NAME="Soranaflow ${VERSION}.dmg"
DMG_PATH="$BUILD_DIR/$DMG_NAME"
SPARKLE_PATH="/opt/homebrew/Caskroom/sparkle/2.8.1"

# ── Parse arguments ──────────────────────────────────────────────────
SIGN_ONLY=false
DMG_ONLY=false
SKIP_NOTARIZE=false

for arg in "$@"; do
    case $arg in
        --sign-only)     SIGN_ONLY=true ;;
        --dmg-only)      DMG_ONLY=true ;;
        --skip-notarize) SKIP_NOTARIZE=true ;;
        --help|-h)
            echo "Usage: $0 [--sign-only|--dmg-only|--skip-notarize]"
            exit 0
            ;;
    esac
done

# ── Helper functions ─────────────────────────────────────────────────
step() { echo ""; echo "═══════════════════════════════════════════════════"; echo "  $1"; echo "═══════════════════════════════════════════════════"; }
info() { echo "  → $1"; }
error() { echo "  ✗ ERROR: $1" >&2; exit 1; }
ok() { echo "  ✓ $1"; }

# ── Step 1: Verify prerequisites ─────────────────────────────────────
step "Verifying prerequisites"

# Check signing identity
if ! security find-identity -v -p codesigning | grep -q "$TEAM_ID"; then
    error "Signing identity not found for team $TEAM_ID"
fi
ok "Signing identity: $SIGNING_IDENTITY"

# Check entitlements
if [ ! -f "$ENTITLEMENTS" ]; then
    error "Entitlements file not found: $ENTITLEMENTS"
fi
ok "Entitlements: $ENTITLEMENTS"

# Check notary credentials (unless skipping)
if [ "$SKIP_NOTARIZE" = false ] && [ "$DMG_ONLY" = false ]; then
    if ! xcrun notarytool history --keychain-profile "$NOTARY_PROFILE" &>/dev/null; then
        echo "  ⚠ Notary credentials not found. Store them with:"
        echo "    xcrun notarytool store-credentials \"$NOTARY_PROFILE\" \\"
        echo "      --apple-id \"haruki7423@gmail.com\" \\"
        echo "      --team-id \"$TEAM_ID\" \\"
        echo "      --password \"<app-specific-password>\""
        echo ""
        echo "  Continuing without notarization..."
        SKIP_NOTARIZE=true
    else
        ok "Notary credentials: $NOTARY_PROFILE"
    fi
fi

# ── Step 2: Build release ────────────────────────────────────────────
if [ "$SIGN_ONLY" = false ] && [ "$DMG_ONLY" = false ]; then
    step "Building release"
    mkdir -p "$BUILD_DIR"
    cd "$BUILD_DIR"

    # Pass MusicKit developer token if available
    # Priority: 1) env var, 2) /tmp file, 3) generate from .p8, 4) cached in CMakeCache
    EXTRA_CMAKE_ARGS=""
    TOKEN_FILE="/tmp/musickit_token.txt"
    P8_FILE="$PROJECT_DIR/AuthKey_4GW6686CH4.p8"
    TOKEN_SCRIPT="$SCRIPT_DIR/generate_developer_token.sh"

    if [ -n "${MUSICKIT_DEVELOPER_TOKEN:-}" ]; then
        echo "  Using MUSICKIT_DEVELOPER_TOKEN from environment"
        EXTRA_CMAKE_ARGS="-DMUSICKIT_DEVELOPER_TOKEN=$MUSICKIT_DEVELOPER_TOKEN"
    elif [ -f "$TOKEN_FILE" ]; then
        echo "  Using token from $TOKEN_FILE"
        EXTRA_CMAKE_ARGS="-DMUSICKIT_DEVELOPER_TOKEN=$(cat "$TOKEN_FILE")"
    elif [ -f "$P8_FILE" ] && [ -x "$TOKEN_SCRIPT" ]; then
        echo "  Generating fresh token from .p8 key..."
        GENERATED_TOKEN=$("$TOKEN_SCRIPT" "$P8_FILE" "4GW6686CH4" "$TEAM_ID" 180)
        if [ -n "$GENERATED_TOKEN" ]; then
            EXTRA_CMAKE_ARGS="-DMUSICKIT_DEVELOPER_TOKEN=$GENERATED_TOKEN"
            echo "  Token generated (${#GENERATED_TOKEN} chars, expires in 180 days)"
        else
            echo "  ⚠ Token generation failed"
        fi
    else
        # Check if token is already cached in CMakeCache
        CACHED_TOKEN=""
        if [ -f "$BUILD_DIR/CMakeCache.txt" ]; then
            CACHED_TOKEN=$(grep "^MUSICKIT_DEVELOPER_TOKEN:STRING=" "$BUILD_DIR/CMakeCache.txt" 2>/dev/null | cut -d= -f2 || true)
        fi
        if [ -n "$CACHED_TOKEN" ]; then
            echo "  Using cached token from CMakeCache (${#CACHED_TOKEN} chars)"
            # Token is already cached, cmake will reuse it
        else
            echo "  ⚠ No MusicKit developer token available!"
            echo "    Apple Music REST API will not work in distributed builds."
            echo "    To fix: set MUSICKIT_DEVELOPER_TOKEN env var or place AuthKey .p8 in project dir"
        fi
    fi

    cmake .. -DCMAKE_BUILD_TYPE=Release $EXTRA_CMAKE_ARGS
    make -j"$(sysctl -n hw.ncpu)"
    ok "Build complete"
fi

# Verify app bundle exists
if [ ! -d "$APP_BUNDLE" ]; then
    error "App bundle not found: $APP_BUNDLE"
fi

# ── Step 3: Embed Sparkle.framework ──────────────────────────────────
if [ "$DMG_ONLY" = false ]; then
    step "Embedding Sparkle.framework"
    SPARKLE_FW="$SPARKLE_PATH/Sparkle.framework"
    if [ -d "$SPARKLE_FW" ]; then
        rm -rf "$APP_BUNDLE/Contents/Frameworks/Sparkle.framework"
        ditto "$SPARKLE_FW" "$APP_BUNDLE/Contents/Frameworks/Sparkle.framework"
        ok "Sparkle.framework embedded"
    else
        echo "  ⚠ Sparkle.framework not found — auto-update disabled"
    fi
fi

# ── Step 3.5: Bundle cleanup (user data + unused frameworks) ─────────
if [ "$DMG_ONLY" = false ]; then
    step "Cleaning app bundle"
    CLEANUP_SCRIPT="$SCRIPT_DIR/cleanup_bundle.sh"
    if [ -x "$CLEANUP_SCRIPT" ]; then
        "$CLEANUP_SCRIPT" "$APP_BUNDLE"
        ok "Bundle cleanup complete"
    else
        echo "  ⚠ cleanup_bundle.sh not found or not executable — skipping"
    fi
fi

# ── Step 4: Deep code sign ───────────────────────────────────────────
if [ "$DMG_ONLY" = false ]; then
    step "Code signing (Developer ID)"

    IDENTITY="$SIGNING_IDENTITY"
    APP="$APP_BUNDLE"
    FW="$APP/Contents/Frameworks"

    # ── Phase 1: Sign all dylibs INSIDE frameworks (deepest first) ────
    info "Phase 1: Signing dylibs inside frameworks..."
    find "$FW" -name "*.framework" -type d -maxdepth 1 | while read -r fwdir; do
        find "$fwdir" -name "*.dylib" -type f 2>/dev/null | while read -r dylib; do
            codesign --force --sign "$IDENTITY" --timestamp --options runtime \
                "$dylib" 2>/dev/null || true
        done
    done

    # ── Phase 2: Sign all executables inside QtWebEngineCore.framework ─
    info "Phase 2: Signing QtWebEngineCore internals..."
    QTWE_FW="$FW/QtWebEngineCore.framework"
    if [ -d "$QTWE_FW" ]; then
        # Sign every Mach-O binary inside EXCEPT the main framework binary
        find "$QTWE_FW" -type f \( -name "*.dylib" -o -perm +111 \) ! -name "QtWebEngineCore" 2>/dev/null | while read -r f; do
            file "$f" | grep -q "Mach-O" && \
                codesign --force --sign "$IDENTITY" --timestamp --options runtime \
                    "$f" 2>/dev/null || true
        done

        # Sign QtWebEngineProcess.app helper bundle
        WEBENGINE_HELPER="$QTWE_FW/Versions/A/Helpers/QtWebEngineProcess.app"
        if [ -d "$WEBENGINE_HELPER" ]; then
            info "  Signing QtWebEngineProcess.app..."
            codesign --force --sign "$IDENTITY" --timestamp --options runtime \
                --entitlements "$ENTITLEMENTS" \
                "$WEBENGINE_HELPER"
        fi
    fi

    # ── Phase 3: Sign Sparkle.framework internals (Versions/B for 2.x) ─
    info "Phase 3: Signing Sparkle internals..."
    SPARKLE_FW_BUNDLE="$FW/Sparkle.framework"
    if [ -d "$SPARKLE_FW_BUNDLE" ]; then
        # Detect version dir (B for 2.x, A for 1.x)
        if [ -d "$SPARKLE_FW_BUNDLE/Versions/B" ]; then
            SPARKLE_VER="$SPARKLE_FW_BUNDLE/Versions/B"
        else
            SPARKLE_VER="$SPARKLE_FW_BUNDLE/Versions/A"
        fi

        # XPC services first
        find "$SPARKLE_VER/XPCServices" -name "*.xpc" -type d 2>/dev/null | while read -r xpc; do
            codesign --force --sign "$IDENTITY" --timestamp --options runtime \
                "$xpc" 2>/dev/null || true
        done

        # Autoupdate binary
        [ -f "$SPARKLE_VER/Autoupdate" ] && \
            codesign --force --sign "$IDENTITY" --timestamp --options runtime \
                "$SPARKLE_VER/Autoupdate" 2>/dev/null || true

        # Updater.app
        [ -d "$SPARKLE_VER/Updater.app" ] && \
            codesign --force --sign "$IDENTITY" --timestamp --options runtime \
                "$SPARKLE_VER/Updater.app" 2>/dev/null || true
    fi

    # ── Phase 4: Sign all framework bundles ───────────────────────────
    info "Phase 4: Signing framework bundles..."
    find "$FW" -name "*.framework" -type d -maxdepth 1 | while read -r fwdir; do
        codesign --force --sign "$IDENTITY" --timestamp --options runtime \
            "$fwdir" 2>/dev/null || true
    done

    # ── Phase 5: Sign standalone dylibs in Frameworks/ ────────────────
    info "Phase 5: Signing standalone dylibs..."
    find "$FW" -maxdepth 1 -name "*.dylib" -type f | while read -r dylib; do
        codesign --force --sign "$IDENTITY" --timestamp --options runtime \
            "$dylib" 2>/dev/null || true
    done

    # ── Phase 6: Sign plugins ─────────────────────────────────────────
    info "Phase 6: Signing plugins..."
    find "$APP/Contents/PlugIns" -name "*.dylib" -type f 2>/dev/null | while read -r plugin; do
        codesign --force --sign "$IDENTITY" --timestamp --options runtime \
            "$plugin" 2>/dev/null || true
    done

    # ── Phase 7: Sign main app bundle (outermost — must be last) ──────
    info "Phase 7: Signing main app bundle..."
    codesign --force --sign "$IDENTITY" --timestamp --options runtime \
        --entitlements "$ENTITLEMENTS" \
        "$APP"

    ok "Code signing complete"

    # Verify signature
    info "Verifying signature..."
    codesign --verify --deep --strict "$APP_BUNDLE"
    ok "Signature verified"

    # Check with spctl (Gatekeeper assessment)
    info "Checking Gatekeeper assessment..."
    if spctl --assess --type exec --verbose=4 "$APP_BUNDLE" 2>&1; then
        ok "Gatekeeper assessment passed"
    else
        echo "  ⚠ Gatekeeper assessment failed (expected before notarization)"
    fi
fi

# ── Step 5: Notarize + staple app bundle ─────────────────────────────
if [ "$SKIP_NOTARIZE" = false ] && [ "$DMG_ONLY" = false ]; then
    step "Notarizing app bundle"
    APP_ZIP="$BUILD_DIR/${BUNDLE_NAME}.zip"
    ditto -c -k --keepParent "$APP_BUNDLE" "$APP_ZIP"
    info "Submitting app bundle for notarization..."
    xcrun notarytool submit "$APP_ZIP" \
        --keychain-profile "$NOTARY_PROFILE" \
        --wait
    ok "App bundle notarized"

    info "Stapling app bundle..."
    xcrun stapler staple "$APP_BUNDLE"
    ok "App bundle stapled"

    rm -f "$APP_ZIP"
fi

# ── Step 6: Create DMG ───────────────────────────────────────────────
step "Creating DMG"

# Detach any stale mount of the same volume name
hdiutil detach "/Volumes/$BUNDLE_NAME" -force 2>/dev/null || true

# Create a staging directory
STAGING_DIR=$(mktemp -d)
trap "rm -rf '$STAGING_DIR'" EXIT

# Copy app to staging (ditto preserves extended attributes + staple ticket)
ditto "$APP_BUNDLE" "$STAGING_DIR/${BUNDLE_NAME}.app"

# Strip ONLY quarantine/provenance xattrs (not all xattrs — xattr -cr destroys
# code-signature-related metadata and can break Gatekeeper assessment)
xattr -dr com.apple.quarantine "$STAGING_DIR/${BUNDLE_NAME}.app" 2>/dev/null || true
xattr -dr com.apple.provenance "$STAGING_DIR/${BUNDLE_NAME}.app" 2>/dev/null || true

# Verify staged app signature is still valid
info "Verifying staged app signature..."
codesign --verify --deep --strict "$STAGING_DIR/${BUNDLE_NAME}.app" || error "Staged app signature broken!"

# Create Applications symlink
ln -s /Applications "$STAGING_DIR/Applications"

# Remove old DMG
rm -f "$DMG_PATH"

# Create DMG using read-write + convert approach (avoids TCC "Operation not permitted"
# that hdiutil create -srcfolder triggers on macOS Sequoia)
info "Creating compressed DMG..."
RW_DMG="$BUILD_DIR/_rw_temp.dmg"
rm -f "$RW_DMG"

# Calculate required size (app size + 20MB headroom)
APP_SIZE_KB=$(du -sk "$STAGING_DIR/${BUNDLE_NAME}.app" | cut -f1)
DMG_SIZE_KB=$(( APP_SIZE_KB + 20480 ))

# Create blank read-write DMG
hdiutil create -size "${DMG_SIZE_KB}k" -fs HFS+ -volname "$BUNDLE_NAME" \
    -type UDIF -layout NONE "$RW_DMG"

# Mount at a temp path (NOT /Volumes — TCC blocks writes to /Volumes on Sequoia)
RW_MOUNT=$(mktemp -d)
hdiutil attach "$RW_DMG" -mountpoint "$RW_MOUNT" -nobrowse -owners off

# Copy contents using ditto (preserves code signatures + staple tickets)
ditto "$STAGING_DIR/${BUNDLE_NAME}.app" "$RW_MOUNT/${BUNDLE_NAME}.app"
ln -s /Applications "$RW_MOUNT/Applications"

# Unmount
hdiutil detach "$RW_MOUNT"
rmdir "$RW_MOUNT" 2>/dev/null || true

# Convert to compressed read-only DMG
hdiutil convert "$RW_DMG" -format UDZO -imagekey zlib-level=9 -o "$DMG_PATH"
rm -f "$RW_DMG"

ok "DMG created: $DMG_PATH"
info "Size: $(du -h "$DMG_PATH" | cut -f1)"

# Sign the DMG
info "Signing DMG..."
codesign --force --timestamp --sign "$SIGNING_IDENTITY" "$DMG_PATH"
ok "DMG signed"

# ── Step 7: Notarize + staple DMG ────────────────────────────────────
if [ "$SKIP_NOTARIZE" = false ]; then
    step "Notarizing DMG"
    info "Submitting DMG for notarization..."

    xcrun notarytool submit "$DMG_PATH" \
        --keychain-profile "$NOTARY_PROFILE" \
        --wait

    ok "DMG notarization complete"

    step "Stapling DMG"
    xcrun stapler staple "$DMG_PATH"
    ok "Stapled: $DMG_PATH"

    xcrun stapler validate "$DMG_PATH"
    ok "DMG staple validation passed"
else
    echo ""
    echo "  ⚠ Notarization skipped (use without --skip-notarize for production)"
fi

# ── Step 8: Verify app inside DMG ────────────────────────────────────
step "Verifying DMG contents"
DMG_MOUNT=$(mktemp -d)
hdiutil attach "$DMG_PATH" -mountpoint "$DMG_MOUNT" -nobrowse -readonly 2>/dev/null
if [ -d "$DMG_MOUNT/${BUNDLE_NAME}.app" ]; then
    info "Checking code signature inside DMG..."
    if codesign --verify --deep --strict "$DMG_MOUNT/${BUNDLE_NAME}.app" 2>&1; then
        ok "App inside DMG: signature valid"
    else
        echo "  ⚠ App inside DMG: signature INVALID"
    fi
    info "Checking Gatekeeper assessment..."
    if spctl --assess --type exec --verbose=4 "$DMG_MOUNT/${BUNDLE_NAME}.app" 2>&1; then
        ok "Gatekeeper: accepted"
    else
        echo "  ⚠ Gatekeeper: rejected (check notarization)"
    fi
else
    echo "  ⚠ Could not find app in mounted DMG"
fi
hdiutil detach "$DMG_MOUNT" -force 2>/dev/null || true
rmdir "$DMG_MOUNT" 2>/dev/null || true

# ── Summary ──────────────────────────────────────────────────────────
step "Deployment complete"
echo ""
echo "  App:     $APP_BUNDLE"
echo "  DMG:     $DMG_PATH"
echo "  Size:    $(du -h "$DMG_PATH" | cut -f1)"
echo "  Version: $VERSION"
echo ""
if [ "$SKIP_NOTARIZE" = false ]; then
    echo "  ✓ Signed, notarized, and stapled — ready for distribution!"
else
    echo "  ✓ Signed and packaged — notarize before distribution."
fi
echo ""

# ── Sparkle update signing (optional) ────────────────────────────────
echo "  To sign this update for Sparkle appcast:"
echo "    ${SPARKLE_PATH}/bin/sign_update \"$DMG_PATH\""
echo ""
