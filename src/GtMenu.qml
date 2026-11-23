import QtQuick
import QtQuick.Controls

// A context menu: a bordered list on the raised surface, the row under the
// pointer marked with an accent bar in its gutter like a cursor row.
Menu {
    font.family: theme.fontFamily
    padding: 1
    implicitWidth: 180

    delegate: MenuItem {
        id: item
        implicitHeight: 28
        font.family: theme.fontFamily
        contentItem: Text {
            textFormat: Text.PlainText
            leftPadding: 8
            text: item.text
            font: item.font
            color: item.enabled ? theme.foreground : theme.dim
            verticalAlignment: Text.AlignVCenter
        }
        background: Rectangle {
            color: item.highlighted ? theme.highlight : "transparent"
            Rectangle {
                visible: item.highlighted
                width: 2
                height: parent.height
                color: theme.accent
            }
        }
    }
    background: Rectangle {
        implicitWidth: 180
        color: theme.raised
        border.width: 1
        border.color: theme.dim
        radius: Math.min(4, theme.radius)
    }
}
