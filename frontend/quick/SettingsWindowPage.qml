import QtQuick
import comlink

/* Built inline in settingsdialog.cpp's buildWindowPage() rather than
 * its own file there; kept as its own file here only because every
 * other category already is one. */
SettingsPage {
    id: root

    /* The main window owns the activity pane and its own size, so it
       is told rather than reached into -- settingsdialog.h's own
       signals, forwarded by SettingsDialog.qml. */
    signal activityLogVisibilityChanged(bool shown)
    signal windowSizeRequested()

    GroupBox {
        width: parent.width
        text: "Activity log"
        description: "The pane along the bottom. Turning it off gives " +
                     "the whole window to the phone; the same lines are " +
                     "still recorded and are in the log window."

        CheckBox {
            wrap: true
            width: parent.width
            text: "Show the activity log"
            /* Read once, the same as the constructor's one-time
               setChecked(sp_ui_get_show_activity_log(...)) -- this
               page does not track it live against the main window. */
            checked: CoreBridge.showActivityLog()
            tip: "The pane along the bottom. Turning it off gives the " +
                 "whole window to the phone; the same lines are still " +
                 "recorded and are in the log window."
            onToggled: function(on) {
                CoreBridge.setShowActivityLog(on);
                root.activityLogVisibilityChanged(on);
            }
        }
    }

    /* A handset's window is the screen; there is no size to keep. */
    GroupBox {
        visible: Qt.platform.os !== "android"
        width: parent.width
        text: "Window size"
        description: "Only the size is remembered, not the position: on " +
                     "Wayland the compositor places windows and a " +
                     "remembered position is ignored."

        PushButton {
            width: parent.width; height: 30
            text: "Save this window size"
            tip: "The main window comes back at the size it was saved " +
                 "at. Closing it saves the size too."
            onClicked: root.windowSizeRequested()
        }
    }
}
