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

  // Optional extras: the track's name scrolling beside the bars, and a thin
  // progress line along the bottom edge that a click seeks along.
  readonly property bool showTitle: horizontal && setting("showTitle", false) === true
  readonly property int titleWidth: Math.max(60, Math.min(400, Math.round(Number(setting("titleWidth", 160)))))
  readonly property bool showProgress: setting("progress", true) !== false

  property string playback: "Stopped"
  property real position: 0
  property real duration: 0
  readonly property real played: Protocol.fraction(position, duration)
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
    : horizontal ? content.implicitWidth + Style.space(14)
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
    } else if (message.type === "progress") {
      position = message.position
      duration = message.duration
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

  Row {
    id: content
    anchors.centerIn: parent
    spacing: Style.space(8)

    Item {
      id: canvas
      anchors.verticalCenter: parent.verticalCenter
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

    // The track's name, scrolling when it is longer than the room given.
    Item {
      id: titleClip
      anchors.verticalCenter: parent.verticalCenter
      visible: root.showTitle && root.title !== ""
      width: Math.min(root.titleWidth, titleText.implicitWidth)
      height: titleText.implicitHeight
      clip: true

      Text {
        id: titleText
        textFormat: Text.PlainText
        text: root.title + (root.artist ? "  ·  " + root.artist : "")
        color: root.bar ? root.bar.barForeground : "white"
        opacity: root.playing ? 1 : 0.6
        font.family: root.bar ? root.bar.fontFamily : ""
        font.pixelSize: Style.font.body
        anchors.verticalCenter: parent.verticalCenter

        readonly property bool needsScroll: implicitWidth > titleClip.width
        // Every frame of the scroll redraws the bar, so it only runs while the
        // track plays and the bar is on screen to show it.
        NumberAnimation on x {
          running: titleText.needsScroll && root.playing && root.visible && titleClip.visible
                   && !(root.bar && root.bar.barHidden) && !Style.reduceMotion
          onRunningChanged: if (!running) titleText.x = 0
          loops: Animation.Infinite
          duration: Style.duration(Math.max(6000, titleText.implicitWidth * 25))
          from: titleClip.width
          to: -titleText.implicitWidth
          easing.type: Easing.Linear
        }
      }
    }
  }

  // How far through the track: a hairline along the bottom edge (or the
  // trailing edge of a vertical bar), under everything else.
  Rectangle {
    id: track
    visible: root.showProgress && root.duration > 0
    readonly property real span: root.horizontal ? content.width : canvas.height
    x: root.horizontal ? content.x : root.width - width - 1
    y: root.horizontal ? root.height - height - 1 : content.y + (content.height - canvas.height) / 2
    width: root.horizontal ? span : Math.max(2, Style.space(2))
    height: root.horizontal ? Math.max(2, Style.space(2)) : span
    radius: height / 2
    color: root.bar ? Qt.alpha(root.bar.barForeground, 0.18) : "#40ffffff"

    Rectangle {
      width: root.horizontal ? parent.width * root.played : parent.width
      height: root.horizontal ? parent.height : parent.height * root.played
      radius: parent.radius
      color: root.bar ? root.bar.barForeground : "white"
    }
  }

  MouseArea {
    anchors.fill: parent
    hoverEnabled: true
    cursorShape: Qt.PointingHandCursor
    acceptedButtons: Qt.LeftButton | Qt.MiddleButton | Qt.RightButton

    // A click on the progress line (the outer few pixels of the bar) seeks.
    function onProgress(mouse) {
      if (!root.showProgress || root.duration <= 0) return -1
      var edge = Math.max(6, Style.space(6))
      if (root.horizontal) {
        if (mouse.y < root.height - edge) return -1
        return (mouse.x - track.x) / Math.max(1, track.width)
      }
      if (mouse.x < root.width - edge) return -1
      return (mouse.y - track.y) / Math.max(1, track.height)
    }

    onClicked: function(mouse) {
      var along = mouse.button === Qt.LeftButton ? onProgress(mouse) : -1
      if (along >= 0) root.send("seek " + Math.round(Math.max(0, Math.min(1, along)) * root.duration))
      else if (mouse.button === Qt.MiddleButton) root.send("playpause")
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
