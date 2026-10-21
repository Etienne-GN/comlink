import QtQuick
import comlink

/* Audio device selection -- audiopage.cpp, part for part. The device
 * list and the selection both come from the core, so the Android
 * build offers the same choices from the same source. */
SettingsPage {
    id: root

    /* The two entries that are not real devices, spelled the way the
       core understands them. */
    readonly property string defaultName: "default"
    readonly property string noneName: "none"
    /* SP_AUDIO_RAW and SP_AUDIO_PLUGIN's ordinal in the ABI's enum. */
    readonly property int rawKind: 3
    readonly property int pluginKind: 4

    property var devices: []
    property bool showAll: false
    property bool headless: false
    property string captureValue: defaultName
    property string playbackValue: defaultName
    property int hidden: 0
    /* Shared between the device description and the processing
       status, the same QLabel 0.8.0 reuses for both. */
    property string detail: ""
    property var dspMap: ({})

    function deviceLabel(d) {
        let shown = d.label;
        if (d.kind === root.rawKind)
            shown += "  (raw, no conversion)";
        else if (d.kind === root.pluginKind)
            shown += "  (ALSA plugin)";
        return shown;
    }

    function buildModel(direction) {
        const out = [{ label: "System default", value: root.defaultName }];
        let hiddenCount = 0;
        for (const d of root.devices) {
            if (!root.showAll && !d.recommended) { hiddenCount++; continue }
            if (direction === "in" && d.inputs > 0)
                out.push({ label: root.deviceLabel(d), value: d.name });
            if (direction === "out" && d.outputs > 0)
                out.push({ label: root.deviceLabel(d), value: d.name });
        }
        root.hidden = hiddenCount;
        return out;
    }

    readonly property var captureModel: root.buildModel("in")
    readonly property var playbackModel: root.buildModel("out")

    function indexOfValue(model, value) {
        for (let i = 0; i < model.length; i++)
            if (model[i].value === value) return i;
        return -1;
    }

    /* Repopulating loses a selection that is no longer offered, which
       is the correct outcome: it says plainly that the entry is no
       longer on offer. A combo with nothing selected still shows its
       first entry, "System default", the same as clear() followed by
       addItem() leaves a QComboBox showing. */
    function syncCombos() {
        let ci = root.indexOfValue(root.captureModel, root.captureValue);
        if (ci < 0) ci = 0;
        let pi = root.indexOfValue(root.playbackModel, root.playbackValue);
        if (pi < 0) pi = 0;
        captureCombo.currentIndex = ci;
        playbackCombo.currentIndex = pi;
        root.captureValue = root.captureModel[ci].value;
        root.playbackValue = root.playbackModel[pi].value;
    }

    /* Only the playback selection is described, even when the
       capture combo is what just changed -- audiopage.cpp's
       describeSelection() reads m_playback regardless of which
       combo fired it, and this reproduces that exactly. */
    function describeSelection() {
        if (root.headless) {
            root.detail = "Audio is off. Calls still connect, SIP is " +
                          "still traced and quality is still measured " +
                          "from RTCP; nothing is played or captured.";
            return;
        }
        for (const d of root.devices) {
            if (d.name !== root.playbackValue)
                continue;
            root.detail = d.name + " — " + d.driver + " driver, "
                          + d.rate + " Hz, " + d.inputs + " in / "
                          + d.outputs + " out";
            return;
        }
        if (root.hidden > 0)
            root.detail = root.hidden + " ALSA plumbing entries hidden. " +
                          "Tick \"Show all\" if you need one.";
        else
            root.detail = "Whatever the system is using.";
    }

    function reload() {
        devices = CoreBridge.audioDevices();
        const inUse = CoreBridge.audioDevicesInUse();
        const cap = inUse.capture || root.defaultName;
        const play = inUse.playback || root.defaultName;
        root.headless = cap === root.noneName || play === root.noneName;
        root.captureValue = cap === root.noneName ? root.defaultName : cap;
        root.playbackValue = play === root.noneName ? root.defaultName : play;
        root.syncCombos();
        root.describeSelection();
        root.dspMap = CoreBridge.dsp();
        ecTailSpin.value = root.dspMap.echoTailMs > 0
                            ? root.dspMap.echoTailMs : 200;
    }
    Component.onCompleted: root.reload()

    function applyProcessing() {
        root.dspMap = CoreBridge.dsp();
        root.detail = "Saved. Echo settings apply now; codec settings " +
                      "apply to the next call.";
    }

    function apply() {
        let cap, play;
        if (root.headless) {
            cap = root.noneName;
            play = root.noneName;
        } else {
            cap = root.captureValue;
            play = root.playbackValue;
        }
        const why = CoreBridge.setAudioDevices(cap, play);
        if (why !== "") {
            warnDlg.text = why;
            warnDlg.open();
            return;
        }
        /* Persisted by the core, so say so: the operator should not
           have to wonder whether this survives a restart. */
        root.detail = "Applied and saved.";
    }

    QuestionDialog { id: warnDlg; title: "Audio"; kind: "warning" }

    GroupBox {
        width: parent.width
        text: "Devices"
        description: "Where calls are heard and spoken. A new choice is " +
                     "used once applied."

        FormRow {
            text: "Microphone"
            dim: root.headless
            ComboBox {
                id: captureCombo
                width: parent.width
                height: 28
                enabled: !root.headless
                model: root.captureModel
                onActivated: function(index) {
                    root.captureValue = captureCombo.valueAt(index);
                    root.describeSelection();
                }
            }
        }
        FormRow {
            text: "Speaker"
            dim: root.headless
            ComboBox {
                id: playbackCombo
                width: parent.width
                height: 28
                enabled: !root.headless
                model: root.playbackModel
                onActivated: function(index) {
                    root.playbackValue = playbackCombo.valueAt(index);
                    root.describeSelection();
                }
            }
        }
        CheckBox {
            wrap: true
            width: parent.width
            text: "Show all ALSA devices"
            enabled: !root.headless
            checked: root.showAll
            tip: "Includes rate converters, channel maps, software " +
                 "mixers and raw hw: entries. Useful when you know " +
                 "exactly what you want; noise otherwise."
            onToggled: function(on) {
                root.showAll = on;
                root.syncCombos();
                root.describeSelection();
            }
        }
        CheckBox {
            wrap: true
            width: parent.width
            text: "No audio device (headless)"
            checked: root.headless
            tip: "Register, signal and capture SIP traces with no " +
                 "sound card. Calls connect and metrics are still " +
                 "measured; there is simply no audio."
            onToggled: function(on) {
                root.headless = on;
                root.describeSelection();
            }
        }
    }

    GroupBox {
        width: parent.width
        text: "Processing"
        description: "Echo, noise and gain take effect at once, even on " +
                     "a call already up; the rest apply to the next call."

        CheckBox {
            wrap: true
            width: parent.width
            text: "Echo cancellation"
            checked: root.dspMap.echoCancel === true
            tip: "On by default: a speakerphone without it " +
                 "howls, and that is not a diagnosis. Takes " +
                 "effect at once, including on a call already " +
                 "up."
            onToggled: function(on) {
                CoreBridge.setDsp("echoCancel", on);
                root.applyProcessing();
            }
        }
        SpinBox {
            id: ecTailSpin
            width: parent.width
            from: 0; to: 500; step: 20
            suffix: " ms tail"
            tip: "How much echo history the canceller keeps. " +
                 "Longer copes with a slower path and costs " +
                 "more CPU."
            onEdited: function(v) {
                CoreBridge.setEchoTailMs(v);
                root.applyProcessing();
            }
        }
        CheckBox {
            wrap: true
            width: parent.width
            text: "Noise suppression"
            checked: root.dspMap.noiseSuppression === true
            tip: "Off by default. It changes what you hear, " +
                 "which is the opposite of useful when the " +
                 "audio itself is what you are diagnosing. " +
                 "Takes effect at once."
            onToggled: function(on) {
                CoreBridge.setDsp("noiseSuppression", on);
                root.applyProcessing();
            }
        }
        CheckBox {
            wrap: true
            width: parent.width
            text: "Automatic gain control"
            checked: root.dspMap.autoGain === true
            tip: "Off by default, for the same reason as noise " +
                 "suppression. Takes effect at once."
            onToggled: function(on) {
                CoreBridge.setDsp("autoGain", on);
                root.applyProcessing();
            }
        }
        CheckBox {
            wrap: true
            id: vadBox
            width: parent.width
            text: "Silence suppression (VAD)"
            checked: root.dspMap.silenceSuppression === true
            tip: "Off by default, unlike an ordinary softphone: " +
                 "a phone that stops sending during silence " +
                 "reports jitter and loss measured over a " +
                 "handful of packets. Applies to the next call."
            onToggled: function(on) {
                CoreBridge.setDsp("silenceSuppression", on);
                root.applyProcessing();
            }
        }
        CheckBox {
            wrap: true
            width: parent.width
            text: "Comfort noise"
            enabled: vadBox.checked
            checked: root.dspMap.comfortNoise === true
            tip: "Fills suppressed silence with low-level " +
                 "noise so the line does not sound dead. Only " +
                 "audible with silence suppression on. " +
                 "Applies to the next call."
            onToggled: function(on) {
                CoreBridge.setDsp("comfortNoise", on);
                root.applyProcessing();
            }
        }
        CheckBox {
            wrap: true
            width: parent.width
            text: "Packet loss concealment"
            checked: root.dspMap.packetLossConcealment === true
            tip: "On by default. Turning it off is a " +
                 "diagnostic technique: you hear each lost " +
                 "packet instead of the stack hiding it. " +
                 "Applies to the next call."
            onToggled: function(on) {
                CoreBridge.setDsp("packetLossConcealment", on);
                root.applyProcessing();
            }
        }
    }

    footer: [
        Text {
            width: parent.width
            text: root.detail
            color: CoreBridge.muted
            font.pointSize: CoreBridge.basePt * 0.9
            wrapMode: Text.WordWrap
        },
        PushButton {
            width: parent.width
            height: 30
            text: "Apply device selection"
            onClicked: root.apply()
        }
    ]
}
