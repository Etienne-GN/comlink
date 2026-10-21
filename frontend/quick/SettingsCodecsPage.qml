import QtQuick
import comlink
import "fusion.js" as Fusion

/* Which codecs are offered, in what order, and each one's own
 * settings -- codecspage.cpp, part for part: a checkable list with
 * Up/Down beside it, a shared form below for whichever row is
 * selected, and one Apply that sends the whole edited, ordered list
 * at once. Nothing reaches the core before that: reordering and
 * enabling only mutate this page's own working copy. */
SettingsPage {
    id: root

    property var workingRows: []
    property int currentIndex: -1
    property string detail: ""
    property string errorText: ""
    property bool loadingParams: false

    function fromSpin(spin) { return spin.value === spin.from ? -1 : spin.value }
    function toSpin(spin, v) { spin.value = v < 0 ? spin.from : v }

    function populate() {
        root.workingRows = CoreBridge.codecs();
        root.currentIndex = root.workingRows.length > 0 ? 0 : -1;
        root.showParamsFor(root.currentIndex);
    }
    Component.onCompleted: root.populate()

    function setEnabled(i, on) {
        const rows = root.workingRows.slice();
        rows[i] = Object.assign({}, rows[i], { enabled: on });
        root.workingRows = rows;
    }

    function stashCurrentEdits() {
        if (root.loadingParams || root.currentIndex < 0
            || root.currentIndex >= root.workingRows.length)
            return;
        const rows = root.workingRows.slice();
        const row = Object.assign({}, rows[root.currentIndex]);
        if (row.supportsBitrate)
            row.bitrate = root.fromSpin(bitrateSpin);
        row.complexity = root.fromSpin(complexitySpin);
        row.loss = root.fromSpin(lossSpin);
        row.ptime = root.fromSpin(ptimeSpin);
        row.cbr = cbrBox.checkState === 2 ? -1
                  : (cbrBox.checkState === 1 ? 1 : 0);
        rows[root.currentIndex] = row;
        root.workingRows = rows;
    }

    function showParamsFor(i) {
        if (i < 0 || i >= root.workingRows.length) {
            root.detail = "";
            return;
        }
        const row = root.workingRows[i];
        root.loadingParams = true;

        /* A control the codec ignores is shown disabled rather than
           hidden: the operator can see the setting exists and that
           this codec is not the place for it. */
        bitrateSpin.enabled = row.supportsBitrate === true;
        complexitySpin.enabled = row.supportsComplexity === true;
        lossSpin.enabled = row.supportsLoss === true;
        ptimeSpin.enabled = row.supportsPtime === true;
        cbrBox.enabled = row.supportsCbr === true;

        root.toSpin(bitrateSpin, row.bitrate);
        root.toSpin(complexitySpin, row.complexity);
        root.toSpin(lossSpin, row.loss);
        root.toSpin(ptimeSpin, row.ptime);
        cbrBox.checkState = row.cbr < 0 ? 2 : (row.cbr ? 1 : 0);

        if (row.supportsPtime && row.framePtime > 0)
            ptimeSpin.step = row.framePtime;

        root.detail = row.clockRate + " Hz, " + row.channels + " channel"
                     + (row.channels === 1 ? "" : "s")
                     + (row.supportsPtime && row.framePtime > 0
                        ? ", " + row.framePtime + " ms frame" : "");
        root.loadingParams = false;
    }

    function selectRow(i) {
        root.stashCurrentEdits();
        root.currentIndex = i;
        root.showParamsFor(i);
    }

    function moveUp() {
        if (root.currentIndex <= 0)
            return;
        root.stashCurrentEdits();
        const rows = root.workingRows.slice();
        const i = root.currentIndex;
        const tmp = rows[i - 1]; rows[i - 1] = rows[i]; rows[i] = tmp;
        root.workingRows = rows;
        root.currentIndex = i - 1;
        root.showParamsFor(root.currentIndex);
    }

    function moveDown() {
        if (root.currentIndex < 0
            || root.currentIndex >= root.workingRows.length - 1)
            return;
        root.stashCurrentEdits();
        const rows = root.workingRows.slice();
        const i = root.currentIndex;
        const tmp = rows[i + 1]; rows[i + 1] = rows[i]; rows[i] = tmp;
        root.workingRows = rows;
        root.currentIndex = i + 1;
        root.showParamsFor(root.currentIndex);
    }

    function apply() {
        root.stashCurrentEdits();
        root.errorText = "";
        const rows = root.workingRows.map(function(r) {
            return { id: r.id, enabled: r.enabled, bitrate: r.bitrate,
                     complexity: r.complexity, loss: r.loss,
                     ptime: r.ptime, cbr: r.cbr };
        });
        const why = CoreBridge.applyCodecList(rows);
        if (why !== "") {
            root.errorText = why;
            return;
        }
        root.populate();
    }

    GroupBox {
        width: parent.width
        text: "Offer order"
        description: "Offered highest first. Unticking one stops it " +
                     "being offered at all. Changes apply to the next " +
                     "call: a codec's settings are fixed once its stream " +
                     "exists."

        Row {
            width: parent.width
            height: 145
            spacing: 4

        Rectangle {
            id: listFrame
            width: parent.width - 64 - 4
            height: parent.height
            color: CoreBridge.surface
            border.width: 1
            border.color: CoreBridge.muted

            ListView {
                id: list
                anchors.fill: parent
                anchors.margins: 1
                clip: true
                model: root.workingRows

                delegate: Item {
                    width: list.width
                    height: 20

                    Rectangle {
                        anchors.fill: parent
                        color: index === root.currentIndex
                               ? CoreBridge.accent : "transparent"
                    }
                    MouseArea {
                        anchors.fill: parent
                        onClicked: root.selectRow(index)
                    }
                    Rectangle {
                        id: box
                        x: 6
                        width: 14; height: 14
                        anchors.verticalCenter: parent.verticalCenter
                        radius: 2
                        color: CoreBridge.surface
                        border.width: 1
                        border.color: Fusion.outline(CoreBridge.background)
                        Text {
                            anchors.centerIn: parent
                            visible: modelData.enabled === true
                            text: "✓"
                            color: CoreBridge.foreground
                            font.pixelSize: 11
                            font.bold: true
                        }
                        MouseArea {
                            anchors.fill: parent
                            anchors.margins: -3
                            onClicked: root.setEnabled(index,
                                                       !modelData.enabled)
                        }
                    }
                    Text {
                        anchors.left: box.right
                        anchors.leftMargin: 6
                        anchors.right: parent.right
                        anchors.verticalCenter: parent.verticalCenter
                        text: modelData.id
                        color: index === root.currentIndex
                               ? (CoreBridge.dark ? "#000000" : "#FFFFFF")
                               : CoreBridge.foreground
                        font.pointSize: CoreBridge.basePt
                        elide: Text.ElideRight
                    }
                }
            }
        }

            Column {
                width: 64
                spacing: 4
                PushButton {
                    width: parent.width; height: 30
                    text: "Up"
                    enabled: root.currentIndex > 0
                    onClicked: root.moveUp()
                }
                PushButton {
                    width: parent.width; height: 30
                    text: "Down"
                    enabled: root.currentIndex >= 0
                             && root.currentIndex < root.workingRows.length - 1
                    onClicked: root.moveDown()
                }
            }
        }
    }

    GroupBox {
        width: parent.width
        text: root.currentIndex >= 0 && root.currentIndex < root.workingRows.length
              ? root.workingRows[root.currentIndex].id : "Codec"
        description: root.detail

        FormRow {
            text: "Bit rate"
            labelWidth: 92
            dim: !bitrateSpin.enabled
            SpinBox {
                id: bitrateSpin
                width: parent.width; height: 28
                from: 999; to: 512000
                specialValueText: "default"
                suffix: " bps"
                tip: "Encoder bit rate. Opus only: no other codec here " +
                     "has a settable rate."
                onEdited: root.stashCurrentEdits()
            }
        }
        FormRow {
            text: "Complexity"
            labelWidth: 92
            dim: !complexitySpin.enabled
            SpinBox {
                id: complexitySpin
                width: parent.width; height: 28
                from: -1; to: 10
                specialValueText: "default"
                onEdited: root.stashCurrentEdits()
            }
        }
        FormRow {
            text: "Expected loss"
            labelWidth: 92
            dim: !lossSpin.enabled
            SpinBox {
                id: lossSpin
                width: parent.width; height: 28
                from: -1; to: 100
                specialValueText: "default"
                suffix: " %"
                tip: "What the encoder should expect to lose. Above " +
                     "zero is what makes Opus send forward error " +
                     "correction."
                onEdited: root.stashCurrentEdits()
            }
        }
        FormRow {
            text: "Packet time"
            labelWidth: 92
            dim: !ptimeSpin.enabled
            SpinBox {
                id: ptimeSpin
                width: parent.width; height: 28
                from: 9; to: 200
                specialValueText: "default"
                suffix: " ms"
                tip: "Milliseconds of audio per packet. Must be a " +
                     "multiple of the codec's frame."
                onEdited: root.stashCurrentEdits()
            }
        }
        CheckBox {
            wrap: true
            id: cbrBox
            x: parent.width < 300 ? 0 : 92 + 6
            width: parent.width - x
            text: "Constant bit rate"
            tristate: true
            tip: "Partly checked leaves it to the codec."
            onStateEdited: function(state) {
                cbrBox.checkState = state;
                root.stashCurrentEdits();
            }
        }
    }

    footer: [
        Text {
            width: parent.width
            visible: root.errorText !== ""
            text: root.errorText
            color: "#b3261e"
            font.pointSize: CoreBridge.basePt
            wrapMode: Text.WordWrap
        },
        PushButton {
            width: parent.width; height: 30
            text: "Apply codecs"
            onClicked: root.apply()
        }
    ]
}
