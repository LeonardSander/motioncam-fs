# Distribution dependencies: never build a second Debug configuration.
set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)
# Qt opens the Vulkan loader dynamically. A static copy in libplacebo would
# create a second loader with incompatible instance/device dispatch state.
if(PORT STREQUAL "vulkan-loader")
    set(VCPKG_LIBRARY_LINKAGE dynamic)
endif()
set(VCPKG_BUILD_TYPE release)
set(VCPKG_CMAKE_SYSTEM_NAME Linux)
