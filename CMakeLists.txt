cmake_minimum_required(VERSION 3.20)
# The single source of truth for the version. The Debian changelog is
# checked against it by tests/packaging/test_deb.sh, so the two cannot
# drift apart unnoticed.
project(comlink VERSION 1.0.0 LANGUAGES C CXX)

# Which build this is, which matters more than the version on a tool
# that gets rebuilt twenty times in an afternoon: every package is
# 0.1.0-1, so without the commit there is no way to tell an installed
# build from a newer one short of hashing files.
#
# Read at configure time. It can therefore be stale if commits land
# without reconfiguring, which is why the About window labels it as
# the configured commit rather than claiming to be authoritative.
find_package(Git QUIET)
set(SP_GIT_COMMIT "unknown")
if(Git_FOUND AND EXISTS "${CMAKE_SOURCE_DIR}/.git")
    execute_process(
        COMMAND ${GIT_EXECUTABLE} rev-parse --short HEAD
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        OUTPUT_VARIABLE SP_GIT_COMMIT
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)
    execute_process(
        COMMAND ${GIT_EXECUTABLE} status --porcelain --untracked-files=no
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        OUTPUT_VARIABLE SP_GIT_DIRTY
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)
    if(NOT SP_GIT_DIRTY STREQUAL "")
        # A build from a modified tree is not the commit it names, and
        # saying so is the whole point of recording it.
        set(SP_GIT_COMMIT "${SP_GIT_COMMIT}+dirty")
    endif()
endif()
string(TIMESTAMP SP_BUILD_DATE "%Y-%m-%d" UTC)
message(STATUS "comlink ${PROJECT_VERSION} (${SP_GIT_COMMIT}, ${SP_BUILD_DATE})")

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# -Wstrict-prototypes is C-only, and Qt's headers are not built to the
# core's warning level, so the strict set is scoped to C.
add_compile_options(
    # -Wformat-security is not in -Wall or -Wextra, and the package
    # build turns it on through hardening=+all. Without it here, a
    # non-literal format string compiles clean on a developer's
    # machine and fails only in the container -- which is how one
    # reached a release build.
    "$<$<COMPILE_LANGUAGE:C>:-Wall;-Wextra;-Werror;-Wshadow;-Wstrict-prototypes;-Wformat-security>"
    "$<$<COMPILE_LANGUAGE:CXX>:-Wall;-Wextra>")

# The Debian build hardens with _FORTIFY_SOURCE, which is what marks
# fread and friends warn_unused_result. Without it an unchecked read
# compiles clean here and fails only in the package build, which has
# now happened twice. Optimised builds only: the macro needs
# optimisation and warns without it, so a Debug build is left alone.
add_compile_options(
    "$<$<AND:$<COMPILE_LANGUAGE:C,CXX>,$<NOT:$<CONFIG:Debug>>>:-U_FORTIFY_SOURCE;-D_FORTIFY_SOURCE=2>")

# The install tree resolves the library from a standard path, so no
# rpath is embedded and no build-machine path leaks into the package.
set(CMAKE_SKIP_INSTALL_RPATH TRUE)

find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)

# pjproject is vendored and built out of tree by scripts/build-pjproject.sh.
# It installs static archives, so the STATIC_ variables are required: the
# non-static link line omits Libs.private and fails on fmod.
# A cache variable, because the Android cross-build passes a different
# one. The default is what this line hardcoded before, so every
# existing invocation behaves identically.
#
# Not named PJ_PREFIX, which is what it was: pkg_check_modules(PJ ...)
# below defines <prefix>_PREFIX from the .pc file's own prefix= and
# silently overwrites it. On Debian the two values are identical, so
# the collision never showed; a build whose dependency prefix and
# pjproject prefix differ would have found it the hard way.
# ANDROID_ABI is set by the NDK's toolchain file, so a cross-build
# finds its own prefix without being told. Gradle drives this same
# CMakeLists per ABI and has no clean way to vary an argument by ABI.
if(ANDROID_ABI)
    set(COMLINK_DEFAULT_PREFIX
        "${CMAKE_SOURCE_DIR}/third_party/prefix-android-${ANDROID_ABI}")
else()
    set(COMLINK_DEFAULT_PREFIX "${CMAKE_SOURCE_DIR}/third_party/prefix-debian")
endif()
set(COMLINK_DEPS_PREFIX "${COMLINK_DEFAULT_PREFIX}"
    CACHE PATH "Prefix holding pjproject and its dependencies")

