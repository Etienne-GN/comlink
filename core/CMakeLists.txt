add_library(comlink_core SHARED
    src/errors.c
    src/handles.c
    src/paths.c
    src/yaml_tree.c
    src/config.c
    src/hdr_rules.c
    src/events.c
    src/thread.c
    src/core.c
    src/account.c
    src/call.c
    src/media.c
    src/hdr_module.c
    src/store.c
    src/redact.c
    src/siplog.c
    src/actionlog.c
    src/history.c
    src/contacts.c
    src/theme.c
    src/modules.c
    src/sipuri.c
    src/quality.c
    src/audioname.c
    src/recname.c
    src/sound.c
    src/playback.c
    src/speedport.c
    src/version.c
    src/present.c
    src/traceexport.c
    src/oggopus_write.c
    src/oggopus_read.c
    src/pcmsink.c
    src/transcript.c
    src/transcriber.c
    src/transcap.c
)
target_include_directories(comlink_core PUBLIC include)
# whisper.cpp for call transcripts. Optional at configure time so the
# core still builds where it is missing; without it the transcript
# module reports that this build has no speech engine.
pkg_check_modules(WHISPER QUIET whisper)
# For the frontends' tests, which live in sibling directories.
set(COMLINK_HAVE_WHISPER ${WHISPER_FOUND} CACHE INTERNAL "")
if(WHISPER_FOUND)
    target_sources(comlink_core PRIVATE src/stt_whisper.c)
    target_include_directories(comlink_core PRIVATE ${WHISPER_INCLUDE_DIRS})
    target_link_directories(comlink_core PRIVATE ${WHISPER_LIBRARY_DIRS})
    target_link_libraries(comlink_core PRIVATE ${WHISPER_LIBRARIES})
    if(ANDROID)
        # Android links whisper.cpp's static archives, and its
        # whisper.pc (v1.9.5) is written for shared ones: "Libs: -lggml
        # -lggml-base -lwhisper" omits ggml-cpu, which holds every CPU
        # kernel, and puts whisper after the ggml it needs, which a
        # static link resolves left to right. Restated in link order.
        # whisper and ggml are C++, so the core links as C++ to bring
        # the NDK's C++ runtime with it.
        target_link_libraries(comlink_core PRIVATE
                              whisper ggml ggml-cpu ggml-base)
        set_target_properties(comlink_core PROPERTIES LINKER_LANGUAGE CXX)
    endif()
    message(STATUS "whisper ${WHISPER_VERSION}: call transcripts enabled")
else()
    target_sources(comlink_core PRIVATE src/stt_none.c)
    message(STATUS "whisper not found: call transcripts disabled")
endif()
include(GNUInstallDirs)
# Where the "no audio device" ALSA configuration lives once installed.
# COMLINK_ALSA_CONF overrides it at runtime, which is how the build
# tree and the test suite use their own copy. If the file is missing
# entirely, ALSA simply finds nothing, which is the same outcome the
# file exists to produce cleanly.
target_compile_definitions(comlink_core PRIVATE
    SP_ALSA_NONE_CONF="${CMAKE_INSTALL_FULL_DATADIR}/comlink/alsa-none.conf"
    SP_SOUND_DIR="${CMAKE_INSTALL_FULL_DATADIR}/comlink/sounds"
    SP_VERSION="${PROJECT_VERSION}"
    SP_GIT_COMMIT="${SP_GIT_COMMIT}"
    SP_BUILD_DATE="${SP_BUILD_DATE}")
