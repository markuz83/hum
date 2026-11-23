import QtQuick
import QtQuick.Controls
import QtQuick.Effects

// The current track's cover, rounded to match the windows' corners, with a
// note glyph standing in when there is none.
Item {
    Rectangle {
        anchors.fill: parent
        color: theme.highlight
        radius: theme.radius
        Label {
            textFormat: Text.PlainText
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
