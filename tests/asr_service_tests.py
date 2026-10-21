"""Model-free loopback regressions; no microphone, downloads, or extra packages."""
from concurrent.futures import Future
from contextlib import redirect_stdout
import http.client
import io
import json
import os
from pathlib import Path
import queue
import runpy
import socket
import sys
import tempfile
import threading
import time
import types
import unittest
from unittest.mock import patch
import wave

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'bin'))
from asr_service import ASRHandler, ASRService, BoundedASRServer
from lifecycle import create_token


def audio_request(language='en', audio=None):
    if audio is None:
        output = io.BytesIO()
        with wave.open(output, 'wb') as wav:
            wav.setnchannels(1)
            wav.setsampwidth(2)
            wav.setframerate(16000)
            wav.writeframes(b'\x00\x00' * 320)
        audio = output.getvalue()
    boundary = 'asr-regression-boundary'
    body = (
        f'--{boundary}\r\nContent-Disposition: form-data; name="language"\r\n\r\n{language}\r\n'
        f'--{boundary}\r\nContent-Disposition: form-data; name="file"; filename="audio.wav"\r\n'
        'Content-Type: audio/wav\r\n\r\n'
    ).encode() + audio + f'\r\n--{boundary}--\r\n'.encode()
    return body, {'Content-Type': f'multipart/form-data; boundary={boundary}'}


class QuietHandler(ASRHandler):
    def log_message(self, *args):
        pass

    def _read_audio(self):
        self.server.read_started.put(True)
        return super()._read_audio()