find_package(PkgConfig REQUIRED)
pkg_check_modules(YAML REQUIRED yaml-0.1)
# libopus itself is already linked in via PJ_STATIC_LDFLAGS (pjproject's
# own libpjproject.pc names it in Libs.private, against the system
# libopus -- see packaging notes in the root CMakeLists.txt). This
# pkg-config lookup exists only to get <opus/opus.h> onto the include
# path: PJ_INCLUDE_DIRS holds pjproject's own headers, not libopus's.
pkg_check_modules(OPUS REQUIRED opus)
# m for the theme's colour maths: pow and round, which arrived with
# the contrast walk that used to live in the Qt frontend.
target_link_libraries(comlink_core PRIVATE Threads::Threads m ${YAML_LIBRARIES} ${SQLITE_LIBRARIES} ${OGG_LIBRARIES} ${OPUS_LIBRARIES})
target_include_directories(comlink_core PRIVATE ${YAML_INCLUDE_DIRS} ${PJ_INCLUDE_DIRS} ${SQLITE_INCLUDE_DIRS} ${OGG_INCLUDE_DIRS} ${OPUS_INCLUDE_DIRS})
target_compile_options(comlink_core PRIVATE ${PJ_CFLAGS_OTHER})
target_link_libraries(comlink_core PRIVATE ${PJ_STATIC_LDFLAGS})
# SQLITE_LIBRARY_DIRS is in the list for the same reason the others
# are, even though the desktop link worked without it: /usr/lib is a
# default search path there, so -lsqlite3 resolved on its own. Nothing
# is a default search path when cross-compiling.
target_link_directories(comlink_core PRIVATE ${YAML_LIBRARY_DIRS} ${OGG_LIBRARY_DIRS} ${OPUS_LIBRARY_DIRS} ${SQLITE_LIBRARY_DIRS})
set_target_properties(comlink_core PROPERTIES
    VERSION ${PROJECT_VERSION} SOVERSION ${PROJECT_VERSION_MAJOR})

# Proves the ABI header is PJSIP-free: compiled with no pj include path.
add_executable(test_abi_purity tests/test_abi_purity.c)
# Links the library but is given only the public include dir: no
# pjproject headers are reachable from this translation unit.
target_link_libraries(test_abi_purity PRIVATE comlink_core)
target_include_directories(test_abi_purity PRIVATE include)
add_test(NAME abi_purity COMMAND test_abi_purity)

add_executable(test_errors tests/test_errors.c)
target_link_libraries(test_errors PRIVATE comlink_core)
target_include_directories(test_errors PRIVATE include tests)
add_test(NAME errors COMMAND test_errors)

add_executable(test_handles tests/test_handles.c)
target_link_libraries(test_handles PRIVATE comlink_core Threads::Threads)
target_include_directories(test_handles PRIVATE include src tests)
add_test(NAME handles COMMAND test_handles)

add_executable(test_paths tests/test_paths.c)
target_link_libraries(test_paths PRIVATE comlink_core)
target_include_directories(test_paths PRIVATE include src tests)
add_test(NAME paths COMMAND test_paths)
set_tests_properties(paths PROPERTIES
    ENVIRONMENT "SP_TEST_TMP=${CMAKE_BINARY_DIR}")

add_executable(test_yaml_tree tests/test_yaml_tree.c)
target_link_libraries(test_yaml_tree PRIVATE comlink_core)
target_include_directories(test_yaml_tree PRIVATE include src tests)
add_test(NAME yaml_tree COMMAND test_yaml_tree)

add_executable(test_config tests/test_config.c)
target_link_libraries(test_config PRIVATE comlink_core)
target_include_directories(test_config PRIVATE include src tests)
add_test(NAME config COMMAND test_config)
set_tests_properties(config PROPERTIES
    ENVIRONMENT "SP_TEST_TMP=${CMAKE_BINARY_DIR}")

add_executable(test_hdr_rules tests/test_hdr_rules.c)
target_link_libraries(test_hdr_rules PRIVATE comlink_core)
target_include_directories(test_hdr_rules PRIVATE include src tests)
add_test(NAME hdr_rules COMMAND test_hdr_rules)

add_executable(test_events tests/test_events.c)
target_link_libraries(test_events PRIVATE comlink_core Threads::Threads)
target_include_directories(test_events PRIVATE include src tests)
add_test(NAME events COMMAND test_events)

