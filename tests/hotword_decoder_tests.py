"""Decoder-only regression cases; no microphone, model weights, or network access."""
import sys
import unittest
import json
import tempfile
from pathlib import Path
import numpy as np
sys.path.insert(0, str(Path(__file__).resolve().parent.parent / 'bin'))
from hotword_decoder import HotwordCTC, LiveDecoderConfig

class DecoderTests(unittest.TestCase):
    def decoder(self, tokens, score=6.0):
        return HotwordCTC(tokens, 0, {'beam_size':4, 'acoustic_top_k':min(4,len(tokens)),
            'max_token_log_gap':10.0, 'hotwords':[{'term':'Omarchy','score':score}]})

    def test_hotword_changes_a_plausible_acoustic_candidate(self):
        decoder = self.decoder(['<blank>','▁Omarchy','▁other'])
        probabilities = np.log(np.array([[0.1,0.25,0.65]]))
        self.assertEqual(decoder.greedy(probabilities), 'other')
        self.assertEqual(decoder.decode(probabilities), 'Omarchy')

    def test_unfinished_word_receives_no_final_reward(self):
        decoder = self.decoder(['<blank>','▁Omarch','▁other'])
        self.assertEqual(decoder.decode(np.log(np.array([[0.1,0.25,0.65]]))), 'other')

    def test_similar_longer_word_is_not_a_hotword_match(self):
        decoder = self.decoder(['<blank>','▁Omarchyx','▁other'])
        self.assertEqual(decoder.decode(np.log(np.array([[0.1,0.25,0.65]]))), 'other')

    def test_acoustically_implausible_hotword_is_pruned(self):
        decoder = self.decoder(['<blank>','▁Omarchy','▁other'])
        self.assertEqual(decoder.decode(np.array([[-20.0,-30.0,-0.01]])), 'other')

    def test_repeated_frames_do_not_duplicate_letters(self):
        decoder = self.decoder(['<blank>','a','b'])
        frames = np.log(np.array([[0.001,0.998,0.001], [0.001,0.998,0.001], [0.998,0.001,0.001]]))
        self.assertEqual(decoder.decode(frames), 'a')

    def test_blank_separates_repeated_letters(self):
        decoder = self.decoder(['<blank>','a','b'])
        frames = np.log(np.array([[0.001,0.998,0.001], [0.998,0.001,0.001], [0.001,0.998,0.001]]))
        self.assertEqual(decoder.decode(frames), 'aa')

    def test_live_config_replaces_graph_and_recovers_after_invalid_or_missing_file(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'asr-config.json'
            config = {'decoder':'hotword_ctc', 'threads':6, 'beam_size':4, 'acoustic_top_k':3,
                      'max_token_log_gap':10.0, 'hotwords':[{'term':'Omarchy','score':6.0}]}
            path.write_text(json.dumps(config))
            live = LiveDecoderConfig(path, ['<blank>', '▁Omarchy', '▁other'], 0, config)
            live.refresh()
            probabilities = np.log(np.array([[0.1,0.25,0.65]]))
            original = live.decoder
            self.assertEqual(original.decode(probabilities), 'Omarchy')
            replacement = dict(config, hotwords=[])
            staged = path.with_suffix('.tmp')
            staged.write_text(json.dumps(replacement)); staged.replace(path)
            live.refresh()
            self.assertEqual(live.decoder.decode(probabilities), 'other')
            self.assertEqual(original.decode(probabilities), 'Omarchy')
            valid_decoder, revision = live.decoder, live.revision
            for invalid in ['{', '[]', json.dumps(dict(config, threads=8)),
                            json.dumps(dict(config, hotwords=[{'term':'bad word','score':4}])),
                            json.dumps(dict(config, beam_size=True))]:
                path.write_text(invalid); live.refresh()
                self.assertTrue(live.error)
                self.assertIs(live.decoder, valid_decoder)
                self.assertEqual(live.revision, revision)
            path.unlink(); live.refresh(); self.assertTrue(live.error)
            path.write_text(json.dumps(replacement)); live.refresh()
            self.assertFalse(live.error)
            path.write_text(json.dumps(config)); live.refresh()
            self.assertFalse(live.error)
            self.assertEqual(live.decoder.decode(probabilities), 'Omarchy')

if __name__ == '__main__': unittest.main()