class ServiceTests(unittest.TestCase):
    def setUp(self):
        self.clients = []
        self.threads = []
        self.releases = []
        self.server = None
        self.addCleanup(self.cleanup)

    def cleanup(self):
        for event in self.releases:
            event.set()
        for client in self.clients:
            client.close()
        for thread in self.threads:
            thread.join(timeout=2)
            self.assertFalse(thread.is_alive(), 'Client thread leaked')
        if self.server:
            self.server.shutdown()
            self.server.server_close()
            self.server_thread.join(timeout=2)
            self.assertFalse(self.server_thread.is_alive(), 'Server thread leaked')
            self.wait_for(lambda: self.server.active_connections == 0)

    def wait_for(self, condition, timeout=2):
        until = time.monotonic() + timeout
        while not condition() and time.monotonic() < until:
            time.sleep(0.005)
        self.assertTrue(condition(), 'Timed out waiting for service state')

    def start(self, decode=None, refresh=None, **kwargs):
        decode = decode or (lambda pcm, language: {'text': ' hello ', 'language': 'en', 'decoder': 'fake'})
        refresh = refresh or (lambda: {'config_revision': 'initial', 'config_error': '', 'hotwords': []})
        service = ASRService(decode, refresh, {'engine': 'sensevoice'})
        self.server = BoundedASRServer(
            ('127.0.0.1', 0), service, handler_class=QuietHandler,
            read_timeout=kwargs.pop('read_timeout', 0.4), write_timeout=0.4, **kwargs)
        self.server.read_started = queue.Queue()
        self.address = self.server.server_address
        self.server_thread = threading.Thread(
            target=self.server.serve_forever, kwargs={'poll_interval': 0.01}, daemon=True)
        self.server_thread.start()

    def request(self, method='GET', path='/', body=None, headers=None):
        connection = http.client.HTTPConnection(*self.address, timeout=2)
        try:
            connection.request(method, path, body=body, headers=headers or {})
            response = connection.getresponse()
            data = response.read()
            return response.status, json.loads(data) if data else {}, dict(response.getheaders())
        finally:
            connection.close()

    def post(self, language='en', audio=None):
        body, headers = audio_request(language, audio)
        return self.request('POST', '/inference', body, headers)

    def async_call(self, callback):
        future = Future()

        def run():
            try:
                future.set_result(callback())
            except BaseException as error:
                future.set_exception(error)

        thread = threading.Thread(target=run, daemon=True)
        self.threads.append(thread)
        thread.start()
        return future

    def raw(self):
        client = socket.create_connection(self.address, timeout=2)
        self.clients.append(client)
        return client

    def incomplete_body(self):
        client = self.raw()
        client.sendall(
            b'POST /inference HTTP/1.0\r\nContent-Length: 10000\r\n'
            b'Content-Type: multipart/form-data; boundary=test\r\n\r\nx')
        self.server.read_started.get(timeout=2)
        return client

    def read_raw_response(self, client):
        response = http.client.HTTPResponse(client)
        response.begin()
        data = response.read()
        return response.status, json.loads(data) if data else {}

    def test_token_is_required_for_health_and_inference(self):
        decoded = []
        self.start(decode=lambda pcm, language: decoded.append(language) or {'text': 'x', 'language': 'en'}, token='secret')
        body, audio_headers = audio_request()
        good = {'Authorization': 'Bearer secret'}
        for headers in ({}, {'Authorization': 'Bearer wrong'}, {'Authorization': 'secret'}, {'Authorization': 'Bearer secret '}):
            status, payload, response = self.request('GET', '/', headers=headers)
            self.assertEqual((status, payload), (401, {'error': 'Unauthorized'}), headers)
            self.assertEqual(response.get('WWW-Authenticate'), 'Bearer')
            status, _, _ = self.request('POST', '/inference', body, {**audio_headers, **headers})
            self.assertEqual(status, 401, headers)
        self.assertEqual(decoded, [], 'Unauthorized audio must never reach recognition')
        self.assertEqual(self.request('GET', '/', headers=good)[0], 200)
        status, payload, _ = self.request('POST', '/inference', body, {**audio_headers, **good})
        self.assertEqual((status, payload['text']), (200, 'x'))
        self.assertEqual(decoded, ['en'])

    def test_valid_audio_and_health_remain_compatible(self):
        received = []

        def decode(pcm, language):
            received.append((len(pcm), language))
            return {'text': '<|en|> hello ', 'language': '', 'decoder': 'fake'}

        self.start(decode)
        code, result, _ = self.post()
        self.assertEqual(code, 200)
        self.assertEqual(result['text'], 'hello')
        self.assertEqual(result['language'], 'en')
        self.assertEqual(result['config_revision'], 'initial')
        self.assertEqual(received, [(640, 'en')])
        self.assertEqual(self.request()[0], 200)
        self.assertEqual(self.request(path='/health')[0], 200)
        self.assertEqual(self.request(path='/unknown')[0], 404)

    def test_incomplete_body_times_out_without_blocking_health(self):
        self.start()
        started = time.monotonic()
        client = self.incomplete_body()
        self.assertEqual(self.request()[0], 200)
        self.assertEqual(self.read_raw_response(client)[0], 408)
        self.assertLess(time.monotonic() - started, 1.5)
        self.assertEqual(self.post()[0], 200)

    def test_slow_trickled_body_has_absolute_not_idle_deadline(self):
        self.start()
        started = time.monotonic()
        client = self.incomplete_body()

        def trickle():
            until = time.monotonic() + 1.2
            while time.monotonic() < until:
                try:
                    client.sendall(b'x')
                except OSError:
                    return
                time.sleep(0.03)

        sender = self.async_call(trickle)
        self.assertEqual(self.read_raw_response(client)[0], 408)
        self.assertLess(time.monotonic() - started, 1.0)
        client.close()
        sender.result(timeout=2)
        self.assertEqual(self.request()[0], 200)

    def test_slow_trickled_headers_also_expire(self):
        self.start()
        client = self.raw()
        client.sendall(b'GET / HTTP/1.0\r\nX-Slow: ')
        self.wait_for(lambda: self.server.active_connections == 1)
        started = time.monotonic()

        def trickle():
            until = time.monotonic() + 1.2
            while time.monotonic() < until:
                try:
                    client.sendall(b'x')
                except OSError:
                    return
                time.sleep(0.03)

        sender = self.async_call(trickle)
        self.assertEqual(self.request()[0], 200)
        try:
            self.assertEqual(client.recv(1024), b'')
        except ConnectionResetError:
            pass
        self.assertLess(time.monotonic() - started, 1.0)
        client.close()
        sender.result(timeout=2)
        self.assertEqual(self.request()[0], 200)

    def test_upload_capacity_leaves_health_available(self):
        self.start(max_connections=4, max_uploads=2, read_timeout=1)
        self.incomplete_body()
        self.incomplete_body()
        code, result, headers = self.post()
        self.assertEqual(code, 503)
        self.assertIn('Too many audio', result['error'])
        self.assertEqual(headers['Retry-After'], '1')
        self.assertEqual(self.request()[0], 200)
        self.assertLessEqual(self.server.active_connections, 4)

    def test_connection_capacity_is_bounded_and_recovers(self):
        self.start(max_connections=3, max_uploads=1, read_timeout=1)
        for _ in range(3):
            client = self.raw()
            client.sendall(b'GET / HTTP/1.0\r\nX-Incomplete: ')
        self.wait_for(lambda: self.server.active_connections == 3)
        for _ in range(5):
            code, _, headers = self.request()
            self.assertEqual(code, 503)
            self.assertEqual(headers['Retry-After'], '1')
            self.assertEqual(self.server.active_connections, 3)
        for client in self.clients:
            client.close()
        self.wait_for(lambda: self.server.active_connections == 0)
        self.assertEqual(self.request()[0], 200)

    def test_short_body_disconnect_does_not_poison_next_request(self):
        self.start()
        client = self.incomplete_body()
        client.shutdown(socket.SHUT_WR)
        code, result = self.read_raw_response(client)
        self.assertEqual(code, 400)
        self.assertIn('Incomplete request body', result['error'])
        client.close()
        self.wait_for(lambda: self.server.active_connections == 0)
        self.assertEqual(self.post()[0], 200)

    def test_disconnect_during_inference_releases_capacity(self):
        entered, release = threading.Event(), threading.Event()
        self.releases.append(release)

        def decode(pcm, language):
            entered.set()
            if not release.wait(timeout=2):
                raise RuntimeError('Test failed to release inference')
            return {'text': 'finished', 'language': language}

        self.start(decode)
        body, headers = audio_request()
        client = self.raw()
        client.sendall((
            f'POST /inference HTTP/1.0\r\nContent-Length: {len(body)}\r\n'
            f'Content-Type: {headers["Content-Type"]}\r\n\r\n'
        ).encode() + body)
        self.assertTrue(entered.wait(timeout=2))
        client.shutdown(socket.SHUT_RDWR)
        client.close()
        self.assertEqual(self.request()[0], 200)
        release.set()
        self.wait_for(lambda: self.server.active_connections == 0)
        self.assertEqual(self.post()[0], 200)

    def test_busy_inference_health_and_config_revision_are_consistent(self):
        entered, release = threading.Event(), threading.Event()
        self.releases.append(release)
        state = {'desired': 'old', 'active': None}
        decoded = []

        def refresh():
            state['active'] = state['desired']
            return {'config_revision': state['active'], 'config_error': '', 'hotwords': [state['active']]}

        def decode(pcm, language):
            revision = state['active']
            decoded.append((language, revision))
            if language == 'auto':
                entered.set()
                if not release.wait(timeout=2):
                    raise RuntimeError('Test failed to release inference')
                self.assertEqual(state['active'], revision)
                return {'text': '日本語', 'language': 'ja'}
            return {'text': f' {revision} ', 'language': language}

        self.start(decode, refresh, max_connections=5, max_uploads=3)
        pending = self.async_call(lambda: self.post('auto'))
        self.assertTrue(entered.wait(timeout=2))
        state['desired'] = 'new'
        for _ in range(4):
            code, health, _ = self.request()
            self.assertEqual(code, 200)
            self.assertEqual(health['config_revision'], 'old')
            self.assertEqual(health['hotwords'], ['old'])
        rivals = [self.async_call(self.post) for _ in range(3)]
        for rival in rivals:
            self.assertEqual(rival.result(timeout=2)[0], 503)
        self.assertEqual(decoded, [('auto', 'old')])
        release.set()
        code, result, _ = pending.result(timeout=2)
        self.assertEqual(code, 200)
        self.assertEqual(result['config_revision'], 'old')
        self.assertEqual(result['text'], 'old')
        self.assertEqual(result['language'], 'zh')
        self.assertEqual(decoded, [('auto', 'old'), ('zh', 'old')])
        self.assertEqual(self.request()[1]['config_revision'], 'new')
        self.assertEqual(self.post()[1]['text'], 'new')

    def test_inference_exception_does_not_leave_lock_held(self):
        calls = []

        def decode(pcm, language):
            calls.append(language)
            if len(calls) == 1:
                raise RuntimeError('Fake model failure')
            return {'text': 'recovered', 'language': language}

        self.start(decode)
        self.assertEqual(self.post()[0], 500)
        self.assertEqual(self.request()[0], 200)
        self.assertEqual(self.post()[0], 200)

    def test_invalid_audio_is_rejected_before_decode(self):
        calls = []
        self.start(lambda pcm, lang: calls.append(lang))
        for language, audio in [('fr', None), ('en', b'not wav'), ('en', b'')]:
            with self.subTest(language=language, audio=audio):
                self.assertEqual(self.post(language, audio)[0], 400)
        self.assertEqual(calls, [])

    def test_malformed_multipart_is_a_client_error(self):
        calls = []
        self.start(lambda pcm, lang: calls.append(lang))
        body, headers = audio_request()
        malformed = [
            body.rsplit(b'--asr-regression-boundary--', 1)[0],
            body.replace(b'name="file"', b'name="language"'),
            body.replace(
                b'name="language"\r\n\r\nen',
                b'name="language"\r\nContent-Type: multipart/mixed; boundary=nested\r\n\r\n'
                b'--nested\r\nContent-Type: text/plain\r\n\r\nen\r\n--nested--'),
        ]
        for candidate in malformed:
            with self.subTest(body=candidate):
                self.assertEqual(self.request('POST', '/inference', candidate, headers)[0], 400)
        self.assertEqual(calls, [])

    def test_invalid_framing_is_rejected_without_reading_body(self):
        self.start()
        for framing in [
            b'Content-Length: 2097153\r\n',
            b'Content-Length: 12\r\nContent-Length: 12\r\n',
            b'Content-Length: 12\r\nTransfer-Encoding: chunked\r\n',
            b'Content-Length: nope\r\n',
        ]:
            with self.subTest(framing=framing):
                client = self.raw()
                client.sendall(b'POST /inference HTTP/1.0\r\n' + framing + b'\r\n')
                self.assertEqual(self.read_raw_response(client)[0], 400)
                client.close()


