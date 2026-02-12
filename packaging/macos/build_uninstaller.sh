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
-- macOS 13.0+

on run
    set appName to "Sorana Flow"
    set appPath to "/Applications/Sorana Flow.app"

    -- Check if app is running (graceful: if System Events access is denied, skip the check)
    set isRunning to false
    try
        tell application "System Events"
            set isRunning to (exists (processes whose name is "SoranaFlow"))
        end tell
    end try

    if isRunning then
        display dialog "Sorana Flow가 실행 중입니다." & return & ¬
            "Sorana Flow is currently running." & return & return & ¬
            "앱을 종료한 후 다시 시도해 주세요." & return & ¬
            "Please quit the app before uninstalling." ¬
            buttons {"확인 / OK"} default button "확인 / OK" ¬
            with title "Sorana Flow Uninstaller" with icon caution
        return
    end if

    -- Confirmation dialog
    set userChoice to display dialog ¬
        "Sorana Flow 및 관련 데이터를 삭제합니다:" & return & ¬
        "This will uninstall Sorana Flow and remove all associated data:" & return & return & ¬
        "  • /Applications/Sorana Flow.app" & return & ¬
        "  • 설정 및 라이브러리 / Settings & library database" & return & ¬
        "  • 캐시 / Caches" & return & ¬
        "  • 로그 / Logs" & return & ¬
        "  • 저장된 윈도우 상태 / Saved window state" & return & return & ¬
        "관리자 암호가 필요할 수 있습니다." & return & ¬
        "You may be prompted for your administrator password." & return & return & ¬
        "음악 파일은 삭제되지 않습니다." & return & ¬
        "Your music files will NOT be touched." ¬
        buttons {"취소 / Cancel", "삭제 / Uninstall"} default button "취소 / Cancel" ¬
        with title "Sorana Flow Uninstaller" with icon caution

    if button returned of userChoice is "취소 / Cancel" then
        return
    end if

    set homePath to POSIX path of (path to home folder)
    set removedItems to {}
    set failedItems to {}

    -- 1. App bundle: /Applications/Sorana Flow.app (requires admin privileges)
    if myExists(appPath, "d") then
        try
            do shell script "rm -rf " & quoted form of appPath with administrator privileges
            set end of removedItems to "Application bundle"
        on error errMsg
            try
                do shell script "rm -rf " & quoted form of appPath
                set end of removedItems to "Application bundle"
            on error
                set end of failedItems to "Application bundle (" & errMsg & ")"
            end try
        end try
    end if

    -- 2. Application Support: ~/Library/Application Support/SoranaFlow/
    set p to homePath & "Library/Application Support/SoranaFlow"
    if myExists(p, "d") then
        try
            do shell script "rm -rf " & quoted form of p
            set end of removedItems to "Settings & library (Application Support)"
        on error errMsg
            set end of failedItems to "Application Support (" & errMsg & ")"
        end try
    end if

    -- 3. Cache: ~/Library/Caches/SoranaFlow/
    set p to homePath & "Library/Caches/SoranaFlow"
    if myExists(p, "d") then
        try
            do shell script "rm -rf " & quoted form of p
            set end of removedItems to "Caches (SoranaFlow)"
        on error errMsg
            set end of failedItems to "Caches SoranaFlow (" & errMsg & ")"
        end try
    end if

    -- 4. Cache: ~/Library/Caches/com.soranaflow.app/
    set p to homePath & "Library/Caches/com.soranaflow.app"
    if myExists(p, "d") then
        try
            do shell script "rm -rf " & quoted form of p
            set end of removedItems to "Caches (com.soranaflow.app)"
        on error errMsg
            set end of failedItems to "Caches com.soranaflow.app (" & errMsg & ")"
        end try
    end if

    -- 5. Saved Application State: ~/Library/Saved Application State/com.soranaflow.app.savedState/
    set p to homePath & "Library/Saved Application State/com.soranaflow.app.savedState"
    if myExists(p, "d") then
        try
            do shell script "rm -rf " & quoted form of p
            set end of removedItems to "Saved application state"
        on error errMsg
            set end of failedItems to "Saved state (" & errMsg & ")"
        end try
    end if

    -- 6. Logs: ~/Library/Logs/SoranaFlow/
    set p to homePath & "Library/Logs/SoranaFlow"
    if myExists(p, "d") then
        try
            do shell script "rm -rf " & quoted form of p
            set end of removedItems to "Logs"
        on error errMsg
            set end of failedItems to "Logs (" & errMsg & ")"
        end try
    end if

    -- 7. Preferences: ~/Library/Preferences/com.soranaflow.app.plist (Sparkle only)
    -- SAFE: Only this clean plist. NEVER touch polluted plists
    -- (com.soranaflow.Sorana Flow.plist, com.soranaflow.SoranaFlow.plist)
    set p to homePath & "Library/Preferences/com.soranaflow.app.plist"
    if myExists(p, "f") then
        try
            do shell script "rm -f " & quoted form of p
            set end of removedItems to "Preferences (com.soranaflow.app.plist)"
        on error errMsg
            set end of failedItems to "Preferences (" & errMsg & ")"
        end try
    end if

    -- 8. HTTP Storages: ~/Library/HTTPStorages/com.soranaflow.app/
    set p to homePath & "Library/HTTPStorages/com.soranaflow.app"
    if myExists(p, "d") then
        try
            do shell script "rm -rf " & quoted form of p
            set end of removedItems to "HTTP storages"
        on error errMsg
            set end of failedItems to "HTTP storages (" & errMsg & ")"
        end try
    end if

    -- Completion dialog
    if (count of removedItems) is 0 and (count of failedItems) is 0 then
        display dialog "Sorana Flow가 이 시스템에서 발견되지 않았습니다." & return & ¬
            "Sorana Flow was not found on this system." & return & return & ¬
            "삭제된 항목이 없습니다." & return & ¬
            "Nothing was removed." ¬
            buttons {"확인 / OK"} default button "확인 / OK" ¬
            with title "Sorana Flow Uninstaller"
    else
        set msg to ""

        if (count of removedItems) > 0 then
            set itemList to ""
            repeat with anItem in removedItems
                set itemList to itemList & "  ✓ " & anItem & return
            end repeat
            set msg to "삭제 완료 / Removed:" & return & itemList
        end if

        if (count of failedItems) > 0 then
            set failList to ""
            repeat with anItem in failedItems
                set failList to failList & "  ✗ " & anItem & return
            end repeat
            set msg to msg & return & "삭제 실패 / Failed:" & return & failList
        end if

        if (count of failedItems) is 0 then
            display dialog "Sorana Flow가 삭제되었습니다." & return & ¬
                "Sorana Flow has been uninstalled." & return & return & ¬
                msg & return & ¬
                "음악 파일은 변경되지 않았습니다." & return & ¬
                "Your music files were not modified." ¬
                buttons {"확인 / OK"} default button "확인 / OK" ¬
                with title "Sorana Flow Uninstaller"
        else
            display dialog "일부 항목을 삭제하지 못했습니다." & return & ¬
                "Some items could not be removed." & return & return & ¬
                msg & return & ¬
                "수동으로 삭제해 주세요." & return & ¬
                "Please remove failed items manually." ¬
                buttons {"확인 / OK"} default button "확인 / OK" ¬
                with title "Sorana Flow Uninstaller" with icon caution
        end if
    end if
