import QtQuick
import comlink
import "fusion.js" as Fusion

/* A QPushButton as Fusion paints it in the theme's palette: the
   two-stop gradient, the dark outline, the faint inner line, two-pixel
   corners. Text, an icon, or both.
 *
 * menuIndicator is the small arrow a QToolButton with a menu draws in
 * its corner; the headers button is one. */
Item {
    id: root
    property string text: ""
    property color ink: CoreBridge.foreground
    property bool bold: false
    property string tip: ""
    property real pointSize: CoreBridge.basePt
    property string icon: ""
    property color iconInk: CoreBridge.foreground
    property real glow: 0.0
    property int iconSize: 17
    property bool menuIndicator: false
    property bool flat: false
    /* Painted over the gradient while set: the recording pulse. */
    property color tint: "transparent"
    property color tintBorder: "transparent"
    property real radius: 2
    signal clicked()

    implicitHeight: Math.max(24, label.implicitHeight + 8)
    implicitWidth: Math.max(80, label.implicitWidth + 16
                                + (icon !== "" ? iconSize + 4 : 0))
    opacity: 1.0

    readonly property bool hover: area.containsMouse && enabled
    readonly property bool down: area.pressed && enabled

    Rectangle {
        anchors.fill: parent
        radius: root.radius
        border.width: 1
        border.color: root.enabled
                      ? Fusion.outline(CoreBridge.background)
                      : Qt.lighter(Fusion.outline(CoreBridge.background), 1.15)
        gradient: Gradient {
            GradientStop {
                position: 0
                color: root.down ? Fusion.pressed(CoreBridge.surface)
                                 : Fusion.gradientTop(CoreBridge.surface,
                                                      root.hover)
            }
            GradientStop {
                position: 1
                color: root.down ? Fusion.pressed(CoreBridge.surface)
                                 : Fusion.gradientBottom(CoreBridge.surface,
                                                         root.hover)
            }
        }
        Rectangle {
            anchors.fill: parent
            anchors.margins: 1
            radius: Math.max(2, root.radius - 1)
            color: "transparent"
            border.width: 1
            border.color: Fusion.innerContrast
        }
        Rectangle {
            anchors.fill: parent
            radius: root.radius + 1
            visible: root.tint.a > 0
            color: root.tint
            border.width: 1
            border.color: root.tintBorder
        }
    }

    Row {
        anchors.centerIn: parent
        spacing: 4
        Glyph {
            visible: root.icon !== ""
            anchors.verticalCenter: parent.verticalCenter
            width: root.iconSize; height: root.iconSize
            fraction: 1.0
            kind: root.icon === "" ? "keypad" : root.icon
            ink: root.enabled ? root.iconInk : CoreBridge.muted
            glow: root.glow
        }
        Text {
            id: label
            visible: root.text !== ""
            anchors.verticalCenter: parent.verticalCenter
            text: root.text
            color: root.enabled ? root.ink : CoreBridge.muted
            font.pointSize: root.pointSize
            font.bold: root.bold
            elide: Text.ElideRight
            width: Math.min(implicitWidth, root.width - 8
                            - (root.icon !== "" ? root.iconSize + 4 : 0))
        }
    }

    Canvas {
        visible: root.menuIndicator
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.rightMargin: 3
        anchors.bottomMargin: 4
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
        onClicked: root.clicked()
    }
}