if(ANDROID_ABI)
    # LIBDIR, not PATH: PATH only prepends, so the host's .pc files
    # stay reachable and a missing Android library is answered by the
    # host's copy. Emptying the search list to one directory is what
    # makes a missing dependency fail.
    #
    # Set here rather than only in scripts/build-core-android.sh
    # because Gradle drives this same CMakeLists and exports no such
    # environment. SYSROOT_DIR is pinned empty for the same reason it
    # is there: the prefix's .pc files hold absolute paths that are
    # already right, including one into the NDK sysroot.
    set(ENV{PKG_CONFIG_LIBDIR} "${COMLINK_DEPS_PREFIX}/lib/pkgconfig")
    set(ENV{PKG_CONFIG_SYSROOT_DIR} "")
else()
    set(ENV{PKG_CONFIG_PATH} "${COMLINK_DEPS_PREFIX}/lib/pkgconfig:$ENV{PKG_CONFIG_PATH}")
endif()
pkg_check_modules(SQLITE REQUIRED sqlite3)
pkg_check_modules(PJ REQUIRED libpjproject)
# Opus recordings are OggOpus; libogg does the page framing. libopus
# itself is already linked in via PJ_STATIC_LDFLAGS, for the SIP codec.
pkg_check_modules(OGG REQUIRED ogg)
if(NOT PJ_STATIC_LDFLAGS)
    message(FATAL_ERROR "libpjproject found but static link flags are empty")
endif()

# Android builds the core and nothing else: the CLI is a desktop
# development tool and the Qt frontend has no Qt to find there. The
# core's own source list stays the single one either way.
option(COMLINK_CORE_ONLY "Build libcomlink_core only, without the frontends" OFF)

# The QML frontend is the only one: one tree, rendered by Qt on Debian
# and on Android, which is what the whole port was for.
enable_testing()
add_subdirectory(core)

# COMLINK_CORE_ONLY, which the core's own test runs use, builds no
# frontend at all.
if(ANDROID AND NOT COMLINK_CORE_ONLY)
    add_subdirectory(frontend/quick)
endif()

# Desktop only. The CLI and every test below it are desktop things --
# none of them would configure on Android, let alone build.
if(NOT ANDROID AND NOT COMLINK_CORE_ONLY)
add_subdirectory(frontend/cli)
add_subdirectory(frontend/quick)

# The desktop entry and the icons belong to the application, not to
# whichever frontend is currently it.
include(GNUInstallDirs)
install(FILES ${CMAKE_SOURCE_DIR}/packaging/debian/comlink.desktop
        DESTINATION ${CMAKE_INSTALL_DATADIR}/applications
        RENAME comlink.desktop)

# One file per size under hicolor, which is what a desktop expects to
# find from "Icon=comlink" in the desktop entry.
foreach(size 16 22 24 32 48 64 128 256 512)
    install(FILES ${CMAKE_SOURCE_DIR}/packaging/icons/comlink-${size}.png
            DESTINATION
                ${CMAKE_INSTALL_DATADIR}/icons/hicolor/${size}x${size}/apps
            RENAME comlink.png)
endforeach()

# The same ALSA override the core tests use, for the same reason:
# these drive comlink-cli, which calls pjsua_init. See
# core/CMakeLists.txt and docs/known-issues.md.
#
# Copied into the build tree rather than used in place: the tests here
# start the whole application, and an application started with
# XDG_CONFIG_HOME pointing into the source tree writes its
# configuration and its database there. Those are the two file types
# .gitignore exists to keep out.
file(COPY ${CMAKE_SOURCE_DIR}/tests/alsa-home/alsa
     DESTINATION ${CMAKE_BINARY_DIR}/alsa-home)
set(SP_TEST_ALSA
    "XDG_CONFIG_HOME=${CMAKE_BINARY_DIR}/alsa-home"
    "COMLINK_SOUND_DIR=${CMAKE_SOURCE_DIR}/packaging/sounds")

add_test(NAME cli_protocol
         COMMAND ${CMAKE_SOURCE_DIR}/tests/cli/test_protocol.sh
                 $<TARGET_FILE:comlink-cli> ${CMAKE_BINARY_DIR})
# Generous: every check starts the CLI, which starts pjsua, and there
# are fifty of them. 120s was enough before the sound and processing
# checks were added.
set_tests_properties(cli_protocol PROPERTIES
    ENVIRONMENT "${SP_TEST_ALSA}" TIMEOUT 600)

# A fresh install must start, rather than sending the operator away to
# copy a file by hand.
add_test(NAME first_run
         COMMAND ${CMAKE_SOURCE_DIR}/tests/ui/test_first_run.sh
                 $<TARGET_FILE:comlink-quick> ${CMAKE_BINARY_DIR}
                 ${CMAKE_SOURCE_DIR})
set_tests_properties(first_run PROPERTIES TIMEOUT 300)

