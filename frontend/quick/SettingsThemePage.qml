import QtQuick
import QtQuick.Dialogs
import comlink

/* The theme picker -- themepage.cpp, part for part. It sends choices
 * to the core and reapplies whatever the core resolves; it never
 * resolves anything itself, which is what makes the preview honest:
 * it shows exactly what Apply would produce. */
SettingsPage {
    id: root

    property var preview: ({})
    property string errorText: ""
    /* Blank on every open, even if an override was saved before: the
       ABI resolves a theme, it does not hand back which parts of it
       were overridden, so there is nothing truthful to prefill these
       two fields with -- themepage.cpp leaves them blank too. */
    property string accent: ""
    property string font: ""

    function refreshPreview() {
        root.preview = CoreBridge.previewTheme(presetCombo.currentIndex,
                                               root.accent, root.font,
                                               scaleCombo.currentIndex);
        root.errorText = Object.keys(root.preview).length === 0
                         ? "Accent must be a colour like #378ADD." : "";
    }

    function load() {
        const t = CoreBridge.theme();
        presetCombo.model = CoreBridge.themePresets().map(function(p) {
            return { label: p.label, value: p.index };
        });
        presetCombo.currentIndex = t.preset === undefined ? 0 : t.preset;
        scaleCombo.currentIndex = t.scale === undefined ? 1 : t.scale;
        root.refreshPreview();
    }
    Component.onCompleted: root.load()

    function apply() {
        const why = CoreBridge.applyTheme(presetCombo.currentIndex,
                                          root.accent, root.font,
                                          scaleCombo.currentIndex);
        if (why !== "") {
            root.errorText = "Could not save: " + why;
            return;
        }
        root.errorText = "";
    }

    ColorDialog {
        id: colorPicker
        title: "Accent colour"
        selectedColor: root.accent !== "" ? root.accent : "#378ADD"
        /* Channel by channel rather than selectedColor.toString():
           that comes back "#AARRGGBB", and the core's accent parser
           wants a plain #RRGGBB. */
        function hex2(v) {
            return Math.round(v * 255).toString(16).padStart(2, "0");
        }
        onAccepted: {
            root.accent = ("#" + hex2(selectedColor.r) + hex2(selectedColor.g)
                          + hex2(selectedColor.b)).toUpperCase();
            accentField.text = root.accent;
            root.refreshPreview();
        }
    }

    GroupBox {
        width: parent.width
        text: "Theme"
        description: "A preset sets every colour and the font together. " +
                     "Nothing changes until Apply."

        FormRow {
            text: "Preset"
            ComboBox {
                id: presetCombo
                width: parent.width
                height: 28
                onActivated: root.refreshPreview()
            }
        }
        FormRow {
            text: "Text size"
            ComboBox {
                id: scaleCombo
                width: parent.width
                height: 28
                model: ["Small", "Medium", "Large", "Extra large"]
                onActivated: root.refreshPreview()
            }
        }
    }

    GroupBox {
        width: parent.width
        text: "Overrides"
        description: "Blank uses the preset's own."

        FormRow {
            id: accentRow
            text: "Accent"
            Field {
                id: accentField
                width: parent.width
                height: 30
                placeholder: "blank uses the preset's accent"
                onTextChanged: { root.accent = text; root.refreshPreview() }
            }
        }
        /* Under the accent field, and as wide as it, wherever the
           form has put it. */
        Row {
            x: accentRow.stacked ? 0 : accentRow.labelWidth + 6
            width: parent.width - x
            spacing: 4
            PushButton {
                width: (parent.width - 4) / 2; height: 28
                text: "Pick…"
                onClicked: colorPicker.open()
            }
            PushButton {
                width: (parent.width - 4) / 2; height: 28
                text: "Use preset"
                onClicked: { accentField.text = ""; root.refreshPreview() }
            }
        }
        FormRow {
            text: "Font"
            Field {
                id: fontField
                width: parent.width
                height: 30
                placeholder: "blank uses the preset's font"
                onTextChanged: { root.font = text; root.refreshPreview() }
            }
        }
    }

    GroupBox {
        width: parent.width
        text: "Preview"

        Rectangle {
            width: parent.width
            height: 56
            radius: 2
            color: root.preview.surface === undefined ? CoreBridge.surface
                                                       : root.preview.surface
            border.width: 2
            border.color: root.preview.accent === undefined
                          ? CoreBridge.accent : root.preview.accent

            Text {
                anchors.centerIn: parent
                width: parent.width - 12
                horizontalAlignment: Text.AlignHCenter
                elide: Text.ElideRight
                text: "Comlink — " + (root.preview.font === undefined
                                           ? "" : root.preview.font)
                color: root.preview.foreground === undefined
                       ? CoreBridge.foreground : root.preview.foreground
                font.pointSize: root.preview.multiplier === undefined
                                ? CoreBridge.basePt
                                : 10.0 * root.preview.multiplier
            }
        }
    }

    footer: [
        Text {
            width: parent.width
            visible: root.errorText !== ""
            text: root.errorText
            color: "#b3261e"
            font.pointSize: CoreBridge.basePt
            wrapMode: Text.WordWrap
        },
        PushButton {
            width: parent.width
            height: 30
            text: "Apply theme"
            onClicked: root.apply()
        }
    ]
}
