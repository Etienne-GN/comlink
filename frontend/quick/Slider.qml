import QtQuick
import comlink
import "fusion.js" as Fusion

/* A horizontal QSlider as Fusion paints it: a five-pixel groove, the
   filled part in the accent, a small square handle.
 *
 * And as it behaves: the handle is dragged, a press beside it moves
 * one page towards the press, the wheel moves one step. */
Item {
    id: root
    property real value: 1.0
    property real from: 0.0
    property real to: 2.0
    property real step: 0.05
    property real pageStep: 0.2
    property string tip: ""
    signal moved(real value)

    implicitHeight: 16
    implicitWidth: 84

    readonly property real span: Math.max(0.0001, to - from)
    readonly property real fraction: Math.max(0, Math.min(1, (value - from) / span))
    readonly property int handle: 13
    readonly property real travel: width - handle

    function emitClamped(v) {
        const s = Math.round((v - from) / step) * step + from;
        const c = Math.max(from, Math.min(to, s));
        if (Math.abs(c - value) > 1e-9)
            moved(c);
    }

    Rectangle {
        id: groove
        anchors.verticalCenter: parent.verticalCenter
        width: parent.width
        height: 5
        radius: 1
        color: Qt.darker(Fusion.buttonColor(CoreBridge.surface), 1.10)
        border.width: 1
        border.color: Fusion.outline(CoreBridge.background)
        Rectangle {
            x: 1; y: 1
            height: 3
            width: Math.max(0, root.travel * root.fraction + root.handle / 2 - 1)
            visible: root.enabled
            gradient: Gradient {
                GradientStop { position: 0; color: Qt.lighter(CoreBridge.accent, 1.12) }
                GradientStop { position: 0.5; color: Qt.lighter(CoreBridge.accent, 1.08) }
                GradientStop { position: 1; color: Qt.lighter(CoreBridge.accent, 1.18) }
            }
        }
    }

    Rectangle {
        id: knob
        anchors.verticalCenter: parent.verticalCenter
        x: root.travel * root.fraction
        width: root.handle; height: root.handle
        radius: 2
        border.width: 1
        border.color: Fusion.outline(CoreBridge.background)
        gradient: Gradient {
            GradientStop { position: 0; color: Fusion.gradientTop(CoreBridge.surface, area.containsMouse) }
            GradientStop { position: 1; color: Fusion.gradientBottom(CoreBridge.surface, area.containsMouse) }
        }
        Rectangle {
            anchors.fill: parent
            anchors.margins: 1
            radius: 2
            color: "transparent"
            border.width: 1
            border.color: Fusion.innerContrast
        }
    }

    TipArea {
        id: area
        anchors.fill: parent
        tip: root.tip
        hoverEnabled: true
        enabled: root.enabled
        property bool dragging: false
        property real grab: 0
        onPressed: function(m) {
            if (m.x >= knob.x && m.x <= knob.x + knob.width) {
                dragging = true;
                grab = m.x - knob.x;
            } else {
                dragging = false;
                root.emitClamped(root.value
                                 + (m.x < knob.x ? -root.pageStep : root.pageStep));
            }
        }
        onPositionChanged: function(m) {
            if (!dragging || !pressed || root.travel <= 0)
                return;
            const f = Math.max(0, Math.min(1, (m.x - grab) / root.travel));
            root.emitClamped(root.from + f * root.span);
        }
        onReleased: dragging = false
        onWheel: function(w) {
            root.emitClamped(root.value + (w.angleDelta.y > 0 ? root.step
                                                              : -root.step));
        }
    }
}
