import QtQuick
import Quickshell
import Quickshell.Io
import qs.Commons
import qs.Ui
import "Protocol.js" as Protocol

// Hum's live spectrum in the bar. Hum streams it over a local socket while it
// plays (src/barlink.h), so nothing runs here when Hum isn't, and the widget
// is hidden until it is.
BarWidget {
  id: root
  moduleName: "markm.hum-visualizer"

  readonly property int barCount: Math.max(4, Math.min(24, Math.round(Number(setting("bars", 12)))))
  readonly property bool horizontal: !vertical

  property string playback: "Stopped"
  property string title: ""
  property string artist: ""
  property var levels: []

  readonly property bool active: link.connected && playback !== "Stopped"
  readonly property bool playing: playback === "Playing"
  // Paused bars sit flat and dim; playing bars follow the music.
  readonly property var bars: playing && levels.length > 0 ? Protocol.resample(levels, shownBars) : new Array(shownBars).fill(0)
  // A vertical bar is only so wide, so it shows fewer, horizontal strips.
  readonly property int shownBars: vertical ? Math.min(barCount, 8) : barCount
  readonly property real thickness: Style.space(3)
  readonly property real gap: Style.space(2)
  // Most of the bar's height, leaving a little room above and below.
  readonly property real crossSize: Math.max(8, Math.round(barSize * 0.72))

  visible: active
  implicitHeight: barSize
  implicitWidth: !active ? 0
    : horizontal ? shownBars * thickness + (shownBars - 1) * gap + Style.space(14)
    : barSize

  function send(command) {
    if (link.connected) link.write(command + "\n")
    link.flush()
  }

  function handle(line) {
    var message = Protocol.parse(line)
    if (!message) return
    if (message.type === "levels") {
      // A hidden bar has nothing to redraw, so drop the frames.
      if (!bar || !bar.barHidden) levels = message.values
    } else if (message.type === "state") {
      playback = message.value
      if (message.value !== "Playing") levels = []
    } else if (message.type === "track") {
      title = message.title
      artist = message.artist
    }
  }

  Socket {
    id: link
    path: Quickshell.env("XDG_RUNTIME_DIR") + "/hum-bar.sock"

    parser: SplitParser {
      onRead: function(data) { root.handle(data) }
    }
    onConnectedChanged: {
      if (!connected) {
        root.playback = "Stopped"
        root.levels = []
      }
    }
  }

  // Hum may start after the shell does, so keep asking while it isn't there.
  Timer {
    interval: 3000
    repeat: true
    running: !link.connected
    triggeredOnStart: true
    onTriggered: link.connected = true
  }

  Item {
    id: canvas
    anchors.centerIn: parent
    width: root.horizontal ? root.shownBars * root.thickness + (root.shownBars - 1) * root.gap : root.crossSize
    height: root.horizontal ? root.crossSize : root.shownBars * root.thickness + (root.shownBars - 1) * root.gap

    Repeater {
      model: root.shownBars

      Rectangle {
        required property int index
        readonly property real level: (root.bars[index] || 0) / 99
        // Never fully empty, so the widget reads as a row of bars at rest.
        readonly property real length: Math.max(root.thickness, level * (root.horizontal ? canvas.height : canvas.width))

        radius: Math.min(root.thickness / 2, Style.cornerRadius)
        color: root.bar
          ? (root.playing ? root.bar.barForeground : Qt.darker(root.bar.barForeground, 1.8))
          : "white"
        width: root.horizontal ? root.thickness : length
        height: root.horizontal ? length : root.thickness
        x: root.horizontal ? index * (root.thickness + root.gap) : 0
        y: root.horizontal ? canvas.height - height : index * (root.thickness + root.gap)
      }
    }
  }

  MouseArea {
    anchors.fill: parent
    hoverEnabled: true
    cursorShape: Qt.PointingHandCursor
    acceptedButtons: Qt.LeftButton | Qt.MiddleButton | Qt.RightButton

    onClicked: function(mouse) {
      if (mouse.button === Qt.MiddleButton) root.send("playpause")
      else if (mouse.button === Qt.RightButton) root.send("next")
      else Quickshell.execDetached(["omarchy-launch-or-focus", "hum", "uwsm-app -- hum"])
    }
    onWheel: function(wheel) {
      if (wheel.angleDelta.y > 0) root.send("volume +5")
      else if (wheel.angleDelta.y < 0) root.send("volume -5")
    }
    // The shared tooltip draws plain text, so a title is shown as written.
    onEntered: if (root.bar) root.bar.showTooltip(root, root.title + (root.artist ? " — " + root.artist : ""))
    onExited: if (root.bar) root.bar.hideTooltip(root)
  }
}
