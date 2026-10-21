import QtQuick
import comlink

/* The dialler: three by four, and the call row under it. The keys
   grow with the window but do not stop it shrinking; the call row
   keeps its own height and is always there.
 *
 * The call row is Call between two narrow buttons: video on the left,
 * a mock until video calls exist, and delete on the right, because a
 * mistyped digit on a handset had no way out but the soft keyboard. */
Item {
    id: root
    signal digit(string d)
    signal call()
    signal backspace()

    readonly property int side: 44

    Row {
        id: callRow
        anchors.bottom: parent.bottom
        width: parent.width
        height: 26
        spacing: 3
        PushButton {
            width: root.side; height: parent.height
            icon: "video"
            enabled: false
            tip: "Video calls are not available yet"
        }
        PushButton {
            width: parent.width - 2 * root.side - 2 * parent.spacing
            height: parent.height
            text: "Call"
            onClicked: root.call()
        }
        PushButton {
            width: root.side; height: parent.height
            icon: "backspace"
            tip: "Delete the last digit"
            onClicked: root.backspace()
        }
    }

    Grid {
        id: pad
        width: parent.width
        anchors.top: parent.top
        anchors.bottom: callRow.top
        anchors.bottomMargin: 6
        columns: 3
        spacing: 3
        readonly property real cw: (width - spacing * 2) / 3
        readonly property real ch: Math.max(26, (height - spacing * 3) / 4)

        Repeater {
            model: ["1","2","3","4","5","6","7","8","9","*","0","#"]
            PushButton {
                width: pad.cw
                height: pad.ch
                radius: 6
                text: modelData
                onClicked: root.digit(modelData)
            }
        }
    }
}