end run

on myExists(posixPath, fileType)
    try
        set cmd to "test -" & fileType & " " & quoted form of posixPath & " && echo yes || echo no"
        set result to do shell script cmd
        return result is "yes"
    on error
        return false
    end try
end myExists
APPLESCRIPT

echo "  ✓ AppleScript compiled"

# ── Rename binary from "applet" to proper name ────────────────────
# osacompile always creates Contents/MacOS/applet. macOS TCC (Full Disk
# Access, Automation, etc.) shows this binary name. Rename it so the
# user sees "SoranaFlow Uninstaller" instead of "applet".
echo "  → Renaming binary..."
mv "$OUTPUT_APP/Contents/MacOS/applet" "$OUTPUT_APP/Contents/MacOS/SoranaFlow Uninstaller"
/usr/libexec/PlistBuddy -c "Set :CFBundleExecutable 'SoranaFlow Uninstaller'" \
    "$OUTPUT_APP/Contents/Info.plist"
/usr/libexec/PlistBuddy -c "Set :CFBundleName 'Sorana Flow Uninstaller'" \
    "$OUTPUT_APP/Contents/Info.plist"
/usr/libexec/PlistBuddy -c "Set :CFBundleIdentifier 'com.soranaflow.uninstaller'" \
    "$OUTPUT_APP/Contents/Info.plist"
echo "  ✓ Binary renamed to 'SoranaFlow Uninstaller'"

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
echo "  Binary: $OUTPUT_APP/Contents/MacOS/SoranaFlow Uninstaller"
echo ""
