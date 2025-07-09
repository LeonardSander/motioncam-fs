cmake_minimum_required(VERSION 3.16)

if(APPLE)
  set(CMAKE_OSX_DEPLOYMENT_TARGET "12.0" CACHE STRING "Minimum OS X deployment version")
endif()

project(MotionCamFuse VERSION 1.0 LANGUAGES CXX)

set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS Widgets Network)
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Widgets Network)

set(APP_NAME "MotionCam Fuse")
set(DEPENDENCIES_PATH deps)

# set(Boost_DEBUG 1)

set(MACOS_BUNDLE_ICON_FILE app_icon.icns)

set(PROJECT_SOURCES
        src/main.cpp
        src/mainwindow.cpp
        src/VirtualFileSystemImpl_MCRAW.cpp
        src/CameraMetadata.cpp
        src/CameraFrameMetadata.cpp
        src/AudioWriter.cpp
        src/Utils.cpp

        include/mainwindow.h
        include/Types.h
        include/IVirtualFileSystem.h
        include/IFuseFileSystem.h
        include/VirtualFileSystemImpl_MCRAW.h
        include/LRUCache.h
        include/AudioWriter.h
        include/Measure.h
        include/SingleApplication.h
        include/CameraMetadata.h
        include/CameraFrameMetadata.h
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

    find_library(projected-fs ProjectedFSLib)

    set(platform-specific ${projected-fs})
elseif(APPLE)
  list(APPEND PROJECT_SOURCES
      ${MACOS_BUNDLE_ICON_FILE}
      src/macos/FuseFileSystemImpl_MacOS.cpp
      include/macos/FuseFileSystemImpl_MacOS.h)

  find_library(FUSE_T_FRAMEWORK fuse_t)

  set(platform-specific ${FUSE_T_FRAMEWORK})

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

target_compile_definitions(${PROJECT_NAME} PRIVATE _FILE_OFFSET_BITS=64 FUSE_USE_VERSION=26)

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
    ${DEPENDENCIES_PATH}/motioncam-decoder/lib/RawData_Legacy.cpp)

target_include_directories(motioncam-decoder
    PUBLIC
    ${DEPENDENCIES_PATH}/bw64/include
    ${DEPENDENCIES_PATH}/motioncam-decoder/thirdparty
    ${DEPENDENCIES_PATH}/motioncam-decoder/lib/include
)

# Find the packages using vcpkg
find_package(spdlog CONFIG REQUIRED)
find_package(fmt CONFIG REQUIRED)  # Explicitly find fmt as well

# Add boost
set(Boost_USE_STATIC_LIBS        ON)
set(Boost_USE_DEBUG_LIBS        OFF)
set(Boost_USE_RELEASE_LIBS       ON)
set(Boost_USE_MULTITHREADED      ON)
set(Boost_USE_STATIC_RUNTIME    OFF)

find_package(Boost 1.86.0 REQUIRED COMPONENTS filesystem algorithm locale)
if(Boost_FOUND)
  include_directories(${Boost_INCLUDE_DIRS})
endif()

target_link_libraries(${PROJECT_NAME} PRIVATE
  Qt${QT_VERSION_MAJOR}::Widgets
  Qt${QT_VERSION_MAJOR}::Network
  ${Boost_FILESYSTEM_LIBRARY}
  spdlog::spdlog
  fmt::fmt
  motioncam-decoder
  ${platform-specific})

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
