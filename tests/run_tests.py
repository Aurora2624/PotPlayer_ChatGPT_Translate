#!/usr/bin/env python3
"""Compile both production scripts in AngelScript and validate captured JSON.

Requires Python 3, g++, and the unmodified official AngelScript 2.38.0 SDK.
All HTTP and persistent storage are in-memory fixtures. No provider is contacted.
"""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent


def reject_duplicate_keys(pairs):
    obj = {}
    for key, value in pairs:
        if key in obj:
            raise AssertionError(f"Duplicate JSON key: {key}")
        obj[key] = value
    return obj


def validate_payload(kind: str, mode: str, raw: str) -> None:
    data = json.loads(raw, object_pairs_hook=reject_duplicate_keys)
    assert isinstance(data, dict), "Request must be an object"
    responses = kind.startswith("responses")
    field = "input" if responses else "messages"
    allowed = {"model", field}
    assert isinstance(data["model"], str)
    assert len(data[field]) == 2
    assert [message["role"] for message in data[field]] == ["system", "user"]
    if responses:
        for message in data[field]:
            assert len(message["content"]) == 1
            assert message["content"][0]["type"] == "input_text"
            assert isinstance(message["content"][0]["text"], str)
    else:
        assert all(isinstance(message["content"], str) for message in data[field])
    if mode == "auto":
        assert "thinking" not in data and "reasoning" not in data
    elif responses:
        assert data["reasoning"] == {"effort": "none" if mode == "disabled" else "high"}
        assert "thinking" not in data
        allowed.add("reasoning")
    else:
        assert data["thinking"] == {"type": mode}
        assert "reasoning" not in data
        allowed.add("thinking")
    if kind in {"chat_cache", "responses_cache", "chat_has_cache"}:
        allowed |= {"prompt_cache_key", "prompt_cache_retention"}
        assert data["prompt_cache_retention"] == "24h"
        assert data["prompt_cache_key"]
        if kind != "chat_has_cache":
            assert data["prompt_cache_key"] == "cache-key"
    if kind == "chat_cache":
        allowed.add("google")
        assert data["google"] == {"cached_content": "cachedContents/example"}
    # An exact key set detects accidental provider-forced or extra fields and
    # proves auto retains the old request shape (including requested caches).
    assert set(data) == allowed, f"Unexpected keys: {set(data) ^ allowed}"
    if kind.endswith("_escaped"):
        assert data["model"] == 'model"/\\'
        contents = [message["content"] for message in data[field]]
        if responses:
            contents = [value[0]["text"] for value in contents]
        assert contents == ['line1\nline2\t"\\/', 'text\r\n"\\/']


def build_runner(sdk: Path, output: Path) -> None:
    include = sdk / "angelscript/include"
    header = include / "angelscript.h"
    if not header.exists():
        raise SystemExit(f"SDK not found at {sdk}; expected sdk/angelscript/include/angelscript.h")
    if '#define ANGELSCRIPT_VERSION_STRING "2.38.0"' not in header.read_text():
        raise SystemExit("Use the official AngelScript SDK 2.38.0 for reproducible results")
    addons = sdk / "add_on"
    command = [os.environ.get("CXX", "g++"), "-std=c++11", "-O1", "-pthread",
               f"-I{include}", f"-I{addons / 'scriptstdstring'}", f"-I{addons / 'scriptarray'}",
               str(HERE / "angelscript_runner.cpp"),
               str(addons / "scriptstdstring/scriptstdstring.cpp"),
               str(addons / "scriptarray/scriptarray.cpp")]
    # Reuse a locally built official SDK library if present; otherwise compile
    # the SDK in the temporary output directory, without modifying the SDK.
    library = sdk / "angelscript/lib/libangelscript.a"
    if library.exists():
        command.append(str(library))
    else:
        command += [str(path) for path in sorted((sdk / "angelscript/source").glob("*.cpp"))]
    command += ["-o", str(output)]
    subprocess.run(command, check=True, cwd=output.parent)


def run_suite(runner: Path) -> None:
    total_assertions = 0
    total_payloads = 0
    variants = [
        ("without-context", "SubtitleTranslate - ChatGPT - Without Context.as", "test_thinking_without_context.as"),
        ("context", "SubtitleTranslate - ChatGPT.as", "test_thinking_context.as"),
    ]
    for label, script, test in variants:
        completed = subprocess.run([str(runner), str(ROOT / script),
                                    str(HERE / "potplayer_stubs.as"),
                                    str(HERE / "test_thinking_common.as"), str(HERE / test)],
                                   text=True, capture_output=True, timeout=30)
        if completed.returncode:
            raise SystemExit(f"{label}: failed\n{completed.stdout}\n{completed.stderr}")
        assertions = payloads = 0
        for line in completed.stdout.splitlines():
            if line.startswith("PAYLOAD\t"):
                _, kind, mode, raw = line.split("\t", 3)
                try:
                    validate_payload(kind, mode, raw)
                except (AssertionError, ValueError, KeyError, TypeError) as error:
                    raise AssertionError(f"{label}: {kind}/{mode}: {error}\n{raw}") from error
                payloads += 1
            elif line.startswith("PASS "):
                assertions = int(re.fullmatch(r"PASS (\d+) AngelScript assertions", line).group(1))
        assert assertions > 0 and payloads > 0, "Runner reported no tests"
        total_assertions += assertions
        total_payloads += payloads
        print(f"PASS {label}: {assertions} AngelScript assertions; {payloads} JSON payloads validated")
    print(f"PASS total: {total_assertions} runtime assertions; {total_payloads} JSON payloads")
    print("Boundary: mocked PotPlayer Host/response JSON; no Windows PotPlayer or live API verification")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sdk", type=Path, default=os.environ.get("ANGELSCRIPT_SDK"),
                        help="Path to the official SDK's sdk directory (or ANGELSCRIPT_SDK)")
    parser.add_argument("--runner", type=Path, help="Reuse an already compiled test runner")
    args = parser.parse_args()
    if args.runner:
        run_suite(args.runner.resolve())
    elif args.sdk:
        with tempfile.TemporaryDirectory(prefix="potplayer-angelscript-tests-") as temp:
            runner = Path(temp) / "runner"
            build_runner(args.sdk.resolve(), runner)
            run_suite(runner)
    else:
        parser.error("Supply --sdk /path/to/sdk or set ANGELSCRIPT_SDK")


if __name__ == "__main__":
    main()
