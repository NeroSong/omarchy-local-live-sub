# System integration and regression checks

This application remains opt-in. Installing its launcher does **not** enable a
boot-time service, record audio, or download models. The user prepares models and
starts captions explicitly. A VM validates installation and application behavior;
it does not establish microphone accuracy or real-time performance on physical
hardware.

## 1. Reliability and private data

- Startup can be cancelled with the stop button, Space, or window close. A late
  health response cannot acquire the microphone after cancellation.
- A supervisor holds its instance lock until its model children have exited.
  Launch requests are serialized; a stale or foreign PID is never signalled.
- ASR has a 10-second absolute request-read deadline, bounded connection/upload
  capacity and a single inference transaction. Health checks do not wait for
  inference. Overlapping inference and overload return HTTP 503 with Retry-After,
  rather than creating an unbounded queue. Slow uploads return 408 or disconnect.
- Model calls are not forcibly interrupted inside native inference libraries.
  The UI still applies its network timeout and preserves completed captions.
- Application directories are private (0700) and logs, state and exported
  captions are private (0600). Symlink/hardlink-sensitive writes fail closed.
  An explicit export/settings override must use an appropriate private directory;
  the UI does not chmod an unrelated existing shared directory.

The localhost services require a bearer token. `bin/models-start` writes a fresh
random token to `$XDG_STATE_HOME/live-sub/api-token` (0600) for each run and removes it
on exit; the ASR adapter and `llama-server` (`--api-key-file`) reject requests without it,
so other local users and web pages cannot submit audio or read configuration. The token is
passed by file path, never on a command line. llama.cpp keeps `/health` public; it reveals
no meeting data. This does not isolate the services from other processes running as the
same user, which can read the token file. Fixed ports can still be occupied by another
user/process; the app reports that conflict rather than terminating an unrelated process.
Do not expose the ports through a public proxy.

## 2. Read-only code, per-user storage

Defaults honor the standard XDG base directories. The paths below assume those
variables are unset:

| Purpose | Default | Optional override |
| --- | --- | --- |
| Vocabulary and topic folders | `~/.config/live-sub/` | `LIVE_SUB_CONFIG_DIR` |
| Exported sessions | `~/.local/share/live-sub/sessions/` | `LIVE_SUB_DATA_DIR`, or `LIVE_SUB_SESSIONS` |
| Logs, process state, UI settings | `~/.local/state/live-sub/` | `LIVE_SUB_STATE_DIR`, or `LIVE_SUB_UI_SETTINGS` |
| Model downloads | `~/.cache/live-sub/models/` | `LIVE_SUB_CACHE_DIR`; existing model overrides remain supported |
| Python environments, engine sources/builds | `~/.local/share/live-sub/runtime/` | `LIVE_SUB_RUNTIME_DIR` |

`LIVE_SUB_ROOT` selects read-only program resources, not the user's configuration.
`SENSEVOICE_MODEL_DIR` and `TRANSLATION_MODEL_PATH` retain their existing meanings.
Use dedicated application directories for path overrides, not a home directory
or a general-purpose shared folder.

Inspect effective paths without creating files:

```sh
./bin/live-sub-launch --paths
# Installed package:
live-sub --paths
```

Source launchers still require their checkout. The **installed package does not**:
its public launchers live in `/usr/bin`, its Qt binary in `/usr/lib/live-sub`,
and its scripts/defaults in `/usr/share/live-sub`. It contains no build-machine
checkout pointer. Models are downloaded per user and are not bundled in the
application package.

## 3. Non-destructive migration and upgrades

On the first source launch, missing user configuration, topic folders, settings
and sessions are copied from the old checkout. Existing destinations are never
overwritten and original files are never deleted. Subsequent editing should use
the active XDG configuration directory displayed in settings, not the old
checkout's defaults. An explicit `LIVE_SUB_CONFIG_DIR` remains authoritative and
is not populated with default vocabulary.

For a package installed after a source version:

```sh
# Copy missing user data only: no downloads or builds.
live-sub --setup --migrate-from /absolute/path/to/old/checkout --migrate-only
# Then prepare the per-user engine and verified models.
live-sub --setup
# Explicit alternative for a CPU-only machine:
live-sub --setup --cpu
```

