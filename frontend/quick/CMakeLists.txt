# The frontend, on Debian and on Android.
find_package(Qt6 REQUIRED COMPONENTS Quick Network)
if(NOT ANDROID)
    # The desktop keyring is reached over the session bus; on
    # Android the keystore is a JNI call and needs none of this.
    find_package(Qt6 REQUIRED COMPONENTS DBus)
endif()
if(ANDROID)
    # QAndroidApplication::context() and QJniObject live behind
    # Qt6::CorePrivate on Android.
    find_package(Qt6 REQUIRED COMPONENTS CorePrivate)
endif()
qt_standard_project_setup(REQUIRES 6.5)

qt_add_executable(comlink-quick main.cpp)

# One QML module, which is what androiddeployqt packages and what the
# desktop loads from the resource system. The same tree either way.
qt_add_qml_module(comlink-quick
    URI comlink
    VERSION 1.0
    QML_FILES
        Main.qml
        Glyph.qml Avatar.qml Meter.qml
        Panel.qml Btn.qml Field.qml SectionLabel.qml Toggle.qml Slider.qml
        TitleBar.qml WindowButton.qml ResizeEdge.qml
        KeypadPane.qml ContactsPane.qml QualityPane.qml
        ActivityPane.qml SettingsPane.qml LogsDialog.qml Player.qml
        ContactEditor.qml AccountEditor.qml PictureDialog.qml CodecRow.qml
        HeadersDialog.qml SettingsDialog.qml PopupMenu.qml TipArea.qml
        DialogHost.qml CallLogOverlay.qml PushButton.qml ToolButton.qml
        QuestionDialog.qml Starfield.qml Emblem.qml fusion.js
        AccountsDialog.qml AboutDialog.qml QualityDialog.qml
        CheckBox.qml ComboBox.qml GroupBox.qml SpinBox.qml
        SettingsAudioPage.qml SettingsCodecsPage.qml SettingsModulesPage.qml
        SettingsRecordingPage.qml SettingsSoundsPage.qml
        SettingsThemePage.qml SettingsWindowPage.qml
        SettingsPage.qml FormRow.qml SettingsTranscriptPage.qml
        TranscriptView.qml
    SOURCES corebridge.h corebridge.cpp secretstore.h secretstore.cpp
            modelfetcher.h modelfetcher.cpp)

# Qt6::Network for QNetworkInformation: a wifi-to-LTE handoff is
# reported by Qt on both platforms, so the transports are rebuilt
# without a line of Java.
target_link_libraries(comlink-quick PRIVATE Qt6::Quick Qt6::Network
                      comlink_core)
if(NOT ANDROID)
    target_link_libraries(comlink-quick PRIVATE Qt6::DBus)
endif()
if(ANDROID)
    target_link_libraries(comlink-quick PRIVATE Qt6::CorePrivate)
    # __android_log_write, for main.cpp's stdout/stderr pump.
    target_link_libraries(comlink-quick PRIVATE log)
endif()
target_include_directories(comlink-quick PRIVATE ${CMAKE_SOURCE_DIR}/core/include)

if(NOT ANDROID)
    # The window icon, embedded rather than only looked up in the icon
    # theme, so a build-tree run and a run before the package is
    # installed both show it. The theme lookup is still tried first at
    # runtime, so a user's icon theme can override it.
    qt_add_resources(comlink-quick "appicon"
        PREFIX "/"
        BASE ${CMAKE_SOURCE_DIR}/packaging/icons
        FILES ${CMAKE_SOURCE_DIR}/packaging/icons/comlink-256.png)

    # This is the application now, installed as `comlink`.
    #
    # Renamed on the way in rather than built under that name: the QML
    # module's URI is "comlink" too, so qt_add_qml_module already owns
    # a directory of that name in the build tree and the linker
    # refuses to write a file over it.
    include(GNUInstallDirs)
    install(PROGRAMS $<TARGET_FILE:comlink-quick>
            DESTINATION ${CMAKE_INSTALL_BINDIR}
            RENAME comlink)
endif()

