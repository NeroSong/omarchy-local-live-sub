"""CTC prefix beam search with bounded, whole-word contextual scores.

Uses the existing SenseVoice ONNX weights; no text substitutions are performed here.
The log-add CTC recurrence merges blank/nonblank paths before contextual pruning.
"""
from dataclasses import dataclass
import math
import re
import time
import json
import hashlib
import numpy as np

NEG = -math.inf

class LiveDecoderConfig:
    """Replace the search graph only after the complete configuration validates."""
    editable = {'hotwords', 'beam_size', 'acoustic_top_k', 'max_token_log_gap'}

    def __init__(self, path, tokens, blank_id, initial):
        self.path, self.tokens, self.blank_id = path, tokens, blank_id
        self.initial = json.loads(json.dumps(initial))
        self.config = self.initial
        self.decoder = HotwordCTC(tokens, blank_id, initial)
        self.revision = hashlib.sha256(json.dumps(initial, sort_keys=True).encode()).hexdigest()
        self.seen = None
        self.error = ''

    def refresh(self):
        try:
            with self.path.open('rb') as source:
                data = source.read(256 * 1024 + 1)
            if len(data) > 256 * 1024:
                # Oversized data cannot be cached as a complete candidate. Forget
                # the previous bytes so restoring them also clears this error.
                self.seen = None
                raise ValueError('ASR configuration exceeds 256 KiB')
            if data == self.seen:
                return
            self.seen = data
            candidate = json.loads(data)
            if not isinstance(candidate, dict):
                raise ValueError('ASR configuration must be an object')
            if {k:v for k,v in candidate.items() if k not in self.editable} != {k:v for k,v in self.initial.items() if k not in self.editable}:
                raise ValueError('Decoder mode and threads require a service restart; keep those values unchanged for hot reload')
            replacement = HotwordCTC(self.tokens, self.blank_id, candidate)
            self.decoder, self.config = replacement, candidate
            self.revision = hashlib.sha256(data).hexdigest()
            self.error = ''
        except (OSError, ValueError, TypeError, KeyError, AttributeError, OverflowError) as error:
            self.error = str(error)
            # Retry the same valid bytes after a temporary read failure.
            if isinstance(error, OSError):
                self.seen = None

def logadd(a, b):
    if a == NEG: return b
    if b == NEG: return a
    high, low = (a, b) if a >= b else (b, a)
    return high + math.log1p(math.exp(low - high))

class ContextGraph:
    """Character trie supports different BPE segmentations of the same hotword."""
    def __init__(self, hotwords, pieces):
        self.children = [{}]
        self.terminal = [0.0]
        self.potential = [0.0]
        names = set()
        for entry in hotwords:
            word, score = entry['term'].lower(), float(entry['score'])
            if not re.fullmatch('[a-z]{3,32}', word) or not 0 < score <= 8 or word in names:
                raise ValueError('Hotwords must be unique ASCII words, length 3–32, with scores in (0, 8]')
            names.add(word)
            node = 0
            for position, char in enumerate(word, 1):
                if char not in self.children[node]:
                    self.children[node][char] = len(self.children)
                    self.children.append({}); self.terminal.append(0.0); self.potential.append(0.0)
                node = self.children[node][char]
                self.potential[node] = max(self.potential[node], score * position / len(word))
            self.terminal[node] = score
        self.pieces = pieces
        self.injections = {}
        ascii_pieces = [(i, p.lower()) for i, p in enumerate(pieces) if re.fullmatch(' ?[A-Za-z]+', p)]
        # Also retain starts from the middle of a non-hotword via a BPE word boundary.
        for node in range(-1, len(self.children)):
            ids = []
            for token, piece in ascii_pieces:
                next_node, completed = self.advance(node, 0.0, piece)
                if next_node > 0 or completed > 0: ids.append(token)
            self.injections[node] = ids

    def advance(self, node, completed, piece):
        for char in piece.lower():
            if 'a' <= char <= 'z' or '0' <= char <= '9':
                node = self.children[node].get(char, -1) if node >= 0 else -1
            else:
                if node >= 0: completed += self.terminal[node]
                node = 0
        return node, completed

    def score(self, node, completed, final=False):
        if node < 0: return completed
        # Incomplete prefixes help survive pruning but earn no reward at the end.
        return completed + (self.terminal[node] if final else self.potential[node])

