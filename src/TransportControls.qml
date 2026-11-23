import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Shuffle and repeat, previous, play and next, and the volume. Media controls
// keep their order in every language, like their glyphs, so they are not mirrored.
RowLayout {
    id: transport

    // The window, for its labels.
    required property var win

    spacing: 4
    LayoutMirroring.enabled: false
    LayoutMirroring.childrenInherit: true

    IconButton {
        glyph: "󰒟"
        checked: backend.shuffle
        tip: qsTr("Shuffle")
        onClicked: backend.toggleShuffle()
    }
    IconButton {
        glyph: backend.repeat === 2 ? "󰑘" : "󰑖"
        checked: backend.repeat > 0
        tip: transport.win.repeatLabel()
        onClicked: backend.cycleRepeat()
    }
    Item { Layout.fillWidth: true }
    IconButton {
        glyph: "󰒮"
        size: 40
        tip: qsTr("Previous track / restart")
        onClicked: backend.previous()
    }
    IconButton {
        glyph: backend.playing ? "󰏤" : "󰐊"
        emphasized: true
        size: 46
        tip: backend.playing ? qsTr("Pause") : qsTr("Play")
        onClicked: backend.toggle()
    }
    IconButton {
        glyph: "󰒭"
        size: 40
        tip: qsTr("Next track")
        onClicked: backend.next()
    }
    Item { Layout.fillWidth: true }
    IconButton {
        glyph: transport.win.volumeGlyph()
        tip: backend.muted ? qsTr("Unmute") : qsTr("Mute")
        onClicked: backend.toggleMute()
    }
    BarSlider {
        Layout.preferredWidth: 110
        Layout.preferredHeight: 28
        fraction: backend.muted ? 0 : backend.volume
        onMoved: f => backend.volume = f
        onWheeled: delta => backend.changeVolume(delta > 0 ? 0.05 : -0.05)

        GtToolTip {
            visible: volumeHover.hovered
            text: backend.muted ? qsTr("Muted") : qsTr("Volume %1%").arg(transport.win.percent(backend.volume))
        }
        HoverHandler { id: volumeHover; cursorShape: Qt.PointingHandCursor }
    }
}
