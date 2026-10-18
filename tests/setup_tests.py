"""Installation regressions using temporary files and fake downloads; no system changes."""
import hashlib
import io
import json
from pathlib import Path
import runpy
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'bin'))
from model_assets import asr_directory, download_verified, ASR_FILES, ASR_REPO


class Response(io.BytesIO):
    def __init__(self, data, status=200, headers=None):
        super().__init__(data)
        self.status, self.headers = status, headers or {}


class SetupTests(unittest.TestCase):
    def test_fresh_machine_selects_bundled_model_without_voxtype(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary); home = root / 'fake-user'
            self.assertEqual(asr_directory(root, {}, home), root / 'models/sensevoice-small-fp32')
            local = home / '.local/share/voxtype/models/sensevoice-small-fp32'
            local.mkdir(parents=True)
            for name in ASR_FILES: (local / name).write_bytes(b'fixture')
            self.assertEqual(asr_directory(root, {}, home), local)
            self.assertEqual(asr_directory(root, {'SENSEVOICE_MODEL_DIR':str(root / 'custom')}, home), root / 'custom')

    def test_resume_works_when_server_ignores_range(self):
        data = b'complete pinned model'
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / 'model.onnx'; partial = path.with_suffix('.onnx.part')
            partial.write_bytes(data[:5])
            with patch('urllib.request.urlopen', return_value=Response(data)) as request:
                download_verified('https://example.test/model', path, hashlib.sha256(data).hexdigest(), len(data))
            self.assertEqual(request.call_args.args[0].get_header('Range'), 'bytes=5-')
            self.assertEqual(path.read_bytes(), data)

    def test_resume_validates_range_and_checksum_before_replacing_model(self):
        data = b'complete pinned model'
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / 'model.onnx'; path.write_bytes(b'existing model')
            path.with_suffix('.onnx.part').write_bytes(data[:5])
            with patch('urllib.request.urlopen', return_value=Response(data[5:], 206, {'Content-Range':f'bytes 5-{len(data)-1}/{len(data)}'})):
                download_verified('https://example.test/model', path, hashlib.sha256(data).hexdigest(), len(data))
            self.assertEqual(path.read_bytes(), data)
            with patch('urllib.request.urlopen', return_value=Response(b'wrong')):
                with self.assertRaises(RuntimeError):
                    download_verified('https://example.test/model', path, '0' * 64)
            self.assertEqual(path.read_bytes(), data)
            with patch('urllib.request.urlopen') as request:
                download_verified('https://example.test/model', path, hashlib.sha256(data).hexdigest())
                request.assert_not_called()

    def test_missing_asr_is_downloaded_to_project_not_user_voxtype(self):
        namespace = runpy.run_path(str(ROOT / 'bin/bootstrap'))
        prepare = namespace['prepare_asr']
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            fixtures = {name: name.encode() for name in ('model.onnx', 'tokens.txt', 'LICENSE')}
            lock = {ASR_REPO:{'revision':'pinned-test', 'files':{name:{'sha256':hashlib.sha256(data).hexdigest(),'size':len(data)} for name,data in fixtures.items()}}}
            (root / 'dependencies.lock.json').write_text(json.dumps(lock))
            def download(url, destination, expected, size):
                self.assertTrue(str(destination).startswith(str(root / 'models')))
                self.assertIn('/resolve/pinned-test/', url)
                destination.write_bytes(fixtures[destination.name])
            with patch.dict(prepare.__globals__, ROOT=root, asr_directory=lambda _:root / 'missing', download_verified=download), patch.dict('os.environ', {}, clear=True):
                record = {}; prepare(record)
            self.assertEqual(Path(record['active_asr']['directory']), root / 'models/sensevoice-small-fp32')
            self.assertTrue((root / 'models/sensevoice-small-fp32/tokens.txt').is_file())

    def test_user_launcher_points_to_checkout_including_spaces(self):
        namespace = runpy.run_path(str(ROOT / 'bin/setup'))
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary) / 'Checkout with spaces'
            namespace['install_desktop'](root, Path(temporary) / 'data')
            entry = Path(temporary) / 'data/applications/live-sub.desktop'
            text = entry.read_text()
            self.assertIn(f'Exec="{root}/bin/start"', text)
            self.assertNotIn('/usr/bin/live-sub', text)
            if shutil.which('desktop-file-validate'):
                result = subprocess.run(['desktop-file-validate', str(entry)], capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

if __name__ == '__main__': unittest.main()
