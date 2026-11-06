import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

ApplicationWindow {
    id: win
    width: 1000
    height: 640
    minimumWidth: 640
    minimumHeight: 440
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

    // Everything here comes from the current Omarchy theme.
    readonly property color ground: theme.background
    readonly property color dim: theme.muted
    property bool helpVisible: false
    property int visualizerStyle: 0
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
    function repeatLabel() {
        return backend.repeat === 2 ? qsTr("Repeat one") : backend.repeat === 1 ? qsTr("Repeat all") : qsTr("Repeat")
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
    Shortcut { sequence: "V"; context: Qt.ApplicationShortcut; enabled: !win.typing; onActivated: win.visualizerStyle = (win.visualizerStyle + 1) % 3 }
    Shortcut { sequences: ["J", "Down"]; context: Qt.ApplicationShortcut; enabled: !win.typing; onActivated: list.incrementCurrentIndex() }
    Shortcut { sequences: ["K", "Up"]; context: Qt.ApplicationShortcut; enabled: !win.typing; onActivated: list.decrementCurrentIndex() }
    Shortcut { sequences: ["Return", "Enter"]; context: Qt.ApplicationShortcut; enabled: !win.typing; onActivated: backend.play(list.currentIndex) }
    Shortcut { sequence: "/"; context: Qt.ApplicationShortcut; enabled: !win.typing; onActivated: filter.forceActiveFocus() }

    // Right-to-left languages mirror the window; the Translator sets the
    // application's layout direction.
    Item {
        anchors.fill: parent
        LayoutMirroring.enabled: Qt.application.layoutDirection === Qt.RightToLeft
        LayoutMirroring.childrenInherit: true

        RowLayout {
            anchors.fill: parent
            spacing: 0

            // Now playing and the visualizer.
            ColumnLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                Layout.margins: 24
                spacing: 16

                Visualizer {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    levels: backend.levels
                    style: win.visualizerStyle
                    color: theme.accent
                    radius: Math.min(4, theme.radius)
                }

                Label {
                    Layout.fillWidth: true
                    visible: backend.error.length > 0
                    text: backend.error
                    color: theme.danger
                    wrapMode: Text.WordWrap
                }

                Label {
                    Layout.fillWidth: true
                    text: backend.title.length > 0 ? backend.title
                        : backend.scanning ? qsTr("Scanning %1…").arg(backend.libraryPath)
                        : qsTr("Nothing playing")
                    font.pixelSize: 26
                    font.bold: true
                    elide: Text.ElideRight
                }
                Label {
                    Layout.fillWidth: true
                    visible: backend.artist.length > 0
                    text: backend.artist
                    color: win.dim
                    font.pixelSize: 16
                    elide: Text.ElideRight
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
                        color: theme.selection
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
                    Label {
                        text: (backend.shuffle ? qsTr("Shuffle") + "   " : "")
                            + (backend.repeat > 0 ? win.repeatLabel() + "   " : "")
                            + (backend.muted ? qsTr("Muted") : qsTr("Volume %1%").arg(win.percent(backend.volume)))
                        color: win.dim
                    }
                    Item { Layout.fillWidth: true }
                    Label { text: win.clock(backend.duration); color: win.dim }
                }
            }

            Rectangle { Layout.fillHeight: true; width: 1; color: theme.selection }

            // The library.
            ColumnLayout {
                Layout.preferredWidth: 340
                Layout.fillHeight: true
                Layout.margins: 16
                spacing: 8

                TextField {
                    id: filter
                    Layout.fillWidth: true
                    placeholderText: qsTr("Filter")
                    text: backend.filter
                    onTextChanged: backend.filter = text
                    onAccepted: { list.forceActiveFocus(); list.currentIndex = 0 }
                    Keys.onEscapePressed: { text = ""; list.forceActiveFocus() }
                }

                ListView {
                    id: list
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    clip: true
                    model: backend.tracks
                    currentIndex: 0
                    boundsBehavior: Flickable.StopAtBounds
                    ScrollBar.vertical: ScrollBar {}

                    // The cursor follows the playing track.
                    Connections {
                        target: backend
                        function onCurrentChanged() {
                            if (backend.currentRow >= 0)
                                list.currentIndex = backend.currentRow
                        }
                    }

                    delegate: Item {
                        required property int index
                        required property string modelData
                        width: ListView.view.width
                        height: 34

                        readonly property bool playingRow: index === backend.currentRow

                        Rectangle {
                            anchors.fill: parent
                            radius: Math.min(4, theme.radius)
                            color: parent.ListView.isCurrentItem ? theme.selection : rowHover.hovered ? theme.raised : "transparent"
                        }
                        Label {
                            anchors.fill: parent
                            anchors.leftMargin: 10
                            anchors.rightMargin: 10
                            verticalAlignment: Text.AlignVCenter
                            text: modelData
                            elide: Text.ElideMiddle
                            color: playingRow ? theme.accent : theme.foreground
                            font.bold: playingRow
                        }
                        HoverHandler { id: rowHover }
                        TapHandler { onTapped: { list.currentIndex = index; backend.play(index) } }
                    }
                }

                Label {
                    Layout.fillWidth: true
                    text: qsTr("%n tracks", "", backend.tracks.length)
                    color: win.dim
                    font.pixelSize: 12
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
