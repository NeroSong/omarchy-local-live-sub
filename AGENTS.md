# Guidance for installation and maintenance agents

This repository is a C++ / Qt Quick application for local Chinese–English microphone captions on x86_64 Omarchy / Arch Linux. Start with README.md and INSTALL.md. The primary handoff target is a Beelink Intel Core Ultra 9 285H desktop using Intel Vulkan graphics. Do not assume another developer's model paths or installed Voxtype are present.

## Installation workflow

1. Inspect the checkout and preserve local changes, topic vocabularies and sessions. Work as the normal desktop user. Use sudo only for prerequisite packages listed in INSTALL.md, never for bootstrap, pip or the application.
2. Run `./bin/doctor --preflight --json`. Resolve actual missing prerequisites before running `./bin/setup --desktop`. Omit `--desktop` for development-only work. Do not build an Arch package unless requested.
3. The setup scripts download pinned, checksum-verified models if missing, create isolated Python environments and compile locally. Do not bypass checksum errors, swap quantizations, copy native binaries from another CPU, or overwrite other apps' models. Existing model checks are read-only. Interrupted downloads can be retried; do not redownload valid cached models for verification.
4. Run `./bin/start --settings`, then `./bin/doctor --services --json`. `./bin/smoke-test --json` probes real local inference without recording. These steps do not replace real microphone acceptance: ask the desktop user to select a microphone and speak English and Chinese, then confirm original text, translation, stopping and export. Report untested steps explicitly. For a documentation/code-only review, do not launch a full installation or redownload models.
5. If setup fails, report the stage, relevant error and log path. Do not claim installation success because a build, demo window or health endpoint worked. Do not silently fall back to CPU or a different recognizer; follow the explicit fallback in INSTALL.md.

## System and data boundaries

- Keep Voxtype, Fcitx/Rime, PipeWire, default audio devices, microphone gain, global shortcuts, desktop configuration and unrelated processes unchanged. This app captures independently and has no translated audio playback.
- Services listen only on 127.0.0.1:8178 and :8179. `bin/models-stop` checks the project's supervisor identity. Do not free ports by broadly killing Python, ffmpeg or llama processes.
- Models, runtime environments, logs, builds and sessions are local artifacts. Do not commit them or send meeting content to external services. The optional vocabulary Agent receives config guidance, not automatic meeting transcripts.
- Before restarting services, stop capture, wait for queued work and let the user export anything needed. Preserve glossary.json, asr-config.json and topics/. Hot updates are documented in README.md; retain `come` protection and do not change the deliberate Omarchy forced-correction policy without user direction.
- The desktop launcher points to this checkout. Keep it in place or rerun setup after moving. Closing the window leaves model services running. Stopped captions close without confirmation; pending work drains before exit, and unexported content is not automatically persisted.

## Development checks

Use `rg` to find code. Build with `./bin/build`, run Qt tests with `./bin/test`, installation regressions with `python3 tests/setup_tests.py`, and decoder regressions with `runtime/hotword-venv/bin/python tests/hotword_decoder_tests.py` when that environment is available. Installation regressions use tiny fake downloads, not model downloads. Scope checks to the change; do not repeat full downloads or full installation unless explicitly requested.

Keep UI strings in the Translator/localization system and colors in Theme; check both English/Chinese and dark/light presentation when changing UI. Preserve separate original/translation displays, real-time font adjustment and raw recognition/correction metadata.

Do not commit or publish unless requested. If publishing is explicitly authorized, use SSH for Git push; do not fall back to HTTPS after an SSH failure.
