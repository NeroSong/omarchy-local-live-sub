# Installation and on-site use

**English** | [简体中文](INSTALL.zh-CN.md) · [Back to README](README.md)

This guide targets an x86_64 Omarchy / Arch Linux desktop with a microphone. The primary handoff machine is a Beelink Intel Core Ultra 9 285H with Intel integrated graphics. Other hardware needs separate driver and performance checks. Defaults are CPU recognition and Vulkan translation; no NPU is required.

This is a source checkout installation, without creating an Arch package. Agents should also read [AGENTS.md](AGENTS.md), run as the logged-in desktop user, and report failures and untested steps accurately.

## Ask your AI to install it

Give a terminal-capable agent the repository link and this prompt:

> Install and launch https://github.com/NeroSong/omarchy-local-live-sub on this Omarchy machine. Read README.md, INSTALL.md, and AGENTS.md first. Inspect the checkout and preserve local changes, vocabulary, and sessions. Run preflight, resolve missing prerequisites, use default Vulkan translation if supported, and add the current user's launcher entry. Use the scripts to download and verify missing models; do not assume Voxtype is installed. Preserve existing speech input, default audio devices, and desktop settings. Open the real caption window so I can select a microphone and try English and Chinese speech. Do not treat demo mode or healthy services as microphone acceptance. If anything fails, report the stage, error, log path, and next step.

A web-only chat assistant can guide you through these steps, but cannot execute them on your desktop.

## 1. Prerequisites

- x86_64 Omarchy / Arch Linux, Python **3.12–3.14**, and access to the current user's desktop session.
- 32 GB RAM recommended, at least 16 GB; allow 20 GB free disk space before setup. Build caches affect actual usage.
- First setup needs GitHub, Hugging Face, and PyPI access. Models total about 2.85 GB, plus dependencies and source code. Recognition and translation can run offline afterward.
- A microphone. Speakers are optional: the app displays text and does not play translated speech.

Use sudo only for system prerequisites. Never run setup, pip, or the app as root. For an Intel machine, install missing packages with:

```bash
sudo pacman -S --needed base-devel git cmake python python-pip qt6-base qt6-declarative qt6-wayland qt6-svg ffmpeg libpulse libnotify mesa vulkan-intel vulkan-icd-loader vulkan-tools shaderc
```

Keep Voxtype, Fcitx/Rime, PipeWire, default audio sources, system services, and global shortcuts unchanged. If pacman reports version, mirror, or database errors, resolve them using the machine's Omarchy maintenance process. Do not use a standalone `pacman -Sy`, which can leave a partial upgrade.

## 2. Prepare the checkout

Choose a directory you will keep. Public HTTPS cloning does not require a GitHub account:

```bash
mkdir -p ~/Work
cd ~/Work
git clone https://github.com/NeroSong/omarchy-local-live-sub.git
cd omarchy-local-live-sub
./bin/doctor --preflight --json
```

Resolve preflight errors before continuing:

```bash
./bin/setup --desktop
```

For an existing checkout, inspect local changes first; preserve vocabulary and captions, and avoid cloning another copy inside it. Setup checks prerequisites, prepares models and isolated Python environments, builds the inference engine and app locally, runs tests, and creates a launcher entry for the current user. It does not install system packages or create an Arch package. Omit `--desktop` for development-only use.

### Models and safe recovery

