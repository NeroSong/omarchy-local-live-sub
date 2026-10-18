"""Shared model paths and checksum-verified downloads for clean installations."""
import hashlib
import os
from pathlib import Path
import shutil
import time
import urllib.request

ASR_REPO = 'csukuangfj/sherpa-onnx-sense-voice-zh-en-ja-ko-yue-2024-07-17'
ASR_FILES = ('model.onnx', 'tokens.txt')


def asr_directory(root, environ=None, home=None):
    environ = os.environ if environ is None else environ
    if environ.get('SENSEVOICE_MODEL_DIR'):
        return Path(environ['SENSEVOICE_MODEL_DIR']).expanduser().resolve()
    bundled = root / 'models/sensevoice-small-fp32'
    local = (Path.home() if home is None else home) / '.local/share/voxtype/models/sensevoice-small-fp32'
    for candidate in (bundled, local):
        if all((candidate / name).is_file() for name in ASR_FILES):
            return candidate
    return bundled


def checksum(path):
    digest = hashlib.sha256()
    with path.open('rb') as source:
        while block := source.read(4 * 1024 * 1024):
            digest.update(block)
    return digest.hexdigest()


def download_verified(url, destination, expected, size=None):
    """Resume when supported; a full response restarts the partial file safely."""
    if len(expected) != 64 or any(c not in '0123456789abcdef' for c in expected):
        raise ValueError('A SHA256 checksum is required before downloading')
    destination.parent.mkdir(parents=True, exist_ok=True)
    if destination.is_file() and checksum(destination) == expected:
        print(f'Verified {destination.name}', flush=True)
        return
    partial = destination.with_suffix(destination.suffix + '.part')
    if size and partial.exists() and partial.stat().st_size >= size:
        if partial.stat().st_size == size and checksum(partial) == expected:
            partial.replace(destination)
            return
        partial.unlink()
    for attempt in range(5):
        try:
            offset = partial.stat().st_size if partial.exists() else 0
            request = urllib.request.Request(url, headers={'Range': f'bytes={offset}-'} if offset else {})
            print(f'Downloading {destination.name}' + (f' (resuming at {offset} bytes)' if offset else ''), flush=True)
            with urllib.request.urlopen(request, timeout=120) as response:
                if response.status == 206:
                    if not response.headers.get('Content-Range', '').startswith(f'bytes {offset}-'):
                        raise RuntimeError('Server returned a different byte range; existing model was not replaced')
                    mode = 'ab' if offset else 'wb'
                elif response.status == 200:
                    mode = 'wb'
                else:
                    raise RuntimeError(f'Unexpected download status: {response.status}')
                with partial.open(mode) as output:
                    shutil.copyfileobj(response, output, length=4 * 1024 * 1024)
                    output.flush()
                    os.fsync(output.fileno())
            if size and partial.stat().st_size != size:
                raise OSError('Incomplete model download; rerun to resume')
            if checksum(partial) != expected:
                partial.unlink()
                raise RuntimeError(f'SHA256 mismatch for {destination.name}; existing model was not replaced')
            partial.replace(destination)
            print(f'Verified {destination.name}', flush=True)
            return
        except OSError:
            if attempt == 4:
                raise
            time.sleep(min(2 + attempt, 6))
