# Speech fixtures

`speech-far-16k.wav` and `speech-near-16k.wav` are the two talkers used by the
default `voice-aec-tests` run: one stands in for Puck's remote voice, the
other for the user. They are mono 16-bit PCM at 16 kHz with a short fade-out;
the far clip is 12 s and the near clip is 7.9 s, which the test zero-pads to
the scenario length. `voice-aec-tests` linearly resamples them to 48 kHz.

Source: LibriSpeech test-clean utterances `103-1240-0000` (female) and
`1034-121119-0000` (male), from
[OpenSLR SLR12](https://www.openslr.org/12), obtained through the sample links
on <https://homepages.inf.ed.ac.uk/htang2/notes/speech-samples.html>.

LibriSpeech is distributed under the
[Creative Commons Attribution 4.0 International licence](https://creativecommons.org/licenses/by/4.0/).
The recordings are by LibriVox readers; the corpus was prepared by Vassil
Panayotov, Guoguo Chen, Daniel Povey, and Sanjeev Khudanpur. No endorsement by
the authors is implied.

Changes from the source FLAC: decoded to mono PCM, the far clip trimmed to
12 seconds, both faded at the end, and stored at 16 kHz. No speech was
reordered or synthesized. The audio remains CC BY 4.0; the rest of the
repository is MIT.

These are measurement inputs, not product assets. The AEC result is still a
file test: use `--far` and `--near` with recordings from the target machine
before treating it as evidence about a real speaker and microphone.
