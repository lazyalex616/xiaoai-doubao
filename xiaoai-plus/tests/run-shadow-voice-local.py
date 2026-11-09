#!/usr/bin/env python3
"""Local-only directed fixtures. Always writes into a NEW evidence directory."""
import argparse
import datetime
import hashlib
import json
import pathlib
import shutil
import subprocess
import time


def digest(path):
    data = path.read_bytes()
    return {"sha256": hashlib.sha256(data).hexdigest(), "bytes": len(data)}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", required=True, type=pathlib.Path)
    args = parser.parse_args()
    root = pathlib.Path(__file__).resolve().parents[1]
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    sources = ["src/audio/shadow_voice.hpp", "src/audio/shadow_voice.cpp",
               "tests/shadow_voice_test.cpp", "tests/run-shadow-voice-local.py"]
    source_receipt = {}
    for relative in sources:
        target = out / "frozen-source" / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(root / relative, target)
        source_receipt[relative] = digest(target)
    (out / "source-receipt.json").write_text(json.dumps(source_receipt, indent=2) + "\n")
    records = []

    def run(name, command):
        started = datetime.datetime.now(datetime.timezone.utc).isoformat()
        before = time.monotonic()
        timed_out = False
        try:
            result = subprocess.run(command, capture_output=True, timeout=30, check=False)
            stdout, stderr, code = result.stdout, result.stderr, result.returncode
        except subprocess.TimeoutExpired as error:
            stdout, stderr, code = error.stdout or b"", error.stderr or b"", None
            timed_out = True
        (out / (name + ".stdout")).write_bytes(stdout)
        (out / (name + ".stderr")).write_bytes(stderr)
        records.append({"name": name, "command": command, "utc": started,
                        "exit_code": code, "timeout": timed_out,
                        "elapsed_seconds": time.monotonic() - before})
        (out / "raw-results.json").write_text(json.dumps(records, indent=2) + "\n")
        print(name, "exit=", code, "timeout=", timed_out, flush=True)
        return code == 0 and not timed_out

    compiler = shutil.which("clang++") or shutil.which("g++")
    if not compiler:
        raise RuntimeError("No local C++ compiler; no dependency download is allowed")
    run("compiler-readback", [compiler, "--version"])
    frozen = out / "frozen-source"
    common = [compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
              "-O1", "-g", "-fno-omit-frame-pointer", "-I" + str(frozen / "src"),
              str(frozen / "src/audio/shadow_voice.cpp"),
              str(frozen / "tests/shadow_voice_test.cpp")]
    okay = True
    for label, options in [("plain", []), ("asan-ubsan", ["-fsanitize=address,undefined"])]:
        binary = out / ("shadow-voice-" + label)
        compiled = run("compile-" + label, common + options + ["-o", str(binary)])
        okay = compiled and okay
        if compiled:
            okay = run("run-" + label, [str(binary)]) and okay
    # Read-only availability check, not a VAD attempt and never an ABI shim.
    package_config = shutil.which("pkg-config")
    has_package = False
    if package_config:
        has_package = run("webrtc-package-readback", [package_config, "--exists",
                                                      "webrtc-audio-processing"])
    (out / "backend-boundary.json").write_text(json.dumps({
        "injected_fixture_only": True,
        "pkg_config_tool_available": bool(package_config),
        "host_webrtc_pkg_config_available": has_package,
        "real_aec_runtime": "NOT_RUN_NO_REAL_BACKEND_CONNECTED",
        "real_vad_runtime": "NOT_RUN_NO_REAL_BACKEND_CONNECTED",
        "adc_timestamp": "UNKNOWN_LOCAL_DELIVERY_TIME_ONLY",
        "acoustic_delay_ms": "UNKNOWN_NOT_ZERO",
        "app_mic_or_playback_connection": False,
        "cancel_or_speech_confirmed_or_cloud_audio": False,
        "device_ssh_arm_package_publish": False,
    }, indent=2) + "\n")
    raise SystemExit(0 if okay else 1)


if __name__ == "__main__":
    main()