if(ANDROID)
    # The sounds the core plays. The Debian package installs them to
    # /usr/share/comlink/sounds and the core finds them there; an APK
    # has no such place, so they ride in as Qt resources and main.cpp
    # unpacks them once. Without this every sound slot reads "none"
    # and the core is blamed for a file that was never shipped.
    # Absolute paths: qt_add_resources resolves FILES against the
    # current source directory whatever BASE says, and BASE only
    # decides the alias each file gets inside the resource system.
    file(GLOB_RECURSE COMLINK_SOUND_FILES
         "${CMAKE_SOURCE_DIR}/packaging/sounds/*")
    qt_add_resources(comlink-quick "comlink_sounds"
        PREFIX "/"
        BASE "${CMAKE_SOURCE_DIR}/packaging"
        FILES ${COMLINK_SOUND_FILES})

    # The desktop draws in DejaVu Sans, fontconfig's sans-serif, and the
    # window is the same only if the handset draws in it too: Roboto is
    # smaller at the same pixel size and has no U+2630 for the menu.
    qt_add_resources(comlink-quick "comlink_fonts"
        PREFIX "/fonts"
        BASE "${CMAKE_SOURCE_DIR}/third_party/fonts/dejavu"
        FILES "${CMAKE_SOURCE_DIR}/third_party/fonts/dejavu/DejaVuSans.ttf"
              "${CMAKE_SOURCE_DIR}/third_party/fonts/dejavu/DejaVuSans-Bold.ttf"
              "${CMAKE_SOURCE_DIR}/third_party/fonts/dejavu/DejaVuSansMono.ttf")

    # Qt's manifest template, with Comlink's permissions added. The
    # generated one has none, so the capture device is refused and a
    # call fails with nothing on screen to say why.
    set_property(TARGET comlink-quick PROPERTY
        QT_ANDROID_PACKAGE_SOURCE_DIR
        "${CMAKE_CURRENT_SOURCE_DIR}/android")
    # androiddeployqt walks the Qt targets and packages what they
    # need; libcomlink_core.so is not one of them, so it has to be
    # named or the APK installs and then cannot resolve it at load.
    # A plain path, not $<TARGET_FILE:...>: Qt's deployment-settings
    # generator writes this list into JSON and mangles a generator
    # expression on the way, which fails as "Expression did not
    # evaluate" with the colon already turned into a semicolon.
    set_property(TARGET comlink-quick APPEND PROPERTY QT_ANDROID_EXTRA_LIBS
        "${CMAKE_BINARY_DIR}/core/libcomlink_core.so")
    # The one OpenSSL in the process: the core links it for SIP over
    # TLS, and Qt's TLS backend dlopens it by these names for HTTPS.
    # Neither is a Qt target, so neither is found without naming it.
    set_property(TARGET comlink-quick APPEND PROPERTY QT_ANDROID_EXTRA_LIBS
        "${COMLINK_DEPS_PREFIX}/lib/libssl_3.so"
        "${COMLINK_DEPS_PREFIX}/lib/libcrypto_3.so")
    # Identity on the handset. Without these the APK installs as
    # "comlink-quick" under org.qtproject.example, which is Qt's
    # example package rather than this application's -- and two
    # applications sharing that id cannot be installed side by side.
    # The icon is the desktop's, rendered into the launcher densities
    # and an adaptive icon under frontend/quick/android/res.
    set_property(TARGET comlink-quick PROPERTY
        QT_ANDROID_PACKAGE_NAME "io.github.etienne_gn.comlink")
    set_property(TARGET comlink-quick PROPERTY QT_ANDROID_APP_NAME "Comlink")
    set_property(TARGET comlink-quick PROPERTY
        QT_ANDROID_VERSION_NAME "${PROJECT_VERSION}")
    # From the version, so it cannot be left behind: it stayed at 901,
    # 0.9.1's, through 0.10.0 and 0.10.1. Android installs an update
    # only over a lower code. major*10000 + minor*100 + patch, which
    # is what 901 already was.
    math(EXPR COMLINK_VERSION_CODE
         "${PROJECT_VERSION_MAJOR} * 10000 + ${PROJECT_VERSION_MINOR} * 100 + ${PROJECT_VERSION_PATCH}")
    set_property(TARGET comlink-quick PROPERTY
        QT_ANDROID_VERSION_CODE ${COMLINK_VERSION_CODE})

    # 28 because Qt 6.10 does not build below it; the core's own
    # libraries are still API 23 and run happily here. See
    # scripts/build-qt-android.sh.
    set_property(TARGET comlink-quick PROPERTY QT_ANDROID_MIN_SDK_VERSION 28)
    set_property(TARGET comlink-quick PROPERTY QT_ANDROID_TARGET_SDK_VERSION 34)
