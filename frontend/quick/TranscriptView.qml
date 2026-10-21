import QtQuick
import comlink

/* One call's transcript, over the Logs window's content: who said
 * what, when, in the order it was said. Read-only; Copy puts the
 * lines on the clipboard as text. */
Rectangle {
    id: root
    property int historyId: 0
    property string who: ""
    property var lines: []
    signal closed()

    color: CoreBridge.background
    visible: false

    function show(id, label) {
        root.historyId = id;
        root.who = label;
        root.lines = CoreBridge.transcript(id);
        root.visible = true;
    }
    function hide() {
        root.visible = false;
        root.closed();
    }
    function mmss(ms) {
        const s = Math.floor(ms / 1000);
        const m = Math.floor(s / 60);
        return (m < 10 ? "0" : "") + m + ":" + (s % 60 < 10 ? "0" : "")
               + (s % 60);
    }

    Text {
        id: title
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        /* who is the far end's display name, which the far end
           chose: AutoText would render it as markup. */
        textFormat: Text.PlainText
        text: "Transcript — " + root.who
        color: CoreBridge.foreground
        font.pointSize: CoreBridge.basePt * 1.1
        font.weight: Font.DemiBold
        elide: Text.ElideRight
    }

    Rectangle {
        id: frame
        anchors.top: title.bottom
        anchors.topMargin: 6
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: buttons.top
        anchors.bottomMargin: 6
        color: CoreBridge.surface
        border.width: 1
        border.color: CoreBridge.muted

        ListView {
            id: list
            anchors.fill: parent
            anchors.margins: 6
            clip: true
            spacing: 4
            model: root.lines
            delegate: Text {
                width: list.width
                wrapMode: Text.Wrap
                textFormat: Text.PlainText
                font.pixelSize: 11
                /* The far end in the accent, this phone in the
                   ordinary ink, so the turns read at a glance. */
                color: modelData.local ? CoreBridge.foreground
                                       : CoreBridge.accent
                text: root.mmss(modelData.startMs) + "  "
                      + modelData.speaker + ": " + modelData.text
            }
        }
        Text {
            anchors.centerIn: parent
            width: parent.width - 20
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.Wrap
            visible: root.lines.length === 0
            text: "No transcript for this call."
            color: CoreBridge.muted
            font.pointSize: CoreBridge.basePt
        }
    }

    Row {
        id: buttons
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        spacing: 4
        PushButton {
            width: 70; height: 30
            text: "Copy"
            enabled: root.lines.length > 0
            tip: "Put the transcript on the clipboard as text."
            onClicked: CoreBridge.copyToClipboard(
                           CoreBridge.transcriptText(root.historyId))
        }
        PushButton {
            width: 70; height: 30
            text: "Back"
            tip: "Back to the list of calls."
            onClicked: root.hide()
        }
    }
}
