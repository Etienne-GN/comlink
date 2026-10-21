import QtQuick
import QtQuick.Dialogs
import comlink

/* Ringtone, ringback and notification tone. The library, the current
 * choice and playback all come from the core, so Android offers the
 * same sounds from the same source. One frame per sound, saying what
 * it is for; Stop is shared, because the core plays one preview at a
 * time and any Stop stops it. */
SettingsPage {
    id: root

    readonly property var slots: [
        { title: "Ringtone",
          description: "Played when a call comes in. Loops until answered.",
          tip: "Played when a call comes in. Loops until answered." },
        { title: "Ringback",
          description: "Played while your outgoing call rings, only when " +
                       "the far end sends no ringing of its own.",
          tip: "Played while your outgoing call rings, but only when the " +
               "far end sends no ringing audio of its own. Otherwise you " +
               "would hear two." },
        { title: "Notification",
          description: "Played once when a call ends unanswered: " +
                       "rejected, busy, or nobody there.",
          tip: "Played once when a call ends without having been " +
               "answered: rejected, busy, or nobody there." }
    ]

    /* { model, index } for a slot's combo: "None (silent)" first,
       then the core's list, bundled shown plain and imported marked;
       a currently-configured absolute path that matches nothing
       known is appended and shown as itself. */
    function buildFor(slot) {
        const choices = CoreBridge.soundChoices(slot);
        const model = [{ label: "None (silent)", value: "none" }];
        for (const c of choices)
            model.push({ label: c.bundled ? c.name : (c.name + "  (imported)"),
                        value: c.name });
        const cur = CoreBridge.soundInUse(slot);
        let at = -1;
        for (let i = 0; i < model.length; i++)
            if (model[i].value === cur) { at = i; break }
        if (at < 0 && cur !== "" && cur.charAt(0) === "/") {
            model.push({ label: cur, value: cur });
            at = model.length - 1;
        }
        return { model: model, index: at >= 0 ? at : 0 };
    }

    function comboFor(slot) {
        const row = sections.itemAt(slot);
        return row ? row.combo : null;
    }

    function reloadRow(slot) {
        const combo = root.comboFor(slot);
        if (!combo)
            return;
        const b = root.buildFor(slot);
        combo.model = b.model;
        combo.currentIndex = b.index;
    }

    function reloadAll() {
        for (let s = 0; s < root.slots.length; s++)
            root.reloadRow(s);
    }
    Component.onCompleted: root.reloadAll()

    function chose(slot, index) {
        const value = root.comboFor(slot).valueAt(index);
        const why = CoreBridge.setSound(slot, value);
        if (why !== "") {
            warnDlg.text = "Could not select that sound: " + why;
            warnDlg.open();
            root.reloadRow(slot);
        }
    }

    function importFor(slot, url) {
        const why = CoreBridge.importSound(slot, url);
        if (why !== "") {
            importWarn.text = why;
            importWarn.open();
            return;
        }
        /* Select what was just imported: importing and then having
           to find it in the list would be a pointless second step. */
        root.reloadAll();
    }

    QuestionDialog { id: warnDlg; title: "Sounds"; kind: "warning" }
    QuestionDialog { id: importWarn; title: "Import"; kind: "warning" }

    FileDialog {
        id: picker
        property int slot: 0
        title: "Import a sound"
        currentFolder: CoreBridge.musicFolder()
        nameFilters: ["Audio (*.wav *.mp3 *.ogg *.flac *.opus *.m4a)",
                      "All files (*)"]
        onAccepted: root.importFor(picker.slot, selectedFile.toString())
    }

    Repeater {
        id: sections
        model: root.slots

        GroupBox {
            id: section
            property alias combo: combo
            width: parent.width
            text: modelData.title
            description: modelData.description

            /* Combo and buttons on one line where there is the width;
               on a handset the combo takes the line, so a sound's name
               is not cut to a few letters, and the buttons go under. */
            Flow {
                id: line
                readonly property bool stacked: width < 300
                width: parent.width
                spacing: 4
                ComboBox {
                    id: combo
                    width: line.stacked ? line.width
                                        : line.width - 30 - 74 - 4 * 2
                    height: 28
                    tip: modelData.tip
                    onActivated: function(i) { root.chose(index, i) }
                }
                PushButton {
                    width: line.stacked ? (line.width - 4) / 2 : 30
                    height: 28
                    icon: "play"
                    iconSize: 22
                    tip: "Play it once, to hear what you have chosen."
                    /* sp_sound_preview only fails for reasons not worth
                       a popup -- silent slots and a missing file both
                       return SP_OK having done nothing. */
                    onClicked: CoreBridge.previewSound(index)
                }
                PushButton {
                    width: line.stacked ? (line.width - 4) / 2 : 74
                    height: 28
                    text: "Import…"
                    tip: "Copy a sound file into your own sound " +
                         "directory. Anything that is not a WAV is " +
                         "converted first, because the media stack " +
                         "reads WAV only."
                    onClicked: { picker.slot = index; picker.open() }
                }
            }
        }
    }

    footer: Item {
        width: parent.width
        height: Math.max(stopBtn.height, note.implicitHeight)
        Text {
            id: note
            anchors.left: parent.left
            anchors.right: stopBtn.left
            anchors.rightMargin: 8
            anchors.verticalCenter: parent.verticalCenter
            text: "Sounds play to the speaker only. The far end never " +
                  "hears them."
            color: CoreBridge.muted
            font.pointSize: CoreBridge.basePt * 0.9
            wrapMode: Text.WordWrap
        }
        PushButton {
            id: stopBtn
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            width: 74; height: 28
            icon: "stopRecording"
            iconSize: 18
            text: "Stop"
            tip: "Stop whatever is playing. The longer ringtones run " +
                 "for ten seconds."
            onClicked: CoreBridge.previewStop()
        }
    }
}