@dataclass
class Beam:
    blank: float = NEG
    nonblank: float = NEG
    node: int = 0
    completed: float = 0.0

class HotwordCTC:
    def __init__(self, tokens, blank_id, config):
        for key in ('beam_size', 'acoustic_top_k'):
            if type(config[key]) is not int:
                raise ValueError(key + ' must be an integer')
        if type(config['max_token_log_gap']) not in (int, float):
            raise ValueError('max_token_log_gap must be a number')
        if not isinstance(config['hotwords'], list) or len(config['hotwords']) > 128:
            raise ValueError('hotwords must be an array with at most 128 entries')
        for entry in config['hotwords']:
            if not isinstance(entry, dict) or not isinstance(entry.get('term'), str) or type(entry.get('score')) not in (int, float):
                raise ValueError('Each hotword requires a string term and numeric score')
        self.tokens = tokens
        self.pieces = [token.replace('▁', ' ') for token in tokens]
        self.blank = blank_id
        self.beam_size = int(config['beam_size'])
        self.top_k = int(config['acoustic_top_k'])
        self.max_gap = float(config['max_token_log_gap'])
        if not 1 <= self.beam_size <= 8 or not 1 <= self.top_k <= 32 or not 0 < self.max_gap <= 15:
            raise ValueError('Invalid decoder search limits')
        self.graph = ContextGraph(config['hotwords'], self.pieces)
        self.special = {i for i,t in enumerate(tokens) if t.startswith('<') and t.endswith('>')}

    def text(self, tokens):
        return ''.join(self.pieces[i] for i in tokens if i not in self.special).strip()

    def greedy(self, log_probs):
        tokens, last = [], -1
        for token in np.argmax(log_probs, axis=1).tolist():
            if token != self.blank and token != last: tokens.append(token)
            last = token
        return self.text(tokens)

    def decode(self, log_probs):
        beams = {(): Beam(blank=0.0)}
        for frame in log_probs:
            top = set(np.argpartition(frame, -self.top_k)[-self.top_k:].tolist())
            best = float(np.max(frame))
            next_beams = {}
            for prefix, beam in beams.items():
                total = logadd(beam.blank, beam.nonblank)
                same = next_beams.setdefault(prefix, Beam(node=beam.node, completed=beam.completed))
                same.blank = logadd(same.blank, total + float(frame[self.blank]))
                candidates = top.union(self.graph.injections[beam.node])
                # Include repeats even if pruning excludes the last emitted token.
                if prefix: candidates.add(prefix[-1])
                for token in candidates:
                    if token == self.blank or token in self.special: continue
                    probability = float(frame[token])
                    if probability < best - self.max_gap: continue
                    if prefix and token == prefix[-1]:
                        same.nonblank = logadd(same.nonblank, beam.nonblank + probability)
                        origin = beam.blank
                    else:
                        origin = total
                    if origin == NEG: continue
                    extended = prefix + (token,)
                    target = next_beams.get(extended)
                    if target is None:
                        node, completed = self.graph.advance(beam.node, beam.completed, self.pieces[token])
                        target = next_beams[extended] = Beam(node=node, completed=completed)
                    target.nonblank = logadd(target.nonblank, origin + probability)
            def rank(item):
                beam = item[1]
                return logadd(beam.blank, beam.nonblank) + self.graph.score(beam.node, beam.completed)
            beams = dict(sorted(next_beams.items(), key=rank, reverse=True)[:self.beam_size])
        def final_rank(item):
            beam = item[1]
            return logadd(beam.blank, beam.nonblank) + self.graph.score(beam.node, beam.completed, final=True)
        winner = max(beams.items(), key=final_rank)[0]
        return self.text(winner)

