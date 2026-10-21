import QtQuick
import comlink
import "fusion.js" as Fusion

/* A QSpinBox as Fusion paints it: a sunken field holding the value (or
   specialValueText at the minimum, the ABI's "the codec's own
   default"), an optional suffix, and two small stepper arrows.
 *
 * Typing commits on editingFinished, like the widget; the arrows and
 * the wheel step by `step`, clamped to from..to. */
Item {
    id: root
    property int from: 0
    property int to: 100
    property int value: 0
    property int step: 1
    property string suffix: ""
    property string specialValueText: ""
    property string tip: ""
    signal edited(int value)

    implicitHeight: 24
    implicitWidth: 90
    opacity: root.enabled ? 1.0 : 0.6

    function clamp(v) { return Math.max(root.from, Math.min(root.to, v)) }
    function displayText() {
        return (root.specialValueText !== "" && root.value === root.from)
               ? root.specialValueText
               : String(root.value) + root.suffix;
    }
    function commit(v) {
        const c = root.clamp(v);
        if (c !== root.value) {
            root.value = c;
            root.edited(c);
        }
        input.text = root.displayText();
    }

    /* What was typed and not yet committed, for a Save that is clicked
       while the entry still holds focus: a QSpinBox's value() already
       includes it, and the value here does not until the entry
       finishes. */
    function interpret() {
        const n = parseInt(input.text, 10);
        if (!isNaN(n))
            root.commit(n);
        else
            input.text = root.displayText();
    }

    Rectangle {
        anchors.fill: parent
        radius: 2
        color: CoreBridge.surface
        border.width: 1
        border.color: Fusion.outline(CoreBridge.background)
    }

    TipArea {
        anchors.fill: parent
        tip: root.tip
        hoverEnabled: true
        acceptedButtons: Qt.NoButton
    }

    TextInput {
        id: input
        /* In the Tab chain, as a QLineEdit is: Tab goes to the next
           field of the form and Shift+Tab back, without the mouse. */
        activeFocusOnTab: true
        anchors.left: parent.left
        anchors.right: steppers.left
        anchors.verticalCenter: parent.verticalCenter
        anchors.leftMargin: 6
        anchors.rightMargin: 4
        clip: true
        color: CoreBridge.foreground
        font.pointSize: CoreBridge.basePt
        selectByMouse: true
        enabled: root.enabled
        text: root.displayText()
        onEditingFinished: {
            const n = parseInt(text, 10);
            if (!isNaN(n))
                root.commit(n);
            else
                text = root.displayText();
        }
        MouseArea {
            anchors.fill: parent
            enabled: root.enabled
            onWheel: function(w) {
                root.commit(root.value + (w.angleDelta.y > 0 ? root.step : -root.step));
            }
        }
    }

    Column {
        id: steppers
        anchors.right: parent.right
        anchors.verticalCenter: parent.verticalCenter
        anchors.rightMargin: 3
        width: 12
        spacing: 1

        Canvas {
            width: 9; height: 6
            onPaint: {
                const ctx = getContext("2d");
                ctx.reset();
                ctx.fillStyle = CoreBridge.foreground;
                ctx.beginPath();
                ctx.moveTo(0, 6); ctx.lineTo(9, 6); ctx.lineTo(4.5, 0);
                ctx.closePath(); ctx.fill();
            }
            MouseArea {
                anchors.fill: parent
                anchors.margins: -2
                enabled: root.enabled
                onClicked: root.commit(root.value + root.step)
            }
        }
        Canvas {
            width: 9; height: 6
            onPaint: {
                const ctx = getContext("2d");
                ctx.reset();
                ctx.fillStyle = CoreBridge.foreground;
                ctx.beginPath();
                ctx.moveTo(0, 0); ctx.lineTo(9, 0); ctx.lineTo(4.5, 6);
                ctx.closePath(); ctx.fill();
            }
            MouseArea {
                anchors.fill: parent
                anchors.margins: -2
                enabled: root.enabled
                onClicked: root.commit(root.value - root.step)
            }
        }
    }
}
