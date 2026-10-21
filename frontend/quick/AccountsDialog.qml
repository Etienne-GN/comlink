import QtQuick
import comlink

/* The Accounts list -- accountsdialog.cpp's AccountsDialog, ported
 * part for part: one row per account ("id — domain  [registered|
 * offline]"), Add/Edit/Remove/Register beside it, the note that
 * changes are saved to the configuration file immediately, and Close.
 *
 * Add and Edit open AccountEditor.qml in a dialog of its own rather
 * than a rebuilt copy of AccountEditDialog: that pane is where the
 * credential-store handling already lives (see corebridge.cpp's
 * SecretStore calls and tests/test_migration.cpp), and duplicating it
 * here is exactly the kind of second path that lets the two drift. */
Item {
    id: root
    property var host
    anchors.fill: parent

    property var rows: []
    property int currentIndex: -1
    property var editingId: 0

    function reload() {
        rows = CoreBridge.accounts();
        if (currentIndex >= rows.length)
            currentIndex = rows.length - 1;
        /* Row 0 selected by default, the same as accountsdialog.cpp's
           reload(). */
        if (currentIndex < 0 && rows.length > 0)
            currentIndex = 0;
    }
    Component.onCompleted: root.reload()

    function openEditor(id) {
        root.editingId = id;
        editDialog.open();
    }

    readonly property var current: (currentIndex >= 0 && currentIndex < rows.length)
                                    ? rows[currentIndex] : null

    Rectangle {
        id: listFrame
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: btnRow.top
        anchors.bottomMargin: 6
        color: CoreBridge.surface
        border.width: 1
        border.color: CoreBridge.muted

        ListView {
            id: list
            anchors.fill: parent
            anchors.margins: 1
            clip: true
            model: root.rows

            delegate: Item {
                width: list.width
                height: 20

                Rectangle {
                    anchors.fill: parent
                    color: index === root.currentIndex ? CoreBridge.accent
                                                        : "transparent"
                }
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    x: 6
                    width: parent.width - 12
                    text: modelData.name + " — " + modelData.domain + "  ["
                          + (modelData.registered ? "registered" : "offline")
                          + "]"
                    color: index === root.currentIndex
                           ? (CoreBridge.dark ? "#000000" : "#FFFFFF")
                           : CoreBridge.foreground
                    font.pointSize: CoreBridge.basePt
                    elide: Text.ElideRight
                }
                MouseArea {
                    anchors.fill: parent
                    /* Only updates the selection; never reload(), the
                       same guard against re-entrancy
                       accountsdialog.cpp's currentRowChanged keeps. */
                    onClicked: root.currentIndex = index
                    onDoubleClicked: {
                        root.currentIndex = index;
                        root.openEditor(modelData.id);
                    }
                }
            }
        }
    }

    /* One line where there is the width, the desktop's; on a handset
       the three list actions share a line and Register takes the next,
       rather than a negative spacer laying it over Remove. */
    Flow {
        id: btnRow
        readonly property bool stacked: width < 340
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: note.top
        anchors.bottomMargin: 6
        spacing: 4

        PushButton {
            width: btnRow.stacked ? (btnRow.width - 8) / 3 : 70; height: 30
            text: "Add"
            onClicked: root.openEditor(0)
        }
        PushButton {
            width: btnRow.stacked ? (btnRow.width - 8) / 3 : 70; height: 30
            text: "Edit"
            enabled: root.current !== null
            onClicked: root.openEditor(root.current.id)
        }
        PushButton {
            width: btnRow.stacked ? (btnRow.width - 8) / 3 : 90; height: 30
            text: "Remove"
            ink: "#b3261e"
            enabled: root.current !== null
            onClicked: removeConfirm.open()
        }
        Item {
            visible: !btnRow.stacked
            width: btnRow.width - 70 - 70 - 90 - 90 - 4 * 4
            height: 1
        }
        PushButton {
            width: btnRow.stacked ? btnRow.width : 90; height: 30
            text: (root.current && root.current.registered)
                  ? "Unregister" : "Register"
            enabled: root.current !== null
            /* Acts on the row highlighted in this list, the same
               semantics as accountsdialog.cpp's toggleRegistration --
               not on whichever account a call would be placed from. */
            onClicked: {
                CoreBridge.setRegistration(root.current.id,
                                           !root.current.registered);
                root.reload();
            }
        }
    }

    Text {
        id: note
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: closeRow.top
        anchors.bottomMargin: 6
        text: "Changes are saved to the configuration file immediately."
        color: CoreBridge.muted
        font.pixelSize: 11
        wrapMode: Text.WordWrap
    }

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

    QuestionDialog {
        id: removeConfirm
        title: "Remove account"
        text: "Remove account \"" + (root.current ? root.current.name : "")
              + "\"?\n\nThis also removes it from the configuration file."
        onYes: {
            if (root.current)
                CoreBridge.removeAccount(root.current.id);
            root.currentIndex = -1;
            root.reload();
        }
    }

    QuestionDialog {
        id: accountWarning
        kind: "warning"
        title: "Accounts"
    }

    DialogHost {
        id: editDialog
        title: root.editingId === 0 ? "Add account" : "Edit account"
        modal: true
        dialogWidth: 420; dialogHeight: 400
        minimumWidth: 420
        onClosed: root.reload()
        content: Component {
            AccountEditor {
                Component.onCompleted: load(root.editingId)
                onFailed: function(text) {
                    accountWarning.text = text;
                    accountWarning.open();
                }
            }
        }
    }
}
