import QtQuick
import comlink

/* The level meter, a port of frontend/qt/levelmeter.cpp.
 *
 * Same sample count, same floor and top, same envelope. Both channels
 * are drawn about one centre line, each symmetric, so they overlap
 * rather than taking a half each: that costs the instant read of
 * which half is which and buys each channel the whole height and a
 * shape that looks like a waveform. Colour carries what position
 * used to.
 *
 * Receive is drawn first so the near end sits on top -- a technician
 * watching this is usually watching what they are sending. */
Canvas {
    id: root

    readonly property int samples: 125
    readonly property int floorLevel: 16
    readonly property int topLevel: 60

    property var tx: new Array(125).fill(0)
    property var rx: new Array(125).fill(0)
    property int head: 0
    property bool any: false

    implicitHeight: 52

    function push(t, r) {
        tx[head] = t;
        rx[head] = r;
        head = (head + 1) % samples;
        any = true;
        requestPaint();
    }

    function clear() {
        tx = new Array(samples).fill(0);
        rx = new Array(samples).fill(0);
        any = false;
        requestPaint();
    }

    function deflection(level) {
        if (level <= floorLevel) return 0;
        if (level >= topLevel) return 1;
        return (level - floorLevel) / (topLevel - floorLevel);
    }

    onPaint: {
        const ctx = getContext("2d");
        ctx.reset();
        const mid = Math.round(height / 2) + 0.5;

        /* The centre line is always drawn, even with no call. An empty
           meter should look like an instrument at rest rather than
           like a widget that failed to load. */
        ctx.lineWidth = 1;
        ctx.strokeStyle = CoreBridge.muted;
        ctx.beginPath();
        ctx.moveTo(0, mid);
        ctx.lineTo(width, mid);
        ctx.stroke();
        if (!any) return;

        const step = width / (samples - 1);
        const room = mid - 2;

        for (let pass = 0; pass < 2; pass++) {
            const receive = pass === 0;
            const src = receive ? rx : tx;
            const c = receive ? CoreBridge.rxTrace : CoreBridge.txTrace;

            ctx.beginPath();
            for (let i = 0; i < samples; i++) {
                const len = deflection(src[(head + i) % samples]) * room;
                if (i === 0) ctx.moveTo(0, mid - len);
                else ctx.lineTo(i * step, mid - len);
            }
            /* Back along the bottom, so the shape closes as one band
               rather than two strokes that happen to be mirrored. */
            for (let i = samples - 1; i >= 0; i--) {
                const len = deflection(src[(head + i) % samples]) * room;
                ctx.lineTo(i * step, mid + len);
            }
            ctx.closePath();

            /* Alpha on both, because they lie on top of each other and
               the overlap has to read as overlap rather than as
               whichever was painted second. */
            const col = Qt.color(c);
            ctx.fillStyle = Qt.rgba(col.r, col.g, col.b,
                                    (receive ? 95 : 110) / 255);
            ctx.fill();
            ctx.strokeStyle = c;
            ctx.lineWidth = 1;
            ctx.stroke();
        }
    }
}
