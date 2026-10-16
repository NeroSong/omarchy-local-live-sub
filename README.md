# Omarchy Local Live Sub

本地实时字幕和翻译，一个 Omarchy App。中文和英文互译，原文与译文左右分屏，支持全屏阅读。

Local real-time subtitles and translation. An Omarchy app for Chinese–English discussions, with original text and translation side by side.

## 当前方案

- **语音识别：SenseVoice Small FP32**，当前试用 ONNX Runtime + 热词 CTC 候选搜索，CPU 6 线程；原 sherpa-onnx 模式保留可回退。
- **翻译：Hy-MT2-1.8B Q8_0**，llama.cpp 常驻服务，默认 Vulkan 加速。
- **界面：C++ / Qt Quick**，跟随 Omarchy 的强调色。
- 默认自动识别中英文，使用识别结果的语言标签选择翻译方向；标签缺失时参考原文文字。也可固定英语或中文。
- 原文约每 2 秒用累计音频更新同一段；停顿约 0.9 秒后提交整段翻译，避免每个短片段独立翻译。连续发言最长 18 秒。
- 译文流式显示，两栏对齐并自动滚动。开始后只显示双栏文字、状态圆点和结束按钮。

当前版本已由用户在本机试用，识别速度和整段翻译效果得到改善。它仍使用能量门限判断停顿，长句、重叠发言、嘈杂全场和语言混说需要按实际环境调整；18 秒上限仍可能切开特别长的句子。

## 依赖与构建

面向 Omarchy / Arch Linux。需要 Qt 6、C++ 工具链、Python（含 venv / pip）、CMake、Git、ffmpeg 和 pactl。Vulkan 构建还需 Vulkan loader、驱动及 `glslc`。

```bash
./bin/build
./bin/run --demo
```

`--demo` 只展示示例文字，不采集音频，不运行模型。构建产物在 `build/`。

## 准备模型与服务

### 1. SenseVoice 模型

如果本机已有 Voxtype 的 `small-fp32`，默认只读复用：

```text
~/.local/share/voxtype/models/sensevoice-small-fp32/model.onnx
~/.local/share/voxtype/models/sensevoice-small-fp32/tokens.txt
```

