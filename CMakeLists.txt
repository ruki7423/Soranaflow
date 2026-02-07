cmake_minimum_required(VERSION 3.21)
set(CMAKE_OSX_DEPLOYMENT_TARGET "13.0" CACHE STRING "Minimum macOS version")
project(SoranaFlow VERSION 1.2.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

# Default to Release build
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# Release optimizations
set(CMAKE_CXX_FLAGS_RELEASE "-O2 -DNDEBUG")
if(APPLE AND CMAKE_BUILD_TYPE STREQUAL "Release")
    set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -flto=thin")
endif()

# Enable Objective-C++ for .mm files
enable_language(OBJCXX)

# MusicKit Developer Token — pre-generated JWT to avoid bundling .p8 private key
# Usage: cmake -DMUSICKIT_DEVELOPER_TOKEN="eyJhbG..." ..
set(MUSICKIT_DEVELOPER_TOKEN "" CACHE STRING "Pre-generated MusicKit Developer Token (JWT)")
if(MUSICKIT_DEVELOPER_TOKEN)
    add_compile_definitions(MUSICKIT_DEVELOPER_TOKEN="${MUSICKIT_DEVELOPER_TOKEN}")
    message(STATUS "MusicKit: Using embedded developer token")
endif()

# Find Qt6
find_package(Qt6 REQUIRED COMPONENTS Widgets Svg SvgWidgets Sql Network WebEngineWidgets WebChannel Concurrent)

# Find FFmpeg via pkg-config
find_package(PkgConfig REQUIRED)
pkg_check_modules(AVFORMAT REQUIRED IMPORTED_TARGET libavformat)
pkg_check_modules(AVCODEC  REQUIRED IMPORTED_TARGET libavcodec)
pkg_check_modules(AVUTIL   REQUIRED IMPORTED_TARGET libavutil)
pkg_check_modules(SWRESAMPLE REQUIRED IMPORTED_TARGET libswresample)

# Find TagLib via pkg-config
pkg_check_modules(TAGLIB REQUIRED IMPORTED_TARGET taglib)

# Find Chromaprint via pkg-config
pkg_check_modules(CHROMAPRINT REQUIRED IMPORTED_TARGET libchromaprint)

# Find libsoxr (SoX Resampler) for high-quality upsampling
pkg_check_modules(SOXR REQUIRED IMPORTED_TARGET soxr)

# Find libebur128 for EBU R128 loudness analysis
pkg_check_modules(EBUR128 REQUIRED IMPORTED_TARGET libebur128)

# Find libmysofa for HRTF binaural audio (OPTIONAL)
# Not available in Homebrew — must be built from source:
#   git clone https://github.com/hoene/libmysofa
#   cd libmysofa/build && cmake .. && make && sudo make install
pkg_check_modules(MYSOFA libmysofa)
if(MYSOFA_FOUND)
    message(STATUS "libmysofa found — HRTF binaural audio enabled")
else()
    # Try find_library as fallback
    find_path(MYSOFA_INCLUDE_DIR mysofa.h
        PATHS /opt/homebrew/include /usr/local/include /usr/include)
    find_library(MYSOFA_LIBRARY mysofa
        PATHS /opt/homebrew/lib /usr/local/lib /usr/lib)
    if(MYSOFA_INCLUDE_DIR AND MYSOFA_LIBRARY)
        set(MYSOFA_FOUND TRUE)
        message(STATUS "libmysofa found via find_library — HRTF binaural audio enabled")
    else()
        message(STATUS "libmysofa NOT found — HRTF binaural audio disabled")
        message(STATUS "  To enable HRTF, build libmysofa from source:")
        message(STATUS "    git clone https://github.com/hoene/libmysofa")
        message(STATUS "    cd libmysofa/build && cmake .. && make && sudo make install")
    endif()
endif()

# Source files
set(CORE_SOURCES
    src/core/MusicData.h
    src/core/MusicData.cpp
    src/core/ThemeManager.h
    src/core/ThemeManager.cpp
    src/core/PlaybackState.h
    src/core/PlaybackState.cpp
    src/core/Settings.h
    src/core/Settings.cpp
    src/core/CoverArtLoader.h
    src/core/CoverArtLoader.cpp
    src/core/lyrics/LyricsProvider.h
    src/core/lyrics/LyricsProvider.cpp
)

set(LIBRARY_SOURCES
    src/core/library/LibraryDatabase.h
    src/core/library/LibraryDatabase.cpp
    src/core/library/LibraryScanner.h
    src/core/library/LibraryScanner.cpp
    src/core/library/PlaylistManager.h
    src/core/library/PlaylistManager.cpp
    src/core/library/AutoOrganizer.h
    src/core/library/AutoOrganizer.cpp
)

set(AUDIO_SOURCES
    src/core/audio/AudioFormat.h
    src/core/audio/SignalPathInfo.h
    src/core/audio/AudioDecoder.h
    src/core/audio/AudioDecoder.cpp
    src/core/audio/DSDDecoder.h
    src/core/audio/DSDDecoder.cpp
    src/core/audio/AudioEngine.h
    src/core/audio/AudioEngine.cpp
    src/core/audio/MetadataReader.h
    src/core/audio/MetadataReader.cpp
    src/core/audio/TagWriter.h
    src/core/audio/TagWriter.cpp
    src/core/audio/AudioDeviceManager.h
    src/platform/macos/AudioDeviceManager_mac.cpp
)

set(DSP_SOURCES
    src/core/dsp/IDSPProcessor.h
    src/core/dsp/GainProcessor.h
    src/core/dsp/EqualizerProcessor.h
    src/core/dsp/EqualizerProcessor.cpp
    src/core/dsp/DSPPipeline.h
    src/core/dsp/DSPPipeline.cpp
    src/core/dsp/UpsamplerProcessor.h
    src/core/dsp/UpsamplerProcessor.cpp
    src/core/dsp/LoudnessAnalyzer.h
    src/core/dsp/LoudnessAnalyzer.cpp
    src/core/dsp/CrossfeedProcessor.h
    src/core/dsp/CrossfeedProcessor.cpp
    src/core/dsp/ConvolutionProcessor.h
    src/core/dsp/ConvolutionProcessor.cpp
    src/core/dsp/HRTFProcessor.h
    src/core/dsp/HRTFProcessor.cpp
)

set(METADATA_SOURCES
    src/metadata/RateLimiter.h
    src/metadata/RateLimiter.cpp
    src/metadata/MusicBrainzProvider.h
    src/metadata/MusicBrainzProvider.cpp
    src/metadata/CoverArtProvider.h
    src/metadata/CoverArtProvider.cpp
    src/metadata/FanartTvProvider.h
    src/metadata/FanartTvProvider.cpp
    src/metadata/MetadataService.h
    src/metadata/MetadataService.cpp
    src/metadata/AcoustIdProvider.h
    src/metadata/AcoustIdProvider.cpp
    src/metadata/AudioFingerprinter.h
    src/metadata/AudioFingerprinter.cpp
)

set(RADIO_SOURCES
    src/radio/LastFmProvider.h
    src/radio/LastFmProvider.cpp
    src/radio/AutoplayManager.h
    src/radio/AutoplayManager.cpp
)

set(PLUGIN_SOURCES
    src/plugins/VST3Host.h
    src/plugins/VST3Host.cpp
    src/plugins/VST3Plugin.h
    src/plugins/VST3Plugin.cpp
    src/plugins/VST2Host.h
    src/plugins/VST2Host.cpp
    src/plugins/VST2Plugin.h
    src/plugins/VST2Plugin.cpp
)

# VST3 SDK sources
set(VST3_SDK_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/external/vst3sdk)

set(VST3_SDK_SOURCES
    # Base sources
    ${VST3_SDK_ROOT}/base/source/baseiids.cpp
    ${VST3_SDK_ROOT}/base/source/fobject.cpp
    ${VST3_SDK_ROOT}/base/source/fstring.cpp
    ${VST3_SDK_ROOT}/base/source/fbuffer.cpp
    ${VST3_SDK_ROOT}/base/source/fdebug.cpp
    ${VST3_SDK_ROOT}/base/source/fdynlib.cpp
    ${VST3_SDK_ROOT}/base/source/updatehandler.cpp
    ${VST3_SDK_ROOT}/base/source/timer.cpp
    ${VST3_SDK_ROOT}/base/thread/source/flock.cpp
    # Plug interfaces sources
    ${VST3_SDK_ROOT}/pluginterfaces/base/funknown.cpp
    ${VST3_SDK_ROOT}/pluginterfaces/base/coreiids.cpp
    # IID definitions
    ${VST3_SDK_ROOT}/public.sdk/source/vst/vstinitiids.cpp
    ${VST3_SDK_ROOT}/public.sdk/source/common/commoniids.cpp
    # Common sources
    ${VST3_SDK_ROOT}/public.sdk/source/common/commonstringconvert.cpp
    # Hosting sources
    ${VST3_SDK_ROOT}/public.sdk/source/vst/hosting/module.cpp
    ${VST3_SDK_ROOT}/public.sdk/source/vst/hosting/hostclasses.cpp
    ${VST3_SDK_ROOT}/public.sdk/source/vst/hosting/pluginterfacesupport.cpp
    ${VST3_SDK_ROOT}/public.sdk/source/vst/hosting/plugprovider.cpp
    ${VST3_SDK_ROOT}/public.sdk/source/vst/hosting/connectionproxy.cpp
    ${VST3_SDK_ROOT}/public.sdk/source/vst/hosting/eventlist.cpp
    ${VST3_SDK_ROOT}/public.sdk/source/vst/hosting/parameterchanges.cpp
    ${VST3_SDK_ROOT}/public.sdk/source/vst/hosting/processdata.cpp
    # Utility
    ${VST3_SDK_ROOT}/public.sdk/source/vst/utility/stringconvert.cpp
)

# macOS-specific VST3 SDK source (Objective-C++ with ARC)
if(APPLE)
    set(VST3_SDK_OBJCXX_SOURCES
        ${VST3_SDK_ROOT}/public.sdk/source/vst/hosting/module_mac.mm
        ${VST3_SDK_ROOT}/public.sdk/source/common/threadchecker_mac.mm
    )
    set_source_files_properties(${VST3_SDK_OBJCXX_SOURCES} PROPERTIES
        COMPILE_FLAGS "-fobjc-arc"
    )
endif()

set(PLATFORM_SOURCES
    src/platform/AudioDevice.h
    src/platform/IAudioOutput.h
    src/platform/macos/CoreAudioOutput.h
    src/platform/macos/CoreAudioOutput.cpp
    src/platform/macos/BookmarkManager.h
    src/platform/macos/BookmarkManager.mm
    src/platform/macos/MacMediaIntegration.h
    src/platform/macos/MacMediaIntegration.mm
    src/platform/macos/SparkleUpdater.h
    src/platform/macos/SparkleUpdater.mm
    src/platform/macos/AudioProcessTap.h
    src/platform/macos/AudioProcessTap.mm
)

# BookmarkManager + MacMediaIntegration need ARC
if(APPLE)
    set_source_files_properties(src/platform/macos/BookmarkManager.mm PROPERTIES
        COMPILE_FLAGS "-fobjc-arc"
    )
    set_source_files_properties(src/platform/macos/MacMediaIntegration.mm PROPERTIES
        COMPILE_FLAGS "-fobjc-arc"
    )
    set_source_files_properties(src/platform/macos/SparkleUpdater.mm PROPERTIES
        COMPILE_FLAGS "-fobjc-arc"
    )
    set_source_files_properties(src/platform/macos/AudioProcessTap.mm PROPERTIES
        COMPILE_FLAGS "-fobjc-arc"
    )
endif()

# Apple Music integration (macOS 13+)
if(APPLE)
    set(APPLE_MUSIC_SOURCES
        src/apple/AppleMusicManager.h
        src/apple/AppleMusicBridge.mm
        src/apple/MusicKitPlayer.h
        src/apple/MusicKitPlayer.cpp
    )
    set_source_files_properties(src/apple/AppleMusicBridge.mm PROPERTIES
        COMPILE_FLAGS "-fobjc-arc"
    )
endif()

# Tidal integration
set(TIDAL_SOURCES
    src/tidal/TidalManager.h
    src/tidal/TidalManager.cpp
)

set(WIDGET_SOURCES
    src/widgets/StyledButton.h
    src/widgets/StyledButton.cpp
    src/widgets/StyledSlider.h
    src/widgets/StyledSlider.cpp
    src/widgets/StyledSwitch.h
    src/widgets/StyledSwitch.cpp
    src/widgets/StyledInput.h
    src/widgets/StyledInput.cpp
    src/widgets/StyledComboBox.h
    src/widgets/StyledComboBox.cpp
    src/widgets/StyledScrollArea.h
    src/widgets/StyledScrollArea.cpp
    src/widgets/FormatBadge.h
    src/widgets/FormatBadge.cpp
    src/widgets/TrackRow.h
    src/widgets/TrackRow.cpp
    src/widgets/TrackTableView.h
    src/widgets/TrackTableView.cpp
    src/widgets/SignalPathWidget.h
    src/widgets/SignalPathWidget.cpp
    src/widgets/LyricsWidget.h
    src/widgets/LyricsWidget.cpp
)

set(UI_SOURCES
    src/ui/SoranaFlowLogo.h
    src/ui/SoranaFlowLogo.cpp
    src/ui/AppSidebar.h
    src/ui/AppSidebar.cpp
    src/ui/PlaybackBar.h
    src/ui/PlaybackBar.cpp
    src/ui/MainWindow.h
    src/ui/MainWindow.cpp
)

set(VIEW_SOURCES
    src/ui/views/NowPlayingView.h
    src/ui/views/NowPlayingView.cpp
    src/ui/views/LibraryView.h
    src/ui/views/LibraryView.cpp
    src/ui/views/AlbumsView.h
    src/ui/views/AlbumsView.cpp
    src/ui/views/AlbumDetailView.h
    src/ui/views/AlbumDetailView.cpp
    src/ui/views/ArtistsView.h
    src/ui/views/ArtistsView.cpp
    src/ui/views/ArtistDetailView.h
    src/ui/views/ArtistDetailView.cpp
    src/ui/views/PlaylistsView.h
    src/ui/views/PlaylistsView.cpp
    src/ui/views/PlaylistDetailView.h
    src/ui/views/PlaylistDetailView.cpp
    src/ui/views/QueueView.h
    src/ui/views/QueueView.cpp
    src/ui/views/SettingsView.h
    src/ui/views/SettingsView.cpp
    src/ui/views/AppleMusicView.h
    src/ui/views/AppleMusicView.cpp
    # src/ui/views/TidalView.h      # TODO: restore when Tidal API available
    # src/ui/views/TidalView.cpp    # TODO: restore when Tidal API available
    src/ui/views/SearchResultsView.h
    src/ui/views/SearchResultsView.cpp
    src/ui/dialogs/TagEditorDialog.h
    src/ui/dialogs/TagEditorDialog.cpp
    src/ui/dialogs/AutoOrganizeDialog.h
    src/ui/dialogs/AutoOrganizeDialog.cpp
    src/ui/dialogs/MetadataSearchDialog.h
    src/ui/dialogs/MetadataSearchDialog.cpp
    src/ui/dialogs/NewPlaylistDialog.h
    src/ui/dialogs/NewPlaylistDialog.cpp
    src/ui/dialogs/StyledMessageBox.h
    src/ui/dialogs/StyledMessageBox.cpp
)

set(RESOURCES
    resources/resources.qrc
)

# Create executable
if(APPLE)
    set(MACOSX_BUNDLE_ICON_FILE "SoranaFlow.icns")
    set(MACOSX_BUNDLE_BUNDLE_NAME "Sorana Flow")
    set(MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION})
    set(MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION})

    # App icon — copy .icns into bundle Resources
    set(APP_ICON_FILE "${CMAKE_CURRENT_SOURCE_DIR}/resources/icons/SoranaFlow.icns")
    set_source_files_properties(${APP_ICON_FILE} PROPERTIES
        MACOSX_PACKAGE_LOCATION "Resources"
    )

    # Sparkle auto-update framework
    set(SPARKLE_PATH "/opt/homebrew/Caskroom/sparkle/2.8.1/Sparkle.framework")

    add_executable(${PROJECT_NAME} MACOSX_BUNDLE
        src/main.cpp
        ${APP_ICON_FILE}
        ${CORE_SOURCES}
        ${LIBRARY_SOURCES}
        ${AUDIO_SOURCES}
        ${DSP_SOURCES}
        ${METADATA_SOURCES}
        ${RADIO_SOURCES}
        ${PLUGIN_SOURCES}
        ${PLATFORM_SOURCES}
        ${APPLE_MUSIC_SOURCES}
        ${TIDAL_SOURCES}
        ${WIDGET_SOURCES}
        ${UI_SOURCES}
        ${VIEW_SOURCES}
        ${RESOURCES}
        ${VST3_SDK_SOURCES}
        ${VST3_SDK_OBJCXX_SOURCES}
    )
