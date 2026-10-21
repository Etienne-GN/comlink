import QtQuick
import QtQuick.Window
import comlink

/* One edge of a frameless window, as a drag target.
 *
 * A port of frontend/qt/resizegrip.cpp. Nothing is painted: the
 * window's own background shows through, so the border stays whatever
 * the theme drew. The compositor resizes -- a Wayland client cannot
 * resize itself.
 */
MouseArea {
    id: root
    /* Which edge this strip is, as Qt.Edges. */
    property int base: Qt.BottomEdge
    /* How far from the end of a strip still counts as its corner.
       Larger than the strip is thick, because a corner is a small
       target and missing it drags one edge instead of two. */
    readonly property int corner: 16

    hoverEnabled: true

    function edgesAt(x, y) {
        let e = base;
        if (base & (Qt.TopEdge | Qt.BottomEdge)) {
            if (x < corner) e |= Qt.LeftEdge;
            else if (x > width - corner) e |= Qt.RightEdge;
        } else {
            if (y < corner) e |= Qt.TopEdge;
            else if (y > height - corner) e |= Qt.BottomEdge;
        }
        return e;
    }

    cursorShape: {
        const e = edgesAt(mouseX, mouseY);
        if (e === (Qt.LeftEdge | Qt.TopEdge)
            || e === (Qt.RightEdge | Qt.BottomEdge))
            return Qt.SizeFDiagCursor;
        if (e === (Qt.RightEdge | Qt.TopEdge)
            || e === (Qt.LeftEdge | Qt.BottomEdge))
            return Qt.SizeBDiagCursor;
        if (e & (Qt.LeftEdge | Qt.RightEdge))
            return Qt.SizeHorCursor;
        return Qt.SizeVerCursor;
    }

    onPressed: function(m) {
        root.Window.window.startSystemResize(edgesAt(m.x, m.y));
    }
}