也可自行准备 [sherpa-onnx 官方 SenseVoice FP32 模型](https://k2-fsa.github.io/sherpa/onnx/sense-voice/pretrained.html)，把 `model.onnx` 和 `tokens.txt` 放到项目 `models/sensevoice-small-fp32/`。项目内模型目录优先于 Voxtype 的目录。

其他位置可显式指定；准备与运行时都要设置该变量：

```bash
export SENSEVOICE_MODEL_DIR=/absolute/path/to/sensevoice-small-fp32
```

本项目不修改 Voxtype 配置，不控制其 daemon，也不使用其热键、剪贴板或键盘输出。

### 2. 安装项目内推理依赖并准备翻译模型

```bash
./bin/bootstrap
```

该脚本会检查已有 SenseVoice 模型，在独立的 `runtime/sensevoice-venv/` 和 `runtime/hotword-venv/` 安装固定版本的 Python 依赖，下载并校验翻译模型，编译 llama.cpp 和应用。它不安装系统包，不再下载或编译 Whisper。

- `requirements.txt` / `requirements-hotwords.txt` 分别固定原识别模式和热词识别模式的依赖版本。
- `dependencies.lock.json` 记录首次准备使用的 llama.cpp / SDK 提交，以及翻译模型版本与散列。
- 新源码检出使用锁定提交；已有 `runtime/` 源码目录保留，不自动更新。
- 翻译模型下载使用发布方 SHA256 校验，通过后才替换最终文件。
- 实际准备记录写入本地 `runtime/manifest.json`。

可选参数：

```bash
./bin/bootstrap --cpu          # 翻译使用 CPU，免 Vulkan 构建依赖
./bin/bootstrap --tools-only   # 跳过模型准备，安装推理依赖并编译
./bin/bootstrap --models-only  # 只检查 / 下载模型
```

准备可能下载数 GB 数据，需提前完成。运行期间只访问本机回环地址。

### 3. 启动

```bash
./bin/start
```

也可分开启动，便于查看服务日志：

```bash
# 终端一：等待 Both models ready
./bin/models-start

# 终端二
./bin/run
```

识别与翻译服务分别监听 `127.0.0.1:8178` 和 `127.0.0.1:8179`。端口占用时启动失败，不停止占用端口的其他服务。模型服务在关闭窗口后继续保留：

```bash
./bin/start --status
./bin/models-stop
```

停止脚本只操作身份匹配的项目服务进程。CPU 备用模式可使用 `./bin/models-start --cpu`。

## 热词解码实验模式

当前 `asr-config.json` 默认 `hotword_ctc`：只读使用同一份 SenseVoice FP32 ONNX 模型，读取模型内的 LFR / CMVN / 语言信息，使用 kaldi-native-fbank 提取特征，通过 ONNX Runtime 获取 CTC 概率，再进行热词增强的候选搜索。没有换用 PyTorch 模型，没有训练或修改已有模型文件。

首批十词：Omarchy、Hyprland、Quickshell、Omacom、Quattro、Linux、Arch、mise、Neovim、Rust。初始分值分别按高 / 中 / 低设为 6 / 3 / 1，尚未经过实际录音调优；这些值是解码加分，不是出现概率。配置修改后需要重启模型服务。

搜索默认保留 4 个候选，每帧考虑 8 个高概率 token 和可延续热词的 token。按完整单词边界加分，失败的半截热词在结束时没有奖励；超过声学差距限制的候选会被剪枝，避免纯靠热词硬造文字。它能提高候选优先级，不能保证所有近音都输出指定名称，也可能增加耗时或误识别，效果由用户试用确认。

为了比较和排查，导出字幕额外保留 `asr_decoder`、同一次模型推理的普通贪心结果 `asr_greedy`、`asr_inference_ms` 和 `asr_decode_ms`。无需额外一次推理，音频仍不落盘。`raw` 是热词解码结果，`original` 是经过术语规则兜底的显示文字。

切回之前的 sherpa-onnx 识别路径：先结束窗口收音，等待字幕处理结束并导出，再执行：

```bash
./bin/models-stop
# 等待原终端中的模型服务退出，再启动
./bin/models-start --decoder greedy
```

切回热词模式使用 `./bin/models-start --decoder hotword_ctc`。也可设置 `SENSEVOICE_DECODER=greedy` 后运行 `./bin/start`，但已有服务不会被该命令替换；必须先停止已有项目服务。

两种模式的识别服务健康信息均返回实际 `decoder`；热词模式还返回当前词表。服务无法启动或推理失败时明确报错，不静默切回普通识别。

技术接口依据：[SenseVoice 模型元数据](https://github.com/k2-fsa/sherpa-onnx/blob/master/sherpa-onnx/csrc/offline-sense-voice-model.cc)、[识别特征与解码处理](https://github.com/k2-fsa/sherpa-onnx/blob/master/sherpa-onnx/csrc/offline-recognizer-sense-voice-impl.h)。热词搜索由本项目实现，并非 sherpa-onnx 官方 SenseVoice 原生热词接口。

## 使用

在设置面板中选择全场麦克风、语言和收音门限，然后开始字幕。停止收音后会完成排队任务；处理失败或积压过多时会停止采集并保留已有文字。

| 快捷键 | 操作 |
| --- | --- |
| Space | 开始 / 结束字幕 |
| F11 / Esc | 进入 / 退出全屏 |
| Ctrl+, | 打开设置 |
| Ctrl+S | 导出文字快照 |
| PageUp / PageDown | 回看上一页 / 下一页 |
| Home / End | 最早内容 / 跟随最新 |
| ? | 帮助 |
| Q | 退出；有未保存内容或活动任务时确认 |

新识别结果或译文更新会自动跟随最新内容；可在两次更新之间手动回看。空闲时左下角显示“清空”，清空前确认。状态圆点：绿色表示收音，黄色表示处理，红色表示异常；悬停查看详情。

字幕导出到 `sessions/`，包含原始识别文字、术语修正后的文字、译文和纠错记录。恢复导出记录：

```bash
./bin/run --restore-session sessions/session-example.json
```

### 术语自动纠错

识别后、显示和翻译前执行本地规则纠错，不增加一次模型请求。原文预览和最终识别均应用相同规则；翻译读取修正后的原文，导出保留 `raw` 和 `corrections`，便于核对误改。

`glossary.json` 下次开始时加载，支持以下字段：

| 字段 | 行为 |
| --- | --- |
| `term` | 标准术语与拼写 |
| `aliases` | 已确认的错词，按词边界直接修复，如 `O much → Omarchy`、`hyper land → Hyprland` |
| `context_aliases` | 需要相关上下文才修复的近音词，包括中文音译 |
| `context` | 本句或最近三段原始识别中的相关词，如 Linux、桌面、Wayland |
| `fuzzy` | 可选，默认关闭；有相关上下文时允许长英文术语的一处增删、替换或相邻字母颠倒 |

默认仅为 Omarchy、Hyprland 开启相似拼写修复。不会将 `much`、`rest` 等普通词单独改成术语；多条同等规则无法确定目标时跳过。规则同时匹配原始文字，不连续猜改；代码片段、链接、邮箱、路径和长命令选项保持原样。中英文相邻时仍可匹配术语，不要求汉字间有空格。

**Omarchy 强制模式（当前启用）**：按用户指定优先保证名称统一，允许误改。该条目的 `force_phonetic: true` 会绕过上下文要求和普通代码保护，强制处理已知别名、`force_patterns` 中的宽松中英文近音模式，以及合并最多三个英文词后的粗略发音键匹配。`Oh much`、`Oh, matching`、`Oh, Martin`、`how much`、`No, match`、`Oh mary`、`Or or ma`、`O mar key` 和中文音译会统一为 Omarchy；`so much`、`too much` 也会强制替换，不再保留这些普通短语；monarchy、anarchy 也已作为强制别名纳入。发音键使用字母归并和宽松编辑距离，并不是音频级发音判断；未覆盖的语言或完全不同的识别文字仍可能漏掉。强制规则优先级高于其他术语规则；保留 `raw` 与纠错记录。`preserve` 列表中的完整词优先保留，大小写不敏感；当前包括 `come`。任何跨越该词的多词近音候选也禁止替换，避免把 `I come`、`Come on` 整段吞掉。将该开关设为 false 可恢复保守行为。以上普通词和代码保护说明只适用于未启用强制模式的词条。

例如 `O much 使用 Hyprlnd 作为 Linux 桌面` 会修正为 `Omarchy 使用 Hyprland 作为 Linux 桌面`。单独的 `Hyprlnd` 缺少上下文时保留，未知的近音错词也不会凭空猜测，可根据实际收音继续补充别名。SenseVoice 不使用 Whisper 式提示词；此机制在识别之后运行，翻译提示中仍保留标准名称。

## 数据与系统边界

音频和文本在本机处理，不上传会议内容。不修改默认音频源、麦克风增益、PipeWire、系统服务、全局快捷键或已有语音输入配置。应用通过独立 ffmpeg 进程采集所选麦克风，结束后释放。

音频仅保留在内存；主动导出才保存字幕。`runtime/logs/` 可能包含识别文字，应作为会议数据保护；启动服务时会覆盖对应日志。

Git 只收录代码、文档和依赖声明。模型、字幕记录、录音、日志、虚拟环境、上游源码和构建产物均不提交。

## 项目结构

```text
src/                    C++ 后端、Qt Quick 界面、图标
bin/                    构建、模型准备、启动和停止脚本
pkgbuild/               可选 Arch 包和桌面入口
tests/                  已有 Qt 单元测试
requirements*.txt       两种模式的 Python 推理依赖版本
asr-config.json         解码模式、热词和搜索参数
dependencies.lock.json  上游源码与翻译模型版本
glossary.json           专业术语表
```

模型、服务状态和数据目录 `models/`、`runtime/`、`build/`、`sessions/` 在首次使用时生成，不随仓库分发。

## 开发与可选安装

```bash
./bin/build
./bin/test
```

已有测试覆盖切段、PCM / WAV、术语、翻译方向、输出截断及 SSE 碎片解析。热词解码器另有无音频的回归用例：`runtime/hotword-venv/bin/python tests/hotword_decoder_tests.py`。当前会议环境的使用效果由用户试用确认；自动化检查不代表全场会议验收。

`./bin/benchmark speech-16k.wav --language auto` 可选测量模型处理耗时，它使用固定 6 秒片段，不能代表界面断句策略或采集端到端延迟；报告包含字幕文字，请勿提交。

`./bin/install` 构建并安装 Arch 包，添加桌面入口。安装包含应用二进制、启动入口、图标、文档和词表；从启动器打开时会自动检查并启动项目模型服务，再运行 `/usr/bin/live-sub`。安装模式默认导出到 `~/.local/share/live-sub/sessions/`。

模型、推理引擎和独立 Python 环境继续保留在本机项目目录，不复制进系统包。安装包记录构建时的项目路径，因此请保留该目录；迁移后重新安装，或通过 `LIVE_SUB_PROJECT=/new/project/path live-sub-launch` 指定位置。仅卸载应用包不会删除项目中的模型或字幕。

## 许可与上游

应用代码采用 MIT，详见 [LICENSE](LICENSE)。模型和第三方引擎分别遵循其上游许可证，不随本仓库分发。

- [SenseVoice / sherpa-onnx](https://k2-fsa.github.io/sherpa/onnx/sense-voice/index.html)
- [Hy-MT2-1.8B](https://huggingface.co/tencent/Hy-MT2-1.8B) / [GGUF 模型](https://huggingface.co/tencent/Hy-MT2-1.8B-GGUF)
- [llama.cpp](https://github.com/ggml-org/llama.cpp)
