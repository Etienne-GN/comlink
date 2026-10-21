import QtQuick

/* The icon set, drawn rather than shipped.
 *
 * A port of frontend/qt/stripicons.cpp and mainwindow.cpp's pageIcon,
 * on the same 32x32 grid, the same coordinates and the same stroke
 * weight. The comments there explain why each mark is the shape it
 * is; they are not repeated, because the reason to read one is to
 * check it still matches the other.
 *
 * Canvas rather than Shape: these are ports of QPainter calls and a
 * Canvas takes them line for line, which is what makes the two
 * readable side by side. They are painted rather than exported as
 * images because they take the theme's ink, and sixteen presets times
 * fourteen marks times every size is not a directory anybody wants.
 */
Canvas {
    id: root

    /* Named for what the mark means, not what it looks like. */
    property string kind: "keypad"
    property color ink: "#E6E9EF"
    /* How much of the box the 32x32 grid fills. */
    property real fraction: 0.62
    /* 0..1, the recording light's halo, driven by a timer. */
    property real glow: 0.0

    onGlowChanged: requestPaint()
    onInkChanged: requestPaint()
    onKindChanged: requestPaint()

    onPaint: {
        const ctx = getContext("2d");
        ctx.reset();
        const side = Math.min(width, height) * fraction;
        ctx.save();
        ctx.translate((width - side) / 2, (height - side) / 2);
        ctx.scale(side / 32, side / 32);

        /* One weight and one cap across the set. Consistency here is
           what makes unrelated shapes look drawn by the same hand. */
        /* Rings of falling alpha rather than a blur, drawn first so the
           shape sits on its own halo. */
        if (glow > 0.0) {
            const g = Math.min(glow, 1.0);
            for (let i = 6; i >= 1; i--) {
                ctx.fillStyle = Qt.rgba(ink.r, ink.g, ink.b, 0.26 * g * (i / 6.0));
                circle(ctx, 16, 16, 6.0 + i * 1.5, true);
            }
        }
        ctx.lineWidth = 2.0;
        ctx.lineCap = "round";
        ctx.lineJoin = "round";
        ctx.strokeStyle = ink;
        ctx.fillStyle = ink;

        switch (kind) {
        case "keypad":   keypad(ctx);   break;
        case "contacts": contacts(ctx); break;
        case "quality":  quality(ctx);  break;
        case "activity": lines(ctx, [18, 13, 17, 9], 9, 5); break;
        case "menu":     lines(ctx, [18, 18, 12], 11, 5);   break;
        case "settings": settings(ctx); break;
        case "page":     page(ctx);     break;
        case "answer":   handset(ctx, false); break;
        case "end":      handset(ctx, true);  break;
        case "hold":     hold(ctx);     break;
        case "transfer": transfer(ctx); break;
        case "record":   dot(ctx);      break;
        case "chevronUp":   chevron(ctx, true);  break;
        case "chevronDown": chevron(ctx, false); break;
        case "expand":   expand(ctx, false); break;
        case "collapse": expand(ctx, true);  break;
        case "resume":   triangle(ctx); break;
        case "play":     triangle(ctx); break;
        case "pause":    pause(ctx);    break;
        case "video":    video(ctx);    break;
        case "backspace": backspace(ctx); break;
        case "saveCopy": saveCopy(ctx); break;
        case "stopRecording": roundRect(ctx, 10.5, 10.5, 11, 11, 1.5, true); break;
        case "mic":      mic(ctx);      break;
        case "winMin":     winMin(ctx);     break;
        case "winMax":     winMax(ctx);     break;
        case "winRestore": winRestore(ctx); break;
        case "winClose":   winClose(ctx);   break;
        case "speaker":  speaker(ctx);  break;
        }
        ctx.restore();
    }

    function circle(ctx, x, y, r, fill) {
        ctx.beginPath();
        ctx.ellipse(x - r, y - r, r * 2, r * 2);
        if (fill) ctx.fill(); else ctx.stroke();
    }
    function line(ctx, x1, y1, x2, y2) {
        ctx.beginPath();
        ctx.moveTo(x1, y1);
        ctx.lineTo(x2, y2);
        ctx.stroke();
    }
    function roundRect(ctx, x, y, w, h, r, fill) {
        ctx.beginPath();
        ctx.roundedRect(x, y, w, h, r, r);
        if (fill) ctx.fill(); else ctx.stroke();
    }

    /* A handset over the keypad it belongs to would be two ideas. The
       grid alone says "the thing you press". */
    function keypad(ctx) {
        for (let row = 0; row < 3; row++)
            for (let col = 0; col < 3; col++)
                circle(ctx, 9 + col * 7, 8 + row * 7, 2.2, true);
        circle(ctx, 16, 29, 2.2, true);
    }

    /* The window's own buttons. A port of frontend/qt/titlebar.cpp's
       glyph(), on its 20x20 grid rather than this file's 32x32 one,
       because these are drawn at 14 pixels and the weight has to
       match what that bar drew. */
    function winScale(ctx) { ctx.scale(32 / 20, 32 / 20); }

    function winMin(ctx) {
        ctx.save(); winScale(ctx);
        ctx.lineWidth = 1.6;
        ctx.lineCap = "butt";
        line(ctx, 6, 13, 14, 13);
        ctx.restore();
    }

    function winMax(ctx) {
        ctx.save(); winScale(ctx);
        ctx.lineWidth = 1.6;
        ctx.lineCap = "butt";
        ctx.beginPath();
        ctx.rect(6.5, 6.5, 7, 7);
        ctx.stroke();
        ctx.restore();
    }

    /* Two offset outlines: it undoes a maximise rather than doing
       one, and the glyph should say so. */
    function winRestore(ctx) {
        ctx.save(); winScale(ctx);
        ctx.lineWidth = 1.6;
        ctx.lineCap = "butt";
        ctx.beginPath();
        ctx.rect(5.5, 8.5, 6, 6);
        ctx.stroke();
        line(ctx, 8.5, 5.5, 14.5, 5.5);
        line(ctx, 14.5, 5.5, 14.5, 11.5);
        ctx.restore();
    }

    function winClose(ctx) {
        ctx.save(); winScale(ctx);
        ctx.lineWidth = 1.6;
        ctx.lineCap = "butt";
        line(ctx, 6, 6, 14, 14);
        line(ctx, 14, 6, 6, 14);
        ctx.restore();
    }

    /* A capsule on a stand: the shape of a microphone, and what the
       far end hears. */
    function mic(ctx) {
        roundRect(ctx, 12.5, 4, 7, 14, 3.5, false);
        ctx.beginPath();
        ctx.arc(16, 16, 7, 0, Math.PI, false);
        ctx.stroke();
        line(ctx, 16, 23, 16, 27);
        line(ctx, 11, 27, 21, 27);
    }

    /* A cone with two waves leaving it: what you hear, and the same
       waves the call display's meter draws. */
    function speaker(ctx) {
        ctx.beginPath();
        ctx.moveTo(6, 13);
        ctx.lineTo(6, 19);
        ctx.lineTo(10, 19);
        ctx.lineTo(15, 25);
        ctx.lineTo(15, 7);
        ctx.lineTo(10, 13);
        ctx.closePath();
        ctx.stroke();
        ctx.beginPath();
        ctx.arc(17, 16, 5, -Math.PI / 2.6, Math.PI / 2.6, false);
        ctx.stroke();
        ctx.beginPath();
        ctx.arc(17, 16, 9.5, -Math.PI / 2.6, Math.PI / 2.6, false);
        ctx.stroke();
    }

    /* A person against the edge of a book: the head and shoulders say
       "who", and the spine says the who is written down somewhere. */
    function contacts(ctx) {
        line(ctx, 9, 6, 9, 26);
        roundRect(ctx, 9, 6, 16, 20, 2.5, false);
        circle(ctx, 17.5, 14, 3.0, false);
        ctx.beginPath();
        ctx.moveTo(13, 22.5);
        ctx.bezierCurveTo(13.5, 18.5, 21.5, 18.5, 22, 22.5);
        ctx.stroke();
    }

    /* Bars rising left to right: the shape of a measurement, and it
       cannot be mistaken for the meter in the call panel above. */
    function quality(ctx) {
        const h = [6, 11, 16, 22];
        for (let i = 0; i < 4; i++)
            line(ctx, 7 + i * 6, 26, 7 + i * 6, 26 - h[i]);
    }

    /* Lines of text, ragged at the right the way a log is; and the
       same mark at three lines is the window menu. */
    function lines(ctx, widths, y0, step) {
        for (let i = 0; i < widths.length; i++)
            line(ctx, 7, y0 + i * step, 7 + widths[i], y0 + i * step);
    }

    /* A gear, because everyone already knows what it means. Six teeth
       reads as one at sixteen pixels; eight turns to mush. */
    function settings(ctx) {
        const cx = 16, cy = 16, ring = 7.5, tooth = 3.2;
        for (let i = 0; i < 6; i++) {
            const a = i * (Math.PI / 3.0) + Math.PI / 12.0;
            const dx = Math.cos(a), dy = Math.sin(a);
            line(ctx, cx + dx * ring, cy + dy * ring,
                 cx + dx * (ring + tooth), cy + dy * (ring + tooth));
        }
        circle(ctx, cx, cy, ring, false);
        circle(ctx, cx, cy, 2.6, false);
    }

    /* The page with its top-right corner turned down, and three lines
       of writing. */
    function page(ctx) {
        const fold = 8.0;
        ctx.beginPath();
        ctx.moveTo(7, 4);
        ctx.lineTo(25 - fold, 4);
        ctx.lineTo(25, 4 + fold);
        ctx.lineTo(25, 28);
        ctx.lineTo(7, 28);
        ctx.closePath();
        ctx.stroke();
        ctx.beginPath();
        ctx.moveTo(25 - fold, 4);
        ctx.lineTo(25 - fold, 4 + fold);
        ctx.lineTo(25, 4 + fold);
        ctx.stroke();
        ctx.lineWidth = 1.6;
        for (let i = 0; i < 3; i++)
            line(ctx, 11, 15 + i * 5, 21, 15 + i * 5);
        ctx.lineWidth = 2.0;
    }

    /* Filled and rotated: tilted up to answer, over on its face to
       hang up. Stroked, at eighteen pixels it is a squiggle and the
       two rotations cannot be told apart. */
    function handset(ctx, down) {
        ctx.save();
        ctx.translate(16, 16);
        ctx.rotate((down ? 135 : -35) * Math.PI / 180);
        ctx.beginPath();
        ctx.ellipse(-11, -4, 8, 8);
        ctx.ellipse(3, -4, 8, 8);
        ctx.roundedRect(-7.5, -3.4, 15.0, 4.0, 2.0, 2.0);
        ctx.fill();
        /* Hollowed underneath, so it reads as a handset rather than a
           bone: the cut is what gives it the earpiece shape. */
        ctx.globalCompositeOperation = "destination-out";
        ctx.beginPath();
        ctx.roundedRect(-4.2, -1.0, 8.4, 6.0, 2.0, 2.0);
        ctx.fill();
        ctx.globalCompositeOperation = "source-over";
        ctx.restore();
    }

    function hold(ctx) {
        line(ctx, 12, 8, 12, 24);
        line(ctx, 20, 8, 20, 24);
    }

    /* Two arrows passing, which is what a transfer is. */
    function transfer(ctx) {
        line(ctx, 7, 12, 23, 12);
        line(ctx, 19, 8, 23, 12);
        line(ctx, 19, 16, 23, 12);
        line(ctx, 25, 21, 9, 21);
        line(ctx, 13, 17, 9, 21);
        line(ctx, 13, 25, 9, 21);
    }

    function dot(ctx) { circle(ctx, 16, 16, 6.0, true); }

    /* Wide and shallow. A tall chevron reads as a play button
       pointing the wrong way; a shallow one reads as a handle. */
    function chevron(ctx, up) {
        const y = up ? 19.0 : 13.0, dy = up ? -5.0 : 5.0;
        line(ctx, 8, y, 16, y + dy);
        line(ctx, 16, y + dy, 24, y);
    }

    /* Corners, not arrows: the same mark every video player uses, and
       it survives being sixteen pixels wide. */
    function expand(ctx, collapse) {
        const a = collapse ? 12.0 : 7.0, len = 5.0;
        const k = [[a, a, 1, 1], [32 - a, a, -1, 1],
                   [a, 32 - a, 1, -1], [32 - a, 32 - a, -1, -1]];
        for (const q of k) {
            line(ctx, q[0], q[1], q[0] + q[2] * len, q[1]);
            line(ctx, q[0], q[1], q[0], q[1] + q[3] * len);
        }
    }

    function triangle(ctx) {
        ctx.beginPath();
        ctx.moveTo(12, 8);
        ctx.lineTo(24, 16);
        ctx.lineTo(12, 24);
        ctx.closePath();
        ctx.fill();
    }

    function pause(ctx) {
        roundRect(ctx, 10.5, 8.0, 4.0, 16.0, 1.0, true);
        roundRect(ctx, 17.5, 8.0, 4.0, 16.0, 1.0, true);
    }

    /* A downward arrow into a line: where a recording leaves the
       application. */
    function saveCopy(ctx) {
        line(ctx, 16, 6, 16, 20);
        line(ctx, 10, 14, 16, 20);
        line(ctx, 22, 14, 16, 20);
        line(ctx, 8, 25, 24, 25);
    }

    /* A camera body and its lens hood, in the stroke the set uses. */
    function video(ctx) {
        roundRect(ctx, 5, 10, 15, 12, 2.5, false);
        ctx.beginPath();
        ctx.moveTo(20, 14.5);
        ctx.lineTo(27, 10.5);
        ctx.lineTo(27, 21.5);
        ctx.lineTo(20, 17.5);
        ctx.closePath();
        ctx.stroke();
    }

    /* The key every phone uses to take a digit back: a tag pointing
       left, with the cross that says remove. */
    function backspace(ctx) {
        ctx.beginPath();
        ctx.moveTo(4, 16);
        ctx.lineTo(11, 8);
        ctx.lineTo(27, 8);
        ctx.lineTo(27, 24);
        ctx.lineTo(11, 24);
        ctx.closePath();
        ctx.stroke();
        line(ctx, 15, 12.5, 22, 19.5);
        line(ctx, 22, 12.5, 15, 19.5);
    }
}
