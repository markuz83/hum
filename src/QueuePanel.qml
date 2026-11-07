import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// The play queue: what plays next, ahead of the library. Rows can be dragged
// to reorder; the cursor and the keyboard shortcuts live in Main.qml.
Item {
    id: panel

    property bool active: false
    property alias list: list
    property alias currentIndex: list.currentIndex
    readonly property int count: list.count

    signal clicked()

    ColumnLayout {
        anchors.fill: parent
        spacing: 4

        RowLayout {
            Layout.fillWidth: true
            Layout.leftMargin: 10
            Layout.rightMargin: 4
            spacing: 8
            Label {
                textFormat: Text.PlainText
                text: qsTr("Queue")
                font.bold: true
                font.pixelSize: 12
                font.capitalization: Font.AllUppercase
                font.letterSpacing: 1
                color: panel.active ? theme.accent : theme.dim
            }
            Label {
                textFormat: Text.PlainText
                visible: panel.count > 0
                text: qsTr("%n tracks", "", panel.count)
                font.pixelSize: 12
                color: theme.dim
            }
            Item { Layout.fillWidth: true }
            Label {
                textFormat: Text.PlainText
                visible: panel.count > 0
                text: qsTr("Clear")
                font.pixelSize: 12
                color: clearHover.hovered ? theme.accent : theme.dim
                HoverHandler { id: clearHover; cursorShape: Qt.PointingHandCursor }
                TapHandler { onTapped: backend.clearQueue() }
            }
        }

        Item {
            Layout.fillWidth: true
            Layout.fillHeight: true

            ListView {
                id: list
                objectName: "queueList"
                anchors.fill: parent
                clip: true
                model: backend.queue
                currentIndex: 0
                boundsBehavior: Flickable.StopAtBounds
                ScrollBar.vertical: ScrollBar {}

                // While a row is dragged, where it would land.
                property int draggingIndex: -1
                property int dropIndex: -1

                // Keep the cursor on a real row as entries come and go.
                onCountChanged: currentIndex = Math.max(0, Math.min(currentIndex, count - 1))

                delegate: Item {
                    id: slot
                    required property int index
                    required property var modelData
                    width: ListView.view.width
                    height: 46

                    Rectangle {
                        id: row
                        width: slot.width
                        height: slot.height
                        z: drag.active ? 10 : 0
                        radius: Math.min(4, theme.radius)
                        color: drag.active ? theme.highlight
                             : slot.ListView.isCurrentItem ? (panel.active ? theme.highlight : theme.raised)
                             : rowHover.hovered ? theme.raised : "transparent"
                        opacity: drag.active ? 0.9 : 1

                        onYChanged: if (drag.active)
                            list.dropIndex = Math.max(0, Math.min(list.count - 1, Math.round((slot.y + row.y) / slot.height)))

                        Column {
                            anchors.left: parent.left
                            anchors.right: removeButton.left
                            anchors.leftMargin: 10
                            anchors.verticalCenter: parent.verticalCenter
                            spacing: 2
                            Label {
                                textFormat: Text.PlainText
                                width: parent.width
                                text: slot.modelData.title
                                elide: Text.ElideRight
                                color: theme.foreground
                            }
                            Label {
                                textFormat: Text.PlainText
                                width: parent.width
                                visible: text.length > 0
                                text: slot.modelData.artist
                                elide: Text.ElideRight
                                font.pixelSize: 11
                                color: theme.dim
                            }
                        }
                        IconButton {
                            id: removeButton
                            anchors.right: parent.right
                            anchors.rightMargin: 4
                            anchors.verticalCenter: parent.verticalCenter
                            visible: rowHover.hovered || slot.ListView.isCurrentItem
                            glyph: "󰅖"
                            size: 26
                            tip: qsTr("Remove from queue")
                            onClicked: backend.removeFromQueue(slot.index)
                        }

                        HoverHandler { id: rowHover }
                        TapHandler {
                            onTapped: {
                                list.currentIndex = slot.index
                                panel.clicked()
                                backend.playFromQueue(slot.index)
                            }
                        }
                        // Drag a row up or down; it lands where it is let go.
                        DragHandler {
                            id: drag
                            target: row
                            xAxis.enabled: false
                            onActiveChanged: {
                                if (active) {
                                    list.draggingIndex = slot.index
                                    list.dropIndex = slot.index
                                    panel.clicked()
                                } else {
                                    const to = list.dropIndex
                                    list.draggingIndex = -1
                                    list.dropIndex = -1
                                    row.y = 0
                                    if (to >= 0 && to !== slot.index) {
                                        list.currentIndex = to
                                        backend.moveInQueue(slot.index, to)
                                    }
                                }
                            }
                        }
                    }
                }

                // A line shows where the dragged row will drop.
                Rectangle {
                    visible: list.draggingIndex >= 0
                    width: list.width
                    height: 2
                    color: theme.accent
                    y: list.dropIndex * 46 + (list.dropIndex > list.draggingIndex ? 46 : 0) - 1 - list.contentY
                    z: 20
                }
            }

            ColumnLayout {
                anchors.centerIn: parent
                width: parent.width - 24
                visible: panel.count === 0
                spacing: 6
                Label {
                    textFormat: Text.PlainText
                    Layout.fillWidth: true
                    horizontalAlignment: Text.AlignHCenter
                    text: qsTr("Nothing queued")
                    font.pixelSize: 15
                }
                Label {
                    textFormat: Text.PlainText
                    Layout.fillWidth: true
                    horizontalAlignment: Text.AlignHCenter
                    wrapMode: Text.WordWrap
                    color: theme.dim
                    font.pixelSize: 12
                    text: qsTr("Press %1 on a track to add it, or %2 to play it next.").arg("A").arg("Shift+A")
                }
            }
        }
    }
}
