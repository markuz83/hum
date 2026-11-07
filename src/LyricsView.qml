import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material

// The lyrics of the playing track. Synced lyrics follow the music with the
// current line highlighted, and a click on a line jumps to it; plain lyrics
// scroll freely.
Item {
    id: view

    readonly property bool synced: backend.lyricsSynced
    readonly property int line: synced ? backend.lyricIndexAt(backend.position) : -1

    ListView {
        id: list
        anchors.fill: parent
        clip: true
        model: backend.lyricLines
        boundsBehavior: Flickable.StopAtBounds
        ScrollBar.vertical: ScrollBar {}
        // Synced lyrics keep the current line in the middle.
        currentIndex: view.line
        highlightRangeMode: view.synced ? ListView.StrictlyEnforceRange : ListView.NoHighlightRange
        preferredHighlightBegin: height / 2 - 20
        preferredHighlightEnd: height / 2 + 20
        highlightMoveDuration: 350

        delegate: Item {
            required property int index
            required property string modelData
            width: ListView.view.width
            height: Math.max(28, text.implicitHeight + 8)
            Label {
                textFormat: Text.PlainText
                id: text
                anchors.centerIn: parent
                width: parent.width - 24
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.WordWrap
                text: modelData
                font.pixelSize: index === view.line ? 20 : 16
                font.bold: index === view.line
                color: index === view.line ? theme.accent : theme.foreground
                opacity: !view.synced || index === view.line ? 1 : 0.6
                Behavior on opacity { NumberAnimation { duration: 150 } }
            }
            TapHandler {
                enabled: view.synced
                onTapped: backend.seek(backend.lyricTimes[index])
            }
        }
    }

    Label {
        textFormat: Text.PlainText
        anchors.centerIn: parent
        visible: list.count === 0
        color: theme.dim
        text: backend.playbackState === "Stopped" && backend.title === "" ? "" : qsTr("No lyrics for this track")
    }
}
