#!/bin/bash
set -euo pipefail

# ── Sorana Flow Uninstaller Builder ─────────────────────────────────
# Builds an AppleScript-based .app uninstaller using osacompile.
# Works on macOS 13.0+ (no Xcode or Swift required).

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
BUILD_DIR="$PROJECT_DIR/build"
OUTPUT_APP="$BUILD_DIR/Sorana Flow Uninstaller.app"
SOURCE_APP="$BUILD_DIR/SoranaFlow.app"

echo "═══════════════════════════════════════════════════"
echo "  Building Sorana Flow Uninstaller"
echo "═══════════════════════════════════════════════════"

# ── Compile AppleScript to .app ─────────────────────────────────────
echo "  → Compiling AppleScript..."

osacompile -o "$OUTPUT_APP" <<'APPLESCRIPT'
-- Sorana Flow Uninstaller
-- Works on macOS 13.0+

on run
    set appName to "Sorana Flow"
    set appPath to "/Applications/Sorana Flow.app"

    -- Check if app is running
    tell application "System Events"
        set isRunning to (exists (processes whose name is "SoranaFlow"))
    end tell

    if isRunning then
        display dialog appName & " is currently running." & return & return & ¬
            "Please quit the app before uninstalling." ¬
            buttons {"OK"} default button "OK" ¬
            with title "Sorana Flow Uninstaller" with icon caution
        return
    end if

    -- Confirmation dialog
    set userChoice to display dialog ¬
        "This will completely uninstall " & appName & " and remove all associated data:" & return & return & ¬
        "  • Application from /Applications" & return & ¬
        "  • Library database and settings" & return & ¬
        "  • Album art and artist image caches" & return & ¬
        "  • Saved window state" & return & return & ¬
        "Your music files will NOT be touched." ¬
        buttons {"Cancel", "Uninstall"} default button "Cancel" ¬
        with title "Sorana Flow Uninstaller" with icon caution

    if button returned of userChoice is "Cancel" then
        return
    end if

    -- Paths to remove
    set homePath to POSIX path of (path to home folder)
    set removedItems to {}

    -- 1. App bundle
    set appExists to (do shell script "test -d " & quoted form of appPath & " && echo yes || echo no")
    if appExists is "yes" then
        do shell script "rm -rf " & quoted form of appPath
        set end of removedItems to "Application bundle"
    end if

    -- 2. Application Support data (library.db)
    set dataPath to homePath & "Library/Application Support/SoranaFlow"
    set dataExists to (do shell script "test -d " & quoted form of dataPath & " && echo yes || echo no")
    if dataExists is "yes" then
        do shell script "rm -rf " & quoted form of dataPath
        set end of removedItems to "Library database"
    end if

    -- 3. Preferences (settings, bookmarks, Tidal tokens)
    set prefsPath to homePath & "Library/Preferences/com.soranaflow.app.plist"
    set prefsExists to (do shell script "test -f " & quoted form of prefsPath & " && echo yes || echo no")
    if prefsExists is "yes" then
        do shell script "rm -f " & quoted form of prefsPath
        -- Also clear cached preferences from cfprefsd
        do shell script "defaults delete com.soranaflow.app 2>/dev/null || true"
        set end of removedItems to "Preferences and settings"
    end if

    -- 4. Caches (album art, artist images)
    set cachePath to homePath & "Library/Caches/SoranaFlow"
    set cacheExists to (do shell script "test -d " & quoted form of cachePath & " && echo yes || echo no")
    if cacheExists is "yes" then
        do shell script "rm -rf " & quoted form of cachePath
        set end of removedItems to "Caches (album art, artist images)"
    end if

    -- 5. Saved Application State
    set statePath to homePath & "Library/Saved Application State/com.soranaflow.app.savedState"
    set stateExists to (do shell script "test -d " & quoted form of statePath & " && echo yes || echo no")
    if stateExists is "yes" then
        do shell script "rm -rf " & quoted form of statePath
        set end of removedItems to "Saved application state"
    end if

    -- Completion dialog
    if (count of removedItems) is 0 then
        display dialog appName & " was not found on this system." & return & return & ¬
            "Nothing was removed." ¬
            buttons {"OK"} default button "OK" ¬
            with title "Sorana Flow Uninstaller"
    else
        set itemList to ""
        repeat with anItem in removedItems
            set itemList to itemList & "  ✓ " & anItem & return
        end repeat

        display dialog appName & " has been uninstalled." & return & return & ¬
            "Removed:" & return & itemList & return & ¬
            "Your music files were not modified." ¬
            buttons {"OK"} default button "OK" ¬
            with title "Sorana Flow Uninstaller"
    end if
end run
APPLESCRIPT

echo "  ✓ AppleScript compiled"

# ── Copy app icon to uninstaller ────────────────────────────────────
if [ -d "$SOURCE_APP" ]; then
    ICON_SOURCE="$SOURCE_APP/Contents/Resources/SoranaFlow.icns"
    if [ -f "$ICON_SOURCE" ]; then
        echo "  → Copying app icon..."
        ICON_DEST="$OUTPUT_APP/Contents/Resources"
        mkdir -p "$ICON_DEST"
        cp "$ICON_SOURCE" "$ICON_DEST/applet.icns"
        echo "  ✓ Icon set"
    fi
fi

echo ""
echo "═══════════════════════════════════════════════════"
echo "  Uninstaller built successfully"
echo "═══════════════════════════════════════════════════"
echo ""
echo "  Output: $OUTPUT_APP"
echo ""
