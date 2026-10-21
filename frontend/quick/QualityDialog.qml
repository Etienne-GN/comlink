import QtQuick
import comlink
import "fusion.js" as Fusion

/* The full metric table for one call -- qualitydialog.cpp, ported
 * part for part: a call picker shown only when more than one call is
 * up, a headline, a caveat, the fourteen-row table, and Close.
 *
 * Refreshed on a timer while the dialog is open rather than from the
 * core's SP_EV_CALL_QUALITY event: QualityPane.qml already polls its
 * own copy of this table the same way, and a second wiring of that
 * event into this dialog would be a second thing to keep in step with
 * the first. Nothing here computes a figure; every one is the core's,
 * by way of CoreBridge.qualityDetail(). */
Item {
    id: root
    property var host
    anchors.fill: parent

    /* Display thresholds, not a standard: qualitydialog.cpp's own
       convention for colouring one number so a tech can glance at
       it. */
    readonly property real mosGood: 4.0
    readonly property real mosFair: 3.1

    property var liveCalls: []
    property var callId: 0
    property var q: ({ exists: false, valid: false })

    function msRange(m) {
        return "cur " + m.cur.toFixed(1) + "  min " + m.min.toFixed(1)
               + "  max " + m.max.toFixed(1) + "  avg " + m.avg.toFixed(1);
    }

    /* Keeps the current call if it is still up; else prefers a call
       that has media; else the first call, or none --
       qualitydialog.cpp's followActiveCall(). */
    function followActiveCall() {
        liveCalls = CoreBridge.calls();
        for (const c of liveCalls)
            if (c.id === root.callId)
                return;
        for (const c of liveCalls) {
            const d = CoreBridge.qualityDetail(c.id);
            if (d.valid) { root.callId = c.id; return; }
        }
        root.callId = liveCalls.length > 0 ? liveCalls[0].id : 0;
    }

    function refresh() {
        root.followActiveCall();
        root.q = root.callId !== 0 ? CoreBridge.qualityDetail(root.callId)
                                    : { exists: false, valid: false };
    }
    Component.onCompleted: root.refresh()
    Timer {
        interval: 1000; running: root.visible; repeat: true
        onTriggered: root.refresh()
    }

    function pickLabel(c) {
        return (c.inbound ? "from " : "to ") + c.label + "  (#" + c.id + ")";
    }

    readonly property var rows: {
        const out = [];
        if (root.callId === 0 || !root.q.exists || !root.q.valid)
            return out;
        const q = root.q;
        out.push(["Packets rx / tx", q.rxPackets + " / " + q.txPackets]);
        out.push(["Bytes rx / tx", q.rxBytes + " / " + q.txBytes]);
        out.push(["Loss rx",
                  q.rxLost + " (" + q.rxLossPct.toFixed(2) + "%)"]);
        /* Named as the peer's report, because that is what it is:
           zero here can mean "no loss" or "the peer never sent an
           RTCP receiver report", and those are different facts. */
        out.push(["Loss tx (peer's report)",
                  q.txLost + " (" + q.txLossPct.toFixed(2) + "%)"]);
        out.push(["Discarded rx", String(q.rxDiscarded)]);
        out.push(["Reordered rx", String(q.rxReordered)]);
        out.push(["Duplicated rx", String(q.rxDuplicated)]);
        out.push(["Jitter rx, ms", root.msRange(q.rxJitter)]);
        out.push(["Jitter tx, ms", root.msRange(q.txJitter)]);
        out.push(["Round trip, ms", root.msRange(q.rtt)]);
        out.push(["Jitter buffer",
                  q.jbSizeFrames + " frames, prefetch " + q.jbPrefetchFrames]);
        out.push(["Buffer delay, ms",
                  "avg " + q.jbAvgDelayMs + "  min " + q.jbMinDelayMs
                  + "  max " + q.jbMaxDelayMs]);
        out.push(["Buffer events",
                  "lost " + q.jbLost + ", discarded " + q.jbDiscarded
                  + ", empty " + q.jbEmpty]);
        out.push(["Sampled", q.final ? "at teardown" : "just now"]);
        return out;
    }

    readonly property string headline: {
        if (root.callId === 0)
            return "No call is up.";
        if (!root.q.exists)
            return "Call #" + root.callId + " is gone.";
        if (!root.q.valid)
            return "Call #" + root.callId + ": waiting for media.";
        return "MOS " + root.q.mos.toFixed(2) + "   R " + root.q.rFactor
               + (root.q.final ? "   (final)" : "");
    }
    readonly property bool headlineLive: root.callId !== 0 && root.q.exists
                                          && root.q.valid
    readonly property color headlineColor: {
        if (!root.headlineLive)
            return CoreBridge.foreground;
        if (root.q.mos >= root.mosGood)
            return CoreBridge.accent;
        if (root.q.mos < root.mosFair)
            return "#b3261e";
        return CoreBridge.foreground;
    }
    readonly property bool headlineBold: root.headlineLive
        && (root.q.mos >= root.mosGood || root.q.mos < root.mosFair)
    readonly property string caveat: {
        if (root.callId === 0)
            return "Place or answer a call and the figures appear here, "
                   + "updating every couple of seconds. Round-trip time "
                   + "needs a few seconds more, because it comes from the "
                   + "far end's RTCP reports.";
        if (root.q.exists && !root.q.valid)
            return "Figures appear once audio is flowing.";
        return "MOS is an estimate. The counters are the measurements.";
    }

    ComboBox {
        id: pick
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        visible: root.liveCalls.length > 1
        model: root.liveCalls.map(function(c) {
            return { label: root.pickLabel(c), value: c.id } })
        currentIndex: {
            for (let i = 0; i < root.liveCalls.length; i++)
                if (root.liveCalls[i].id === root.callId)
                    return i;
            return -1;
        }
        onActivated: function(i) {
            root.callId = pick.valueAt(i);
            root.refresh();
        }
    }

    Text {
        id: headlineText
        anchors.top: pick.visible ? pick.bottom : parent.top
        anchors.topMargin: pick.visible ? 4 : 0
        anchors.left: parent.left
        anchors.right: parent.right
        text: root.headline
        color: root.headlineColor
        font.bold: root.headlineBold
        font.pointSize: CoreBridge.basePt
        wrapMode: Text.WordWrap
    }
    Text {
        id: caveatText
        anchors.top: headlineText.bottom
        anchors.topMargin: 4
        anchors.left: parent.left
        anchors.right: parent.right
        text: root.caveat
        color: CoreBridge.foreground
        font.pointSize: CoreBridge.basePt
        wrapMode: Text.WordWrap
    }

    TextMetrics {
        id: metricHeaderMetrics
        font.pointSize: CoreBridge.basePt
        text: "Metric"
    }
    readonly property int metricColW: Math.ceil(metricHeaderMetrics.width) + 10

    Rectangle {
        id: tableFrame
        anchors.top: caveatText.bottom
        anchors.topMargin: 4
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: closeRow.top
        color: CoreBridge.surface
        border.width: 1
        border.color: CoreBridge.muted

        Rectangle {
            id: header
            anchors.top: parent.top
            anchors.left: parent.left
            anchors.right: parent.right
            height: 22
            gradient: Gradient {
                GradientStop { position: 0
                               color: Fusion.gradientTop(CoreBridge.surface, false) }
                GradientStop { position: 1
                               color: Fusion.gradientBottom(CoreBridge.surface, false) }
            }
            border.width: 1
            border.color: Fusion.outline(CoreBridge.background)
            Text {
                width: root.metricColW
                height: parent.height
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
                text: "Metric"
                color: CoreBridge.foreground
                font.pointSize: CoreBridge.basePt
            }
            Rectangle {
                x: root.metricColW
                width: 1
                height: parent.height
                color: Fusion.outline(CoreBridge.background)
            }
            Text {
                x: root.metricColW
                width: parent.width - root.metricColW
                height: parent.height
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
                text: "Value"
                color: CoreBridge.foreground
                font.pointSize: CoreBridge.basePt
            }
        }

        ListView {
            anchors.top: header.bottom
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            anchors.margins: 1
            clip: true
            model: root.rows

            delegate: Item {
                width: parent.width
                height: 20
                Text {
                    x: 6
                    width: root.metricColW - 10
                    height: parent.height
                    verticalAlignment: Text.AlignVCenter
                    text: modelData[0]
                    color: CoreBridge.foreground
                    font.pointSize: CoreBridge.basePt * 0.9
                    elide: Text.ElideRight
                }
                Text {
                    x: root.metricColW + 4
                    width: parent.width - root.metricColW - 8
                    height: parent.height
                    verticalAlignment: Text.AlignVCenter
                    text: modelData[1]
                    color: CoreBridge.foreground
                    font.pointSize: CoreBridge.basePt * 0.9
                    elide: Text.ElideRight
                }
            }
        }
    }

    /* Ignored horizontally, so it fills the width -- close's own
       QSizePolicy in qualitydialog.cpp, not the right-anchored button
       every other dialog here uses. */
    Item {
        id: closeRow
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        height: 30

        PushButton {
            anchors.fill: parent
            text: "Close"
            onClicked: if (root.host) root.host.close()
        }
    }
}
