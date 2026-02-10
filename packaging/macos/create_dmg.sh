#!/bin/bash
set -euo pipefail

APP_NAME="Sorana Flow"
VERSION="1.3.2"
DMG_NAME="${APP_NAME}-${VERSION}.dmg"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="${SCRIPT_DIR}/../../build"
APP_BUNDLE="${BUILD_DIR}/SoranaFlow.app"

echo "=== Sorana Flow DMG Creator ==="
echo "Version: ${VERSION}"
echo "Build dir: ${BUILD_DIR}"

# Verify app bundle exists
if [ ! -d "${APP_BUNDLE}" ]; then
    echo "ERROR: App bundle not found at ${APP_BUNDLE}"
    echo "Run 'cmake .. && make' in the build directory first."
    exit 1
fi

# Run macdeployqt to bundle Qt frameworks
echo "Running macdeployqt..."
if command -v macdeployqt &>/dev/null; then
    macdeployqt "${APP_BUNDLE}" -verbose=1
elif [ -d "$(brew --prefix qt)/bin" ]; then
    "$(brew --prefix qt)/bin/macdeployqt" "${APP_BUNDLE}" -verbose=1
else
    echo "WARNING: macdeployqt not found. Qt frameworks may not be bundled."
fi

# Create staging directory
echo "Creating staging directory..."
STAGING_DIR=$(mktemp -d)
trap "rm -rf '${STAGING_DIR}'" EXIT

cp -R "${APP_BUNDLE}" "${STAGING_DIR}/"

# Create Applications symlink
ln -s /Applications "${STAGING_DIR}/Applications"

# Remove old DMG if exists
rm -f "${SCRIPT_DIR}/${DMG_NAME}"

# Create DMG
echo "Creating DMG..."
hdiutil create -volname "${APP_NAME}" \
    -srcfolder "${STAGING_DIR}" \
    -ov -format UDZO \
    "${SCRIPT_DIR}/${DMG_NAME}"

echo ""
echo "=== Done ==="
echo "Created: ${SCRIPT_DIR}/${DMG_NAME}"
echo "Size: $(du -h "${SCRIPT_DIR}/${DMG_NAME}" | cut -f1)"
