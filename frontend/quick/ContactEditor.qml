import QtQuick
import QtQuick.Dialogs
import comlink

/* One contact, editable -- contacteditor.cpp, ported part for part:
 * the picture square and its hint, Name/Company/Note/"Keep at the top
 * of the list", the repeatable number rows, an error line, and
 * Save/Cancel. Hosted modally in a DialogHost, title "New contact" /
 * "Edit contact" set by whoever opens this (ContactsPane.qml, the
 * same as accountsdialog.cpp's own editor is titled from outside).
 *
 * A picture chosen or removed here stays local until Save -- see
 * Avatar.qml's overridePicture/forceNoPicture and
 * CoreBridge.stagedPreview -- the same as contacteditor.cpp's own
 * m_pictureBytes never reaches the core until save() runs, so Cancel
 * genuinely discards it. There is no Delete here: 0.8.0 keeps that in
 * ContactsPage, with its own confirmation. */
Item {
    id: root
    property var host
    anchors.fill: parent

    property var id: 0
    /* "label|uri" strings, the same shape CoreBridge.contact() and
       saveContact() both use, so a save sends the working copy
       straight through with no reshaping. */
    property var numbers: ["|"]

    property bool pictureCleared: false
    property string picturePreview: ""
    property var pictureCrop: null   // {x, y, edge}, source-image pixels

    function load(cid) {
        root.id = cid;
        root.pictureCleared = false;
        root.picturePreview = "";
        root.pictureCrop = null;
        errorText.text = "";
        const c = CoreBridge.contact(cid);
        nameField.text = c.name;
        companyField.text = c.company;
        noteField.text = c.note;
        favouriteBox.checked = c.favourite;
        root.numbers = c.numbers.length > 0 ? c.numbers.slice() : ["|"];
        nameField.focusInput();
    }

    /* For the probe: the crop dialog with nothing chosen yet, which is
       what a screenshot of "choose a picture" should show anyway. */
    function probePicture() {
        pictureDialog.sourceUrl = "";
        pictureDialog.open();
    }

    function addRow() { root.numbers = root.numbers.concat(["|"]) }
    function removeRow(i) {
        const n = root.numbers.slice();
        n.splice(i, 1);
        root.numbers = n.length > 0 ? n : ["|"];
    }

    readonly property bool hasNumber: root.numbers.some(function(n) {
        return n.split("|")[1] !== undefined && n.split("|")[1].trim() !== "" })

    function save() {
        const name = nameField.text.trim();
        if (name === "") {
            errorText.text = "A contact needs a name.";
            nameField.focusInput();
            return;
        }
        if (!root.hasNumber) {
            errorText.text = "A contact needs at least one number, or it "
                             + "cannot be dialled.";
            return;
        }
        const newId = CoreBridge.saveContact(root.id, name,
            companyField.text.trim(), noteField.text.trim(),
            favouriteBox.checked, root.numbers);
        if (newId === 0) {
            errorText.text = "Could not save: " + CoreBridge.lastError;
            return;
        }
        if (root.pictureCleared)
            CoreBridge.clearPicture(newId);
        else if (root.pictureCrop !== null)
            CoreBridge.commitPicture(newId, root.pictureCrop.x,
                                     root.pictureCrop.y,
                                     root.pictureCrop.edge);
        root.done();
    }
    signal done()

    function pictureFilters() {
        const exts = CoreBridge.pictureFormats();
        if (exts.length === 0)
            return ["Images (*)"];
        const patterns = exts.map(function(e) { return "*." + e });
        return ["Images (" + patterns.join(" ") + ")"];
    }

    Row {
        id: pictureRow
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        height: 64
        spacing: 8

        Avatar {
            id: avatar
            width: 64; height: 64
            name: nameField.text
            contactId: root.id
            overridePicture: root.picturePreview
            forceNoPicture: root.pictureCleared
            MouseArea {
                id: avatarArea
                anchors.fill: parent
                cursorShape: Qt.PointingHandCursor
                acceptedButtons: Qt.LeftButton | Qt.RightButton
                onClicked: function(m) {
                    if (m.button === Qt.RightButton) {
                        const at = avatarArea.mapToItem(removeMenu.parent,
                                                        m.x, m.y);
                        removeMenu.openAt(at.x, at.y);
                    } else {
                        picturePicker.open();
                    }
                }
            }
        }
        Text {
            width: pictureRow.width - 64 - 8
            anchors.verticalCenter: parent.verticalCenter
            text: "Click the square to choose a picture. Right-click to "
                 + "remove one."
            color: CoreBridge.foreground
            font.pointSize: CoreBridge.basePt
            wrapMode: Text.WordWrap
        }
    }

    PopupMenu {
        id: removeMenu
        parent: root.Window.window ? (root.Window.window.overlayLayer || root.Window.window.contentItem) : root
        items: [
            { text: "Remove picture", action: function() {
                root.pictureCleared = true;
                root.picturePreview = "";
                root.pictureCrop = null;
            } }
        ]
    }

    FileDialog {
        id: picturePicker
        title: "Choose a picture"
        nameFilters: root.pictureFilters()
        onAccepted: {
            const why = CoreBridge.stagePicture(selectedFile.toString());
            if (why !== "") { errorText.text = why; return }
            pictureDialog.sourceUrl = selectedFile.toString();
            pictureDialog.open();
        }
    }

    DialogHost {
        id: pictureDialog
        property string sourceUrl: ""
        title: "Crop the picture"
        modal: true
        dialogWidth: 420; dialogHeight: 460
        content: Component {
            PictureDialog {
                Component.onCompleted: load(pictureDialog.sourceUrl)
                onAccepted: function(x, y, edge) {
                    root.pictureCrop = { x: x, y: y, edge: edge };
                    root.picturePreview = CoreBridge.stagedPreview(x, y, edge);
                    root.pictureCleared = false;
                    host.close();
                }
                onCancelled: host.close()
            }
        }
    }

    Column {
        id: form
        anchors.top: pictureRow.bottom
        anchors.topMargin: 8
        anchors.left: parent.left
        anchors.right: parent.right
        spacing: 4

        Row {
            width: parent.width
            spacing: 6
            Text {
                width: 70; height: 28
                verticalAlignment: Text.AlignVCenter
                text: "Name"
                color: CoreBridge.foreground
                font.pointSize: CoreBridge.basePt
            }
            Field { id: nameField; width: parent.width - 76; height: 28 }
        }
        Row {
            width: parent.width
            spacing: 6
            Text {
                width: 70; height: 28
                verticalAlignment: Text.AlignVCenter
                text: "Company"
                color: CoreBridge.foreground
                font.pointSize: CoreBridge.basePt
            }
            Field { id: companyField; width: parent.width - 76; height: 28 }
        }
        Row {
            width: parent.width
            spacing: 6
            Text {
                width: 70; height: 28
                verticalAlignment: Text.AlignVCenter
                text: "Note"
                color: CoreBridge.foreground
                font.pointSize: CoreBridge.basePt
            }
            Field { id: noteField; width: parent.width - 76; height: 28 }
        }
        Row {
            width: parent.width
            spacing: 6
            Item { width: 70; height: 22 }
            CheckBox {
                id: favouriteBox
                anchors.verticalCenter: parent.verticalCenter
                text: "Keep at the top of the list"
                onToggled: function(on) { checked = on }
            }
        }
    }

    Text {
        id: numbersLabel
        anchors.top: form.bottom
        anchors.topMargin: 8
        anchors.left: parent.left
        text: "Numbers"
        color: CoreBridge.foreground
        font.pointSize: CoreBridge.basePt
    }

    Column {
        id: numberRows
        anchors.top: numbersLabel.bottom
        anchors.topMargin: 4
        anchors.left: parent.left
        anchors.right: parent.right
        spacing: 3

        Repeater {
            model: root.numbers.length
            Row {
                width: numberRows.width
                spacing: 3
                Field {
                    width: 80; height: 26
                    placeholder: "label"
                    text: root.numbers[index].split("|")[0]
                    onTextChanged: {
                        const parts = root.numbers[index].split("|");
                        if (text === parts[0]) return;
                        const n = root.numbers.slice();
                        n[index] = text + "|" + parts[1];
                        root.numbers = n;
                    }
                }
                Field {
                    width: numberRows.width - 80 - 26 - 6
                    height: 26
                    placeholder: "extension, or a full sip: URI"
                    text: root.numbers[index].split("|")[1]
                    onTextChanged: {
                        const parts = root.numbers[index].split("|");
                        if (text === parts[1]) return;
                        const n = root.numbers.slice();
                        n[index] = parts[0] + "|" + text;
                        root.numbers = n;
                    }
                }
                ToolButton {
                    width: 26; height: 26
                    text: "−"
                    tip: "Remove this number"
                    onClicked: root.removeRow(index)
                }
            }
        }
    }

    PushButton {
        id: addRowBtn
        anchors.top: numberRows.bottom
        anchors.topMargin: 4
        anchors.left: parent.left
        anchors.right: parent.right
        height: 28
        text: "Add a number"
        onClicked: root.addRow()
    }

    Text {
        id: errorText
        anchors.top: addRowBtn.bottom
        anchors.topMargin: 6
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: buttonRow.top
        anchors.bottomMargin: 6
        visible: text !== ""
        color: "#b3261e"
        font.pointSize: CoreBridge.basePt
        wrapMode: Text.WordWrap
    }

    Row {
        id: buttonRow
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        spacing: 6
        PushButton {
            width: 80; height: 30
            text: "Save"
            onClicked: root.save()
        }
        PushButton {
            width: 80; height: 30
            text: "Cancel"
            onClicked: root.done()
        }
    }
}
