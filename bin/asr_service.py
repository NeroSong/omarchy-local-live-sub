"""Bounded, model-independent HTTP transport for the loopback ASR adapter."""
from copy import deepcopy
from email.parser import BytesParser
from email.policy import default
from http.server import BaseHTTPRequestHandler, HTTPServer
import hmac
import io
import json
import re
from socketserver import ThreadingMixIn
import threading
import time
import wave


class InferenceBusy(Exception):
    pass


class ASRService:
    """Publish coherent config snapshots without making health wait for inference.

    Both callbacks run under one lock. The decoder therefore cannot change between
    a config refresh, the initial decode, and an optional Chinese fallback decode.
    The decode callback receives mono 16 kHz PCM16 bytes, not a model dependency.
    """
    def __init__(self, decode, refresh_config, metadata):
        self.decode = decode
        self.refresh_config = refresh_config
        self.metadata = deepcopy(metadata)
        self._inference = threading.Lock()
        self._status = {}
        with self._inference:
            self._refresh()

    def _refresh(self):
        self._status = deepcopy(self.refresh_config())
        return deepcopy(self._status)

    def health(self):
        # While inference uses a snapshot, report that snapshot rather than
        # modifying its decoder or waiting behind a potentially slow model.
        if self._inference.acquire(blocking=False):
            try:
                self._refresh()
            finally:
                self._inference.release()
        return {**deepcopy(self.metadata), **deepcopy(self._status)}

    def infer(self, pcm, language):
        # No inference queue: concurrent callers get an explicit retryable error.
        if not self._inference.acquire(blocking=False):
            raise InferenceBusy('Recognition is busy; retry after the current request')
        try:
            status = self._refresh()
            result = dict(self.decode(pcm, language))
            detected = result['language']
            # Do not pass Japanese/Korean output into a Chinese-English discussion.
            if language == 'auto' and (
                detected in ('ja', 'ko', 'yue')
                or re.search('[\u3040-\u30ff\uac00-\ud7af]', result['text'])
            ):
                result = dict(self.decode(pcm, 'zh'))
                detected = 'zh'
            result['text'] = re.sub(r'<\|[^|]*\|>', '', result['text']).strip()
            result['language'] = detected or language
            result.update(status)
            return result
        finally:
            self._inference.release()


class _DeadlineReader(io.RawIOBase):
    """Apply one absolute deadline to headers AND body, even with trickled bytes."""
    def __init__(self, connection, deadline):
        super().__init__()
        self.connection = connection
        self.deadline = deadline

    def readable(self):
        return True

    def readinto(self, buffer):
        remaining = self.deadline - time.monotonic()
        if remaining <= 0:
            raise TimeoutError('Request read deadline exceeded')
        self.connection.settimeout(remaining)
        return self.connection.recv_into(buffer)


class BoundedASRServer(ThreadingMixIn, HTTPServer):
    """Bound worker count and uploads separately, leaving room for health checks."""
    daemon_threads = True
    block_on_close = False
    request_queue_size = 16

    def __init__(self, address, service, *, max_connections=8, max_uploads=2,
                 read_timeout=10.0, write_timeout=2.0, handler_class=None, token=None):
        if max_connections < 2 or not 1 <= max_uploads < max_connections:
            raise ValueError('Uploads must leave at least one connection for health')
        if read_timeout <= 0 or write_timeout <= 0:
            raise ValueError('Request timeouts must be positive')
        self.service = service
        # Loopback is reachable by every local user and by browsers; require a bearer token.
        self.token = token
        self.read_timeout = read_timeout
        self.write_timeout = write_timeout
        self._connections = threading.BoundedSemaphore(max_connections)
        self.uploads = threading.BoundedSemaphore(max_uploads)
        self._active_lock = threading.Lock()
        self._active_connections = 0
        super().__init__(address, handler_class or ASRHandler)

    @property
    def active_connections(self):
        with self._active_lock:
            return self._active_connections

    def process_request(self, request, client_address):
        if not self._connections.acquire(blocking=False):
            # Never block the accept loop behind an overloaded/disconnected peer.
            # This small best-effort response may be omitted if the peer is blocked.
            try:
                request.setblocking(False)
                request.send(
                    b'HTTP/1.0 503 Service Unavailable\r\n'
                    b'Content-Length: 0\r\nConnection: close\r\nRetry-After: 1\r\n\r\n'
                )
            except OSError:
                pass
            self.shutdown_request(request)
            return
        with self._active_lock:
            self._active_connections += 1
        try:
            super().process_request(request, client_address)
        except Exception:
            self._release_connection()
            raise

    def _release_connection(self):
        with self._active_lock:
            self._active_connections -= 1
        self._connections.release()

    def process_request_thread(self, request, client_address):
        try:
            super().process_request_thread(request, client_address)
        finally:
            self._release_connection()


