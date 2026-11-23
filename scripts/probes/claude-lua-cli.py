#!/usr/bin/env python3
"""Owned stream-json CLI fixture for the native Lua integration test."""
import json
from pathlib import Path
import sys
import time


def emit(document):
    print(json.dumps(document, ensure_ascii=False), flush=True)


def main():
    arguments = sys.argv[1:]
    if arguments == ["--version"]:
        print("2.1.323 (Claude Code)")
        return
    assert "--print" in arguments and "--restricted" in arguments
    assert arguments[arguments.index("--tools") + 1] == ""
    assert arguments[arguments.index("--output-format") + 1] == "stream-json"
    assert Path.cwd().name == "w" and Path.cwd().parent.name.startswith("claude-")
    prompt = sys.stdin.read()
    if prompt in ("interrupt this owned fixture", "stop this owned fixture"):
        Path("waiting-stop" if prompt.startswith("stop") else "waiting").touch()
        time.sleep(90)
        raise RuntimeError("expected owned interrupt")
    assert prompt == "explicit CLI fixture 中文😀"
    emit({"type": "assistant", "message": {"content": [{"type": "text", "text": "完整响应😀" * 400}]}})
    emit({"type": "result", "subtype": "success", "is_error": False, "result": "fixture completed"})


if __name__ == "__main__":
    main()
