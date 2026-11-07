import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// Every keyboard shortcut, one keypress away.
Rectangle {
    id: overlay
    signal dismissed()

    color: "#cc000000"

    readonly property var shortcuts: [
        ["Space", qsTr("Play / pause")],
        ["N / P", qsTr("Next track") + " · " + qsTr("Previous track / restart")],
        ["← →", qsTr("Seek 5 seconds")],
        ["+ −", qsTr("Volume up / down")],
        ["M", qsTr("Mute")],
        ["S", qsTr("Shuffle")],
        ["R", qsTr("Repeat")],
        ["V", qsTr("Change visualizer")],
        ["Y", qsTr("Lyrics")],
        ["Shift+S", qsTr("Sort the title list")],
        ["F", qsTr("Visualizer refresh rate")],
        ["G", qsTr("Volume leveling (ReplayGain)")],
        ["C", qsTr("Crossfade")],
        ["T", qsTr("Sleep timer")],
        ["J K ↑ ↓", qsTr("Move in the list")],
        ["H L Tab", qsTr("Switch column")],
        ["Enter", qsTr("Play the selected track")],
        ["/", qsTr("Filter the list")],
        ["O", qsTr("Choose music folder")],
        ["A / Shift+A", qsTr("Add to queue") + " · " + qsTr("Play next")],
        ["X Del", qsTr("Remove from queue")],
        ["Shift+X", qsTr("Clear the queue")],
        ["Shift+J K", qsTr("Move in the queue")],
        ["Ctrl+Z", qsTr("Undo / redo")],
        ["[ ]", qsTr("Resize the queue pane")],
        ["\\", qsTr("Reset pane sizes")],
        ["?", qsTr("Show or hide this help")],
        ["Q", qsTr("Quit")]
    ]

    MouseArea { anchors.fill: parent; onClicked: overlay.dismissed() }

    Rectangle {
        id: card
        anchors.centerIn: parent
        width: Math.min(parent.width - 48, 860)
        height: Math.min(parent.height - 48, grid.implicitHeight + title.implicitHeight + 72)
        color: theme.raised
        radius: theme.radius
        border.color: theme.highlight

        // A taller list than the window scrolls instead of being cut off.
        Flickable {
            anchors.fill: parent
            anchors.margins: 24
            contentHeight: content.implicitHeight
            clip: true
            boundsBehavior: Flickable.StopAtBounds

            ColumnLayout {
                id: content
                width: parent.width
                spacing: 14

                Label {
                    id: title
                    text: qsTr("Keyboard shortcuts")
                    font.pixelSize: 18
                    font.bold: true
                }
                GridLayout {
                    id: grid
                    Layout.fillWidth: true
                    columns: card.width >= 700 ? 2 : 1
                    columnSpacing: 32
                    rowSpacing: 8
                    flow: GridLayout.TopToBottom
                    rows: Math.ceil(overlay.shortcuts.length / columns)

                    Repeater {
                        model: overlay.shortcuts
                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 16
                            Label {
                                text: modelData[0]
                                color: theme.accent
                                Layout.preferredWidth: 110
                            }
                            Label {
                                text: modelData[1]
                                Layout.fillWidth: true
                                wrapMode: Text.WordWrap
                            }
                        }
                    }
                }
            }
        }
    }
}
