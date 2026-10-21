import QtQuick
import comlink

/* The figures for the call in the panel: a table laid out with
   spaces, in the fixed-width face and smaller, so the columns line up
   and the whole readout fits at once. */
Flickable {
    id: root
    property int peakTx: 0
    property int peakRx: 0
    property bool moduleOn: true
    /* LevelMeter::fullScaleLevel: the codeword the meter tops out at. */
    readonly property int fullScale: 60

    contentHeight: body.implicitHeight
    clip: true

    property var q: ({ valid: false })
    function reload() { q = CoreBridge.quality() }
    Timer {
        interval: 1000; running: root.visible; repeat: true
        onTriggered: root.reload()
    }
    onVisibleChanged: if (visible) reload()
    Component.onCompleted: reload()

    function pad(s, n) { s = String(s); while (s.length < n) s += " "; return s }

    TextEdit {
        id: body
        width: root.width
        readOnly: true
        selectByMouse: true
        wrapMode: TextEdit.Wrap
        color: CoreBridge.foreground
        font.family: "monospace"
        font.pointSize: CoreBridge.basePt * 0.75
        text: {
            if (!CoreBridge.inCall)
                return "No call.\n\nFigures appear here while one is up.";
            if (!root.moduleOn)
                return "The audio quality module is switched off.";
            if (!root.q.valid)
                return "Waiting for the first report.\n\nFigures come from RTCP, "
                       + "so they need the far end to send it.";
            return "Received\n"
                   + "  lost      " + root.q.rxLost + "\n"
                   + "  jitter    " + Math.floor(root.q.rxJitterAvgMs) + " ms\n"
                   + "Sent\n"
                   + "  lost      " + root.q.txLost + "\n"
                   + "  jitter    " + Math.floor(root.q.txJitterAvgMs) + " ms\n"
                   + "\nRound trip  " + Math.floor(root.q.rttAvgMs) + " ms\n"
                   + "MOS         " + root.q.mos.toFixed(2) + "\n"
                   + "Peak out    " + root.peakTx + "  (full at " + root.fullScale + ")\n"
                   + "Peak in     " + root.peakRx + "\n";
        }
    }
    TipArea {
        anchors.fill: parent
        acceptedButtons: Qt.NoButton
        tip: "Peak levels are what the media stack reports: a mu-law "
             + "codeword, 0 to 127. A send level well under the receive "
             + "level is a microphone that is too quiet."
    }
}
