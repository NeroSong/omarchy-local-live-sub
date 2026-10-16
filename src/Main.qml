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
    color: "#101418"
    Material.theme: Material.Dark
    Material.accent: theme.accent
    property bool fullScreen: visibility === Window.FullScreen
    property bool previewReading: false
    property bool reading: backend.recording || backend.busy || previewReading
    property bool follow: true
    property int subtitleSize: fullScreen ? 42 : 32
    property bool allowClose: false
    property string lastError: ""

    function toggleCapture() {
        if (backend.recording) { backend.stop(); return }
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
        if (!allowClose && (backend.unsaved || backend.recording || backend.busy)) {
            close.accepted = false; quitDialog.open()
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
                color: "#eef2f5"; elide: Text.ElideRight; Layout.fillWidth: true
            }
            ToolButton { text: win.fullScreen ? "退出全屏" : "全屏"; onClicked: win.toggleFullScreen() }
            ToolButton {
                icon.source: "qrc:/settings.svg"; icon.width: 20; icon.height: 20; icon.color: "#a4aeb7"
                Accessible.name: "设置"; ToolTip.visible: hovered; ToolTip.text: "设置 · Ctrl+,"
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
                Label { width: (parent.width - 40) / 2; text: "原文"; color: "#8c9aa5"; font.pixelSize: 13 }
                Label { width: (parent.width - 40) / 2; text: "译文"; color: "#8bb7a4"; font.pixelSize: 13 }
            }
            Rectangle {
                visible: readingArea.hasTranscript
                x: (parent.width - 12) / 2; width: 1; height: parent.height; color: "#273139"
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
                        id: sourceText; width: (parent.width - 40) / 2
                        text: originalText; textFormat: Text.PlainText; wrapMode: Text.Wrap
                        color: "#f1f4f6"; font.pixelSize: win.subtitleSize; lineHeight: 1.12
                    }
                    Text {
                        id: translationText; x: (parent.width + 40) / 2; width: (parent.width - 40) / 2
                        text: translatedText === "正在本地翻译…" ? "" : translatedText
                        textFormat: Text.PlainText; wrapMode: Text.Wrap
                        color: "#b0dac8"; font.pixelSize: win.subtitleSize; lineHeight: 1.12
                    }
                }
            }
            Column {
                anchors.centerIn: parent; width: parent.width * 0.9; spacing: 16
                visible: !readingArea.hasTranscript
                Label { anchors.horizontalCenter: parent.horizontalCenter; text: "本地实时字幕和翻译"; color: "#eff3f5"; font.pixelSize: 32 }
                Label { width: parent.width; horizontalAlignment: Text.AlignHCenter; text: "Local real-time subtitles and translation"; color: "#a1adb7"; font.pixelSize: 17 }
                Label { width: parent.width; horizontalAlignment: Text.AlignHCenter; text: "一个 Omarchy App · 中文 / English"; color: "#71818d"; font.pixelSize: 13 }
            }
        }
        Item {
            Layout.fillWidth: true; implicitHeight: win.reading ? 36 : 44
            ToolButton {
                visible: !win.reading
                anchors.left: parent.left; anchors.verticalCenter: parent.verticalCenter
                text: "清空"; enabled: subtitles.count > 0 && !backend.recording && !backend.busy
                ToolTip.visible: hovered
                ToolTip.text: enabled ? "清空字幕" : "结束字幕后可清空"
                onClicked: clearDialog.open()
            }
            Row {
                anchors.centerIn: parent
                spacing: win.reading ? 22 : 10
                Rectangle {
                    width: 8; height: 8; radius: 4
                    anchors.verticalCenter: parent.verticalCenter
                    color: backend.error.length ? "#e59186" : backend.recording ? "#8ed8b5" : backend.busy ? "#dfbc79" : "#667681"
                    Accessible.name: backend.status
                    ToolTip.visible: statusHover.containsMouse
                    ToolTip.text: backend.status + "\n待处理 " + backend.backlog + (win.follow ? "" : "\nEnd：跟随最新")
                    MouseArea { id: statusHover; anchors.fill: parent; anchors.margins: -8; hoverEnabled: true; acceptedButtons: Qt.NoButton }
                }
                Label { visible: !win.reading; text: backend.demo ? "演示模式" : backend.error.length ? "需要检查设置" : microphone.count ? "准备就绪" : "等待麦克风"; color: "#7f909c"; font.pixelSize: 12; anchors.verticalCenter: parent.verticalCenter }
                ToolButton {
                    visible: win.reading
                    implicitWidth: 36; implicitHeight: 36
                    enabled: backend.recording
                    icon.source: "qrc:/stop.svg"; icon.width: 17; icon.height: 17; icon.color: hovered ? "#c4cdd4" : "#71828e"
                    Accessible.name: "结束字幕"; ToolTip.visible: hovered; ToolTip.text: backend.recording ? "结束字幕 · Space" : "正在完成剩余字幕"
                    onClicked: backend.stop()
                }
            }
            Button {
                visible: !win.reading
                anchors.right: parent.right; anchors.verticalCenter: parent.verticalCenter
                text: "开始字幕"; highlighted: true; enabled: !backend.demo && !backend.busy && microphone.count > 0
                onClicked: win.toggleCapture()
            }
        }
    }
    component AppDialog: Dialog {
        id: dialogRoot
        Material.theme: Material.Dark
        Material.background: "#192128"
        background: Rectangle { color: "#192128"; radius: 14; border.color: "#303c46" }
        Overlay.modal: Rectangle { color: "#55000000" }
        header: Rectangle {
            color: "#192128"; implicitHeight: 58
            Label { anchors.left: parent.left; anchors.leftMargin: 24; anchors.verticalCenter: parent.verticalCenter; text: dialogRoot.title; color: "#eef2f5"; font.pixelSize: 22 }
        }
    }
    AppDialog {
        id: settings
        anchors.centerIn: parent; modal: true; title: "设置"
        width: Math.min(win.width - 40, 480)
        Material.theme: Material.Dark
        background: Rectangle { color: "#192128"; radius: 14; border.color: "#303c46" }
        Overlay.modal: Rectangle { color: "#55000000" }
        header: Rectangle {
            color: "#192128"; implicitHeight: 58
            Label { anchors.left: parent.left; anchors.leftMargin: 24; anchors.verticalCenter: parent.verticalCenter; text: "设置"; color: "#eef2f5"; font.pixelSize: 22 }
        }
        standardButtons: Dialog.NoButton
        ColumnLayout {
            width: parent.width; spacing: 14
            Label { text: "麦克风"; color: "#a4aeb7" }
            RowLayout {
                Layout.fillWidth: true
                ComboBox {
                    id: microphone; Layout.fillWidth: true; model: backend.devices; textRole: "label"
                    enabled: !backend.recording && !backend.busy && !backend.demo
                    displayText: count === 0 ? "暂无麦克风" : currentText
                }
                ToolButton { text: "↻"; enabled: !backend.recording && !backend.busy; onClicked: backend.refreshDevices() }
            }
            Label { text: "说话语言"; color: "#a4aeb7" }
            ComboBox { id: language; currentIndex: 0; Layout.fillWidth: true; model: ["自动 · 中英互译", "英语 → 中文", "中文 → 英语"]; enabled: !backend.recording && !backend.busy }
            RowLayout {
                Layout.fillWidth: true
                Label { text: "收音门限"; color: "#a4aeb7" }
                Item { Layout.fillWidth: true }
                Label { text: Math.round(threshold.value) + " dB"; color: "#a4aeb7" }
            }
            Slider { id: threshold; Layout.fillWidth: true; from: -60; to: -25; value: -42; stepSize: 1; enabled: !backend.recording && !backend.busy }
            Label { Layout.fillWidth: true; text: backend.error.length ? backend.error : backend.status; textFormat: Text.PlainText; wrapMode: Text.Wrap; color: backend.error.length ? "#e59186" : "#81939f"; font.pixelSize: 12 }
            RowLayout {
                Button { text: "导出文字"; enabled: subtitles.count > 0 && !backend.demo; onClicked: backend.exportSession() }
                Button { text: "清空字幕"; enabled: subtitles.count > 0 && !backend.recording && !backend.busy; onClicked: clearDialog.open() }
                Item { Layout.fillWidth: true }
                ToolButton { text: "帮助"; onClicked: help.open() }
            }
            Button { Layout.alignment: Qt.AlignRight; text: "完成"; onClicked: settings.close() }
        }
    }
    AppDialog {
        id: help; anchors.centerIn: parent; modal: true; title: "使用说明"
        width: Math.min(win.width - 40, 500); standardButtons: Dialog.Ok
        Label { width: parent.width; wrapMode: Text.Wrap; color: "#d4dce2"; text: "Space  开始 / 结束字幕\nF11  全屏    Esc  返回窗口\nCtrl+,  设置    Ctrl+S  导出文字\nPageUp / PageDown  回看    End  跟随最新\nQ  退出    ?  帮助\n\n原文约每 2 秒更新并持续补全，停顿约 0.9 秒后整句翻译；连续发言最长 18 秒。默认自动检测中英文，译文逐步显示，两栏同步滚动。\n\n仅导出时保存文字，不保存录音。术语别名在 glossary.json 中编辑，下次开始生效。" }
    }
    AppDialog {
        id: errorDialog; anchors.centerIn: parent; modal: true; title: "字幕服务提示"
        width: Math.min(win.width - 40, 480); standardButtons: Dialog.Ok
        Label { width: parent.width; text: backend.error; textFormat: Text.PlainText; wrapMode: Text.Wrap }
    }
    AppDialog {
        id: clearDialog; anchors.centerIn: parent; modal: true; title: "清空本次字幕？"
        standardButtons: Dialog.Ok | Dialog.Cancel
        Label { text: "未导出的文字将丢失。可先按 Ctrl+S 导出。" }
        onAccepted: backend.clear()
    }
    AppDialog {
        id: quitDialog; anchors.centerIn: parent; modal: true; title: "退出字幕？"
        standardButtons: Dialog.Ok | Dialog.Cancel
        Label { text: "收音和待处理任务将停止。未导出的文字将丢失。\n可取消后结束收音，等待处理完成，再导出。" }
        onAccepted: { win.allowClose = true; win.close() }
    }
}
