import QtQuick
import comlink

/* The call display's avatar, a port of frontend/qt/avatar.cpp.
 *
 * A rounded square, not a circle: it is the frame a contact picture
 * will sit in, and a picture clipped to the same shape is what makes
 * a contact look like itself in the list, the editor and the call
 * panel. A wash of the accent rather than a block of it, because this
 * is not a thing to look at itself.
 *
 * The initials rule is sp_contact_initials, in the core. */
Rectangle {
    id: root
    property string name: ""
    /* A contact's own picture when there is one. Clipped to the same
       rounded square the initials sit in, so a contact looks like
       itself in the list, the editor and the call panel. */
    property var contactId: 0
    /* A picture not yet written to the core -- the crop dialog's own
       preview, so a change stays undoable by Cancel the way
       contacteditor.cpp's local m_pictureBytes is: nothing here
       reaches CoreBridge.contactPicture until the edit is saved. Takes
       over from contactId's own picture when set; forceNoPicture takes
       over from both, for "Remove picture" staged the same way. */
    property string overridePicture: ""
    property bool forceNoPicture: false
    readonly property string picture:
        forceNoPicture ? ""
        : overridePicture !== "" ? overridePicture
        : (contactId ? CoreBridge.contactPicture(contactId) : "")
    readonly property string initials: CoreBridge.initialsOf(name)

    implicitWidth: 52
    implicitHeight: 52
    radius: 5
    color: Qt.rgba(accent.r, accent.g, accent.b, 28 / 255)
    border.width: 1
    border.color: Qt.rgba(accent.r, accent.g, accent.b, 110 / 255)
    readonly property color accent: CoreBridge.accent

    /* clip on the rounded parent is what gives the picture the same
       corners as the frame around it. */
    Image {
        anchors.fill: parent
        anchors.margins: 1
        visible: root.picture !== ""
        source: root.picture
        fillMode: Image.PreserveAspectCrop
        clip: true
    }

    Text {
        anchors.centerIn: parent
        visible: root.picture === "" && root.initials !== ""
        text: root.initials
        color: CoreBridge.accent
        font.pixelSize: root.height * 0.42
        font.bold: true
    }

    /* Nobody known: head and shoulders, in the same stroke weight as
       the icons so it belongs to the same set. */
    Canvas {
        anchors.fill: parent
        visible: root.picture === "" && root.initials === ""
        onPaint: {
            const ctx = getContext("2d");
            ctx.reset();
            const k = width / 52;
            ctx.lineWidth = 1.8 * k;
            ctx.lineCap = "round";
            ctx.strokeStyle = Qt.rgba(CoreBridge.foreground.r,
                                      CoreBridge.foreground.g,
                                      CoreBridge.foreground.b, 120 / 255);
            const cx = width / 2, cy = height / 2;
            ctx.beginPath();
            ctx.ellipse(cx - 6 * k, cy - 11 * k, 12 * k, 12 * k);
            ctx.stroke();
            ctx.beginPath();
            ctx.moveTo(cx - 11 * k, cy + 15 * k);
            ctx.bezierCurveTo(cx - 10 * k, cy + 3 * k,
                              cx + 10 * k, cy + 3 * k,
                              cx + 11 * k, cy + 15 * k);
            ctx.stroke();
        }
    }
}
