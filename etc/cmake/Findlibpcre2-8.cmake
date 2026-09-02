# Findlibpcre2-8.cmake
#
# Compatibility shim for the vcpkg-provided BDE packages.
#
# Since bde 4.36 the vcpkg 'bde' port links against vcpkg's own 'pcre2' port
# and BDE's generated bdlConfig.cmake therefore calls
# find_dependency(libpcre2-8).  vcpkg's pcre2 port only ships a 'pcre2'
# package configuration (target pcre2::pcre2-8-static, which is what
# bdlTargets.cmake references), so plain find_package(libpcre2-8) fails.
#
# This module satisfies that lookup by locating the real pcre2 package, which
# also defines the pcre2::* imported targets bdl links to.  It is only ever
# reached through CMAKE_MODULE_PATH, i.e. when BDE is consumed from vcpkg.

include(FindPackageHandleStandardArgs)

find_package(pcre2 CONFIG QUIET)

# Report success when the pcre2 package (and its 8-bit target) was found.
find_package_handle_standard_args(libpcre2-8
    REQUIRED_VARS pcre2_FOUND
    VERSION_VAR pcre2_VERSION)
