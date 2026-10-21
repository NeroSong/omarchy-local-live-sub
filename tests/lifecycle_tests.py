"""Lifecycle regressions with sleeping fake children; no models or inference."""
import json
import os
from pathlib import Path
import shutil
import socket
import runpy
import signal
import subprocess
import sys
import tempfile
import time
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'bin'))
from lifecycle import atomic_json, private_open, read_owned


def free_port():
    with socket.socket() as probe:
        probe.bind(('127.0.0.1', 0))
        return probe.getsockname()[1]


class LifecycleTests(unittest.TestCase):
    def fake_runtime(self, root):
        (root / 'bin').mkdir()
        for name in ('models-start', 'models-stop', 'model_assets.py', 'lifecycle.py', 'app_paths.py'):
            shutil.copy2(ROOT / 'bin' / name, root / 'bin' / name)
        (root / 'asr-config.json').write_text('{"decoder":"greedy"}')
        (root / 'runtime/sensevoice-venv/bin').mkdir(parents=True)
        (root / 'runtime').chmod(0o700)
        (root / 'runtime/sensevoice-venv/bin/python').symlink_to(sys.executable)
        (root / 'models/sensevoice-small-fp32').mkdir(parents=True)
        for name in ('model.onnx', 'tokens.txt'):
            (root / 'models/sensevoice-small-fp32' / name).write_bytes(b'fake')
        (root / 'models/translation.gguf').write_bytes(b'fake')
        sleeper = '#!' + sys.executable + '\nimport os, time\nprint(os.getpid(), flush=True)\ntime.sleep(120)\n'
        (root / 'bin/sensevoice-server').write_text(sleeper)
        llama = root / 'runtime/llama.cpp/build/bin/llama-server'
        llama.parent.mkdir(parents=True); llama.write_text(sleeper); llama.chmod(0o755)
        env = {k:v for k,v in os.environ.items() if not k.startswith(('LIVE_SUB_', 'SENSEVOICE_', 'TRANSLATION_'))}
        # Never depend on, or collide with, a developer's real services on 8178/8179.
        env.update(LIVE_SUB_ASR_PORT=str(free_port()), LIVE_SUB_TRANSLATION_PORT=str(free_port()), LIVE_SUB_ROOT=str(root), LIVE_SUB_CONFIG_DIR=str(root), LIVE_SUB_STATE_DIR=str(root / 'state'), LIVE_SUB_DATA_DIR=str(root / 'data'), LIVE_SUB_CACHE_DIR=str(root / 'cache'), LIVE_SUB_RUNTIME_DIR=str(root / 'runtime'), SENSEVOICE_MODEL_DIR=str(root / 'models/sensevoice-small-fp32'), TRANSLATION_MODEL_PATH=str(root / 'models/translation.gguf'))
        return env

    def test_private_atomic_files_and_unsafe_links(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            old = os.umask(0)
            try:
                target = root / 'private/state.json'
                atomic_json(target, {'pid': 1})
                self.assertEqual(target.stat().st_mode & 0o777, 0o600)
                self.assertEqual(target.parent.stat().st_mode & 0o777, 0o700)
                victim = root / 'victim'; victim.write_text('untouched')
                link = target.parent / 'log'; link.symlink_to(victim)
                with self.assertRaises(OSError): private_open(link, 'w')
                link.unlink(); os.link(victim, link)
                with self.assertRaises(RuntimeError): private_open(link, 'w')
                self.assertEqual(victim.read_text(), 'untouched')
            finally:
                os.umask(old)

    def test_foreign_and_malformed_records_are_not_owned(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / 'state.json'
            for record in ({'pid': os.getpid(), 'start_ticks':'0'}, {'pid':-1}, {'pid':'bad'}, []):
                path.write_text(json.dumps(record))
                self.assertIsNone(read_owned(path, ROOT))

    def test_supervisor_singleton_and_owned_cleanup(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            env = self.fake_runtime(root)
            first = subprocess.Popen([sys.executable, str(root / 'bin/models-start'), '--cpu'], env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
            try:
                state = root / 'state/models-process.json'
                deadline = time.monotonic() + 8
                while not state.exists() and first.poll() is None and time.monotonic() < deadline:
                    time.sleep(0.02)
                self.assertTrue(state.exists(), 'Fake supervisor failed: ' + (first.stdout.read() if first.poll() is not None else 'timed out'))
                record = json.loads(state.read_text())
                self.assertEqual(record['pid'], first.pid)
                second = subprocess.run([sys.executable, str(root / 'bin/models-start'), '--cpu'], env=env, capture_output=True, text=True, timeout=8)
                self.assertNotEqual(second.returncode, 0)
                self.assertIn('already running', second.stderr)
                self.assertEqual(json.loads(state.read_text()), record)
                for path in (state, root / 'state/logs/asr.log', root / 'state/logs/translation.log'):
                    self.assertEqual(path.stat().st_mode & 0o777, 0o600)
                stop = subprocess.run([sys.executable, str(root / 'bin/models-stop')], env=env, capture_output=True, text=True, timeout=15)
                self.assertEqual(stop.returncode, 0, stop.stderr)
                self.assertEqual(first.wait(timeout=5), 0)
                self.assertFalse(state.exists())
                for name in ('asr', 'translation'):
                    pid = int((root / f'state/logs/{name}.log').read_text().strip())
                    self.assertFalse(Path(f'/proc/{pid}').exists(), 'Owned child was not reaped')
            finally:
                if first.poll() is None:
                    first.send_signal(signal.SIGTERM)
                    first.wait(timeout=15)
                first.stdout.close()

    def test_invalid_port_override_is_rejected_before_any_child_starts(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary); env = self.fake_runtime(root)
            env['LIVE_SUB_ASR_PORT'] = '70000'
            run = subprocess.run([sys.executable, str(root / 'bin/models-start'), '--cpu'], env=env, capture_output=True, text=True, timeout=8)
            self.assertNotEqual(run.returncode, 0)
            self.assertIn('ports', run.stderr)
            self.assertFalse((root / 'state/models-process.json').exists())

    def test_restart_tolerates_time_wait_but_not_a_live_listener(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary); env = self.fake_runtime(root)
            port = int(env['LIVE_SUB_ASR_PORT'])
            with socket.socket() as server:
                server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
                server.bind(('127.0.0.1', port)); server.listen()
                client = socket.create_connection(('127.0.0.1', port)); peer, _ = server.accept()
                busy = subprocess.run([sys.executable, str(root / 'bin/models-start'), '--cpu'], env=env, capture_output=True, text=True, timeout=8)
                self.assertNotEqual(busy.returncode, 0)
                self.assertIn('port is unavailable', busy.stderr)
                self.assertFalse((root / 'state/models-process.json').exists())
                peer.close(); client.close()  # server-side close leaves TIME_WAIT on the port
            first = subprocess.Popen([sys.executable, str(root / 'bin/models-start'), '--cpu'], env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
            try:
                state = root / 'state/models-process.json'
                deadline = time.monotonic() + 8
                while not state.exists() and first.poll() is None and time.monotonic() < deadline:
                    time.sleep(0.02)
                self.assertTrue(state.exists(), 'Supervisor refused a port that only had TIME_WAIT')
            finally:
                if first.poll() is None:
                    first.send_signal(signal.SIGTERM); first.wait(timeout=15)
                first.stdout.close()

    def test_second_child_spawn_failure_cleans_first_child_and_lock(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary); env = self.fake_runtime(root)
            main = runpy.run_path(str(ROOT / 'bin/models-start'))['main']
            children = []; real_popen = subprocess.Popen
            def spawn(*args, **kwargs):
                if children:
                    raise OSError('fixture second-child spawn failure')
                child = real_popen(*args, **kwargs); children.append(child)
                return child
            handlers = {sig:signal.getsignal(sig) for sig in (signal.SIGINT, signal.SIGTERM)}
            try:
                with patch.dict(os.environ, env, clear=True), patch.object(sys, 'argv', ['models-start', '--cpu']), patch('subprocess.Popen', side_effect=spawn):
                    with self.assertRaisesRegex(OSError, 'fixture second-child'):
                        main()
                self.assertEqual(len(children), 1)
                self.assertIsNotNone(children[0].poll())
                self.assertFalse((root / 'state/models-process.json').exists())
                from lifecycle import acquire_lock
                with acquire_lock(root / 'state/models.lock', blocking=False):
                    pass
            finally:
                for sig, handler in handlers.items(): signal.signal(sig, handler)
                for child in children:
                    if child.poll() is None: child.kill(); child.wait()


if __name__ == '__main__':
    unittest.main()