Existing legacy or Voxtype models can be checksum-verified and reused read-only.
Keep the legacy checkout while it is being reused. To make it removable, prepare
independent cached models first; do not delete the only model copy. Python venvs
and native builds are not automatically moved because they contain absolute
interpreter/build paths. Setup builds a fresh runtime at the destination instead.
An explicit runtime override is available for deliberate in-place reuse.

Before upgrades or changing configuration directories, stop recording, drain
queued work, export anything needed, and stop model services. Package upgrades
replace code/default resources; existing per-user vocabulary and sessions stay
unchanged. New default vocabulary does not silently replace custom vocabulary.

## 4. Package install and removal

On an Arch/Omarchy build machine with the documented dependencies:

```sh
cd pkgbuild
makepkg -s
```

Install the resulting package using the normal package manager. As the ordinary
desktop user, run `live-sub --setup` once, then `live-sub --settings`.
`live-sub --doctor --services --json`, `live-sub --status`, and `live-sub --stop`
are the installed equivalents of the source commands.

Uninstalling the package removes its program files and system launcher only.
It intentionally retains per-user configuration, sessions, models and runtime.
Stop models before uninstalling. If the source installation also created a
per-user desktop entry, remove that entry separately. Review and back up
`live-sub --paths` before deciding whether to delete any retained personal data;
there is no automatic destructive cleanup.

## 5. Model-free regression suite

Qt6 development/QML packages and Python with NumPy are required. The configured
hotword venv is used for decoder tests when present; alternatively select a
Python interpreter with `LIVE_SUB_TEST_PYTHON`.

```sh
./bin/build
./bin/test-all
```

Source build, setup, tests and launch use the same default output:
`$LIVE_SUB_RUNTIME_DIR/ui-build/<hash of canonical checkout path>/`. This avoids
both writes to read-only source trees and accidental reuse of another checkout's
binary. `LIVE_SUB_BUILD_DIR` overrides that directory consistently; set it for
both build and launch. A missing override binary fails instead of falling back.
Rebuild once after upgrading from the old `build/` or unscoped `runtime/ui-build`
layout. Those old files remain untouched but are no longer selected. Arch package
builds explicitly select their own `build/` and installed launches use the packaged binary.
Without a display the Qt tests use offscreen/software rendering. For demo screenshots, `live-sub --demo --screenshot` saves a private PNG to the state directory (`preview.png`), or to `LIVE_SUB_SCREENSHOT_PATH`; it never writes into installed resources. The tests cover:

- Real QML window-close/cancel-button paths and both ASR/translation startup
  phases, cancelled callbacks, repeated toggles and deliberate restart.
- Caption export content and 0700/0600 permissions under a permissive umask.
- Model-free ASR HTTP half bodies, deadlines, concurrent health, disconnects,
  upload/worker capacity and consistent configuration snapshots.
- Supervisor ownership/single-instance cleanup using disposable fake children.
- Packaged launch without the original checkout, separate user profiles,
  no-overwrite migration, upgrade/removal preservation and interrupted-source
  fetch recovery using local fixtures.
- Existing segmentation, WAV, terminology, translation/SSE, theme, language and
  synthetic decoder tests.

The tests do not download large models or touch a real microphone. They are not
substitutes for clean-machine package-manager acceptance or physical-device
inference tests.

## 6. Release acceptance on target hardware

Before enabling this by default for a broader system distribution:

1. Install/upgrade/remove on a clean Omarchy image; verify launcher, offline
   restart, missing-model guidance, and retained data with two ordinary users.
2. Exercise both UI languages and light/dark themes; cancel startup, repeat start,
   close during startup, stop while work is queued, export and restore.
3. Test microphone unplug/reconnect and service crash/timeout with real devices.
4. Run a fixed public Chinese/English corpus with negation, numbers, technical
   names, mixed speech and ordinary non-hotword phrases. Compare greedy and
   hotword recognition, including false insertions.
5. Measure cold start, idle/peak RSS, CPU/GPU use, queue growth and end-to-end p95
   latency during a sustained session on minimum supported hardware.

Do not equate a TCG VM, a healthy endpoint, demo captions or a serial six-second
benchmark with real-time microphone acceptance.
