import QtQuick
import QtQuick.Dialogs
import comlink
import "fusion.js" as Fusion

/* The call log panel of the Qt window. There it grew the window to
   the right by its own width; here it covers the app instead, because
   a handset cannot grow its screen. Its own page mark sits where the
   status row's does, so the button that opened it closes it. */
Rectangle {
    id: root
    visible: false
    color: CoreBridge.background
    signal callBack(string uri)
    signal trace(string callId)

    property var rows: []
    property int current: -1
    property var pb: ({ state: 0 })
    property bool callUp: CoreBridge.inCall
    readonly property var row: current >= 0 && current < rows.length ? rows[current] : null
    readonly property string recPath: row ? row.recording : ""
    readonly property bool sameFile: recPath !== "" && pb.path === recPath

    /* Remembered, as the Qt window remembers it -- except when a
       probe opens it, which must not change what the next start
       shows. */
    function open(remember) {
        visible = true;
        if (remember !== false)
            CoreBridge.setShowCallLog(true);
        reload();
    }
    function toggle() {
        if (visible) {
            visible = false;
            CoreBridge.setShowCallLog(false);
        } else {
            open();
        }
    }

    function when(us) { return Qt.formatDateTime(new Date(us / 1000), "MM-dd HH:mm") }
    function label(r) {
        /* The transcript mark goes before the number, not after it: a
           narrow panel elides the end of the line. */
        let s = when(r.startUs) + " " + (r.inbound ? "↙" : "↗") + " "
                + (r.transcript === 1 ? "[txt…] " : r.transcript === 2 ? "[txt] " : "")
                + (r.remote !== "" ? r.remote : "-");
        if (r.duration > 0) s += "  " + r.duration + "s";
        if (r.qPackets > 0) s += "  MOS " + r.mos.toFixed(1);
        return s;
    }
    function showTranscript() { transcriptView.show(row.id, row.remote) }
    function reload() {
        const keep = row ? row.remote : "";
        rows = CoreBridge.history(200);
        let restore = -1;
        if (keep !== "")
            for (let i = 0; i < rows.length; i++)
                if (rows[i].remote === keep) { restore = i; break }
        current = restore;
        refreshPlayback();
    }
    function refreshPlayback() { pb = CoreBridge.playback() }
    function minSec(ms) {
        const s = Math.floor(ms / 1000);
        return Math.floor(s / 60) + ":" + ("0" + (s % 60)).slice(-2);
    }
    function speedLabel(s) {
        if (Math.abs(s - 2.0) < 0.01) return "2×";
        if (Math.abs(s - 0.5) < 0.01) return "0.5×";
        return "1×";
    }
    function nextSpeed(s) {
        if (Math.abs(s - 1.0) < 0.01) return 2.0;
        if (Math.abs(s - 2.0) < 0.01) return 0.5;
        return 1.0;
    }
    function doCallBack() {
        if (!row) { CoreBridge.note("select a call in the log first"); return }
        if (row.remote === "" || row.remote === "-") {
            CoreBridge.note("that call has no remote address recorded");
            return;
        }
        root.callBack(row.remote);
    }

    Timer {
        interval: 40; repeat: true; running: root.visible
        onTriggered: root.refreshPlayback()
    }

    MouseArea { anchors.fill: parent }

    readonly property bool transcriptOn: CoreBridge.modules().some(function(m) {
        return m.id === "transcript" && m.enabled })
    /* A transcript finishing while the panel is up moves its row from
       [txt…] to [txt] without a Refresh. */
    Connections {
        target: CoreBridge
        function onTranscriptChanged() { if (root.visible) root.reload() }
    }

    Column {
        x: 6; y: 6
        width: parent.width - 12
        height: parent.height - 12
        spacing: 4

        Item {
            id: head
            width: parent.width
            height: 24
            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: "Call log"
                color: CoreBridge.foreground
                font.pointSize: CoreBridge.basePt
                font.weight: Font.DemiBold
            }
            ToolButton {
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                width: 22; height: 22
                icon: "page"
                iconSize: 16
                checked: true
                tip: "Hide the call log"
                onClicked: root.toggle()
            }
        }

        Rectangle {
            id: listFrame
            width: parent.width
            height: parent.height - head.height - buttons.height - playRow.height
                    - 3 * parent.spacing
            color: CoreBridge.surface
            border.width: 1
            border.color: Fusion.outline(CoreBridge.background)
            ListView {
                id: list
                anchors.fill: parent
                anchors.margins: 1
                clip: true
                model: root.rows
                currentIndex: root.current
                delegate: Rectangle {
                    width: ListView.view.width
                    height: 20
                    readonly property bool selected: index === root.current
                    color: selected ? CoreBridge.accent : "transparent"
                    Glyph {
                        x: 3
                        anchors.verticalCenter: parent.verticalCenter
                        width: 16; height: 16
                        fraction: 1.0
                        visible: modelData.recording !== ""
                        kind: "record"
                        ink: CoreBridge.stop
                    }
                    Text {
                        x: 3 + 16 + 4
                        anchors.verticalCenter: parent.verticalCenter
                        width: parent.width - x - 3
                        text: root.label(modelData)
                        color: parent.selected ? (CoreBridge.dark ? "#000000" : "#FFFFFF")
                               : (modelData.answered ? CoreBridge.foreground : "#b3261e")
                        font.pixelSize: 11
                        elide: Text.ElideRight
                    }
                    TipArea {
                        anchors.fill: parent
                        tip: root.label(modelData)
                             + (modelData.recording !== "" ? "\n" + modelData.recording : "")
                        /* Selecting only. Dialling is the Call back
                           button's job alone: a double-click is too easy
                           to make by accident to be allowed to place a
                           call. */
                        onClicked: root.current = index
                    }
                }
            }
        }

        /* One line when there is room for four buttons, two lines of two
           when there is not: a handset's 270 pixels cut every label. */
        Grid {
            id: buttons
            width: parent.width
            spacing: 3
            readonly property var shown: [traceBtn, textBtn, backBtn,
                                          refreshBtn].filter(
                function(b) { return b.visible })
            columns: shown.length > 3 && width < 4 * 80 + 3 * spacing
                     ? 2 : Math.max(1, shown.length)
            readonly property real cell: (width - (columns - 1) * spacing)
                                         / columns
            PushButton {
                id: traceBtn
                visible: CoreBridge.modules().some(function(m) {
                    return m.id === "sip_trace" && m.enabled })
                width: buttons.cell; height: 24
                text: "SIP trace"
                tip: "Open this call's signalling in the log window, filtered to its Call-ID."
                onClicked: {
                    if (!root.row) { CoreBridge.note("select a call in the log first"); return }
                    if (root.row.callId === "") {
                        CoreBridge.note("that call has no SIP Call-ID recorded, so its "
                                        + "messages cannot be identified");
                        return;
                    }
                    root.trace(root.row.callId);
                }
            }
            PushButton {
                id: textBtn
                visible: root.transcriptOn
                enabled: root.row !== null && root.row.transcript === 2
                width: buttons.cell; height: 24
                text: "Transcript"
                tip: "Read what was said on this call."
                onClicked: root.showTranscript()
            }
            PushButton {
                id: backBtn
                width: buttons.cell; height: 24
                text: "Call back"
                tip: "Dial this call's remote party again."
                onClicked: root.doCallBack()
            }
            PushButton {
                id: refreshBtn
                width: buttons.cell; height: 24
                text: "Refresh"
                onClicked: root.reload()
            }
        }

        Row {
            id: playRow
            width: parent.width
            height: 22
            spacing: 3
            PushButton {
                width: 26; height: 22
                icon: root.sameFile && root.pb.state === 1 ? "pause" : "play"
                iconInk: CoreBridge.muted
                enabled: root.recPath !== "" && !root.callUp
                tip: root.sameFile && root.pb.state === 1 ? "Pause this recording."
                     : root.sameFile && root.pb.state === 2 ? "Resume this recording."
                     : "Play back this call's recording."
                onClicked: {
                    if (root.sameFile && root.pb.state === 1) CoreBridge.playbackPause();
                    else if (root.sameFile && root.pb.state === 2) CoreBridge.playbackResume();
                    else CoreBridge.playbackStart(root.recPath);
                    root.refreshPlayback();
                }
            }
            Slider {
                id: position
                anchors.verticalCenter: parent.verticalCenter
                width: parent.width - 26 - timeLabel.width - speedBtn.width - 26 - 4 * 3
                from: 0
                to: root.sameFile ? Math.max(1, root.pb.durationMs) : 1
                step: 1
                pageStep: Math.max(1, to / 10)
                value: root.sameFile ? root.pb.positionMs : 0
                enabled: root.sameFile && !root.callUp && root.pb.durationMs > 0
                         && root.pb.seekable === true
                onMoved: function(v) { CoreBridge.playbackSeek(Math.round(v)); root.refreshPlayback() }
            }
            Text {
                id: timeLabel
                anchors.verticalCenter: parent.verticalCenter
                width: widest.implicitWidth
                text: root.minSec(root.sameFile ? root.pb.positionMs : 0) + " / "
                      + root.minSec(root.sameFile ? root.pb.durationMs : 0)
                color: CoreBridge.foreground
                font.pixelSize: 10
                Text { id: widest; visible: false; text: "00:00 / 00:00"; font: timeLabel.font }
            }
            PushButton {
                id: speedBtn
                width: speedWidest.implicitWidth + 12; height: 22
                text: root.speedLabel(root.pb.speed || 1.0)
                enabled: root.sameFile && !root.callUp && root.pb.state !== 0
                tip: "Playback speed. Pitch is preserved, so 2x is a voice talking "
                     + "quickly rather than a higher one."
                onClicked: {
                    CoreBridge.playbackSpeed(root.nextSpeed(root.pb.speed || 1.0));
                    root.refreshPlayback();
                }
                Text { id: speedWidest; visible: false; text: "0.5×"
                       font.pointSize: CoreBridge.basePt }
            }
            PushButton {
                width: 26; height: 22
                icon: "saveCopy"
                iconInk: CoreBridge.muted
                enabled: root.recPath !== ""
                tip: "Save a copy of this recording -- this is where it leaves the application."
                onClicked: saver.open()
            }
        }
    }

    FileDialog {
        id: saver
        title: "Save a copy of this recording"
        fileMode: FileDialog.SaveFile
        currentFile: "file://" + root.recPath
        onAccepted: {
            const why = CoreBridge.saveRecordingCopy(root.recPath, selectedFile.toString());
            if (why !== "") { failure.text = why; failure.open() }
        }
    }
    QuestionDialog {
        id: failure
        title: "Save a copy"
        kind: "warning"
    }

    /* Over the list and its buttons; Back returns to them. */
    TranscriptView {
        id: transcriptView
        z: 5
        x: 6; y: 6
        width: parent.width - 12
        height: parent.height - 12
    }
}
