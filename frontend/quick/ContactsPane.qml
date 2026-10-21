import QtQuick
import comlink

/* The contact book -- contactspage.cpp, ported part for part: search,
 * the list (picture/initials square, name over a muted URI line,
 * alternating rows), an empty state, and New/Edit/Delete underneath.
 *
 * Stays a pane in Main.qml's strip, the same as ContactsPage stays a
 * page in mainwindow.cpp's m_paneStack -- not a dialog. New and Edit
 * open ContactEditor.qml as a DialogHost window, the same as
 * ContactsPage::onNew()/onEdit() open ContactEditor modally. */
Item {
    id: root
    signal dialled(string uri)

    property var rows: []
    property int currentIndex: -1
    property var editingId: 0

    readonly property var current: (root.currentIndex >= 0
            && root.currentIndex < root.rows.length)
        ? root.rows[root.currentIndex] : null

    function reload() {
        const keep = root.current ? root.current.id : 0;
        root.rows = CoreBridge.contacts(search.text);
        root.currentIndex = -1;
        if (keep) {
            for (let i = 0; i < root.rows.length; i++) {
                if (root.rows[i].id === keep) { root.currentIndex = i; break }
            }
        }
    }
    /* Built before the core exists -- the pane stack is laid out in
       Main.qml before Component.onCompleted calls CoreBridge.start()
       -- so a plain reload() here would run against no core and find
       nothing. contactspage.cpp waits the same way, for its own
       setCore(core) call from MainWindow after sp_core_create. */
    Component.onCompleted: if (CoreBridge.started) root.reload()
    Connections {
        target: CoreBridge
        function onStartedChanged() { if (CoreBridge.started) root.reload() }
    }

    /* For the probe and the smoke test: open the editor, and the
       crop dialog behind it, without a finger -- contactspage.cpp's
       own newContactForProbe()/pictureEditorForProbe() reached through
       the editor the same way, just headlessly further in. */
    function probeOverlay(which) {
        root.editingId = root.rows.length > 0 ? root.rows[0].id : 0;
        editDialog.open();
        if (which === "picture")
            Qt.callLater(function() {
                if (editDialog.item && editDialog.item.probePicture)
                    editDialog.item.probePicture();
            });
    }

    Field {
        id: search
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        height: 30
        placeholder: "search"
        onTextChanged: root.reload()
    }

    Rectangle {
        id: listFrame
        anchors.top: search.bottom
        anchors.topMargin: 4
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: btnRow.top
        anchors.bottomMargin: 4
        visible: root.rows.length > 0
        color: CoreBridge.surface
        border.width: 1
        border.color: CoreBridge.muted

        ListView {
            id: list
            anchors.fill: parent
            anchors.margins: 1
            clip: true
            model: root.rows

            delegate: Rectangle {
                width: list.width
                height: 46
                readonly property bool selected: index === root.currentIndex
                /* Alternating rows, the same cue
                   QListWidget::setAlternatingRowColors(true) draws. */
                color: selected ? CoreBridge.accent
                       : (index % 2 === 0) ? "transparent"
                                           : Qt.rgba(CoreBridge.foreground.r,
                                                     CoreBridge.foreground.g,
                                                     CoreBridge.foreground.b,
                                                     10 / 255)

                Avatar {
                    x: 4
                    anchors.verticalCenter: parent.verticalCenter
                    width: 34; height: 34
                    name: modelData.name
                    contactId: modelData.id
                }
                Column {
                    x: 4 + 34 + 8
                    anchors.verticalCenter: parent.verticalCenter
                    width: parent.width - (4 + 34 + 8) - 8
                    Text {
                        width: parent.width
                        text: modelData.name
                        color: parent.parent.selected
                               ? (CoreBridge.dark ? "#000000" : "#FFFFFF")
                               : CoreBridge.foreground
                        font.pointSize: CoreBridge.basePt
                        elide: Text.ElideRight
                    }
                    Text {
                        width: parent.width
                        text: modelData.uri
                        color: parent.parent.selected
                               ? (CoreBridge.dark ? "#000000" : "#FFFFFF")
                               : CoreBridge.muted
                        font.pointSize: CoreBridge.basePt * 0.85
                        elide: Text.ElideMiddle
                    }
                }

                TipArea {
                    anchors.fill: parent
                    tip: modelData.favourite ? "Favourite" : ""
                    onClicked: root.currentIndex = index
                    /* Enter/double click dials the first number --
                       contactspage.cpp's onActivated(). */
                    onDoubleClicked: {
                        root.currentIndex = index;
                        if (modelData.uri !== "")
                            root.dialled(modelData.uri);
                    }
                    acceptedButtons: Qt.LeftButton | Qt.RightButton
                    onPressed: function(m) {
                        if (m.button === Qt.RightButton) {
                            root.currentIndex = index;
                            contextMenu.rowIndex = index;
                            const at = mapToItem(contextMenu.parent, m.x, m.y);
                            contextMenu.openAt(at.x, at.y);
                        }
                    }
                }
            }
        }
    }

    Text {
        anchors.centerIn: listFrame
        width: listFrame.width - 24
        visible: root.rows.length === 0
        horizontalAlignment: Text.AlignHCenter
        wrapMode: Text.WordWrap
        color: CoreBridge.muted
        font.pointSize: CoreBridge.basePt
        text: search.text === ""
              ? "No contacts yet.\n\nNew adds one. A contact needs a name "
                + "and at least one number."
              : "Nothing matches \"" + search.text + "\"."
    }

    /* Right click: every number the row offers, then Edit/Delete --
       contactspage.cpp's onContextMenu(). */
    PopupMenu {
        id: contextMenu
        parent: root.Window.window ? (root.Window.window.overlayLayer || root.Window.window.contentItem) : root
        property int rowIndex: -1
        readonly property var row: (rowIndex >= 0 && rowIndex < root.rows.length)
            ? root.rows[rowIndex] : null
        onVisibleChanged: if (visible) items = build()
        function build() {
            if (!contextMenu.row)
                return [];
            const out = [];
            const uri = contextMenu.row.uri;
            if (uri !== "")
                out.push({ text: "Call " + uri, action: function() {
                    root.dialled(uri) } });
            if (out.length > 0)
                out.push({ separator: true });
            out.push({ text: "Edit…", action: function() {
                root.editingId = contextMenu.row.id;
                editDialog.open();
            } });
            out.push({ text: "Delete…", action: function() {
                deleteConfirm.name = contextMenu.row.name;
                deleteConfirm.id = contextMenu.row.id;
                deleteConfirm.open();
            } });
            return out;
        }
    }

    Row {
        id: btnRow
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        height: 30
        spacing: 4
        PushButton {
            width: (parent.width - 8) / 3; height: 30
            text: "New"
            onClicked: { root.editingId = 0; editDialog.open() }
        }
        PushButton {
            width: (parent.width - 8) / 3; height: 30
            text: "Edit"
            enabled: root.current !== null
            onClicked: {
                root.editingId = root.current.id;
                editDialog.open();
            }
        }
        PushButton {
            width: (parent.width - 8) / 3; height: 30
            text: "Delete"
            enabled: root.current !== null
            onClicked: {
                deleteConfirm.name = root.current.name;
                deleteConfirm.id = root.current.id;
                deleteConfirm.open();
            }
        }
    }

    QuestionDialog {
        id: deleteConfirm
        property var id: 0
        property string name: ""
        title: "Delete contact"
        text: "Delete " + deleteConfirm.name + " from the contact book?"
        onYes: {
            if (!CoreBridge.deleteContact(deleteConfirm.id)) {
                deleteFailed.text = "Could not delete it: " + CoreBridge.lastError;
                deleteFailed.open();
                return;
            }
            root.reload();
        }
    }
    QuestionDialog {
        id: deleteFailed
        kind: "warning"
        title: "Delete contact"
    }

    DialogHost {
        id: editDialog
        title: root.editingId === 0 ? "New contact" : "Edit contact"
        modal: true
        dialogWidth: 420; dialogHeight: 400
        minimumWidth: 320
        onClosed: root.reload()
        content: Component {
            ContactEditor {
                Component.onCompleted: load(root.editingId)
                onDone: host.close()
            }
        }
    }
}
