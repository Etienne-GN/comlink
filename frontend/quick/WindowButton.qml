import QtQuick
import comlink

/* One window button: square, tight, flat.
 *
 * No border radius anywhere -- the rounded outlines came from the
 * adwaita decoration, which is gone. Hovering lifts the background a
 * little; close reddens, which is the one convention worth keeping.
 */
Item {
    id: root
    property string kind: "winClose"
    property bool danger: false
    signal pressed()

    width: 24
    height: 22

    Rectangle {
        anchors.fill: parent
        color: root.danger
               ? (area.pressed ? "#8c1d16" : "#b3261e")
               : Qt.lighter(CoreBridge.surface, area.pressed ? 1.15 : 1.40)
        visible: area.containsMouse
    }
    Glyph {
        anchors.centerIn: parent
        width: 14; height: 14
        fraction: 1.0
        kind: root.kind
        ink: CoreBridge.foreground
    }
    MouseArea {
        id: area
        anchors.fill: parent
        hoverEnabled: true
        onClicked: root.pressed()
    }
}