class Recognizer:
    def __init__(self, model_dir, config):
        import kaldi_native_fbank as knf
        import onnxruntime as ort
        self.knf = knf
        opts = ort.SessionOptions()
        opts.intra_op_num_threads = int(config.get('threads', 6))
        opts.inter_op_num_threads = 1
        opts.execution_mode = ort.ExecutionMode.ORT_SEQUENTIAL
        self.session = ort.InferenceSession(str(model_dir / 'model.onnx'), sess_options=opts, providers=['CPUExecutionProvider'])
        self.meta = self.session.get_modelmeta().custom_metadata_map
        self.inputs = self.session.get_inputs()
        if len(self.inputs) != 4: raise ValueError('Expected the four-input sherpa-onnx SenseVoice export')
        def vector(name):
            values = np.fromstring(self.meta[name], sep=',', dtype=np.float32)
            if values.size != self.window * 80: raise ValueError('Invalid CMVN dimensions')
            return values
        self.window = int(self.meta['lfr_window_size'])
        self.shift = int(self.meta['lfr_window_shift'])
        self.mean, self.scale = vector('neg_mean'), vector('inv_stddev')
        self.normalized = int(self.meta['normalize_samples']) != 0
        self.languages = {lang:int(self.meta['lang_' + lang]) for lang in ('zh','en','auto')}
        self.itn = int(self.meta['with_itn'])
        vocab = int(self.meta['vocab_size'])
        tokens = [''] * vocab
        for line in (model_dir / 'tokens.txt').read_text().splitlines():
            token, index = line.rsplit(' ', 1)
            tokens[int(index)] = token
        if not all(tokens): raise ValueError('Incomplete model token table')
        self.decoder = HotwordCTC(tokens, int(self.meta.get('blank_id', '0')), config)
        self.config = config

    def features(self, samples):
        opts = self.knf.FbankOptions()
        opts.frame_opts.samp_freq = 16000
        opts.frame_opts.dither = 0.0
        opts.frame_opts.window_type = 'hamming'
        opts.frame_opts.snip_edges = True
        opts.mel_opts.num_bins = 80
        opts.mel_opts.high_freq = 0
        opts.energy_floor = 0.0
        fbank = self.knf.OnlineFbank(opts)
        signal = samples if self.normalized else samples * 32768
        fbank.accept_waveform(16000, signal.tolist())
        fbank.input_finished()
        if not fbank.num_frames_ready: raise ValueError('Audio too short for feature extraction')
        frames = np.stack([fbank.get_frame(i) for i in range(fbank.num_frames_ready)])
        centers = np.arange(0, len(frames), self.shift)
        offsets = np.arange(self.window) - (self.window - 1) // 2
        indices = np.clip(centers[:,None] + offsets[None,:], 0, len(frames)-1)
        lfr = frames[indices].reshape(len(centers), -1)
        return ((lfr + self.mean) * self.scale).astype(np.float32)[None,:,:]

    def decode(self, samples, language):
        started = time.monotonic()
        features = self.features(samples)
        arrays = [features, np.array([features.shape[1]]), np.array([self.languages[language]]), np.array([self.itn])]
        feed = {}
        for spec, array in zip(self.inputs, arrays):
            dtype = np.float32 if spec.type == 'tensor(float)' else np.int32 if spec.type == 'tensor(int32)' else np.int64
            feed[spec.name] = array.astype(dtype)
        logits = self.session.run(None, feed)[0][0]
        expected = features.shape[1] + 4
        if logits.ndim != 2 or logits.shape[0] < expected or logits.shape[1] != len(self.decoder.tokens):
            raise ValueError('Unexpected SenseVoice output shape')
        logits = logits[:expected]
        labels = np.argmax(logits[:4], axis=1)
        detected = self.decoder.tokens[int(labels[0])].replace('<|','').replace('|>','')
        text_logits = logits[4:]
        # Log normalization also handles exports that return logits rather than log probabilities.
        maximum = np.max(text_logits, axis=1, keepdims=True)
        log_probs = text_logits - maximum - np.log(np.exp(text_logits-maximum).sum(axis=1,keepdims=True))
        greedy = self.decoder.greedy(log_probs)
        inference_ms = round((time.monotonic()-started)*1000)
        decode_start = time.monotonic()
        text = self.decoder.decode(log_probs)
        return {'text':text, 'language':detected, 'decoder':'hotword_ctc', 'greedy_text':greedy,
                'inference_ms':inference_ms, 'decode_ms':round((time.monotonic()-decode_start)*1000)}
