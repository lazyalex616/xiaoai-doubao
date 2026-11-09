#!/usr/bin/env python3
"""Guard the native-media side effect against accidental playback-path reuse."""

from pathlib import Path

source = (Path(__file__).resolve().parents[1] / "src/app/app.cpp").read_text()


def body(signature: str) -> str:
    start = source.index(signature)
    opening = source.index("{", start)
    depth = 1
    for index in range(opening + 1, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[opening + 1:index]
    raise AssertionError(f"unclosed method: {signature}")


pause = "PauseMphelperBounded();"
assert source.count(pause) == 1, "native media pause must have one call site"
start = body("void App::ControlLoop()")
assert start.count(pause) == 1
assert start.index("client_->StartSession(") < start.index(pause) < start.index("client_->SendSayHello()")
assert "native_wake_monitor_->Blocked()" in start[start.index("client_->StartSession("):start.index(pause)]

interrupt = body("void App::PlaybackControlLoop()")
assert "player_->Interrupt()" in interrupt and pause not in interrupt
native = body("void App::OnNativeWake()")
assert "InterruptPlayback();" in native and 'gate_->Disarm("native_xiaoai_wake")' in native
for method in (
    "void App::OnNativeWake()",
    "void App::OnWakeInterrupt(const std::string&)",
    "void App::OnServerExitIntent()",
):
    assert pause not in body(method), method
print("P3f media routing PASS: one new-session pause; local interruption and exits leave native media alone")
