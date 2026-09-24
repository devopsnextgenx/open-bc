import QtQuick
import QtQuick.Controls

TextEdit {
    id: editor

    property string sourceCode: ""
    property string language: "rust"
    property bool darkMode: true
    property string highlightedHtml: ""
    property var highlightBridge: null

    readOnly: true
    selectByMouse: true
    textFormat: TextEdit.RichText
    wrapMode: TextEdit.NoWrap
    text: highlightedHtml

    onSourceCodeChanged: refreshHighlight()
    onLanguageChanged: refreshHighlight()
    onDarkModeChanged: refreshHighlight()

    function refreshHighlight() {
        if (highlightBridge) {
            highlightedHtml = highlightBridge.highlightToHtml(sourceCode, language, darkMode)
        }
    }
}