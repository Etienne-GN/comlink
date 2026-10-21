import QtQuick
import comlink

/* A button, in the same grammar as Panel, with the ink saying what it
   is for. Disabled goes translucent rather than grey, so it stays the
   same button on all sixteen presets. */
Panel {
    id: root
    property alias text: label.text
    property color ink: CoreBridge.foreground
    signal clicked

    implicitHeight: 34
    opacity: enabled ? 1.0 : 0.45

    Text {
        id: label
        anchors.centerIn: parent
        color: root.ink
        font.pixelSize: 13
    }
    MouseArea {
        anchors.fill: parent
        enabled: root.enabled
        onClicked: root.clicked()
    }
}
