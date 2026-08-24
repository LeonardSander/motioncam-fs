cmake_minimum_required(VERSION 3.16)

# CMake 4 prefers Boost's package config; distributions still commonly ship
# only the classic FindBoost metadata.
if(POLICY CMP0167)
  cmake_policy(SET CMP0167 OLD)
endif()

if(APPLE)
  option(MOTIONCAMFUSE_MACOS_X86_64 "Build x86_64 on macOS (Intel / Rosetta)" OFF)
  option(MOTIONCAMFUSE_MACOS_UNIVERSAL "Build universal macOS binary (x86_64 + arm64)" OFF)
  if(NOT DEFINED CMAKE_OSX_ARCHITECTURES)
    if(MOTIONCAMFUSE_MACOS_UNIVERSAL)
      set(CMAKE_OSX_ARCHITECTURES "x86_64;arm64" CACHE STRING "Build architectures for macOS" FORCE)
    elseif(MOTIONCAMFUSE_MACOS_X86_64)
      set(CMAKE_OSX_ARCHITECTURES "x86_64" CACHE STRING "Build architectures for macOS" FORCE)
    endif()
  endif()
  set(CMAKE_OSX_DEPLOYMENT_TARGET "12.0" CACHE STRING "Minimum OS X deployment version")
endif()

project(MotionCamFuse VERSION 1.0 LANGUAGES C CXX)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS Widgets Network Concurrent)
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Widgets Network Concurrent)

set(APP_NAME "MotionCam Fuse")
set(DEPENDENCIES_PATH deps)

# set(Boost_DEBUG 1)

set(MACOS_BUNDLE_ICON_FILE app_icon.icns)

set(PROJECT_SOURCES
        src/main.cpp
        src/mainwindow.cpp
        src/FrameTimingDialog.cpp
        src/settingsdialog.cpp
        src/VirtualFileSystemImpl.cpp
        src/VirtualFileSystemImpl_MCRAW.cpp
        src/VirtualFileSystemImpl_DirectLog.cpp
        src/VirtualFileSystemImpl_DNG.cpp
        src/DirectLogDecoder.cpp
        src/DNGDecoder.cpp
        src/CameraMetadata.cpp
        src/CameraFrameMetadata.cpp
        src/CalibrationData.cpp
        src/AudioWriter.cpp
        src/Utils.cpp
        src/IsoOverlay.cpp
        src/HigherCFADemosaic.cpp

        include/mainwindow.h
        include/FrameTimingDialog.h
        include/settingsdialog.h
        include/Types.h
        include/DataLevels.h
        include/IVirtualFileSystem.h
        include/IFuseFileSystem.h
        include/VirtualFileSystemImpl.h
        include/VirtualFileSystemImpl_MCRAW.h
        include/VirtualFileSystemImpl_DirectLog.h
        include/VirtualFileSystemImpl_DNG.h
        include/DirectLogDecoder.h
        include/DNGDecoder.h
        include/LRUCache.h
        include/AudioWriter.h
        include/Measure.h
        include/SingleApplication.h
        include/CameraMetadata.h
        include/CameraFrameMetadata.h
        include/CalibrationData.h
        include/Utils.h

        ui/mainwindow.ui
)

qt_add_resources(PROJECT_SOURCES
    resources.qrc
    qdarkstyle/dark/darkstyle.qrc)

if(WIN32)
    list(APPEND PROJECT_SOURCES
        resources/app.rc

        src/win/FuseFileSystemImpl_Win.cpp
        src/win/virtualizationInstance.cpp
        src/win/dirInfo.cpp

        include/win/dirInfo.h
        include/win/FuseFileSystemImpl_Win.h
        include/win/virtualizationInstance.h)

    # Visual Studio and the Windows SDK expose this through their normal
    # library search paths. CI may also provide it explicitly with
    # -Dprojected-fs=<path>.
    if(NOT projected-fs)
        find_library(projected-fs NAMES ProjectedFSLib)
    endif()
    
    if(NOT projected-fs)
        message(FATAL_ERROR "ProjectedFSLib not found. Install the Windows SDK and enable the Projected File System feature.")
    endif()

    set(platform-specific ${projected-fs})
