import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Shown in place of the browser while scanning, and when there is nothing to
// browse: says so, and says what to do about it.
ColumnLayout {
    spacing: 12

    property bool empty: false

    BusyIndicator {
        Layout.alignment: Qt.AlignHCenter
        visible: backend.scanning
        running: visible
    }
    Label {
        textFormat: Text.PlainText
        Layout.fillWidth: true
        horizontalAlignment: Text.AlignHCenter
        wrapMode: Text.WordWrap
        font.pixelSize: 18
        text: backend.scanning ? qsTr("Scanning %1…").arg(backend.libraryPath)
            : backend.error.length > 0 ? backend.error : qsTr("No music found")
    }
    Label {
        textFormat: Text.PlainText
        Layout.fillWidth: true
        visible: empty
        horizontalAlignment: Text.AlignHCenter
        wrapMode: Text.WordWrap
        color: theme.dim
        text: qsTr("Press %1 to choose a music folder.").arg("O")
    }
    Rectangle {
        Layout.alignment: Qt.AlignHCenter
        visible: empty
        implicitWidth: chooseLabel.implicitWidth + 32
        implicitHeight: 38
        radius: Math.min(8, theme.radius)
        color: chooseTap.pressed ? Qt.darker(theme.accent, 1.2) : theme.accent
        Label {
            textFormat: Text.PlainText
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
