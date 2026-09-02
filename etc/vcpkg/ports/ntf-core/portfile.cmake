# Overlay port for ntf-core.
#
# The upstream vcpkg port is stuck on ntf-core 2.5.4 and is marked as failing
# in vcpkg's own CI (scripts/ci.baseline.txt) because ntf-core, when installed
# into an empty prefix, emits lower-case CMake metadata files
# (nts-targets-release.cmake) while the portfile expects the PascalCase
# variants (nts-Targets-release.cmake).
#
# This overlay:
#   * pins ntf-core to 2.6.12, the version used by the non-vcpkg build paths
#     (see docker/build_deps.sh and .github/workflows/dependencies.yaml);
#   * asks ntf-core to emit PascalCase CMake metadata so the layout fix-ups
#     below can find the generated files.

vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO bloomberg/ntf-core
    REF "${VERSION}"
    SHA512 ee5bb0558c02bb6cef09c210ff264ddd2e7cff1573cb6ba591908968ab950029d8822ba682502d96bceb53881cd56e849ad80de38c1b02296a196d39424c32ca
    HEAD_REF main
    # vcpkg always installs libraries under <prefix>/lib; stop ntf-core from
    # selecting lib64 on 64-bit Linux.
    PATCHES dont-use-lib64.patch
)

# ntf-core requires debugger information for dev tooling purposes, so we just
# fake it. NTF_CONFIGURE_WITH_CMAKE_METADATA_PASCAL_CASE forces the
# <target>Config.cmake / <target>Targets-<cfg>.cmake naming scheme regardless
# of whether a previous ntf-core install exists in the prefix.
#
# Compression/TLS drivers mirror docker/build_deps.sh (zlib + OpenSSL on,
# zstd/lz4 off); ntf-core 2.6.x enables all of them by default and fails to
# configure when the corresponding pkg-config module is missing.
#
# ntf-core looks its BDE dependencies up with find_package(CONFIG) first and
# only falls back to pkg-config (exporting PkgConfig::<dep> link targets that
# consumers cannot resolve) when that fails.  vcpkg's bde >= 4.36 config files
# call find_dependency(libpcre2-8), a package vcpkg's pcre2 port does not
# provide, so expose BlazingMQ's Findlibpcre2-8.cmake shim (etc/cmake) via
# CMAKE_MODULE_PATH to keep the CMake lookup path working.
#
# CMAKE_CXX_STANDARD is pinned to 23 so ntf-core is ABI-compatible with the
# overlay bde port (also C++23) and with BlazingMQ's vcpkg preset.
vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        "-DCMAKE_MODULE_PATH=${CMAKE_CURRENT_LIST_DIR}/../../../cmake"
        "-DCMAKE_CXX_STANDARD=23"
        "-DNTF_BUILD_WITH_USAGE_EXAMPLES=0"
        "-DNTF_TOOLCHAIN_DEBUGGER_PATH=NOT-FOUND"
        "-DNTF_CONFIGURE_WITH_CMAKE_METADATA_PASCAL_CASE=ON"
        "-DNTF_CONFIGURE_WITH_ZLIB=1"
        "-DNTF_CONFIGURE_WITH_OPENSSL=1"
        "-DNTF_CONFIGURE_WITH_ZSTD=0"
        "-DNTF_CONFIGURE_WITH_LZ4=0"
        -DNTF_BUILD_SYSTEM=ON
)

vcpkg_cmake_build()

vcpkg_cmake_install()

# ntf-core installs into a UFID-specific sub-directory (e.g. lib/opt_exc_mt)
# and bakes that sub-directory into the pkg-config and CMake target files.
# Strip the UFID component so the layout matches what vcpkg expects.
function(fix_pkgconfig_ufid lib_dir ufid pc_name)
    vcpkg_replace_string("${CURRENT_PACKAGES_DIR}/${lib_dir}/pkgconfig/${pc_name}.pc" "/${ufid}" "")
    if ("${ufid}" MATCHES opt)
        set(build_mode "release")
    else()
        set(build_mode "debug")
    endif()

    vcpkg_replace_string("${CURRENT_PACKAGES_DIR}/${lib_dir}/cmake/${pc_name}/${pc_name}Targets-${build_mode}.cmake" "/${ufid}" "")
endfunction()

# Move <lib_dir>/<ufid>/* up to <lib_dir>/* and drop everything else that was
# installed directly under <lib_dir> (alias UFID directories and libraries).
function(fix_install_dir lib_dir ufid)
    message(STATUS "Fixing ufid layout for ${CURRENT_PACKAGES_DIR}/${lib_dir}/${ufid}")
    file(RENAME "${CURRENT_PACKAGES_DIR}/${lib_dir}/${ufid}" "${CURRENT_PACKAGES_DIR}/tmp")
    file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/${lib_dir}")
    file(RENAME "${CURRENT_PACKAGES_DIR}/tmp" "${CURRENT_PACKAGES_DIR}/${lib_dir}")

    fix_pkgconfig_ufid("${lib_dir}" "${ufid}" "nts")
    fix_pkgconfig_ufid("${lib_dir}" "${ufid}" "ntc")
endfunction()

fix_install_dir("lib" "opt_exc_mt")
fix_install_dir("debug/lib" "dbg_exc_mt")

# Relocate the CMake package configs to share/nts and share/ntc.
vcpkg_cmake_config_fixup(CONFIG_PATH "lib/cmake" PACKAGE_NAME nts)
file(RENAME "${CURRENT_PACKAGES_DIR}/share/nts" "${CURRENT_PACKAGES_DIR}/share/nts_original")
file(RENAME "${CURRENT_PACKAGES_DIR}/share/nts_original/ntc" "${CURRENT_PACKAGES_DIR}/share/ntc")
file(RENAME "${CURRENT_PACKAGES_DIR}/share/nts_original/nts" "${CURRENT_PACKAGES_DIR}/share/nts")
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/share/nts_original")

# vcpkg's zlib port ships no zlibConfig.cmake, so ntf-core can only find zlib
# through pkg-config and exports a PkgConfig::zlib link dependency plus a
# find_dependency(zlib) call, neither of which exists in a consuming project.
# Rewrite both to the canonical FindZLIB names (ZLIB / ZLIB::ZLIB), which
# vcpkg's toolchain resolves to the installed zlib.
foreach(pkg IN ITEMS nts ntc)
    vcpkg_replace_string("${CURRENT_PACKAGES_DIR}/share/${pkg}/${pkg}Config.cmake"
        "find_dependency(zlib " "find_dependency(ZLIB ")
    vcpkg_replace_string("${CURRENT_PACKAGES_DIR}/share/${pkg}/${pkg}Targets.cmake"
        "PkgConfig::zlib" "ZLIB::ZLIB")
endforeach()

# Handle copyright
vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
vcpkg_fixup_pkgconfig()

# Usage
file(INSTALL "${CMAKE_CURRENT_LIST_DIR}/usage" DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}")

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")