elseif(APPLE)
  list(APPEND PROJECT_SOURCES
      ${MACOS_BUNDLE_ICON_FILE}
      src/CrashDebug.cpp
      src/macos/FuseFileSystemImpl_MacOS.cpp
      include/CrashDebug.h
      include/macos/FuseFileSystemImpl_MacOS.h)

  find_library(FUSE_T_FRAMEWORK fuse_t)
  if(NOT FUSE_T_FRAMEWORK)
    find_library(FUSE_LIBRARY fuse)
    if(FUSE_LIBRARY)
      set(FUSE_T_FRAMEWORK ${FUSE_LIBRARY})
    else()
      find_library(FUSE3_LIBRARY fuse3)
      if(FUSE3_LIBRARY)
        set(FUSE_T_FRAMEWORK ${FUSE3_LIBRARY})
      endif()
    endif()
  endif()
  if(NOT FUSE_T_FRAMEWORK)
    message(FATAL_ERROR "macFUSE not found. Install macFUSE or set FUSE_T_FRAMEWORK.")
  endif()
  find_library(AUDIO_TOOLBOX_FRAMEWORK AudioToolbox)
  find_library(CORE_MEDIA_FRAMEWORK CoreMedia)
  find_library(CORE_VIDEO_FRAMEWORK CoreVideo)
  find_library(SECURITY_FRAMEWORK Security)
  find_library(VIDEO_TOOLBOX_FRAMEWORK VideoToolbox)

  set(platform-specific
      ${FUSE_T_FRAMEWORK}
      ${AUDIO_TOOLBOX_FRAMEWORK}
      ${CORE_MEDIA_FRAMEWORK}
      ${CORE_VIDEO_FRAMEWORK}
      ${SECURITY_FRAMEWORK}
      ${VIDEO_TOOLBOX_FRAMEWORK})

elseif(UNIX)
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(FUSE3 REQUIRED IMPORTED_TARGET fuse3)

  list(APPEND PROJECT_SOURCES
      src/linux/FuseFileSystemImpl_Linux.cpp
      include/linux/FuseFileSystemImpl_Linux.h)

  set(platform-specific PkgConfig::FUSE3)

endif()

set(CMAKE_AUTOUIC_SEARCH_PATHS ui)

if(${QT_VERSION_MAJOR} GREATER_EQUAL 6)
    qt_add_executable(${PROJECT_NAME}
        MANUAL_FINALIZATION
        ${PROJECT_SOURCES}
    )
# Define target properties for Android with Qt 6 as:
#    set_property(TARGET ${PROJECT_NAME} APPEND PROPERTY QT_ANDROID_PACKAGE_SOURCE_DIR
#                 ${CMAKE_CURRENT_SOURCE_DIR}/android)
# For more information, see https://doc.qt.io/qt-6/qt-add-executable.html#target-creation
else()
    if(ANDROID)
        add_library(${PROJECT_NAME} SHARED
            ${PROJECT_SOURCES}
        )
# Define properties for Android with Qt 5 after find_package() calls as:
#    set(ANDROID_PACKAGE_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/android")
    else()
        add_executable(${PROJECT_NAME}
            ${PROJECT_SOURCES}
        )
    endif()
endif()

target_include_directories(${PROJECT_NAME} PRIVATE include)

target_compile_definitions(${PROJECT_NAME} PRIVATE _FILE_OFFSET_BITS=64)
if(APPLE)
    target_compile_definitions(${PROJECT_NAME} PRIVATE FUSE_USE_VERSION=26)
endif()

