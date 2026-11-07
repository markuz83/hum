import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Effects
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

    Material.theme: theme.dark ? Material.Dark : Material.Light
    Material.accent: theme.accent
    Material.foreground: theme.foreground
    Material.roundedScale: theme.radius
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
    readonly property string rateLabel: backend.visualizerRate === 0
        ? qsTr("matches display (%1 Hz)").arg(Math.round(backend.refreshRate))
        : qsTr("%1 Hz").arg(backend.visualizerRate)
    function sleepLabel(minutes) {
        return minutes === 0 ? qsTr("off") : minutes < 0 ? qsTr("end of track") : qsTr("%1 min").arg(minutes)
    }
    Binding { target: backend; property: "refreshRate"; value: win.screen ? win.screen.refreshRate : 60 }
    Timer { id: noticeTimer; interval: 2000; onTriggered: win.noticeVisible = false }
    // One tick per frame the window actually draws, while the bars move.
    FrameAnimation {
        running: backend.animating && backend.visualizerRate === 0
        onTriggered: backend.frame()
    }
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
    readonly property bool typing: filter.activeFocus

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

    Shortcut { sequence: "?"; context: Qt.ApplicationShortcut; enabled: !win.typing; onActivated: win.helpVisible = !win.helpVisible }
    Shortcut { sequence: "Escape"; context: Qt.ApplicationShortcut; enabled: win.helpVisible; onActivated: win.helpVisible = false }
    Shortcut { sequence: "Q"; context: Qt.ApplicationShortcut; enabled: !win.typing; onActivated: Qt.quit() }
    Shortcut { sequence: "Space"; context: Qt.ApplicationShortcut; enabled: !win.typing; onActivated: backend.toggle() }
    Shortcut { sequence: "N"; context: Qt.ApplicationShortcut; enabled: !win.typing; onActivated: backend.next() }
    Shortcut { sequence: "P"; context: Qt.ApplicationShortcut; enabled: !win.typing; onActivated: backend.previous() }
    Shortcut { sequence: "Right"; context: Qt.ApplicationShortcut; enabled: !win.typing; onActivated: backend.seekBy(5000) }
    Shortcut { sequence: "Left"; context: Qt.ApplicationShortcut; enabled: !win.typing; onActivated: backend.seekBy(-5000) }
    Shortcut { sequences: ["+", "="]; context: Qt.ApplicationShortcut; enabled: !win.typing; onActivated: backend.changeVolume(0.05) }
    Shortcut { sequences: ["-", "_"]; context: Qt.ApplicationShortcut; enabled: !win.typing; onActivated: backend.changeVolume(-0.05) }
    Shortcut { sequence: "M"; context: Qt.ApplicationShortcut; enabled: !win.typing; onActivated: backend.toggleMute() }
    Shortcut { sequence: "S"; context: Qt.ApplicationShortcut; enabled: !win.typing; onActivated: backend.toggleShuffle() }
    Shortcut { sequence: "R"; context: Qt.ApplicationShortcut; enabled: !win.typing; onActivated: backend.cycleRepeat() }
    Shortcut {
        sequence: "F"; context: Qt.ApplicationShortcut; enabled: !win.typing
        onActivated: { backend.cycleVisualizerRate(); win.notify(qsTr("Visualizer refresh: %1").arg(win.rateLabel)) }
    }
    Shortcut {
        sequence: "G"; context: Qt.ApplicationShortcut; enabled: !win.typing
        onActivated: {
            backend.cycleReplayGain()
            win.notify(qsTr("Volume leveling: %1").arg(backend.replayGain === 0 ? qsTr("off")
                : backend.replayGain === 1 ? qsTr("per track") : qsTr("per album")))
        }
    }
    Shortcut {
        sequence: "C"; context: Qt.ApplicationShortcut; enabled: !win.typing
        onActivated: {
            backend.cycleCrossfade()
            win.notify(qsTr("Crossfade: %1").arg(backend.crossfade === 0 ? qsTr("off (gapless)") : qsTr("%1 s").arg(backend.crossfade)))
        }
    }
    Shortcut {
        sequence: "T"; context: Qt.ApplicationShortcut; enabled: !win.typing
        onActivated: { backend.cycleSleep(); win.notify(qsTr("Sleep timer: %1").arg(win.sleepLabel(backend.sleepSetting))) }
    }
    Shortcut { sequence: "Y"; context: Qt.ApplicationShortcut; enabled: !win.typing; onActivated: backend.showLyrics = !backend.showLyrics }
    Shortcut {
        sequence: "Shift+S"; context: Qt.ApplicationShortcut; enabled: !win.typing
        onActivated: {
            backend.cycleSort()
            win.notify(qsTr("Sort: %1").arg([qsTr("album order"), qsTr("title"), qsTr("year"), qsTr("recently added"), qsTr("length")][backend.sortMode]))
        }
    }
    Shortcut { sequence: "V"; context: Qt.ApplicationShortcut; enabled: !win.typing; onActivated: backend.cycleVisualizer() }
    Shortcut { sequences: ["J", "Down"]; context: Qt.ApplicationShortcut; enabled: !win.typing; onActivated: win.move(1) }
    Shortcut { sequences: ["K", "Up"]; context: Qt.ApplicationShortcut; enabled: !win.typing; onActivated: win.move(-1) }
    Shortcut { sequences: ["L", "Tab"]; context: Qt.ApplicationShortcut; enabled: !win.typing; onActivated: win.activeColumn = Math.min(3, win.activeColumn + 1) }
    Shortcut { sequences: ["H", "Backtab"]; context: Qt.ApplicationShortcut; enabled: !win.typing; onActivated: win.activeColumn = Math.max(0, win.activeColumn - 1) }
    Shortcut { sequences: ["Return", "Enter"]; context: Qt.ApplicationShortcut; enabled: !win.typing; onActivated: win.activate() }
    Shortcut { sequence: "O"; context: Qt.ApplicationShortcut; enabled: !win.typing; onActivated: backend.chooseLibrary() }
    Shortcut { sequence: "/"; context: Qt.ApplicationShortcut; enabled: !win.typing; onActivated: filter.forceActiveFocus() }
    Shortcut { sequence: "A"; context: Qt.ApplicationShortcut; enabled: !win.typing; onActivated: win.addToQueue(false) }
    Shortcut { sequence: "Shift+A"; context: Qt.ApplicationShortcut; enabled: !win.typing; onActivated: win.addToQueue(true) }
    Shortcut { sequences: ["X", "Delete", "Backspace"]; context: Qt.ApplicationShortcut; enabled: !win.typing; onActivated: win.removeFromQueue() }
    Shortcut { sequence: "Shift+X"; context: Qt.ApplicationShortcut; enabled: !win.typing; onActivated: backend.clearQueue() }
    Shortcut { sequences: ["Shift+J", "Alt+Down"]; context: Qt.ApplicationShortcut; enabled: !win.typing; onActivated: win.moveQueueItem(1) }
    Shortcut { sequences: ["Shift+K", "Alt+Up"]; context: Qt.ApplicationShortcut; enabled: !win.typing; onActivated: win.moveQueueItem(-1) }
    Shortcut { sequence: "Ctrl+Z"; context: Qt.ApplicationShortcut; enabled: !win.typing; onActivated: backend.undoQueue() }
    Shortcut { sequences: ["Ctrl+Shift+Z", "Ctrl+Y"]; context: Qt.ApplicationShortcut; enabled: !win.typing; onActivated: backend.redoQueue() }
    // Pane sizes: [ and ] make the queue narrower or wider, and \ puts it back.
    Shortcut { sequence: "]"; context: Qt.ApplicationShortcut; enabled: !win.typing; onActivated: win.resizeQueue(0.02) }
    Shortcut { sequence: "["; context: Qt.ApplicationShortcut; enabled: !win.typing; onActivated: win.resizeQueue(-0.02) }
    Shortcut { sequence: "\\"; context: Qt.ApplicationShortcut; enabled: !win.typing; onActivated: backend.queueRatio = 1 / 3 }

    // Right-click on a row: play it, play it next, or queue it.
    Menu {
        id: contextMenu
        objectName: "contextMenu"
        property int column: 2
        property int row: 0
        font.family: theme.fontFamily
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

                Item {
                    Layout.alignment: Qt.AlignTop
                    Layout.preferredWidth: Math.min(240, nowPlaying.height)
                    Layout.preferredHeight: Layout.preferredWidth

                    Rectangle {
                        anchors.fill: parent
                        color: theme.highlight
                        radius: theme.radius
                        Label {
                            anchors.centerIn: parent
                            visible: !backend.hasCover
                            text: "♪"
                            color: theme.dim
                            font.pixelSize: parent.width / 3
                        }
                    }
                    Image {
                        id: coverImage
                        anchors.fill: parent
                        // Sharp corners need no effect; with rounded windows the
                        // image is hidden here and drawn through the mask below.
                        visible: backend.hasCover && theme.radius === 0
                        source: backend.hasCover ? "image://cover/" + backend.coverId : ""
                        fillMode: Image.PreserveAspectCrop
                        sourceSize: Qt.size(480, 480)
                        cache: false
                    }
                    // Rounded to match the windows' corners.
                    Rectangle {
                        id: coverMask
                        anchors.fill: parent
                        radius: theme.radius
                        visible: false
                        layer.enabled: true
                    }
                    MultiEffect {
                        anchors.fill: parent
                        visible: backend.hasCover && theme.radius > 0
                        source: coverImage
                        maskEnabled: true
                        maskSource: coverMask
                    }
                }

                ColumnLayout {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    spacing: 8

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 12
                        Label {
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
                            visible: backend.title.length > 0 && !backend.playing
                            text: qsTr("Paused")
                            color: win.dim
                            font.pixelSize: 14
                            font.capitalization: Font.AllUppercase
                            font.letterSpacing: 1
                        }
                    }
                    Label {
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
                            anchors.right: parent.right
                            anchors.top: parent.top
                            visible: win.noticeVisible
                            text: win.noticeText
                            color: theme.dim
                            font.pixelSize: 13
                        }
                    }

                    // Seek bar: click or drag anywhere along it.
                    Item {
                        id: seek
                        Layout.fillWidth: true
                        Layout.preferredHeight: 20

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
                                width: backend.duration > 0 ? parent.width * Math.min(1, backend.position / backend.duration) : 0
                                color: theme.accent
                            }
                        }
                        MouseArea {
                            anchors.fill: parent
                            preventStealing: true
                            function seekTo() {
                                const x = LayoutMirroring.enabled ? width - mouseX : mouseX
                                backend.seek(Math.max(0, Math.min(1, x / width)) * backend.duration)
                            }
                            onPressed: seekTo()
                            onPositionChanged: if (pressed) seekTo()
                        }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        Label { text: win.clock(backend.position); color: win.dim }
                        Item { Layout.fillWidth: true }
                        // Time left on the sleep timer, or that it will stop after this track.
                        Label {
                            visible: backend.sleepSetting !== 0
                            color: theme.accent
                            text: "☾ " + (backend.sleepSetting < 0 ? qsTr("end of track") : win.clock(backend.sleepRemaining * 1000))
                        }
                        Item { Layout.fillWidth: true }
                        Label { text: win.clock(backend.duration); color: win.dim }
                    }

                    // Transport keeps previous, play, next in every language, like
                    // their glyphs; media controls are not mirrored.
                    RowLayout {
                        Layout.fillWidth: true
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
                            tip: win.repeatLabel()
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
                            glyph: win.volumeGlyph()
                            tip: backend.muted ? qsTr("Unmute") : qsTr("Mute")
                            onClicked: backend.toggleMute()
                        }
                        // Volume: click or drag along it, or scroll over it.
                        Item {
                            Layout.preferredWidth: 110
                            Layout.preferredHeight: 28
                            Rectangle {
                                anchors.verticalCenter: parent.verticalCenter
                                width: parent.width
                                height: 4
                                radius: Math.min(2, theme.radius)
                                color: theme.highlight
                                Rectangle {
                                    height: parent.height
                                    radius: parent.radius
                                    width: parent.width * (backend.muted ? 0 : backend.volume)
                                    color: theme.accent
                                }
                            }
                            MouseArea {
                                anchors.fill: parent
                                preventStealing: true
                                function setTo() { backend.volume = Math.max(0, Math.min(1, mouseX / width)) }
                                onPressed: setTo()
                                onPositionChanged: if (pressed) setTo()
                                onWheel: wheel => backend.changeVolume(wheel.angleDelta.y > 0 ? 0.05 : -0.05)
                            }
                            ToolTip.visible: volumeHover.hovered
                            ToolTip.delay: 600
                            ToolTip.text: backend.muted ? qsTr("Muted") : qsTr("Volume %1%").arg(win.percent(backend.volume))
                            HoverHandler { id: volumeHover; cursorShape: Qt.PointingHandCursor }
                        }
                    }
                }
            }

            Rectangle { Layout.fillWidth: true; height: 1; color: theme.highlight }

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

                // A compact filter box, with a clear button once it has text.
                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 34
                    radius: Math.min(6, theme.radius)
                    color: "transparent"
                    border.width: 1
                    border.color: filter.activeFocus ? theme.accent : theme.highlight

                    TapHandler { onTapped: filter.forceActiveFocus() }

                    Label {
                        anchors.left: parent.left
                        anchors.leftMargin: 12
                        anchors.verticalCenter: parent.verticalCenter
                        visible: filter.text.length === 0
                        text: qsTr("Filter")
                        color: win.dim
                    }
                    TextInput {
                        id: filter
                        anchors.left: parent.left
                        anchors.right: clearButton.left
                        anchors.leftMargin: 12
                        anchors.verticalCenter: parent.verticalCenter
                        clip: true
                        color: theme.foreground
                        selectionColor: theme.accent
                        selectedTextColor: theme.accentForeground
                        font.family: theme.fontFamily
                        font.pixelSize: 14
                        text: backend.filter
                        onTextChanged: backend.filter = text
                        onAccepted: { win.activeColumn = 2; titleColumn.list.currentIndex = 0; win.contentItem.forceActiveFocus() }
                        Keys.onEscapePressed: { text = ""; win.contentItem.forceActiveFocus() }
                    }
                    IconButton {
                        id: clearButton
                        anchors.right: parent.right
                        anchors.rightMargin: 4
                        anchors.verticalCenter: parent.verticalCenter
                        visible: filter.text.length > 0
                        glyph: "󰅖"
                        size: 24
                        tip: qsTr("Clear filter")
                        onClicked: { filter.text = ""; filter.forceActiveFocus() }
                    }
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

                    // While scanning, and when there is nothing to browse, say so
                    // and say what to do about it.
                    ColumnLayout {
                        anchors.centerIn: parent
                        width: Math.min(parent.width - 32, 480)
                        visible: win.emptyLibrary || backend.scanning
                        spacing: 12

                        BusyIndicator {
                            Layout.alignment: Qt.AlignHCenter
                            visible: backend.scanning
                            running: visible
                        }
                        Label {
                            Layout.fillWidth: true
                            horizontalAlignment: Text.AlignHCenter
                            wrapMode: Text.WordWrap
                            font.pixelSize: 18
                            text: backend.scanning ? qsTr("Scanning %1…").arg(backend.libraryPath)
                                : backend.error.length > 0 ? backend.error : qsTr("No music found")
                        }
                        Label {
                            Layout.fillWidth: true
                            visible: win.emptyLibrary
                            horizontalAlignment: Text.AlignHCenter
                            wrapMode: Text.WordWrap
                            color: win.dim
                            text: qsTr("Press %1 to choose a music folder.").arg("O")
                        }
                        Rectangle {
                            Layout.alignment: Qt.AlignHCenter
                            visible: win.emptyLibrary
                            implicitWidth: chooseLabel.implicitWidth + 32
                            implicitHeight: 38
                            radius: Math.min(8, theme.radius)
                            color: chooseTap.pressed ? Qt.darker(theme.accent, 1.2) : theme.accent
                            Label {
                                id: chooseLabel
                                anchors.centerIn: parent
                                text: qsTr("Choose music folder")
                                color: theme.accentForeground
                                font.bold: true
                            }
                            HoverHandler { cursorShape: Qt.PointingHandCursor }
                            TapHandler { id: chooseTap; onTapped: backend.chooseLibrary() }
                        }
                    }
                }

                // Quiet status line: how many tracks, and where the shortcuts are.
                RowLayout {
                    Layout.fillWidth: true
                    Label {
                        text: backend.reading ? qsTr("Reading tags…") : qsTr("%n tracks", "", backend.titles.length)
                        color: win.dim
                        font.pixelSize: 12
                    }
                    Item { Layout.fillWidth: true }
                    Label {
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

                // Drag to resize; double-click to reset. [ and ] do the same from the keyboard.
                Item {
                    id: splitter
                    anchors.right: queuePane.left
                    anchors.rightMargin: -width / 2
                    anchors.top: parent.top
                    anchors.bottom: parent.bottom
                    width: 12
                    z: 5

                    Rectangle {
                        anchors.horizontalCenter: parent.horizontalCenter
                        anchors.verticalCenter: parent.verticalCenter
                        width: splitArea.pressed || splitArea.containsMouse ? 3 : 1
                        height: parent.height - 24
                        radius: width / 2
                        color: splitArea.pressed || splitArea.containsMouse ? theme.accent : theme.highlight
                    }
                    MouseArea {
                        id: splitArea
                        objectName: "splitter"
                        anchors.fill: parent
                        hoverEnabled: true
                        preventStealing: true
                        cursorShape: Qt.SplitHCursor
                        onPositionChanged: mouse => {
                            if (!pressed) return
                            const x = mapToItem(lower, mouse.x, 0).x
                            // Right-to-left puts the queue on the left, so the share flips.
                            backend.queueRatio = LayoutMirroring.enabled ? x / lower.width : 1 - x / lower.width
                        }
                        onDoubleClicked: backend.queueRatio = 1 / 3
                    }
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
