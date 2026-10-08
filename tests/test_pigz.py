"""Integration tests for the in-process gzip backend (Python stdlib only)."""

import argparse
import binascii
import gzip
import json
import os
from pathlib import Path
import random
import struct
import subprocess
import tempfile
import time
import zlib


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path, required=True)
    parser.add_argument("--baseline", type=Path)
    parser.add_argument("--pigz", type=Path)
    parser.add_argument("--benchmark", action="store_true")
    args = parser.parse_args()
    exe = args.exe.resolve(strict=True)
    baseline = args.baseline.resolve(strict=True) if args.baseline else None
    pigz = args.pigz.resolve(strict=True) if args.pigz else None
    artifact_root = Path(__file__).resolve().parents[1] / "build-tests"
    artifact_root.mkdir(exist_ok=True)
    checks = 0
    timings = {}
    # Native programs must work without the compiler, Git Bash or helper tools.
    native_env = os.environ.copy()
    if os.name == "nt":
        native_env["PATH"] = str(Path(os.environ["SystemRoot"]) / "System32")

    with tempfile.TemporaryDirectory(prefix="pigz-", dir=artifact_root) as name:
        work = Path(name).resolve()
        # Verify the absolute cleanup target is inside our artifact directory.
        work.relative_to(artifact_root.resolve())

        def run(binary, *command, data=None, ok=True):
            result = subprocess.run(
                [str(binary), *map(str, command)], input=data,
                stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                cwd=work, env=native_env, timeout=60,
            )
            if (result.returncode == 0) != ok:
                raise AssertionError(
                    f"Exit {result.returncode}: {command}\n"
                    + (result.stdout + result.stderr).decode(errors="replace")
                )
            return result.stdout

        def check_gzip(encoded, expected):
            nonlocal checks
            decoder = zlib.decompressobj(31)
            assert decoder.decompress(encoded) + decoder.flush() == expected
            assert decoder.eof and decoder.unused_data == b"", "Must be one gzip member"
            assert gzip.decompress(encoded) == expected
            crc, size = struct.unpack("<II", encoded[-8:])
            assert crc == binascii.crc32(expected) & 0xFFFFFFFF
            assert size == len(expected) & 0xFFFFFFFF
            checks += 1

        serial = 0

        def compress(source, *options, binary=exe):
            nonlocal serial
            serial += 1
            archive = work / f"case-{serial}.gz"
            run(binary, "a", "-y", "-bd", "-tgzip", "-mtm=off", *options, archive, source)
            return archive, archive.read_bytes()

        rng = random.Random(7491)
        edge_sizes = [0, 1, 2, 3, 32767, 32768, 32769,
                      131071, 131072, 131073, 262144, 262145, 524305]
        for size in edge_sizes:
            source = work / f"edge-{size}.bin"
            payload = rng.randbytes(size)
            source.write_bytes(payload)
            reference = None
            for threads in (1, 4, 8):
                archive, encoded = compress(source, "-mm=Pigz", f"-mmt={threads}")
                check_gzip(encoded, payload)
                if reference is None:
                    reference = encoded
                assert encoded == reference, "Output must not depend on scheduling"
                run(exe, "t", "-bd", archive)
                if baseline:
                    run(baseline, "t", "-bd", archive)
                if pigz:
                    run(pigz, "-t", archive)

        # Repeat history within zlib's maximum usable match distance.
        payload = rng.randbytes(30000) * 45 + b"tail\x00\xff\r\n"
        source = work / "混合 数据.bin"
        source.write_bytes(payload)
        for level in range(10):
            archive, encoded = compress(source, "-mm=Pigz", "-mmt=4", f"-mx={level}")
            check_gzip(encoded, payload)
            assert encoded[8] == (2 if level == 9 else 4 if level == 1 else 0)
            if level > 0:
                assert len(encoded) < len(payload) // 8, "Cross-block history was lost"
        for block in ("32k", "64k", "128k", "1m"):
            _, encoded = compress(source, "-mm=Pigz", "-mmt=4", f"-mcc={block}")
            check_gzip(encoded, payload)

        _, explicit = compress(source, "-mm=Pigz", "-mmt=4")
        _, automatic = compress(source, "-mmt=4")
        assert automatic == explicit, "Gzip default must select pigz"
        _, single = compress(source, "-mm=Pigz", "-mmt=off")
        assert single == explicit
        _, capped = compress(source, "-mm=Pigz", "-mmt=1000")
        assert capped == explicit
        _, method_options = compress(source, "-mm=Pigz:mt=4:cc=128k")
        assert method_options == explicit

        legacy_archive, legacy = compress(source, "-mm=Deflate")
        check_gzip(legacy, payload)
        if baseline:
            _, original = compress(source, binary=baseline)
            assert legacy == original, "Explicit Deflate must preserve original bytes"
            _, fallback = compress(source, "-mfb=64")
            _, tuned_original = compress(source, "-mfb=64", binary=baseline)
            assert fallback == tuned_original, "Deflate tuning must retain original backend"

        for options in (("-mm=Unknown",), ("-mm=Pigz", "-mcc=16k"),
                        ("-mm=Pigz", "-mcc=32m"), ("-mm=Pigz", "-mmt=0"),
                        ("-mm=Pigz", "-mx=10"), ("-mm=Pigz", "-mfb=64")):
            serial += 1
            run(exe, "a", "-tgzip", *options, work / f"invalid-{serial}.gz", source, ok=False)
            checks += 1

        encoded = run(exe, "a", "-tgzip", "-mm=Pigz", "-mmt=4", "-so",
                      "-siinput.bin", "stdout.gz", data=payload)
        check_gzip(encoded, payload)
        assert run(exe, "x", "-tgzip", "-si", "-so", data=encoded) == payload
        stdin_archive = work / "stdin.gz"
        run(exe, "a", "-tgzip", "-mmt=4", "-siinput.bin", stdin_archive, data=payload)
        check_gzip(stdin_archive.read_bytes(), payload)

        damaged = bytearray(explicit)
        damaged[-8] ^= 1
        corrupt = work / "bad-crc.gz"
        corrupt.write_bytes(damaged)
        run(exe, "t", corrupt, ok=False)
        checks += 1

        # Test gzip metadata with a regular, portable file name.
        metadata_source = work / "metadata.txt"
        metadata_source.write_bytes(b"metadata\n")
        os.utime(metadata_source, (1700000000, 1700000000))
        metadata_archive = work / "metadata.gz"
        run(exe, "a", "-tgzip", metadata_archive, metadata_source)
        metadata = metadata_archive.read_bytes()
        assert struct.unpack("<I", metadata[4:8])[0] == 1700000000
        assert metadata[3] & 8 and metadata[10:].split(b"\0", 1)[0] == b"metadata.txt"
        check_gzip(metadata, b"metadata\n")

        # Other archive handlers must remain compatible with the original tool.
        for kind, options in (("7z", ()), ("zip", ("-mm=Deflate",))):
            archive = work / f"regression.{kind}"
            run(exe, "a", f"-t{kind}", *options, archive, source)
            run(exe, "t", archive)
            assert run(exe, "x", "-so", archive) == payload
            if baseline:
                assert run(baseline, "x", "-so", archive) == payload
            checks += 1

        if args.benchmark:
            # Mixed text with changing fields: a reproducible local workload.
            text = b"".join(
                f"{i:08d},sample-{rng.randrange(1000000):06d},gzip compression record\n".encode()
                for i in range(150000)
            )
            bench = text * 4
            bench_source = work / "benchmark.txt"
            bench_source.write_bytes(bench)
            cases = [("pigz-1-thread", exe, ("-mm=Pigz", "-mmt=1")),
                     ("pigz-4-threads", exe, ("-mm=Pigz", "-mmt=4"))]
            if baseline:
                cases.append(("original-7zip", baseline, ()))
            for label, binary, options in cases:
                start = time.perf_counter()
                _, packed = compress(bench_source, "-mx=6", *options, binary=binary)
                seconds = time.perf_counter() - start
                check_gzip(packed, bench)
                timings[label] = {"seconds": round(seconds, 4), "input_bytes": len(bench),
                                  "output_bytes": len(packed)}

    result = {"status": "PASS", "checks": checks, "exe": str(exe), "timings": timings}
    (artifact_root / "results-latest.json").write_text(json.dumps(result, indent=2), encoding="utf-8")
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
