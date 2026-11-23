import QtQuick
import QtQuick.Controls

// A thin bar with no arrows, only there while the list is long enough to scroll.
ScrollBar {
    id: bar
    implicitWidth: 8
    policy: size < 1 ? ScrollBar.AlwaysOn : ScrollBar.AlwaysOff
    contentItem: Rectangle {
        implicitWidth: 6
        radius: Math.min(3, theme.radius)
        color: bar.pressed || bar.hovered ? theme.dim : theme.highlight
    }
    background: null
}
