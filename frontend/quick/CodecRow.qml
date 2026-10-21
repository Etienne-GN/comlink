import QtQuick
import comlink

/* One codec: whether it is offered, where in the offer it sits, and
   the settings that are its own.
 *
 * -1 means "the codec's own default" everywhere in the ABI, and an
 * empty field is how that is said here: writing a default back as a
 * number would turn it into a decision the operator never made.
 *
 * A control the codec ignores is shown disabled rather than hidden,
 * so the operator can see the setting exists and that this codec is
 * not the place for it. The supports_ flags come from the media
 * stack, not from the codec's name. */
Panel {
    id: root
    property var codec: ({})
    property bool expanded: false
    property bool first: false
    property bool last: false
    signal changed()
    signal problem(string why)
    signal toggleExpand()

    implicitHeight: body.implicitHeight + 10

    function apply(key, value) {
        const why = CoreBridge.setCodecParam(root.codec.id, key, value);
        if (why !== "") root.problem(why); else root.problem("");
        root.changed();
    }

    Column {
        id: body
        x: 6
        y: 5
        width: parent.width - 12
        spacing: 4

        Row {
            width: parent.width
            spacing: 4

            Btn {
                width: 26; height: 26; text: "▴"
                enabled: !root.first
                onClicked: {
                    root.problem(CoreBridge.moveCodec(root.codec.id, -1));
                    root.changed();
                }
            }
            Btn {
                width: 26; height: 26; text: "▾"
                enabled: !root.last
                onClicked: {
                    root.problem(CoreBridge.moveCodec(root.codec.id, 1));
                    root.changed();
                }
            }

            Toggle {
                width: parent.width - 26 * 2 - 8
                anchors.verticalCenter: parent.verticalCenter
                text: root.codec.id + "   "
                      + (root.codec.clockRate / 1000) + " kHz"
                      + (root.codec.channels > 1 ? " stereo" : "")
                checked: root.codec.enabled === true
                onToggled: function(on) {
                    root.problem(CoreBridge.setCodecEnabled(root.codec.id,
                                                            on));
                    root.changed();
                }
            }
        }

        Text {
            width: parent.width
            text: (root.expanded ? "▾  " : "▸  ")
                  + root.codec.clockRate + " Hz, "
                  + root.codec.channels
                  + (root.codec.channels === 1 ? " channel" : " channels")
                  + (root.codec.supportsPtime && root.codec.framePtime > 0
                     ? ", " + root.codec.framePtime + " ms frame" : "")
            color: CoreBridge.muted
            font.pixelSize: 10
            MouseArea {
                anchors.fill: parent
                onClicked: root.toggleExpand()
            }
        }

        Column {
            width: parent.width
            spacing: 3
            visible: root.expanded

            Repeater {
                model: [
                    { key: "bitrate", label: "Bit rate (bps)",
                      supported: root.codec.supportsBitrate,
                      value: root.codec.bitrate },
                    { key: "complexity", label: "Complexity 0-10",
                      supported: root.codec.supportsComplexity,
                      value: root.codec.complexity },
                    { key: "loss", label: "Expected loss %",
                      supported: root.codec.supportsLoss,
                      value: root.codec.loss },
                    { key: "ptime", label: "Packet time (ms)",
                      supported: root.codec.supportsPtime,
                      value: root.codec.ptime }
                ]
                Row {
                    width: body.width
                    spacing: 4
                    Text {
                        width: parent.width - 96 - 4
                        anchors.verticalCenter: parent.verticalCenter
                        text: modelData.label
                        color: modelData.supported ? CoreBridge.foreground
                                                   : CoreBridge.muted
                        font.pixelSize: 11
                        elide: Text.ElideRight
                    }
                    Field {
                        width: 96
                        enabled: modelData.supported
                        placeholder: "default"
                        text: modelData.value >= 0
                              ? String(modelData.value) : ""
                        onEditingFinished: {
                            const t = text.trim();
                            root.apply(modelData.key,
                                       t === "" ? -1 : parseInt(t));
                        }
                    }
                }
            }

            /* Three states, because the ABI has three: off, on, and
               "leave it to the codec". */
            Row {
                width: body.width
                spacing: 4
                Text {
                    width: parent.width - 96 - 4
                    anchors.verticalCenter: parent.verticalCenter
                    text: "Constant bit rate"
                    color: root.codec.supportsCbr ? CoreBridge.foreground
                                                  : CoreBridge.muted
                    font.pixelSize: 11
                    elide: Text.ElideRight
                }
                Row {
                    width: 96
                    spacing: 2
                    Repeater {
                        model: [{ l: "def", v: -1 }, { l: "off", v: 0 },
                                { l: "on", v: 1 }]
                        Btn {
                            width: (96 - 4) / 3
                            height: 26
                            text: modelData.l
                            enabled: root.codec.supportsCbr
                            ink: root.codec.cbr === modelData.v
                                 ? CoreBridge.accent : CoreBridge.foreground
                            onClicked: root.apply("cbr", modelData.v)
                        }
                    }
                }
            }
        }
    }
}
