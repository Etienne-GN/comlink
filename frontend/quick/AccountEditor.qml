import QtQuick
import comlink

/* One account -- accountsdialog.cpp's AccountEditDialog, part for
 * part: the same twelve rows in the same order with the same
 * placeholders, Save and Cancel, and the complaint above the buttons
 * where the dialog shows it.
 *
 * The password does not travel the way the Widgets window sends it.
 * CoreBridge.accountCfg reads it from the credential store (or, for an
 * installation that predates the store, from the configuration) and
 * CoreBridge.saveAccount puts it back there; this form only carries
 * `passwordKnown` from one to the other, so a store that could not be
 * read is never taken for an instruction to erase what it holds. See
 * tests/test_migration.cpp. */
Item {
    id: root
    property var host
    /* var, not int: an account handle is 64-bit and QML's int is not. */
    property var editing: 0
    property bool passwordKnown: true
    property string problem: ""
    /* A refusal from the core, or a partial failure worth saying:
       AccountsDialog shows it once this dialog has closed, as
       accountsdialog.cpp's fail() does. */
    signal failed(string text)

    readonly property int rowHeight: 26
    readonly property int gap: 6
    readonly property int labelWidth: Math.ceil(labelMetrics.advanceWidth)

    TextMetrics {
        id: labelMetrics
        text: "Registration expiry"
        font.pointSize: CoreBridge.basePt
    }

    function load(id) {
        editing = id;
        problem = "";
        const a = CoreBridge.accountCfg(id);
        passwordKnown = a.passwordKnown;
        name.text = a.id;
        user.text = a.user;
        domain.text = a.domain;
        authUser.text = a.authUser;
        password.text = a.password;
        registrar.text = a.registrar;
        proxy.text = a.proxy;
        stun.text = a.stun;
        transport.currentIndex = a.transport;
        srtp.currentIndex = a.srtp;
        /* A new account starts at the form's own default; an existing
           one is clamped into the box's range, the way QSpinBox
           clamps what it is given. */
        expires.value = id === 0 ? 300 : expires.clamp(a.expires);
        doRegister.checked = a.doRegister;
    }

    function form() {
        return {
            "id": name.text, "user": user.text, "domain": domain.text,
            "authUser": authUser.text, "password": password.text,
            "registrar": registrar.text, "proxy": proxy.text,
            "stun": stun.text, "transport": transport.currentIndex,
            "srtp": srtp.currentIndex, "expires": expires.value,
            "doRegister": doRegister.checked,
            "passwordKnown": root.passwordKnown
        };
    }

    /* Checked here so the operator sees the problem next to the
       field rather than as a failure from the core after the dialog
       has closed. */
    function complaint() {
        if (name.text.trim() === "")
            return "Name is required.";
        if (name.text.indexOf(" ") >= 0)
            return "Name cannot contain spaces.";
        if (user.text.trim() === "")
            return "User is required.";
        if (domain.text.trim() === "")
            return "Domain is required.";
        if (doRegister.checked && password.text === "")
            return "A password is needed to register. Untick "
                   + "registration if this account only places "
                   + "direct IP calls.";
        return "";
    }

    function save() {
        expires.interpret();
        const why = root.complaint();
        if (why !== "") {
            root.problem = why;
            return;
        }
        const adding = root.editing === 0;
        const saved = CoreBridge.saveAccount(root.editing, root.form());
        if (saved === 0)
            root.failed((adding ? "Adding the account"
                                : "Updating the account")
                        + " failed.\n\n" + CoreBridge.lastError);
        else if (CoreBridge.lastError !== "")
            root.failed(CoreBridge.lastError);
        if (root.host)
            root.host.close();
    }

    Component.onCompleted: name.focusInput()

    /* Save is the dialog's default button and Cancel answers Escape,
       as in a QDialogButtonBox. */
    Keys.onReturnPressed: root.save()
    Keys.onEnterPressed: root.save()
    Keys.onEscapePressed: if (root.host) root.host.close()

    /* The dialog is as tall as its form, and taller by the complaint
       while there is one, as a QDialog is. */
    Binding {
        target: root.host
        property: "dialogHeight"
        value: Math.ceil(flick.contentHeight) + 2 * 9 + 2 * 2
        when: root.host !== undefined && root.host !== null
    }

    Flickable {
        id: flick
        anchors.fill: parent
        anchors.margins: 2
        contentHeight: column.implicitHeight
        interactive: contentHeight > height
        boundsBehavior: Flickable.StopAtBounds
        clip: true

        component RowLabel: Text {
            width: root.labelWidth
            height: root.rowHeight
            verticalAlignment: Text.AlignVCenter
            color: CoreBridge.foreground
            font.pointSize: CoreBridge.basePt
        }

        Column {
            id: column
            width: flick.width
            spacing: root.gap

            readonly property real fieldWidth:
                width - root.labelWidth - root.gap

            Row {
                spacing: root.gap
                RowLabel { text: "Name" }
                Field {
                    id: name
                    width: column.fieldWidth; height: root.rowHeight
                    placeholder: "a short name, e.g. work"
                    tip: "A short name for this account. Renaming it also "
                         + "renames it in any header rule scoped to this "
                         + "account. Past calls in the history keep the "
                         + "name they were made under."
                }
            }
            Row {
                spacing: root.gap
                RowLabel { text: "User" }
                Field {
                    id: user
                    width: column.fieldWidth; height: root.rowHeight
                    placeholder: "extension or username"
                }
            }
            Row {
                spacing: root.gap
                RowLabel { text: "Domain" }
                Field {
                    id: domain
                    width: column.fieldWidth; height: root.rowHeight
                    placeholder: "pbx.example.lan"
                }
            }
            Row {
                spacing: root.gap
                RowLabel { text: "Auth user" }
                Field {
                    id: authUser
                    width: column.fieldWidth; height: root.rowHeight
                    placeholder: "defaults to User"
                }
            }
            Row {
                spacing: root.gap
                RowLabel { text: "Password" }
                Field {
                    id: password
                    width: column.fieldWidth; height: root.rowHeight
                    secret: true
                }
            }
            Row {
                spacing: root.gap
                RowLabel { text: "Transport" }
                ComboBox {
                    id: transport
                    width: column.fieldWidth; height: root.rowHeight
                    model: ["UDP", "TCP", "TLS"]
                    currentIndex: 0
                }
            }
            Row {
                spacing: root.gap
                RowLabel { text: "SRTP" }
                ComboBox {
                    id: srtp
                    width: column.fieldWidth; height: root.rowHeight
                    model: ["Disabled", "Optional", "Mandatory"]
                    currentIndex: 0
                }
            }
            Row {
                spacing: root.gap
                RowLabel { text: "Registrar" }
                Field {
                    id: registrar
                    width: column.fieldWidth; height: root.rowHeight
                    placeholder: "defaults to sip:<domain>"
                }
            }
            Row {
                spacing: root.gap
                RowLabel { text: "Outbound proxy" }
                Field {
                    id: proxy
                    width: column.fieldWidth; height: root.rowHeight
                    placeholder: "optional"
                }
            }
            Row {
                spacing: root.gap
                RowLabel { text: "STUN server" }
                Field {
                    id: stun
                    width: column.fieldWidth; height: root.rowHeight
                    placeholder: "optional"
                }
            }
            Row {
                spacing: root.gap
                RowLabel { text: "Registration expiry" }
                SpinBox {
                    id: expires
                    width: column.fieldWidth; height: root.rowHeight
                    from: 30; to: 86400
                    value: 300
                    suffix: " s"
                }
            }
            Row {
                spacing: root.gap
                Item { width: root.labelWidth; height: 1 }
                CheckBox {
                    id: doRegister
                    width: column.fieldWidth
                    wrap: true
                    text: "Register with the registrar"
                    checked: true
                    onToggled: function(on) { checked = on }
                }
            }

            Text {
                width: parent.width
                visible: root.problem !== ""
                text: root.problem
                color: "#b3261e"
                font.pointSize: CoreBridge.basePt
                wrapMode: Text.WordWrap
            }

            Item {
                width: parent.width
                height: 30

                Row {
                    anchors.right: parent.right
                    spacing: 6
                    PushButton {
                        width: 80; height: 30
                        text: "Save"
                        onClicked: root.save()
                    }
                    PushButton {
                        width: 80; height: 30
                        text: "Cancel"
                        onClicked: if (root.host) root.host.close()
                    }
                }
            }
        }
    }
}
