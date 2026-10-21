import QtQuick
import comlink

/* What every Settings page is laid out in: its sections in a column
   that scrolls when the page is taller than the window -- a handset
   always is -- and an optional footer pinned below it, for the one
   button a page applies with and the line that reports what it did. */
Item {
    id: root
    default property alias content: col.data
    property alias footer: foot.data

    Flickable {
        id: flick
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: foot.top
        anchors.bottomMargin: foot.children.length > 0 ? 8 : 0
        contentHeight: col.implicitHeight
        boundsBehavior: Flickable.StopAtBounds
        clip: true

        Column {
            id: col
            width: flick.width - (bar.visible ? bar.width + 4 : 0)
            spacing: 12
        }
    }
    /* The same thin bar ComboBox's list scrolls with. */
    Rectangle {
        id: bar
        visible: flick.contentHeight > flick.height
        anchors.right: flick.right
        y: flick.y + (flick.height - height) * (flick.contentY
               / Math.max(1, flick.contentHeight - flick.height))
        width: 6
        height: Math.max(16, flick.height * flick.height
                             / Math.max(1, flick.contentHeight))
        radius: 3
        color: CoreBridge.muted
        opacity: 0.7
    }

    Column {
        id: foot
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        spacing: 6
    }
}
