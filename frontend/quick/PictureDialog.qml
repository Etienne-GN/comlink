import QtQuick
import comlink

/* The crop stage of picturedialog.cpp's PictureDialog: a square
 * selection dragged and resized over the chosen picture, the surround
 * dimmed, then OK/Cancel. The file chooser that comes before this is
 * ContactEditor.qml's own FileDialog -- runFor() is split the same
 * way in spirit, native chooser then this modal crop step.
 *
 * Everything here reasons in the source image's own pixels (root.cropX
 * /cropY/cropEdge), which is the space CoreBridge.commitPicture and
 * stagedPreview both expect; only the on-screen rectangles are ever
 * converted, and only for drawing and hit-testing. */
Item {
    id: root
    property var host
    anchors.fill: parent

    property string source: ""
    property int srcWidth: 0
    property int srcHeight: 0
    property real cropX: 0
    property real cropY: 0
    property real cropEdge: 1

    signal accepted(int x, int y, int edge)
    signal cancelled()

    function load(url) {
        root.source = url;
        const info = CoreBridge.stagedPicture();
        root.srcWidth = info.width || 0;
        root.srcHeight = info.height || 0;
        const edge = Math.max(1, Math.min(root.srcWidth, root.srcHeight));
        root.cropEdge = edge;
        root.cropX = (root.srcWidth - edge) / 2;
        root.cropY = (root.srcHeight - edge) / 2;
    }

    /* The one place a square is trusted to still be a square fully
       inside the image -- picturedialog.cpp's clampSquareToImage(),
       called after every drag, resize and wheel change. */
    function clampCrop() {
        if (root.srcWidth <= 0 || root.srcHeight <= 0)
            return;
        const maxEdge = Math.min(root.srcWidth, root.srcHeight);
        const minEdge = Math.min(16, maxEdge);
        const edge = Math.max(minEdge, Math.min(root.cropEdge, maxEdge));
        root.cropX = Math.max(0, Math.min(root.cropX, root.srcWidth - edge));
        root.cropY = Math.max(0, Math.min(root.cropY, root.srcHeight - edge));
        root.cropEdge = edge;
    }

    Text {
        id: title
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        text: "Crop the picture"
        color: CoreBridge.foreground
        font.pointSize: CoreBridge.basePt * 1.1
        font.weight: Font.DemiBold
    }

    Rectangle {
        id: area
        anchors.top: title.bottom
        anchors.topMargin: 6
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: hint.top
        anchors.bottomMargin: 6
        clip: true
        color: CoreBridge.surface
        border.width: 1
        border.color: CoreBridge.muted

        Image {
            id: img
            anchors.fill: parent
            visible: root.source !== ""
            source: root.source
            fillMode: Image.PreserveAspectFit
            smooth: true
        }

        readonly property real dispX: (area.width - img.paintedWidth) / 2
        readonly property real dispY: (area.height - img.paintedHeight) / 2
        readonly property real scale: root.srcWidth > 0
            ? img.paintedWidth / root.srcWidth : 1

        function toWidget(r) {
            return Qt.rect(area.dispX + r.x * area.scale,
                           area.dispY + r.y * area.scale,
                           r.width * area.scale, r.height * area.scale);
        }
        function toImagePt(x, y) {
            const ix = (x - area.dispX) / area.scale;
            const iy = (y - area.dispY) / area.scale;
            return Qt.point(Math.max(0, Math.min(root.srcWidth, ix)),
                            Math.max(0, Math.min(root.srcHeight, iy)));
        }

        readonly property rect wcrop: area.toWidget(
            Qt.rect(root.cropX, root.cropY, root.cropEdge, root.cropEdge))

        /* The dimmed surround, as four bands rather than a path
           subtraction -- QML has no fillPath(outer.subtracted(inner)),
           and four rectangles read the same. */
        Rectangle {
            color: "#96000000"
            x: 0; y: 0; width: area.width; height: area.wcrop.y
        }
        Rectangle {
            color: "#96000000"
            x: 0; y: area.wcrop.y + area.wcrop.height
            width: area.width
            height: Math.max(0, area.height - y)
        }
        Rectangle {
            color: "#96000000"
            x: 0; y: area.wcrop.y
            width: area.wcrop.x; height: area.wcrop.height
        }
        Rectangle {
            color: "#96000000"
            x: area.wcrop.x + area.wcrop.width; y: area.wcrop.y
            width: Math.max(0, area.width - x); height: area.wcrop.height
        }

        Rectangle {
            x: area.wcrop.x; y: area.wcrop.y
            width: area.wcrop.width; height: area.wcrop.height
            color: "transparent"
            border.width: 2
            border.color: "white"
        }
        Repeater {
            model: [
                { x: area.wcrop.x, y: area.wcrop.y },
                { x: area.wcrop.x + area.wcrop.width, y: area.wcrop.y },
                { x: area.wcrop.x, y: area.wcrop.y + area.wcrop.height },
                { x: area.wcrop.x + area.wcrop.width,
                  y: area.wcrop.y + area.wcrop.height }
            ]
            Rectangle {
                x: modelData.x - 4; y: modelData.y - 4
                width: 8; height: 8
                color: "white"
            }
        }

        MouseArea {
            id: drag
            anchors.fill: parent
            property int mode: 0  // 0 none, 1 move, 2 resize
            property point pressImagePt
            property point moveStart
            property point resizeAnchor

            function hitCorner(mx, my) {
                const wr = area.wcrop;
                const pts = [Qt.point(wr.x, wr.y),
                            Qt.point(wr.x + wr.width, wr.y),
                            Qt.point(wr.x, wr.y + wr.height),
                            Qt.point(wr.x + wr.width, wr.y + wr.height)];
                for (let i = 0; i < 4; i++) {
                    const dx = pts[i].x - mx, dy = pts[i].y - my;
                    if (Math.abs(dx) + Math.abs(dy) <= 10)
                        return i;
                }
                return -1;
            }
            function opposite(corner) {
                const r = { x: root.cropX, y: root.cropY,
                           w: root.cropEdge, h: root.cropEdge };
                switch (corner) {
                case 0: return Qt.point(r.x + r.w, r.y + r.h);
                case 1: return Qt.point(r.x, r.y + r.h);
                case 2: return Qt.point(r.x + r.w, r.y);
                default: return Qt.point(r.x, r.y);
                }
            }

            onPressed: function(m) {
                const p = area.toImagePt(m.x, m.y);
                pressImagePt = p;
                const corner = hitCorner(m.x, m.y);
                if (corner >= 0) {
                    mode = 2;
                    resizeAnchor = opposite(corner);
                } else {
                    mode = 1;
                    const wr = area.wcrop;
                    if (m.x < wr.x || m.x > wr.x + wr.width
                        || m.y < wr.y || m.y > wr.y + wr.height) {
                        root.cropX = p.x - root.cropEdge / 2;
                        root.cropY = p.y - root.cropEdge / 2;
                        root.clampCrop();
                    }
                    moveStart = Qt.point(root.cropX, root.cropY);
                }
            }
            onPositionChanged: function(m) {
                if (mode === 0 || root.srcWidth <= 0)
                    return;
                const p = area.toImagePt(m.x, m.y);
                if (mode === 1) {
                    root.cropX = moveStart.x + (p.x - pressImagePt.x);
                    root.cropY = moveStart.y + (p.y - pressImagePt.y);
                } else {
                    const dx = p.x - resizeAnchor.x, dy = p.y - resizeAnchor.y;
                    const maxEdge = Math.min(root.srcWidth, root.srcHeight);
                    const minEdge = Math.min(16, maxEdge);
                    const edge = Math.max(minEdge,
                        Math.min(Math.max(Math.abs(dx), Math.abs(dy)), maxEdge));
                    root.cropX = dx >= 0 ? resizeAnchor.x : resizeAnchor.x - edge;
                    root.cropY = dy >= 0 ? resizeAnchor.y : resizeAnchor.y - edge;
                    root.cropEdge = edge;
                }
                root.clampCrop();
            }
            onReleased: mode = 0
            onWheel: function(w) {
                const notches = w.angleDelta.y / 120;
                if (notches === 0 || root.cropEdge <= 0 || root.srcWidth <= 0) {
                    w.accepted = false;
                    return;
                }
                const pivot = area.toImagePt(w.x, w.y);
                const fx = (pivot.x - root.cropX) / root.cropEdge;
                const fy = (pivot.y - root.cropY) / root.cropEdge;
                const maxEdge = Math.min(root.srcWidth, root.srcHeight);
                const step = Math.max(4, root.cropEdge / 10);
                const minEdge = Math.min(16, maxEdge);
                const edge = Math.max(minEdge,
                    Math.min(root.cropEdge + notches * step, maxEdge));
                root.cropX = pivot.x - fx * edge;
                root.cropY = pivot.y - fy * edge;
                root.cropEdge = edge;
                root.clampCrop();
            }
        }

        Text {
            anchors.centerIn: parent
            visible: root.source === ""
            text: "No picture chosen"
            color: CoreBridge.muted
            font.pointSize: CoreBridge.basePt
        }
    }

    Text {
        id: hint
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: buttonRow.top
        anchors.bottomMargin: 6
        text: "Drag the square to move it, drag a corner to resize it, or "
              + "scroll to zoom. It is saved as a 256x256 picture."
        color: CoreBridge.foreground
        font.pointSize: CoreBridge.basePt
        wrapMode: Text.WordWrap
    }

    Row {
        id: buttonRow
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        spacing: 6
        PushButton {
            width: 80; height: 30
            text: "OK"
            onClicked: root.accepted(Math.round(root.cropX),
                                     Math.round(root.cropY),
                                     Math.round(root.cropEdge))
        }
        PushButton {
            width: 80; height: 30
            text: "Cancel"
            onClicked: root.cancelled()
        }
    }
}