else()
    add_executable(${PROJECT_NAME}
        src/main.cpp
        ${CORE_SOURCES}
        ${LIBRARY_SOURCES}
        ${AUDIO_SOURCES}
        ${DSP_SOURCES}
        ${METADATA_SOURCES}
        ${RADIO_SOURCES}
        ${PLUGIN_SOURCES}
        ${PLATFORM_SOURCES}
        ${WIDGET_SOURCES}
        ${UI_SOURCES}
        ${VIEW_SOURCES}
        ${RESOURCES}
        ${VST3_SDK_SOURCES}
    )
endif()

# VST3 SDK requires one of DEVELOPMENT, RELEASE, _DEBUG, or NDEBUG
target_compile_definitions(${PROJECT_NAME} PRIVATE
    $<$<CONFIG:Debug>:DEVELOPMENT=1>
    $<$<NOT:$<CONFIG:Debug>>:RELEASE=1>
)

# Include directories
target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/src/core
    ${CMAKE_CURRENT_SOURCE_DIR}/src/core/audio
    ${CMAKE_CURRENT_SOURCE_DIR}/src/core/dsp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/core/library
    ${CMAKE_CURRENT_SOURCE_DIR}/src/plugins
    ${CMAKE_CURRENT_SOURCE_DIR}/src/platform/macos
    ${CMAKE_CURRENT_SOURCE_DIR}/src/widgets
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ui
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ui/views
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ui/dialogs
    ${CMAKE_CURRENT_SOURCE_DIR}/src/metadata
    ${CMAKE_CURRENT_SOURCE_DIR}/src/radio
    ${CMAKE_CURRENT_SOURCE_DIR}/src/apple
    ${CMAKE_CURRENT_SOURCE_DIR}/src/tidal
    ${VST3_SDK_ROOT}
    ${CMAKE_CURRENT_SOURCE_DIR}/external/vst2sdk/include
    ${TAGLIB_INCLUDE_DIRS}
    ${CMAKE_CURRENT_BINARY_DIR}  # For generated SoranaFlow-Swift.h
)

