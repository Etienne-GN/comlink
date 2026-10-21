import QtQuick
import comlink

/* A still field of stars behind the call panel, for the Republic
   preset. The same stars every time: the positions come from a fixed
   seed, so the panel does not reshuffle its sky whenever it is
   redrawn. Faint enough that nothing in front has to compete with it. */
Canvas {
    id: root
    property color ink: CoreBridge.foreground

    onInkChanged: requestPaint()
    onWidthChanged: requestPaint()
    onHeightChanged: requestPaint()

    onPaint: {
        const ctx = getContext("2d");
        ctx.reset();
        let seed = 1977;
        function next() {
            seed = (seed * 1103515245 + 12345) % 2147483648;
            return seed / 2147483648;
        }
        const count = Math.round(width * height / 520);
        for (let i = 0; i < count; i++) {
            const x = next() * width;
            const y = next() * height;
            const r = 0.35 + next() * next() * 1.1;
            ctx.fillStyle = Qt.rgba(ink.r, ink.g, ink.b, 0.10 + next() * 0.32);
            ctx.beginPath();
            ctx.ellipse(x - r, y - r, 2 * r, 2 * r);
            ctx.fill();
        }
    }
}
