# 安装与现场使用 / Installation handoff

适用对象：零刻（Beelink）Intel Core Ultra 9 285H、Intel 集成显卡、带麦克风的 x86_64 Omarchy / Arch Linux 主机。其他设备需单独确认驱动；不依赖 NPU。以下默认使用 Vulkan 翻译、CPU 识别。

This guide is also the installation contract for an AI agent. Read `AGENTS.md` and `README.md`, run commands as the logged-in desktop user, and report failures accurately. The application supports Chinese and English UI and speech. This is a source checkout installation, without creating an Arch package.

## 发给 AI 的话 / Prompt to give your AI

把仓库链接和下面这段话发给目标机器上的 AI 即可：

> 请在这台零刻 Ultra 9 285H 的 Omarchy 主机上安装和启动 https://github.com/NeroSong/omarchy-local-live-sub 。先阅读仓库 README.md、INSTALL.md 和 AGENTS.md，按说明安装缺少的依赖，使用默认 Vulkan 模式，添加当前用户的应用启动入口。模型不存在时由脚本下载并校验，不依赖本机装有 Voxtype。不要修改系统语音输入、默认音频设备或全局桌面配置。先完成诊断，再打开真实字幕窗口让我选择麦克风并试说中英文；不要把演示模式或健康检查当作试音成功。如果失败，请报告具体阶段、错误和下一步。

目标机器上的 AI 需要能执行终端命令；只有网页聊天的 AI 可按本页逐步指导操作。

## 1. 系统准备 / Prerequisites

- x86_64 Omarchy / Arch Linux，Python **3.12–3.14**，能进入当前用户的桌面会话。
- 内存建议 32 GB，至少预留 16 GB；安装前建议有 20 GB 空闲磁盘。实际占用随构建缓存变化。
- 首次准备需要访问 GitHub、Hugging Face、PyPI，下载模型约 2.85 GB，另外还需依赖和源码。准备完成后，字幕识别和翻译可离线运行。
- 内置扬声器不是必要条件：当前应用只显示字幕，没有译文语音播放。

只为系统依赖使用 sudo；不要使用 `sudo ./bin/setup` 或以 root 启动应用。Intel 设备安装缺少的包：

```bash
sudo pacman -S --needed base-devel git cmake python python-pip qt6-base qt6-declarative qt6-wayland qt6-svg ffmpeg libpulse libnotify mesa vulkan-intel vulkan-icd-loader vulkan-tools shaderc
```

不要为本应用调整 Voxtype、Fcitx/Rime、PipeWire 配置、默认音频源、系统服务或全局快捷键。若 pacman 报版本、镜像或数据库问题，先说明具体错误并按这台 Omarchy 的维护方式处理，不要用单独 `pacman -Sy` 制造部分升级。

## 2. 获取与准备 / Prepare the checkout

选择长期保留的位置。公开仓库可直接 HTTPS 克隆，无需 GitHub 账号：

```bash
mkdir -p ~/Work
cd ~/Work
git clone https://github.com/NeroSong/omarchy-local-live-sub.git
cd omarchy-local-live-sub
./bin/doctor --preflight --json
./bin/setup --desktop
```

已有检出时先查看本地修改；不要覆盖现场词表、字幕或未提交内容，也不要重复克隆到嵌套目录。`setup` 会检查依赖、准备模型、创建项目独立 Python 环境、编译推理引擎和应用、运行本地测试，最后创建当前用户的桌面入口。它不会安装系统包或创建安装包。开发时可省略 `--desktop`。

**模型来源和安全恢复：**

