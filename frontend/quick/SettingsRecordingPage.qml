import QtQuick
import QtQuick.Dialogs
import comlink

/* Where recordings land and in what form, and where a trace export
 * goes -- recordingpage.cpp, part for part. Every value is read from
 * and written to the core; this page holds no state of its own. */
SettingsPage {
    id: root

    property var settings: ({})
    /* Set by a failure and left until the next one, as
       recordingpage.cpp's m_detail is: load() never clears it. */
    property string errorText: ""

    readonly property var formatModel: [
        { label: "WAV, 16-bit PCM — about 1920 KB a minute", value: 0 },
        { label: "WAV, G.711 µ-law — about 960 KB a minute",
          value: 1 },
        { label: "Opus — about 180 KB a minute", value: 2 }
    ]

    function load() {
        root.settings = CoreBridge.recordingSettings();
        formatCombo.currentIndex = root.settings.format === undefined
                                    ? 0 : root.settings.format;
    }
    Component.onCompleted: root.load()

    FolderDialog {
        id: recDirPicker
        title: "Where recordings go"
        onAccepted: {
            const why = CoreBridge.setRecordingDir(selectedFolder.toString());
            if (why !== "")
                root.errorText = "That directory could not be used: " + why;
            root.load();
        }
    }
    FolderDialog {
        id: traceDirPicker
        title: "Where trace exports go"
        onAccepted: {
            const why = CoreBridge.setTraceDir(selectedFolder.toString());
            if (why !== "")
                root.errorText = "That directory could not be used: " + why;
            root.load();
        }
    }

    GroupBox {
        width: parent.width
        text: "Recordings"
        description: "Where a recording is written. The default is a " +
                     "recordings directory beside your configuration " +
                     "file. A recording is somebody's voice, so where " +
                     "you point this matters more than a chooser can show."

        Row {
            width: parent.width
            spacing: 4
            Field {
                width: parent.width - 74 - 4
                height: 30
                readOnly: true
                text: root.settings.dir || ""
                tip: "Where a recording is written. The default is a " +
                     "recordings directory beside your configuration " +
                     "file."
            }
            PushButton {
                width: 74; height: 30
                text: "Choose…"
                onClicked: recDirPicker.open()
            }
        }
        CheckBox {
            wrap: true
            width: parent.width
            text: "Record every call automatically"
            checked: root.settings.auto === true
            tip: "Starts recording as soon as a call is answered, into " +
                 "the directory and format above. Stopping it with Rec " +
                 "during a call stops it for that call. Needs the " +
                 "recording module."
            onToggled: function(on) {
                const why = CoreBridge.setRecordingAuto(on);
                if (why !== "")
                    root.errorText = "That could not be changed: " + why;
                root.load();
            }
        }
    }

    GroupBox {
        width: parent.width
        text: "Format"
        description: "All three record the mixed conversation at the " +
                     "bridge's 16 kHz, not a copy of what crossed the " +
                     "wire. µ-law halves the WAV file size for a modest " +
                     "loss of quality; Opus is roughly a fifth the size " +
                     "of WAV and plays in current browsers and in VLC, " +
                     "but Windows support depends on the version " +
                     "installed -- for a recording that may one day " +
                     "leave this building, WAV is the format nobody has " +
                     "to argue with."

        ComboBox {
            id: formatCombo
            width: parent.width
            height: 28
            model: root.formatModel
            onActivated: function(index) {
                const why = CoreBridge.setRecordingFormat(
                    formatCombo.valueAt(index));
                if (why !== "")
                    root.errorText = "That format could not be used: " + why;
                root.load();
            }
        }
    }

    GroupBox {
        width: parent.width
        text: "Trace exports"
        description: "Where the Export button in the log window writes " +
                     "a call's SIP messages."

        Row {
            width: parent.width
            spacing: 4
            Field {
                width: parent.width - 74 - 4
                height: 30
                readOnly: true
                text: root.settings.traceDir || ""
                tip: "Where the Export button in the log window writes " +
                     "a call's SIP messages."
            }
            PushButton {
                width: 74; height: 30
                text: "Choose…"
                onClicked: traceDirPicker.open()
            }
        }
    }

    Text {
        width: parent.width
        visible: root.errorText !== ""
        text: root.errorText
        color: "#b3261e"
        font.pointSize: CoreBridge.basePt
        wrapMode: Text.WordWrap
    }
}
