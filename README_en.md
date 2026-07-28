> Advertisement: for model deployment optimization, model acceleration (cloud, device-side, edge-side), and computer vision needs, contact zhuilewang@163.com
> For industries and manufacturers: industrial inspection, surface defect detection, SOP behavior detection, and more

# OpenAI Reasoning Guard

[简体中文](README.md) | English

OpenAI Reasoning Guard is a local quality gateway placed between Codex or another OpenAI-compatible client and the real upstream API. It targets intermittent model degradation, empty text, truncated streams, missing usage, and abnormal completion under otherwise identical requests. Suspicious responses are discarded and retried inside the gateway; only responses that pass inspection are returned to the client.

## Core Features

- **Degradation interception**: inspects `reasoning_tokens` in OpenAI usage metadata and guards suspicious fixed values such as `516,1034,1552` by default.
- **Completeness protection**: validates JSON and streaming SSE for empty text, missing usage, missing terminal events, `failed/error` events, and unexpected disconnects.
- **Transparent internal retries**: Guard matches, first-token timeouts, and explicit upstream capacity errors share one retry budget; intermediate failures stay hidden from the client.
- **Faithful final errors**: forwards the final real upstream error when retries still fail; local Guard failures use explicit status codes and `error_type` values.
- **Resource and timeout protection**: request/response size limits, buffer timeout, first-token timeout, total upstream timeout, and upstream cancellation after a client disconnect.
- **Complete HTTP request support**: handles `Content-Length` and `Transfer-Encoding: chunked` for Responses and Chat Completions routes.
- **Multiple upstream profiles**: SQLite-backed Base URLs, API keys, User-Agents, proxies, and timeouts with selection, search, pagination, import, and export.
- **Cross-platform delivery**: GUI and CLI builds with Linux deb/rpm/AppImage, Windows installer/portable, and macOS shell installers.

![OpenAI Reasoning Guard GUI](docs/images/gui-main-window.png)

> The screenshot uses an isolated demo profile. URLs and local paths are redacted.

## Download and Quick Start

