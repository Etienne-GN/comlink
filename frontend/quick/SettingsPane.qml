import QtQuick
import comlink

/* Everything you set once and stop thinking about: the levels you run
   at, and the headers this phone attaches. The rest is behind
   "All settings…", as in the Qt window. */
Item {
    id: root
    /* The ad-hoc headers staged for the next call, owned by the
       window because the Call button sends them. */
    property var staged: []
    signal headersMenu(real x, real y)
    signal editHeaders()
    signal allSettings()

    property var current: ({ tx: 1.0, rx: 1.0, txMuted: false, rxMuted: false })
    function reload() { current = CoreBridge.levels() }
    function push() {
        CoreBridge.setLevels(current.tx, current.rx,
                             current.txMuted, current.rxMuted);
        reload();
    }
    Component.onCompleted: reload()

    Column {
        width: parent.width
        spacing: 8

        /* Two rows of one line each, so the labels stay readable at the
           narrowest usable width. */
        Column {
            width: parent.width
            spacing: 2
            Repeater {
                model: [
                    { glyph: "\u{1F3A4}", key: "tx", mute: "txMuted",
                      tip: "Microphone: what the far end hears",
                      muteTip: "Mute the microphone",
                      said: ["microphone unmuted", "microphone muted"] },
                    { glyph: "\u{1F50A}", key: "rx", mute: "rxMuted",
                      tip: "Speaker: what you hear",
                      muteTip: "Mute the speaker",
                      said: ["speaker unmuted", "speaker muted"] }
                ]
                Row {
                    width: root.width
                    spacing: 3
                    ToolButton {
                        width: 24; height: 22
                        text: modelData.glyph
                        checked: root.current[modelData.mute] === true
                        /* Muted is said in red, as the Qt window does. */
                        ink: checked ? "#b3261e" : CoreBridge.foreground
                        pixelSize: 13
                        tip: modelData.muteTip
                        onClicked: {
                            const c = root.current;
                            c[modelData.mute] = !c[modelData.mute];
                            root.current = c;
                            root.push();
                            CoreBridge.note(modelData.said[c[modelData.mute] ? 1 : 0]);
                        }
                    }
                    Slider {
                        anchors.verticalCenter: parent.verticalCenter
                        width: root.width - 24 - 3
                        from: 0.0; to: 2.0; step: 0.05
                        value: root.current[modelData.key]
                        /* A muted slider is greyed, so a slider that
                           appears to do nothing has a visible reason. */
                        enabled: root.current[modelData.mute] !== true
                        tip: modelData.tip
                        onMoved: function(v) {
                            const c = root.current;
                            c[modelData.key] = v;
                            root.current = c;
                            root.push();
                        }
                    }
                }
            }
        }

        /* The counter is the affordance: it says at a glance how many
           headers the next call will carry. */
        Row {
            width: parent.width
            spacing: 4
            PushButton {
                id: hdrBtn
                width: parent.width - editBtn.width - 4
                flat: true
                menuIndicator: true
                text: root.staged.length === 0
                      ? "Headers: none  ▾"
                      : "Headers: " + root.staged.length + "  ▾"
                /* Lit when something is staged, so a left-over header
                   is visible rather than a surprise on the next call. */
                ink: root.staged.length > 0 ? CoreBridge.accent
                                            : CoreBridge.foreground
                bold: root.staged.length > 0
                tip: root.staged.length === 0
                     ? "no ad-hoc headers on the next call"
                     : root.staged.map(function(h) {
                           return h.name + ": " + h.value }).join("\n")
                onClicked: {
                    const p = hdrBtn.mapToItem(hdrBtn.Window.window.overlayLayer || null, 0, hdrBtn.height);
                    root.headersMenu(p.x, p.y);
                }
            }
            PushButton {
                id: editBtn
                width: Math.max(56, implicitWidth)
                text: "Edit…"
                onClicked: root.editHeaders()
            }
        }

        PushButton {
            width: parent.width
            text: "All settings…"
            tip: "Accounts, sounds, themes, codecs and modules"
            onClicked: root.allSettings()
        }
    }
}
