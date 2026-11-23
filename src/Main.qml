import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ApplicationWindow {
    id: win
    width: 1100
    height: 760
    minimumWidth: 720
    minimumHeight: 540
    visible: true
    title: backend.title.length > 0 ? backend.title + " — hum" : "hum"
    // Follows Omarchy's Window Opacity: solid, slightly see-through, or glassy
    // (which Hyprland blurs behind). Only the background fades, never the text.
    // Wayland expects premultiplied alpha, and Qt clears the window with this
    // color as given, so without premultiplying a see-through window comes out
    // too bright.
    color: Qt.rgba(ground.r * theme.windowAlpha, ground.g * theme.windowAlpha,
                   ground.b * theme.windowAlpha, theme.windowAlpha)

    // Omarchy's monospace font, which follows `omarchy font set`.
    font.family: theme.fontFamily

    // Everything here comes from the current Omarchy theme.
    readonly property color ground: theme.background
    readonly property color dim: theme.dim
    property bool helpVisible: false
    // Which browser column the keyboard drives: 0 artists, 1 albums, 2 titles.
    property int activeColumn: 2
    // The empty library (and the scan before it) replaces the three columns.
    readonly property bool emptyLibrary: backend.libraryCount === 0 && !backend.scanning
    // Errors fade after a while and can be dismissed; a new one shows again.
    // A short message after a key changes a setting that has no control of its own.
    property bool noticeVisible: false
    property string noticeText: ""
    function notify(text) {
        noticeText = text
        noticeVisible = true
        noticeTimer.restart()
    }
    function sleepLabel(minutes) {
        return minutes === 0 ? qsTr("off") : minutes < 0 ? qsTr("end of track") : qsTr("%1 min").arg(minutes)
    }
    Timer { id: noticeTimer; interval: 2000; onTriggered: win.noticeVisible = false }
    property bool errorShown: backend.error.length > 0
    Connections {
        target: backend
        function onErrorChanged() { win.errorShown = backend.error.length > 0 }
    }
    Timer {
        interval: 12000
        running: win.errorShown && !win.emptyLibrary
        onTriggered: win.errorShown = false
    }
    // Typing in the filter must not trigger the single-key shortcuts.
    readonly property bool typing: filter.typing

    function pad(n) {
        return (n < 10 ? (0).toLocaleString(Qt.locale(), 'f', 0) : "") + n.toLocaleString(Qt.locale(), 'f', 0)
    }
    function clock(ms) {
        translator.language  // re-render the digits when the language switches
        const total = Math.max(0, Math.floor(ms / 1000))
        return Math.floor(total / 60).toLocaleString(Qt.locale(), 'f', 0) + ":" + pad(total % 60)
    }
    function percent(value) {
        translator.language
        return Math.round(value * 100).toLocaleString(Qt.locale(), 'f', 0)
    }
    function volumeGlyph() {
        return backend.muted || backend.volume === 0 ? "󰖁" : backend.volume < 0.34 ? "󰕿" : backend.volume < 0.67 ? "󰖀" : "󰕾"
    }
    function repeatLabel() {
        return backend.repeat === 2 ? qsTr("Repeat one") : backend.repeat === 1 ? qsTr("Repeat all") : qsTr("Repeat")
    }
    // The first row of the artist and album columns is "All", a null.
    function artistLabel(value) {
        translator.language
        return value === null ? qsTr("All artists") : value === "" ? qsTr("Unknown artist") : value
    }
    function albumLabel(value) {
        translator.language
        return value === null ? qsTr("All albums") : value === "" ? qsTr("Unknown album") : value
    }

    function move(delta) {
        if (activeColumn === 0)
            backend.selectArtist(Math.max(0, Math.min(backend.artists.length, backend.artistRow + delta)))
        else if (activeColumn === 1)
            backend.selectAlbum(Math.max(0, Math.min(backend.albums.length, backend.albumRow + delta)))
        else if (activeColumn === 2)
            titleColumn.list.currentIndex = Math.max(0, Math.min(titleColumn.count - 1, titleColumn.currentIndex + delta))
        else
            queue.list.currentIndex = Math.max(0, Math.min(queue.count - 1, queue.currentIndex + delta))
    }
    function activate() {
        if (activeColumn === 3) {
            backend.playFromQueue(queue.currentIndex)
        } else if (activeColumn === 2) {
            backend.play(titleColumn.currentIndex)
        } else {
            // Choosing an artist or album plays its first track.
            activeColumn = 2
            titleColumn.list.currentIndex = 0
            backend.play(0)
        }
    }
    function playFrom(column, row) {
        if (column === 2) {
            backend.play(row)
            return
        }
        if (column === 0) backend.selectArtist(row)
        else backend.selectAlbum(row)
        activeColumn = 2
        titleColumn.list.currentIndex = 0
        backend.play(0)
    }
    // An artist or album adds all its tracks; a title adds just itself.
    function enqueueFrom(column, row, next) {
        if (column === 2) backend.enqueue(row, next)
        else backend.enqueueGroup(column, row, next)
    }
    function addToQueue(next) {
        if (activeColumn === 3) return
        enqueueFrom(activeColumn, activeColumn === 0 ? backend.artistRow : activeColumn === 1 ? backend.albumRow : titleColumn.currentIndex, next)
    }
    function moveQueueItem(delta) {
        if (activeColumn !== 3) return
        const from = queue.currentIndex, to = from + delta
        if (to < 0 || to >= queue.count) return
        backend.moveInQueue(from, to)
        queue.list.currentIndex = to
    }
    function removeFromQueue() {
        if (activeColumn === 3) backend.removeFromQueue(queue.currentIndex)
    }
    // Resizing starts from the width the queue actually has, which the
    // browser's minimum width can hold below the saved ratio.
    function resizeQueue(delta) {
        backend.queueRatio = lower.queueWidth / lower.width + delta
    }

    AppShortcuts { win: win }

    // Right-click on a row: play it, play it next, or queue it.
    GtMenu {
        id: contextMenu
        objectName: "contextMenu"
        property int column: 2
        property int row: 0
        MenuItem { objectName: "playItem"; text: qsTr("Play"); onTriggered: win.playFrom(contextMenu.column, contextMenu.row) }
        MenuItem { objectName: "playNextItem"; text: qsTr("Play next"); onTriggered: win.enqueueFrom(contextMenu.column, contextMenu.row, true) }
        MenuItem { objectName: "addItem"; text: qsTr("Add to queue"); onTriggered: win.enqueueFrom(contextMenu.column, contextMenu.row, false) }
    }

    // Right-to-left languages mirror the window; the Translator sets the
    // application's layout direction.
    Item {
        anchors.fill: parent
        LayoutMirroring.enabled: Qt.application.layoutDirection === Qt.RightToLeft
        LayoutMirroring.childrenInherit: true

        ColumnLayout {
            anchors.fill: parent
            spacing: 0

            // Now playing: cover art top-left, the visualizer and seek bar beside it.
            RowLayout {
                id: nowPlaying
                Layout.fillWidth: true
                Layout.fillHeight: true
                Layout.margins: 24
                spacing: 24

                CoverArt {
                    Layout.alignment: Qt.AlignTop
                    Layout.preferredWidth: Math.min(240, nowPlaying.height)
                    Layout.preferredHeight: Layout.preferredWidth
                }

                ColumnLayout {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    spacing: 8

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 12
                        Label {
                            textFormat: Text.PlainText
                            Layout.fillWidth: true
                            text: backend.title.length > 0 ? backend.title
                                : backend.scanning ? qsTr("Scanning %1…").arg(backend.libraryPath)
                                : qsTr("Nothing playing")
                            font.pixelSize: 26
                            font.bold: true
                            elide: Text.ElideRight
                        }
                        // Pausing (or a track waiting where it was left) shows here,
                        // since the bars going flat is easy to miss.
                        Label {
                            textFormat: Text.PlainText
                            visible: backend.title.length > 0 && !backend.playing
                            text: qsTr("Paused")
                            color: win.dim
                            font.pixelSize: 14
                            font.capitalization: Font.AllUppercase
                            font.letterSpacing: 1
                        }
                    }
                    Label {
                        textFormat: Text.PlainText
                        Layout.fillWidth: true
                        visible: text.length > 0
                        text: backend.artist + (backend.artist.length > 0 && backend.album.length > 0 ? " · " : "") + backend.album
                        color: win.dim
                        font.pixelSize: 16
                        elide: Text.ElideRight
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        visible: win.errorShown && backend.error.length > 0 && !win.emptyLibrary
                        Label {
                            textFormat: Text.PlainText
                            Layout.fillWidth: true
                            text: backend.error
                            color: theme.danger
                            wrapMode: Text.WordWrap
                        }
                        IconButton {
                            glyph: "󰅖"
                            size: 24
                            tip: qsTr("Dismiss")
                            onClicked: win.errorShown = false
                        }
                    }

                    Item {
                        Layout.fillWidth: true
                        Layout.fillHeight: true

                        Visualizer {
                            anchors.fill: parent
                            visible: !backend.showLyrics
                            levels: backend.levels
                            style: backend.visualizerStyle
                            color: theme.accent
                            radius: Math.min(4, theme.radius)
                        }
                        LyricsView {
                            anchors.fill: parent
                            visible: backend.showLyrics
                        }
                        Text {
                            textFormat: Text.PlainText
                            anchors.right: parent.right
                            anchors.top: parent.top
                            visible: win.noticeVisible
                            text: win.noticeText
                            color: theme.dim
                            font.pixelSize: 13
                        }
                    }

                    // Seek bar: click or drag anywhere along it.
                    BarSlider {
                        Layout.fillWidth: true
                        Layout.preferredHeight: 20
                        fraction: backend.duration > 0 ? backend.position / backend.duration : 0
                        onMoved: f => backend.seek(f * backend.duration)
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        Label { text: win.clock(backend.position); color: win.dim }
                        Item { Layout.fillWidth: true }
                        // Time left on the sleep timer, or that it will stop after this track.
                        Label {
                            textFormat: Text.PlainText
                            visible: backend.sleepSetting !== 0
                            color: theme.accent
                            text: "☾ " + (backend.sleepSetting < 0 ? qsTr("end of track") : win.clock(backend.sleepRemaining * 1000))
                        }
                        Item { Layout.fillWidth: true }
                        Label { text: win.clock(backend.duration); color: win.dim }
                    }

                    TransportControls {
                        Layout.fillWidth: true
                        win: win
                    }
                }
            }

            Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: theme.highlight }

            // The library: Artist | Album | Title.
            Item {
                id: lower
                Layout.fillWidth: true
                Layout.preferredHeight: Math.max(220, win.height * 0.42)

                // The queue takes its saved share of the window, but never so
                // much that the browser can't show its three columns.
                readonly property real minBrowserWidth: 360
                readonly property real queueWidth: Math.max(120, Math.min(width * backend.queueRatio, width - minBrowserWidth))

                ColumnLayout {
                    id: browserPane
                    anchors.left: parent.left
                    anchors.right: queuePane.left
                    anchors.top: parent.top
                    anchors.bottom: parent.bottom
                    anchors.margins: 16
                    anchors.rightMargin: 8
                    spacing: 8

                FilterBox {
                    id: filter
                    Layout.fillWidth: true
                    onAccepted: { win.activeColumn = 2; titleColumn.list.currentIndex = 0; win.contentItem.forceActiveFocus() }
                    onDismissed: win.contentItem.forceActiveFocus()
                }

                Item {
                    Layout.fillWidth: true
                    Layout.fillHeight: true

                    RowLayout {
                        anchors.fill: parent
                        visible: !win.emptyLibrary && !backend.scanning
                        spacing: 12

                        BrowserColumn {
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            Layout.preferredWidth: 1
                            heading: qsTr("Artist")
                            model: [null].concat(backend.artists)
                            label: win.artistLabel
                            active: win.activeColumn === 0
                            currentIndex: backend.artistRow
                            onClicked: win.activeColumn = 0
                            onPicked: index => backend.selectArtist(index)
                            onContextRequested: index => { contextMenu.column = 0; contextMenu.row = index; contextMenu.popup() }
                        }
                        BrowserColumn {
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            Layout.preferredWidth: 1
                            heading: qsTr("Album")
                            model: [null].concat(backend.albums)
                            label: win.albumLabel
                            active: win.activeColumn === 1
                            currentIndex: backend.albumRow
                            onClicked: win.activeColumn = 1
                            onPicked: index => backend.selectAlbum(index)
                            onContextRequested: index => { contextMenu.column = 1; contextMenu.row = index; contextMenu.popup() }
                        }
                        BrowserColumn {
                            id: titleColumn
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            Layout.preferredWidth: 1.4
                            heading: qsTr("Title")
                            model: backend.titles
                            trailing: backend.titleSeconds.map(s => s > 0 ? win.clock(s * 1000) : "")
                            active: win.activeColumn === 2
                            playingIndex: backend.currentRow
                            onClicked: win.activeColumn = 2
                            onPicked: index => { list.currentIndex = index; backend.play(index) }
                            onContextRequested: index => { list.currentIndex = index; contextMenu.column = 2; contextMenu.row = index; contextMenu.popup() }

                            // The cursor follows the playing track, and stays in range
                            // when the list is narrowed.
                            Connections {
                                target: backend
                                function onCurrentChanged() {
                                    if (backend.currentRow >= 0)
                                        titleColumn.list.currentIndex = backend.currentRow
                                }
                                function onBrowserChanged() {
                                    titleColumn.list.currentIndex = Math.max(0, Math.min(titleColumn.list.currentIndex, backend.titles.length - 1))
                                }
                            }
                        }
                    }

                    LibraryPlaceholder {
                        anchors.centerIn: parent
                        width: Math.min(parent.width - 32, 480)
                        visible: win.emptyLibrary || backend.scanning
                        empty: win.emptyLibrary
                    }
                }

                // Quiet status line: how many tracks, and where the shortcuts are.
                RowLayout {
                    Layout.fillWidth: true
                    Label {
                        textFormat: Text.PlainText
                        text: backend.reading ? qsTr("Reading tags…") : qsTr("%n tracks", "", backend.titles.length)
                        color: win.dim
                        font.pixelSize: 12
                    }
                    Item { Layout.fillWidth: true }
                    Label {
                        textFormat: Text.PlainText
                        text: qsTr("Press %1 for shortcuts").arg("?")
                        color: win.dim
                        font.pixelSize: 12
                    }
                }
                }

                // The queue pane, and the splitter between it and the browser.
                Item {
                    id: queuePane
                    anchors.right: parent.right
                    anchors.top: parent.top
                    anchors.bottom: parent.bottom
                    width: lower.queueWidth

                    QueuePanel {
                        id: queue
                        anchors.fill: parent
                        anchors.margins: 16
                        anchors.leftMargin: 8
                        active: win.activeColumn === 3
                        onClicked: win.activeColumn = 3
                    }
                }

                QueueSplitter {
                    anchors.right: queuePane.left
                    anchors.rightMargin: -width / 2
                    anchors.top: parent.top
                    anchors.bottom: parent.bottom
                    container: lower
                }
            }
        }

        HelpOverlay {
            anchors.fill: parent
            visible: win.helpVisible
            onDismissed: win.helpVisible = false
        }
    }
}