- 默认识别模型是锁定版本的 [SenseVoice FP32](https://huggingface.co/csukuangfj/sherpa-onnx-sense-voice-zh-en-ja-ko-yue-2024-07-17)，翻译模型是 [Hy-MT2-1.8B Q8_0](https://huggingface.co/tencent/Hy-MT2-1.8B-GGUF)。提交、文件、大小和 SHA256 在 `dependencies.lock.json` 中。
- 模型不存在时自动下载到 `models/`。完整的项目内识别模型优先；其次只读复用 Voxtype 同名模型，并校验内容。没有 Voxtype 不影响安装。
- 如果现有 Voxtype 模型与锁定版本不同，保留它，运行 `./bin/setup --desktop --download-asr` 下载项目独立模型。若设置过 `SENSEVOICE_MODEL_DIR`，先明确检查并取消该覆盖；脚本不会往其目录下载。
- 下载中断后重新执行原命令，会尝试续传 `.part`；服务器不支持续传则重新下载该文件。完整模型校验通过才替换最终文件；已校验模型不会再次下载。SHA256 不一致时停止，不得跳过校验、随意替换量化版本或使用来源不明的镜像。
- 网络受限时保留错误和进度，由机器使用者配置可用网络/代理后重试；不需要为公开模型提供令牌。不要把代理密码或其他凭据写入仓库。

若只需提前准备模型，使用 `./bin/bootstrap --models-only`，无需编译。完整准备仍需执行 `setup`。

用户入口指向当前检出目录的 `bin/start`；保留这个目录。移动后在新位置重新运行 `./bin/setup --desktop`。不应复制其他机器的虚拟环境或本机 CPU 优化过的二进制；应在目标机器编译。

## 3. 启动与验收 / Start and accept

```bash
./bin/start --settings
```

也可从应用启动器搜索 **Omarchy Local Live Sub / 本地双语字幕**。首次加载模型需要等待。启动器退出后模型服务保留，下次打开可复用。

在同一桌面用户的终端检查：

```bash
./bin/doctor --services --json
./bin/smoke-test --json
```

`doctor` 检查依赖、模型文件、麦克风枚举和服务；需要重新核对已有文件散列时加 `--verify-models`（只读取文件，不重新下载）。`smoke-test` 用合成静音请求识别，再用两句公开例句请求中英互译，不录麦克风、不下载模型。它能验证推理接口，不能验证现场语音质量。

请让使用者完成下面的实际试用，AI 应报告哪些已完成、哪些待使用者确认：

1. 在设置选择实际麦克风；不要选择音响的 monitor / 系统回放。切换界面语言为 English 或简体中文，确认设置和提示可读。
2. 点击“开始字幕”，以实际距离、实际音量分别说一句英文和中文，停顿后确认左侧原文和右侧译文都出现且方向正确。再试一句包含 Omarchy、Beelink、Hyprland 的真实讨论内容。
3. 确认停止后不再收音，队列完成；全屏及底部字号 − / + 可用。确认深浅主题下字幕清晰。
4. 导出一次所需字幕；停止收音后关闭窗口无需二次确认。未主动导出的内容不会自动保存；停止后还有任务时，关闭会等待处理结束。

请提前完成安装和麦克风试用，再进入正式沟通。应用目前用能量门限断句；多人同时说话、回声、远距离和噪声会影响结果。调整收音门限、靠近麦克风和轮流说话后再比较，不应只凭静音测试宣布现场验收成功。

## 4. 现场使用 / On site

- 说话语言默认 Auto，也可固定英语/中文；这与界面语言独立。
- 词表有识别热词、识别后纠错和翻译映射三层。设置里的“打开 Agent 沟通词表”会把配置路径和格式交给 Omarchy 默认 Agent，语言跟随当前界面选择。详见 README 的“现场调整与主题配置”。
- 此按钮要求 Omarchy 默认 Agent 已安装、已配置；缺少 Agent 不影响字幕运行。Agent 的提供方可能联网，应用不会主动将字幕、录音或日志放入提示。若用户手工提供例句，应自行决定是否分享。
- `glossary.json` 中 Omarchy 强制近音规则有意较激进，可能把普通短语改成 Omarchy；需查看 `raw` 和纠错记录确认。`come` 的保留规则应继续保留，未经现场使用者决定不要改动这项策略。
- 词表有效修改会热更新；无效修改保留进程内上一份有效配置。修改解码模式/线程数或切换整个配置目录需要停止并重启项目模型服务。已完成字幕不会被追溯重写。
- 字幕只在主动导出后写入 `sessions/`；`runtime/logs/` 也可能包含识别文字。不要上传真实会议数据到 Git 或排错帖子。

## 5. 排查与回退 / Troubleshooting

| 现象 | 处理 |
| --- | --- |
| 下载失败/连接超时 | 查看报错的站点，修复网络后重跑原命令；已完成模型不重下，保留 `.part` 可续传。不要关闭校验。 |
| Python 版本或 wheel 不支持 | 使用 Python 3.12–3.14 创建本项目环境；不要修改系统 Python 链接或使用 `sudo pip`。 |
| Vulkan 不可用 | 检查 Intel 驱动和 `vulkaninfo --summary`，确认硬件设备可见；不要将 llvmpipe/lavapipe 软件设备当作 GPU。需要时采用下方显式 CPU 回退。 |
| 没有麦克风/Pulse connection refused | 在当前登录桌面用户的终端运行，确认物理麦克风已连接及 `pactl -f json list sources` 可见；SSH、容器或沙箱不可见不能直接证明主机故障。 |
| 窗口未出现 | 查看 `runtime/logs/ui.log`，核对桌面会话环境与 Qt Wayland 依赖。`--demo` 只能检查界面。 |
| 模型启动失败 | 查看 `runtime/logs/supervisor.log`、`asr.log`、`translation.log`；保留具体错误，不静默换模型。 |
| 8178/8179 端口占用 | `./bin/start --status` 检查归属。仅 `./bin/models-stop` 停止当前项目管理的服务；其他程序需要先确认归属，不能 `pkill` 所有 Python/llama 进程。 |
| 配置目录不一致/旧服务 | 停止收音、等待并导出，然后 `./bin/models-stop`，再 `./bin/start`。切换主题词表目录时设置相同的 `LIVE_SUB_CONFIG_DIR`。 |
| 术语修改未生效 | 检查设置中的词表版本/错误和识别服务健康信息；保证两个 JSON 文件完整合法，再用新的一句话验证。 |

显式 CPU 回退：先停止收音、等待队列完成并导出，再执行：

```bash
./bin/models-stop
./bin/setup --cpu --desktop
./bin/start
```

这会把翻译引擎重新构建为 CPU 模式并记录选择，速度需要重新试用。`bin/start` 没有 `--cpu` 选项；若只是临时禁用已构建 Vulkan 引擎的 GPU，可另开终端运行 `./bin/models-start --cpu`，待就绪后再启动应用。不要在原有字幕任务进行中切换。

无需服务时 `./bin/models-stop` 释放项目模型占用；关闭字幕窗口本身不停止常驻模型。用户入口可从 `~/.local/share/applications/live-sub.desktop` 删除（自定义 XDG_DATA_HOME 时在对应目录）；删除项目前先备份词表、`topics/` 和所需字幕。
