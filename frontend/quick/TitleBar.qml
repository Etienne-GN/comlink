import QtQuick
import QtQuick.Window
import comlink

/* Our own title bar, on the desktop.
 *
 * A port of frontend/qt/titlebar.cpp, for the reason that file gives:
 * Qt draws the window decoration itself on Wayland, because GNOME
 * provides no server-side decorations for xdg-shell, and both
 * available plugins ignore the application palette. A themed title
 * bar is not something they can be talked into -- it has to be drawn
 * here.
 *
 * On a handset there is no decoration to replace and no window to
 * move, so the buttons are simply absent and what is left is the name.
 */
Item {
    id: root
    readonly property bool desktop: Qt.platform.os !== "android"
    /* Which side the buttons belong on, read from the desktop rather
       than chosen: they were already on one side and moving them
       because we started drawing the bar would be a regression
       nobody asked for. */
    readonly property bool onLeft:
        root.desktop && CoreBridge.windowButtonsOnLeft()
    signal minimise()
    signal maximise()
    signal closed()

    implicitHeight: 26

    Rectangle {
        anchors.fill: parent
        color: CoreBridge.surface
    }
    /* A hairline under the bar, so it reads as a title bar and not as
       part of the first row of controls. */
    Rectangle {
        anchors.bottom: parent.bottom
        width: parent.width
        height: 1
        color: Qt.rgba(CoreBridge.foreground.r, CoreBridge.foreground.g,
                       CoreBridge.foreground.b, 40 / 255)
    }

    /* Dragging the bar moves the window and a double-click maximises
       it, which is what a title bar is for. The compositor does the
       move: a Wayland client cannot position itself. */
    MouseArea {
        anchors.fill: parent
        enabled: root.desktop
        onPressed: root.Window.window.startSystemMove()
        onDoubleClicked: root.maximise()
    }

    Row {
        id: leftButtons
        anchors.left: parent.left
        anchors.leftMargin: 2
        height: parent.height
        visible: root.desktop && root.onLeft
        /* GNOME's order for a left layout is close, minimise,
           maximise, which is what the operator was already using. */
        WindowButton { kind: "winClose"; danger: true
                       onPressed: root.closed() }
        WindowButton { kind: "winMin"; onPressed: root.minimise() }
        WindowButton { kind: root.Window.window
                             && root.Window.window.visibility === Window.Maximized
                             ? "winRestore" : "winMax"
                       onPressed: root.maximise() }
    }

    Text {
        anchors.verticalCenter: parent.verticalCenter
        anchors.left: root.onLeft ? leftButtons.right : parent.left
        anchors.leftMargin: root.onLeft ? 8 : 6
        anchors.right: root.onLeft ? parent.right : rightButtons.left
        text: "Comlink"
        color: CoreBridge.foreground
        font.pointSize: CoreBridge.basePt
        font.weight: Font.DemiBold
        elide: Text.ElideRight
    }

    Row {
        id: rightButtons
        anchors.right: parent.right
        height: parent.height
        visible: root.desktop && !root.onLeft
        WindowButton { kind: "winMin"; onPressed: root.minimise() }
        WindowButton { kind: root.Window.window
                             && root.Window.window.visibility === Window.Maximized
                             ? "winRestore" : "winMax"
                       onPressed: root.maximise() }
        WindowButton { kind: "winClose"; danger: true
                       onPressed: root.closed() }
    }
}
