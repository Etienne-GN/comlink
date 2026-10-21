import QtQuick
import comlink

/* The speech model the transcript module needs, and where it comes
 * from. Holds no state of its own: what is on disk is ModelFetcher's
 * answer, and which file is in use is the core's. */
SettingsPage {
    id: root

    property var models: []
    property string inUse: ""
    property int choice: 0
    property string errorText: ""
    readonly property bool available: CoreBridge.transcriptAvailable()

    readonly property var picked: root.models.length > root.choice
                                  ? root.models[root.choice] : null

    function load() {
        root.models = ModelFetcher.models();
        root.inUse = CoreBridge.transcriptModel();
    }
    function use(path) {
        const why = CoreBridge.setTranscriptModel(path);
        root.errorText = why === "" ? ""
                                    : "The model could not be used: " + why;
        root.load();
    }
    Component.onCompleted: root.load()

    Connections {
        target: ModelFetcher
        function onChanged() { root.load() }
        /* main.cpp has already put a first model in use by now. */
        function onFinished(name, path) { root.load() }
    }

    GroupBox {
        width: parent.width
        text: "Speech model"
        description: "Calls are transcribed on this device after they " +
                     "end. Nothing is sent anywhere. The model is " +
                     "downloaded once, from whisper.cpp's own repository."

        Text {
            width: parent.width
            visible: !root.available
            wrapMode: Text.Wrap
            color: "#b3261e"
            font.pointSize: CoreBridge.basePt
            text: "This build has no speech engine: calls are not " +
                  "transcribed, whatever the module says."
        }
        FormRow {
            text: "Model"
            ComboBox {
                width: parent.width
                height: 28
                model: root.models.map(function(m) {
                    return m.name + " — " + m.sizeMb + " MB"
                           + (m.present ? ", downloaded" : "");
                })
                currentIndex: root.choice
                tip: "small understands French noticeably better; base " +
                     "is a third of the size and faster."
                onActivated: function(i) { root.choice = i }
            }
        }
        Text {
            width: parent.width
            wrapMode: Text.Wrap
            color: ModelFetcher.state === "error" ? "#b3261e"
                                                  : CoreBridge.muted
            font.pointSize: CoreBridge.basePt
            text: ModelFetcher.state === "downloading"
                  ? "Downloading… " + Math.round(ModelFetcher.progress * 100)
                    + " %"
                  : ModelFetcher.state === "error" ? ModelFetcher.error
                  : root.inUse !== "" ? "In use: " + root.inUse
                  : "No model yet: calls are not transcribed."
        }
        Flow {
            width: parent.width
            spacing: 4
            PushButton {
                width: 96; height: 30
                text: "Download"
                visible: ModelFetcher.state !== "downloading"
                enabled: root.available && root.picked !== null
                         && !root.picked.present
                onClicked: ModelFetcher.download(root.picked.name)
            }
            PushButton {
                width: 96; height: 30
                text: "Cancel"
                visible: ModelFetcher.state === "downloading"
                onClicked: ModelFetcher.cancel()
            }
            PushButton {
                width: 96; height: 30
                text: "Use"
                enabled: root.picked !== null && root.picked.present
                         && root.picked.path !== root.inUse
                onClicked: root.use(root.picked.path)
            }
            PushButton {
                width: 96; height: 30
                text: "Delete"
                ink: "#b3261e"
                enabled: root.picked !== null && root.picked.present
                         && ModelFetcher.state !== "downloading"
                onClicked: {
                    if (root.picked.path === root.inUse)
                        CoreBridge.setTranscriptModel("");
                    ModelFetcher.remove(root.picked.name);
                    root.load();
                }
            }
        }
    }

    Text {
        width: parent.width
        visible: root.errorText !== ""
        text: root.errorText
        color: "#b3261e"
        font.pointSize: CoreBridge.basePt
        wrapMode: Text.Wrap
    }
}
