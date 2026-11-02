"""Capture the existing HWV S/sample/E microphone stream as 16-kHz mono WAV."""

import argparse
import struct
import time
import wave

import serial

SAMPLE_RATE = 16000
MAX_SECONDS = 1  # Both boards' capture commands accept whole seconds, max one.
CAPTURE_DEADLINE = 120  # Includes 16000 text samples at 115200 baud on Asterix.


def main(port, output, seconds):
    if not 1 <= seconds <= MAX_SECONDS:
        raise ValueError(f"capture duration must be 1-{MAX_SECONDS} whole seconds")

    total = seconds * SAMPLE_RATE
    samples = bytearray(total * 2)
    with serial.Serial(port, 115200, timeout=1, write_timeout=5) as ser:
        ser.reset_input_buffer()
        ser.write(f"hwv mic capture {seconds}\r".encode("ascii"))
        deadline = time.monotonic() + CAPTURE_DEADLINE
        started = False
        count = 0
        while time.monotonic() < deadline:
            data = ser.readline()
            if not data:
                continue
            try:
                line = data.decode("ascii").strip()
            except UnicodeDecodeError as exc:
                raise RuntimeError("non-ASCII microphone stream") from exc
            if line == "S" and not started:
                started = True
                continue
            if line == "E" and started:
                if count != total:
                    raise RuntimeError(f"incomplete capture: {count}/{total} samples")
                break
            if line.startswith(("Error:", "error:", "Microphone capture failed")):
                raise RuntimeError(f"HWV capture failed: {line}")
            if not started:
                continue  # Echo, prompt, and capture diagnostics precede S.
            if count >= total:
                raise RuntimeError("microphone stream exceeds requested duration")
            try:
                value = int(line, 10)
                struct.pack_into("<h", samples, count * 2, value)
            except (ValueError, struct.error) as exc:
                raise RuntimeError(f"invalid 16-bit microphone sample: {line!r}") from exc
            count += 1
        else:
            raise TimeoutError(f"microphone stream timed out after {CAPTURE_DEADLINE}s")

    with wave.open(output, "wb") as wav:
        wav.setnchannels(1)
        wav.setsampwidth(2)
        wav.setframerate(SAMPLE_RATE)
        wav.writeframes(samples)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("-p", "--port", required=True, help="Serial port")
    parser.add_argument("-o", "--output", required=True, help="Output WAV file")
    parser.add_argument("-s", "--seconds", type=int, default=1, help="Whole seconds (1)")
    args = parser.parse_args()
    try:
        main(args.port, args.output, args.seconds)
    except (ValueError, RuntimeError, TimeoutError, serial.SerialException, OSError) as exc:
        parser.exit(1, f"wavgen: {exc}\n")
