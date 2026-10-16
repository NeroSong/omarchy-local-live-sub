"""Decoder-only regression cases; no microphone, model weights, or network access."""
import sys
import unittest
from pathlib import Path
import numpy as np
sys.path.insert(0, str(Path(__file__).resolve().parent.parent / 'bin'))
from hotword_decoder import HotwordCTC

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

if __name__ == '__main__': unittest.main()
