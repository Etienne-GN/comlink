import QtQuick
import comlink

/* The surface card the whole interface is built from: a filled
   rectangle, a one-pixel border in the muted ink, three-pixel
   corners. The Qt window's grammar, in one place so no component
   invents its own. */
Rectangle {
    radius: 3
    color: CoreBridge.surface
    border.width: 1
    border.color: CoreBridge.muted
}
