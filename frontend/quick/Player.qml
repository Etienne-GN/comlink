import QtQuick
import comlink

/* The transport for a recording: play and pause, a position bar, and
   the two speeds. It appears when something is loaded and goes away
   when it is not.
 *
 * Whether the bar can be dragged is the core's answer, not a guess
 * from the file's extension: sp_playback refuses to seek an OggOpus
 * recording and says so through "seekable". */
Panel {
    id: root
    property var p: ({ state: 0 })
    visible: p.state !== 0
    implicitHeight: 54

    function reload() { p = CoreBridge.playback() }
    Timer {
        interval: 250; running: root.visible; repeat: true
        onTriggered: root.reload()
    }

    function mmss(ms) {
        const s = Math.floor(ms / 1000);
        return Math.floor(s / 60) + ":" + ("0" + (s % 60)).slice(-2);
    }

    Column {
        anchors.fill: parent
        anchors.margins: 6
        spacing: 4

        Row {
            width: parent.width
            spacing: 6
            Glyph {
                width: 26; height: 20
                /* One control that shows what pressing it will do,
                   which is what the desktop's play/pause button
                   became. */
                kind: root.p.state === 1 ? "hold" : "chevronDown"
                ink: CoreBridge.foreground
                MouseArea {
                    anchors.fill: parent
                    onClicked: {
                        if (root.p.state === 1) CoreBridge.playbackPause();
                        else CoreBridge.playbackResume();
                        root.reload();
                    }
                }
            }
            Glyph {
                width: 26; height: 20
                kind: "record"
                ink: CoreBridge.stop
                MouseArea {
                    anchors.fill: parent
                    onClicked: { CoreBridge.playbackStop(); root.reload() }
                }
            }
            Text {
                text: root.mmss(root.p.positionMs || 0) + " / "
                      + root.mmss(root.p.durationMs || 0)
                color: CoreBridge.muted
                font.family: "monospace"
                font.pixelSize: 11
                height: 20
                verticalAlignment: Text.AlignVCenter
            }
            Repeater {
                model: [0.5, 1.0, 2.0]
                Btn {
                    width: 42; height: 20
                    text: modelData === 1.0 ? "1x" : (modelData + "x")
                    ink: Math.abs((root.p.speed || 1) - modelData) < 0.01
                         ? CoreBridge.accent : CoreBridge.foreground
                    onClicked: {
                        CoreBridge.playbackSpeed(modelData);
                        root.reload();
                    }
                }
            }
        }

        Rectangle {
            width: parent.width
            height: 6
            radius: 3
            color: CoreBridge.background
            border.width: 1
            border.color: CoreBridge.muted
            Rectangle {
                height: parent.height - 2
                y: 1; x: 1
                width: root.p.durationMs > 0
                       ? (parent.width - 2) * root.p.positionMs
                         / root.p.durationMs
                       : 0
                radius: 2
                color: CoreBridge.accent
            }
            MouseArea {
                anchors.fill: parent
                enabled: root.p.seekable === true
                onClicked: function(m) {
                    CoreBridge.playbackSeek(
                        Math.round(root.p.durationMs * m.x / width));
                    root.reload();
                }
            }
        }
    }
}
