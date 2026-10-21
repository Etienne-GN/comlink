import QtQuick
import QtQuick.Window
import comlink
import "fusion.js" as Fusion

/* A non-editable QComboBox as Fusion paints it: button chrome from
   fusion.js, a left-aligned label, a triangular arrow, and a dropdown
   list in the theme's colours -- the same list style Main.qml's
   account combo already drops down.
 *
 * model is a list of strings, or of { label, value } objects; a plain
 * string stands for itself as both. */
Item {
    id: root
    property var model: []
    property int currentIndex: -1
    property string tip: ""
    signal activated(int index)

    implicitHeight: 24
    implicitWidth: 120
    opacity: root.enabled ? 1.0 : 0.6

    function labelAt(i) {
        if (i < 0 || i >= root.model.length)
            return "";
        const m = root.model[i];
        return (typeof m === "string") ? m : m.label;
    }
    function valueAt(i) {
        if (i < 0 || i >= root.model.length)
            return undefined;
        const m = root.model[i];
        return (typeof m === "string") ? m : m.value;
    }

    readonly property bool hover: area.containsMouse && root.enabled

    Rectangle {
        anchors.fill: parent
        radius: 2
        border.width: 1
        border.color: Fusion.outline(CoreBridge.background)
        gradient: Gradient {
            GradientStop { position: 0; color: Fusion.gradientTop(CoreBridge.surface, root.hover) }
            GradientStop { position: 1; color: Fusion.gradientBottom(CoreBridge.surface, root.hover) }
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

    Text {
        anchors.verticalCenter: parent.verticalCenter
        x: 6
        width: parent.width - 12 - 16
        text: root.labelAt(root.currentIndex)
        color: root.enabled ? CoreBridge.foreground : CoreBridge.muted
        font.pointSize: CoreBridge.basePt
        elide: Text.ElideRight
    }

    Canvas {
        anchors.verticalCenter: parent.verticalCenter
        anchors.right: parent.right
        anchors.rightMargin: 8
        width: 8; height: 5
        property color ink: root.enabled ? CoreBridge.foreground : CoreBridge.muted
        onInkChanged: requestPaint()
        onPaint: {
            const ctx = getContext("2d");
            ctx.reset();
            ctx.fillStyle = Qt.rgba(ink.r, ink.g, ink.b, 0.8);
            ctx.beginPath();
            ctx.moveTo(0, 0); ctx.lineTo(8, 0); ctx.lineTo(4, 5);
            ctx.closePath(); ctx.fill();
        }
    }

    TipArea {
        id: area
        anchors.fill: parent
        tip: root.tip
        hoverEnabled: true
        enabled: root.enabled
        onClicked: {
            if (!overlay.visible) {
                /* Measured at the moment it opens. A binding on
                   mapToItem is not re-evaluated when an ancestor
                   moves, so it kept the place the combo had when its
                   page was built -- the left edge, before the dialog
                   laid the page out beside its sidebar. */
                const p = root.mapToItem(overlay, 0, root.height);
                box.atX = p.x;
                box.atY = p.y;
            }
            overlay.visible = !overlay.visible;
        }
    }

    Item {
        id: overlay
        visible: false
        parent: root.Window.window ? (root.Window.window.overlayLayer || root.Window.window.contentItem) : root
        anchors.fill: parent
        z: 150

        MouseArea {
            anchors.fill: parent
            onPressed: overlay.visible = false
        }

        /* QComboBox's maxVisibleItems: ten rows, and the rest scroll.
           A list taller than the window drawn whole ran off its edge
           and hid the rows that could not be reached. */
        Rectangle {
            id: box
            readonly property int rows: Math.min(root.model.length, 10)
            property real atX: 0
            property real atY: 0
            x: Math.max(0, Math.min(atX, overlay.width - width))
            y: atY + height <= overlay.height ? atY
               : Math.max(0, atY - root.height - height)
            width: root.width
            height: Math.min(rows * 22, overlay.height) + 2
            color: CoreBridge.surface
            border.width: 1
            border.color: Fusion.outline(CoreBridge.background)

            ListView {
                id: list
                x: 1; y: 1
                width: parent.width - 2
                height: parent.height - 2
                clip: true
                model: root.model
                boundsBehavior: Flickable.StopAtBounds
                currentIndex: root.currentIndex
                onVisibleChanged: if (visible) positionViewAtIndex(Math.max(0, root.currentIndex),
                                                                    ListView.Contain)
                delegate: Rectangle {
                    width: ListView.view.width - (bar.visible ? bar.width : 0)
                    height: 22
                    color: rowArea.containsMouse || index === root.currentIndex
                           ? CoreBridge.accent : "transparent"
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        x: 6
                        width: parent.width - 10
                        text: (typeof modelData === "string") ? modelData : modelData.label
                        color: rowArea.containsMouse || index === root.currentIndex
                               ? (CoreBridge.dark ? "#000000" : "#FFFFFF")
                               : CoreBridge.foreground
                        font.pointSize: CoreBridge.basePt
                        elide: Text.ElideRight
                    }
                    MouseArea {
                        id: rowArea
                        anchors.fill: parent
                        hoverEnabled: true
                        onClicked: {
                            root.currentIndex = index;
                            overlay.visible = false;
                            root.activated(index);
                        }
                    }
                }
            }
            /* A thin scroll bar, when there is more than fits. */
            Rectangle {
                id: bar
                visible: list.contentHeight > list.height
                anchors.right: parent.right
                anchors.rightMargin: 2
                y: 1 + (list.height - height) * (list.contentY
                       / Math.max(1, list.contentHeight - list.height))
                width: 6
                height: Math.max(16, list.height * list.height / Math.max(1, list.contentHeight))
                radius: 3
                color: CoreBridge.muted
                opacity: 0.7
            }
        }
    }
}
