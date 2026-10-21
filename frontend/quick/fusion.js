.pragma library

/* The arithmetic Qt's Fusion style paints the Qt window's controls
   with, from the application palette the core's theme sets: Window is
   the theme background, Button and Base the surface, Highlight the
   accent. Ported so a QML control and the widget it stands in for come
   out the same colour on every preset. */

function gray(c) {
    return Math.floor((Math.round(c.r * 255) * 11 + Math.round(c.g * 255) * 16
                       + Math.round(c.b * 255) * 5) / 32);
}

/* QFusionStylePrivate::buttonColor */
function buttonColor(button) {
    const val = gray(button);
    const l = Qt.lighter(button, (100 + Math.max(1, Math.floor((180 - val) / 6)))
                                 / 100);
    const h = l.hsvHue < 0 ? 0 : l.hsvHue;
    return Qt.hsva(h, l.hsvSaturation * 0.75, l.hsvValue, 1);
}

/* QFusionStylePrivate::outline */
function outline(window) {
    return Qt.darker(window, 1.4);
}

/* QFusionStylePrivate::highlightedOutline */
function highlightedOutline(highlight) {
    const c = Qt.darker(highlight, 1.25);
    if (c.hslLightness * 255 > 160)
        return Qt.hsla(c.hslHue < 0 ? 0 : c.hslHue, c.hslSaturation,
                       160 / 255, 1);
    return c;
}

/* The panel's fill: qt_fusion_gradient's two stops, over the button
   colour, which hovering brings up by the 4% it otherwise loses. */
function gradientTop(button, hover) {
    const b = buttonColor(button);
    return Qt.lighter(hover ? b : Qt.darker(b, 1.04), 1.24);
}
function gradientBottom(button, hover) {
    const b = buttonColor(button);
    return Qt.lighter(hover ? b : Qt.darker(b, 1.04), 1.02);
}
function pressed(button) {
    return Qt.darker(buttonColor(button), 1.10);
}

const innerContrast = Qt.rgba(1, 1, 1, 30 / 255);
