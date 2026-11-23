import QtQuick

// The splitter between the browser and the queue. Drag to resize; double-click
// to reset. [ and ] do the same from the keyboard.
Item {
    id: splitter

    // The item whose width the queue's share is measured against.
    required property Item container

    width: 12
    z: 5

    Rectangle {
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.verticalCenter: parent.verticalCenter
        width: area.pressed || area.containsMouse ? 3 : 1
        height: parent.height - 24
        radius: width / 2
        color: area.pressed || area.containsMouse ? theme.accent : theme.highlight
    }
    MouseArea {
        id: area
        objectName: "splitter"
        anchors.fill: parent
        hoverEnabled: true
        preventStealing: true
        cursorShape: Qt.SplitHCursor
        onPositionChanged: mouse => {
            if (!pressed) return
            const x = mapToItem(splitter.container, mouse.x, 0).x
            // Right-to-left puts the queue on the left, so the share flips.
            backend.queueRatio = LayoutMirroring.enabled ? x / splitter.container.width : 1 - x / splitter.container.width
        }
        onDoubleClicked: backend.queueRatio = 1 / 3
    }
}
