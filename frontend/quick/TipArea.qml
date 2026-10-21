import QtQuick
import comlink

/* A MouseArea that also carries the control's tooltip, shown after
   the pointer rests on it the way a QToolTip is. A handset has no
   pointer to rest, so there it is only the MouseArea. */
MouseArea {
    id: root
    property string tip: ""
    hoverEnabled: tip !== ""

    Timer {
        id: wait
        interval: 700
        onTriggered: bubble.visible = root.containsMouse && root.tip !== ""
    }
    onContainsMouseChanged: {
        if (containsMouse) wait.restart();
        else { wait.stop(); bubble.visible = false }
    }
    onPressed: { wait.stop(); bubble.visible = false }

    Rectangle {
        id: bubble
        visible: false
        parent: root.Window.window ? (root.Window.window.overlayLayer || root.Window.window.contentItem) : root
        z: 200
        readonly property point at: root.mapToItem(parent, root.mouseX,
                                                   root.height)
        width: Math.min(tipText.implicitWidth + 10,
                        parent ? parent.width - 8 : 300)
        height: tipText.implicitHeight + 6
        x: Math.max(4, Math.min(at.x, (parent ? parent.width : 0) - width - 4))
        y: at.y + 4 + height > (parent ? parent.height : 0)
           ? at.y - root.height - height - 4 : at.y + 4
        color: CoreBridge.surface
        border.width: 1
        border.color: CoreBridge.muted
        Text {
            id: tipText
            x: 5; y: 3
            width: Math.min(implicitWidth, 290)
            text: root.tip
            color: CoreBridge.foreground
            font.pointSize: CoreBridge.basePt * 0.9
            wrapMode: Text.WordWrap
        }
    }
}
