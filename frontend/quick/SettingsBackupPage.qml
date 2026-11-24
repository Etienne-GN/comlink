import QtQuick
import QtQuick.Dialogs
import comlink

/* Everything the operator set up, out to one file and back: the
 * configuration, the passwords and the contacts. Uninstalling the
 * application takes all three with it, and this is the copy that
 * survives. */
SettingsPage {
    id: root

    property string status: ""
    property bool failed: false
    property string pending: ""

    function say(why, done) {
        root.failed = why !== "";
        root.status = why !== "" ? why : done;
    }

    signal restoredOk()

    function restore() {
        const why = CoreBridge.restoreFrom(root.pending, openPass.text);
        openPass.text = "";
        root.say(why, "Restored.");
        if (why === "")
            root.restoredOk();
    }

    FileDialog {
        id: saver
        title: "Save a backup"
        fileMode: FileDialog.SaveFile
        defaultSuffix: "comlink-backup"
        nameFilters: ["Comlink backups (*.comlink-backup)", "All files (*)"]
        onAccepted: {
            root.say(CoreBridge.backupTo(selectedFile.toString(),
                                         savePass.text),
                     savePass.text !== ""
                         ? "Backup saved, sealed with your passphrase."
                         : "Backup saved, with the passwords readable " +
                           "in it. Keep it somewhere private.");
            savePass.text = "";
        }
    }
    FileDialog {
        id: opener
        title: "Restore a backup"
        fileMode: FileDialog.OpenFile
        nameFilters: ["Comlink backups (*.comlink-backup)", "All files (*)"]
        onAccepted: {
            root.pending = selectedFile.toString();
            if (CoreBridge.backupIsSealed(root.pending)
                && openPass.text === "") {
                root.say("That backup is sealed. Type its passphrase " +
                         "below, then choose it again.", "");
                return;
            }
            confirm.open();
        }
    }
    QuestionDialog {
        id: confirm
        title: "Restore a backup"
        dialogHeight: 170
        text: "Replace the settings, accounts, passwords and contacts " +
              "on this device with the ones in that backup?\n\n" +
              "Call history and recordings are left as they are."
        onYes: root.restore()
    }

    GroupBox {
        width: parent.width
        text: "Back up"
        description: "Writes your settings, accounts with their " +
                     "passwords, header rules and contacts to one " +
                     "file. Call history, the SIP trace and recordings " +
                     "are not in it. With a passphrase the file is " +
                     "sealed; without one the passwords can be read " +
                     "by anyone who has the file."

        Field {
            id: savePass
            width: parent.width
            height: 30
            secret: true
            placeholder: "Passphrase (optional)"
            tip: "Seals the backup. It is asked for again on restore, " +
                 "and nothing can recover a backup whose passphrase " +
                 "is lost."
        }
        PushButton {
            width: 160; height: 30
            text: "Save a backup…"
            onClicked: saver.open()
        }
    }

    GroupBox {
        width: parent.width
        text: "Restore"
        description: "Replaces the settings, accounts, passwords and " +
                     "contacts on this device with a backup's. Not " +
                     "possible during a call."

        Field {
            id: openPass
            width: parent.width
            height: 30
            secret: true
            placeholder: "Passphrase, if the backup has one"
        }
        PushButton {
            width: 160; height: 30
            text: "Restore a backup…"
            enabled: !CoreBridge.inCall
            onClicked: opener.open()
        }
    }

    Text {
        width: parent.width
        visible: root.status !== ""
        text: root.status
        color: root.failed ? "#b3261e" : CoreBridge.foreground
        font.pointSize: CoreBridge.basePt
        wrapMode: Text.WordWrap
    }
}
