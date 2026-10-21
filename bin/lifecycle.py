"""Private lifecycle state and identity checks shared by the launcher/supervisor."""
import fcntl
import json
import os
from pathlib import Path
import stat
import tempfile


def private_directory(path):
    path = Path(path)
    path.mkdir(mode=0o700, parents=True, exist_ok=True)
    info = path.lstat()
    if not stat.S_ISDIR(info.st_mode) or info.st_uid != os.getuid():
        raise RuntimeError(f'Refusing unsafe private directory: {path}')
    path.chmod(0o700)
    return path


def private_open(path, mode='a'):
    path = Path(path)
    private_directory(path.parent)
    flags = os.O_CREAT | os.O_NOFOLLOW | os.O_NONBLOCK | (os.O_RDWR if '+' in mode else os.O_WRONLY)
    flags |= os.O_APPEND if 'a' in mode else 0
    fd = os.open(path, flags, 0o600)
    try:
        info = os.fstat(fd)
        if not stat.S_ISREG(info.st_mode) or info.st_uid != os.getuid() or info.st_nlink != 1:
            raise RuntimeError(f'Refusing unsafe private file: {path}')
        os.fchmod(fd, 0o600)
        if 'w' in mode:
            os.ftruncate(fd, 0)
        return os.fdopen(fd, mode)
    except BaseException:
        os.close(fd)
        raise


def acquire_lock(path, blocking=True):
    guard = private_open(path, 'a+')
    try:
        fcntl.flock(guard, fcntl.LOCK_EX | (0 if blocking else fcntl.LOCK_NB))
        return guard
    except BaseException:
        guard.close()
        raise


def atomic_json(path, value):
    path = Path(path)
    private_directory(path.parent)
    if path.is_symlink():
        raise RuntimeError(f'Refusing symlink state file: {path}')
    fd, temporary = tempfile.mkstemp(prefix='.' + path.name + '.', dir=path.parent)
    try:
        with os.fdopen(fd, 'w') as output:
            os.fchmod(output.fileno(), 0o600)
            json.dump(value, output)
            output.write('\n')
            output.flush()
            os.fsync(output.fileno())
        os.replace(temporary, path)
    finally:
        Path(temporary).unlink(missing_ok=True)


def process_stat(pid):
    # comm in /proc/PID/stat can contain spaces and parentheses.
    fields = Path(f'/proc/{pid}/stat').read_text().rsplit(')', 1)[1].split()
    return fields[0], fields[19]


def identity(root, config):
    return {'pid': os.getpid(), 'start_ticks': process_stat(os.getpid())[1],
            'root': str(Path(root).resolve()), 'config_directory': str(Path(config).resolve())}


def read_owned(path, root, config=None):
    """Fail closed on stale, malformed or foreign records. Never signal by PID alone."""
    try:
        info = Path(path).lstat()
        if not stat.S_ISREG(info.st_mode) or info.st_uid != os.getuid() or info.st_nlink != 1:
            return None
        record = json.loads(Path(path).read_text())
        pid = record['pid']
        if type(pid) is not int or pid <= 1:
            return None
        root = Path(root).resolve()
        if record.get('root') != str(root):
            return None
        if config is not None and record.get('config_directory') != str(Path(config).resolve()):
            return None
        process = Path(f'/proc/{pid}')
        state, ticks = process_stat(pid)
        if state == 'Z' or ticks != record['start_ticks'] or process.stat().st_uid != os.getuid():
            return None
        cwd = process.joinpath('cwd').resolve()
        arguments = process.joinpath('cmdline').read_bytes().split(b'\0')
        if not any((cwd / os.fsdecode(arg)).resolve() == root / 'bin/models-start' for arg in arguments if arg):
            return None
        return record
    except (OSError, ValueError, TypeError, KeyError, IndexError):
        return None


def remove_owned(path, record):
    try:
        if not Path(path).is_symlink() and json.loads(Path(path).read_text()) == record:
            Path(path).unlink()
    except (OSError, ValueError):
        pass
