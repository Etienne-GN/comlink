import QtQuick
import comlink

/* A QMenu, drawn in the theme's colours: rows of text, separators, and
   submenus that open beside the row that holds them.
 *
 * Placed over the whole window so a press anywhere outside closes it,
 * the way a QMenu does. Each item is
 *   { text, action, enabled, visible, separator, sub: [items] }
 * with enabled and visible true unless said otherwise. */
Item {
    id: root
    anchors.fill: parent
    visible: false
    z: 100

    property var items: []
    property real anchorX: 0
    property real anchorY: 0
    property var subItems: []
    property real subY: 0

    function openAt(x, y) {
        anchorX = x;
        anchorY = y;
        subItems = [];
        visible = true;
    }
    function close() {
        subItems = [];
        visible = false;
    }
    function shown(list) {
        return list.filter(function(i) { return i.visible !== false });
    }

    MouseArea {
        anchors.fill: parent
        onPressed: root.close()
    }

    component MenuBox: Rectangle {
        id: box
        property var entries: []
        property bool isSub: false
        width: Math.max(120, list.maxText + 48)
        height: list.implicitHeight + 8
        color: CoreBridge.surface
        border.width: 1
        border.color: CoreBridge.muted
        radius: 2

        MouseArea { anchors.fill: parent }

        Column {
            id: list
            y: 4
            width: parent.width
            property real maxText: 0

            Repeater {
                model: root.shown(box.entries)
                Item {
                    id: row
                    width: list.width
                    height: modelData.separator === true ? 7 : 24
                    readonly property bool usable:
                        modelData.separator !== true
                        && modelData.enabled !== false

                    Rectangle {
                        visible: modelData.separator === true
                        anchors.verticalCenter: parent.verticalCenter
                        x: 6
                        width: parent.width - 12
                        height: 1
                        color: CoreBridge.muted
                        opacity: 0.6
                    }
                    Rectangle {
                        anchors.fill: parent
                        anchors.leftMargin: 2
                        anchors.rightMargin: 2
                        visible: row.usable && (hover.containsMouse
                                 || (modelData.sub !== undefined
                                     && root.subItems === modelData.sub))
                        color: CoreBridge.accent
                        radius: 2
                    }
                    Text {
                        id: label
                        visible: modelData.separator !== true
                        anchors.verticalCenter: parent.verticalCenter
                        x: 24
                        text: modelData.text || ""
                        font.pointSize: CoreBridge.basePt
                        color: !row.usable ? CoreBridge.muted
                               : hover.containsMouse
                                 ? (CoreBridge.dark ? "#000000" : "#FFFFFF")
                                 : CoreBridge.foreground
                        Component.onCompleted:
                            list.maxText = Math.max(list.maxText, implicitWidth)
                    }
                    Text {
                        visible: modelData.sub !== undefined
                        anchors.verticalCenter: parent.verticalCenter
                        anchors.right: parent.right
                        anchors.rightMargin: 8
                        text: "▸"
                        font.pointSize: CoreBridge.basePt
                        color: label.color
                    }
                    MouseArea {
                        id: hover
                        anchors.fill: parent
                        hoverEnabled: true
                        enabled: row.usable
                        onEntered: {
                            if (box.isSub)
                                return;
                            if (modelData.sub !== undefined) {
                                root.subItems = modelData.sub;
                                root.subY = box.y + row.y + list.y - 4;
                            } else {
                                root.subItems = [];
                            }
                        }
                        onClicked: {
                            if (modelData.sub !== undefined) {
                                root.subItems = modelData.sub;
                                root.subY = box.y + row.y + list.y - 4;
                                return;
                            }
                            const act = modelData.action;
                            root.close();
                            if (act)
                                act();
                        }
                    }
                }
            }
        }
    }

    MenuBox {
        id: main
        entries: root.items
        x: Math.max(0, Math.min(root.anchorX, root.width - width))
        y: Math.max(0, Math.min(root.anchorY, root.height - height))
    }

    MenuBox {
        id: side
        isSub: true
        visible: root.subItems.length > 0
        entries: root.subItems
        /* Beside the parent when there is room, on the other side when
           there is not, and over it on a screen too narrow for
           either. */
        x: main.x + main.width + width <= root.width
           ? main.x + main.width - 2
           : (main.x - width >= 0 ? main.x - width + 2
                                  : Math.max(0, root.width - width))
        y: Math.max(0, Math.min(root.subY, root.height - height))
    }
}
