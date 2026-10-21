import QtQuick
import comlink
import "fusion.js" as Fusion

/* A QToolButton with autoRaise, as Fusion paints it: nothing but its
   text or icon until the pointer is over it, and the sunken panel
   while pressed or checked. */
Item {
    id: root
    property string text: ""
    property color ink: CoreBridge.foreground
    property int pixelSize: 0
    property string icon: ""
    property color iconInk: CoreBridge.foreground
    property int iconSize: 16
    property bool checkable: false
    property bool checked: false
    property bool menuIndicator: false
    property string tip: ""
    signal clicked()

    implicitWidth: 22
    implicitHeight: 22

    readonly property bool hover: area.containsMouse && enabled
    readonly property bool sunk: (area.pressed && enabled) || checked

    Rectangle {
        anchors.fill: parent
        visible: root.hover || root.sunk
        radius: 2
        border.width: 1
        border.color: Fusion.outline(CoreBridge.background)
        gradient: Gradient {
            GradientStop {
                position: 0
                color: root.sunk ? Fusion.pressed(CoreBridge.surface)
                                 : Fusion.gradientTop(CoreBridge.surface, true)
            }
            GradientStop {
                position: 1
                color: root.sunk ? Fusion.pressed(CoreBridge.surface)
                                 : Fusion.gradientBottom(CoreBridge.surface,
                                                         true)
            }
        }
    }

    Text {
        visible: root.text !== ""
        anchors.centerIn: parent
        text: root.text
        color: root.enabled ? root.ink : CoreBridge.muted
        font: root.pixelSize > 0 ? Qt.font({ pixelSize: root.pixelSize })
                                 : Qt.font({ pointSize: CoreBridge.basePt })
    }
    Glyph {
        visible: root.icon !== ""
        anchors.centerIn: parent
        width: root.iconSize; height: root.iconSize
        fraction: 1.0
        kind: root.icon === "" ? "keypad" : root.icon
        ink: root.enabled ? root.iconInk : CoreBridge.muted
    }
    Canvas {
        visible: root.menuIndicator
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.rightMargin: 1
        anchors.bottomMargin: 3
        width: 7; height: 4
        onPaint: {
            const ctx = getContext("2d");
            ctx.reset();
            ctx.fillStyle = CoreBridge.muted;
            ctx.beginPath();
            ctx.moveTo(0, 0); ctx.lineTo(7, 0); ctx.lineTo(3.5, 4);
            ctx.closePath();
            ctx.fill();
        }
    }

    TipArea {
        id: area
        anchors.fill: parent
        tip: root.tip
        hoverEnabled: true
        enabled: root.enabled
        onClicked: {
            if (root.checkable)
                root.checked = !root.checked;
            root.clicked();
        }
    }
}