endif()

# The bridge, tested. Desktop only: it needs no window, and on Android
# there is no ctest to run it from.
if(NOT ANDROID)
    add_executable(test_bridge tests/test_bridge.cpp corebridge.cpp
                               secretstore.cpp)
    # Qt6::Gui for QImage: the picture path decodes, crops and
    # encodes here, so the test has to be able to read what it
    # produced.
    target_link_libraries(test_bridge PRIVATE Qt6::Qml Qt6::Gui
                          Qt6::Network Qt6::DBus comlink_core)
    # Debian's package builds with whisper; the bridge test checks the
    # engine is there wherever the core found it.
    if(COMLINK_HAVE_WHISPER)
        target_compile_definitions(test_bridge PRIVATE SP_TEST_EXPECT_WHISPER)
    endif()
    target_include_directories(test_bridge PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/core/include)
    find_package(Python3 COMPONENTS Interpreter QUIET)

    # A Q_INVOKABLE referenced without its parentheses is not an error
    # in QML: it renders as "function() { [native code] }". This reads
    # the header and every .qml and says so.
    if(Python3_Interpreter_FOUND)
        add_test(NAME quick_bridge_refs
                 COMMAND ${Python3_EXECUTABLE}
                         ${CMAKE_SOURCE_DIR}/tests/ui/qml_bridge_refs.py)
    endif()

    # Every pane and overlay, loaded offscreen with warnings made
    # fatal, then the images they produced checked for a band that
    # drew nothing. A pane that fails to lay out is an empty
    # rectangle, and an empty rectangle raises no warning.
    add_test(NAME quick_panes
             COMMAND ${CMAKE_COMMAND} -E env
                     bash ${CMAKE_SOURCE_DIR}/tests/ui/qml_panes.sh
                     $<TARGET_FILE:comlink-quick>
                     ${CMAKE_BINARY_DIR}/quick-panes)
    set_tests_properties(quick_panes PROPERTIES TIMEOUT 300)
    if(Python3_Interpreter_FOUND)
        add_test(NAME quick_render
                 COMMAND ${Python3_EXECUTABLE}
                         ${CMAKE_SOURCE_DIR}/tests/ui/qml_render.py
                         ${CMAKE_BINARY_DIR}/quick-panes)
        # The images are what it reads, so it runs after the run that
        # writes them.
        set_tests_properties(quick_render PROPERTIES DEPENDS quick_panes)
    endif()

    # The upgrade path, in its own process: one sp_core may be
    # started per process and this one starts from a configuration
    # the test wrote.
    add_executable(test_migration tests/test_migration.cpp corebridge.cpp
                                  secretstore.cpp)
    target_link_libraries(test_migration PRIVATE Qt6::Qml Qt6::Gui
                          Qt6::Network Qt6::DBus comlink_core)
    target_include_directories(test_migration PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/core/include)
    add_test(NAME quick_migration COMMAND test_migration)
    set_tests_properties(quick_migration PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;XDG_CONFIG_HOME=${CMAKE_BINARY_DIR}/alsa-home"
        TIMEOUT 120)

    # The model download, against local files and a silent socket: no
    # network, no Hugging Face.
    find_package(Qt6 REQUIRED COMPONENTS Test)
    add_executable(test_modelfetcher tests/test_modelfetcher.cpp
                                     modelfetcher.cpp)
    target_link_libraries(test_modelfetcher PRIVATE Qt6::Qml Qt6::Network
                          Qt6::Test)
    target_include_directories(test_modelfetcher PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR})
    add_test(NAME quick_modelfetcher COMMAND test_modelfetcher)
    set_tests_properties(quick_modelfetcher PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 60)

    add_test(NAME quick_bridge COMMAND test_bridge)
    set_tests_properties(quick_bridge PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;XDG_CONFIG_HOME=${CMAKE_BINARY_DIR}/alsa-home"
        TIMEOUT 120)
endif()
