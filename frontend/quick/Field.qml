import QtQuick
import comlink

/* A single-line entry, in the same grammar as everything else. Qt
   Widgets needed a stylesheet to stop a line edit coming back white
   inside a styled panel; here the colour is simply stated. */
Panel {
    id: root
    property alias text: input.text
    property alias placeholder: hint.text
    property bool secret: false
    /* A field the setting does not apply to is shown rather than
       hidden, so the operator can see it exists. */
    /* A read-only field the operator fills through a chooser beside
       it, like the recording and trace-export directories: shown at
       full strength, not editable. */
    property bool readOnly: false
    /* Red text rather than the default ink -- a read-only field
       reporting a failure, the same swap logsdialog.cpp's own
       m_exportPath makes with setStyleSheet("color:#b3261e;"). */
    property bool danger: false
    property string tip: ""
    signal editingFinished()
    /* Fires when the inner TextInput gains focus -- not activeFocus on
       this Item, which a plain Item never gets while its child holds
       the keyboard. A row of two Fields uses this to say which row a
       lone Remove button should act on, the way a QTableWidget's own
       row selection does. */
    signal focused()
    /* Fires for what the operator typed, not for a text set from code:
       a completer that opened on every assignment would pop up over a
       field the moment its row was built. */
    signal edited()
    /* Every key press, before the entry acts on it, so a popup laid
       over the field can take the arrows and Enter for itself by
       accepting the event. */
    signal keyPressed(var event)
    readonly property bool inputFocus: input.activeFocus
    function focusInput() { input.forceActiveFocus() }
    function moveCursorToEnd() { input.cursorPosition = input.length }

    implicitHeight: 32
    /* On the panel rather than on the text, so the border dims with
       the content -- a lit frame around dead text reads as a field
       that is merely empty. */
    opacity: root.enabled ? 1.0 : 0.45

    TipArea {
        anchors.fill: parent
        tip: root.tip
        hoverEnabled: true
        /* No buttons accepted: this is only for the hover tooltip,
           not a click target -- the TextInput below still gets the
           press, same as SpinBox's identical guard. */
        acceptedButtons: Qt.NoButton
    }

    TextInput {
        id: input
        /* In the Tab chain, as a QLineEdit is: Tab goes to the next
           field of the form and Shift+Tab back, without the mouse. */
        activeFocusOnTab: true
        anchors.fill: parent
        anchors.leftMargin: 8
        anchors.rightMargin: 8
        verticalAlignment: TextInput.AlignVCenter
        color: root.danger ? "#b3261e" : CoreBridge.foreground
        font.pixelSize: 13
        clip: true
        echoMode: root.secret ? TextInput.Password : TextInput.Normal
        selectByMouse: true
        enabled: root.enabled
        readOnly: root.readOnly
        onEditingFinished: root.editingFinished()
        onActiveFocusChanged: if (activeFocus) root.focused()
        onTextEdited: root.edited()
        Keys.onPressed: function(event) { root.keyPressed(event) }
        /* A path longer than the field shows its end by default; the
           start is what a technician needs to recognise, so it is
           wound back after every programmatic change, same as the
           0.8.0 recording page's QLineEdit. */
        onTextChanged: if (!activeFocus) cursorPosition = 0
    }
    Text {
        id: hint
        anchors.fill: input
        verticalAlignment: Text.AlignVCenter
        /* The word an Android keyboard is still composing is not in
           text until it is committed, which it is when focus leaves;
           counting only text left the hint drawn under it. */
        visible: input.text === "" && input.preeditText === ""
        color: CoreBridge.muted
        font: input.font
        /* Inside the field, however narrow: unclipped, a long hint
           ran on under whatever sat beside it. */
        elide: Text.ElideRight
    }
}
