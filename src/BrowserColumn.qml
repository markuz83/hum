import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// One column of the library browser: a heading over a list of rows.
Item {
    id: column

    property string heading
    property var model: []
    property bool active: false
    // The row that is playing, drawn in the accent color; -1 for none.
    property int playingIndex: -1
    // Turns a model value into the text shown, so the caller can translate it.
    property var label: function (value, index) { return value }
    property alias currentIndex: list.currentIndex
    property alias count: list.count
    property alias list: list

    signal picked(int index)
    signal clicked()

    ColumnLayout {
        anchors.fill: parent
        spacing: 4

        Label {
            text: column.heading
            font.bold: true
            font.pixelSize: 12
            font.capitalization: Font.AllUppercase
            font.letterSpacing: 1
            color: column.active ? theme.accent : theme.dim
            Layout.leftMargin: 10
        }

        ListView {
            id: list
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            model: column.model
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: ScrollBar {}

            delegate: Item {
                required property int index
                required property var modelData
                width: ListView.view.width
                height: 30

                readonly property bool playingRow: index === column.playingIndex

                Rectangle {
                    anchors.fill: parent
                    radius: Math.min(4, theme.radius)
                    color: parent.ListView.isCurrentItem
                           ? (column.active ? theme.highlight : theme.raised)
                           : rowHover.hovered ? theme.raised : "transparent"
                }
                Label {
                    anchors.fill: parent
                    anchors.leftMargin: 10
                    anchors.rightMargin: 10
                    verticalAlignment: Text.AlignVCenter
                    text: column.label(modelData, index)
                    elide: Text.ElideRight
                    color: playingRow ? theme.accent : modelData === null ? theme.dim : theme.foreground
                    font.bold: playingRow
                }
                HoverHandler { id: rowHover }
                TapHandler {
                    onTapped: {
                        column.clicked()
                        column.picked(index)
                    }
                }
            }
        }
    }
}