# Link Qt6 libraries
target_link_libraries(${PROJECT_NAME} PRIVATE
    Qt6::Widgets
    Qt6::Svg
    Qt6::SvgWidgets
    Qt6::Sql
    Qt6::Network
    Qt6::WebEngineWidgets
    Qt6::WebChannel
    Qt6::Concurrent
    PkgConfig::AVFORMAT
    PkgConfig::AVCODEC
    PkgConfig::AVUTIL
    PkgConfig::SWRESAMPLE
    PkgConfig::TAGLIB
    PkgConfig::CHROMAPRINT
    PkgConfig::SOXR
    PkgConfig::EBUR128
)

# Link libmysofa if available (HRTF binaural audio)
if(MYSOFA_FOUND)
    if(MYSOFA_LIBRARIES)
        target_link_libraries(${PROJECT_NAME} PRIVATE ${MYSOFA_LIBRARIES})
        target_include_directories(${PROJECT_NAME} PRIVATE ${MYSOFA_INCLUDE_DIRS})
    else()
        target_link_libraries(${PROJECT_NAME} PRIVATE ${MYSOFA_LIBRARY})
        target_include_directories(${PROJECT_NAME} PRIVATE ${MYSOFA_INCLUDE_DIR})
    endif()
    target_compile_definitions(${PROJECT_NAME} PRIVATE HAVE_LIBMYSOFA)
