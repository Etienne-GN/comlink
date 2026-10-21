import QtQuick
import QtQuick.Dialogs
import comlink
import "fusion.js" as Fusion

/* The Logs window -- logsdialog.cpp, ported part for part: the
 * redaction banner, the sort row shared by all three lists, a
 * SIP trace / Activity / Calls tab set, and Close.
 *
 * Hosted modeless in Main.qml's DialogHost (no `modal: true`), the
 * same as 0.8.0's show()/raise(): a trace should stay watchable while
 * a call is up. showCallTrace(callId) is called from the outside --
 * the call log panel's "SIP trace" button, Main.qml's onTrace -- the
 * same as logsdialog.cpp's own public showCallTrace().
 *
 * `page` selects among the tabs actually on offer, the same
 * declarative hook SettingsDialog.qml's category sidebar uses, and
 * how a probe opens straight to one ("logs:N"). */
Item {
    id: root
    property var host
    anchors.fill: parent

    property int page: 0

    /* SIP trace is a tab only when the module is on -- the same
       condition logsdialog.cpp's constructor checks before calling
       m_tabs->addTab(). A tab that would draw nothing is left out
       rather than shown empty. */
    readonly property bool sipTraceOn: CoreBridge.modules().some(function(m) {
        return m.id === "sip_trace" && m.enabled })
    readonly property bool transcriptOn: CoreBridge.modules().some(function(m) {
        return m.id === "transcript" && m.enabled;
    })
    readonly property var tabs: (root.sipTraceOn
            ? [{ id: "sip", label: "SIP trace" }] : [])
        .concat([{ id: "activity", label: "Activity" },
                 { id: "calls", label: "Calls" }])
    onTabsChanged: if (root.page >= root.tabs.length) root.page = 0
    readonly property string currentTab:
        root.page >= 0 && root.page < root.tabs.length
            ? root.tabs[root.page].id : ""

    property int order: CoreBridge.logOrder()

    // --- SIP trace state -------------------------------------------
    property string filterText: ""
    property var sipRows: []
    property int sipCurrent: -1
    property string exportedPath: ""
    property bool exportFailed: false

    // --- activity state ----------------------------------------------
    property var actRows: []

    // --- call history state --------------------------------------------
    property var histRows: []
    property int histCurrent: -1
    property var pb: ({ state: 0 })

    function reloadAll() { reloadSip(); reloadActivity(); reloadHistory() }
    Component.onCompleted: root.reloadAll()

    function reloadSip() {
        root.sipRows = CoreBridge.siplog(root.filterText.trim(), 500);
        root.sipCurrent = root.sipRows.length > 0 ? 0 : -1;
    }
    function reloadActivity() { root.actRows = CoreBridge.activity(1000) }
    function reloadHistory() {
        root.histRows = CoreBridge.history(500);
        root.refreshPlayback();
    }
    function refreshPlayback() { root.pb = CoreBridge.playback() }
    /* Polled at well inside 250ms, the same interval m_playPoll runs
       at: a position bar that updates once a second looks broken. */
    Timer {
        interval: 200; running: root.visible; repeat: true
        onTriggered: root.refreshPlayback()
    }

    /* Enabled only while the SIP list's selection carries a Call-ID --
       logsdialog.cpp's own updateExportEnabled(). */
    readonly property var sipRow: (root.sipCurrent >= 0
            && root.sipCurrent < root.sipRows.length)
        ? root.sipRows[root.sipCurrent] : null
    readonly property bool exportEnabled: root.sipRow !== null
        && root.sipRow.callId !== ""

    readonly property var histRow: (root.histCurrent >= 0
            && root.histCurrent < root.histRows.length)
        ? root.histRows[root.histCurrent] : null
    readonly property string recPath: root.histRow ? root.histRow.recording : ""
    readonly property bool sameFile: root.recPath !== ""
        && root.pb.path === root.recPath
    readonly property bool callUp: CoreBridge.calls().length > 0

    /* The point of the feature: a call and its signalling in one
       place. */
    function showCallTrace(callId) {
        if (callId === "")
            return;
        root.filterText = callId;
        filterField.text = callId;
        root.reloadSip();
        const i = root.tabs.findIndex(function(t) { return t.id === "sip" });
        if (i >= 0)
            root.page = i;
    }

    function openTranscript(id, who) { transcriptView.show(id, who) }

    function openSelectedCallTrace() {
        if (!root.histRow)
            return;
        if (root.histRow.callId === "") {
            noTrace.open();
            return;
        }
        root.showCallTrace(root.histRow.callId);
    }

    function stamp(us) {
        return Qt.formatDateTime(new Date(us / 1000), "HH:mm:ss.zzz");
    }
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
    function sipRowLabel(r) {
        const dir = r.direction === "tx" ? "tx" : "rx";
        const statusStr = r.status ? String(r.status) : "";
        return root.stamp(r.tsUs) + "  " + dir + "  " + r.method + statusStr
               + "  " + (r.redacted ? "" : "[verbatim]");
    }
    function actRowLabel(r) { return root.stamp(r.tsUs) + "  " + r.text }
    function histRowLabel(r) {
        const when = Qt.formatDateTime(new Date(r.startUs / 1000),
                                       "yyyy-MM-dd HH:mm:ss");
        let s = when + "  " + (r.inbound ? "in " : "out") + "  "
                + r.remote + "  " + r.duration + "s  code=" + r.code
                + (r.codec !== "" ? "  " + r.codec : "")
                + (r.recording !== "" ? "  [rec]" : "")
                + (r.transcript === 1 ? "  [txt…]"
                   : r.transcript === 2 ? "  [txt]"
                   : r.transcript === 3
                     ? "  [txt failed" + (r.transcriptWhy !== ""
                                          ? ": " + r.transcriptWhy : "") + "]"
                   : "");
        if (r.qPackets > 0)
            s += "  MOS " + r.mos.toFixed(2) + "  loss "
                 + r.qLossPct.toFixed(1) + "%";
        return s;
    }

    /* Stops only this dialog's own row's playback, and only if it is
       what is loaded -- never the main window's transport -- the same
       guard logsdialog.cpp's done() override keeps. Runs when the
       DialogHost's Loader tears this item down, which is this
       component's equivalent of a QDialog::done() override. */
    Component.onDestruction: {
        if (root.recPath !== "" && root.pb.path === root.recPath)
            CoreBridge.playbackStop();
    }

    // --- banner ------------------------------------------------------
    Rectangle {
        id: banner
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        visible: !CoreBridge.settings().sipLogRedact
        height: visible ? bannerText.implicitHeight + 12 : 0
        color: "#b3261e"
        Text {
            id: bannerText
            anchors.centerIn: parent
            width: parent.width - 12
            text: "Redaction is OFF — credentials are being written to disk"
            color: "white"
            font.bold: true
            font.pointSize: CoreBridge.basePt
            wrapMode: Text.WordWrap
            horizontalAlignment: Text.AlignHCenter
        }
    }

    Row {
        id: sortRow
        anchors.top: banner.bottom
        anchors.topMargin: banner.visible ? 6 : 0
        anchors.left: parent.left
        height: 26
        spacing: 4
        Text {
            anchors.verticalCenter: parent.verticalCenter
            text: "Sort:"
            color: CoreBridge.foreground
            font.pointSize: CoreBridge.basePt
        }
        PushButton {
            width: 100; height: 26
            text: "Newest first"
            /* The sunken look a checkable QPushButton gets while
               checked, painted over the ordinary gradient rather than
               swapping the text colour -- tint/tintBorder exist on
               PushButton for exactly this. */
            tint: root.order === 0
                  ? Qt.rgba(CoreBridge.accent.r, CoreBridge.accent.g,
                           CoreBridge.accent.b, 55 / 255) : "transparent"
            tintBorder: root.order === 0 ? CoreBridge.accent : "transparent"
            onClicked: {
                root.order = 0;
                CoreBridge.setLogOrder(0);
                root.reloadAll();
            }
        }
        PushButton {
            width: 100; height: 26
            text: "Oldest first"
            tint: root.order === 1
                  ? Qt.rgba(CoreBridge.accent.r, CoreBridge.accent.g,
                           CoreBridge.accent.b, 55 / 255) : "transparent"
            tintBorder: root.order === 1 ? CoreBridge.accent : "transparent"
            onClicked: {
                root.order = 1;
                CoreBridge.setLogOrder(1);
                root.reloadAll();
            }
        }
    }

    // --- tabs ----------------------------------------------------------
    Rectangle {
        id: tabBar
        anchors.top: sortRow.bottom
        anchors.topMargin: 6
        anchors.left: parent.left
        anchors.right: parent.right
        height: 26
        color: CoreBridge.surface
        border.width: 1
        border.color: Fusion.outline(CoreBridge.background)
        Row {
            anchors.fill: parent
            anchors.margins: 1
            Repeater {
                model: root.tabs
                Rectangle {
                    readonly property bool here: index === root.page
                    width: Math.max(80, tabLabel.implicitWidth + 20)
                    height: parent.height
                    color: here ? CoreBridge.accent : "transparent"
                    Text {
                        id: tabLabel
                        anchors.centerIn: parent
                        text: modelData.label
                        color: parent.here
                               ? (CoreBridge.dark ? "#000000" : "#FFFFFF")
                               : CoreBridge.foreground
                        font.pointSize: CoreBridge.basePt
                    }
                    MouseArea {
                        anchors.fill: parent
                        onClicked: root.page = index
                    }
                }
            }
        }
    }

    Item {
        id: tabContent
        anchors.top: tabBar.bottom
        anchors.topMargin: 6
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: closeRow.top
        anchors.bottomMargin: 6

        // --- SIP trace -----------------------------------------------
        Item {
            id: sipPage
            anchors.fill: parent
            visible: root.currentTab === "sip"

            /* One line on the desktop; on a handset the filter takes
               a line of its own and the three buttons share the next,
               rather than squeezing the filter to nothing. */
            Flow {
                id: filterRow
                readonly property bool stacked: width < 400
                readonly property real buttonWidth:
                    stacked ? (width - 4 * 2) / 3 : 70
                anchors.top: parent.top
                anchors.left: parent.left
                anchors.right: parent.right
                spacing: 4
                Field {
                    id: filterField
                    width: filterRow.stacked ? filterRow.width
                                             : filterRow.width - 70 * 3 - 4 * 3
                    height: 30
                    placeholder: "filter by Call-ID, blank for everything"
                    onTextChanged: root.filterText = text
                    onEditingFinished: root.reloadSip()
                }
                PushButton {
                    width: filterRow.buttonWidth; height: 30
                    text: "Refresh"
                    onClicked: root.reloadSip()
                }
                PushButton {
                    width: filterRow.buttonWidth; height: 30
                    text: "Clear"
                    ink: "#b3261e"
                    onClicked: clearSipConfirm.open()
                }
                PushButton {
                    width: filterRow.buttonWidth; height: 30
                    text: "Export"
                    tip: "Write the selected call's SIP messages to a file."
                    enabled: root.exportEnabled
                    onClicked: {
                        const p = CoreBridge.exportTrace(root.sipRow.callId);
                        root.exportFailed = p === "";
                        root.exportedPath = root.exportFailed
                            ? "export failed" : p;
                    }
                }
            }

            Row {
                id: exportRow
                anchors.top: filterRow.bottom
                anchors.topMargin: 4
                anchors.left: parent.left
                anchors.right: parent.right
                height: 30
                spacing: 4
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    text: "Exported to:"
                    color: CoreBridge.foreground
                    font.pointSize: CoreBridge.basePt
                }
                Field {
                    width: parent.width - 84
                    height: 30
                    readOnly: true
                    danger: root.exportFailed
                    placeholder: "(nothing exported yet)"
                    text: root.exportedPath
                }
            }

            /* The QSplitter logsdialog.cpp puts between the message list
               and the message text: vertical, a handle the operator
               drags, and neither pane fixed in size. The list
               starts at the height a list widget asks for and the text
               takes the rest, and a pane can be dragged shut, as the
               widget's children are collapsible unless told otherwise
               -- logsdialog.cpp never tells it. A window resize is
               taken up by the text first. */
            Item {
                id: split
                anchors.top: exportRow.bottom
                anchors.topMargin: 4
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom

                readonly property real handleHeight: 4
                /* What a list widget or a plain text edit will not be
                   squeezed below, short of collapsing altogether. */
                readonly property real minPane: 70
                readonly property real room: Math.max(0, height - handleHeight)
                property real listWant: 192
                property bool textShut: false
                readonly property real listHeight:
                    textShut ? room
                    : (listWant === 0 ? 0
                       : Math.max(minPane, Math.min(listWant, room - minPane)))

                /* Halfway under its minimum is where a pane gives up and
                   collapses, and where it stops following the handle. */
                function dragTo(wanted) {
                    if (wanted < minPane / 2) {
                        textShut = false;
                        listWant = 0;
                    } else if (room - wanted < minPane / 2) {
                        textShut = true;
                    } else {
                        textShut = false;
                        listWant = Math.max(minPane,
                                            Math.min(wanted, room - minPane));
                    }
                }
            }

            Item {
                id: splitHandle
                anchors.top: split.top
                anchors.topMargin: split.listHeight
                anchors.left: parent.left
                anchors.right: parent.right
                height: split.handleHeight

                /* Fusion's grip: a row of small dots, lit from above, the
                   top-left pixel of each in shadow. */
                Repeater {
                    model: 6
                    Item {
                        x: Math.floor(splitHandle.width / 2) - 7 + index * 3
                        y: 1
                        width: 2; height: 2
                        Rectangle {
                            anchors.fill: parent
                            color: Qt.rgba(1, 1, 1, 90 / 255)
                        }
                        Rectangle {
                            width: 1; height: 1
                            color: Qt.rgba(0, 0, 0, 60 / 255)
                        }
                    }
                }
                MouseArea {
                    anchors.fill: parent
                    cursorShape: Qt.SplitVCursor
                    property real grabY: 0
                    property real grabHeight: 0
                    onPressed: function(m) {
                        grabY = mapToItem(split, m.x, m.y).y;
                        grabHeight = split.listHeight;
                    }
                    onPositionChanged: function(m) {
                        if (pressed)
                            split.dragTo(grabHeight
                                         + mapToItem(split, m.x, m.y).y - grabY);
                    }
                }
            }

            Rectangle {
                id: sipListFrame
                anchors.top: split.top
                anchors.left: parent.left
                anchors.right: parent.right
                height: split.listHeight
                visible: height > 0
                color: CoreBridge.surface
                border.width: 1
                border.color: CoreBridge.muted
                ListView {
                    id: sipList
                    anchors.fill: parent
                    anchors.margins: 1
                    clip: true
                    model: root.sipRows
                    delegate: Rectangle {
                        width: sipList.width
                        height: 18
                        readonly property bool selected: index === root.sipCurrent
                        color: selected ? CoreBridge.accent : "transparent"
                        Text {
                            x: 4
                            anchors.verticalCenter: parent.verticalCenter
                            width: parent.width - 8
                            text: root.sipRowLabel(modelData)
                            color: parent.selected
                                   ? (CoreBridge.dark ? "#000000" : "#FFFFFF")
                                   : (modelData.direction === "tx"
                                      ? "#0b5394" : CoreBridge.foreground)
                            font.family: "monospace"
                            font.pixelSize: 11
                            elide: Text.ElideRight
                        }
                        MouseArea {
                            anchors.fill: parent
                            onClicked: root.sipCurrent = index
                        }
                    }
                }
            }

            Rectangle {
                id: sipTextFrame
                anchors.top: splitHandle.bottom
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                visible: height > 0
                color: CoreBridge.surface
                border.width: 1
                border.color: CoreBridge.muted
                Flickable {
                    anchors.fill: parent
                    anchors.margins: 4
                    contentHeight: sipText.implicitHeight
                    contentWidth: sipText.implicitWidth
                    clip: true
                    TextEdit {
                        id: sipText
                        readOnly: true
                        selectByMouse: true
                        selectByKeyboard: true
                        color: CoreBridge.foreground
                        selectionColor: CoreBridge.accent
                        selectedTextColor: CoreBridge.dark ? "#000000" : "#FFFFFF"
                        font.family: "monospace"
                        font.pixelSize: 11
                        text: root.sipRows.length === 0
                              ? "no messages captured"
                              : (root.sipRow === null ? ""
                                 : (root.sipRow.callId !== ""
                                    ? "Call-ID: " + root.sipRow.callId + "\n\n"
                                      + CoreBridge.sipMessageAt(
                                            root.filterText.trim(),
                                            root.sipRow.index)
                                    : CoreBridge.sipMessageAt(
                                          root.filterText.trim(),
                                          root.sipRow.index)))
                    }
                }
            }
        }

        // --- activity --------------------------------------------------
        Item {
            id: actPage
            anchors.fill: parent
            visible: root.currentTab === "activity"

            Rectangle {
                id: actListFrame
                anchors.top: parent.top
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: actRow.top
                anchors.bottomMargin: 4
                color: CoreBridge.surface
                border.width: 1
                border.color: CoreBridge.muted
                ListView {
                    anchors.fill: parent
                    anchors.margins: 1
                    clip: true
                    model: root.actRows
                    delegate: Text {
                        width: ListView.view.width
                        height: 18
                        x: 4
                        text: root.actRowLabel(modelData)
                        color: CoreBridge.foreground
                        font.pixelSize: 11
                        elide: Text.ElideRight
                        verticalAlignment: Text.AlignVCenter
                    }
                }
            }
            Row {
                id: actRow
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                height: 30
                spacing: 4
                PushButton {
                    width: 70; height: 30
                    text: "Refresh"
                    onClicked: root.reloadActivity()
                }
                PushButton {
                    width: 70; height: 30
                    text: "Clear"
                    ink: "#b3261e"
                    onClicked: clearActivityConfirm.open()
                }
            }
        }

        // --- calls -------------------------------------------------------
        Item {
            id: callsPage
            anchors.fill: parent
            visible: root.currentTab === "calls"

            Rectangle {
                id: histListFrame
                anchors.top: parent.top
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: histRow_.top
                anchors.bottomMargin: 4
                color: CoreBridge.surface
                border.width: 1
                border.color: CoreBridge.muted
                ListView {
                    id: histList
                    anchors.fill: parent
                    anchors.margins: 1
                    clip: true
                    model: root.histRows
                    delegate: Rectangle {
                        width: histList.width
                        height: 18
                        readonly property bool selected: index === root.histCurrent
                        color: selected ? CoreBridge.accent : "transparent"
                        Glyph {
                            x: 3
                            anchors.verticalCenter: parent.verticalCenter
                            width: 14; height: 14
                            fraction: 1.0
                            visible: modelData.recording !== ""
                            kind: "record"
                            ink: CoreBridge.stop
                        }
                        Text {
                            x: 3 + 14 + 4
                            anchors.verticalCenter: parent.verticalCenter
                            width: parent.width - x - 4
                            text: root.histRowLabel(modelData)
                            color: parent.selected
                                   ? (CoreBridge.dark ? "#000000" : "#FFFFFF")
                                   : (modelData.answered ? CoreBridge.foreground
                                                          : "#b3261e")
                            font.pixelSize: 11
                            elide: Text.ElideRight
                        }
                        TipArea {
                            anchors.fill: parent
                            tip: modelData.recording
                            onClicked: root.histCurrent = index
                            onDoubleClicked: {
                                root.histCurrent = index;
                                root.openSelectedCallTrace();
                            }
                        }
                    }
                }
            }
            /* The trace button on the left and Refresh and Clear on
               the right, the desktop's; on a handset the trace button
               takes a line and the other two share the next. */
            Flow {
                id: histRow_
                /* What the buttons take on one line. Never below 350,
                   the width the toolbar has always wrapped at. */
                readonly property real need: 70 * 2 + 4 * 2
                    + (root.sipTraceOn ? 190 + 4 : 0)
                    + (root.transcriptOn ? 190 + 4 : 0)
                readonly property bool stacked: width < Math.max(350, need)
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: playRow.top
                anchors.bottomMargin: 4
                spacing: 4
                PushButton {
                    width: histRow_.stacked ? histRow_.width : 190; height: 30
                    visible: root.sipTraceOn
                    text: "Show this call's SIP trace"
                    onClicked: root.openSelectedCallTrace()
                }
                PushButton {
                    width: histRow_.stacked ? histRow_.width : 190; height: 30
                    visible: root.transcriptOn
                    enabled: root.histRow !== null
                             && root.histRow.transcript === 2
                    text: "Show this call's transcript"
                    onClicked: root.openTranscript(root.histRow.id,
                                                   root.histRow.remote)
                }
                Item {
                    visible: !histRow_.stacked
                    width: histRow_.width - histRow_.need
                    height: 30
                }
                PushButton {
                    width: histRow_.stacked ? (histRow_.width - 4) / 2 : 70
                    height: 30
                    text: "Refresh"
                    onClicked: root.reloadHistory()
                }
                PushButton {
                    id: clearHistBtn
                    width: histRow_.stacked ? (histRow_.width - 4) / 2 : 70
                    height: 30
                    text: "Clear"
                    ink: "#b3261e"
                    onClicked: clearHistConfirm.open()
                }
            }

            /* The transport for the selected row's recording -- same
               controls, same tooltips, same layout as the call log
               panel's own (CallLogOverlay.qml), because it is the
               same core playback the two surfaces share. */
            Row {
                id: playRow
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                height: 22
                spacing: 4
                PushButton {
                    width: 26; height: 22
                    icon: root.sameFile && root.pb.state === 1 ? "pause" : "play"
                    iconInk: CoreBridge.muted
                    enabled: root.recPath !== "" && !root.callUp
                    tip: root.sameFile && root.pb.state === 1
                         ? "Pause this recording."
                         : root.sameFile && root.pb.state === 2
                           ? "Resume this recording."
                           : "Play back the selected call's recording."
                    onClicked: {
                        if (root.sameFile && root.pb.state === 1)
                            CoreBridge.playbackPause();
                        else if (root.sameFile && root.pb.state === 2)
                            CoreBridge.playbackResume();
                        else
                            CoreBridge.playbackStart(root.recPath);
                        root.refreshPlayback();
                    }
                }
                Slider {
                    id: position
                    anchors.verticalCenter: parent.verticalCenter
                    width: parent.width - 26 - timeLabel.width - speedBtn.width
                           - 26 - 4 * 4
                    from: 0
                    to: root.sameFile ? Math.max(1, root.pb.durationMs) : 1
                    step: 1
                    pageStep: Math.max(1, to / 10)
                    value: root.sameFile ? root.pb.positionMs : 0
                    enabled: root.sameFile && !root.callUp
                             && root.pb.durationMs > 0
                             && root.pb.seekable === true
                    onMoved: function(v) {
                        CoreBridge.playbackSeek(Math.round(v));
                        root.refreshPlayback();
                    }
                }
                Text {
                    id: timeLabel
                    anchors.verticalCenter: parent.verticalCenter
                    width: widest.implicitWidth
                    text: root.minSec(root.sameFile ? root.pb.positionMs : 0)
                          + " / "
                          + root.minSec(root.sameFile ? root.pb.durationMs : 0)
                    color: CoreBridge.foreground
                    font.pixelSize: 10
                    Text { id: widest; visible: false; text: "00:00 / 00:00"
                           font: timeLabel.font }
                }
                PushButton {
                    id: speedBtn
                    width: speedWidest.implicitWidth + 12; height: 22
                    text: root.speedLabel(root.pb.speed || 1.0)
                    enabled: root.sameFile && !root.callUp && root.pb.state !== 0
                    tip: "Playback speed. Pitch is preserved, so 2x is a "
                         + "voice talking quickly rather than a higher one."
                    onClicked: {
                        CoreBridge.playbackSpeed(
                            root.nextSpeed(root.pb.speed || 1.0));
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
                    tip: "Save a copy of this recording -- this is where "
                         + "it leaves the application."
                    onClicked: saver.open()
                }
            }
        }
    }

    Item {
        id: closeRow
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        height: 30
        PushButton {
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            width: 90; height: 30
            text: "Close"
            onClicked: if (root.host) root.host.close()
        }
    }

    /* "Save a copy...": a native save chooser, then a checked copy
       out of the application's own recordings directory -- the same
       pairing logsdialog.cpp's saveRecordingCopy() makes, just with
       the checked-copy half of it living in CoreBridge so both
       platforms share one implementation. */
    FileDialog {
        id: saver
        title: "Save a copy of this recording"
        fileMode: FileDialog.SaveFile
        currentFile: "file://" + root.recPath
        onAccepted: {
            const why = CoreBridge.saveRecordingCopy(root.recPath,
                                                      selectedFile.toString());
            if (why !== "") { saveFailed.text = why; saveFailed.open() }
        }
    }
    QuestionDialog {
        id: saveFailed
        kind: "warning"
        title: "Save a copy"
    }

    /* Over the tabs and the sort row, under the Close row. */
    TranscriptView {
        id: transcriptView
        z: 5
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: closeRow.top
        anchors.bottomMargin: 6
    }
    /* A transcript finishing while the window is open moves its row
       from [txt…] to [txt] without a Refresh. */
    Connections {
        target: CoreBridge
        function onTranscriptChanged() { root.reloadHistory() }
    }

    QuestionDialog {
        id: clearSipConfirm
        title: "Clear SIP log"
        text: "Delete every captured SIP message?"
        onYes: { CoreBridge.clearLog("sip"); root.reloadSip() }
    }
    QuestionDialog {
        id: clearActivityConfirm
        title: "Clear activity"
        text: "Delete the entire action history?"
        onYes: { CoreBridge.clearLog("activity"); root.reloadActivity() }
    }
    QuestionDialog {
        id: clearHistConfirm
        title: "Clear history"
        text: "Delete the entire call history?"
        onYes: { CoreBridge.clearLog("history"); root.reloadHistory() }
    }
    QuestionDialog {
        id: noTrace
        kind: "warning"
        title: "No trace"
        text: "This call has no SIP Call-ID recorded, so its messages "
              + "cannot be identified. That happens when a call fails "
              + "before any signalling exists."
    }
}