# Debug symbols configuration
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    if(MSVC AND NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        # MSVC with PDB symbols (not LLDB-compatible)
        target_compile_options(${PROJECT_NAME} PRIVATE /Zi /Od /MDd)
        target_link_options(${PROJECT_NAME} PRIVATE /DEBUG:FASTLINK)
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        # Clang/GCC with DWARF symbols (LLDB-compatible)
        target_compile_options(${PROJECT_NAME} PRIVATE -g -O0 -gdwarf-4 -fno-omit-frame-pointer)
        # Better debugging experience
        target_compile_options(${PROJECT_NAME} PRIVATE -fno-limit-debug-info)
    elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        target_compile_options(${PROJECT_NAME} PRIVATE -g -O0 -gdwarf-4 -fno-omit-frame-pointer)
    endif()
endif()

# # Debug configuration with sanitizers
# if(CMAKE_BUILD_TYPE STREQUAL "Debug")
#     target_compile_options(${PROJECT_NAME} PRIVATE
#         -fsanitize=address,undefined
#         -fsanitize-address-use-after-scope
#         -fno-sanitize-recover=all
#         -fstack-protector-strong
#         -g
#         -O0
#         -fno-omit-frame-pointer  # Better stack traces
#     )

#     target_link_options(${PROJECT_NAME} PRIVATE
#       -fsanitize=address,undefined
#       -fsanitize-address-use-after-scope
#       -fno-sanitize-recover=all
#       -fstack-protector-strong
#     )
# endif()

# Add motioncam decoder as static library
add_library(motioncam-decoder STATIC
    ${DEPENDENCIES_PATH}/motioncam-decoder/lib/Decoder.cpp
    ${DEPENDENCIES_PATH}/motioncam-decoder/lib/RawData.cpp
    ${DEPENDENCIES_PATH}/motioncam-decoder/lib/RawData_Legacy.cpp
    ${DEPENDENCIES_PATH}/motioncam-decoder/thirdparty/liblj92/lj92.c)

# The decoder uses std::memcpy/std::memset in legacy sources without always
# including <cstring> directly. Keep clean C++ builds independent of
# transitive includes without applying a C++ header to liblj92.c.
if(MSVC)
  set_source_files_properties(
    ${DEPENDENCIES_PATH}/motioncam-decoder/lib/Decoder.cpp
    ${DEPENDENCIES_PATH}/motioncam-decoder/lib/RawData.cpp
    ${DEPENDENCIES_PATH}/motioncam-decoder/lib/RawData_Legacy.cpp
    PROPERTIES COMPILE_OPTIONS /FIcstring)
else()
  target_compile_options(motioncam-decoder PRIVATE
    "$<$<COMPILE_LANGUAGE:CXX>:-include;cstring>")
endif()

target_include_directories(motioncam-decoder
    PUBLIC
    ${DEPENDENCIES_PATH}/bw64/include
    ${DEPENDENCIES_PATH}/motioncam-decoder/thirdparty
    ${DEPENDENCIES_PATH}/motioncam-decoder/lib/include
)

# Find the packages using vcpkg
find_package(spdlog CONFIG REQUIRED)
find_package(fmt CONFIG REQUIRED)  # Explicitly find fmt as well

if(APPLE)
  if(TARGET spdlog::spdlog_header_only)
    set(MOTIONCAM_SPDLOG_TARGET spdlog::spdlog_header_only)
  else()
    set(MOTIONCAM_SPDLOG_TARGET spdlog::spdlog)
  endif()

  if(TARGET fmt::fmt-header-only)
    set(MOTIONCAM_FMT_TARGET fmt::fmt-header-only)
  else()
    set(MOTIONCAM_FMT_TARGET fmt::fmt)
  endif()
else()
  set(MOTIONCAM_SPDLOG_TARGET spdlog::spdlog)
  set(MOTIONCAM_FMT_TARGET fmt::fmt)
endif()

# Link these directly to the decoder to keep compile definitions and fmt ABI
# consistent in every translation unit.
target_link_libraries(motioncam-decoder PRIVATE
  ${MOTIONCAM_SPDLOG_TARGET}
  ${MOTIONCAM_FMT_TARGET})

if(MSVC)
  target_compile_options(motioncam-decoder PRIVATE /utf-8)
endif()

# JPEG XL is the DNG 1.7 compression backend. Prefer pkg-config (used by
# distro packages and vcpkg), with a conventional library fallback.
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(JXL QUIET IMPORTED_TARGET libjxl)
endif()
if(TARGET PkgConfig::JXL)
    set(JXL_TARGET PkgConfig::JXL)
else()
    find_path(JXL_INCLUDE_DIR jxl/encode.h REQUIRED)
    find_library(JXL_LIBRARY NAMES jxl REQUIRED)
    add_library(MotionCamJXL UNKNOWN IMPORTED)
    set_target_properties(MotionCamJXL PROPERTIES
        IMPORTED_LOCATION "${JXL_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${JXL_INCLUDE_DIR}")
    set(JXL_TARGET MotionCamJXL)
endif()

# Find FFmpeg - try manual path first, then pkg-config, then vcpkg
set(FFMPEG_ROOT "" CACHE PATH "Path to FFmpeg installation (e.g., C:/ffmpeg)")

if(FFMPEG_ROOT)
    # Manual FFmpeg path specified
    set(FFMPEG_INCLUDE_DIRS "${FFMPEG_ROOT}/include")
    set(FFMPEG_LIBRARY_DIRS "${FFMPEG_ROOT}/lib")
    
    find_library(AVFORMAT_LIBRARY avformat PATHS ${FFMPEG_LIBRARY_DIRS} NO_DEFAULT_PATH)
    find_library(AVCODEC_LIBRARY avcodec PATHS ${FFMPEG_LIBRARY_DIRS} NO_DEFAULT_PATH)
    find_library(SWRESAMPLE_LIBRARY swresample PATHS ${FFMPEG_LIBRARY_DIRS} NO_DEFAULT_PATH)
    find_library(SWSCALE_LIBRARY swscale PATHS ${FFMPEG_LIBRARY_DIRS} NO_DEFAULT_PATH)
    find_library(AVUTIL_LIBRARY avutil PATHS ${FFMPEG_LIBRARY_DIRS} NO_DEFAULT_PATH)

    if(AVFORMAT_LIBRARY AND AVCODEC_LIBRARY AND SWRESAMPLE_LIBRARY AND SWSCALE_LIBRARY AND AVUTIL_LIBRARY)
        set(FFMPEG_FOUND TRUE)
        set(FFMPEG_LIBRARIES ${AVFORMAT_LIBRARY} ${AVCODEC_LIBRARY} ${SWRESAMPLE_LIBRARY} ${SWSCALE_LIBRARY} ${AVUTIL_LIBRARY})
        include_directories(${FFMPEG_INCLUDE_DIRS})
        link_directories(${FFMPEG_LIBRARY_DIRS})
        message(STATUS "Found FFmpeg at: ${FFMPEG_ROOT}")
    endif()
endif()

if(NOT FFMPEG_FOUND AND NOT WIN32)
    # Skip pkg-config on Windows as it doesn't work well with vcpkg + Clang
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(FFMPEG QUIET IMPORTED_TARGET
            libavformat
            libavcodec
            libswresample
            libswscale
            libavutil
        )
        if(FFMPEG_FOUND)
            set(FFMPEG_LIBRARIES PkgConfig::FFMPEG)
        endif()
    endif()
endif()

# Fallback to vcpkg if nothing else worked
if(NOT FFMPEG_FOUND)
    find_package(ffmpeg CONFIG QUIET)
    if(ffmpeg_FOUND)
        set(FFMPEG_FOUND TRUE)
        set(FFMPEG_LIBRARIES ffmpeg::avformat ffmpeg::avcodec ffmpeg::swresample ffmpeg::swscale ffmpeg::avutil)
    endif()
endif()

# Last resort: manually find libraries in vcpkg directory
if(NOT FFMPEG_FOUND AND WIN32)
    set(VCPKG_INSTALLED_DIR "${CMAKE_BINARY_DIR}/vcpkg_installed/x64-windows")
    if(CMAKE_BUILD_TYPE STREQUAL "Debug")
        set(VCPKG_LIB_DIR "${VCPKG_INSTALLED_DIR}/debug/lib")
    else()
        set(VCPKG_LIB_DIR "${VCPKG_INSTALLED_DIR}/lib")
    endif()
    
    # Clear cached values to force re-search
    unset(AVFORMAT_LIBRARY CACHE)
    unset(AVCODEC_LIBRARY CACHE)
    unset(SWRESAMPLE_LIBRARY CACHE)
    unset(SWSCALE_LIBRARY CACHE)
    unset(AVUTIL_LIBRARY CACHE)
    
    find_library(AVFORMAT_LIBRARY avformat PATHS ${VCPKG_LIB_DIR} NO_DEFAULT_PATH)
    find_library(AVCODEC_LIBRARY avcodec PATHS ${VCPKG_LIB_DIR} NO_DEFAULT_PATH)
    find_library(SWRESAMPLE_LIBRARY swresample PATHS ${VCPKG_LIB_DIR} NO_DEFAULT_PATH)
    find_library(SWSCALE_LIBRARY swscale PATHS ${VCPKG_LIB_DIR} NO_DEFAULT_PATH)
    find_library(AVUTIL_LIBRARY avutil PATHS ${VCPKG_LIB_DIR} NO_DEFAULT_PATH)
    
    if(AVFORMAT_LIBRARY AND AVCODEC_LIBRARY AND SWRESAMPLE_LIBRARY AND SWSCALE_LIBRARY AND AVUTIL_LIBRARY)
        set(FFMPEG_FOUND TRUE)
        set(FFMPEG_LIBRARIES ${AVFORMAT_LIBRARY} ${AVCODEC_LIBRARY} ${SWRESAMPLE_LIBRARY} ${SWSCALE_LIBRARY} ${AVUTIL_LIBRARY})
        message(STATUS "Found FFmpeg in vcpkg: ${VCPKG_LIB_DIR}")
    endif()
endif()

if(NOT FFMPEG_FOUND)
    message(FATAL_ERROR "FFmpeg not found. Please install via vcpkg or set FFMPEG_ROOT to pre-built FFmpeg location.")
endif()

# Add boost
set(Boost_USE_STATIC_LIBS        ON)
set(Boost_USE_DEBUG_LIBS        OFF)
set(Boost_USE_RELEASE_LIBS       ON)
set(Boost_USE_MULTITHREADED      ON)
set(Boost_USE_STATIC_RUNTIME    OFF)

find_package(Boost 1.74 REQUIRED COMPONENTS filesystem locale regex)
if(Boost_FOUND)
  include_directories(${Boost_INCLUDE_DIRS})
endif()

# Use the same pinned thread-pool API on every platform. In particular, do not
# pick up the newer templated BS::thread_pool API from vcpkg on Windows/macOS.
include(FetchContent)
FetchContent_Declare(bs_thread_pool
    GIT_REPOSITORY https://github.com/bshoshany/thread-pool.git
    GIT_TAG v4.1.0
    GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(bs_thread_pool)
target_include_directories(${PROJECT_NAME} BEFORE PRIVATE
    ${bs_thread_pool_SOURCE_DIR}/include)

target_link_libraries(${PROJECT_NAME} PRIVATE
  Qt${QT_VERSION_MAJOR}::Widgets
  Qt${QT_VERSION_MAJOR}::Network
  Qt${QT_VERSION_MAJOR}::Concurrent
  Boost::filesystem
  Boost::locale
  Boost::regex
  ${MOTIONCAM_SPDLOG_TARGET}
  ${MOTIONCAM_FMT_TARGET}
  motioncam-decoder
  ${JXL_TARGET}
  ${FFMPEG_LIBRARIES}
  ${platform-specific})
target_compile_definitions(${PROJECT_NAME} PRIVATE TINY_DNG_ENABLE_JPEG_XL)
add_compile_definitions(BS_THREAD_POOL_ENABLE_PRIORITY)

if(WIN32)
  target_link_libraries(${PROJECT_NAME} PRIVATE delayimp)
  target_link_options(${PROJECT_NAME} PRIVATE "/DELAYLOAD:projectedfslib.dll")
  add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      "${CMAKE_CURRENT_SOURCE_DIR}/Enable_ProjFS.bat"
      "$<TARGET_FILE_DIR:${PROJECT_NAME}>/Enable_ProjFS.bat"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      "${CMAKE_CURRENT_SOURCE_DIR}/Disable_ProjFS.bat"
      "$<TARGET_FILE_DIR:${PROJECT_NAME}>/Disable_ProjFS.bat"
  )
endif()

set(MACOSX_BUNDLE_GUI_IDENTIFIER "com.motioncam.fuse")

# Qt for iOS sets MACOSX_BUNDLE_GUI_IDENTIFIER automatically since Qt 6.1.
# If you are developing for iOS or macOS you should consider setting an
# explicit, fixed bundle identifier manually though.
if(${QT_VERSION} VERSION_LESS 6.1.0)
  set(BUNDLE_ID_OPTION MACOSX_BUNDLE_GUI_IDENTIFIER com.motioncam.fuse)
endif()

set_target_properties(${PROJECT_NAME} PROPERTIES
    ${BUNDLE_ID_OPTION}
    MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION}
    MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}
    MACOSX_BUNDLE_ICON_FILE ${MACOS_BUNDLE_ICON_FILE}
    MACOSX_BUNDLE_BUNDLE_NAME ${APP_NAME}
    MACOSX_BUNDLE TRUE
    WIN32_EXECUTABLE TRUE
)

