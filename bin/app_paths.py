"""Read-only application resources, private XDG user data, and non-destructive migration."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import stat
import tempfile

from lifecycle import private_directory

CONFIG_FILES = ('glossary.json', 'asr-config.json', 'hotwords.candidates.json')


def _path(value):
    return Path(value).expanduser().resolve()


def _user_path(value):
    # Preserve the final component for private_directory's symlink check.
    # Resolving it first could chmod an unrelated target before validation.
    return Path(os.path.abspath(Path(value).expanduser()))


def copy_missing(source, destination):
    """Copy regular files without replacing existing files, even under concurrent setup."""
    source, destination = Path(source), Path(destination)
    if source.is_symlink() or not source.is_file() or destination.exists() or destination.is_symlink():
        return False
    private_directory(destination.parent)
    fd, temporary = tempfile.mkstemp(prefix='.' + destination.name + '.', dir=destination.parent)
    try:
        with os.fdopen(fd, 'wb') as output, source.open('rb') as incoming:
            os.fchmod(output.fileno(), 0o600)
            shutil.copyfileobj(incoming, output)
            output.flush()
            os.fsync(output.fileno())
        try:
            os.link(temporary, destination)
        except FileExistsError:
            return False
        return True
    finally:
        Path(temporary).unlink(missing_ok=True)


def copy_tree_missing(source, destination):
    if source.is_symlink() or not source.is_dir():
        return
    for entry in source.iterdir():
        if entry.is_symlink():
            continue
        target = destination / entry.name
        if target.is_symlink():
            continue
        if entry.is_dir():
            if not target.exists() or target.is_dir():
                private_directory(target)
                copy_tree_missing(entry, target)
        else:
            copy_missing(entry, target)


class AppPaths:
    def __init__(self, root=None, environ=None, home=None):
        env = os.environ if environ is None else environ
        self.environ = env
        self.home = Path.home() if home is None else Path(home)
        self.root = _path(env.get('LIVE_SUB_ROOT') or root or Path(__file__).resolve().parent.parent)
        def user_path(name, xdg, fallback):
            return _user_path(env.get(name) or Path(env.get(xdg) or self.home / fallback) / 'live-sub')
        self.config = user_path('LIVE_SUB_CONFIG_DIR', 'XDG_CONFIG_HOME', '.config')
        self.data = user_path('LIVE_SUB_DATA_DIR', 'XDG_DATA_HOME', '.local/share')
        self.state = user_path('LIVE_SUB_STATE_DIR', 'XDG_STATE_HOME', '.local/state')
        self.cache = user_path('LIVE_SUB_CACHE_DIR', 'XDG_CACHE_HOME', '.cache')
        self.runtime = _user_path(env.get('LIVE_SUB_RUNTIME_DIR') or self.data / 'runtime')
        self.models = self.cache / 'models'
        self.logs = self.state / 'logs'
        self.token = self.state / 'api-token'
        self.packaged = (self.root / 'package.json').is_file()
        supplied = env.get('LIVE_SUB_LEGACY_ROOT') or env.get('LIVE_SUB_PROJECT')
        if not supplied and not self.packaged and (self.root / 'live-sub.pro').is_file():
            supplied = self.root
        if not supplied:
            try:
                supplied = json.loads((self.data / 'legacy-source.json').read_text()).get('root')
            except (OSError, ValueError, AttributeError):
                pass
        self.legacy = _path(supplied) if supplied else None
        private_keys = (('data', 'LIVE_SUB_DATA_DIR'), ('state', 'LIVE_SUB_STATE_DIR'), ('cache', 'LIVE_SUB_CACHE_DIR'), ('runtime', 'LIVE_SUB_RUNTIME_DIR'))
        names = env.get('LIVE_SUB_EXPLICIT_PRIVATE')
        self.explicit_private_names = names.split(',') if names is not None else [name for name, key in private_keys if env.get(key)]
        self.explicit_private = {getattr(self, name) for name, _ in private_keys if name in self.explicit_private_names}
        self.explicit_config = env.get('LIVE_SUB_CONFIG_EXPLICIT', '1' if env.get('LIVE_SUB_CONFIG_DIR') else '0') == '1'

    @property
    def build(self):
        # One authoritative output for build, setup, tests and source launch.
        # Separate checkouts must never silently launch each other's executable.
        identity = hashlib.sha256(os.fsencode(self.root)).hexdigest()[:16]
        return _path(self.environ.get('LIVE_SUB_BUILD_DIR') or self.runtime / 'ui-build' / identity)

    @property
    def binary(self):
        if self.environ.get('LIVE_SUB_UI_BINARY'):
            return _path(self.environ['LIVE_SUB_UI_BINARY'])
        if self.packaged:
            return self.root.parents[1] / 'lib/live-sub/live-sub'
        return self.build / 'live-sub'

    def values(self):
        return {name:str(value) for name,value in (
            ('LIVE_SUB_ROOT', self.root), ('LIVE_SUB_CONFIG_DIR', self.config),
            ('LIVE_SUB_DATA_DIR', self.data), ('LIVE_SUB_STATE_DIR', self.state),
            ('LIVE_SUB_CACHE_DIR', self.cache), ('LIVE_SUB_RUNTIME_DIR', self.runtime))}

    def export(self):
        os.environ.update(self.values())
        os.environ['LIVE_SUB_EXPLICIT_PRIVATE'] = ','.join(self.explicit_private_names)
        os.environ['LIVE_SUB_CONFIG_EXPLICIT'] = '1' if self.explicit_config else '0'
        # Avoid bytecode writes beside installed, read-only Python resources.
        os.environ['PYTHONDONTWRITEBYTECODE'] = '1'
        if self.legacy:
            os.environ['LIVE_SUB_LEGACY_ROOT'] = str(self.legacy)

    def initialize(self):
        os.umask(0o077)
        for path in (self.data, self.state, self.cache, self.runtime, self.models, self.logs):
            if path.resolve() in (self.home.resolve(), Path('/'), self.root):
                raise RuntimeError(f'Use a dedicated private application directory, not {path}')
            if path in self.explicit_private and path.exists():
                info = path.stat()
                if info.st_uid != os.getuid() or stat.S_IMODE(info.st_mode) & 0o077:
                    raise RuntimeError(f'Explicit application directory must already be private (0700); no permissions were changed: {path}. Choose a dedicated subdirectory instead.')
            private_directory(path)
        if not self.explicit_config or not self.config.exists():
            private_directory(self.config)
        elif not self.config.is_dir():
            raise RuntimeError(f'Configuration directory is not a directory: {self.config}')
        legacy = self.legacy
        if legacy and legacy != self.data:
            record = self.data / 'legacy-source.json'
            if not record.exists():
                # Atomic, no-overwrite migration marker, just like user configuration.
                fd, temporary = tempfile.mkstemp(dir=self.data)
                try:
                    with os.fdopen(fd, 'w') as output:
                        json.dump({'root':str(legacy)}, output)
                    copy_missing(Path(temporary), record)
                finally:
                    Path(temporary).unlink(missing_ok=True)
            copy_missing(legacy / 'runtime/ui.ini', self.state / 'ui.ini')
            copy_tree_missing(legacy / 'sessions', self.data / 'sessions')
            if not self.explicit_config:
                for name in CONFIG_FILES:
                    copy_missing(legacy / name, self.config / name)
                copy_tree_missing(legacy / 'topics', self.config / 'topics')
        # An explicit topic directory is authoritative; never seed or chmod its files.
        if not self.explicit_config:
            for name in CONFIG_FILES:
                copy_missing(self.root / name, self.config / name)
        self.export()
