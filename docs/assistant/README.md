# Rack Assistant - User Guide

The Rack Assistant is a chat panel built into this VCV Rack 2 fork. You describe what you want
("build an acid bassline with a resonant filter", "what does this patch do?") and an LLM reads
and edits the current patch through tools: it can search installed modules, add, remove and move
modules, set parameters, connect and disconnect cables, and save the patch. Everything it changes
is undoable. For the internals see [ARCHITECTURE.md](ARCHITECTURE.md).

Privacy: your chat messages and the patch contents the model asks for (module list, parameters,
cables, selected modules, installed plugin names) are sent to the configured provider. Nothing is
sent until you send a message. Use a local server or mock mode if that is not acceptable.

## Building

```sh
git submodule update --init --recursive   # needed for plugins that use submodules
make dep                                  # once; builds dep/ (slow)
make -j4                                  # builds ./Rack
```

Plugins are built separately in place under `plugins/` (dev mode `./Rack -d` loads them from there):

```sh
cd plugins/Fundamental
RACK_DIR=../.. make -j4
cd ../AudibleInstruments
git submodule update --init --recursive   # AudibleInstruments needs its eurorack submodule
RACK_DIR=../.. make -j4
```

Run in development mode with `./Rack -d` (user dir is the repo root, so `assistant.json` etc. live
there; they are git-ignored).

Version note: Rack derives its version from `git describe --match "v2.*"`. A fork without version
tags gets an empty version and the Core plugin refuses to load (no Audio/MIDI modules). The Makefile
therefore falls back to the newest `### 2.x.y` heading in `CHANGELOG.md`. If Core is still empty,
build with `make RACK_VERSION=2.6.4`.

## API keys

The key is looked up in this order (first non-empty wins, whitespace is trimmed):

1. env `RACK_ASSISTANT_API_KEY`
2. env `OPENROUTER_API_KEY`
3. `api_key` in `assistant.json`

Prefer the environment: `RACK_ASSISTANT_API_KEY="$(cat ~/keyfile)" ./Rack`. Never commit a key.
`assistant.json` is written with mode 600 and is git-ignored, but treat it as a secret. In the
settings dialog the key is masked (`sk-or-v1...abcd`), cannot be copied out, and a blank field keeps
the stored key ("Clear stored key" removes it). A key from the environment is shown read-only.
Local servers on `localhost` / `127.0.0.1` / `[::1]` need no key.

## Configuration: assistant.json

Location: `<Rack user dir>/assistant.json` (Linux: `~/.local/share/Rack2/`, dev mode: repo root).
Created with defaults on first start. Editable in Assistant > Settings or by hand; it is re-read
when you send a message. Invalid values fall back to the default and a warning is logged.

| Field | Default | Meaning |
|---|---|---|
| `base_url` | `https://openrouter.ai/api/v1` | OpenAI-compatible base; `/chat/completions` is appended. `mock:` prefix selects the mock client. |
| `model` | `openai/gpt-5.6-terra` | Model id as the provider names it. |
| `reasoning_effort` | `medium` | `off`, `none`, `minimal`, `low`, `medium`, `high`. See below. |
| `reasoning_param_style` | `openrouter` | `openrouter` sends `"reasoning":{"effort":E}` and `max_tokens`; `openai` sends `"reasoning_effort":E` and `max_completion_tokens`. |
| `max_tokens` | `null` | Output cap; `null`, `0` or `""` = not sent. |
| `temperature` | `null` | 0..2; `null` or `""` = not sent (reasoning models usually reject it). |
| `extra_headers` | `HTTP-Referer`, `X-Title` | Object name -> value; replaces the defaults. Names/values with CR/LF or `:` in names are dropped. |
| `confirm_destructive` | `true` | Ask before remove_module, clear_patch, overwriting another file. |
| `max_tool_rounds` | `15` | Model round trips per message, clamped 1..100. |
| `timeout_sec` | `180` | Total timeout per request, clamped 10..1800. |
| `ca_bundle` | `""` | CA file for TLS (see Troubleshooting). Env `RACK_ASSISTANT_CA_BUNDLE` is the fallback, then Rack's `cacert.pem`. |
| `mock` | `false` | Use the offline mock client (no network, no cost). |
| `api_key` | (omitted) | Stored key; only written if non-empty. |
| `panel_width` | `400` | Panel width in px, clamped 280..1200 (drag the left edge to resize). |
| `attach_selection` | `true` | Attach selected modules to your message by default. |
| `max_context_chars` | `200000` | Older turns are dropped beyond this size (clamped 1000..100000000). |

## Provider recipes

**OpenRouter (default).** Set `OPENROUTER_API_KEY`, nothing else. Any tool-capable model works.

**OpenAI direct.**
```json
{ "base_url": "https://api.openai.com/v1", "model": "gpt-5.6-terra",
  "reasoning_param_style": "openai", "reasoning_effort": "none" }
```
OpenAI rejects function tools together with a reasoning effort on `/v1/chat/completions` for
`gpt-5.6-*`, so `reasoning_effort` must be `none`. If you forget, the 400 error is detected, the run
is retried once with `none` (remembered for the rest of the chat while the settings stay the same), and the chat tells you to change the setting.

**Local OpenAI-compatible server** (Ollama, LM Studio, llama.cpp):
`base_url` = `http://localhost:11434/v1` (or your port), `model` = the local model name,
`reasoning_effort` = `off`. No key needed for localhost. The model must support tool calling.

**`off` vs `none`.** `off` sends no reasoning parameter at all (for models that do not know it).
`none` sends the explicit value "none" (OpenAI reasoning models: reasoning disabled, tools allowed).