endif()

# macOS specific settings
if(APPLE)
    target_link_libraries(${PROJECT_NAME} PRIVATE
        "-framework CoreAudio"
        "-framework AudioToolbox"
        "-framework CoreFoundation"
        "-framework Cocoa"
        "-framework AppKit"
        "-framework Accelerate"
        "-framework MusicKit"
        "-framework MediaPlayer"
    )

    # Link Sparkle framework
    if(EXISTS "${SPARKLE_PATH}")
        target_link_libraries(${PROJECT_NAME} PRIVATE "${SPARKLE_PATH}")
        target_include_directories(${PROJECT_NAME} PRIVATE
            "${SPARKLE_PATH}/Headers"
        )
    else()
        message(WARNING "Sparkle.framework not found at ${SPARKLE_PATH} — auto-update disabled")
    endif()

    # ── Swift MusicKit bridge ────────────────────────────────────────
    execute_process(
        COMMAND xcrun --show-sdk-path
        OUTPUT_VARIABLE MACOS_SDK_PATH
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    set(SWIFT_SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/src/apple/MusicKitSwiftBridge.swift)
    set(SWIFT_OBJECT ${CMAKE_CURRENT_BINARY_DIR}/MusicKitSwiftBridge.o)
    set(SWIFT_HEADER ${CMAKE_CURRENT_BINARY_DIR}/SoranaFlow-Swift.h)

    add_custom_command(
        OUTPUT ${SWIFT_OBJECT} ${SWIFT_HEADER}
        COMMAND xcrun swiftc
            -c ${SWIFT_SOURCE}
            -o ${SWIFT_OBJECT}
            -emit-objc-header-path ${SWIFT_HEADER}
            -module-name SoranaFlow
            -parse-as-library
            -target arm64-apple-macos13.0
            -sdk ${MACOS_SDK_PATH}
            -O
        DEPENDS ${SWIFT_SOURCE}
        COMMENT "Compiling Swift MusicKit bridge..."
    )
    add_custom_target(SwiftBridge DEPENDS ${SWIFT_OBJECT} ${SWIFT_HEADER})
    add_dependencies(${PROJECT_NAME} SwiftBridge)

    target_link_libraries(${PROJECT_NAME} PRIVATE ${SWIFT_OBJECT})
    # Swift runtime (built into macOS 12.2+)
    target_link_options(${PROJECT_NAME} PRIVATE
        -L${MACOS_SDK_PATH}/usr/lib/swift
        -L/usr/lib/swift
    )

    # Ensure @rpath resolves to bundled Frameworks directory
    set_target_properties(${PROJECT_NAME} PROPERTIES
        BUILD_RPATH "@executable_path/../Frameworks"
        INSTALL_RPATH "@executable_path/../Frameworks"
    )

    set_target_properties(${PROJECT_NAME} PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_GUI_IDENTIFIER "com.soranaflow.app"
        MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION}
        MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}
        MACOSX_BUNDLE_INFO_PLIST ${CMAKE_CURRENT_SOURCE_DIR}/packaging/macos/Info.plist
        MACOSX_BUNDLE_COPYRIGHT "Copyright © 2026 Haeseong Choi"
        MACOSX_BUNDLE_INFO_STRING "Sorana Flow - Audiophile Music Player"
    )

    # Auto-run macdeployqt, cleanup, strip, and codesign after build
    find_program(MACDEPLOYQT_EXECUTABLE macdeployqt
        HINTS "${Qt6_DIR}/../../../bin" /opt/homebrew/opt/qt/bin
    )
    if(MACDEPLOYQT_EXECUTABLE)
        set(APP_BUNDLE "${CMAKE_CURRENT_BINARY_DIR}/${PROJECT_NAME}.app")
        set(CLEANUP_SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/scripts/cleanup_bundle.sh")

        add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
            # Step 1: Clean old bundle contents
            COMMAND ${CMAKE_COMMAND} -E remove_directory "${APP_BUNDLE}/Contents/Frameworks"
            COMMAND ${CMAKE_COMMAND} -E remove_directory "${APP_BUNDLE}/Contents/PlugIns"
            # Step 2: Run macdeployqt
            COMMAND ${MACDEPLOYQT_EXECUTABLE} "${APP_BUNDLE}" -always-overwrite
            # Step 3: Remove unnecessary frameworks and libraries
            COMMAND bash "${CLEANUP_SCRIPT}" "${APP_BUNDLE}"
            COMMENT "Bundling and cleaning up..."
        )
        # Step 4: Strip debug symbols in Release mode
        if(CMAKE_BUILD_TYPE STREQUAL "Release")
            add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
                COMMAND strip -x "${APP_BUNDLE}/Contents/MacOS/${PROJECT_NAME}"
                COMMENT "Stripping debug symbols..."
            )
        endif()
        # Step 5: Copy Apple Music .p8 key into bundle (DEBUG ONLY)
        # In release builds, use -DMUSICKIT_DEVELOPER_TOKEN=... instead
        if(NOT MUSICKIT_DEVELOPER_TOKEN)
            set(P8_KEY_FILE "${CMAKE_CURRENT_SOURCE_DIR}/AuthKey_4GW6686CH4.p8")
            if(EXISTS "${P8_KEY_FILE}")
                add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
                    COMMAND ${CMAKE_COMMAND} -E make_directory "${APP_BUNDLE}/Contents/Resources"
                    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${P8_KEY_FILE}" "${APP_BUNDLE}/Contents/Resources/"
                    COMMENT "Copying Apple Music .p8 key to bundle..."
                )
            endif()
        endif()
        # Step 6: Embed Sparkle.framework into bundle
        if(EXISTS "${SPARKLE_PATH}")
            add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E rm -rf
                    "${APP_BUNDLE}/Contents/Frameworks/Sparkle.framework"
                COMMAND ditto
                    "${SPARKLE_PATH}"
                    "${APP_BUNDLE}/Contents/Frameworks/Sparkle.framework"
                COMMENT "Embedding Sparkle.framework (preserving symlinks)..."
            )
        endif()
        # Step 7: Re-sign after all modifications (must be last)
        set(ADHOC_SIGN_SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/scripts/adhoc_sign.sh")
        add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
            COMMAND bash "${ADHOC_SIGN_SCRIPT}" "${APP_BUNDLE}"
            COMMENT "Ad-hoc codesigning (use deploy.sh for Developer ID signing)..."
        )
    else()
        message(WARNING "macdeployqt not found — app bundle will not be self-contained")
    endif()
endif()
