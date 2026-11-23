import QtQuick

// A thin track with a filled share, for the seek bar and the volume: click or
// drag anywhere along it, or scroll over it.
Item {
    id: slider

    // How much of the track is filled, 0 to 1.
    property real fraction: 0
    // Emitted with the position, 0 to 1, that the user pressed or dragged to.
    signal moved(real fraction)
    signal wheeled(real delta)

    Rectangle {
        anchors.verticalCenter: parent.verticalCenter
        width: parent.width
        height: 4
        radius: Math.min(2, theme.radius)
        color: theme.highlight
        Rectangle {
            anchors.left: parent.left
            height: parent.height
            radius: parent.radius
            width: parent.width * Math.max(0, Math.min(1, slider.fraction))
            color: theme.accent
        }
    }
    MouseArea {
        anchors.fill: parent
        preventStealing: true
        function report() {
            const x = LayoutMirroring.enabled ? width - mouseX : mouseX
            slider.moved(Math.max(0, Math.min(1, x / width)))
        }
        onPressed: report()
        onPositionChanged: if (pressed) report()
        onWheel: wheel => slider.wheeled(wheel.angleDelta.y)
    }
}
