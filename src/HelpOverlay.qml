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
        ["J K ↑ ↓", qsTr("Move in the list")],
        ["Enter", qsTr("Play the selected track")],
        ["/", qsTr("Filter the list")],
        ["S", qsTr("Shuffle")],
        ["R", qsTr("Repeat")],
        ["V", qsTr("Change visualizer")],
        ["?", qsTr("Show or hide this help")],
        ["Q", qsTr("Quit")]
    ]

    MouseArea { anchors.fill: parent; onClicked: overlay.dismissed() }

    Rectangle {
        anchors.centerIn: parent
        width: Math.min(parent.width - 48, 520)
        height: column.implicitHeight + 48
        color: theme.raised
        radius: theme.radius
        border.color: theme.selection

        ColumnLayout {
            id: column
            anchors.fill: parent
            anchors.margins: 24
            spacing: 10

            Label {
                text: qsTr("Keyboard shortcuts")
                font.pixelSize: 18
                font.bold: true
            }
            Repeater {
                model: overlay.shortcuts
                RowLayout {
                    Layout.fillWidth: true
                    spacing: 16
                    Label {
                        text: modelData[0]
                        color: theme.accent
                        font.family: "monospace"
                        Layout.preferredWidth: 90
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
