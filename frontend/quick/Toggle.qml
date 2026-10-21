import QtQuick
import comlink

/* A setting that is on or off, as a row rather than a checkbox: on a
   handset the whole row is the target, and on the desktop it reads
   the same. The accent says on; the muted outline says off. */
Item {
    id: root
    property alias text: label.text
    property bool checked: false
    signal toggled(bool on)

    implicitHeight: 30

    Text {
        id: label
        anchors.left: parent.left
        anchors.verticalCenter: parent.verticalCenter
        width: parent.width - 42
        color: CoreBridge.foreground
        font.pixelSize: 12
        elide: Text.ElideRight
    }
    Rectangle {
        anchors.right: parent.right
        anchors.verticalCenter: parent.verticalCenter
        width: 34; height: 18; radius: 9
        color: root.checked ? CoreBridge.accent : "transparent"
        border.width: 1
        border.color: root.checked ? CoreBridge.accent : CoreBridge.muted
        Rectangle {
            width: 12; height: 12; radius: 6
            anchors.verticalCenter: parent.verticalCenter
            x: root.checked ? parent.width - width - 3 : 3
            color: root.checked ? CoreBridge.surface : CoreBridge.muted
            Behavior on x { NumberAnimation { duration: 90 } }
        }
    }
    MouseArea {
        anchors.fill: parent
        onClicked: root.toggled(!root.checked)
    }
}