add_executable(test_lifecycle tests/test_lifecycle.c)
target_link_libraries(test_lifecycle PRIVATE comlink_core Threads::Threads)
target_include_directories(test_lifecycle PRIVATE include src tests)
add_test(NAME lifecycle COMMAND test_lifecycle)
set_tests_properties(lifecycle PROPERTIES
    ENVIRONMENT "SP_TEST_TMP=${CMAKE_BINARY_DIR}")

add_executable(test_account tests/test_account.c)
target_link_libraries(test_account PRIVATE comlink_core Threads::Threads)
target_include_directories(test_account PRIVATE include src tests)
add_test(NAME account COMMAND test_account)
set_tests_properties(account PROPERTIES
    ENVIRONMENT "SP_TEST_TMP=${CMAKE_BINARY_DIR}")

add_executable(test_call tests/test_call.c)
target_link_libraries(test_call PRIVATE comlink_core Threads::Threads)
target_include_directories(test_call PRIVATE include src tests)
add_test(NAME call COMMAND test_call)
set_tests_properties(call PROPERTIES
    ENVIRONMENT "SP_TEST_TMP=${CMAKE_BINARY_DIR}")

add_executable(verify_tx_mutation tests/verify_tx_mutation.c)
target_link_libraries(verify_tx_mutation PRIVATE comlink_core Threads::Threads)
target_include_directories(verify_tx_mutation PRIVATE include)

