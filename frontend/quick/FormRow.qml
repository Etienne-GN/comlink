import QtQuick
import comlink

/* A label and the control it names: side by side where there is the
   width, the label above the control where there is not -- a handset
   gives a control beside an 84-pixel label too little to show its
   value. The control is the child, and takes the width it is left. */
Item {
    id: root
    property string text: ""
    property int labelWidth: 84
    property bool dim: false
    default property alias content: holder.children

    readonly property bool stacked: width < 300

    width: parent ? parent.width : 0
    implicitHeight: stacked ? label.implicitHeight + 2 + holder.height
                            : Math.max(label.implicitHeight, holder.height)

    Text {
        id: label
        width: root.stacked ? root.width : root.labelWidth
        y: root.stacked ? 0 : (root.height - implicitHeight) / 2
        text: root.text
        color: root.dim ? CoreBridge.muted : CoreBridge.foreground
        font.pointSize: CoreBridge.basePt
        elide: Text.ElideRight
    }
    Item {
        id: holder
        x: root.stacked ? 0 : root.labelWidth + 6
        y: root.stacked ? label.implicitHeight + 2
                        : (root.height - height) / 2
        width: root.width - x
        height: childrenRect.height
    }
}
