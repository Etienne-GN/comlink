import QtQuick
import comlink

/* The window's own account of what it has been doing, one timestamped
   line each, in a small fixed face. Wrapped rather than scrolled
   sideways; the newest line at the bottom. */
Rectangle {
    id: root
    color: CoreBridge.surface
    border.width: 1
    border.color: Qt.rgba(CoreBridge.foreground.r, CoreBridge.foreground.g,
                          CoreBridge.foreground.b, 70 / 255)

    property string lines: CoreBridge.windowLog().join("\n")
    Connections {
        target: CoreBridge
        function onLogged(line) {
            root.lines = root.lines === "" ? line : root.lines + "\n" + line;
            if (flick.atYEnd || flick.contentHeight <= flick.height)
                Qt.callLater(flick.toEnd);
        }
    }

    Flickable {
        id: flick
        anchors.fill: parent
        anchors.margins: 4
        anchors.rightMargin: 4 + 14
        contentHeight: log.implicitHeight
        clip: true
        function toEnd() { contentY = Math.max(0, contentHeight - height) }
        TextEdit {
            id: log
            width: flick.width
            readOnly: true
            selectByMouse: true
            selectByKeyboard: true
            wrapMode: TextEdit.Wrap
            color: CoreBridge.foreground
            selectionColor: CoreBridge.accent
            selectedTextColor: CoreBridge.dark ? "#000000" : "#FFFFFF"
            font.family: "monospace"
            font.pixelSize: 10
            text: root.lines
        }
    }

    /* A wider scrollbar, because the default is hard to grab on a
       narrow window. */
    Rectangle {
        anchors.right: parent.right
        anchors.rightMargin: 2
        visible: flick.contentHeight > flick.height
        y: 4 + (flick.height - height) * (flick.contentY
               / Math.max(1, flick.contentHeight - flick.height))
        width: 10
        height: Math.max(20, flick.height * flick.height / Math.max(1, flick.contentHeight))
        radius: 3
        color: CoreBridge.muted
        opacity: 0.6
        MouseArea {
            anchors.fill: parent
            drag.target: null
            property real startY: 0
            property real startContent: 0
            onPressed: function(m) { startY = m.y + parent.y; startContent = flick.contentY }
            onPositionChanged: function(m) {
                const travel = flick.height - parent.height;
                if (travel <= 0) return;
                const dy = m.y + parent.y - startY;
                flick.contentY = Math.max(0, Math.min(flick.contentHeight - flick.height,
                    startContent + dy * (flick.contentHeight - flick.height) / travel));
            }
        }
    }
}
