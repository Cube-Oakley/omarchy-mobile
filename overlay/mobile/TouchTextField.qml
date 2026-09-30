import QtQuick
import QtQuick.Controls

// A popup becoming visible or regaining focus is not a request to type.
// Long-press selects; Copy/Paste use the system clipboard. The shell records
// copies through copyRequested so history stays in omarchy-mobile-clipboard.
TextField {
    id: field
    property bool editing: false
    property bool selecting: false
    property bool longPressUsed: false
    property bool canCopy: echoMode !== TextInput.Password
    signal editingRequested()
    signal copyRequested(string text)
    readOnly: !editing && !selecting
    // Themed by default (Qt's own field is white); a caller can still override.
    color: MobileTheme.foreground
    placeholderTextColor: MobileTheme.secondary
    selectionColor: MobileTheme.selection
    selectedTextColor: MobileTheme.foreground
    font.family: MobileTheme.fontFamily
    font.pixelSize: 16
    leftPadding: 14
    rightPadding: 14
    implicitHeight: 50
    background: Rectangle {
        radius: MobileTheme.radius(12)
        color: MobileTheme.surface
        border.width: 1
        border.color: field.editing ? MobileTheme.accent : MobileTheme.muted
    }
    persistentSelection: true
    selectByMouse: true
    focusPolicy: Qt.NoFocus
    activeFocusOnPress: false
    onEditingChanged: if (!editing) {
        selecting = false
        deselect()
    }
    function textInput() { return field.contentItem }
    function posAt(point) {
        const item = textInput();
        if (!item || typeof item.positionAt !== "function") return cursorPosition;
        return item.positionAt(point.x - item.x, point.y - item.y);
    }
    function caretRect(pos) {
        const item = textInput();
        if (!item) return Qt.rect(0, 0, 1, height);
        let rect;
        if (typeof item.positionToRectangle === "function")
            rect = item.positionToRectangle(pos);
        else {
            const start = selectionStart, end = selectionEnd;
            cursorPosition = pos;
            rect = item.cursorRectangle;
            if (start !== end) select(start, end);
        }
        return Qt.rect(item.x + rect.x, item.y + rect.y, Math.max(1, rect.width), Math.max(field.font.pixelSize, rect.height));
    }
    function beginSelection(point) {
        selecting = true;
        editingRequested();
        cursorPosition = posAt(point);
        selectWord();
        if (!selectedText.length && text.length)
            select(Math.max(0, cursorPosition - 1), Math.min(text.length, cursorPosition + 1));
    }
    function copySelection() {
        if (!canCopy || !selectedText.length) return;
        copy();
        copyRequested(selectedText);
    }
    function pasteClipboard() {
        paste();
        selecting = false;
    }
    TapHandler {
        id: tap
        longPressThreshold: 0.45
        onPressedChanged: if (pressed) field.longPressUsed = false
        onTapped: {
            if (field.longPressUsed) return;
            field.selecting = false;
            field.editingRequested();
        }
        onLongPressed: {
            field.longPressUsed = true;
            field.beginSelection(point.position);
        }
    }
    Rectangle {
        id: toolbar
        visible: field.selecting
        z: 8
        width: tools.width + 16
        height: 40
        radius: MobileTheme.radius(12)
        x: field.selecting ? Math.max(0, Math.min(field.width - width, field.caretRect(field.selectionStart).x - 8)) : 0
        y: field.selecting ? Math.max(-44, field.caretRect(field.selectionStart).y - 44) : 0
        color: MobileTheme.surface
        border.color: MobileTheme.muted
        Row {
            id: tools
            anchors.centerIn: parent
            spacing: 14
            Repeater {
                model: [
                    {label: "Copy", enabled: field.canCopy && field.selectedText.length > 0, action: "copy"},
                    {label: "Paste", enabled: true, action: "paste"},
                    {label: "All", enabled: field.text.length > 0, action: "all"}
                ]
                Text {
                    required property var modelData
                    text: modelData.label
                    color: modelData.enabled ? MobileTheme.foreground : MobileTheme.secondary
                    font.pixelSize: 14
                    font.bold: true
                    TapHandler {
                        enabled: modelData.enabled
                        onTapped: {
                            if (modelData.action === "copy") field.copySelection();
                            else if (modelData.action === "paste") field.pasteClipboard();
                            else field.selectAll();
                        }
                    }
                }
            }
        }
    }
    Repeater {
        model: field.selecting && field.selectedText.length ? [field.selectionStart, field.selectionEnd] : []
        Rectangle {
            id: handle
            required property int index
            required property int modelData
            property real grabX: 0
            z: 9
            width: 22
            height: 22
            radius: MobileTheme.radius(11)
            color: MobileTheme.accent
            x: drag.active ? grabX + drag.translation.x : field.caretRect(modelData).x - width / 2
            y: field.caretRect(modelData).y + field.caretRect(modelData).height - 6
            DragHandler {
                id: drag
                target: null
                xAxis.enabled: true
                yAxis.enabled: false
                onActiveChanged: if (active) handle.grabX = handle.x
                onTranslationChanged: {
                    if (!active) return;
                    const pos = field.posAt(Qt.point(handle.x + handle.width / 2, handle.y));
                    if (handle.index === 0) field.select(Math.min(pos, field.selectionEnd), field.selectionEnd);
                    else field.select(field.selectionStart, Math.max(pos, field.selectionStart));
                }
            }
        }
    }
}
