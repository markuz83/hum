import QtQuick
import QtQuick.Controls

// Busy, as a braille spinner stepping at 80 ms a frame.
Label {
    id: spinner
    property bool running: true
    property int frame: 0
    readonly property var frames: ["⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧", "⠇", "⠏"]

    textFormat: Text.PlainText
    text: frames[frame]
    color: theme.accent
    font.pixelSize: 28
    Timer {
        interval: 80
        repeat: true
        running: spinner.running && spinner.visible
        onTriggered: spinner.frame = (spinner.frame + 1) % spinner.frames.length
    }
}
