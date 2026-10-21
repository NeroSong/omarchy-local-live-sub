"""Portable package, XDG profiles, upgrades and migration tests with fake artifacts only."""
import json
import os
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
from app_paths import AppPaths, CONFIG_FILES
from model_assets import asr_directory, translation_path


class InstallationTests(unittest.TestCase):
    def environment(self, base, **overrides):
        env = {k:v for k,v in os.environ.items() if not k.startswith(('LIVE_SUB_', 'XDG_', 'SENSEVOICE_', 'TRANSLATION_'))}
        env.update(HOME=str(base / 'home'), XDG_CONFIG_HOME=str(base / 'config'), XDG_DATA_HOME=str(base / 'data'), XDG_STATE_HOME=str(base / 'state'), XDG_CACHE_HOME=str(base / 'cache'))
        (base / 'home').mkdir(parents=True, exist_ok=True)
        env.update(overrides)
        return env

    def defaults(self, root):
        root.mkdir(parents=True)
        for name in CONFIG_FILES:
            (root / name).write_text('default ' + name)

    def test_source_build_test_and_launch_share_checkout_specific_output(self):
        with tempfile.TemporaryDirectory() as temporary:
            base = Path(temporary); source = base / 'checkout with spaces'; self.defaults(source)
            (source / 'live-sub.pro').touch()
            (source / 'bin').mkdir()
            for name in ('build', 'test', 'run', 'app_paths.py', 'lifecycle.py'):
                shutil.copy2(ROOT / 'bin' / name, source / 'bin' / name)
            tools = base / 'tools'; tools.mkdir()
            (tools / 'qmake6').write_text('#!/bin/sh\nprintf "%s\\n" "$PWD" >> "$BUILD_TRACE"\n')
            (tools / 'make').write_text('#!/bin/sh\nname=live-sub\ncase "$PWD" in */tests) name=live_sub_tests;; esac\nprintf \'#!/bin/sh\\nprintf fresh-build\' > "$name"\nchmod +x "$name"\n')
            for tool in tools.iterdir(): tool.chmod(0o755)
            env = self.environment(base, PATH=str(tools) + os.pathsep + os.environ['PATH'], BUILD_TRACE=str(base / 'build-trace'))
            paths = AppPaths(source, environ=env)
            for stale in (source / 'build/live-sub', paths.runtime / 'ui-build/live-sub'):
                stale.parent.mkdir(parents=True, exist_ok=True)
                stale.write_text('old binary')
            for script in ('build', 'test'):
                result = subprocess.run([str(source / 'bin' / script)], env=env, capture_output=True, text=True, timeout=10)
                self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual((base / 'build-trace').read_text().splitlines(), [str(paths.build), str(paths.build / 'tests')])
            result = subprocess.run([str(source / 'bin/run'), '--demo'], env=env, capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(result.stdout, 'fresh-build')
            self.assertEqual((source / 'build/live-sub').read_text(), 'old binary')
            self.assertEqual((paths.runtime / 'ui-build/live-sub').read_text(), 'old binary')
            other = AppPaths(base / 'different-checkout', environ=env)
            self.assertNotEqual(paths.binary, other.binary)
            self.assertFalse(other.binary.exists(), 'another checkout must not launch this build')
            override = dict(env, LIVE_SUB_BUILD_DIR=str(base / 'explicit-missing-build'))
            self.assertEqual(AppPaths(source, environ=override).binary, base / 'explicit-missing-build/live-sub')
            result = subprocess.run([str(source / 'bin/run'), '--demo'], env=override, capture_output=True, text=True, timeout=10)
            self.assertNotEqual(result.returncode, 0, 'missing explicit builds must not silently fall back')
            self.assertNotIn('fresh-build', result.stdout)

    def test_xdg_paths_are_private_and_profiles_are_isolated(self):
        with tempfile.TemporaryDirectory() as temporary:
            base = Path(temporary); resources = base / 'resources'; self.defaults(resources)
            all_paths = []
            for profile in ('alice', 'bob'):
                env = self.environment(base / profile)
                with patch.dict(os.environ, env, clear=True):
                    paths = AppPaths(resources); paths.initialize(); all_paths.append(paths)
                    (paths.config / 'glossary.json').write_text(profile)
                    for directory in (paths.config, paths.data, paths.state, paths.cache, paths.runtime, paths.logs):
                        self.assertEqual(directory.stat().st_mode & 0o777, 0o700)
                    self.assertEqual((paths.config / 'asr-config.json').stat().st_mode & 0o777, 0o600)
                    self.assertNotIn(str(resources), str(paths.runtime))
            self.assertEqual((all_paths[0].config / 'glossary.json').read_text(), 'alice')
            self.assertEqual((all_paths[1].config / 'glossary.json').read_text(), 'bob')
            self.assertFalse((resources / 'runtime').exists())

    def test_managed_directory_symlinks_fail_without_chmod_or_writes(self):
        with tempfile.TemporaryDirectory() as temporary:
            base = Path(temporary); resources = base / 'resources'; self.defaults(resources)
            for name in ('data', 'state', 'cache', 'runtime', 'config'):
                with self.subTest(directory=name):
                    profile = base / name
                    env = self.environment(profile)
                    with patch.dict(os.environ, env, clear=True):
                        paths = AppPaths(resources)
                        target = profile / 'unrelated'; target.mkdir(); target.chmod(0o755)
                        link = getattr(paths, name); link.parent.mkdir(parents=True, exist_ok=True)
                        link.symlink_to(target, target_is_directory=True)
                        with self.assertRaisesRegex(RuntimeError, 'unsafe private directory'):
                            AppPaths(resources).initialize()
                        self.assertEqual(target.stat().st_mode & 0o777, 0o755)
                        self.assertEqual(list(target.iterdir()), [])
                        self.assertTrue(link.is_symlink())
            # A symlink in the XDG parent is allowed; only the application-owned
            # final component must be a real, owned private directory.
            profile = base / 'parent-link'
            env = self.environment(profile)
            target = profile / 'actual-data'; target.mkdir(); target.chmod(0o755)
            Path(env['XDG_DATA_HOME']).symlink_to(target, target_is_directory=True)
            with patch.dict(os.environ, env, clear=True):
                paths = AppPaths(resources); paths.initialize()
                self.assertEqual(target.stat().st_mode & 0o777, 0o755)
                self.assertEqual(paths.data.stat().st_mode & 0o777, 0o700)
            alias = base / 'source-parent-alias'; alias.symlink_to(resources.parent, target_is_directory=True)
            resources.chmod(0o755)
            env['LIVE_SUB_DATA_DIR'] = str(alias / resources.name)
            with patch.dict(os.environ, env, clear=True):
                with self.assertRaisesRegex(RuntimeError, 'dedicated private application directory'):
                    AppPaths(resources).initialize()
            self.assertEqual(resources.stat().st_mode & 0o777, 0o755)

    def test_upgrade_preserves_edits_and_uninstall_retains_user_data(self):
        with tempfile.TemporaryDirectory() as temporary:
            base = Path(temporary); resources = base / 'resources'; self.defaults(resources)
            with patch.dict(os.environ, self.environment(base), clear=True):
                paths = AppPaths(resources); paths.initialize()
                edited = paths.config / 'glossary.json'; edited.write_text('user edited')
                session = paths.data / 'sessions/meeting.txt'; session.parent.mkdir(); session.write_text('saved captions')
                (resources / 'glossary.json').write_text('new package default')
                paths.initialize()
                self.assertEqual(edited.read_text(), 'user edited')
                shutil.rmtree(resources)
                self.assertEqual(edited.read_text(), 'user edited')
                self.assertEqual(session.read_text(), 'saved captions')

    def test_legacy_copy_is_no_clobber_and_models_are_reused_without_relocation(self):
        with tempfile.TemporaryDirectory() as temporary:
            base = Path(temporary); resources = base / 'resources'; self.defaults(resources)
            legacy = base / 'old checkout'; self.defaults(legacy)
            (legacy / 'runtime').mkdir(); (legacy / 'runtime/ui.ini').write_text('font=28')
            (legacy / 'runtime/sensevoice-venv').mkdir(); (legacy / 'runtime/sensevoice-venv/old-python').write_text('absolute shebang fixture')
            (legacy / 'sessions').mkdir(); (legacy / 'sessions/a.txt').write_text('original captions')
            (legacy / 'topics/topic').mkdir(parents=True); (legacy / 'topics/topic/glossary.json').write_text('topic edits')
            model = legacy / 'models/sensevoice-small-fp32'; model.mkdir(parents=True)
            for name in ('model.onnx', 'tokens.txt'): (model / name).write_bytes(b'fake model')
            (legacy / 'models/translation.gguf').write_bytes(b'fake translation')
            env = self.environment(base, LIVE_SUB_LEGACY_ROOT=str(legacy))
            with patch.dict(os.environ, env, clear=True):
                paths = AppPaths(resources); paths.initialize()
                self.assertEqual((paths.state / 'ui.ini').read_text(), 'font=28')
                self.assertEqual((paths.data / 'sessions/a.txt').read_text(), 'original captions')
                self.assertEqual((paths.config / 'topics/topic/glossary.json').read_text(), 'topic edits')
                self.assertFalse((paths.runtime / 'sensevoice-venv').exists())
                self.assertEqual(asr_directory(resources), model)
                self.assertEqual(translation_path(resources), legacy / 'models/translation.gguf')
                (paths.data / 'sessions/a.txt').write_text('new user copy')
                paths.initialize()
                self.assertEqual((paths.data / 'sessions/a.txt').read_text(), 'new user copy')
                self.assertEqual((legacy / 'sessions/a.txt').read_text(), 'original captions')
                self.assertTrue((legacy / 'runtime/sensevoice-venv/old-python').exists())
            env.pop('LIVE_SUB_LEGACY_ROOT')
            with patch.dict(os.environ, env, clear=True):
                self.assertEqual(asr_directory(resources), model)

    def test_explicit_topic_and_runtime_overrides_survive_exports(self):
        with tempfile.TemporaryDirectory() as temporary:
            base = Path(temporary); resources = base / 'resources'; self.defaults(resources)
            topic = base / 'topic'; topic.mkdir(mode=0o755); topic.chmod(0o755)
            (topic / 'glossary.json').write_text('topic')
            runtime = base / 'reused-runtime'; runtime.mkdir(mode=0o700); runtime.chmod(0o700); (runtime / 'marker').write_text('existing')
            env = self.environment(base, LIVE_SUB_CONFIG_DIR=str(topic), LIVE_SUB_RUNTIME_DIR=str(runtime))
            with patch.dict(os.environ, env, clear=True):
                paths = AppPaths(resources); paths.initialize()
                child = AppPaths(resources); child.initialize()
                self.assertEqual(child.config, topic)
                self.assertEqual(child.runtime, runtime)
                self.assertEqual(topic.stat().st_mode & 0o777, 0o755)
                self.assertFalse((topic / 'asr-config.json').exists())
                self.assertEqual((runtime / 'marker').read_text(), 'existing')

    def test_package_manifest_contains_no_checkout_dependency(self):
        with tempfile.TemporaryDirectory() as temporary:
            base = Path(temporary); source = base / 'disposable checkout'; shutil.copytree(ROOT, source, ignore=shutil.ignore_patterns('.git', 'build', 'runtime', '__pycache__'))
            (source / 'build').mkdir(); (source / 'build/live-sub').write_text('#!' + sys.executable + '\nimport json, os\nfrom pathlib import Path\nPath(os.environ["LIVE_SUB_STATE_DIR"], "executed-ui.json").write_text(json.dumps({key:value for key,value in os.environ.items() if key.startswith("LIVE_SUB_")}))\n')
            if not (source / 'SYSTEM-INTEGRATION.md').exists():
                (source / 'SYSTEM-INTEGRATION.md').write_text('Integration fixture')
            package = base / 'package'
            command = 'source "$1/PKGBUILD"; startdir="$1"; pkgdir="$2"; package'
            result = subprocess.run(['bash', '-c', command, 'fixture', str(source / 'pkgbuild'), str(package)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            shutil.rmtree(source)
            moved = base / 'moved'; package.rename(moved)
            launcher = moved / 'usr/bin/live-sub'; resources = moved / 'usr/share/live-sub'
            self.assertFalse((resources / 'project-root').exists())
            for name in ('app_paths.py', 'lifecycle.py', 'model_assets.py', 'hotword_decoder.py', 'sensevoice-server', 'bootstrap', 'setup', 'asr_service.py'):
                self.assertTrue((resources / 'bin' / name).is_file(), name)
            for profile in ('alice', 'bob'):
                env = self.environment(base / profile)
                for arguments in (['--help'], ['--setup', '--help'], ['--doctor', '--help'], ['--paths']):
                    result = subprocess.run([str(launcher), *arguments], env=env, capture_output=True, text=True, timeout=10)
                    self.assertEqual(result.returncode, 0, result.stderr)
                    self.assertNotIn(str(source), result.stdout)
                self.assertFalse((base / profile / 'config/live-sub').exists(), 'help must not mutate user state')
                legacy = base / profile / 'legacy checkout'
                self.defaults(legacy)
                (legacy / 'runtime').mkdir(); (legacy / 'runtime/ui.ini').write_bytes(b'legacy settings')
                (legacy / 'sessions').mkdir(); (legacy / 'sessions/caption.txt').write_bytes(b'legacy private captions')
                result = subprocess.run([str(launcher), '--setup', '--migrate-only', '--migrate-from', str(legacy)], env=env, capture_output=True, text=True, timeout=10)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual((base / profile / 'state/live-sub/ui.ini').read_bytes(), b'legacy settings')
                self.assertEqual((base / profile / 'data/live-sub/sessions/caption.txt').read_bytes(), b'legacy private captions')
                self.assertEqual((legacy / 'sessions/caption.txt').read_bytes(), b'legacy private captions')
                result = subprocess.run([str(launcher), '--demo'], env=env, capture_output=True, text=True, timeout=10)
                self.assertEqual(result.returncode, 0, result.stderr)
                with patch.dict(os.environ, env, clear=True):
                    paths = AppPaths(resources)
                    self.assertEqual(paths.binary, moved / 'usr/lib/live-sub/live-sub')
                    executed = json.loads((paths.state / 'executed-ui.json').read_text())
                    self.assertEqual(executed['LIVE_SUB_CONFIG_DIR'], str(paths.config))
                    self.assertEqual(executed['LIVE_SUB_ROOT'], str(resources))
                    (paths.config / 'glossary.json').write_bytes(b'user vocabulary edits')
                    (paths.state / 'ui.ini').write_bytes(b'font=31')
                (resources / 'glossary.json').write_text('upgraded defaults')
                result = subprocess.run([str(launcher), '--demo'], env=env, capture_output=True, text=True, timeout=10)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual((paths.config / 'glossary.json').read_bytes(), b'user vocabulary edits')
                self.assertEqual((paths.state / 'ui.ini').read_bytes(), b'font=31')
            self.assertFalse((resources / '__pycache__').exists())
            self.assertFalse((resources / 'bin/__pycache__').exists())
            self.assertFalse((resources / 'runtime').exists())
            shutil.rmtree(moved)
            for profile in ('alice', 'bob'):
                self.assertEqual((base / profile / 'config/live-sub/glossary.json').read_bytes(), b'user vocabulary edits')
                self.assertEqual((base / profile / 'state/live-sub/ui.ini').read_bytes(), b'font=31')

    def test_explicit_shared_directory_is_rejected_without_chmod(self):
        with tempfile.TemporaryDirectory() as temporary:
            base = Path(temporary); resources = base / 'resources'; self.defaults(resources)
            documents = base / 'Documents'; documents.mkdir(); documents.chmod(0o755)
            with patch.dict(os.environ, self.environment(base, LIVE_SUB_DATA_DIR=str(documents)), clear=True):
                with self.assertRaisesRegex(RuntimeError, 'must already be private'):
                    AppPaths(resources).initialize()
            self.assertEqual(documents.stat().st_mode & 0o777, 0o755)
            self.assertEqual(list(documents.iterdir()), [])

    @unittest.skipUnless(shutil.which('git'), 'git required for interrupted clone fixture')
    def test_interrupted_clone_resumes_pinned_source_and_preserves_unknown_files(self):
        clone = runpy.run_path(str(ROOT / 'bin/bootstrap'))['clone_source']
        with tempfile.TemporaryDirectory() as temporary:
            base = Path(temporary); upstream = base / 'upstream'; target = base / 'interrupted'
            def git(*args):
                return subprocess.check_output(['git', *map(str, args)], stderr=subprocess.DEVNULL, text=True).strip()
            git('init', upstream); (upstream / 'fixture').write_text('pinned')
            git('-C', upstream, 'add', 'fixture')
            git('-C', upstream, '-c', 'user.name=Test', '-c', 'user.email=test@example.invalid', 'commit', '-m', 'fixture')
            revision = git('-C', upstream, 'rev-parse', 'HEAD')
            git('init', target); git('-C', target, 'remote', 'add', 'origin', upstream)
            clone('fixture', str(upstream), target, {'fixture':revision})
            self.assertEqual(git('-C', target, 'rev-parse', 'HEAD'), revision)
            unknown = base / 'unknown'; unknown.mkdir(); (unknown / 'important').write_text('keep')
            with self.assertRaises(RuntimeError): clone('fixture', str(upstream), unknown, {'fixture':revision})
            self.assertEqual((unknown / 'important').read_text(), 'keep')


if __name__ == '__main__': unittest.main()
