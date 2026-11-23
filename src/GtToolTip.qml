import QtQuick
import QtQuick.Controls

// A tooltip in the terminal look: monospace text in a one-pixel border, no
// shadow. Names that come from tags are drawn as plain text, never markup.
ToolTip {
    id: tip
    delay: 600
    font.family: theme.fontFamily
    font.pixelSize: 12
    padding: 6

    contentItem: Text {
        textFormat: Text.PlainText
        text: tip.text
        font: tip.font
        color: theme.foreground
    }
    background: Rectangle {
        color: theme.raised
        border.width: 1
        border.color: theme.dim
        radius: Math.min(4, theme.radius)
    }
}
