import QtQuick
import comlink

/* QMessageBox::question and ::warning: a sentence and the buttons that
   answer it, modal, in a window of its own on the desktop. */
DialogHost {
    id: box
    property string text: ""
    /* "question" asks Yes or No; "warning" only says OK. */
    property string kind: "question"
    signal yes()

    modal: true
    dialogWidth: 360
    dialogHeight: 130
    content: Component {
        Item {
            property var host
            Text {
                width: parent.width
                text: box.text
                color: CoreBridge.foreground
                font.pointSize: CoreBridge.basePt
                wrapMode: Text.Wrap
            }
            Row {
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                spacing: 6
                PushButton {
                    text: box.kind === "question" ? "Yes" : "OK"
                    onClicked: {
                        box.close();
                        if (box.kind === "question")
                            box.yes();
                    }
                }
                PushButton {
                    visible: box.kind === "question"
                    text: "No"
                    onClicked: box.close()
                }
            }
        }
    }
}