Download the matching `latest` or `nightly` package from [GitHub Releases](https://github.com/thb1314/openai-reasoning-guard/releases). Linux packages include deb, rpm, and GUI/CLI AppImages; Windows provides installer and portable zip packages; macOS provides Intel and Apple Silicon shell installers.

Launch the GUI after installation:

```bash
openai-reasoning-guard-gui
```

For first-time CLI use, create and select an upstream profile, then start the local proxy:

```bash
openai-reasoning-guard-cli profile add \
  --name "Primary" \
  --base-url https://api.openai.com/v1 \
  --api-key sk-example

openai-reasoning-guard-cli --proxy-host 127.0.0.1 --proxy-port 8010
```

Point Codex or another client at `http://127.0.0.1:8010/v1`. A non-empty profile API key always overrides client authorization. An empty key forwards the client `Authorization`, so Codex can continue using its own `auth.json`.

<details>
<summary><strong>How It Works and reasoning_tokens</strong></summary>

### Request Flow

1. The client sends an OpenAI-compatible request to the local Guard.
2. The Guard forwards the request and inspects the JSON or SSE response structure.
3. Responses API reads `usage.output_tokens_details.reasoning_tokens`; Chat Completions reads `usage.completion_tokens_details.reasoning_tokens`.
4. A `reasoning_equals` match, incomplete stream, first-token timeout, or explicit capacity error discards the current upstream result and retries within the shared budget.
5. Only a response that passes inspection reaches the client. After exhaustion, the Guard returns a local Guard error or faithfully forwards the final real upstream status, headers, and body.

`reasoning_tokens` is the reasoning-token count returned by OpenAI in `usage` metadata. It is not the visible answer's character length, and this project does not recalculate it with its own tokenizer. For example:

```json
{
  "usage": {
    "output_tokens": 1186,
    "output_tokens_details": {
      "reasoning_tokens": 516
    }
  }
}
```

Observed values `516`, `1034`, and `1552` follow `518*n - 2` and frequently coincide with abnormal or clearly under-reasoned responses, so they form the default Guard set. The rule is fully configurable, and streaming or non-streaming interception can be disabled independently.

Strict streaming mode buffers the complete SSE response and writes it only after terminal events, usage, and content structure are validated. This allows a late usage match to discard the whole response and retry, instead of exposing half an answer. With `stream_action=disconnect`, responses are still fully discarded while retry budget remains; pass-through scanning is only used after exhaustion, disconnecting on a later match.

The project does not change the model, modify prompts, or translate model protocols. It only supplies an observable and configurable quality gate in the request path.

References:

- [OpenAI Reasoning models](https://developers.openai.com/api/docs/guides/reasoning)
- [OpenAI Chat Completions API Reference](https://developers.openai.com/api/reference/resources/chat/subresources/completions/methods/create)

</details>

<details>
<summary><strong>Data Files, Global Options, and Upstream Fields</strong></summary>

### Data Files

- `config.json`: global listen address, buffer limits, Guard policy, language, and font settings.
- `upstream-profiles.sqlite3`: upstream profiles, API keys, and the last selected profile UUID.

| Platform | Default directory |
| --- | --- |
| Linux | `${XDG_CONFIG_HOME:-$HOME/.config}/OpenAI Reasoning Guard/` |
| Windows | `%APPDATA%\OpenAI Reasoning Guard\` |
| macOS | `~/Library/Application Support/OpenAI Reasoning Guard/` |

Use `--config /path/config.json` or `NET_TUNNEL_CONFIG=/path/config.json` to choose another location; the SQLite database is placed beside it. Configuration writes are atomic. New configuration, database, backup, and export files are owner-only on POSIX systems.

### Global Configuration Example

```json
{
  "lang": "zh",
  "ui_font_point_size": 0,
  "proxy_host": "127.0.0.1",
  "proxy_port": "8010",
  "proxy_prefix": "/v1",
  "buffer_timeout_sec": 180,
  "request_body_limit_bytes": 104857600,
  "response_buffer_limit_bytes": 104857600,
  "intercept_rule_mode": "reasoning_tokens",
  "reasoning_equals": [516, 1034, 1552],
  "guard_retry_attempts": 3,
  "retry_upstream_capacity_errors": true,
  "guard_endpoints": ["/responses", "/chat/completions", "/v1/responses", "/v1/chat/completions"],
  "intercept_streaming": true,
  "intercept_non_streaming": true,
  "non_stream_status_code": 502,
  "stream_action": "strict_502"
}
```

### Global Options

| Field | Default | Example | Description |
| --- | --- | --- | --- |
| `lang` | `zh` | `"en"` | GUI language; display only. |
| `ui_font_point_size` | `0` | `14` | `0` follows the system font; fixed range is `8..20 pt`. |
| `proxy_host` | `127.0.0.1` | `"0.0.0.0"` | Local listen address; configure a firewall before exposing it. |
| `proxy_port` | `8010` | `8011` | Local listen port. |
| `proxy_prefix` | `/v1` | `""` | Business path prefix; empty enables root proxy and forwards `GET /`. |
| `buffer_timeout_sec` | `180` | `360` | Request/response buffering timeout in seconds; records `buffer_timeout`. |
| `request_body_limit_bytes` | `104857600` | `10485760` | Request buffer limit; exceeding it returns `413`. |
| `response_buffer_limit_bytes` | `104857600` | `209715200` | Response buffer limit; exceeding it returns `502`. |
| `intercept_rule_mode` | `reasoning_tokens` | `"final_answer_only_high_xhigh"` | Guard decision mode; the latter is an experimental high/xhigh rule. |
| `reasoning_equals` | `[516,1034,1552]` | `[516,1034]` | Exact reasoning-token values to intercept. |
| `guard_retry_attempts` | `3` | `10` | Shared internal retry budget for Guard, first-token, and capacity failures. |
| `reasoning_516_retry_count` | `3` | `10` | Compatibility field for `guard_retry_attempts`. |
| `retry_upstream_capacity_errors` | `true` | `false` | Retry explicit capacity errors without generalizing ordinary `429/502`. |
| `guard_endpoints` | four Responses/Chat paths | `["/v1/responses"]` | Business routes requiring inspection. |
| `intercept_streaming` | `true` | `false` | Actually intercept SSE; disabled mode still records observations. |
| `intercept_non_streaming` | `true` | `false` | Actually intercept non-streaming JSON. |
| `non_stream_status_code` | `502` | `503` | Local status after Guard retries are exhausted. |
| `stream_action` | `strict_502` | `"disconnect"` | Strict full-stream buffering or post-exhaustion pass-through scanning. |

### Upstream Profile Fields

| Field | Default | Example | Description |
| --- | --- | --- | --- |
| `id` | generated UUID | `"550e8400-..."` | Immutable identifier; CLI accepts either name or UUID. |
| `display_name` | required | `"Primary"` | Case-insensitively unique, up to `512` UTF-8 bytes. |
| `base_url` | required | `"https://api.openai.com/v1"` | HTTP/HTTPS Base URL; credentials, query, and fragment are forbidden. |
| `api_key` | empty | `"sk-..."` | Non-empty overrides authorization; empty forwards client `Authorization`. |
| `user_agent` | `curl/8.7.1` | `"codex-cli/0.1"` | User-Agent sent upstream when client UA forwarding is disabled. |
| `forward_user_agent` | `false` | `true` | Prefer the client's User-Agent. |
| `upstream_proxy` | empty | `"http://127.0.0.1:7890"` | One HTTP/SOCKS5-family proxy; empty means direct. |
| `upstream_timeout_sec` | `1800` | `600` | Total timeout for one upstream attempt, range `1..86400`; returns `504`. |
| `first_token_timeout_sec` | `30` | `10` or `0` | Wait for the first non-empty streaming body byte; `0` disables, range `0..3600`. |

API keys are stored as plaintext in the current user's SQLite database, not in a system keychain. Lists, logs, status endpoints, and normal exports never expose a complete key. Only explicit `--show-secret` or `--include-secrets` operations reveal one.

Legacy JSON upstream fields are transactionally migrated into SQLite on first startup with a backup. The macOS installer migrates a bundle-local `config.json` before replacing an old app and never overwrites an existing user configuration.

</details>

<details>
<summary><strong>CLI Profile Management and Common Commands</strong></summary>

| Command | Example | Purpose |
| --- | --- | --- |
| `profile list` | `profile list --page 1 --page-size 20 --search openai` | Paginate, search, and sort profiles. |
| `profile show` | `profile show "Primary" --json` | Inspect one profile; add `--show-secret` for the complete key. |
| `profile add` | `profile add --name "Primary" --base-url https://api.example.com/v1` | Add a profile; the first becomes current automatically. |
| `profile update` | `profile update "Primary" --proxy http://127.0.0.1:7890` | Update supplied fields only; `--proxy=` selects direct access. |
| `profile delete` | `profile delete "Backup"` | Delete an inactive profile and select the next one when needed. |
| `profile select` | `profile select "Primary"` | Persist the current selection. |
| `profile export` | `profile export --output profiles.json` | Omit keys by default; `--include-secrets` includes them explicitly. |
| `profile import` | `profile import --input profiles.json --conflict overwrite` | Transactional import with `skip` or `overwrite` conflicts. |

Common runtime commands:

```bash
# Start with the selected upstream profile
openai-reasoning-guard-cli --proxy-host 127.0.0.1 --proxy-port 8010

# One-time overrides; nothing is written to SQLite
openai-reasoning-guard-cli \
  --upstream-base-url https://api.example.com/v1 \
  --upstream-api-key sk-example

# Query runtime status
openai-reasoning-guard-cli --query-status \
  --query-url http://127.0.0.1:8010/status
```

`--keep-config` persists global listen, buffering, and Guard options only; it never saves temporary `--upstream-*` overrides. Every command accepts `--config <path>`, and profile management commands support `--json`.

</details>

<details>
<summary><strong>Control Endpoints, Statistics, and Error Classes</strong></summary>

### Control Endpoints

- `GET /healthz`: health check and current configuration summary.
- `GET /status` or `/metrics`: health data and runtime statistics.
- `GET /version` or `/v1/version`: version and configuration capabilities.
- `GET /props` or `/v1/props`: client-discoverable feature switches.

With an empty `proxy_prefix`, `GET /` is forwarded to the upstream root and is not reserved for health checks.

### Main Statistics

- `requests_total`: all control and business requests.
- `control_requests_total`: control endpoint requests.
- `intercepted_requests_total`: business requests entering the proxy path.
- `successful_requests_total` / `failed_requests_total`: final results for completed business requests.
- `in_flight_proxy_requests`: unfinished business requests; success plus failure may therefore temporarily be below the business-request count.
- `upstream_attempts_total`: real upstream attempts, including internal retries.
- `inspected_response_count` / `matched_response_count`: inspection and Guard-match counts.
- `guard_match_rate`: `matched_response_count / inspected_response_count`.
- `blocked_response_count`: client responses finally blocked after budget exhaustion; not the same as matches.
- `guard_retry_total`: retries triggered by Guard or protected failures.
- `buffer_timeout_total` / `first_token_timeout_total` / `upstream_timeout_total`: timeout counters.
- `client_connection_error_total`: early client disconnects and connection failures.
- `local_proxy_error_total`: local limits and bad requests, excluded from upstream HTTP errors.
- `status_code_counts`, `observed_reasoning_counts`, `last_result`, `last_failure`: status, token distribution, and latest outcome data.

</details>

<details>
<summary><strong>Source Build, Tests, Packaging, and GitHub Actions</strong></summary>

The project uses C++11, Qt 5.15.x, and CMake. The Qt SDK must include Core, Network, Gui, Widgets, Sql, QtTest for tests, and the QSQLITE driver.

```bash
cmake -S . -B build \
  -DNET_TUNNEL_QT_SDK_ROOT=/path/to/qt5 \
  -DNET_TUNNEL_BUILD_TESTS=ON
cmake --build build -j2
ctest --test-dir build --output-on-failure
```

Local development defaults to the self-built Qt5 under `/mnt/data/qt-2080ti-sync` and never silently falls back to system Qt5.

Local packaging entry points:

```bash
QT_ROOT=/path/to/qt5 scripts/package-linux.sh --all --clean
QT_ROOT=/path/to/qt5 scripts/package-windows-mingw.sh --arch x86_64 --clean
QT_ROOT=/path/to/qt5 scripts/package-macos.sh --arch aarch64 --clean
```

- Linux: x86_64, x86_32, ARM64, and ARM32 with deb, rpm, and GUI/CLI AppImages.
- Windows: x86_64, x86_32, and ARM64 with installer exe and portable zip packages.
- macOS: Intel and Apple Silicon shell installers containing the app, CLI, and temporary DMG payload.

GitHub Actions:

- [Qt SDK Archives](.github/workflows/qt-sdk.yml): builds reusable Qt SDK archives from Qt/OpenSSL source.
- [OpenAI Reasoning Guard Packages](.github/workflows/linux-packages.yml): builds, tests, and publishes every platform on native runners or cross toolchains.

Final release publication requires `target=all` and validates the source tag plus the names and sizes of all 24 assets. See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) for architecture details and [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) for bundled dependencies.

</details>

## License

The project source is licensed under MIT. Bundled QUI components and font resources remain under their respective upstream licenses.