## Using the panel

- Ctrl+L or Assistant > Show assistant toggles the docked panel on the right; the rack stays usable.
- Type in the box: Enter sends, Shift+Enter inserts a newline, Escape leaves the field.
- Selection: modules you have selected are attached as context ("Selection: N" button toggles it).
- Confirmations: destructive calls show Allow / Deny buttons in the chat. If the panel is hidden when
  a question arrives it re-opens automatically.
- Undo: a whole run is one undo step. Ctrl+Z works over the rack, and in the chat input while it is empty (with text in it, Ctrl+Z is ignored so typing never undoes patch edits). "Undo changes" appears after a run when its
  changes are still the newest history step. Loading or creating a patch cancels a running request.
- Send turns into Cancel while a request runs. New (or Assistant > New chat) clears the conversation.
- Right-click a message > Copy text. "Reasoning" on a reply expands the model's reasoning text.
- Saving: `save_patch` accepts only relative file names; they are stored under the user `patches/`
  folder (`.vcv` appended unless the name already ends in it, no `..`, symlinks may not lead out of that folder).
- Assistant > Open user folder opens the directory with your config files.

### System prompt

`assistant-system-prompt.md` in the user dir is created from the built-in default on first use. Edit it
to change tone, add sound-design knowledge or house rules; it is re-read on each message. Delete the
file to restore the default. A short environment block (Rack version, installed plugins) is appended
automatically.

## Mock mode

Enable with `"mock": true`, the Settings checkbox, or `"base_url": "mock:"`. No network. Scenarios
are chosen by words in your message:

| Message contains | Behavior |
|---|---|
| `/mock-build`, `build`, `bau`, `demo`, `acid` | Builds a Fundamental VCO -> VCF and sets the cutoff (needs Fundamental). |
| `/mock-delete` | Reads the patch and removes its first module (triggers a confirmation). |
| `/mock-badargs` | Calls a tool with invalid JSON arguments. |
| `/mock-loop` | Calls `get_patch` forever (tests `max_tool_rounds`). |
| `/mock-error 402` | Returns that HTTP error. |
| anything else | `Mock reply: <your text>` |

Custom script: put `assistant-mock.json` in the user dir:
```json
{ "responses": [
  { "choices": [ { "message": { "role": "assistant", "content": null,
      "tool_calls": [ { "id": "c1", "type": "function",
        "function": { "name": "get_patch", "arguments": "{}" } } ] },
      "finish_reason": "tool_calls" } ] },
  { "choices": [ { "message": { "role": "assistant", "content": "Done." },
      "finish_reason": "stop" } ] }
] }
```
Entries are standard chat-completion responses, returned in order (index = number of assistant
messages in the request, clamped to the last). When the file exists it replaces the built-in scenarios.

## Troubleshooting

| Message | Cause / fix |
|---|---|
| No API key configured... | Set `RACK_ASSISTANT_API_KEY` / `OPENROUTER_API_KEY` or enter a key (remote hosts only). |
| The API key contains line breaks... | Key has control characters; re-copy it. |
| Authentication failed (401) | Wrong or expired key. |
| Insufficient credits (402) | Top up or choose a cheaper model. |
| Access denied (403) | Provider policy or region; see the detail text. |
| Model or endpoint not found (404) | Check `model` and `base_url`. |
| Rate limited (429) / server error (5xx) | Wait and retry. |
| The provider rejected the request (400) | Check temperature, max_tokens, reasoning settings; OpenAI + tools needs effort `none`. |
| TLS error ... set 'ca_bundle' | A proxy intercepts TLS: set `ca_bundle` or `RACK_ASSISTANT_CA_BUNDLE` to the proxy CA file (e.g. `/etc/ssl/certs/ca-certificates.crt`). `HTTPS_PROXY` is honored. |
| Could not connect to host | DNS/firewall/proxy, or the local server is not running. |
| The request timed out | Raise `timeout_sec` or use a faster model. |
| The response was cut off (max_tokens) | Raise or unset `max_tokens`. |
| Stopped after N tool rounds | Raise `max_tool_rounds` or split the request. |
| Module not found / unknown tag | The module is not installed; the model is told and picks alternatives. |
| No audio/MIDI modules, Core is empty | Rack has no version (no git tags). See the version note under Building. |

## Development

- Unit tests (Linux): `make test-assistant`. Filter: `make test-assistant ASSISTANT_TEST_ARGS="--filter http"`.
  Covers protocol, config, argument helpers, mock client and the HTTP client against a local fake server.
- Live tests (cost money, off by default): `ASSISTANT_TEST_ARGS=--live`, i.e.
  `RACK_ASSISTANT_API_KEY=... make test-assistant ASSISTANT_TEST_ARGS="--live --filter live"`.
  Env: `RACK_ASSISTANT_API_KEY` (required), `RACK_ASSISTANT_BASE_URL`, `RACK_ASSISTANT_MODEL`,
  `RACK_ASSISTANT_PARAM_STYLE`, `RACK_ASSISTANT_REASONING_EFFORT` (default `low`),
  `RACK_ASSISTANT_CA_BUNDLE`. Output never contains the key or request bodies.
- In-app self test: `tests/assistant/run_selftest.sh [tools|mock|all]` starts Rack under Xvfb with a
  temporary user dir, runs the tool and mock-controller scenarios and exits non-zero on failure.
  Same as `RACK_ASSISTANT_SELFTEST=all ./Rack -d -u <tmpdir>`.
- Architecture and implementation notes: [ARCHITECTURE.md](ARCHITECTURE.md).
