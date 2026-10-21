import QtQuick
import comlink
import "fusion.js" as Fusion

/* What this build is, and where its files are -- aboutdialog.cpp,
 * ported part for part. The paths are here because they are the first
 * thing anyone asks for when something misbehaves, and Copy exists so
 * an operator can paste the lot into a ticket instead of
 * transcribing it.
 *
 * Assembled once as plain text, then shown and copied from the same
 * string: a Copy button that produced something different from what
 * was on screen would be worse than no Copy button. */
Item {
    id: root
    property var host
    anchors.fill: parent

    readonly property string bodyText:
        "Comlink " + CoreBridge.version + "\n"
        + "SIP endpoint and diagnostic instrument for telephony technicians\n\n"
        + "Configured commit: " + CoreBridge.commit + "\n"
        + "Configured: " + CoreBridge.buildDate() + "\n"
        + "pjproject: " + CoreBridge.pjproject + "\n"
        + "Qt: " + CoreBridge.qtVersion + "\n\n"
        + "Config: " + CoreBridge.configPath + "\n"
        + "Database (default): " + CoreBridge.databasePath + "\n"
        + "Bundled sounds: " + CoreBridge.soundDir + "\n"

    Text {
        id: title
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        text: "<b>Comlink " + CoreBridge.version + "</b>"
        textFormat: Text.RichText
        color: CoreBridge.foreground
        font.pointSize: CoreBridge.basePt
    }

    /* QTextBrowser's own frame: a sunken box in the Base colour, which
       here is the theme's surface. */
    Rectangle {
        id: bodyFrame
        anchors.top: title.bottom
        anchors.topMargin: 6
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: btnRow.top
        anchors.bottomMargin: 6
        color: CoreBridge.surface
        border.width: 1
        border.color: Fusion.outline(CoreBridge.background)

        Flickable {
            id: bodyFlick
            anchors.fill: parent
            anchors.margins: 4
            contentHeight: body.implicitHeight
            clip: true

            TextEdit {
                id: body
                width: bodyFlick.width
                text: root.bodyText
                readOnly: true
                selectByMouse: true
                wrapMode: TextEdit.WordWrap
                color: CoreBridge.foreground
                font.pointSize: CoreBridge.basePt
            }
        }
    }

    Row {
        id: btnRow
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        spacing: 6

        PushButton {
            width: 80; height: 30
            text: "Copy"
            tip: "Copy all of this, to paste into a ticket."
            onClicked: CoreBridge.copyToClipboard(root.bodyText)
        }
        PushButton {
            width: 80; height: 30
            text: "Close"
            onClicked: if (root.host) root.host.close()
        }
    }
}
