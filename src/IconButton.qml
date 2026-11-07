import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material

// A round or square icon-only button, drawn with a Nerd Font glyph like the
// rest of Omarchy's controls. A toggle shows in the accent color when on.
Item {
    id: button

    property string glyph
    property string tip
    property bool checked: false
    // The main action (play/pause) is a filled accent circle.
    property bool emphasized: false
    property real size: 36

    signal clicked()

    implicitWidth: size
    implicitHeight: size

    Rectangle {
        anchors.fill: parent
        radius: button.emphasized ? width / 2 : Math.min(8, theme.radius)
        color: button.emphasized ? (tap.pressed ? Qt.darker(theme.accent, 1.2) : theme.accent)
             : tap.pressed ? theme.highlight
             : hover.hovered ? theme.raised : "transparent"
    }
    Label {
        textFormat: Text.PlainText
        anchors.centerIn: parent
        text: button.glyph
        font.pixelSize: button.size * (button.emphasized ? 0.5 : 0.56)
        color: button.emphasized ? theme.accentForeground : button.checked ? theme.accent : theme.foreground
    }
    HoverHandler { id: hover; cursorShape: Qt.PointingHandCursor }
    TapHandler { id: tap; onTapped: button.clicked() }

    ToolTip.visible: hover.hovered && button.tip.length > 0
    ToolTip.delay: 600
    ToolTip.text: button.tip
}
