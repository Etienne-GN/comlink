import QtQuick
import comlink

/* Parts of the phone that can be switched off -- modulespage.cpp,
 * part for part. */
SettingsPage {
    id: root
    signal modulesChanged()

    property var moduleList: []
    property string errorText: ""

    function populate() { root.moduleList = CoreBridge.modules() }
    Component.onCompleted: root.populate()

    GroupBox {
        width: parent.width
        text: "Modules"
        description: "Parts of the phone that can be switched off. " +
                     "Switching one off stops the work and hides its " +
                     "controls; nothing already recorded or logged is " +
                     "removed."

        Repeater {
            model: root.moduleList
            CheckBox {
                wrap: true
                width: parent.width
                text: modelData.label
                checked: modelData.enabled === true
                /* Shown but not switchable, rather than left out:
                   what the phone is built on is worth seeing. */
                enabled: modelData.removable === true
                tip: modelData.removable === true ? ""
                     : "The phone is this. It cannot be switched off."
                onToggled: function(on) {
                    const why = CoreBridge.setModule(modelData.id, on);
                    if (why !== "") {
                        root.errorText = "Could not change "
                                        + modelData.id + ": " + why;
                        /* Repopulating rebinds every row to what the
                           core actually holds, which is the click
                           undone -- the same outcome as
                           modulespage.cpp's QSignalBlocker revert. */
                        root.populate();
                        return;
                    }
                    root.errorText = "";
                    root.populate();
                    root.modulesChanged();
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
        wrapMode: Text.WordWrap
    }
}
