import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

ApplicationWindow {
    id: win
    width: 1120; height: 800
    minimumWidth: 560; minimumHeight: 460
    visible: true
    title: "Omarchy Local Live Sub"
    color: theme.background
    Material.theme: theme.lightMode ? Material.Light : Material.Dark
    Material.accent: theme.accent
    Material.foreground: theme.foreground
    Material.background: theme.background
    palette.window: theme.background
    palette.windowText: theme.foreground
    palette.text: theme.foreground
    palette.buttonText: theme.foreground
    palette.highlight: theme.accent
    palette.highlightedText: theme.accentForeground
    property bool fullScreen: visibility === Window.FullScreen
    property bool previewReading: false
    property bool reading: backend.recording || backend.busy || previewReading
    property bool follow: true
    property int subtitleSizeAdjustment: 0
    readonly property int subtitleSize: Math.max(18, Math.min(72, (fullScreen ? 42 : 32) + subtitleSizeAdjustment))
    property bool allowClose: false
    property bool closeWhenIdle: false
    property string lastError: ""

    function changeSubtitleSize(delta) {
        const next = Math.max(18, Math.min(72, subtitleSize + delta))
        subtitleSizeAdjustment += next - subtitleSize
    }

    function toggleCapture() {
        if (win.closeWhenIdle) return
        if (backend.recording || backend.starting) { backend.stop(); return }
        let input = backend.devices[microphone.currentIndex]
        win.follow = true
        settings.close()
        backend.toggle(input ? input.name : "", ["auto", "en", "zh"][language.currentIndex], threshold.value)
    }
    function toggleFullScreen() {
        if (win.fullScreen) win.showNormal(); else win.showFullScreen()
    }
    function showSettings() { settings.open() }
    onClosing: function(close) {
        if (!allowClose && backend.starting) {
            // A pending health check must never acquire the microphone after closing.
            backend.stop()
        } else if (!allowClose && backend.recording) {
            close.accepted = false; quitDialog.open()
        } else if (!allowClose && backend.busy) {
            close.accepted = false
            win.closeWhenIdle = true
        }
    }
    Shortcut { sequence: "Space"; context: Qt.ApplicationShortcut; onActivated: win.toggleCapture() }
    Shortcut { sequence: "F11"; context: Qt.ApplicationShortcut; onActivated: win.toggleFullScreen() }
    Shortcut { sequence: "Escape"; context: Qt.ApplicationShortcut; onActivated: { if (win.fullScreen) win.showNormal() } }
    Shortcut { sequence: "Ctrl+,"; context: Qt.ApplicationShortcut; onActivated: settings.open() }
    Shortcut { sequence: "Ctrl+S"; context: Qt.ApplicationShortcut; onActivated: backend.exportSession() }
    Shortcut { sequence: "End"; context: Qt.ApplicationShortcut; onActivated: { win.follow = true; subtitles.positionViewAtEnd() } }
    Shortcut { sequence: "Home"; context: Qt.ApplicationShortcut; onActivated: { win.follow = false; subtitles.positionViewAtBeginning() } }
    Shortcut { sequence: "PgUp"; context: Qt.ApplicationShortcut; onActivated: { win.follow = false; subtitles.contentY = Math.max(subtitles.originY, subtitles.contentY - subtitles.height * 0.85) } }
    Shortcut { sequence: "PgDown"; context: Qt.ApplicationShortcut; onActivated: { win.follow = false; subtitles.contentY = Math.min(subtitles.originY + Math.max(0, subtitles.contentHeight - subtitles.height), subtitles.contentY + subtitles.height * 0.85) } }
    Shortcut { sequence: "?"; context: Qt.ApplicationShortcut; onActivated: help.open() }
    Shortcut { sequence: "Q"; context: Qt.ApplicationShortcut; onActivated: win.close() }
    Connections {
        target: backend
        function onChanged() {
            if (win.closeWhenIdle) {
                if (!backend.recording && !backend.busy) { win.allowClose = true; win.close() }
                return
            }
            if (backend.error !== win.lastError) {
                win.lastError = backend.error
                if (backend.error.length > 0) errorDialog.open()
            }
        }
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: win.fullScreen ? 36 : 24
        spacing: win.reading ? 8 : 24
        RowLayout {
            visible: !win.reading
            Layout.fillWidth: true
            spacing: 8
            Label {
                text: "Omarchy Local Live Sub"
                font.pixelSize: 21; font.weight: Font.DemiBold
                color: theme.foreground; elide: Text.ElideRight; Layout.fillWidth: true
            }
            ToolButton { text: win.fullScreen ? qsTr("Exit full screen") : qsTr("Full screen"); onClicked: win.toggleFullScreen() }
            ToolButton {
                icon.source: "qrc:/settings.svg"; icon.width: 20; icon.height: 20; icon.color: theme.secondaryForeground
                Accessible.name: qsTr("Settings"); ToolTip.visible: hovered; ToolTip.text: qsTr("Settings · Ctrl+,")
                onClicked: settings.open()
            }
        }
        Item {
            id: readingArea
            Layout.fillWidth: true; Layout.fillHeight: true
            property bool hasTranscript: transcriptModel.count > 0 || win.reading
            function followLatest() { if (win.follow) followTimer.restart() }
            Timer {
                id: followTimer; interval: 32
                onTriggered: {
                    if (!win.follow) return
                    subtitles.forceLayout()
                    subtitles.positionViewAtEnd()
                }
            }
            ListModel { id: transcriptModel }
            function updateCaption(index) {
                let row = backend.captionAt(index)
                if (index < transcriptModel.count)
                    transcriptModel.set(index, { originalText: String(row.original), translatedText: String(row.translation) })
                win.follow = true
                followLatest()
            }
            function syncCaptions() {
                let count = backend.captionCount
                if (transcriptModel.count > count)
                    transcriptModel.remove(count, transcriptModel.count - count)
                for (let i = transcriptModel.count; i < count; ++i) {
                    let row = backend.captionAt(i)
                    transcriptModel.append({ originalText: String(row.original), translatedText: String(row.translation) })
                }
                win.follow = true
                followLatest()
            }
            Component.onCompleted: syncCaptions()
            Connections {
                target: backend
                function onCaptionsChanged() { readingArea.syncCaptions() }
                function onCaptionUpdated(index) { readingArea.updateCaption(index) }
            }
            Row {
                id: columnHeaders
                visible: readingArea.hasTranscript && !win.reading
                width: parent.width - 12; height: visible ? 32 : 0
                spacing: 40
                Label { width: (parent.width - 40) / 2; text: qsTr("Original"); color: theme.mutedForeground; font.pixelSize: 13 }
                Label { width: (parent.width - 40) / 2; text: qsTr("Translation"); color: theme.translationForeground; font.pixelSize: 13 }
            }
            Rectangle {
                visible: readingArea.hasTranscript
                x: (parent.width - 12) / 2; width: 1; height: parent.height; color: theme.border
            }
            ListView {
                id: subtitles
                anchors.top: columnHeaders.bottom; anchors.bottom: parent.bottom
                width: parent.width
                clip: true; spacing: 14
                model: transcriptModel
                onMovementStarted: win.follow = false
                onContentHeightChanged: readingArea.followLatest()
                onHeightChanged: readingArea.followLatest()
                onWidthChanged: readingArea.followLatest()
                ScrollBar.vertical: ScrollBar {
                    policy: win.reading ? ScrollBar.AlwaysOff : ScrollBar.AsNeeded
                    onPressedChanged: if (pressed) win.follow = false
                }
                delegate: Item {
                    required property string originalText
                    required property string translatedText
                    width: subtitles.width - 12
                    height: Math.max(sourceText.implicitHeight, translationText.implicitHeight)
                    onHeightChanged: readingArea.followLatest()
                    Text {
                        id: sourceText; objectName: "sourceCaptionText"; width: (parent.width - 40) / 2
                        text: originalText; textFormat: Text.PlainText; wrapMode: Text.Wrap
                        color: theme.foreground; font.pixelSize: win.subtitleSize; lineHeight: 1.12
                    }
                    Text {
                        id: translationText; objectName: "translatedCaptionText"; x: (parent.width + 40) / 2; width: (parent.width - 40) / 2
                        text: translatedText === "Translating locally…" ? "" : translatedText
                        textFormat: Text.PlainText; wrapMode: Text.Wrap
                        color: theme.translationForeground; font.pixelSize: win.subtitleSize; lineHeight: 1.12
                    }
                }
            }
            Column {
                anchors.centerIn: parent; width: parent.width * 0.9; spacing: 16
                visible: !readingArea.hasTranscript
                Label { anchors.horizontalCenter: parent.horizontalCenter; text: qsTr("Local live captions and translation"); color: theme.foreground; font.pixelSize: 32 }
                Label { width: parent.width; horizontalAlignment: Text.AlignHCenter; text: "Local real-time subtitles and translation"; color: theme.secondaryForeground; font.pixelSize: 17 }
                Label { width: parent.width; horizontalAlignment: Text.AlignHCenter; text: qsTr("An Omarchy app · Chinese / English"); color: theme.mutedForeground; font.pixelSize: 13 }
            }
        }
        Item {
            Layout.fillWidth: true; implicitHeight: 44
            ToolButton {
                visible: !win.reading
                anchors.left: parent.left; anchors.verticalCenter: parent.verticalCenter
                text: qsTr("Clear"); enabled: subtitles.count > 0 && !backend.recording && !backend.busy
                ToolTip.visible: hovered
                ToolTip.text: enabled ? qsTr("Clear captions") : qsTr("Stop captions before clearing")
                onClicked: clearDialog.open()
            }
            Row {
                anchors.centerIn: parent
                spacing: 10
                Rectangle {
                    width: 8; height: 8; radius: 4
                    anchors.verticalCenter: parent.verticalCenter
                    color: backend.error.length ? theme.error : backend.recording ? theme.positive : backend.busy ? theme.warning : theme.mutedForeground
                    Accessible.name: backend.status
                    ToolTip.visible: statusHover.containsMouse
                    ToolTip.text: backend.status + qsTr("\nPending: ") + backend.backlog + (win.follow ? "" : qsTr("\nEnd: follow latest"))
                    MouseArea { id: statusHover; anchors.fill: parent; anchors.margins: -8; hoverEnabled: true; acceptedButtons: Qt.NoButton }
                }
                Label { visible: !win.reading; text: backend.demo ? qsTr("Demo mode") : backend.error.length ? qsTr("Check settings") : microphone.count ? qsTr("Ready") : qsTr("Waiting for a microphone"); color: theme.mutedForeground; font.pixelSize: 12; anchors.verticalCenter: parent.verticalCenter }
                ToolButton {
                    objectName: "decreaseSubtitleSize"
                    visible: win.reading; enabled: win.subtitleSize > 18
                    implicitWidth: 36; implicitHeight: 40
                    text: "−"; font.pixelSize: 24
                    Accessible.name: qsTr("Decrease text size")
                    ToolTip.visible: hovered; ToolTip.text: qsTr("Decrease text size")
                    onClicked: win.changeSubtitleSize(-2)
                }
                Label {
                    objectName: "subtitleSizeLabel"
                    visible: win.reading; text: win.subtitleSize
                    width: 24; horizontalAlignment: Text.AlignHCenter
                    color: theme.secondaryForeground; font.pixelSize: 13
                    anchors.verticalCenter: parent.verticalCenter
                }
                ToolButton {
                    objectName: "increaseSubtitleSize"
                    visible: win.reading; enabled: win.subtitleSize < 72
                    implicitWidth: 36; implicitHeight: 40
                    text: "+"; font.pixelSize: 24
                    Accessible.name: qsTr("Increase text size")
                    ToolTip.visible: hovered; ToolTip.text: qsTr("Increase text size")
                    onClicked: win.changeSubtitleSize(2)
                }
            }
            Button {
                objectName: "stopCaptionsButton"
                visible: win.reading
                anchors.right: parent.right; anchors.verticalCenter: parent.verticalCenter
                implicitWidth: Math.max(136, contentItem.implicitWidth + leftPadding + rightPadding)
                implicitHeight: 40
                leftPadding: 20; rightPadding: 20
                enabled: backend.recording || backend.starting
                text: backend.starting ? qsTr("Cancel startup") : qsTr("Stop captions")
                font.pixelSize: 14; font.weight: Font.DemiBold
                Material.foreground: enabled ? theme.foreground : theme.mutedForeground
                background: Rectangle {
                    radius: 6
                    color: parent.down ? theme.selection : parent.hovered && parent.enabled ? theme.selection : "transparent"
                    border.width: 1
                    border.color: parent.enabled ? (parent.hovered ? theme.accent : theme.secondaryForeground) : theme.border
                }
                Accessible.name: text; ToolTip.visible: hovered; ToolTip.text: backend.starting ? qsTr("Cancel startup") : backend.recording ? qsTr("Stop captions · Space") : qsTr("Finishing pending captions")
                onClicked: backend.stop()
            }
            Button {
                visible: !win.reading
                anchors.right: parent.right; anchors.verticalCenter: parent.verticalCenter
                text: qsTr("Start captions"); highlighted: true; Material.foreground: theme.accentForeground; enabled: !backend.demo && !backend.busy && microphone.count > 0
                onClicked: win.toggleCapture()
            }
        }
    }
    component AppDialog: Dialog {
        id: dialogRoot
        parent: Overlay.overlay
        anchors.centerIn: parent
        modal: true
        padding: 24
        topPadding: 0
        bottomPadding: 0
        Material.theme: theme.lightMode ? Material.Light : Material.Dark
        Material.background: theme.panelBackground
        Material.foreground: theme.foreground
        Material.accent: theme.accent
        background: Rectangle { color: theme.panelBackground; radius: 14; border.color: theme.border }
        Overlay.modal: Rectangle { color: Qt.rgba(0, 0, 0, theme.lightMode ? 0.18 : 0.35) }
        header: Item {
            implicitHeight: 68
            Label { anchors.left: parent.left; anchors.leftMargin: 24; anchors.verticalCenter: parent.verticalCenter; text: dialogRoot.title; color: theme.foreground; font.pixelSize: 22 }
        }
        footer: Item {
            implicitHeight: dialogRoot.standardButtons === Dialog.NoButton ? 20 : 68
            Row {
                anchors.right: parent.right; anchors.rightMargin: 16
                anchors.verticalCenter: parent.verticalCenter; spacing: 8
                Button { visible: (dialogRoot.standardButtons & Dialog.Cancel) !== 0; text: qsTr("Cancel"); onClicked: dialogRoot.reject() }
                Button { visible: (dialogRoot.standardButtons & Dialog.Ok) !== 0; text: qsTr("OK"); onClicked: dialogRoot.accept() }
            }
        }
    }
    AppDialog {
        id: settings
        objectName: "settingsDialog"
        anchors.centerIn: parent; modal: true; title: qsTr("Settings")
        width: Math.min(win.width - 40, 540)
        standardButtons: Dialog.NoButton
        footer: Item {
            implicitHeight: 68
            Button { objectName: "settingsDone"; anchors.right: parent.right; anchors.rightMargin: 16; anchors.verticalCenter: parent.verticalCenter; text: qsTr("Done"); onClicked: settings.close() }
        }
        contentItem: ScrollView {
            id: settingsScroll
            objectName: "settingsScroll"
            implicitHeight: Math.min(settingsFields.implicitHeight, win.height - 180)
            contentWidth: availableWidth
            clip: true
            ColumnLayout {
            id: settingsFields
            width: settingsScroll.availableWidth; spacing: 14
            Label { text: qsTr("Interface language"); color: theme.secondaryForeground }
            ComboBox {
                id: interfaceLanguage
                objectName: "interfaceLanguage"
                Layout.fillWidth: true
                model: [qsTr("Follow Omarchy"), "简体中文", "English"]
                function syncSelection() { currentIndex = ["auto", "zh-CN", "en"].indexOf(translator.selection) }
                Component.onCompleted: syncSelection()
                onModelChanged: Qt.callLater(syncSelection)
                onActivated: function(index) { translator.setSelection(["auto", "zh-CN", "en"][index]) }
                Connections { target: translator; function onChanged() { Qt.callLater(interfaceLanguage.syncSelection) } }
            }
            Label { text: qsTr("Microphone"); color: theme.secondaryForeground }
            RowLayout {
                Layout.fillWidth: true
                ComboBox {
                    id: microphone; Layout.fillWidth: true; model: backend.devices; textRole: "label"
                    enabled: !backend.recording && !backend.busy && !backend.demo
                    displayText: count === 0 ? qsTr("No microphone available") : currentText
                }
                ToolButton { text: "↻"; enabled: !backend.recording && !backend.busy; onClicked: backend.refreshDevices() }
            }
            Label { text: qsTr("Spoken language"); color: theme.secondaryForeground }
            ComboBox { id: language; currentIndex: 0; Layout.fillWidth: true; model: [qsTr("Auto · Chinese ↔ English"), qsTr("English → Chinese"), qsTr("Chinese → English")]; enabled: !backend.recording && !backend.busy }
            RowLayout {
                Layout.fillWidth: true
                Label { text: qsTr("Microphone threshold"); color: theme.secondaryForeground }
                Item { Layout.fillWidth: true }
                Label { text: Math.round(threshold.value) + " dB"; color: theme.secondaryForeground }
            }
            Slider { id: threshold; Layout.fillWidth: true; from: -60; to: -25; value: -42; stepSize: 1; enabled: !backend.recording && !backend.busy }
            RowLayout {
                Layout.fillWidth: true
                Label { text: qsTr("Topic vocabulary · live updates"); color: theme.secondaryForeground; Layout.fillWidth: true; wrapMode: Text.Wrap }
            }
            Button { objectName: "vocabularyAgent"; text: qsTr("Discuss vocabulary with Agent"); onClicked: backend.openVocabularyAgent(translator.language) }
            Label { Layout.fillWidth: true; text: qsTr("The default Agent receives vocabulary paths, format rules and testing guidance. Tell it the topic and examples to adjust."); wrapMode: Text.Wrap; color: theme.mutedForeground; font.pixelSize: 12 }
            Label { Layout.fillWidth: true; text: backend.configStatus; textFormat: Text.PlainText; wrapMode: Text.Wrap; color: theme.secondaryForeground; font.pixelSize: 12 }
            Label { Layout.fillWidth: true; text: backend.error.length ? backend.error : backend.status; textFormat: Text.PlainText; wrapMode: Text.Wrap; color: backend.error.length ? theme.error : theme.mutedForeground; font.pixelSize: 12 }
            RowLayout {
                Button { text: qsTr("Export captions"); enabled: subtitles.count > 0 && !backend.demo; onClicked: backend.exportSession() }
                Button { text: qsTr("Clear captions"); enabled: subtitles.count > 0 && !backend.recording && !backend.busy; onClicked: clearDialog.open() }
                Item { Layout.fillWidth: true }
                ToolButton { text: qsTr("Help"); onClicked: help.open() }
            }
            }
        }
    }
    AppDialog {
        id: help; objectName: "helpDialog"; anchors.centerIn: parent; modal: true; title: qsTr("Help")
        width: Math.min(win.width - 40, 500); standardButtons: Dialog.Ok
        contentItem: ScrollView {
            id: helpScroll; contentWidth: availableWidth; clip: true
            implicitHeight: Math.min(helpText.implicitHeight, win.height - 180)
            Label {
                id: helpText; width: helpScroll.availableWidth; wrapMode: Text.Wrap; color: theme.foreground
                text: qsTr("Space  Start / stop captions\nF11  Full screen    Esc  Return to window\nCtrl+,  Settings    Ctrl+S  Export\nPageUp / PageDown  Review    End  Follow latest\nQ  Quit    ?  Help\n\nOriginal text updates about every 2 seconds. Translation begins after a pause of about 0.9 seconds; continuous speech is split after 18 seconds. Chinese and English are detected automatically.\n\nAudio is not saved. Captions are saved only when exported. Open the default Agent from Settings to discuss topic vocabulary. Valid changes apply to subsequent tasks; invalid changes keep the previous configuration.")
            }
        }
    }
    AppDialog {
        id: errorDialog; objectName: "errorDialog"; anchors.centerIn: parent; modal: true; title: qsTr("Caption service")
        width: Math.min(win.width - 40, 480); standardButtons: Dialog.Ok
        Label { width: parent.width; text: backend.error; textFormat: Text.PlainText; wrapMode: Text.Wrap }
    }
    AppDialog {
        id: clearDialog; objectName: "clearDialog"; anchors.centerIn: parent; modal: true; title: qsTr("Clear these captions?")
        standardButtons: Dialog.Ok | Dialog.Cancel
        Label { text: qsTr("Unexported captions will be lost. Press Ctrl+S to export first.") }
        onAccepted: backend.clear()
    }
    AppDialog {
        id: quitDialog; objectName: "quitDialog"; anchors.centerIn: parent; modal: true; title: qsTr("Quit captions?")
        standardButtons: Dialog.Ok | Dialog.Cancel
        Label { text: qsTr("Recording and pending tasks will stop. Unexported captions will be lost.\nCancel to stop recording, finish pending tasks and export first.") }
        onAccepted: { win.allowClose = true; win.close() }
    }
}
