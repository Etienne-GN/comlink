import QtQuick
import comlink

/* The Settings dialog of the Qt window, ported part for part:
 * settingsdialog.cpp's category list down the side, one page beside
 * it, and the one Close button at the bottom. Accounts and About are
 * not here -- 0.8.0 keeps those as separate dialogs reached from the
 * hamburger menu, ported separately. */
Rectangle {
    id: root
    color: CoreBridge.background

    /* Set by DialogHost's Loader once this is instantiated. */
    property var host
    /* The selected sidebar row, and the QStackedWidget's current
       index in one: Audio, Codecs, Modules, Recording, Sounds,
       Theme, Window, the desktop's order. Also how a probe opens
       straight to a page ("settings:N"). */
    property int page: 0

    /* 0.8.0's modulesChanged() and the Window page's two controls,
       which the main window is told about rather than reaching into
       directly -- see settingsdialog.h. */
    signal modulesChanged()
    signal activityLogVisibilityChanged(bool shown)
    signal windowSizeRequested()

    readonly property var categories: ["Audio", "Codecs", "Modules",
                                       "Recording", "Sounds", "Theme",
                                       "Window", "Transcript"]

    /* Too narrow for the sidebar and a page beside it -- the handset,
       always, or a desktop window shrunk to its width. The categories
       then become a list of their own, and a page opens over it with
       a way back. Decided by width, not platform, so the desktop
       shows the handset's layout when it is as narrow. */
    readonly property bool narrow: width < 420
    property bool pageOpen: false

    /* Back from a page to the list, for Android's Back: true when
       there was somewhere to go back to. */
    function back() {
        if (root.narrow && root.pageOpen) {
            root.pageOpen = false;
            return true;
        }
        return false;
    }

    Item {
        id: content
        anchors.fill: parent
        anchors.margins: 6

        Row {
            id: body
            anchors.top: parent.top
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: closeRow.top
            anchors.bottomMargin: 6
            spacing: 6

            /* Sized to its contents and no more -- the pages are what
               should get the width, the same rule settingsdialog.cpp
               states for m_categories. */
            Rectangle {
                id: sidebar
                visible: !root.narrow
                width: visible ? 104 : 0
                height: body.height
                color: CoreBridge.background

                Column {
                    id: catColumn
                    width: parent.width

                    Repeater {
                        model: root.categories
                        Rectangle {
                            width: catColumn.width
                            height: 26
                            color: root.page === index ? CoreBridge.accent
                                                       : "transparent"
                            Text {
                                x: 8
                                anchors.verticalCenter: parent.verticalCenter
                                width: parent.width - 8
                                text: modelData
                                color: root.page === index
                                       ? (CoreBridge.dark ? "#000000"
                                                          : "#FFFFFF")
                                       : CoreBridge.foreground
                                font.pointSize: CoreBridge.basePt
                                elide: Text.ElideRight
                            }
                            MouseArea {
                                anchors.fill: parent
                                onClicked: root.page = index
                            }
                        }
                    }
                }
            }

            /* The categories as a page of their own, when narrow:
               rows tall enough for a thumb, each saying it leads on. */
            Column {
                id: catList
                visible: root.narrow && !root.pageOpen
                width: body.width
                Repeater {
                    model: root.categories
                    Item {
                        width: catList.width
                        height: 40
                        Text {
                            x: 8
                            anchors.verticalCenter: parent.verticalCenter
                            text: modelData
                            color: CoreBridge.foreground
                            font.pointSize: CoreBridge.basePt * 1.1
                        }
                        Text {
                            anchors.right: parent.right
                            anchors.rightMargin: 8
                            anchors.verticalCenter: parent.verticalCenter
                            text: "›"
                            color: CoreBridge.muted
                            font.pointSize: CoreBridge.basePt * 1.4
                        }
                        Rectangle {
                            anchors.bottom: parent.bottom
                            width: parent.width
                            height: 1
                            visible: index < root.categories.length - 1
                            color: CoreBridge.dark ? Qt.rgba(1, 1, 1, 20 / 255)
                                                   : Qt.rgba(0, 0, 0, 26 / 255)
                        }
                        MouseArea {
                            anchors.fill: parent
                            onClicked: { root.page = index; root.pageOpen = true }
                        }
                    }
                }
            }

            /* Every page is built up front, the same as
               settingsdialog.cpp building all six pages and the
               Window page before the dialog is ever shown -- only
               the stack's current index changes when a category is
               picked. */
            Item {
                id: pageHost
                visible: !root.narrow || root.pageOpen
                width: root.narrow ? body.width
                                   : body.width - sidebar.width - body.spacing
                height: body.height

                /* The way back to the list, and the page's name, when
                   the page has the whole width. */
                Item {
                    id: backRow
                    visible: root.narrow
                    width: parent.width
                    height: visible ? 36 : 0
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        x: 4
                        text: "‹  " + root.categories[root.page]
                        color: CoreBridge.foreground
                        font.pointSize: CoreBridge.basePt * 1.1
                        font.weight: Font.DemiBold
                    }
                    MouseArea {
                        anchors.fill: parent
                        onClicked: root.pageOpen = false
                    }
                }
                Item {
                    id: pageArea
                    anchors.top: backRow.bottom
                    anchors.topMargin: root.narrow ? 4 : 0
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.bottom: parent.bottom
                }

                SettingsAudioPage {
                    anchors.fill: pageArea
                    visible: root.page === 0
                }
                SettingsCodecsPage {
                    anchors.fill: pageArea
                    visible: root.page === 1
                }
                SettingsModulesPage {
                    anchors.fill: pageArea
                    visible: root.page === 2
                    onModulesChanged: root.modulesChanged()
                }
                SettingsRecordingPage {
                    anchors.fill: pageArea
                    visible: root.page === 3
                }
                SettingsSoundsPage {
                    anchors.fill: pageArea
                    visible: root.page === 4
                }
                SettingsThemePage {
                    anchors.fill: pageArea
                    visible: root.page === 5
                }
                SettingsWindowPage {
                    anchors.fill: pageArea
                    visible: root.page === 6
                    onActivityLogVisibilityChanged: function(shown) {
                        root.activityLogVisibilityChanged(shown);
                    }
                    onWindowSizeRequested: root.windowSizeRequested()
                }
                SettingsTranscriptPage {
                    anchors.fill: pageArea
                    visible: root.page === 7
                }
            }
        }

        /* QDialogButtonBox::Close, alone -- no OK/Cancel/Apply
           anywhere in this dialog, the same as every page inside
           it. */
        Item {
            id: closeRow
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            height: 30

            PushButton {
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                width: 90; height: 30
                text: "Close"
                onClicked: if (root.host) root.host.close()
            }
        }
    }
}