# Asking the binary its version must not need a display.
add_test(NAME version_headless
         COMMAND ${CMAKE_SOURCE_DIR}/tests/ui/test_version_headless.sh
                 $<TARGET_FILE:comlink-quick>)
set_tests_properties(version_headless PROPERTIES TIMEOUT 60)

# And --config must be honoured, or an operator with two
# configurations silently edits one and runs the other.
add_test(NAME config_option
         COMMAND ${CMAKE_SOURCE_DIR}/tests/ui/test_config_option.sh
                 $<TARGET_FILE:comlink-quick> ${CMAKE_BINARY_DIR})
set_tests_properties(config_option PROPERTIES
    ENVIRONMENT "${SP_TEST_ALSA}" TIMEOUT 120)

# Needs raw-socket privileges for tcpdump, so it carries a label the
# package build excludes.
# RUN_SERIAL on all four: these place real calls over the loopback
# interface and judge the packet capture, so a starved peer is
# indistinguishable from a broken one. Sharing the machine with three
# other tests delays the 100 past SIP timer A, alice retransmits, and
# "exactly one INVITE on the wire" fails on correct behaviour. The
# assertion is worth keeping exact; the parallelism is not.
add_test(NAME loopback
         COMMAND ${CMAKE_SOURCE_DIR}/tests/integration/loopback.sh
                 $<TARGET_FILE:comlink-cli> ${CMAKE_BINARY_DIR}
                 ${CMAKE_SOURCE_DIR})
set_tests_properties(loopback PROPERTIES TIMEOUT 120 LABELS integration
    RUN_SERIAL TRUE ENVIRONMENT "${SP_TEST_ALSA}")

add_test(NAME loopback_xfer
         COMMAND ${CMAKE_SOURCE_DIR}/tests/integration/loopback-xfer.sh
                 $<TARGET_FILE:comlink-cli> ${CMAKE_BINARY_DIR}
                 ${CMAKE_SOURCE_DIR})
set_tests_properties(loopback_xfer PROPERTIES TIMEOUT 60 LABELS integration
    RUN_SERIAL TRUE ENVIRONMENT "${SP_TEST_ALSA}")

add_test(NAME loopback_tls
         COMMAND ${CMAKE_SOURCE_DIR}/tests/integration/loopback-tls.sh
                 $<TARGET_FILE:comlink-cli> ${CMAKE_BINARY_DIR}
                 ${CMAKE_SOURCE_DIR})
set_tests_properties(loopback_tls PROPERTIES TIMEOUT 150 LABELS integration
    RUN_SERIAL TRUE ENVIRONMENT "${SP_TEST_ALSA}")

add_test(NAME loopback_srtp
         COMMAND ${CMAKE_SOURCE_DIR}/tests/integration/loopback-srtp.sh
                 $<TARGET_FILE:comlink-cli> ${CMAKE_BINARY_DIR}
                 ${CMAKE_SOURCE_DIR})
set_tests_properties(loopback_srtp PROPERTIES TIMEOUT 180 LABELS integration
    RUN_SERIAL TRUE ENVIRONMENT "${SP_TEST_ALSA}")

add_test(NAME loopback_quality
         COMMAND ${CMAKE_SOURCE_DIR}/tests/integration/loopback-quality.sh
                 $<TARGET_FILE:comlink-cli> ${CMAKE_BINARY_DIR}
                 ${CMAKE_SOURCE_DIR})
set_tests_properties(loopback_quality PROPERTIES TIMEOUT 180 LABELS integration
    RUN_SERIAL TRUE ENVIRONMENT "${SP_TEST_ALSA}")

# Every test in this file starts a binary that calls pjsua_init, so
# every one of them needs the ALSA override -- not just the handful
# that said so. core/CMakeLists.txt explains what happens without it:
# pjmedia's ALSA driver attaches the mixer named "default", which on
# this desktop is the pipewire plugin, and when that socket is
# unreachable the failure path joins a thread nothing wakes. The
# process hangs inside pjsua_init before any of our code runs.
#
# The UI tests had been passing on the accident of a reachable
# pipewire. The moment it was not, ten of them timed out at once and
# the first guess was a code change. Same loop as core's, same reason.
get_property(sp_root_tests DIRECTORY . PROPERTY TESTS)
foreach(t IN LISTS sp_root_tests)
    get_test_property(${t} ENVIRONMENT env)
    if(NOT env)
        set(env "")
    endif()
    if(NOT "${env}" MATCHES "XDG_CONFIG_HOME")
        list(APPEND env "${SP_TEST_ALSA}")
        set_tests_properties(${t} PROPERTIES ENVIRONMENT "${env}")
    endif()
endforeach()
endif()
