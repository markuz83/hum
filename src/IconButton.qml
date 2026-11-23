import QtQuick
import QtQuick.Controls

// An icon-only button drawn with a Nerd Font glyph, in the terminal look: a
// square with a one-pixel border that appears under the pointer, never a
// shadow or a ripple. A toggle shows its glyph in the accent color when on;
// the main action (play/pause) is the one filled accent button.
Item {
    id: button

    property string glyph
    property string tip
    property bool checked: false
    property bool emphasized: false
    property real size: 36

    signal clicked()

    implicitWidth: size
    implicitHeight: size

    Rectangle {
        anchors.fill: parent
        radius: Math.min(4, theme.radius)
        border.width: 1
        border.color: button.emphasized ? (tap.pressed ? theme.accent : "transparent")
                    : hover.hovered || tap.pressed ? theme.dim : "transparent"
        // Hover steps the surface up at once; a press on the accent button
        // inverts it to a tinted fill for a moment.
        color: button.emphasized ? (tap.pressed ? theme.highlight : hover.hovered ? Qt.lighter(theme.accent, 1.15) : theme.accent)
             : tap.pressed ? theme.highlight
             : hover.hovered ? theme.raised : "transparent"
    }
    Label {
        textFormat: Text.PlainText
        anchors.centerIn: parent
        text: button.glyph
        font.pixelSize: button.size * (button.emphasized ? 0.5 : 0.56)
        color: button.emphasized ? (tap.pressed ? theme.accent : theme.accentForeground) : button.checked ? theme.accent : theme.foreground
    }
    HoverHandler { id: hover; cursorShape: Qt.PointingHandCursor }
    TapHandler { id: tap; onTapped: button.clicked() }

    GtToolTip {
        visible: hover.hovered && button.tip.length > 0
        text: button.tip
    }
}