- Recognition uses pinned [SenseVoice FP32](https://huggingface.co/csukuangfj/sherpa-onnx-sense-voice-zh-en-ja-ko-yue-2024-07-17); translation uses [Hy-MT2-1.8B Q8_0](https://huggingface.co/tencent/Hy-MT2-1.8B-GGUF). Revisions, filenames, sizes, and SHA256 checksums are in [dependencies.lock.json](dependencies.lock.json).
- Missing models download to `models/`. A complete project-local recognition model takes priority. Otherwise, an existing Voxtype model can be verified and reused read-only. Voxtype is not required.
- If Voxtype's model differs from the pinned version, preserve it and run `./bin/setup --desktop --download-asr` to prepare a separate project model. If `SENSEVOICE_MODEL_DIR` is set, inspect and unset that override first; scripts do not download into that external directory.
- Rerun the same command after an interrupted download. Scripts attempt to resume `.part` files; a server without resume support requires that file to restart. Files become final only after checksum verification, and valid cached models are not downloaded again. Stop on checksum mismatches; do not bypass checks or substitute another quantization or an unknown mirror.
- For network failures, keep the error and progress, fix connectivity or the proxy, and retry. Public models require no token. Keep proxy passwords and other credentials out of the repository.

To prepare only models in advance, run `./bin/bootstrap --models-only`. Full setup is still required later.

The user launcher points to this checkout's `bin/start`. Keep the directory; after moving it, rerun `./bin/setup --desktop` at the new location. Build on the target machine rather than copying another CPU's optimized binaries or Python environments.

## 3. Start and verify

```bash
./bin/start --settings
```

Alternatively, search the launcher for **Omarchy Local Live Sub / 本地双语字幕**. Initial model loading takes time. Closing the window leaves model services available for reuse.

From a terminal in the same desktop session:

```bash
./bin/doctor --services --json
./bin/smoke-test --json
```

`doctor` checks dependencies, model files, microphone enumeration, and services. Add `--verify-models` to reread and verify existing model hashes without downloading them. `smoke-test` sends synthetic silence to recognition and two public example sentences to translation. It does not record the microphone or download models. These checks validate inference endpoints, not speech quality in the room.

Have the desktop user complete these checks and report which remain untested:

1. Select the physical microphone in settings, avoiding monitor/system playback sources. Try both English and Simplified Chinese UI and check readability.
2. Start captions and speak an English sentence and a Chinese sentence at the intended distance and volume. Pause, then confirm the original on the left and translation on the right, in the correct direction. Try a realistic sentence containing names such as Omarchy, Beelink, or Hyprland.
3. Confirm stopping releases capture and queued work finishes. Try fullscreen and the bottom font-size controls. Check readability in light and dark themes.
4. Export any captions needed. Once capture is stopped, closing requires no second confirmation; pending tasks drain before exit. Unexported captions are not saved automatically.

Finish installation and a microphone trial before the actual conversation. Pause detection uses an energy threshold. Overlapping voices, echo, distance, and noise affect results; adjust the app's capture threshold, move closer to the microphone, and take turns speaking before comparing results.

## 4. During a conversation

- Speech language defaults to Auto, with fixed English and Chinese options. UI language is independent.
- Vocabulary has three layers: recognition hotwords, corrections after recognition, and translation mappings. See [vocabulary tuning](README.md#make-it-understand-your-topic) or the [extended Chinese reference](README.zh-CN.md#现场调整与主题配置).
- **Open Agent to discuss vocabulary** in settings opens your configured Omarchy agent with configuration paths and format guidance, in the selected UI language. The agent must be installed and configured under Omarchy's Setup → Defaults → Agent. Missing an agent does not prevent captions.
- The agent provider may use online services. The app does not automatically add captions, recordings, or logs to its prompt; decide what examples you want to share manually.
- The glossary's default aggressive Omarchy rule may rewrite ordinary phrases. Check `raw` and correction records. Retain the `come` protection and discuss policy changes with the person using the app.
- Valid vocabulary edits reload during use; invalid edits retain the running process's last valid configuration. Changing decoder/thread settings or the configuration directory requires stopping and restarting project model services. Completed captions are not rewritten.
- Exports from this checkout are saved in `sessions/`. Logs in `runtime/logs/` may also contain recognition text. Keep real conversation data out of Git and public bug reports.

## 5. Troubleshooting

| Symptom | What to check |
| --- | --- |
| Download failure / timeout | Check the failing host, fix connectivity, and rerun the same command. Keep `.part` files for resume; valid completed models are reused. Keep checksum verification enabled. |
| Unsupported Python version or wheel | Use Python 3.12–3.14 for the project environments. Do not replace the system Python symlink or use `sudo pip`. |
| Vulkan unavailable | Check Intel drivers and `vulkaninfo --summary`. A hardware device must be visible; llvmpipe/lavapipe software devices do not establish GPU readiness. Use the explicit CPU fallback below if needed. |
| Missing microphone / Pulse connection refused | Run in the logged-in desktop user's terminal. Check the physical connection and `pactl -f json list sources`. Missing access in SSH, a container, or a sandbox does not prove a host fault. |
| Window does not appear | Check `runtime/logs/ui.log`, desktop-session environment, and Qt Wayland dependencies. Demo mode checks the UI only. |
| Model startup fails | Check `runtime/logs/supervisor.log`, `asr.log`, and `translation.log`. Preserve the specific error; do not silently switch models. |
| Ports 8178/8179 occupied | Check ownership with `./bin/start --status`. Use `./bin/models-stop` only for matching project-managed services. Do not broadly kill Python or llama processes. |
| Configuration-directory mismatch / old service | Stop capture, wait, export, run `./bin/models-stop`, then start again. Use the same `LIVE_SUB_CONFIG_DIR` for the selected topic. |
| Vocabulary edit has no effect | Check vocabulary revisions/errors in settings and recognition-service health. Save complete, valid JSON and test with a new utterance. |

### Explicit CPU fallback

Stop capture, wait for queued work, and export anything needed, then run:

```bash
./bin/models-stop
./bin/setup --cpu --desktop
./bin/start
```

This rebuilds translation for CPU and records the selection. Test performance again. `bin/start` has no `--cpu` option. To temporarily disable GPU use with an already-built Vulkan engine, instead run `./bin/models-start --cpu` in another terminal after stopping the previous services, wait for readiness, then open the app. Do not switch during an active caption session.

### Decoder fallback

The default `hotword_ctc` decoder is experimental. To explicitly try the previous sherpa-onnx recognition path, stop capture, wait for pending work, export, and run:

```bash
./bin/models-stop
# Wait for the previous model services to exit.
./bin/models-start --decoder greedy
```

Once both models are ready, run `./bin/run` in another terminal. Use `--decoder hotword_ctc` to return to the hotword path after stopping services again. The health response reports the actual decoder; failures never silently select a different recognizer.

## 6. Stop or remove

Use `./bin/models-stop` when you no longer need the models in memory. Closing the caption window alone leaves them running. Stop capture, drain queued work, and export first.

To remove the user launcher, delete `~/.local/share/applications/live-sub.desktop` (or its equivalent under a custom `XDG_DATA_HOME`). Before deleting the checkout, back up `glossary.json`, `asr-config.json`, `topics/`, and any needed captions.
