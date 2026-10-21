import QtQuick
import comlink
import "fusion.js" as Fusion

/* A QCheckBox as Fusion paints it: a small sunken box with a check
   mark (or a dash for "partly checked, leave it to the codec"), and a
   label beside it. The whole row is the click target, the same as a
   real QCheckBox's label. */
Item {
    id: root
    property string text: ""
    property bool checked: false
    property bool tristate: false
    /* 0 unchecked, 1 checked, 2 partially checked. Source of truth
       when tristate is set; mirrors checked otherwise. */
    property int checkState: checked ? 1 : 0
    property string tip: ""
    signal toggled(bool on)
    /* Not stateChanged: Item already has one, for its own State/
       Transition state property, and a redeclaration is a QML error
       rather than a shadow. */
    signal stateEdited(int state)

    readonly property int boxSize: 15
    /* A label that runs to a second line rather than losing its end,
       where the width is a handset's. */
    property bool wrap: false

    implicitHeight: Math.max(boxSize, label.implicitHeight)
    implicitWidth: boxSize + 6 + label.implicitWidth
    opacity: root.enabled ? 1.0 : 0.6

    Rectangle {
        id: box
        width: root.boxSize; height: root.boxSize
        anchors.left: parent.left
        anchors.verticalCenter: parent.verticalCenter
        radius: 2
        color: CoreBridge.surface
        border.width: 1
        border.color: Fusion.outline(CoreBridge.background)

        Text {
            anchors.centerIn: parent
            visible: root.tristate ? root.checkState === 1 : root.checked
            text: "✓"
            color: CoreBridge.foreground
            font.pixelSize: 12
            font.bold: true
        }
        /* "Partly checked", which Fusion draws as a filled square a
           little inside the frame rather than as a dash: the text
           colour a shade darker, fading in from the top edge to the
           bottom, and an outline of the same ink that is brighter than the
           fill, as the stroke lands on top of it there. */
        Rectangle {
            id: partial
            readonly property color ink: Qt.darker(CoreBridge.foreground, 1.2)
            visible: root.tristate && root.checkState === 2
            x: 2; y: 2
            width: 10; height: 10
            gradient: Gradient {
                GradientStop { position: 0
                               color: Qt.rgba(partial.ink.r, partial.ink.g,
                                              partial.ink.b, 80 / 255) }
                GradientStop { position: 1
                               color: Qt.rgba(partial.ink.r, partial.ink.g,
                                              partial.ink.b, 140 / 255) }
            }
            border.width: 1
            border.color: Qt.rgba(partial.ink.r, partial.ink.g,
                                 partial.ink.b, 195 / 255)
        }
    }
    Text {
        id: label
        anchors.left: box.right
        anchors.leftMargin: 6
        anchors.right: parent.right
        anchors.verticalCenter: parent.verticalCenter
        text: root.text
        color: CoreBridge.foreground
        font.pointSize: CoreBridge.basePt
        elide: root.wrap ? Text.ElideNone : Text.ElideRight
        wrapMode: root.wrap ? Text.WordWrap : Text.NoWrap
    }
    TipArea {
        anchors.fill: parent
        tip: root.tip
        hoverEnabled: true
        enabled: root.enabled
        onClicked: {
            /* Reports the change rather than setting checked/checkState
               itself: the caller's binding is the source of truth (the
               modules page reverts it on a failure), the same contract
               Toggle.qml keeps for its row switch. */
            if (root.tristate)
                root.stateEdited((root.checkState + 1) % 3);
            else
                root.toggled(!root.checked);
        }
    }
}