install(TARGETS comlink_core LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(FILES ${CMAKE_SOURCE_DIR}/packaging/alsa-none.conf
        DESTINATION ${CMAKE_INSTALL_DATADIR}/comlink)
install(DIRECTORY ${CMAKE_SOURCE_DIR}/packaging/sounds/
        DESTINATION ${CMAKE_INSTALL_DATADIR}/comlink/sounds)
install(DIRECTORY include/comlink
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

add_executable(test_config_save tests/test_config_save.c)
target_link_libraries(test_config_save PRIVATE comlink_core)
target_include_directories(test_config_save PRIVATE include src tests)
add_test(NAME config_save COMMAND test_config_save)
set_tests_properties(config_save PROPERTIES
    ENVIRONMENT "SP_TEST_TMP=${CMAKE_BINARY_DIR}")

add_executable(test_races tests/test_races.c)
target_link_libraries(test_races PRIVATE comlink_core Threads::Threads)
target_include_directories(test_races PRIVATE include src tests)
add_test(NAME races COMMAND test_races)

add_executable(test_store tests/test_store.c)
target_link_libraries(test_store PRIVATE comlink_core ${SQLITE_LIBRARIES})
target_include_directories(test_store PRIVATE include src tests
    ${SQLITE_INCLUDE_DIRS})
target_link_directories(test_store PRIVATE ${SQLITE_LIBRARY_DIRS})
add_test(NAME store COMMAND test_store)
set_tests_properties(store PROPERTIES
    ENVIRONMENT "SP_TEST_TMP=${CMAKE_BINARY_DIR}")

add_executable(test_redact tests/test_redact.c)
target_link_libraries(test_redact PRIVATE comlink_core)
target_include_directories(test_redact PRIVATE include src tests)
add_test(NAME redact COMMAND test_redact)

add_executable(test_siplog tests/test_siplog.c)
target_link_libraries(test_siplog PRIVATE comlink_core Threads::Threads)
target_include_directories(test_siplog PRIVATE include src tests)
add_test(NAME siplog COMMAND test_siplog)
set_tests_properties(siplog PROPERTIES
    ENVIRONMENT "SP_TEST_TMP=${CMAKE_BINARY_DIR}")

add_executable(test_traceexport tests/test_traceexport.c)
target_link_libraries(test_traceexport PRIVATE comlink_core Threads::Threads
    ${SQLITE_LIBRARIES})
target_include_directories(test_traceexport PRIVATE include src tests
    ${SQLITE_INCLUDE_DIRS})
target_link_directories(test_traceexport PRIVATE ${SQLITE_LIBRARY_DIRS})
add_test(NAME traceexport COMMAND test_traceexport)
set_tests_properties(traceexport PROPERTIES
    ENVIRONMENT "SP_TEST_TMP=${CMAKE_BINARY_DIR}")

add_executable(test_history tests/test_history.c)
target_link_libraries(test_history PRIVATE comlink_core Threads::Threads)
target_include_directories(test_history PRIVATE include src tests)
add_test(NAME history COMMAND test_history)
set_tests_properties(history PROPERTIES
    ENVIRONMENT "SP_TEST_TMP=${CMAKE_BINARY_DIR}")

# The contact book, at the store layer: normalisation, the round
# trip, and the property the matching rule exists for -- an extension
# never borrowing a citizen's name off a suffix.
add_executable(test_contacts tests/test_contacts.c)
target_link_libraries(test_contacts PRIVATE comlink_core Threads::Threads)
target_include_directories(test_contacts PRIVATE include src tests)
add_test(NAME contacts COMMAND test_contacts)
set_tests_properties(contacts PROPERTIES
    ENVIRONMENT "SP_TEST_TMP=${CMAKE_BINARY_DIR}")

add_executable(test_modules tests/test_modules.c)
target_link_libraries(test_modules PRIVATE comlink_core)
target_include_directories(test_modules PRIVATE include src tests)
add_test(NAME modules COMMAND test_modules)
set_tests_properties(modules PROPERTIES
    ENVIRONMENT "SP_TEST_TMP=${CMAKE_BINARY_DIR}")

add_executable(test_codecs tests/test_codecs.c)
target_link_libraries(test_codecs PRIVATE comlink_core)
target_include_directories(test_codecs PRIVATE include src tests)
add_test(NAME codecs COMMAND test_codecs)
set_tests_properties(codecs PROPERTIES
    ENVIRONMENT "SP_TEST_TMP=${CMAKE_BINARY_DIR}")

add_executable(test_theme tests/test_theme.c)
target_link_libraries(test_theme PRIVATE comlink_core m)
target_include_directories(test_theme PRIVATE include src tests)
add_test(NAME theme COMMAND test_theme)
set_tests_properties(theme PROPERTIES
    ENVIRONMENT "SP_TEST_TMP=${CMAKE_BINARY_DIR}")

add_executable(test_recording tests/test_recording.c)
target_link_libraries(test_recording PRIVATE comlink_core Threads::Threads)
target_include_directories(test_recording PRIVATE include src tests)
add_test(NAME recording COMMAND test_recording)
set_tests_properties(recording PROPERTIES
    ENVIRONMENT "SP_TEST_TMP=${CMAKE_BINARY_DIR}")

# Builds an actual OggOpus fixture with sp_oggopus_writer_create (the
# same call.c makes), not hand-rolled bytes, which needs pjsua/libogg/
# libopus visibility here too -- see test_oggopus's own comment on why.
add_executable(test_playback tests/test_playback.c)
target_link_libraries(test_playback PRIVATE comlink_core Threads::Threads
    ${PJ_STATIC_LDFLAGS} ${OGG_LIBRARIES} ${OPUS_LIBRARIES})
target_include_directories(test_playback PRIVATE include src tests
    ${PJ_INCLUDE_DIRS} ${OGG_INCLUDE_DIRS} ${OPUS_INCLUDE_DIRS})
target_compile_options(test_playback PRIVATE ${PJ_CFLAGS_OTHER})
target_link_directories(test_playback PRIVATE ${OGG_LIBRARY_DIRS} ${OPUS_LIBRARY_DIRS})
add_test(NAME playback COMMAND test_playback)
set_tests_properties(playback PROPERTIES
    ENVIRONMENT "SP_TEST_TMP=${CMAKE_BINARY_DIR}")

# Drives the OggOpus writer port directly -- no core, no call, no
# sound device -- so it needs pjlib's pool/init API and libopus/libogg
# on its own include and link lines, not just comlink_core's.
add_executable(test_oggopus tests/test_oggopus.c)
target_link_libraries(test_oggopus PRIVATE comlink_core
    ${PJ_STATIC_LDFLAGS} ${OGG_LIBRARIES} ${OPUS_LIBRARIES})
target_include_directories(test_oggopus PRIVATE include src tests
    ${PJ_INCLUDE_DIRS} ${OGG_INCLUDE_DIRS} ${OPUS_INCLUDE_DIRS})
target_compile_options(test_oggopus PRIVATE ${PJ_CFLAGS_OTHER})
target_link_directories(test_oggopus PRIVATE ${OGG_LIBRARY_DIRS} ${OPUS_LIBRARY_DIRS})
add_test(NAME oggopus COMMAND test_oggopus)
set_tests_properties(oggopus PROPERTIES
    ENVIRONMENT "SP_TEST_TMP=${CMAKE_BINARY_DIR}")

# The transcript's capture port, driven directly like the OggOpus
# writer: pjlib's pool API on its own include and link lines.
add_executable(test_pcmsink tests/test_pcmsink.c)
target_link_libraries(test_pcmsink PRIVATE comlink_core ${PJ_STATIC_LDFLAGS})
target_include_directories(test_pcmsink PRIVATE include src tests
    ${PJ_INCLUDE_DIRS})
target_compile_options(test_pcmsink PRIVATE ${PJ_CFLAGS_OTHER})
add_test(NAME pcmsink COMMAND test_pcmsink)
set_tests_properties(pcmsink PROPERTIES
    ENVIRONMENT "SP_TEST_TMP=${CMAKE_BINARY_DIR}")

# Drives the speed wrapper port directly against a synthetic tone
# source -- no core, no call, no sound device -- so it needs pjlib's
# pool/init API on its own include and link lines, the same shape as
# test_oggopus's own standalone build.
add_executable(test_speedport tests/test_speedport.c)
target_link_libraries(test_speedport PRIVATE comlink_core
    ${PJ_STATIC_LDFLAGS})
target_include_directories(test_speedport PRIVATE include src tests
    ${PJ_INCLUDE_DIRS})
target_compile_options(test_speedport PRIVATE ${PJ_CFLAGS_OTHER})
add_test(NAME speedport COMMAND test_speedport)

add_executable(test_transcript tests/test_transcript.c)
target_link_libraries(test_transcript PRIVATE comlink_core m)
target_include_directories(test_transcript PRIVATE include src tests)
add_test(NAME transcript COMMAND test_transcript)
set_tests_properties(transcript PROPERTIES
    ENVIRONMENT "SP_TEST_TMP=${CMAKE_BINARY_DIR}")

# Needs pjsua's headers for core_internal.h, as the call tests do not:
# it reaches into the core for the transcriber and the history.
add_executable(test_transcriber tests/test_transcriber.c)
target_link_libraries(test_transcriber PRIVATE comlink_core Threads::Threads m)
target_include_directories(test_transcriber PRIVATE include src tests
    ${PJ_INCLUDE_DIRS})
target_compile_options(test_transcriber PRIVATE ${PJ_CFLAGS_OTHER})
add_test(NAME transcriber COMMAND test_transcriber)
set_tests_properties(transcriber PROPERTIES
    ENVIRONMENT "SP_TEST_TMP=${CMAKE_BINARY_DIR}")

add_executable(test_transcap tests/test_transcap.c)
target_link_libraries(test_transcap PRIVATE comlink_core Threads::Threads)
target_include_directories(test_transcap PRIVATE include src tests
    ${PJ_INCLUDE_DIRS})
target_compile_options(test_transcap PRIVATE ${PJ_CFLAGS_OTHER})
add_test(NAME transcap COMMAND test_transcap)
set_tests_properties(transcap PROPERTIES
    ENVIRONMENT "SP_TEST_TMP=${CMAKE_BINARY_DIR}")

# The real engine, only where there is one. The French case runs when
# COMLINK_WHISPER_MODEL names a model at configure time; see the test.
if(WHISPER_FOUND)
    set(COMLINK_WHISPER_MODEL "" CACHE FILEPATH
        "A whisper ggml model for test_whisper's French case")
    add_executable(test_whisper tests/test_whisper.c)
    target_link_libraries(test_whisper PRIVATE comlink_core)
    target_include_directories(test_whisper PRIVATE include src tests)
    target_compile_definitions(test_whisper PRIVATE
        SP_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures")
    add_test(NAME whisper COMMAND test_whisper)
    set_tests_properties(whisper PROPERTIES
        ENVIRONMENT "COMLINK_WHISPER_MODEL=${COMLINK_WHISPER_MODEL}")
endif()

add_executable(test_sipuri tests/test_sipuri.c)
target_link_libraries(test_sipuri PRIVATE comlink_core)
target_include_directories(test_sipuri PRIVATE include src tests)
add_test(NAME sipuri COMMAND test_sipuri)

add_executable(test_presets tests/test_presets.c)
target_link_libraries(test_presets PRIVATE comlink_core Threads::Threads)
target_include_directories(test_presets PRIVATE include src tests)
add_test(NAME presets COMMAND test_presets)
set_tests_properties(presets PROPERTIES
    ENVIRONMENT "SP_TEST_TMP=${CMAKE_BINARY_DIR}")

add_executable(test_sound tests/test_sound.c)
target_link_libraries(test_sound PRIVATE comlink_core)
target_include_directories(test_sound PRIVATE include src tests)
add_test(NAME sound COMMAND test_sound)
set_tests_properties(sound PROPERTIES
    ENVIRONMENT "SP_TEST_TMP=${CMAKE_BINARY_DIR}")

add_executable(test_audioname tests/test_audioname.c)
target_link_libraries(test_audioname PRIVATE comlink_core)
target_include_directories(test_audioname PRIVATE include src tests)
add_test(NAME audioname COMMAND test_audioname)

# The presentation rules that used to live in frontend/qt.
add_executable(test_present tests/test_present.c)
target_link_libraries(test_present PRIVATE comlink_core)
target_include_directories(test_present PRIVATE include src tests)
add_test(NAME present COMMAND test_present)

add_executable(test_quality tests/test_quality.c)
target_link_libraries(test_quality PRIVATE comlink_core)
target_include_directories(test_quality PRIVATE include src tests)
add_test(NAME quality COMMAND test_quality)

# Every core test runs with ALSA's "default" device redirected away
# from any sound-server plugin, and with a timeout.
#
# pjmedia's ALSA driver labels devices by calling
# snd_mixer_attach(handle, "default"). On a desktop Debian box that
# name is the pipewire ctl plugin, and when the pipewire socket is
# unreachable the plugin's failure path joins a thread nothing wakes:
# the process hangs inside pjsua_init, before any of our code runs.
# dpkg-buildpackage runs the suite without the operator's session,
# which is exactly that condition, and it wedged a package build for
# fifteen hours.
#
# The override arrives through XDG_CONFIG_HOME rather than
# ALSA_CONFIG_PATH: alsa.conf's load hook reads
# $XDG_CONFIG_HOME/alsa/asoundrc last, so it wins, while a file named
# by ALSA_CONFIG_PATH is parsed before the hook runs and is then
# overridden by /etc/alsa/conf.d. Verified with amixer.
#
# The timeout is here so the next hang fails instead of waiting. See
# docs/known-issues.md.
get_property(sp_core_tests DIRECTORY . PROPERTY TESTS)
foreach(t IN LISTS sp_core_tests)
    get_test_property(${t} ENVIRONMENT env)
    if(NOT env)
        set(env "")
    endif()
    list(APPEND env "XDG_CONFIG_HOME=${CMAKE_SOURCE_DIR}/tests/alsa-home")
    set_tests_properties(${t} PROPERTIES ENVIRONMENT "${env}" TIMEOUT 300)
endforeach()
