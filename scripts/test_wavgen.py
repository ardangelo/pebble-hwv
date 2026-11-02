"""Host-side contract checks for the HWV microphone serial sample stream."""

import tempfile
import unittest
import wave
from pathlib import Path
from unittest.mock import patch

import wavgen


class SerialStream:
    def __init__(self, lines):
        self.lines = iter(lines)
        self.written = []

    def __enter__(self):
        return self

    def __exit__(self, *_):
        return False

    def reset_input_buffer(self):
        pass

    def write(self, command):
        self.written.append(command)

    def readline(self):
        return next(self.lines, b"")


class WavgenTest(unittest.TestCase):
    def capture(self, stream, seconds=1):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "captured.wav"
            with patch.object(wavgen.serial, "Serial", return_value=stream):
                wavgen.main("fixture-port", str(output), seconds)
            with wave.open(str(output), "rb") as wav:
                return wav.getparams(), wav.readframes(wav.getnframes())

    def test_exact_pcm_and_wav_duration(self):
        values = [-32768, 32767, 0, 1234] * 4000
        stream = SerialStream([b"hwv mic capture 1\n", b"MIC1 rms=40\n", b"S\n"] +
                              [f"{sample}\n".encode() for sample in values] + [b"E\n"])
        params, audio = self.capture(stream)
        self.assertEqual((params.nchannels, params.sampwidth, params.framerate,
                          params.nframes), (1, 2, 16000, 16000))
        self.assertEqual(audio[:8], b"\x00\x80\xff\x7f\x00\x00\xd2\x04")
        self.assertEqual(stream.written, [b"hwv mic capture 1\r"])

    def test_duration_bounds_before_serial_open(self):
        for invalid in (0, -1, 2):
            with self.subTest(seconds=invalid), patch.object(wavgen.serial, "Serial") as serial:
                with self.assertRaises(ValueError):
                    wavgen.main("fixture-port", "unused.wav", invalid)
                serial.assert_not_called()

    def test_truncated_stream_never_creates_wav(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "captured.wav"
            with patch.object(wavgen.serial, "Serial", return_value=SerialStream([b"S\n", b"1\n", b"E\n"])):
                with self.assertRaisesRegex(RuntimeError, "incomplete capture"):
                    wavgen.main("fixture-port", str(output), 1)
            self.assertFalse(output.exists())

    def test_error_and_bad_samples_fail_without_output(self):
        for lines, error in (([b"Error: DMIC read failed\n"], "HWV capture failed"),
                             ([b"S\n", b"32768\n"], "invalid 16-bit"),
                             ([b"S\n", b"garbled\n"], "invalid 16-bit")):
            with self.subTest(lines=lines), tempfile.TemporaryDirectory() as directory:
                output = Path(directory) / "captured.wav"
                with patch.object(wavgen.serial, "Serial", return_value=SerialStream(lines)):
                    with self.assertRaisesRegex(RuntimeError, error):
                        wavgen.main("fixture-port", str(output), 1)
                self.assertFalse(output.exists())

    def test_missing_end_marker_times_out(self):
        with patch.object(wavgen.serial, "Serial", return_value=SerialStream([b"S\n"])), \
             patch.object(wavgen.time, "monotonic", side_effect=[0, 1, 121]):
            with self.assertRaises(TimeoutError):
                wavgen.main("fixture-port", "unused.wav", 1)


if __name__ == "__main__":
    unittest.main()
