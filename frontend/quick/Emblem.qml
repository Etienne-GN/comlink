import QtQuick
import comlink

/* The mark a preset draws faintly behind a call in progress, named by
 * the core (sp_theme.emblem). Each is an homage drawn for this
 * application, not a copy of anybody's artwork. Blades rather than
 * lines throughout, so at this faintness each still reads as a shape
 * and not as scratches.
 *
 *   republic  an eight-spoked wheel between two rings
 *   empire    a six-spoked cog, the spokes widening to the rim
 *   jedi      an upright blade with two swept wings
 *   sith      a six-pointed star of thorns round a hollow hexagon */
Canvas {
    id: root
    property string kind: CoreBridge.emblem
    property color ink: CoreBridge.accent
    property real strength: 0.13

    onKindChanged: requestPaint()
    onInkChanged: requestPaint()
    onWidthChanged: requestPaint()
    onHeightChanged: requestPaint()

    function ring(ctx, cx, cy, r, w) {
        ctx.lineWidth = w;
        ctx.beginPath();
        ctx.arc(cx, cy, r, 0, 2 * Math.PI, false);
        ctx.stroke();
    }
    function disc(ctx, cx, cy, r) {
        ctx.beginPath();
        ctx.arc(cx, cy, r, 0, 2 * Math.PI, false);
        ctx.fill();
    }
    function polygon(ctx, pts) {
        ctx.beginPath();
        ctx.moveTo(pts[0][0], pts[0][1]);
        for (let i = 1; i < pts.length; i++)
            ctx.lineTo(pts[i][0], pts[i][1]);
        ctx.closePath();
        ctx.fill();
    }
    function at(cx, cy, a, r) { return [cx + Math.cos(a) * r, cy + Math.sin(a) * r] }

    function republic(ctx, cx, cy, R) {
        ring(ctx, cx, cy, R * 0.92, R * 0.07);
        ring(ctx, cx, cy, R * 0.30, R * 0.05);
        disc(ctx, cx, cy, R * 0.12);
        for (let i = 0; i < 8; i++) {
            const a = i * Math.PI / 4 - Math.PI / 2;
            const major = i % 2 === 0;
            const half = major ? 0.13 : 0.10;
            polygon(ctx, [at(cx, cy, a - half, R * 0.36),
                          at(cx, cy, a, R * (major ? 0.84 : 0.70)),
                          at(cx, cy, a + half, R * 0.36)]);
        }
    }

    function empire(ctx, cx, cy, R) {
        ring(ctx, cx, cy, R * 0.86, R * 0.12);
        ring(ctx, cx, cy, R * 0.28, R * 0.08);
        for (let i = 0; i < 6; i++) {
            const a = i * Math.PI / 3 - Math.PI / 2;
            polygon(ctx, [at(cx, cy, a - 0.10, R * 0.32), at(cx, cy, a - 0.26, R * 0.80),
                          at(cx, cy, a + 0.26, R * 0.80), at(cx, cy, a + 0.10, R * 0.32)]);
            /* The teeth outside the rim that make it a cog. */
            const t = a + Math.PI / 6;
            polygon(ctx, [at(cx, cy, t - 0.12, R * 0.90), at(cx, cy, t - 0.08, R * 1.0),
                          at(cx, cy, t + 0.08, R * 1.0), at(cx, cy, t + 0.12, R * 0.90)]);
        }
    }

    function jedi(ctx, cx, cy, R) {
        /* The blade, point up, and the hilt below the wings. */
        polygon(ctx, [[cx, cy - R * 0.98], [cx + R * 0.08, cy - R * 0.55],
                      [cx + R * 0.06, cy + R * 0.55], [cx - R * 0.06, cy + R * 0.55],
                      [cx - R * 0.08, cy - R * 0.55]]);
        polygon(ctx, [[cx - R * 0.10, cy + R * 0.58], [cx + R * 0.10, cy + R * 0.58],
                      [cx + R * 0.07, cy + R * 0.92], [cx - R * 0.07, cy + R * 0.92]]);
        /* Each wing sweeps up and out from the crossguard in three
           feathers, longest at the top. */
        for (const side of [-1, 1]) {
            for (let k = 0; k < 3; k++) {
                const y0 = cy + R * (0.10 + k * 0.14);
                const reach = R * (0.92 - k * 0.18);
                const lift = R * (0.62 - k * 0.12);
                polygon(ctx, [[cx + side * R * 0.10, y0],
                              [cx + side * reach, y0 - lift],
                              [cx + side * (reach - R * 0.10), y0 - lift + R * 0.12],
                              [cx + side * R * 0.10, y0 + R * 0.10]]);
            }
        }
        ctx.strokeStyle = Qt.rgba(ink.r, ink.g, ink.b, strength);
        ctx.lineWidth = R * 0.05;
        ctx.beginPath();
        ctx.arc(cx, cy + R * 0.05, R * 0.95, Math.PI * 0.22, Math.PI * 0.78, false);
        ctx.stroke();
    }

    function sith(ctx, cx, cy, R) {
        for (let i = 0; i < 6; i++) {
            const a = i * Math.PI / 3 - Math.PI / 2;
            /* Thorns: a long point on each vertex, curving off to one
               side, so the star turns rather than sits. */
            polygon(ctx, [at(cx, cy, a - 0.30, R * 0.42), at(cx, cy, a + 0.10, R * 1.0),
                          at(cx, cy, a + 0.24, R * 0.42)]);
            polygon(ctx, [at(cx, cy, a + 0.55, R * 0.40), at(cx, cy, a + 0.52, R * 0.62),
                          at(cx, cy, a + 0.75, R * 0.40)]);
        }
        ctx.lineWidth = R * 0.07;
        ctx.beginPath();
        for (let i = 0; i <= 6; i++) {
            const p = at(cx, cy, i * Math.PI / 3 - Math.PI / 2, R * 0.30);
            if (i === 0) ctx.moveTo(p[0], p[1]); else ctx.lineTo(p[0], p[1]);
        }
        ctx.closePath();
        ctx.stroke();
    }

    onPaint: {
        const ctx = getContext("2d");
        ctx.reset();
        const cx = width / 2, cy = height / 2;
        const R = Math.min(width, height) / 2;
        ctx.fillStyle = Qt.rgba(ink.r, ink.g, ink.b, strength);
        ctx.strokeStyle = Qt.rgba(ink.r, ink.g, ink.b, strength);
        if (kind === "republic") republic(ctx, cx, cy, R);
        else if (kind === "empire") empire(ctx, cx, cy, R);
        else if (kind === "jedi") jedi(ctx, cx, cy, R);
        else if (kind === "sith") sith(ctx, cx, cy, R);
    }
}
