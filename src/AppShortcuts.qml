import QtQuick

// Every keyboard shortcut. They are inert while the filter box has focus, so
// typing there never plays, skips or quits.
Item {
    id: keys

    required property var win

    Shortcut { sequence: "?"; context: Qt.ApplicationShortcut; enabled: !keys.win.typing; onActivated: keys.win.helpVisible = !keys.win.helpVisible }
    Shortcut { sequence: "Escape"; context: Qt.ApplicationShortcut; enabled: keys.win.helpVisible; onActivated: keys.win.helpVisible = false }
    Shortcut { sequence: "Q"; context: Qt.ApplicationShortcut; enabled: !keys.win.typing; onActivated: Qt.quit() }
    Shortcut { sequence: "Space"; context: Qt.ApplicationShortcut; enabled: !keys.win.typing; onActivated: backend.toggle() }
    Shortcut { sequence: "N"; context: Qt.ApplicationShortcut; enabled: !keys.win.typing; onActivated: backend.next() }
    Shortcut { sequence: "P"; context: Qt.ApplicationShortcut; enabled: !keys.win.typing; onActivated: backend.previous() }
    Shortcut { sequence: "Right"; context: Qt.ApplicationShortcut; enabled: !keys.win.typing; onActivated: backend.seekBy(5000) }
    Shortcut { sequence: "Left"; context: Qt.ApplicationShortcut; enabled: !keys.win.typing; onActivated: backend.seekBy(-5000) }
    Shortcut { sequences: ["+", "="]; context: Qt.ApplicationShortcut; enabled: !keys.win.typing; onActivated: backend.changeVolume(0.05) }
    Shortcut { sequences: ["-", "_"]; context: Qt.ApplicationShortcut; enabled: !keys.win.typing; onActivated: backend.changeVolume(-0.05) }
    Shortcut { sequence: "M"; context: Qt.ApplicationShortcut; enabled: !keys.win.typing; onActivated: backend.toggleMute() }
    Shortcut { sequence: "S"; context: Qt.ApplicationShortcut; enabled: !keys.win.typing; onActivated: backend.toggleShuffle() }
    Shortcut { sequence: "R"; context: Qt.ApplicationShortcut; enabled: !keys.win.typing; onActivated: backend.cycleRepeat() }
    Shortcut {
        sequence: "F"; context: Qt.ApplicationShortcut; enabled: !keys.win.typing
        onActivated: { backend.cycleVisualizerRate(); keys.win.notify(qsTr("Visualizer refresh: %1").arg(qsTr("%1 Hz").arg(backend.visualizerRate))) }
    }
    Shortcut {
        sequence: "G"; context: Qt.ApplicationShortcut; enabled: !keys.win.typing
        onActivated: {
            backend.cycleReplayGain()
            keys.win.notify(qsTr("Volume leveling: %1").arg(backend.replayGain === 0 ? qsTr("off")
                : backend.replayGain === 1 ? qsTr("per track") : qsTr("per album")))
        }
    }
    Shortcut {
        sequence: "C"; context: Qt.ApplicationShortcut; enabled: !keys.win.typing
        onActivated: {
            backend.cycleCrossfade()
            keys.win.notify(qsTr("Crossfade: %1").arg(backend.crossfade === 0 ? qsTr("off (gapless)") : qsTr("%1 s").arg(backend.crossfade)))
        }
    }
    Shortcut {
        sequence: "T"; context: Qt.ApplicationShortcut; enabled: !keys.win.typing
        onActivated: { backend.cycleSleep(); keys.win.notify(qsTr("Sleep timer: %1").arg(keys.win.sleepLabel(backend.sleepSetting))) }
    }
    Shortcut { sequence: "Y"; context: Qt.ApplicationShortcut; enabled: !keys.win.typing; onActivated: backend.showLyrics = !backend.showLyrics }
    Shortcut {
        sequence: "Shift+S"; context: Qt.ApplicationShortcut; enabled: !keys.win.typing
        onActivated: {
            backend.cycleSort()
            keys.win.notify(qsTr("Sort: %1").arg([qsTr("album order"), qsTr("title"), qsTr("year"), qsTr("recently added"), qsTr("length")][backend.sortMode]))
        }
    }
    Shortcut { sequence: "V"; context: Qt.ApplicationShortcut; enabled: !keys.win.typing; onActivated: backend.cycleVisualizer() }
    Shortcut { sequences: ["J", "Down"]; context: Qt.ApplicationShortcut; enabled: !keys.win.typing; onActivated: keys.win.move(1) }
    Shortcut { sequences: ["K", "Up"]; context: Qt.ApplicationShortcut; enabled: !keys.win.typing; onActivated: keys.win.move(-1) }
    Shortcut { sequences: ["L", "Tab"]; context: Qt.ApplicationShortcut; enabled: !keys.win.typing; onActivated: keys.win.activeColumn = Math.min(3, keys.win.activeColumn + 1) }
    Shortcut { sequences: ["H", "Backtab"]; context: Qt.ApplicationShortcut; enabled: !keys.win.typing; onActivated: keys.win.activeColumn = Math.max(0, keys.win.activeColumn - 1) }
    Shortcut { sequences: ["Return", "Enter"]; context: Qt.ApplicationShortcut; enabled: !keys.win.typing; onActivated: keys.win.activate() }
    Shortcut { sequence: "O"; context: Qt.ApplicationShortcut; enabled: !keys.win.typing; onActivated: backend.chooseLibrary() }
    Shortcut { sequence: "/"; context: Qt.ApplicationShortcut; enabled: !keys.win.typing; onActivated: filter.forceActiveFocus() }
    Shortcut { sequence: "A"; context: Qt.ApplicationShortcut; enabled: !keys.win.typing; onActivated: keys.win.addToQueue(false) }
    Shortcut { sequence: "Shift+A"; context: Qt.ApplicationShortcut; enabled: !keys.win.typing; onActivated: keys.win.addToQueue(true) }
    Shortcut { sequences: ["X", "Delete", "Backspace"]; context: Qt.ApplicationShortcut; enabled: !keys.win.typing; onActivated: keys.win.removeFromQueue() }
    Shortcut { sequence: "Shift+X"; context: Qt.ApplicationShortcut; enabled: !keys.win.typing; onActivated: backend.clearQueue() }
    Shortcut { sequences: ["Shift+J", "Alt+Down"]; context: Qt.ApplicationShortcut; enabled: !keys.win.typing; onActivated: keys.win.moveQueueItem(1) }
    Shortcut { sequences: ["Shift+K", "Alt+Up"]; context: Qt.ApplicationShortcut; enabled: !keys.win.typing; onActivated: keys.win.moveQueueItem(-1) }
    Shortcut { sequence: "Ctrl+Z"; context: Qt.ApplicationShortcut; enabled: !keys.win.typing; onActivated: backend.undoQueue() }
    Shortcut { sequences: ["Ctrl+Shift+Z", "Ctrl+Y"]; context: Qt.ApplicationShortcut; enabled: !keys.win.typing; onActivated: backend.redoQueue() }
    // Pane sizes: [ and ] make the queue narrower or wider, and \ puts it back.
    Shortcut { sequence: "]"; context: Qt.ApplicationShortcut; enabled: !keys.win.typing; onActivated: keys.win.resizeQueue(0.02) }
    Shortcut { sequence: "["; context: Qt.ApplicationShortcut; enabled: !keys.win.typing; onActivated: keys.win.resizeQueue(-0.02) }
    Shortcut { sequence: "\\"; context: Qt.ApplicationShortcut; enabled: !keys.win.typing; onActivated: backend.queueRatio = 1 / 3 }
}
