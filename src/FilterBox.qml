import QtQuick
import QtQuick.Controls

// A compact filter box, with a clear button once it has text.
Rectangle {
    id: box

    property alias input: input
    readonly property bool typing: input.activeFocus

    // The text was accepted with Return.
    signal accepted()
    // Escape was pressed; the filter is already cleared.
    signal dismissed()

    function focusInput() { input.forceActiveFocus() }

    implicitHeight: 34
    radius: Math.min(6, theme.radius)
    color: "transparent"
    border.width: 1
    border.color: input.activeFocus ? theme.accent : theme.highlight

    TapHandler { onTapped: input.forceActiveFocus() }

    Label {
        textFormat: Text.PlainText
        anchors.left: parent.left
        anchors.leftMargin: 12
        anchors.verticalCenter: parent.verticalCenter
        visible: input.text.length === 0
        text: qsTr("Filter")
        color: theme.dim
    }
    TextInput {
        id: input
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
        onAccepted: box.accepted()
        Keys.onEscapePressed: { text = ""; box.dismissed() }
    }
    IconButton {
        id: clearButton
        anchors.right: parent.right
        anchors.rightMargin: 4
        anchors.verticalCenter: parent.verticalCenter
        visible: input.text.length > 0
        glyph: "󰅖"
        size: 24
        tip: qsTr("Clear filter")
        onClicked: { input.text = ""; input.forceActiveFocus() }
    }
}
