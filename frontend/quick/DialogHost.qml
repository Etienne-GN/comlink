import QtQuick
import QtQuick.Window
import comlink

/* Where a secondary window of the Qt window opens.
 *
 * On the desktop it is its own top-level window, with the title, size
 * and modality the Qt dialog had. A handset cannot put a second
 * window beside the first, so there it covers the whole app, as the
 * call log does, with its title and a close mark along the top.
 *
 * The content is a Component; it reaches this host through a `host`
 * property if it declares one, so it can close itself. */
Item {
    id: host
    property string title: ""
    property int dialogWidth: 400
    property int dialogHeight: 300
    property int minimumWidth: 0
    property int minimumHeight: 0
    property bool modal: false
    property Component content
    property bool shown: false
    readonly property Item item: desktop ? (win.item ? win.item.body : null)
                                         : overlayBody.item
    readonly property bool desktop: Qt.platform.os !== "android"
    signal closed()

    function open() { shown = true }
    function close() {
        if (!shown)
            return;
        shown = false;
        closed();
    }

    Loader {
        id: win
        active: host.desktop && host.shown
        sourceComponent: Window {
            property alias body: body.item
            title: host.title
            width: host.dialogWidth
            height: host.dialogHeight
            minimumWidth: host.minimumWidth
            minimumHeight: host.minimumHeight
            modality: host.modal ? Qt.ApplicationModal : Qt.NonModal
            color: CoreBridge.background
            visible: true
            onClosing: host.close()
            Loader {
                id: body
                anchors.fill: parent
                anchors.margins: 9
                sourceComponent: host.content
                onLoaded: if (item.hasOwnProperty("host")) item.host = host
            }
        }
    }

    Rectangle {
        id: overlay
        visible: !host.desktop && host.shown
        parent: host.Window.window ? (host.Window.window.overlayLayer || host.Window.window.contentItem) : host
        anchors.fill: parent
        z: 60
        color: CoreBridge.background
        MouseArea { anchors.fill: parent }

        Item {
            id: bar
            width: parent.width
            height: 40
            Text {
                anchors.verticalCenter: parent.verticalCenter
                x: 12
                width: parent.width - 60
                text: host.title
                color: CoreBridge.foreground
                font.pointSize: CoreBridge.basePt * 1.1
                font.weight: Font.DemiBold
                elide: Text.ElideRight
            }
            Glyph {
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                width: 40; height: 40
                kind: "winClose"
                ink: CoreBridge.foreground
                fraction: 0.45
                MouseArea { anchors.fill: parent; onClicked: host.close() }
            }
        }
        Loader {
            id: overlayBody
            active: !host.desktop && host.shown
            anchors.top: bar.bottom
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            anchors.margins: 9
            sourceComponent: host.content
            onLoaded: if (item.hasOwnProperty("host")) item.host = host
        }
    }
}
