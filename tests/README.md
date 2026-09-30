# Thinking-mode regression tests

These tests compile **both complete production `.as` scripts unchanged** with the
real AngelScript 2.38.0 compiler and execute their configuration, login, logout,
payload, translation, fallback, and retry functions. Python independently parses
captured request JSON and checks its exact keys, values, and escaping.

## Run

Requirements: Python 3.9+, a C++11 compiler (`g++` by default; `CXX` may override),
and the official [AngelScript 2.38.0 SDK](https://www.angelcode.com/angelscript/downloads.html).

Download/extract the SDK outside the repository, then run from the repository root:

```sh
curl -fsSLO https://www.angelcode.com/angelscript/sdk/files/angelscript_2.38.0.zip
printf '%s\n' 'b33b5dbcda10317ef67d628353d83246984ce6fcac102d4dc2aed121eba52e6f  angelscript_2.38.0.zip' | sha256sum -c -
unzip angelscript_2.38.0.zip -d /tmp/potplayer-angelscript
python3 tests/run_tests.py --sdk /tmp/potplayer-angelscript/sdk
```

Alternatively set `ANGELSCRIPT_SDK` to that SDK directory. The test runner builds
in a temporary directory and removes its build outputs afterward. If an official
SDK static library already exists at `sdk/angelscript/lib/libangelscript.a`, it is
reused; otherwise SDK sources are compiled directly. `--runner /path/to/runner`
can reuse a previously built `angelscript_runner.cpp` executable. No SDK files,
third-party source, binaries, API keys, or HTTP logs are committed.

## Coverage

Both variants:

- Normalization of `auto`, `enabled`, and `disabled`, including whitespace/case
- Rejection of empty, boolean, misspelled, and otherwise invalid explicit values
  before any request, preserving the saved and active mode
- Thinking option before/after URL and alongside existing key/delay/retry options
- Successful validation stores the selected mode, and refresh restores it
- Provider rejection, malformed response, and failed URL correction preserve mode
- Omitted option and explicit `thinking=auto` reset a previous override
- Chat requests use `thinking.type`; `auto` emits neither thinking nor reasoning
- Model names alone do not add a thinking field
- JSON escaping and original model/messages request shape
- Successful `/chat/completions` auto-correction preserves and persists the mode
- Logout/reset returns to the installer default, and variants use distinct keys
- Actual `Translate` requests preserve the saved setting

Context variant:

- Responses payloads use `reasoning.effort=none/high` and omit Chat thinking fields
- Actual `Translate` → Responses requests for all three modes
- Responses failure → Chat fallback preserves the selected mode
- Prompt-cache field rejection → rebuilt Chat retry preserves the selected mode
  while dropping unsupported cache metadata
- Existing prompt-cache and Gemini cached-content fields coexist unchanged

## Boundaries

This is **mocked-host runtime testing**, not Windows/PotPlayer integration or live
provider acceptance testing. The C++ adapter supplies PotPlayer-style string
methods; the AngelScript stub supplies in-memory settings and queued HTTP results.
A small fixture-based `JsonValue`/`JsonReader` models the response shapes exercised
by these tests, rather than PotPlayer's JSON implementation. The Python standard
library parses every captured request as real JSON, rejects duplicate keys, and
checks the expected schema. No network request is made by the test suite.

The production scripts are read directly each run: no production logic is copied
or translated into another language. All script syntax and function signatures
are compiled, including code branches beyond the focused runtime assertions.
