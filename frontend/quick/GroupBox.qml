import QtQuick
import comlink

/* A QGroupBox as Fusion paints it: the title above a thin frame.
   Children go in the default property, laid out as a column with the
   padding the widget gives QFormLayout. A description, when given,
   is the first line inside the frame: what the group is for, in
   words, rather than only in a tooltip a handset cannot hover. */
Item {
    id: root
    property string text: ""
    property string description: ""
    default property alias content: body.children

    implicitHeight: title.height + frame.height

    Text {
        id: title
        width: parent.width
        text: root.text
        color: CoreBridge.foreground
        font.pointSize: CoreBridge.basePt
        font.weight: Font.DemiBold
        elide: Text.ElideRight
        height: implicitHeight + 4
    }
    Rectangle {
        id: frame
        anchors.top: title.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        height: inner.implicitHeight + 12
        radius: 2
        /* Fusion's frame is not a coloured line but a lift off the
           background: light-on-dark or dark-on-light by a few percent,
           with a fainter shade just inside it and the whole interior
           a hair darker, so it follows every preset without a colour
           of its own. */
        color: Qt.rgba(0, 0, 0, 3 / 255)
        border.width: 1
        border.color: CoreBridge.dark ? Qt.rgba(1, 1, 1, 20 / 255)
                                      : Qt.rgba(0, 0, 0, 26 / 255)
        Rectangle {
            anchors.fill: parent
            anchors.margins: 1
            radius: 1
            color: "transparent"
            border.width: 1
            border.color: Qt.rgba(0, 0, 0, 6 / 255)
        }
    }
    Column {
        id: inner
        anchors.top: frame.top
        anchors.topMargin: 6
        x: 8
        width: parent.width - 16
        spacing: 6

        Text {
            width: parent.width
            visible: root.description !== ""
            text: root.description
            color: CoreBridge.muted
            font.pointSize: CoreBridge.basePt * 0.9
            wrapMode: Text.WordWrap
        }
        Column {
            id: body
            width: parent.width
            spacing: 4
        }
    }
}