class ASRHandler(BaseHTTPRequestHandler):
    max_body_size = 2 * 1024 * 1024

    def setup(self):
        super().setup()
        self.rfile.close()
        self.rfile = io.BufferedReader(_DeadlineReader(
            self.connection, time.monotonic() + self.server.read_timeout))

    def handle(self):
        try:
            super().handle()
        except (ConnectionError, TimeoutError):
            # A client can disappear during headers, upload, or response.
            self.close_connection = True

    def respond(self, code, data):
        body = json.dumps(data, ensure_ascii=False).encode()
        self.close_connection = True
        self.connection.settimeout(self.server.write_timeout)
        try:
            self.send_response(code)
            self.send_header('Content-Type', 'application/json; charset=utf-8')
            self.send_header('Content-Length', str(len(body)))
            self.send_header('Connection', 'close')
            if code == 503:
                self.send_header('Retry-After', '1')
            if code == 401:
                self.send_header('WWW-Authenticate', 'Bearer')
            self.end_headers()
            self.wfile.write(body)
        except OSError:
            # Do not attempt to send a second error response after a failed write.
            pass

    def _authorized(self):
        expected = self.server.token
        if expected is None:
            return True
        supplied = self.headers.get('Authorization', '')
        if hmac.compare_digest(supplied.encode(), ('Bearer ' + expected).encode()):
            return True
        self.respond(401, {'error': 'Unauthorized'})
        return False

    def do_GET(self):
        if not self._authorized():
            return
        if self.path not in ('/', '/health'):
            return self.respond(404, {'error': 'Unknown endpoint'})
        self.respond(200, self.server.service.health())

    def _read_audio(self):
        lengths = self.headers.get_all('Content-Length', [])
        if len(lengths) != 1 or self.headers.get('Transfer-Encoding'):
            raise ValueError('Exactly one Content-Length and no Transfer-Encoding required')
        length = int(lengths[0])
        if not 0 < length <= self.max_body_size:
            raise ValueError('Invalid audio size')
        content_type = self.headers.get('Content-Type', '')
        body = self.rfile.read(length)
        if len(body) != length:
            raise ValueError('Incomplete request body')
        message = BytesParser(policy=default).parsebytes(
            ('Content-Type: ' + content_type + '\r\nMIME-Version: 1.0\r\n\r\n').encode() + body)
        if message.get_content_type() != 'multipart/form-data' or not message.is_multipart() or message.defects:
            raise ValueError('Expected a complete multipart/form-data body')
        fields = {}
        for part in message.iter_parts():
            name = part.get_param('name', header='content-disposition')
            if name not in ('file', 'language'):
                continue
            value = part.get_payload(decode=True)
            if name in fields or not isinstance(value, bytes) or part.defects:
                raise ValueError('Invalid or duplicate audio form field')
            fields[name] = value
        language = fields.get('language', b'auto').decode()
        if language not in ('auto', 'zh', 'en'):
            raise ValueError('Only zh, en and auto are supported')
        with wave.open(io.BytesIO(fields['file']), 'rb') as audio:
            if (audio.getnchannels(), audio.getsampwidth(), audio.getframerate()) != (1, 2, 16000):
                raise ValueError('Expected mono 16kHz PCM16 WAV')
            pcm = audio.readframes(audio.getnframes())
            if not pcm or len(pcm) != audio.getnframes() * 2:
                raise ValueError('Empty or incomplete WAV audio')
        return pcm, language

    def do_POST(self):
        if not self._authorized():
            return
        if self.path != '/inference':
            return self.respond(404, {'error': 'Unknown endpoint'})
        if not self.server.uploads.acquire(blocking=False):
            return self.respond(503, {'error': 'Too many audio requests; retry shortly'})
        try:
            pcm, language = self._read_audio()
            self.respond(200, self.server.service.infer(pcm, language))
        except TimeoutError:
            self.respond(408, {'error': 'Request read deadline exceeded'})
        except InferenceBusy as error:
            self.respond(503, {'error': str(error)})
        except (ValueError, KeyError, EOFError, wave.Error) as error:
            self.respond(400, {'error': str(error)})
        except ConnectionError:
            self.close_connection = True
        except Exception as error:
            self.respond(500, {'error': str(error)})
        finally:
            self.server.uploads.release()