set_source_files_properties(${MACOS_BUNDLE_ICON_FILE} PROPERTIES
    MACOSX_PACKAGE_LOCATION "Resources"
)

include(GNUInstallDirs)
install(TARGETS ${PROJECT_NAME}
    BUNDLE DESTINATION .
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

include(CTest)
if(BUILD_TESTING)
    add_executable(motioncam-demosaic-test
        tests/demosaic_test.cpp
        src/HigherCFADemosaic.cpp)
    target_include_directories(motioncam-demosaic-test PRIVATE
        include
        deps/motioncam-decoder/thirdparty)
    add_test(NAME motioncam-demosaic-test COMMAND motioncam-demosaic-test)

    add_executable(motioncam-core-tests
        tests/core_tests.cpp
        src/CalibrationData.cpp)
    target_include_directories(motioncam-core-tests PRIVATE
        include
        deps/motioncam-decoder/thirdparty)
    target_link_libraries(motioncam-core-tests PRIVATE spdlog::spdlog)
    add_test(NAME motioncam-core-tests COMMAND motioncam-core-tests)

    add_executable(motioncam-lossless-jpeg-test
        tests/lossless_jpeg_test.cpp
        src/IsoOverlay.cpp
        src/DNGDecoder.cpp
        src/HigherCFADemosaic.cpp
        deps/motioncam-decoder/thirdparty/liblj92/lj92.c)
    target_include_directories(motioncam-lossless-jpeg-test PRIVATE
        deps/motioncam-decoder/thirdparty
        deps/motioncam-decoder/thirdparty/liblj92)
    target_include_directories(motioncam-lossless-jpeg-test PRIVATE include)
    target_link_libraries(motioncam-lossless-jpeg-test PRIVATE
        Boost::filesystem Boost::regex spdlog::spdlog ${JXL_TARGET})
    add_test(NAME motioncam-lossless-jpeg-test
        COMMAND motioncam-lossless-jpeg-test)

    add_executable(motioncam-jpeg-xl-test
        tests/jpeg_xl_test.cpp
        src/IsoOverlay.cpp
        src/DNGDecoder.cpp
        src/HigherCFADemosaic.cpp
        deps/motioncam-decoder/thirdparty/liblj92/lj92.c)
    target_include_directories(motioncam-jpeg-xl-test PRIVATE
        deps/motioncam-decoder/thirdparty)
    target_include_directories(motioncam-jpeg-xl-test PRIVATE include)
    target_link_libraries(motioncam-jpeg-xl-test PRIVATE
        ${JXL_TARGET} Boost::filesystem Boost::regex spdlog::spdlog)
    target_compile_definitions(motioncam-jpeg-xl-test PRIVATE TINY_DNG_ENABLE_JPEG_XL)
    add_test(NAME motioncam-jpeg-xl-test COMMAND motioncam-jpeg-xl-test)

    add_executable(motioncam-rife-fake tests/rife_fake.cpp)
    add_executable(motioncam-finalize-interpolation-test
        tests/finalize_interpolation_test.cpp
        src/IsoOverlay.cpp
        src/VirtualFileSystemImpl.cpp
        src/CalibrationData.cpp
        src/DNGDecoder.cpp
        src/HigherCFADemosaic.cpp
        deps/motioncam-decoder/thirdparty/liblj92/lj92.c)
    target_include_directories(motioncam-finalize-interpolation-test PRIVATE
        ${bs_thread_pool_SOURCE_DIR}/include
        include deps/motioncam-decoder/thirdparty deps/motioncam-decoder/thirdparty/liblj92)
    target_link_libraries(motioncam-finalize-interpolation-test PRIVATE
        Qt${QT_VERSION_MAJOR}::Widgets ${JXL_TARGET} Boost::filesystem Boost::regex
        spdlog::spdlog motioncam-decoder)
    target_compile_definitions(motioncam-finalize-interpolation-test PRIVATE
        TINY_DNG_ENABLE_JPEG_XL
        RIFE_FAKE_EXECUTABLE="$<TARGET_FILE:motioncam-rife-fake>")
    add_dependencies(motioncam-finalize-interpolation-test motioncam-rife-fake)
    add_test(NAME motioncam-finalize-interpolation-test
        COMMAND motioncam-finalize-interpolation-test)

    # These standalone tests intentionally use assert() for both execution and
    # validation. Keep it enabled in Release builds so the tested operations
    # are not compiled out.
    foreach(test_target motioncam-demosaic-test motioncam-core-tests motioncam-lossless-jpeg-test motioncam-jpeg-xl-test motioncam-finalize-interpolation-test)
        if(MSVC)
            target_compile_options(${test_target} PRIVATE /UNDEBUG)
        else()
            target_compile_options(${test_target} PRIVATE -UNDEBUG)
        endif()
    endforeach()
endif()

if(QT_VERSION_MAJOR EQUAL 6)
    qt_finalize_executable(${PROJECT_NAME})
    
    # Deploy Qt dependencies using Qt's deployment tool
    if(WIN32)
        # Get Qt installation directory
        get_target_property(QT_QMAKE_EXECUTABLE Qt6::qmake IMPORTED_LOCATION)
        get_filename_component(QT_BIN_DIR "${QT_QMAKE_EXECUTABLE}" DIRECTORY)
        
        # Use windeployqt to copy all required Qt dependencies
        add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
            COMMAND "${QT_BIN_DIR}/windeployqt.exe"
                --debug
                --no-translations
                --no-system-d3d-compiler
                --no-opengl-sw
                --no-compiler-runtime
                --verbose 2
                $<TARGET_FILE:${PROJECT_NAME}>
        )
        add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
            COMMAND "${QT_BIN_DIR}/windeployqt.exe"
                --no-translations
                --no-system-d3d-compiler
                --no-opengl-sw
                --no-compiler-runtime
                --verbose 2
                $<TARGET_FILE:${PROJECT_NAME}>
        )       # add both release and debug qt dlls because im lazy
    endif()
endif()
