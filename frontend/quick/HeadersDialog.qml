import QtQuick
import QtQuick.Window
import comlink
import "fusion.js" as Fusion

/* Ad-hoc headers for the next call -- headersdialog.cpp, ported part
 * for part: a two-column table, Add/Remove/Save-as-preset beside it,
 * the note that these are applied after the configured rules, and
 * Ok/Cancel. This dialog never dials: Ok only writes the edited list
 * back to root.staged in Main.qml, the same as headersdialog.cpp's
 * headers() only ever stages a list for whoever placed the call.
 *
 * Presets, the "Apply preset"/"Replace with preset"/"Delete preset"
 * submenus and the header-name chips all live in the header button's
 * own popup (Main.qml's headerMenu, mainwindow.cpp's
 * buildHeaderMenu()) -- 0.8.0 keeps those out of this dialog too. */
Item {
    id: root
    property var host
    anchors.fill: parent

    /* [{name, value}], the same shape as root.staged; edited in a
       working copy so Cancel discards it untouched. */
    property var rows: []
    property int currentIndex: -1
    property int focusRow: -1

    /* What the name fields complete from, and the popup's state while
       one of them is being typed in: the field it belongs to, what
       was typed there, the matches, and the highlighted one (-1 for
       none, as QCompleter's popup starts). */
    property var names: []
    property Item completing: null
    property string typed: ""
    property var matches: []
    property int highlight: -1

    signal accepted(var headers)
    signal cancelled()

    /* headersdialog.cpp's knownNames(): what the core has remembered,
       plus the names people actually use so completion is useful
       before anything has been saved, sorted without regard to case. */
    function knownNames() {
        const out = CoreBridge.headerNames().slice();
        for (const seed of ["X-Ticket", "X-Priority", "X-Geo-Location",
                            "X-Trace-Id", "User-Agent", "Subject"])
            if (out.indexOf(seed) < 0)
                out.push(seed);
        out.sort(function(a, b) {
            const x = a.toLowerCase();
            const y = b.toLowerCase();
            return x < y ? -1 : (x > y ? 1 : 0);
        });
        return out;
    }
    Component.onCompleted: root.names = root.knownNames()

    /* QCompleter as headersdialog.cpp sets it up: case-insensitive,
       matching anywhere in the name, the popup opening under the field
       as the operator types and closing when nothing matches or the
       field is emptied. */
    function complete(field) {
        const t = field.text;
        if (t === "") {
            root.hideCompletion();
            return;
        }
        const needle = t.toLowerCase();
        const found = root.names.filter(function(n) {
            return n.toLowerCase().indexOf(needle) >= 0;
        });
        if (found.length === 0) {
            root.hideCompletion();
            return;
        }
        root.completing = field;
        root.typed = t;
        root.matches = found;
        root.highlight = -1;
        /* Measured at the moment it opens: a binding on mapToItem is
           not re-evaluated when an ancestor moves, the reason
           ComboBox.qml places its list the same way. */
        const p = field.mapToItem(completion, 0, field.height);
        popup.atX = p.x;
        popup.atY = p.y;
        popup.fieldWidth = field.width;
        popup.fieldHeight = field.height;
        completion.visible = true;
    }

    function hideCompletion() {
        completion.visible = false;
        root.completing = null;
        root.highlight = -1;
    }

    /* Highlighting a match puts it in the field, and stepping off
       either end puts back what was typed, as the widget's popup
       does. */
    function highlightRow(i) {
        root.highlight = i;
        const f = root.completing;
        if (!f)
            return;
        f.text = i >= 0 ? root.matches[i] : root.typed;
        f.moveCursorToEnd();
        if (i >= 0)
            matchList.positionViewAtIndex(i, ListView.Contain);
    }

    function choose(i) {
        const f = root.completing;
        if (f) {
            f.text = root.matches[i];
            f.moveCursorToEnd();
        }
        root.hideCompletion();
    }

    function completionKey(field, event) {
        if (!completion.visible || root.completing !== field)
            return;
        const last = root.matches.length - 1;
        switch (event.key) {
        case Qt.Key_Down:
            root.highlightRow(root.highlight < 0 ? 0
                              : (root.highlight === last ? -1
                                                         : root.highlight + 1));
            event.accepted = true;
            break;
        case Qt.Key_Up:
            root.highlightRow(root.highlight < 0 ? last
                              : (root.highlight === 0 ? -1
                                                      : root.highlight - 1));
            event.accepted = true;
            break;
        case Qt.Key_Return:
        case Qt.Key_Enter:
        case Qt.Key_Escape:
            root.hideCompletion();
            event.accepted = true;
            break;
        case Qt.Key_Tab:
        case Qt.Key_Backtab:
            root.hideCompletion();
            break;
        }
    }

    function loadFrom(staged) {
        rows = staged.map(function(h) { return { name: h.name, value: h.value } });
        currentIndex = -1;
    }

    /* Drops nameless rows, same as headersdialog.cpp's headers(). */
    function result() {
        const out = [];
        for (const r of root.rows) {
            const name = r.name.trim();
            if (name === "")
                continue;
            out.push({ name: name, value: r.value });
        }
        return out;
    }

    function addRow() {
        root.hideCompletion();
        /* Before the row exists: its delegate is built the moment rows
           changes, and decides then whether it takes the focus. */
        focusRow = rows.length;
        rows = rows.concat([{ name: "", value: "" }]);
        currentIndex = rows.length - 1;
    }
    function removeRow() {
        if (currentIndex < 0 || currentIndex >= rows.length)
            return;
        root.hideCompletion();
        const rs = rows.slice();
        rs.splice(currentIndex, 1);
        rows = rs;
        currentIndex = -1;
    }

    function finishSavePreset(name) {
        const list = root.result().map(function(h) { return h.name + "|" + h.value });
        if (CoreBridge.savePreset(name, list)) {
            saveInfo.title = "Save preset";
            saveInfo.text = "Saved \"" + name + "\" with " + list.length
                            + " header(s).";
        } else {
            saveInfo.title = "Save preset";
            saveInfo.text = "Could not save.\n\n" + CoreBridge.lastError;
        }
        saveInfo.open();
    }

    Rectangle {
        id: tableFrame
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: btnRow.top
        anchors.bottomMargin: 6
        color: CoreBridge.surface
        border.width: 1
        border.color: CoreBridge.muted

        Rectangle {
            id: header
            anchors.top: parent.top
            anchors.left: parent.left
            anchors.right: parent.right
            height: 22
            gradient: Gradient {
                GradientStop { position: 0
                               color: Fusion.gradientTop(CoreBridge.surface, false) }
                GradientStop { position: 1
                               color: Fusion.gradientBottom(CoreBridge.surface, false) }
            }
            border.width: 1
            border.color: Fusion.outline(CoreBridge.background)
            Text {
                width: parent.width / 2
                height: parent.height
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
                text: "Name"
                color: CoreBridge.foreground
                font.pointSize: CoreBridge.basePt
            }
            Rectangle {
                x: parent.width / 2
                width: 1
                height: parent.height
                color: Fusion.outline(CoreBridge.background)
            }
            Text {
                x: parent.width / 2
                width: parent.width / 2
                height: parent.height
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
                text: "Value"
                color: CoreBridge.foreground
                font.pointSize: CoreBridge.basePt
            }
        }

        ListView {
            id: list
            anchors.top: header.bottom
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            anchors.margins: 1
            clip: true
            /* Not the view's own current item: that one takes the
               keyboard focus from the field just given it. */
            currentIndex: -1
            model: root.rows

            delegate: Item {
                id: rowItem
                width: list.width
                height: 26

                Rectangle {
                    anchors.fill: parent
                    visible: index === root.currentIndex
                    color: CoreBridge.accent
                    opacity: 0.35
                }
                Field {
                    id: nameField
                    x: 2; y: 2
                    width: list.width / 2 - 4; height: 22
                    placeholder: "X-Something"
                    text: modelData.name
                    onFocused: root.currentIndex = index
                    /* Written into the row where it stands: giving
                       rows a new array rebuilds every delegate, and
                       a field rebuilt under the operator's cursor
                       loses the focus after each letter. */
                    onTextChanged: root.rows[index].name = text
                    onEdited: root.complete(nameField)
                    onKeyPressed: function(event) {
                        root.completionKey(nameField, event)
                    }
                    onInputFocusChanged: {
                        if (!inputFocus && root.completing === nameField)
                            root.hideCompletion();
                    }
                    /* Add appends and focuses the empty row it made,
                       the same as headersdialog.cpp's addRow(). Once
                       the view has parented the delegate: a focus
                       given earlier is taken back by that. */
                    Component.onCompleted: {
                        if (index !== root.focusRow)
                            return;
                        root.focusRow = -1;
                        Qt.callLater(nameField.focusInput);
                    }
                }
                Field {
                    x: list.width / 2 + 2; y: 2
                    width: list.width / 2 - 4; height: 22
                    placeholder: "value"
                    text: modelData.value
                    onFocused: root.currentIndex = index
                    onTextChanged: root.rows[index].value = text
                }
            }
        }
    }

    /* One line on the desktop; on a handset Add and Remove share a
       line and Save as preset takes the next, rather than a negative
       spacer laying it over Remove. */
    Flow {
        id: btnRow
        readonly property bool stacked: width < 310
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: note.top
        anchors.bottomMargin: 6
        spacing: 4

        PushButton {
            width: btnRow.stacked ? (btnRow.width - 4) / 2 : 70; height: 30
            text: "Add"
            onClicked: root.addRow()
        }
        PushButton {
            width: btnRow.stacked ? (btnRow.width - 4) / 2 : 90; height: 30
            text: "Remove"
            enabled: root.currentIndex >= 0 && root.currentIndex < root.rows.length
            onClicked: root.removeRow()
        }
        Item {
            visible: !btnRow.stacked
            width: btnRow.width - 70 - 90 - 130 - 4 * 3; height: 1
        }
        PushButton {
            width: btnRow.stacked ? btnRow.width : 130; height: 30
            text: "Save as preset…"
            enabled: root.rows.length > 0
            onClicked: {
                if (root.result().length === 0) {
                    saveInfo.title = "Save preset";
                    saveInfo.text = "Add at least one header with a name first.";
                    saveInfo.open();
                    return;
                }
                presetNameDialog.open();
            }
        }
    }

    Text {
        id: note
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: closeRow.top
        anchors.bottomMargin: 6
        text: "Applied after the configured rules, so these always win."
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

        Row {
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            spacing: 6
            PushButton {
                width: 80; height: 30
                text: "OK"
                onClicked: {
                    const out = root.result();
                    root.accepted(out);
                }
            }
            PushButton {
                width: 80; height: 30
                text: "Cancel"
                onClicked: {
                    root.cancelled();
                }
            }
        }
    }

    TextMetrics {
        id: rowMetrics
        text: "X"
        font.pointSize: CoreBridge.basePt
    }

    /* QCompleter's popup: a list under the field in the field's width,
       seven rows before it scrolls, in the theme's colours. Laid on
       the window's overlay so the table's clipping cannot cut it. */
    Item {
        id: completion
        visible: false
        parent: root.Window.window ? (root.Window.window.overlayLayer || root.Window.window.contentItem) : root
        anchors.fill: parent
        z: 150

        MouseArea {
            anchors.fill: parent
            onPressed: root.hideCompletion()
        }

        Rectangle {
            id: popup
            property real atX: 0
            property real atY: 0
            property real fieldWidth: 0
            property real fieldHeight: 0
            readonly property int rowHeight: Math.ceil(rowMetrics.height)
            readonly property int rows: Math.min(root.matches.length, 7)
            x: Math.max(0, Math.min(atX, completion.width - width))
            y: atY + height <= completion.height ? atY
               : Math.max(0, atY - fieldHeight - height)
            width: fieldWidth
            height: rows * rowHeight + 2
            color: CoreBridge.surface
            border.width: 1
            border.color: Fusion.outline(CoreBridge.background)

            ListView {
                id: matchList
                x: 1; y: 1
                width: parent.width - 2
                height: parent.height - 2
                clip: true
                interactive: false
                boundsBehavior: Flickable.StopAtBounds
                model: root.matches
                currentIndex: -1
                delegate: Rectangle {
                    width: ListView.view.width - (matchBar.visible ? matchBar.width + 2 : 0)
                    height: popup.rowHeight
                    color: index === root.highlight ? CoreBridge.accent
                                                    : "transparent"
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        x: 3
                        width: parent.width - 6
                        text: modelData
                        color: index === root.highlight
                               ? (CoreBridge.dark ? "#000000" : "#FFFFFF")
                               : CoreBridge.foreground
                        font.pointSize: CoreBridge.basePt
                        elide: Text.ElideRight
                    }
                    MouseArea {
                        anchors.fill: parent
                        onClicked: root.choose(index)
                    }
                }
                WheelHandler {
                    onWheel: function(w) {
                        matchList.contentY = Math.max(0, Math.min(
                            matchList.contentHeight - matchList.height,
                            matchList.contentY - w.angleDelta.y / 120 * popup.rowHeight * 3));
                    }
                }
            }
            Rectangle {
                id: matchBar
                visible: matchList.contentHeight > matchList.height
                anchors.right: parent.right
                anchors.rightMargin: 2
                y: 1 + (matchList.height - height) * (matchList.contentY
                       / Math.max(1, matchList.contentHeight - matchList.height))
                width: 6
                height: Math.max(16, matchList.height * matchList.height / Math.max(1, matchList.contentHeight))
                radius: 3
                color: CoreBridge.muted
                opacity: 0.7
            }
        }
    }

    DialogHost {
        id: presetNameDialog
        title: "Save preset"
        modal: true
        dialogWidth: 320; dialogHeight: 120
        content: Component {
            Column {
                property var host
                spacing: 8
                Text {
                    text: "Preset name:"
                    color: CoreBridge.foreground
                    font.pointSize: CoreBridge.basePt
                }
                /* Starts blank on every open, the same as
                   QInputDialog::getText's own default. */
                Field { id: presetNameFieldInner; width: parent.width; focus: true }
                Row {
                    anchors.right: parent.right
                    spacing: 6
                    PushButton {
                        width: 80; height: 30
                        text: "OK"
                        onClicked: {
                            const name = presetNameFieldInner.text.trim();
                            host.close();
                            if (name !== "")
                                root.finishSavePreset(name);
                        }
                    }
                    PushButton {
                        width: 80; height: 30
                        text: "Cancel"
                        onClicked: host.close()
                    }
                }
            }
        }
    }

    QuestionDialog {
        id: saveInfo
        kind: "warning"
        title: "Save preset"
    }
}
