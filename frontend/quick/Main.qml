import QtQuick
import QtQuick.Window
import comlink
import "fusion.js" as Fusion

/* Comlink's window: the Qt Widgets window of 0.8.0, drawn in QML so
 * the same tree runs on Debian and on Android.
 *
 * The layout is mainwindow.cpp's buildUi, part for part: a title bar
 * we own inside a five-pixel resize border, then the phone column --
 * the status row, the call panel, the handle, the pane and the strip
 * that chooses it. The one departure is the call log, which covers
 * the app instead of growing the window to the right: a handset
 * cannot grow its screen.
 *
 * Every colour comes from the core through CoreBridge. */
Window {
    id: root
    width: 380
    height: 660
    /* Deliberately small: the window can be dragged down to roughly a
       dialpad's worth of space. */
    minimumWidth: 210
    minimumHeight: 400
    visible: true

    readonly property bool desktop: Qt.platform.os !== "android"
    /* Frameless on the desktop, because the title bar is ours: Qt
       renders the decoration itself on Wayland and neither plugin
       follows the application palette. Transparent behind, so the
       corners can be rounded. */
    flags: desktop ? (Qt.Window | Qt.FramelessWindowHint) : Qt.Window
    title: "Comlink"
    color: desktop ? "transparent" : CoreBridge.background

    /* The grabbable border of the frameless window, which is also the
       visible border once the corners are rounded. */
    readonly property int edge: desktop ? 5 : 0
    readonly property int pad: 6
    readonly property int gap: 4

    /* What is about to be dialled, when no call is up. */
    property string dialTarget: ""
    /* The ad-hoc headers the next call carries: [{name, value}]. */
    property var staged: []
    property var accountRows: []
    property var callRows: []
    /* The call takes the window; the pane takes the window. */
    property bool expanded: false
    property bool paneExpanded: false
    property bool hadCall: false
    property int pane: CoreBridge.startPane
    property var moduleState: ({})
    property bool activityOffered: true
    /* The loudest each way this call, for the quality pane. */
    property int peakTx: 0
    property int peakRx: 0
    property bool blinkOn: false

    function reloadAccounts() { accountRows = CoreBridge.accounts() }

    /* Measured as it opens, as ComboBox.qml does: a binding on
       mapToItem kept the place the combo had before the window was
       laid out, its top-left corner. */
    function toggleAccountList() {
        reloadAccounts();
        if (!accountList.visible) {
            const p = combo.mapToItem(surface, 0, combo.height);
            accountList.x = p.x;
            accountList.y = p.y;
        }
        accountList.visible = !accountList.visible;
    }
    function reloadCalls() { callRows = CoreBridge.calls() }
    function reloadModules() {
        const m = {};
        for (const row of CoreBridge.modules())
            m[row.id] = row.enabled;
        moduleState = m;
        activityOffered = CoreBridge.showActivityLog();
    }
    function moduleOn(id) { return moduleState[id] !== false }

    readonly property var currentAccount: {
        for (const a of accountRows)
            if (a.current) return a;
        return null;
    }

    Connections {
        target: CoreBridge
        function onAccountChanged() { root.reloadAccounts() }
        function onCallChanged() {
            root.reloadCalls();
            /* Follows the call: open when one starts, closed when it
               ends. Driven by the change rather than the state, so a
               manual collapse during a call stays collapsed. */
            if (CoreBridge.inCall !== root.hadCall) {
                root.hadCall = CoreBridge.inCall;
                root.setExpanded(CoreBridge.inCall);
            }
            if (!CoreBridge.inCall) {
                meter.clear();
                root.peakTx = 0;
                root.peakRx = 0;
                /* Handed back for typing, cleared on the way out of a
                   call, and focused so the next number can be typed --
                   on the desktop. On the handset focus opens the
                   keyboard, over a keypad that is already there. */
                root.dialTarget = "";
                if (root.desktop)
                    prompt.forceActiveFocus();
            }
            if (!CoreBridge.inCall && callLog.visible)
                callLog.reload();
        }
    }

    function setExpanded(on) {
        /* A call cannot be shown in a panel that is not there, so
           asking for the call to fill the window un-hides it first. */
        if (on && paneExpanded)
            paneExpanded = false;
        expanded = on;
    }

    Component.onCompleted: {
        CoreBridge.start();
        reloadAccounts();
        reloadCalls();
        reloadModules();
        const g = CoreBridge.windowSize();
        if (g.width > 0 && g.height > 0) {
            root.width = Math.max(g.width, minimumWidth);
            root.height = Math.max(g.height, minimumHeight);
        }
        if (CoreBridge.showCallLog())
            callLog.open(false);

        if (CoreBridge.startDial !== "") {
            root.dialTarget = CoreBridge.startDial;
            root.dial();
        }
        /* Opened from the outside, so a screenshot and the smoke test
           can reach every secondary window: name[:page]. */
        const spec = CoreBridge.startOverlay.split(":");
        const which = spec[0];
        const page = spec.length > 1 ? parseInt(spec[1]) : 0;
        const dialogs = { accounts: accountsDialog, logs: logsDialog,
                          quality: qualityDialog, about: aboutDialog,
                          headers: headersDialog, settings: settingsDialog,
                          transfer: transferDialog };
        if (dialogs[which] !== undefined) {
            dialogs[which].open();
            Qt.callLater(function() {
                const it = dialogs[which].item;
                if (it && it.hasOwnProperty("page"))
                    it.page = page;
                if (it && it.hasOwnProperty("pageOpen") && spec.length > 1)
                    it.pageOpen = true;
            });
        } else if (which === "calllog") {
            callLog.open(false);
        } else if (which === "calllog-transcript") {
            callLog.open(false);
            Qt.callLater(function() {
                callLog.current = callLog.rows.findIndex(function(r) {
                    return r.transcript === 2 });
                if (callLog.row)
                    callLog.showTranscript();
            });
        } else if (which === "menu") {
            mainMenu.openAt(surface.width - 60, 60);
        } else if (which === "transcript") {
            logsDialog.open();
            Qt.callLater(function() {
                if (logsDialog.item)
                    logsDialog.item.openTranscript(0, "Probe");
            });
        } else if (which === "accountlist") {
            root.toggleAccountList();
        } else if (which === "contact" || which === "picture") {
            root.pane = 4;
            contactsPane.probeOverlay(which);
        } else if (which === "account") {
            accountsDialog.open();
        } else if (which === "account-editor") {
            accountsDialog.open();
            Qt.callLater(function() {
                if (accountsDialog.item)
                    accountsDialog.item.openEditor(0);
            });
        }
    }

    /* Back, on a handset, closes what is open and then hides the
       window rather than ending the application: a softphone nobody
       can reach because Back was pressed is not a phone.

       Only Back, though. Android destroying the activity -- the app
       swiped out of Recents -- reaches here as a close too, after the
       app has already left the foreground, and Qt then waits for the
       event loop to end before letting the process exit. Refusing that
       close left the process alive with no window: the next launch
       showed the splash screen and nothing else. */
    onClosing: function(close) {
        if (root.desktop) {
            CoreBridge.playbackStop();
            CoreBridge.saveWindowSize(root.width, root.height);
            return;
        }
        if (Qt.application.state !== Qt.ApplicationActive)
            return;
        close.accepted = false;
        if (Qt.inputMethod.visible)
            Qt.inputMethod.hide();
        else if (!root.dismissTop())
            CoreBridge.moveToBackground();
    }

    function dismissTop() {
        if (settingsDialog.shown && settingsDialog.item
            && settingsDialog.item.back())
            return true;
        for (const d of [transferDialog, headersDialog, settingsDialog,
                         accountsDialog, aboutDialog, qualityDialog,
                         logsDialog])
            if (d.shown) { d.close(); return true }
        if (mainMenu.visible) { mainMenu.close(); return true }
        if (headerMenu.visible) { headerMenu.close(); return true }
        if (accountList.visible) { accountList.visible = false; return true }
        if (callLog.visible) { callLog.toggle(); return true }
        return false;
    }

    Timer {
        id: geometrySave
        interval: 600
        onTriggered: CoreBridge.saveWindowSize(root.width, root.height)
    }
    onWidthChanged: geometrySave.restart()
    onHeightChanged: geometrySave.restart()

    /* 40 ms: fast enough to see speech shape, cheap enough to run. */
    Timer {
        interval: 40; running: CoreBridge.inCall; repeat: true
        onTriggered: {
            CoreBridge.pollSignal();
            const t = CoreBridge.txLevel(), r = CoreBridge.rxLevel();
            /* A call without media yet is silence, not absence. */
            meter.push(Math.max(0, t), Math.max(0, r));
            if (t > root.peakTx) root.peakTx = t;
            if (r > root.peakRx) root.peakRx = r;
        }
    }
    Timer {
        interval: 1000; running: CoreBridge.inCall; repeat: true
        onTriggered: { CoreBridge.refreshCall(); root.reloadCalls() }
    }
    /* The "lights up" part: while a call rings the panel's border and
       background alternate, so it is visible from across the room. */
    Timer {
        interval: 550; repeat: true
        running: CoreBridge.ringing
        onRunningChanged: root.blinkOn = running
        onTriggered: root.blinkOn = !root.blinkOn
    }

    /* Resolves and dials what is in the number, with the headers
       staged for it. */
    function dial() {
        CoreBridge.silenceFailureTone();
        if (!root.currentAccount) {
            CoreBridge.note("no account selected");
            return;
        }
        const t = root.dialTarget.trim();
        if (t === "") {
            CoreBridge.note("enter an extension or a sip: URI");
            return;
        }
        if (root.staged.length === 0)
            CoreBridge.dial(t);
        else
            CoreBridge.dialWithHeaders(t, root.staged.map(function(h) {
                return h.name + "|" + h.value }));
    }

    /* The on-screen keypad takes over from the keyboard on the
       handset. A field still focused behind a minimised keyboard keeps
       the keyboard's own idea of its text, and edits made around it --
       a digit from the keypad, then the keypad's Backspace -- left the
       two apart: Backspace stopped working until the keyboard came
       back. Committing the keyboard's state and dropping the focus
       first keeps the number the one thing both edit. */
    function leaveField() {
        if (root.desktop || !prompt.activeFocus)
            return;
        Qt.inputMethod.commit();
        prompt.focus = false;
        Qt.inputMethod.hide();
    }

    /* During a call a digit is DTMF; otherwise it is part of the
       number. One rule for the keypad and the keyboard. */
    function enterDigit(d) {
        CoreBridge.silenceFailureTone();
        const left = CoreBridge.enterDigit(d);
        if (left !== "")
            root.dialTarget += left;
        else if (CoreBridge.callState === 5)
            CoreBridge.note("sent DTMF " + d);
    }

    /* Everything is drawn on one surface laid out at the width the
       operator keeps the desktop window at, and scaled to the screen,
       so the handset shows the window he knows at the size he knows
       it: text, panel and keypad in the same proportions. The
       dialogs, menus and tooltips that open over the window open on
       it too, through overlayLayer, so they scale with it. */
    readonly property int handsetLayoutWidth: 270
    readonly property real ui: desktop ? 1.0 : width / handsetLayoutWidth
    readonly property Item overlayLayer: surface

    /* What the handset's own bars cover. Qt draws Android windows
       edge to edge, under the status and navigation bars, and the
       strip at the bottom of the column landed behind the navigation
       bar. Zero on the desktop, where the window has no such bars. */
    Item {
        id: safe
        anchors.fill: parent
        readonly property real insetTop: root.desktop ? 0 : SafeArea.margins.top
        readonly property real insetBottom: root.desktop ? 0 : SafeArea.margins.bottom
        onInsetTopChanged: report()
        onInsetBottomChanged: report()
        Component.onCompleted: report()
        function report() {
            if (!root.desktop)
                console.log("comlink: safe area top=" + insetTop + " bottom="
                            + insetBottom + " window=" + root.width + "x"
                            + root.height);
        }
    }

    Item {
        id: surface
        y: safe.insetTop
        width: root.width / root.ui
        height: (root.height - safe.insetTop - safe.insetBottom) / root.ui
        scale: root.ui
        transformOrigin: Item.TopLeft

        // --- the window's own frame ----------------------------------------
        Rectangle {
            anchors.fill: parent
            visible: root.desktop
            radius: 9
            color: CoreBridge.background
            border.width: 1
            border.color: Qt.rgba(CoreBridge.foreground.r, CoreBridge.foreground.g,
                                  CoreBridge.foreground.b, 70 / 255)
        }

        Repeater {
            model: root.visibility === Window.Maximized || !root.desktop ? [] : [
                { edge: Qt.TopEdge },    { edge: Qt.BottomEdge },
                { edge: Qt.LeftEdge },   { edge: Qt.RightEdge }
            ]
            ResizeEdge {
                base: modelData.edge
                z: 20
                readonly property bool horizontal:
                    modelData.edge === Qt.TopEdge
                    || modelData.edge === Qt.BottomEdge
                width: horizontal ? surface.width : 5
                height: horizontal ? 5 : surface.height
                x: modelData.edge === Qt.RightEdge ? surface.width - 5 : 0
                y: modelData.edge === Qt.BottomEdge ? surface.height - 5 : 0
            }
        }

        /* The keyboard. A focused field consumes its own keys before this
           runs; what is left is a phone, and a digit belongs in the
           number. */
        Item {
            id: keys
            anchors.fill: parent
            focus: true

            Keys.onPressed: function(e) {
                /* Whatever the key, the operator has moved on from the
                   call that failed. */
                CoreBridge.silenceFailureTone();
                const plain = !(e.modifiers & ~Qt.ShiftModifier);
                const t = e.text;
                const dialable = t.length === 1
                    && ((t >= "0" && t <= "9") || t === "*" || t === "#"
                        || t === "+");
                if (dialable && plain) {
                    root.enterDigit(t);
                    e.accepted = true;
                    return;
                }
                if (e.key === Qt.Key_Backspace && plain && !CoreBridge.inCall
                    && root.dialTarget !== "") {
                    root.dialTarget = root.dialTarget.slice(0, -1);
                    e.accepted = true;
                    return;
                }
                if ((e.key === Qt.Key_Return || e.key === Qt.Key_Enter)
                    && !CoreBridge.inCall && root.dialTarget !== "") {
                    root.dial();
                    e.accepted = true;
                }
            }

            Column {
                id: frame
                x: root.edge
                y: root.edge
                width: surface.width - 2 * root.edge
                height: surface.height - 2 * root.edge

                TitleBar {
                    id: titleBar
                    width: parent.width
                    onMinimise: root.showMinimized()
                    onMaximise: root.visibility === Window.Maximized
                                ? root.showNormal() : root.showMaximized()
                    onClosed: root.close()
                }

                // --- the phone column --------------------------------------
                Item {
                    id: phone
                    width: parent.width
                    height: parent.height - titleBar.height

                    Column {
                        id: column
                        x: root.pad
                        y: root.pad
                        width: parent.width - 2 * root.pad
                        height: parent.height - 2 * root.pad
                        spacing: root.gap

                        // --- compact top bar -------------------------------
                        Item {
                            id: topBar
                            width: parent.width
                            height: 24

                            /* The registration control for the selected
                               account: click to register, again to
                               unregister. */
                            ToolButton {
                                id: statusDot
                                anchors.verticalCenter: parent.verticalCenter
                                x: 0
                                width: 22; height: 22
                                enabled: root.currentAccount !== null
                                text: root.currentAccount && root.currentAccount.registered
                                      ? "●" : "○"
                                pixelSize: 14
                                ink: CoreBridge.transportError === ""
                                     && root.currentAccount
                                     && root.currentAccount.registered
                                     ? "#0b6b3a" : "#b3261e"
                                tip: {
                                    const a = root.currentAccount;
                                    if (!a) return "no account";
                                    if (CoreBridge.transportError !== "")
                                        return "transport failed: " + CoreBridge.transportError;
                                    if (a.registered)
                                        return a.name + " is registered — click to unregister";
                                    return a.name + " is not registered"
                                           + (a.regCode ? " (" + a.regCode + " " + a.detail + ")" : "")
                                           + " — click to register";
                                }
                                onClicked: {
                                    const a = root.currentAccount;
                                    if (!a) { CoreBridge.note("no account selected"); return }
                                    CoreBridge.setRegistration(a.id, !a.registered);
                                    root.reloadAccounts();
                                }
                            }

                            /* The account selector: a combo box. */
                            Item {
                                id: combo
                                anchors.verticalCenter: parent.verticalCenter
                                x: statusDot.width + 6
                                width: parent.width - x - 6 - menuBtn.width - logBtn.width
                                height: 24
                                readonly property bool hover: comboArea.containsMouse
                                Rectangle {
                                    anchors.fill: parent
                                    radius: 2
                                    border.width: 1
                                    border.color: Fusion.outline(CoreBridge.background)
                                    gradient: Gradient {
                                        GradientStop { position: 0; color: Fusion.gradientTop(CoreBridge.surface, combo.hover) }
                                        GradientStop { position: 1; color: Fusion.gradientBottom(CoreBridge.surface, combo.hover) }
                                    }
                                    Rectangle {
                                        anchors.fill: parent
                                        anchors.margins: 1
                                        radius: 2
                                        color: "transparent"
                                        border.width: 1
                                        border.color: Fusion.innerContrast
                                    }
                                }
                                Text {
                                    anchors.verticalCenter: parent.verticalCenter
                                    x: 5
                                    width: parent.width - 28
                                    text: root.currentAccount ? root.currentAccount.name : ""
                                    color: CoreBridge.foreground
                                    font.pointSize: CoreBridge.basePt
                                    elide: Text.ElideRight
                                }
                                Canvas {
                                    anchors.verticalCenter: parent.verticalCenter
                                    anchors.right: parent.right
                                    anchors.rightMargin: 8
                                    width: 8; height: 5
                                    property color ink: CoreBridge.foreground
                                    onInkChanged: requestPaint()
                                    onPaint: {
                                        const ctx = getContext("2d");
                                        ctx.reset();
                                        ctx.fillStyle = Qt.rgba(ink.r, ink.g, ink.b, 0.8);
                                        ctx.beginPath();
                                        ctx.moveTo(0, 0); ctx.lineTo(8, 0); ctx.lineTo(4, 5);
                                        ctx.closePath(); ctx.fill();
                                    }
                                }
                                MouseArea {
                                    id: comboArea
                                    anchors.fill: parent
                                    hoverEnabled: true
                                    onClicked: root.toggleAccountList()
                                }
                            }

                            /* Accounts, logs and settings live behind one
                               button, so the window carries only what a
                               call needs. */
                            ToolButton {
                                id: menuBtn
                                anchors.verticalCenter: parent.verticalCenter
                                x: combo.x + combo.width + 6
                                width: 28; height: 22
                                text: "☰"
                                menuIndicator: true
                                tip: "menu"
                                onClicked: {
                                    const p = menuBtn.mapToItem(surface, 0, menuBtn.height);
                                    mainMenu.openAt(p.x, p.y);
                                }
                            }
                            /* Hard against the right edge: the call log is
                               a view toggle, the one control on this bar an
                               operator touches mid-call. */
                            ToolButton {
                                id: logBtn
                                anchors.verticalCenter: parent.verticalCenter
                                anchors.right: parent.right
                                width: 22; height: 22
                                icon: "page"
                                iconSize: 16
                                checked: callLog.visible
                                tip: callLog.visible ? "Hide the call log"
                                                     : "Show the call log"
                                onClicked: callLog.toggle()
                            }
                        }

                        // --- call display ----------------------------------
                        Rectangle {
                            id: display
                            width: parent.width
                            visible: !root.paneExpanded
                            /* Pinned to the size it needs with a call up,
                               so answering does not shove the keypad down
                               the window at the moment someone reaches for
                               it. Only a second call grows it, for the list
                               that then says which call the buttons act
                               on. */
                            /* The ten is the two-pixel border and the
                               three-pixel margins top and bottom; the other
                               four are what the Qt layout added, measured
                               against 0.8.0's 218 at its default font. */
                            readonly property int packed: head.height
                                + Math.max(52, prompt.implicitHeight + 4
                                               + fontProbe.implicitHeight
                                               + clockProbe.implicitHeight)
                                + 2 + meter.height + 2
                                + 26 + callsBoxPacked + 14
                            readonly property int callsBoxPacked: qualityLine.implicitHeight
                                                                  + 3 + callButtons.height
                            /* What the fixed height leaves over once
                               everything showing has its natural size. */
                            readonly property real natural: head.height
                                + Math.max(52, prompt.implicitHeight + 4
                                               + Math.max(uriLine.implicitHeight, fontProbe.implicitHeight)
                                               + clockProbe.implicitHeight
                                               + (whoLine.visible ? whoLine.implicitHeight : 0))
                                + 2 + meter.height + 2
                                + (ringRow.visible ? 26 : 0)
                                + (callsBox.visible ? callsBox.implicitHeight : 0)
                            readonly property real extra: Math.max(0, height - 10 - natural)
                            height: root.expanded
                                    ? Math.max(packed, column.height - topBar.height - root.gap)
                                    : packed + (callList.visible ? callList.height + 3 : 0)
                            radius: 6
                            color: CoreBridge.ringing && root.blinkOn ? CoreBridge.accent
                                                                      : CoreBridge.surface
                            border.width: CoreBridge.ringing ? 3 : 2
                            border.color: CoreBridge.ringing || CoreBridge.inCall
                                          ? CoreBridge.accent : CoreBridge.muted

                            /* A click anywhere on the panel also stops the
                               unanswered-call tone; it takes nothing from
                               the controls on top, which get the press. */
                            MouseArea {
                                anchors.fill: parent
                                onPressed: function(m) {
                                    CoreBridge.silenceFailureTone();
                                    m.accepted = false;
                                }
                            }

                            /* The preset's sky, and its mark behind a call
                               in progress. Inside the border, under
                               everything else. */
                            Starfield {
                                visible: CoreBridge.emblem !== ""
                                x: 3; y: 3
                                width: parent.width - 6
                                height: parent.height - 6
                            }
                            Emblem {
                                visible: CoreBridge.emblem !== ""
                                         && CoreBridge.inCall && !CoreBridge.ringing
                                readonly property real side: Math.min(parent.width, parent.height) * 0.86
                                width: side; height: side
                                anchors.centerIn: parent
                            }

                            Column {
                                id: disp
                                x: 2 + 6
                                y: 2 + 3
                                width: parent.width - 2 * (2 + 6)
                                spacing: 0

                                Item {
                                    id: head
                                    width: parent.width
                                    height: Math.max(title.implicitHeight, 16)
                                    Text {
                                        id: title
                                        anchors.centerIn: parent
                                        text: CoreBridge.callTitle
                                        color: CoreBridge.muted
                                        font.pixelSize: 11
                                        font.letterSpacing: CoreBridge.ringing ? 1 : 0
                                    }
                                    ToolButton {
                                        anchors.right: parent.right
                                        anchors.verticalCenter: parent.verticalCenter
                                        width: 20; height: 16
                                        icon: root.expanded ? "collapse" : "expand"
                                        iconSize: 14
                                        iconInk: CoreBridge.muted
                                        tip: root.expanded ? "Give the window back"
                                                           : "Give the whole window to the call"
                                        onClicked: root.setExpanded(!root.expanded)
                                    }
                                }

                                /* Who, beside the square their picture goes
                                   in. */
                                Row {
                                    id: identity
                                    width: parent.width
                                    spacing: 8
                                    height: Math.max(avatar.height, who.implicitHeight)
                                    clip: true
                                    Avatar {
                                        id: avatar
                                        width: 52; height: 52
                                        name: CoreBridge.inCall ? CoreBridge.contactName : ""
                                    }
                                    Column {
                                        id: who
                                        width: parent.width - 52 - 8
                                        spacing: 0
                                        Text {
                                            id: whoLine
                                            visible: CoreBridge.inCall && CoreBridge.contactName !== ""
                                                     || CoreBridge.ringing
                                            width: parent.width
                                            height: implicitHeight + display.extra / 2
                                            verticalAlignment: Text.AlignVCenter
                                            text: CoreBridge.ringing
                                                  ? (CoreBridge.contactName !== ""
                                                     ? CoreBridge.contactName
                                                     : (CoreBridge.remoteLabel !== ""
                                                        ? CoreBridge.remoteLabel : "unknown"))
                                                  : CoreBridge.contactName
                                            color: CoreBridge.foreground
                                            font.pixelSize: CoreBridge.ringing ? 17 : 15
                                            font.weight: CoreBridge.ringing ? Font.Bold
                                                                            : Font.DemiBold
                                            wrapMode: Text.Wrap
                                        }
                                        /* One field: idle it is where you type
                                           what to dial, in a call it is who you
                                           are talking to, read only. */
                                        Item {
                                            width: parent.width
                                            height: prompt.implicitHeight + 4
                                            TextInput {
                                                id: prompt
                                                anchors.verticalCenter: parent.verticalCenter
                                                width: parent.width - (clearMark.visible ? 20 : 0)
                                                readOnly: CoreBridge.inCall
                                                text: CoreBridge.inCall
                                                      ? (CoreBridge.remoteUser !== ""
                                                         ? CoreBridge.remoteUser
                                                         : (CoreBridge.ringing ? CoreBridge.remoteUri
                                                                               : CoreBridge.remoteLabel))
                                                      : root.dialTarget
                                                onTextEdited: {
                                                    CoreBridge.silenceFailureTone();
                                                    root.dialTarget = text;
                                                }
                                                color: CoreBridge.inCall ? CoreBridge.accent
                                                                         : CoreBridge.foreground
                                                font.pointSize: CoreBridge.basePt * 1.2
                                                font.weight: Font.DemiBold
                                                /* No caret on the desktop: the field
                                                   is focused from the start so digits
                                                   typed anywhere reach it, and a caret
                                                   blinking in an idle phone is noise.
                                                   The handset focuses it only when
                                                   tapped, and without a caret there
                                                   was no telling where a correction
                                                   would land: Backspace at an
                                                   invisible start deleted nothing. */
                                                cursorDelegate: Rectangle {
                                                    width: 2
                                                    color: CoreBridge.foreground
                                                    visible: !root.desktop
                                                             && prompt.cursorVisible
                                                }
                                                selectByMouse: true
                                                clip: true
                                                focus: Qt.platform.os !== "android"
                                                onAccepted: if (!CoreBridge.inCall
                                                                && root.dialTarget !== "")
                                                                root.dial()
                                                Text {
                                                    anchors.verticalCenter: parent.verticalCenter
                                                    visible: prompt.text === "" && prompt.preeditText === ""
                                                    width: parent.width
                                                    text: "extension, or a full sip: URI"
                                                    color: CoreBridge.muted
                                                    font.pointSize: CoreBridge.basePt * 1.2 * 0.68
                                                    elide: Text.ElideRight
                                                }
                                            }
                                            /* QLineEdit's clear button. */
                                            Text {
                                                id: clearMark
                                                visible: !CoreBridge.inCall && root.dialTarget !== ""
                                                anchors.right: parent.right
                                                anchors.verticalCenter: parent.verticalCenter
                                                text: "✕"
                                                color: CoreBridge.muted
                                                font.pointSize: CoreBridge.basePt * 0.9
                                                MouseArea {
                                                    anchors.fill: parent
                                                    anchors.margins: -4
                                                    onClicked: root.dialTarget = ""
                                                }
                                            }
                                        }
                                        Text {
                                            id: uriLine
                                            width: parent.width
                                            /* The labels take what the pinned
                                               panel has spare, as QLabels in a
                                               box layout do, which is what puts
                                               the meter at the bottom of an idle
                                               panel. */
                                            height: Math.max(implicitHeight, fontProbe.implicitHeight)
                                                    + (whoLine.visible ? display.extra / 2
                                                                       : display.extra)
                                            verticalAlignment: Text.AlignVCenter
                                            Text { id: fontProbe; visible: false; text: " "
                                                   font: uriLine.font }
                                            text: !CoreBridge.inCall ? ""
                                                  : CoreBridge.ringing ? CoreBridge.remoteUri
                                                  : CoreBridge.remoteUser
                                                    + (CoreBridge.codec !== ""
                                                       ? "  ·  " + CoreBridge.codec : "")
                                            color: CoreBridge.muted
                                            font.pixelSize: 11
                                            wrapMode: Text.Wrap
                                        }
                                        /* The largest thing on the screen, and
                                           the only large thing. Idle it shows
                                           nothing, but keeps its height. */
                                        Text {
                                            id: clock
                                            width: parent.width
                                            height: clockProbe.implicitHeight
                                            text: CoreBridge.ringing ? "" : CoreBridge.clock
                                            color: text === "" ? CoreBridge.muted : CoreBridge.accent
                                            font.pointSize: CoreBridge.basePt * 1.55
                                            font.weight: Font.Light
                                            font.features: { "tnum": 1 }
                                            Text {
                                                id: clockProbe
                                                visible: false
                                                text: "00:00"
                                                font: clock.font
                                            }
                                        }
                                    }
                                }

                                Item { width: 1; height: 2 }
                                Meter { id: meter; width: parent.width; height: 52 }
                                Item { width: 1; height: 2 }

                                Row {
                                    id: ringRow
                                    width: parent.width
                                    spacing: 6
                                    visible: CoreBridge.ringing
                                    PushButton {
                                        width: (parent.width - 6) / 2
                                        height: 26
                                        text: "Answer"
                                        onClicked: CoreBridge.answer()
                                    }
                                    PushButton {
                                        width: (parent.width - 6) / 2
                                        height: 26
                                        text: "Decline"
                                        onClicked: CoreBridge.hangup(486)
                                    }
                                }

                                /* The figures, the call they are about, and
                                   the buttons that act on it -- the order
                                   you need them in. Hidden with no call. */
                                Column {
                                    id: callsBox
                                    width: parent.width
                                    spacing: 3
                                    visible: root.callRows.length > 0

                                    Text {
                                        id: qualityLine
                                        visible: CoreBridge.qualityLine !== ""
                                        width: parent.width
                                        text: CoreBridge.qualityLine !== ""
                                              ? CoreBridge.qualityLine
                                              : "MOS 0.00 · loss 0.0% · jit 0.0 · rtt 0 ms"
                                        /* Display thresholds, not a standard. */
                                        color: CoreBridge.qualityMos >= 4.0 ? CoreBridge.accent
                                               : CoreBridge.qualityMos < 3.1 ? "#b3261e"
                                               : CoreBridge.foreground
                                        font.pointSize: CoreBridge.basePt
                                        elide: Text.ElideRight
                                    }

                                    /* A chooser, so only with more than one
                                       call: with one it repeats the panel. */
                                    Rectangle {
                                        id: callList
                                        visible: root.callRows.length > 1
                                        width: parent.width
                                        height: Math.min(root.callRows.length, 4) * 20 + 6
                                        color: "transparent"
                                        radius: 3
                                        border.width: 1
                                        border.color: CoreBridge.muted
                                        clip: true
                                        ListView {
                                            anchors.fill: parent
                                            anchors.margins: 3
                                            model: root.callRows
                                            delegate: Rectangle {
                                                width: ListView.view.width
                                                height: 20
                                                color: modelData.current ? CoreBridge.accent
                                                                         : "transparent"
                                                Text {
                                                    anchors.verticalCenter: parent.verticalCenter
                                                    x: 3
                                                    width: parent.width - 6
                                                    text: (modelData.inbound ? "↙" : "↗")
                                                          + " " + (modelData.contact !== ""
                                                                   ? modelData.contact
                                                                   : (modelData.label !== ""
                                                                      ? modelData.label : "?"))
                                                          + "  " + modelData.stateName
                                                          + "  " + modelData.duration + "s"
                                                          + (modelData.onHold ? " [hold]" : "")
                                                          + (modelData.recording ? " [rec]" : "")
                                                    color: modelData.current
                                                           ? (CoreBridge.dark ? "#000000" : "#FFFFFF")
                                                           : CoreBridge.foreground
                                                    font.pointSize: CoreBridge.basePt
                                                    elide: Text.ElideRight
                                                }
                                                MouseArea {
                                                    anchors.fill: parent
                                                    onClicked: {
                                                        CoreBridge.setCurrentCall(modelData.id);
                                                        root.reloadCalls();
                                                    }
                                                }
                                            }
                                        }
                                    }

                                    Row {
                                        id: callButtons
                                        width: parent.width
                                        height: 26
                                        spacing: 3
                                        readonly property real cell: (width - 4 * spacing) / 5
                                        readonly property bool has: root.callRows.length > 0
                                        PushButton {
                                            width: callButtons.cell; height: parent.height
                                            icon: "answer"; iconInk: CoreBridge.go
                                            enabled: callButtons.has
                                            tip: "Answer the selected call"
                                            onClicked: CoreBridge.answer()
                                        }
                                        PushButton {
                                            width: callButtons.cell; height: parent.height
                                            icon: "end"; iconInk: CoreBridge.stop
                                            enabled: callButtons.has
                                            tip: "Hang up the selected call"
                                            onClicked: CoreBridge.hangup(603)
                                        }
                                        PushButton {
                                            width: callButtons.cell; height: parent.height
                                            icon: CoreBridge.onHold ? "resume" : "hold"
                                            enabled: callButtons.has
                                            tip: CoreBridge.onHold ? "Resume the call" : "Hold the call"
                                            onClicked: {
                                                const was = CoreBridge.onHold;
                                                CoreBridge.setHold(!was);
                                                CoreBridge.note(was ? "resumed" : "on hold");
                                            }
                                        }
                                        PushButton {
                                            width: callButtons.cell; height: parent.height
                                            icon: "transfer"
                                            enabled: callButtons.has
                                            tip: "Blind transfer"
                                            onClicked: transferDialog.open()
                                        }
                                        PushButton {
                                            id: recordBtn
                                            width: callButtons.cell; height: parent.height
                                            visible: root.moduleOn("recording")
                                            icon: CoreBridge.recording ? "stopRecording" : "record"
                                            iconInk: CoreBridge.stop
                                            enabled: callButtons.has
                                            tip: CoreBridge.recording ? "Stop recording"
                                                                      : "Start recording"
                                            /* The one control whose state
                                               outlives the press, so it
                                               breathes while it runs: two
                                               seconds a cycle, never all the
                                               way out. */
                                            property real phase: 0
                                            readonly property real g: 0.35 + 0.65
                                                * (0.5 - 0.5 * Math.cos(phase * Math.PI))
                                            glow: CoreBridge.recording ? g : 0
                                            tint: CoreBridge.recording
                                                  ? Qt.rgba(CoreBridge.stop.r, CoreBridge.stop.g,
                                                            CoreBridge.stop.b, (26 + 78 * g) / 255)
                                                  : "transparent"
                                            tintBorder: Qt.rgba(CoreBridge.stop.r, CoreBridge.stop.g,
                                                                CoreBridge.stop.b, (110 + 145 * g) / 255)
                                            Timer {
                                                interval: 60; repeat: true
                                                running: CoreBridge.recording
                                                onRunningChanged: recordBtn.phase = 0
                                                onTriggered: recordBtn.phase =
                                                    (recordBtn.phase + 0.06) % 2.0
                                            }
                                            onClicked: {
                                                if (!CoreBridge.recording && CoreBridge.callState !== 5) {
                                                    CoreBridge.note("call #" + CoreBridge.currentCall()
                                                                    + " is not connected yet");
                                                    return;
                                                }
                                                const was = CoreBridge.recording;
                                                const r = CoreBridge.toggleRecording();
                                                CoreBridge.note(was
                                                    ? "recording stopped for call #" + CoreBridge.currentCall()
                                                    : "recording call #" + CoreBridge.currentCall() + " to " + r);
                                            }
                                        }
                                    }
                                }
                            }
                        }

                        /* The handle between the call panel and whatever
                           is under it: it gives the column to the pane by
                           taking the panel out of it. */
                        ToolButton {
                            id: grip
                            visible: !root.expanded
                            width: parent.width
                            height: 18
                            icon: root.paneExpanded ? "chevronDown" : "chevronUp"
                            iconSize: 22
                            iconInk: CoreBridge.muted
                            tip: root.paneExpanded ? "Show the call panel again"
                                                   : "Give the space to what is below"
                            onClicked: root.paneExpanded = !root.paneExpanded
                        }

                        // --- panes -----------------------------------------
                        Item {
                            id: panes
                            visible: !root.expanded
                            width: parent.width
                            /* Whatever is left: the pane yields before
                               anything else does, and the strip never
                               does. */
                            height: Math.max(0, column.height - y
                                                - (strip.visible ? strip.height + root.gap : 0))
                            clip: true

                            function reportGeometry() {
                                if (!CoreBridge.probing)
                                    return;
                                const p = mapToItem(surface, 0, 0);
                                console.log("PROBE quick pane y=" + Math.round(p.y)
                                            + " h=" + Math.round(height));
                            }
                            Component.onCompleted: reportGeometry()
                            onYChanged: reportGeometry()
                            onHeightChanged: reportGeometry()

                            KeypadPane {
                                anchors.fill: parent
                                visible: root.pane === 0
                                onDigit: function(d) {
                                    root.leaveField();
                                    root.enterDigit(d);
                                }
                                onCall: root.dial()
                                onBackspace: {
                                    root.leaveField();
                                    CoreBridge.silenceFailureTone();
                                    if (!CoreBridge.inCall && root.dialTarget !== "")
                                        root.dialTarget = root.dialTarget.slice(0, -1);
                                }
                            }
                            QualityPane {
                                anchors.fill: parent
                                visible: root.pane === 1
                                peakTx: root.peakTx
                                peakRx: root.peakRx
                                moduleOn: root.moduleOn("audio_quality")
                            }
                            ActivityPane {
                                anchors.fill: parent
                                visible: root.pane === 2
                            }
                            SettingsPane {
                                anchors.fill: parent
                                visible: root.pane === 3
                                staged: root.staged
                                onHeadersMenu: function(x, y) { headerMenu.openAt(x, y) }
                                onEditHeaders: headersDialog.open()
                                onAllSettings: settingsDialog.open()
                            }
                            ContactsPane {
                                id: contactsPane
                                anchors.fill: parent
                                visible: root.pane === 4
                                onDialled: function(uri) {
                                    if (CoreBridge.inCall)
                                        return;
                                    root.dialTarget = uri;
                                    root.dial();
                                }
                            }
                        }

                        // --- strip -----------------------------------------
                        /* What can occupy the pane, in a fixed order, each
                           offered only when it is switched on. A strip of
                           one is not a choice and is not shown. */
                        Row {
                            id: strip
                            readonly property var entries: [
                                { icon: "keypad", tip: "Dialer", page: 0, offered: true },
                                { icon: "contacts", tip: "Who a number belongs to", page: 4,
                                  offered: true },
                                { icon: "quality", tip: "Audio quality for the call in the panel",
                                  page: 1, offered: root.moduleOn("audio_quality") },
                                { icon: "activity", tip: "What the phone has been doing",
                                  page: 2, offered: root.activityOffered },
                                { icon: "settings", tip: "Levels, headers, and the rest",
                                  page: 3, offered: true }
                            ].filter(function(e) { return e.offered })
                            visible: !root.expanded && entries.length > 1
                            width: parent.width
                            height: 2 + 30
                            topPadding: 2
                            spacing: 2
                            onEntriesChanged: {
                                /* A pane that has just been withdrawn falls
                                   back to the keypad rather than leaving a
                                   pane nothing can reach. */
                                if (!entries.some(function(e) { return e.page === root.pane }))
                                    root.pane = 0;
                            }
                            Repeater {
                                model: strip.entries
                                Item {
                                    width: (strip.width - (strip.entries.length - 1) * strip.spacing)
                                           / strip.entries.length
                                    height: 30
                                    readonly property bool here: root.pane === modelData.page
                                    Glyph {
                                        anchors.centerIn: parent
                                        anchors.verticalCenterOffset: -1
                                        width: 18; height: 18
                                        fraction: 1.0
                                        kind: modelData.icon
                                        ink: parent.here ? CoreBridge.accent : CoreBridge.muted
                                    }
                                    /* A rule under the live one, the way a
                                       tab says "you are here". */
                                    Rectangle {
                                        anchors.bottom: parent.bottom
                                        width: parent.width
                                        height: 2
                                        color: parent.here ? CoreBridge.accent : "transparent"
                                    }
                                    TipArea {
                                        anchors.fill: parent
                                        tip: modelData.tip
                                        onClicked: root.pane = modelData.page
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }

        // --- things that open over the window ------------------------------

        /* The account list the combo box drops down. */
        Rectangle {
            id: accountList
            visible: false
            z: 90
            width: combo.width
            height: accountColumn.implicitHeight + 2
            color: CoreBridge.surface
            border.width: 1
            border.color: Fusion.outline(CoreBridge.background)
            Column {
                id: accountColumn
                x: 1; y: 1
                width: parent.width - 2
                Repeater {
                    model: root.accountRows
                    Rectangle {
                        width: accountColumn.width
                        height: 22
                        color: rowArea.containsMouse ? CoreBridge.accent : "transparent"
                        Text {
                            anchors.verticalCenter: parent.verticalCenter
                            x: 5
                            width: parent.width - 10
                            text: modelData.name
                            color: rowArea.containsMouse
                                   ? (CoreBridge.dark ? "#000000" : "#FFFFFF")
                                   : CoreBridge.foreground
                            font.pointSize: CoreBridge.basePt
                            elide: Text.ElideRight
                        }
                        MouseArea {
                            id: rowArea
                            anchors.fill: parent
                            hoverEnabled: true
                            onClicked: {
                                CoreBridge.setCurrentAccount(modelData.id);
                                accountList.visible = false;
                                root.reloadAccounts();
                            }
                        }
                    }
                }
            }
        }

        PopupMenu {
            id: mainMenu
            items: [
                { text: "Accounts…", action: function() { accountsDialog.open() } },
                { text: "Logs…", action: function() { logsDialog.open() } },
                { text: "Quality…", visible: root.moduleOn("audio_quality"),
                  action: function() { qualityDialog.open() } },
                { separator: true },
                { text: "Settings…", action: function() { settingsDialog.open() } },
                { text: "About…", action: function() { aboutDialog.open() } },
                { separator: true },
                { text: "Quit", action: function() { root.close() } }
            ]
        }

        /* Rebuilt each time it opens, so newly saved presets appear. */
        PopupMenu {
            id: headerMenu
            onVisibleChanged: if (visible) items = build()
            function stage(hs, replace) {
                let out = replace ? [] : root.staged.slice();
                for (const h of hs) {
                    /* Same name twice would send a duplicate header, so an
                       existing one is replaced. */
                    const i = out.findIndex(function(e) {
                        return e.name.toLowerCase() === h.name.toLowerCase() });
                    if (i >= 0 && !replace) out[i] = { name: out[i].name, value: h.value };
                    else out.push({ name: h.name, value: h.value });
                }
                root.staged = out;
            }
            function build() {
                const list = [{ text: "Edit headers…",
                                action: function() { headersDialog.open() } }];
                const presets = CoreBridge.headerPresets();
                if (presets.length > 0) {
                    const apply = [], replace = [], del = [];
                    for (const p of presets) {
                        const hs = p.headers.map(function(s) {
                            const k = s.indexOf("|");
                            return { name: s.slice(0, k), value: s.slice(k + 1) } });
                        const label = p.name + "  (" + hs.length + ")";
                        apply.push({ text: label, action: function() {
                            headerMenu.stage(hs, false);
                            CoreBridge.note("applied preset \"" + p.name + "\"") } });
                        replace.push({ text: label, action: function() {
                            headerMenu.stage(hs, true);
                            CoreBridge.note("headers replaced with preset \"" + p.name + "\"") } });
                        del.push({ text: p.name, action: function() {
                            deletePreset.name = p.name;
                            deletePreset.open() } });
                    }
                    list.push({ separator: true });
                    list.push({ text: "Apply preset", sub: apply });
                    list.push({ text: "Replace with preset", sub: replace });
                    list.push({ text: "Delete preset", sub: del });
                }
                list.push({ separator: true });
                list.push({ text: "Clear staged headers", enabled: root.staged.length > 0,
                            action: function() {
                                root.staged = [];
                                CoreBridge.note("staged headers cleared") } });
                return list;
            }
        }

        QuestionDialog {
            id: deletePreset
            property string name: ""
            title: "Delete preset"
            text: "Delete preset \"" + name + "\"?"
            onYes: CoreBridge.note(CoreBridge.deletePreset(name)
                                   ? "deleted preset \"" + name + "\""
                                   : "could not delete \"" + name + "\": "
                                     + CoreBridge.lastError)
        }

        CallLogOverlay {
            id: callLog
            x: root.edge
            y: root.edge + titleBar.height
            width: surface.width - 2 * root.edge
            height: surface.height - 2 * root.edge - titleBar.height
            z: 50
            onCallBack: function(uri) {
                root.dialTarget = uri;
                root.dial();
            }
            onTrace: function(callId) {
                logsDialog.open();
                if (logsDialog.item && logsDialog.item.showCallTrace)
                    logsDialog.item.showCallTrace(callId);
            }
        }

        // --- the secondary windows -----------------------------------------

        DialogHost {
            id: transferDialog
            title: "Transfer"
            modal: true
            dialogWidth: 300; dialogHeight: 110
            content: Component {
                Column {
                    property var host
                    spacing: 8
                    Text {
                        text: "Transfer to extension or URI:"
                        color: CoreBridge.foreground
                        font.pointSize: CoreBridge.basePt
                    }
                    Field { id: xferField; width: parent.width; focus: true }
                    Row {
                        anchors.right: parent.right
                        spacing: 6
                        PushButton {
                            text: "OK"
                            onClicked: {
                                const t = xferField.text.trim();
                                host.close();
                                if (t === "")
                                    return;
                                const ok = CoreBridge.transfer(t);
                                CoreBridge.note(ok ? "transferring to " + t
                                                   : "transfer failed: " + CoreBridge.lastError);
                            }
                        }
                        PushButton { text: "Cancel"; onClicked: host.close() }
                    }
                }
            }
        }

        DialogHost {
            id: headersDialog
            title: "Headers for the next call"
            modal: true
            dialogWidth: 460; dialogHeight: 320
            content: Component {
                HeadersDialog {
                    Component.onCompleted: loadFrom(root.staged)
                    onAccepted: function(headers) {
                        root.staged = headers;
                        CoreBridge.note(headers.length === 0
                            ? "no ad-hoc headers staged"
                            : headers.length
                              + " ad-hoc header(s) staged for the next call");
                        host.close();
                    }
                    onCancelled: host.close()
                }
            }
        }

        DialogHost {
            id: settingsDialog
            title: "Settings"
            modal: true
            dialogWidth: 560; dialogHeight: 460
            minimumWidth: 270; minimumHeight: 300
            onClosed: { root.reloadModules(); root.reloadAccounts() }
            content: Component {
                SettingsDialog {
                    onModulesChanged: root.reloadModules()
                    onActivityLogVisibilityChanged: function(shown) {
                        root.activityOffered = shown;
                    }
                    onWindowSizeRequested:
                        CoreBridge.saveWindowSize(root.width, root.height)
                }
            }
        }

        DialogHost {
            id: accountsDialog
            title: "Accounts"
            modal: true
            dialogWidth: 460; dialogHeight: 340
            onClosed: root.reloadAccounts()
            content: Component {
                AccountsDialog {}
            }
        }

        DialogHost {
            id: logsDialog
            title: "Logs"
            dialogWidth: 760; dialogHeight: 620
            content: Component {
                LogsDialog {}
            }
        }

        DialogHost {
            id: qualityDialog
            title: "Audio quality"
            dialogWidth: 420; dialogHeight: 480
            minimumWidth: 240; minimumHeight: 220
            content: Component {
                QualityDialog {}
            }
        }

        DialogHost {
            id: aboutDialog
            title: "About Comlink"
            modal: true
            dialogWidth: 520; dialogHeight: 400
            minimumWidth: 300; minimumHeight: 260
            content: Component {
                AboutDialog {}
            }
        }
    }
}