class EntryPointTests(unittest.TestCase):
    def test_greedy_uses_configured_threads_without_loading_models(self):
        engines = []

        def create_engine(**kwargs):
            engines.append(kwargs)
            return object()

        fake_modules = {
            'numpy': types.ModuleType('numpy'),
            'model_assets': types.SimpleNamespace(asr_directory=lambda root: Path('/unused-model')),
            'sherpa_onnx': types.SimpleNamespace(
                OfflineRecognizer=types.SimpleNamespace(from_sense_voice=create_engine)),
        }
        with tempfile.TemporaryDirectory() as directory:
            Path(directory, 'asr-config.json').write_text(json.dumps({'decoder': 'greedy', 'threads': 3}))
            token = Path(directory, 'state/api-token')
            create_token(token)
            with patch.dict(sys.modules, fake_modules), patch.dict(
                os.environ, {'LIVE_SUB_CONFIG_DIR': directory, 'SENSEVOICE_DECODER': 'greedy',
                             'LIVE_SUB_API_TOKEN_FILE': str(token)}):
                entry = runpy.run_path(str(ROOT / 'bin/sensevoice-server'))
                with patch.dict(entry['main'].__globals__, {'BoundedASRServer': FakeServer}), redirect_stdout(io.StringIO()):
                    entry['main']()
        self.assertEqual([engine['num_threads'] for engine in engines], [3, 3, 3])
        self.assertEqual([engine['language'] for engine in engines], ['zh', 'en', 'auto'])


    def test_entry_point_refuses_to_serve_without_a_private_token(self):
        fake_modules = {'numpy': types.ModuleType('numpy')}
        with tempfile.TemporaryDirectory() as directory:
            Path(directory, 'asr-config.json').write_text(json.dumps({'decoder': 'greedy'}))
            for token in ('', str(Path(directory, 'missing'))):
                with self.subTest(token=token), patch.dict(sys.modules, fake_modules), patch.dict(
                    os.environ, {'LIVE_SUB_CONFIG_DIR': directory, 'SENSEVOICE_DECODER': 'greedy',
                                 'LIVE_SUB_API_TOKEN_FILE': token}):
                    entry = runpy.run_path(str(ROOT / 'bin/sensevoice-server'))
                    engine = types.SimpleNamespace(OfflineRecognizer=types.SimpleNamespace(from_sense_voice=lambda **kw: object()))
                    with patch.dict(sys.modules, {'sherpa_onnx': engine, 'model_assets': types.SimpleNamespace(asr_directory=lambda root: Path('/unused'))}), \
                         redirect_stdout(io.StringIO()), self.assertRaises(SystemExit):
                        entry['main']()


class FakeServer:
    def __init__(self, address, service, token=None):
        assert address[0] == '127.0.0.1'
        assert token, 'The entry point must pass the private token to the server'
        assert service.health()['decoder'] == 'greedy'

    def __enter__(self):
        return self

    def __exit__(self, *args):
        pass

    def serve_forever(self):
        pass


if __name__ == '__main__':
    unittest.main()
