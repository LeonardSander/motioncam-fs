vcpkg_check_linkage(ONLY_STATIC_LIBRARY)

vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO haasn/libplacebo
    REF v7.360.1
    SHA512 209b1713cff34f06149af16fb3ea52e3662a566ef5df6b29811ad295aa8cb6388f827a93fc8e0eed1a72f35b3b3aae835520c933079e706a51d11136a8128799
    HEAD_REF master
)

set(VULKAN_REGISTRY "${CURRENT_INSTALLED_DIR}/share/vulkan/registry/vk.xml")
if(NOT EXISTS "${VULKAN_REGISTRY}")
    message(FATAL_ERROR "Vulkan registry is required at ${VULKAN_REGISTRY}")
endif()

# Meson otherwise sees the build machine's shaderc/vulkan .pc files and
# silently probes headers and libraries outside the selected target triplet.
if(CMAKE_HOST_WIN32)
    set(pkgconfig_separator ";")
else()
    set(pkgconfig_separator ":")
endif()
set(ENV{PKG_CONFIG_LIBDIR}
    "${CURRENT_INSTALLED_DIR}/lib/pkgconfig${pkgconfig_separator}${CURRENT_INSTALLED_DIR}/debug/lib/pkgconfig${pkgconfig_separator}${CURRENT_INSTALLED_DIR}/share/pkgconfig")
set(ENV{PKG_CONFIG_PATH} "")

# Load Vulkan explicitly on Windows so a machine without a Vulkan driver can
# still start the application and use its CPU fallback.
if(VCPKG_TARGET_IS_WINDOWS)
    set(vulkan_entrypoint -Dvk-proc-addr=disabled)
else()
    set(vulkan_entrypoint -Dvk-proc-addr=auto)
endif()

vcpkg_configure_meson(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        -Ddefault_library=static
        -Dvulkan=enabled
        ${vulkan_entrypoint}
        -Dvulkan-registry=${VULKAN_REGISTRY}
        -Dshaderc=enabled
        -Dglslang=disabled
        -Dopengl=disabled
        -Dd3d11=disabled
        -Dlcms=disabled
        -Ddovi=disabled
        -Dlibdovi=disabled
        -Dunwind=disabled
        -Dxxhash=disabled
        -Ddemos=false
        -Dtests=false
)
vcpkg_install_meson()
vcpkg_fixup_pkgconfig()
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")
vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
