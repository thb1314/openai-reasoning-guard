> Advertisement: for model deployment optimization, model acceleration (cloud, device-side, edge-side), and computer vision related needs, please contact zhuilewang@163.com
> For industries and manufacturers: industrial inspection, surface defect detection, SOP behavior detection, and more.


# OpenAI Reasoning Guard

[简体中文](README.md) | English

This project is mainly used to mitigate GPT model "degradation" issues in Codex / OpenAI-compatible API calls: with the same model and the same request, the upstream service may sometimes return responses that are clearly under-reasoned, structurally abnormal, empty, missing usage, or suspected to be low quality. If the client accepts such responses directly, the answer may become shorter, the logic may become worse, the task may be interrupted, or the result may be empty.

This project sits between the client and the real upstream service as a local intelligent gateway. The client still requests a local OpenAI-compatible address, and the proxy forwards the request to the upstream service; when the proxy determines that the current response is suspected to be degraded or unusable, it swallows this response locally and retries the upstream request. Only responses that pass the checks are returned to the client.

This project is an independent Qt/C++11 local OpenAI-compatible intelligent proxy implementation managed by CMake, and it produces both CLI and GUI executables.
It forwards requests from Codex or other OpenAI-compatible clients to the real upstream service, and locally checks `reasoning_tokens`, streaming SSE structure, and upstream exceptions in the response; when necessary, the proxy retries internally to avoid handing suspicious responses directly to the client.



## How It Works

> When the reason token length is 518*n - 2, GPT is very likely to be in a degraded state.

The core judgment signal comes from `usage.output_tokens_details.reasoning_tokens` in the response. In existing observations, fixed reasoning token values such as `516`, `1034`, and `1552` are often related to low-quality or abnormal responses, so they are used as the default guard set. After the proxy parses a JSON or SSE response, if these values are hit, it retries the request; only after retries are exhausted does it return the configured local error status.

This project does not change the model, modify the prompt, or perform protocol conversion. It solves the quality gatekeeping problem in the request path: by using observable response structure and reasoning token signals, it blocks suspected degraded responses before they reach the client, and uses automatic retries to try to obtain a more normal upstream result.

## `reasoning_tokens` in OpenAI Responses

`reasoning_tokens` can be understood as the "number/count of reasoning tokens", but it is not the character length of the visible answer text, nor is it a length recalculated by this project from strings, byte counts, or its own tokenizer. It is a counting field returned by OpenAI in the API response `usage` metadata.

In OpenAI's official description, the reasoning token count of the Responses API appears in `usage.output_tokens_details.reasoning_tokens`. An example structure is as follows:

```json
{
  "usage": {
    "input_tokens": 75,
    "output_tokens": 1186,
    "output_tokens_details": {
      "reasoning_tokens": 1024
    },
    "total_tokens": 1261
  }
}
```

The Chat Completions API uses `usage.completion_tokens_details.reasoning_tokens`:

```json
{
  "usage": {
    "prompt_tokens": 82,
    "completion_tokens": 17,
    "total_tokens": 99,
    "completion_tokens_details": {
      "reasoning_tokens": 0
    }
  }
}
```

Therefore, this project's guard is not "calculating answer length"; it reads the usage fields returned by the upstream service: the Responses path reads `output_tokens_details.reasoning_tokens`, and the Chat Completions path reads `completion_tokens_details.reasoning_tokens`. OpenAI's official documentation also states that reasoning tokens are not returned as visible content, but they occupy the context window and are billed as output tokens.

References:

- OpenAI Reasoning models documentation: `https://developers.openai.com/api/docs/guides/reasoning`
- OpenAI Chat Completions API Reference: `https://developers.openai.com/api/reference/resources/chat/subresources/completions/methods/create`

## Components

- `openai-reasoning-guard-cli`: the installed headless command-line proxy entry point.
- `openai-reasoning-guard-gui`: the installed Qt Widgets graphical interface entry point.
- `net-tunnel-core`: the core library shared by CLI/GUI, including configuration, HTTP proxy, interception policy, and statistics logic.

Core capabilities:

- OpenAI-compatible HTTP proxy, supporting paths such as `/v1/responses` and `/v1/chat/completions`.
- JSON/SSE response buffering and inspection.
- Identifies `reasoning_tokens` by fixed OpenAI usage paths.
- Default guard set: `516,1034,1552`.
- After hitting the guard, it retries internally first; intermediate retries only count as retry, and do not count as blocked/failed; only after retries are exhausted does it return the configured error status.
- Streaming SSE anomaly detection: if a `200` stream is incomplete, lacks a terminal event, lacks usage, or contains a failed/error event, it is not passed through as success.
- Request body and response body resource limits, request/response buffer timeouts, and upstream request timeout.
- Supports both `Content-Length` and `Transfer-Encoding: chunked` request bodies.
- Supports a generic upstream proxy and split HTTP/HTTPS/SOCKS upstream proxy fields.
- Control endpoints: `/healthz`, `/status`, `/version`, `/props`.



## Download and Installation

Regular users do not need to build from source. Please download the release package for your platform from GitHub Releases:

```text
https://github.com/thb1314/openai-reasoning-guard/releases
```

It is recommended to download the asset matching your system from `latest` or `nightly`:

| Platform | Recommended asset | Installation method |
| --- | --- | --- |
| Linux Debian/Ubuntu x86_64 | `openai-reasoning-guard_<version>_amd64.deb` | `sudo apt install ./openai-reasoning-guard_<version>_amd64.deb` |
| Linux Debian/Ubuntu arm64 | `openai-reasoning-guard_<version>_arm64.deb` | `sudo apt install ./openai-reasoning-guard_<version>_arm64.deb` |
| Linux RPM-based distributions | `openai-reasoning-guard-<version>-1.<arch>.rpm` | `sudo dnf install ./openai-reasoning-guard-<version>-1.<arch>.rpm` or `sudo rpm -Uvh ...` |
| Linux portable | `openai-reasoning-guard-gui-<version>-<arch>.AppImage` / `openai-reasoning-guard-cli-<version>-<arch>.AppImage` | Run directly after `chmod +x *.AppImage` |
| Windows | `openai-reasoning-guard-windows-x86_64-<version>-installer.exe` | Double-click to install; download `portable.zip` for the portable version |
| Windows ARM64 | `openai-reasoning-guard-windows-arm64-<version>-installer.exe` | Double-click to install; download `portable.zip` for the portable version |
| macOS Apple Silicon | `openai-reasoning-guard-macos-aarch64-<version>-installer.sh` | `bash openai-reasoning-guard-macos-aarch64-<version>-installer.sh` |
| macOS Intel | `openai-reasoning-guard-macos-x86_64-<version>-installer.sh` | `bash openai-reasoning-guard-macos-x86_64-<version>-installer.sh` |

After installing Linux deb/rpm packages, you can run:

```bash
openai-reasoning-guard-gui
openai-reasoning-guard-cli --help
```

The macOS shell installer will request `sudo` permission, install the app to `/Applications/OpenAI Reasoning Guard.app`, and install the CLI wrapper script to `/usr/local/bin/openai-reasoning-guard-cli`.

## CLI

Start the intelligent proxy:

```bash
openai-reasoning-guard-cli \
  --proxy-host 127.0.0.1 \
  --proxy-port 8010 \
  --proxy-prefix /v1 \
  --upstream-base-url https://api.openai.com/v1 \
  --reasoning-equals 516,1034,1552 \
  --guard-retry-attempts 3
```

`--api-proxy` can still be passed as a compatibility flag, but the CLI currently always runs only in proxy mode. `--reasoning-516-retries` is a compatibility alias for `--guard-retry-attempts`.

Query the status of a running proxy:

```bash
openai-reasoning-guard-cli --query-status --proxy-host 127.0.0.1 --proxy-port 8010
```

You can also explicitly specify the full status URL:

```bash
openai-reasoning-guard-cli --query-status --query-url http://127.0.0.1:8010/status
```

Write the final startup configuration back to the configuration file:

```bash
openai-reasoning-guard-cli --keep-config ...
```

## GUI


The first GUI screen is divided into four areas:

- Top runtime target overview.
- Left-side intelligent interception and upstream forwarding configuration.
- Upper-right real-time statistics.
- Right-side information panel and console logs.

The information panel displays the current proxy listen address, upstream address, path prefix, control endpoints, buffer limit, stream action, and reasoning guard policy. The GUI can directly edit `request_body_limit_bytes`, `response_buffer_limit_bytes`, and `stream_action`.

The project provides a one-click restart script:

```bash
scripts/restart-gui.sh
```

The script stops existing `net-tunnel-gui` processes under the current user, inherits the display environment from the old process, and then starts the new binary.

## Configuration File

The default configuration file is located in the same directory as the executable. For development builds, it is usually:

```text
build/config.json
```

You can also explicitly specify a configuration:

```bash
openai-reasoning-guard-cli --config config.example.json --api-proxy
```

Full example:

```json
{
  "lang": "zh",
  "proxy_host": "127.0.0.1",
  "proxy_port": "8010",
  "proxy_prefix": "/v1",
  "upstream_base_url": "",
  "upstream_api_key": "",
  "upstream_user_agent": "curl/8.7.1",
  "forward_user_agent": false,
  "upstream_proxy": "",
  "upstream_http_proxy": "",
  "upstream_https_proxy": "",
  "upstream_socks_proxy": "",
  "upstream_timeout_sec": 1800,
  "buffer_timeout_sec": 180,
  "request_body_limit_bytes": 104857600,
  "response_buffer_limit_bytes": 104857600,
  "intercept_rule_mode": "reasoning_tokens",
  "reasoning_equals": [516, 1034, 1552],
  "guard_retry_attempts": 3,
  "reasoning_516_retry_count": 3,
  "retry_upstream_capacity_errors": true,
  "guard_endpoints": ["/responses", "/chat/completions", "/v1/responses", "/v1/chat/completions"],
  "intercept_streaming": true,
  "intercept_non_streaming": true,
  "non_stream_status_code": 502,
  "stream_action": "strict_502"
}
```

## Configuration Options

| Field | Default | Example | Description |
| --- | --- | --- | --- |
| `lang` | `zh` | `"en"` | GUI language. `zh` displays Chinese, and `en` displays English; it only affects the interface display and does not affect proxy behavior. |
| `proxy_host` | `127.0.0.1` | `"0.0.0.0"` | Local proxy listen address. Keep `127.0.0.1` when only local clients use it; set it to `0.0.0.0` when other machines on the LAN need access. |
| `proxy_port` | `8010` | `8011` | Local proxy listen port. The client base URL needs to use this port, for example `http://127.0.0.1:8011/v1`. |
| `proxy_prefix` | `/v1` | `""` or `"/api"` | Business path prefix used by clients to access the proxy. `"/v1"` means the client requests `/v1/responses`; an empty string means root proxy, and a request to `/responses` is forwarded directly. Under root proxy, `GET /` is forwarded to the upstream root path, and `/healthz` is used for health checks. |
| `upstream_base_url` | empty | `"https://api.openai.com/v1"` | The real upstream OpenAI-compatible API base URL. The proxy appends the business path after removing `proxy_prefix` to this base URL. When empty, no built-in upstream address is used automatically, and it must be explicitly filled in the GUI, configuration file, or CLI before starting the proxy. |
| `upstream_api_key` | empty | `"sk-..."` | Explicit upstream Bearer token. When non-empty, the upstream `Authorization` is fixed to `Bearer sk-...`; when empty, the client `Authorization` is passed through, which means Codex continues to use its own `auth.json`. |
| `upstream_user_agent` | `curl/8.7.1` | `"codex-cli/0.1"` | Default `User-Agent` sent to the upstream service. This value is used when `forward_user_agent=false`. |
| `forward_user_agent` | `false` | `true` | Whether to forward the `User-Agent` from the client request to the upstream service unchanged. When `false`, `upstream_user_agent` is used. |
| `upstream_proxy` | empty | `"http://127.0.0.1:7890"` | Generic upstream proxy address. It can also be `"socks5://127.0.0.1:7890"`. When non-empty, it takes precedence over the split proxy fields. |
| `upstream_http_proxy` | empty | `"http://127.0.0.1:7890"` | HTTP upstream proxy field. The current implementation also uses it as a fallback when the HTTPS upstream lacks `upstream_https_proxy`. It is not automatically folded into `upstream_proxy` when saving configuration. |
| `upstream_https_proxy` | empty | `"http://127.0.0.1:7890"` | HTTPS upstream proxy field. It is used preferentially only when `upstream_proxy` is empty and the upstream base URL is HTTPS. |
| `upstream_socks_proxy` | empty | `"127.0.0.1:7890"` or `"socks5://127.0.0.1:7890"` | SOCKS upstream proxy field. When no scheme is present, `socks5://` is added by default. It is the final fallback when the generic/protocol proxy fields are all empty. |
| `upstream_timeout_sec` | `1800` | `600` | Maximum wait time in seconds for a single upstream request. Timeout returns `504`, and records `error_type=upstream_timeout` and `upstream_timeout_total`. Long reasoning scenarios should keep this value relatively large. |
| `buffer_timeout_sec` | `180` | `360` | Wait time in seconds for request body buffering and upstream response buffering. If the request body is not fully received, `408` is returned; if response buffering times out, `502` is returned; both record `error_type=buffer_timeout`. |
| `request_body_limit_bytes` | `104857600` | `10485760` | Maximum buffered byte count for the client request body. The example is 10MB; the default is 100MB. If exceeded, `413` is returned and the upstream service is not requested. |
| `response_buffer_limit_bytes` | `104857600` | `209715200` | Maximum buffered byte count for the upstream response. The example is 200MB; the default is 100MB. Both strict streaming and non-streaming inspection are protected by this limit, and exceeding it returns `502`. |
| `intercept_rule_mode` | `reasoning_tokens` | `"final_answer_only_high_xhigh"` | Interception rule mode. `reasoning_tokens` matches exactly according to `reasoning_equals`; `final_answer_only_high_xhigh` is an experimental mode that uses high/xhigh and a final-answer-only structure as the match feature. |
| `reasoning_equals` | `[516,1034,1552]` | `[516, 1034]` or `"516,1034"` | Set of `reasoning_tokens` values to intercept. A JSON array is recommended in the configuration file; comma or space separation can be used in CLI/GUI. |
| `guard_retry_attempts` | `3` | `10` | Number of internal upstream retries after hitting the guard. Intermediate retries are not returned to the client; only after exhaustion is `non_stream_status_code` returned. |
| `reasoning_516_retry_count` | `3` | `10` | Compatibility field, with the same meaning as `guard_retry_attempts`. It is kept consistent with `guard_retry_attempts` when saving configuration. |
| `retry_upstream_capacity_errors` | `true` | `false` | Whether to internally retry specific upstream capacity errors. It only matches explicit capacity wording, and does not generalize retry behavior to ordinary `429/502`. |
| `guard_endpoints` | `/responses`, `/chat/completions`, `/v1/responses`, `/v1/chat/completions` | `["/responses", "/v1/responses"]` | Path set requiring reasoning guard inspection. When matching, both the original path and the business path after removing `proxy_prefix` are checked. |
| `intercept_streaming` | `true` | `false` | Whether to actually intercept streaming SSE responses. When disabled, statistics can still be observed, but hits do not block the client response. |
| `intercept_non_streaming` | `true` | `false` | Whether to actually intercept non-streaming JSON responses. When disabled, statistics can still be observed, but hits do not block the client response. |
| `non_stream_status_code` | `502` | `503` | Status code returned to the client after guard retries are exhausted or local interception finally happens. The field name keeps `non_stream`, but the current strict streaming mode also uses it. |
| `stream_action` | `strict_502` | `"disconnect"` | Action after a streaming hit. `strict_502` buffers the whole stream, discards the entire response after a hit, and retries or returns an error; `disconnect` also discards the whole response and retries when retry budget remains, and only after the budget is exhausted does it pass through while scanning, discarding the hit chunk and disconnecting if a hit occurs after data has already been passed through. |

## API Key Behavior

`upstream_api_key` is the explicit upstream key override for the local proxy.

- If `upstream_api_key` is non-empty, the `Authorization` sent by the proxy to the upstream service is fixed to `Bearer <upstream_api_key>`, even if the client request already contains `Authorization`.
- If `upstream_api_key` is empty, the proxy does not generate a key itself, but passes through the `Authorization` from the client request. In Codex scenarios, this is usually the token Codex attaches according to its own `auth.json`.

Therefore: if you want to force a specific backup key, fill in `upstream_api_key`; if you want to continue using Codex's original `auth.json`, leave `upstream_api_key` empty.

## CLI Option Reference

| CLI option | Example | Description |
| --- | --- | --- |
| `--config <path>` | `--config build/config.json` | Specify the configuration file path. |
| `--proxy-host` | `--proxy-host 127.0.0.1` | Override `proxy_host`. |
| `--proxy-port` | `--proxy-port 8011` | Override `proxy_port`. |
| `--proxy-prefix` | `--proxy-prefix /v1` or `--proxy-prefix ""` | Override `proxy_prefix`; an empty string means root proxy. |
| `--upstream-base-url` | `--upstream-base-url https://api.openai.com/v1` | Override `upstream_base_url`. |
| `--upstream-api-key` | `--upstream-api-key sk-...` | Override `upstream_api_key`; when non-empty, it forcibly overrides the client Authorization. |
| `--upstream-user-agent` | `--upstream-user-agent curl/8.7.1` | Override `upstream_user_agent`. |
| `--forward-user-agent` | `--forward-user-agent` | Forward the client `User-Agent` to the upstream service. |
| `--upstream-proxy` | `--upstream-proxy http://127.0.0.1:7890` | Override `upstream_proxy`. |
| `--upstream-http-proxy` | `--upstream-http-proxy http://127.0.0.1:7890` | Override `upstream_http_proxy`. |
| `--upstream-https-proxy` | `--upstream-https-proxy http://127.0.0.1:7890` | Override `upstream_https_proxy`. |
| `--upstream-socks-proxy` | `--upstream-socks-proxy socks5://127.0.0.1:7890` | Override `upstream_socks_proxy`. |
| `--upstream-timeout` | `--upstream-timeout 1800` | Override `upstream_timeout_sec`. |
| `--buffer-timeout` | `--buffer-timeout 360` | Override `buffer_timeout_sec`. |
| `--request-body-limit-bytes` | `--request-body-limit-bytes 104857600` | Override `request_body_limit_bytes`. |
| `--response-buffer-limit-bytes` | `--response-buffer-limit-bytes 104857600` | Override `response_buffer_limit_bytes`. |
| `--intercept-rule-mode` | `--intercept-rule-mode reasoning_tokens` | Override `intercept_rule_mode`. |
| `--reasoning-equals` | `--reasoning-equals 516,1034,1552` | Override `reasoning_equals`. |
| `--guard-retry-attempts` | `--guard-retry-attempts 10` | Override `guard_retry_attempts`. |
| `--reasoning-516-retries` | `--reasoning-516-retries 10` | Compatibility alias, equivalent to setting `guard_retry_attempts`. |
| `--retry-upstream-capacity-errors` | `--retry-upstream-capacity-errors` | Set `retry_upstream_capacity_errors=true`. |
| `--no-retry-upstream-capacity-errors` | `--no-retry-upstream-capacity-errors` | Set `retry_upstream_capacity_errors=false`. |
| `--guard-endpoints` | `--guard-endpoints /responses,/v1/responses` | Override `guard_endpoints`. |
| `--no-intercept-streaming` | `--no-intercept-streaming` | Temporarily disable `intercept_streaming`. |
| `--no-intercept-non-streaming` | `--no-intercept-non-streaming` | Temporarily disable `intercept_non_streaming`. |
| `--non-stream-status-code` | `--non-stream-status-code 502` | Override `non_stream_status_code`. |
| `--stream-action` | `--stream-action strict_502` or `--stream-action disconnect` | Override `stream_action`. |
| `--query-status` | `--query-status` | Query a running proxy and exit. |
| `--query-url` | `--query-url http://127.0.0.1:8010/status` | Specify the status query URL. |
| `--status-json` | `--status-json` | Print status JSON once after startup. |
| `--keep-config` | `--keep-config` | Write the final startup configuration back to `config.json`. |

## Control Endpoints

- `GET /healthz`: lightweight health check, returning a summary of the current configuration.
- `GET /status` or `GET /metrics`: returns health information and runtime statistics.
- `GET /version` or `GET /v1/version`: returns the proxy version and configuration capabilities.
- `GET /props` or `GET /v1/props`: returns feature switches discoverable by clients.

When `proxy_prefix` is empty, `GET /` is not fixed as a health check, but is forwarded to the upstream root path as a business request; please use `/healthz` for health checks.

## Runtime Statistics Semantics

The `runtime` field in `/status` displays statistics since the current proxy startup:

- `requests_total`: total number of all requests, including control endpoints.
- `control_requests_total`: total number of control endpoint requests.
- `health_requests_total`: number of health check requests.
- `status_requests_total`: number of status, version, and props requests.
- `intercepted_requests_total`: number of business requests entering the proxy forwarding path.
- `upstream_attempts_total`: number of actual attempts sent to the upstream service; internal retries increase this value.
- `successful_requests_total` / `failed_requests_total`: number of final proxy requests completed successfully/failed for the client.
- `proxy_error_total`: number of network proxy layer errors.
- `upstream_http_error_total`: upstream HTTP error statistics; local limits, bad requests, and timeouts are not counted in this metric.
- `client_connection_error_total`: number of early client disconnects or connection exceptions.
- `buffer_timeout_total`: number of request body or response buffer timeouts.
- `upstream_timeout_total`: number of upstream request timeouts.
- `local_proxy_error_total`: number of local errors such as local request/response limits and bad requests.
- `inspected_response_count`: number of responses inspected by the guard.
- `bypassed_proxy_request_count`: number of business requests that did not enter guard inspection.
- `matched_response_count`: number of responses matching the current interception rule.
- `matched_streaming_count` / `matched_non_streaming_count`: number of streaming/non-streaming responses matching the current interception rule.
- `blocked_response_count`: total number of responses actually intercepted in the end.
- `blocked_streaming_count` / `blocked_non_streaming_count`: number of streaming/non-streaming responses actually intercepted in the end.
- `guard_retry_total`: number of internal retries triggered by guard or protected exceptions.
- `reasoning_tokens_516_retry_total`: number of internal retries triggered by `reasoning_tokens=516`.
- `observed_reasoning_counts`: distribution of observed reasoning token counts.
- `status_code_counts`: distribution of final status codes returned to the client.
- `last_result` / `last_failure`: the most recent proxy result and the most recent failure details, including `error_type`, status code, and elapsed time.

## Directory Layout

```text
CMakeLists.txt
config.example.json
scripts/
  archive-qt-sdk.sh
  archive-qt-sdk.ps1
  build-qt5-linux-sdk.sh
  build-qt5-macos-sdk.sh
  build-qt5-windows-mingw-sdk.sh
  build-qt5-windows-sdk.ps1
  package-linux.sh
  package-macos.sh
  package-windows-mingw.sh
  package-windows.ps1
  restart-gui.sh
src/
  core/   # business core: configuration, HTTP proxy, interception policy, statistics
  cli/    # command-line entry point
  gui/    # Qt Widgets main window
tests/
  http_proxy_server_test.cpp
docs/
  ARCHITECTURE.md
```

## Verification

```bash
cmake --build build -j2
cd build
ctest --output-on-failure
```

The current QtTest coverage includes request body/response body limits, buffer timeouts, upstream timeout, chunked request body decoding, upstream cancellation after client disconnect, root proxy root-path forwarding, split proxy field round-trip, streaming guard retry, incomplete SSE response retry, and authorization priority.


## Build

```bash
cmake -S . -B build
cmake --build build -j2
```

By default, the build uses an explicit Qt 5 SDK from the current environment. CI uniformly produces SDK archives based on Qt 5.15.x:

```text
/mnt/data/qt-2080ti-sync/qt5-openssl
```

To replace the Qt path:

```bash
cmake -S . -B build \
  -DNET_TUNNEL_QT_SDK_ROOT=/path/to/qt5
```

Local development builds use the self-built Qt5 under `/mnt/data/qt-2080ti-sync` by default, and do not use the system Qt5.

The CMake targets used in source development builds still keep historical names internally. The official user entry points in installation packages are `openai-reasoning-guard-cli` and `openai-reasoning-guard-gui`.

## Packaging

The project provides three local packaging entry points:

- Linux: [scripts/package-linux.sh](scripts/package-linux.sh), producing `.deb`, `.rpm`, GUI AppImage, and CLI AppImage.
- Windows: [scripts/package-windows-mingw.sh](scripts/package-windows-mingw.sh), cross-compiling with MinGW on Linux and producing installer `.exe` and portable `.zip`; [scripts/package-windows.ps1](scripts/package-windows.ps1) is kept for native Windows/MSVC builds.
- macOS: [scripts/package-macos.sh](scripts/package-macos.sh), producing a self-extracting shell installer that contains the GUI app, CLI, and temporary DMG payload.

All platforms handle Qt according to the same principle: build and package only with an explicit Qt SDK, and never automatically use the system Qt5. Local Linux searches the self-built Qt5 under `/mnt/data/qt-2080ti-sync` by default; CI must provide Qt through Qt SDK archive secrets for each architecture.

### Linux

Build `.deb`, `.rpm`, GUI AppImage, and CLI AppImage locally in one command:

```bash
scripts/package-linux.sh --all --clean
```

Build only one of them:

```bash
scripts/package-linux.sh --deb --clean
scripts/package-linux.sh --rpm --clean
scripts/package-linux.sh --appimage --clean
```

Output artifacts:

```text
dist/openai-reasoning-guard_<version>_amd64.deb
dist/openai-reasoning-guard-<version>-1.x86_64.rpm
dist/openai-reasoning-guard-gui-<version>-x86_64.AppImage
dist/openai-reasoning-guard-cli-<version>-x86_64.AppImage
```

By default, the script only looks for self-built Qt5 under `/mnt/data/qt-2080ti-sync`, and does not automatically fall back to system Qt5. CI or other machines need to explicitly pass the Qt SDK path:

```bash
QT_ROOT=/path/to/qt5 scripts/package-linux.sh --all --clean
```

Available environment variables:

| Variable | Example | Description |
| --- | --- | --- |
| `PACKAGE_ID` | `openai-reasoning-guard` | Linux package name, installation directory name, AppImage filename prefix, and default package prefix for Windows/macOS. |
| `APP_NAME` | `"OpenAI Reasoning Guard"` | Application display name in the desktop file. |
| `GUI_COMMAND` | `openai-reasoning-guard-gui` | Installed GUI command name. |
| `CLI_COMMAND` | `openai-reasoning-guard-cli` | Installed CLI command name. |
| `ICON_SOURCE` | `assets/openai-reasoning-guard-icon-1024.png` | Application icon used for packaging. If it does not exist, the script falls back to the built-in SVG. |
| `QT_ROOT` | `/mnt/data/qt-2080ti-sync/qt5-openssl` | Qt SDK root directory, which must contain `bin/moc`, `lib/libQt5Core.so.5`, and `plugins/platforms/libqxcb.so`. |
| `LOCAL_QT_BASE` | `/mnt/data/qt-2080ti-sync` | Local default Qt search root; used only when `QT_ROOT` is empty. |
| `OPENSSL_ROOT` | `/path/to/openssl` | Optional OpenSSL runtime root; when unset, it is preferentially copied from Qt's `lib` directory. |
| `DEB_ARCH` | `amd64` | Override the deb architecture name. Automatic values are usually `amd64`, `i386`, `arm64`, or `armhf`. |
| `RPM_ARCH` | `x86_64` | Override the rpm architecture name. Automatic values are usually `x86_64`, `i686`, `aarch64`, or `armv7hl`. |
| `RPM_RELEASE` | `1` | rpm release field, which also appears in the rpm filename. |
| `APPIMAGE_ARCH` | `x86_64` | Override the AppImage architecture name. Automatic values are usually `x86_64`, `i686`, `aarch64`, or `armhf`. |
| `VERSION` | `0.1.0` | Override the package version. The default reads the CMake project version. |
| `BUILD_DIR` | `build-package` | Release build directory. |
| `DIST_DIR` | `dist` | Package artifact output directory. |
| `WORK_DIR` | `.package-work` | Packaging staging temporary directory. |
| `TOOL_DIR` | `$HOME/.cache/openai-reasoning-guard/package-tools` | `appimagetool` cache directory; `--clean` does not delete this directory. |
| `APPIMAGETOOL` | `/path/to/appimagetool` | Explicitly specify an existing local `appimagetool`, completely skipping download. |
| `JOBS` | `8` | Number of parallel build jobs. |
| `BUILD_TESTS` | `ON` | Whether to build QtTest during package builds. Default is `OFF`. |
| `SKIP_BUILD` | `1` | Skip compilation and package directly with existing binaries in `BUILD_DIR`. |
| `DOWNLOAD_PROXY` | `http://127.0.0.1:7890` | Proxy used when downloading `appimagetool`. |

Runtime Qt libraries are packaged into `/opt/openai-reasoning-guard/qt`. The installed official entry points are `/usr/bin/openai-reasoning-guard-gui` and `/usr/bin/openai-reasoning-guard-cli`, while `/usr/bin/net-tunnel-gui` and `/usr/bin/net-tunnel-cli` are kept as compatibility symlinks. The package wrapper places the configuration file at `${XDG_CONFIG_HOME:-$HOME/.config}/openai-reasoning-guard/config.json`; if the old `${XDG_CONFIG_HOME:-$HOME/.config}/net-tunnel-cpp-client/config.json` exists, it is copied once automatically to avoid writing into `/opt`.

### Windows

CI uses the MinGW route by default: it installs a MinGW compiler on Linux and cross-compiles with a Windows MinGW Qt SDK. The Qt SDK must contain both Linux-executable Qt host tools and Windows target libraries:

- `bin/moc`, `bin/rcc`, `bin/uic`: Linux host tools used by CMake automoc/autorcc/autouic.
- `bin/Qt5Core.dll`, `bin/Qt5Network.dll`, `bin/Qt5Gui.dll`, `bin/Qt5Widgets.dll`: Windows runtime DLLs.
- `plugins/platforms/qwindows.dll`: Windows Qt platform plugin.
- `lib/cmake/Qt5/Qt5Config.cmake` and `lib/libQt5Core.a` or `lib/libQt5Core.dll.a`: CMake package and MinGW import library.

The packaging script generates two files:

- installer `.exe`: NSIS installer.
- portable `.zip`: single-file portable package, runnable directly after extraction.

The package contains:

- `openai-reasoning-guard-gui.exe`
- `openai-reasoning-guard-cli.exe`
- Qt DLLs, `plugins/platforms/qwindows.dll`, fonts, configuration example, and documentation files

Linux/MinGW example:

```bash
QT_ROOT=/path/to/qt-5.15.x-mingw64-posix \
MINGW_TRIPLE=x86_64-w64-mingw32 \
scripts/package-windows-mingw.sh --arch x86_64 --clean
```

Windows ARM64 uses the `aarch64-w64-mingw32` toolchain from `llvm-mingw`:

```bash
QT_ROOT=/path/to/qt-5.15.x-mingw-arm64 \
MINGW_TRIPLE=aarch64-w64-mingw32 \
MINGW_BIN_DIR=/path/to/llvm-mingw/bin \
scripts/package-windows-mingw.sh --arch arm64 --clean
```

Output example:

```text
dist/openai-reasoning-guard-windows-x86_64-0.1.0-installer.exe
dist/openai-reasoning-guard-windows-x86_64-0.1.0-portable.zip
```

The native Windows/MSVC route remains available, and is suitable for machines that already have an MSVC Qt SDK:

```powershell
powershell -ExecutionPolicy Bypass -File scripts/package-windows.ps1 `
  -Arch x86_64 `
  -QtRoot C:\Qt\5.15.2\msvc2019_64 `
  -Clean
```

`--arch x86_32` / `-Arch x86_32` is used for 32-bit Windows packages, and `--arch arm64` / `-Arch arm64` is used for Windows ARM64 packages. The Qt SDK must match the ABI of the current C++ compiler; for example, MinGW x86_64 uses 64-bit MinGW Qt, Windows ARM64 uses `aarch64-w64-mingw32` Qt, and MSVC x86_64 uses 64-bit MSVC Qt.

### macOS

The macOS packaging script creates an `.app` bundle, uses `macdeployqt` to collect Qt frameworks, first generates a temporary DMG payload, and then embeds that DMG into a self-extracting shell installer. The final artifact released to users is `.sh`; when executed, it requests `sudo` permission, mounts the internal DMG, installs the app to `/Applications`, removes the app's quarantine marker, and installs the `/usr/local/bin/openai-reasoning-guard-cli` CLI wrapper script.

Examples:

```bash
QT_ROOT=/path/to/Qt/5.15.2/clang_64 scripts/package-macos.sh --arch x86_64 --clean
QT_ROOT=/path/to/Qt/5.15.2/macos scripts/package-macos.sh --arch aarch64 --clean
```

Output examples:

```text
dist/openai-reasoning-guard-macos-x86_64-0.1.0-installer.sh
dist/openai-reasoning-guard-macos-aarch64-0.1.0-installer.sh
```

macOS distribution has two signing modes:

- Default mode: uses ad-hoc signing to ensure the signing structure of Qt frameworks, plugins, GUI, and CLI inside the bundle is complete. The installer requests `sudo` permission at runtime and runs `xattr -dr com.apple.quarantine` on the local copy installed to `/Applications`.
- Official distribution mode: signs with an Apple Developer Program `Developer ID Application` certificate and submits to Apple notarization. After successful notarization and stapling, the internal DMG/app has an official distribution signature; the installer still installs to `/Applications` through the same path.

The DMG appearance is arranged by default in Finder icon view as an installation disk style of "drag the app to Applications", and a light background image is generated. Related switches:

```bash
MACOS_DMG_STYLE=1              # generate draggable installation layout, enabled by default
MACOS_DMG_STYLE_STRICT=0       # whether Finder layout failure should fail packaging, default is warning only
MACOS_DMG_BACKGROUND=1         # generate DMG background image, enabled by default
MACOS_KEEP_DMG=0               # by default only the shell installer is released; set to 1 to also keep the internal DMG
```

User installation example:

```bash
bash openai-reasoning-guard-macos-aarch64-0.1.0-installer.sh
```

Installer environment switches:

```bash
OPEN_AFTER_INSTALL=0 bash openai-reasoning-guard-macos-aarch64-0.1.0-installer.sh
INSTALL_CLI_SYMLINK=0 bash openai-reasoning-guard-macos-aarch64-0.1.0-installer.sh
```

To enable official distribution mode in GitHub Actions, configure the following secrets:

```text
MACOS_CERTIFICATE_P12_BASE64   # base64 of the Developer ID Application certificate .p12
MACOS_CERTIFICATE_PASSWORD     # .p12 password
MACOS_CODESIGN_IDENTITY        # for example Developer ID Application: Your Name (TEAMID)
MACOS_NOTARY_APPLE_ID          # Apple ID
MACOS_NOTARY_TEAM_ID           # Team ID
MACOS_NOTARY_PASSWORD          # app-specific password
```

You can also use a saved notarytool profile:

```text
MACOS_NOTARY_PROFILE
```

### GitHub Actions

The project has two CI pipelines:

- [.github/workflows/qt-sdk.yml](.github/workflows/qt-sdk.yml): builds reusable Qt SDK archives from Qt/OpenSSL source and uploads them to standard GitHub Release tags.
- [.github/workflows/linux-packages.yml](.github/workflows/linux-packages.yml): uses Qt SDK archives to build final user installation packages, can be triggered manually, and also triggers on `v*` tags.

When the packaging workflow is triggered manually, `target` defaults to only building `linux-x86_64`; you can also choose a single target or `all`. The workflow covers the following targets:

| Target | runner/container | Artifacts | Qt SDK secret |
| --- | --- | --- | --- |
| Linux x86_64 | `linux/amd64` Docker | deb, rpm, GUI AppImage, CLI AppImage | `QT_LINUX_X86_64_URL` |
| Linux x86_32 | `linux/386` Docker | deb, rpm, GUI AppImage, CLI AppImage | `QT_LINUX_X86_32_URL` |
| Linux arm64 | `linux/arm64` Docker | deb, rpm, GUI AppImage, CLI AppImage | `QT_LINUX_ARM64_URL` |
| Linux arm32 | `linux/arm/v7` Docker | deb, rpm, GUI AppImage, CLI AppImage | `QT_LINUX_ARM32_URL` |
| Windows x86_64 | Ubuntu runner + MinGW `x86_64-w64-mingw32` | installer `.exe`, portable `.zip` | `QT_WINDOWS_X86_64_URL` |
| Windows x86_32 | Ubuntu runner + MinGW `i686-w64-mingw32` | installer `.exe`, portable `.zip` | `QT_WINDOWS_X86_32_URL` |
| Windows ARM64 | Ubuntu runner + llvm-mingw `aarch64-w64-mingw32` | installer `.exe`, portable `.zip` | `QT_WINDOWS_ARM64_URL` |
| macOS x86_64 | `macos-15-intel` | shell installer | `QT_MACOS_X86_64_URL` |
| macOS aarch64 | `macos-14` | shell installer | `QT_MACOS_ARM64_URL` |

Each successful job uploads an Actions artifact first by default. Artifacts are suitable for inspecting build results but will expire. To put artifacts into GitHub Releases:

- Pushing a `v*` tag automatically publishes to the Release with the same name, for example `v0.1.0`.
- When manually triggered, set `publish_release` to `true`; by default it publishes to the `nightly` prerelease, and you can also fill in another `release_tag`.

The value of each Qt SDK secret is a downloadable archive URL, supporting `tar`, `tar.gz`, `tar.xz`, `tgz`, or `zip`. If a secret is empty, the packaging workflow tries to read from the standard Release of this repository:

| Target | fallback Release asset |
| --- | --- |
| Linux x86_64 | `qt-sdk-linux-x86_64/qt5-linux-x86_64.tar.xz` |
| Linux x86_32 | `qt-sdk-linux-x86_32/qt5-linux-x86_32.tar.xz` |
| Linux arm64 | `qt-sdk-linux-arm64/qt5-linux-arm64.tar.xz` |
| Linux arm32 | `qt-sdk-linux-arm32/qt5-linux-arm32.tar.xz` |
| Windows x86_64 | `qt-sdk-windows-x86_64/qt5-windows-x86_64.tar.xz` |
| Windows x86_32 | `qt-sdk-windows-x86_32/qt5-windows-x86_32.tar.xz` |
| Windows ARM64 | `qt-sdk-windows-arm64/qt5-windows-arm64.tar.xz` |
| macOS x86_64 | `qt-sdk-macos-x86_64/qt5-macos-x86_64.tar.xz` |
| macOS aarch64 | `qt-sdk-macos-aarch64/qt5-macos-aarch64.tar.xz` |

After extraction, the archive needs to make the corresponding platform's Qt tools and runtime discoverable:

- Linux archive: contains `bin/moc`, `lib/libQt5Core.so.5`, `plugins/platforms/libqxcb.so`, and preferably also `lib/cmake/Qt5`.
- Windows MinGW archive: contains Linux host tools `bin/moc`, `bin/rcc`, `bin/uic`, Windows target runtime `bin/Qt5Core.dll`, `bin/Qt5Network.dll`, `bin/Qt5Gui.dll`, `bin/Qt5Widgets.dll`, `plugins/platforms/qwindows.dll`, `lib/cmake/Qt5`, and MinGW import libraries. It is recommended to put runtime DLLs matching the Qt builder into `runtime/mingw`; GCC MinGW usually needs `libgcc_s_*.dll`, `libstdc++-6.dll`, `libwinpthread-1.dll`, and Windows ARM64 llvm-mingw usually needs `libc++.dll`, `libc++abi.dll`, `libunwind.dll`, `libwinpthread-1.dll`.
- macOS archive: contains `bin/moc`, `bin/macdeployqt`, and Qt frameworks/CMake package.

Optional secret:

- `DOWNLOAD_PROXY`: proxy used when downloading Qt SDK or `appimagetool`, for example `http://127.0.0.1:7890`.

The Linux architectures in CI first compile and run QtTest, and then call the corresponding packaging script. Linux jobs run by default inside Debian bookworm target-architecture containers, and cover x86_32, arm64, and arm32 through QEMU; Windows jobs cross-compile with MinGW inside an Ubuntu runner and do not run Windows exe by default; macOS uses the native compiler on GitHub-hosted runners. The workflow does not install or use system Qt5. The Linux container version must not be lower than the glibc version used when building the Qt SDK; if you need compatibility with older distributions, rebuild the Qt SDK in an older target container first.

### Preparing Qt SDK for CI

The CI package build depends on Qt SDK archives that are "runnable for the target platform". The recommended flow is to run the `Qt SDK Archives` workflow first, generate standard SDK Releases from source, and then trigger the package build workflow directly. Only when SDKs are stored at external or private addresses do you need to write asset URLs into `QT_*_URL` secrets.

SDK workflow inputs:

| Input | Example | Description |
| --- | --- | --- |
| `target` | `windows-x86_64` | SDK target to build; `all` can also be selected, but it will take a very long time. |
| `qtbase_url` | `https://download.qt.io/archive/qt/5.15/5.15.2/submodules/qtbase-everywhere-src-5.15.2.tar.xz` | Qt 5.15.x qtbase source archive download URL. |
| `qttools_url` | `https://download.qt.io/archive/qt/5.15/5.15.2/submodules/qttools-everywhere-src-5.15.2.tar.xz` | Required by macOS SDK to build `macdeployqt`. |
| `openssl_url` | `https://github.com/openssl/openssl/releases/download/OpenSSL_1_1_1w/openssl-1.1.1w.tar.gz` | Required by Linux and Windows MinGW SDKs; macOS uses SecureTransport and does not need it. |
| `llvm_mingw_url` | `https://github.com/mstorsjo/llvm-mingw/releases/download/20260616/llvm-mingw-20260616-ucrt-ubuntu-22.04-x86_64.tar.xz` | Required by Windows ARM64 SDK to provide the `aarch64-w64-mingw32` cross toolchain. |
| `release_tag` | empty | When left empty, automatically uses `qt-sdk-<target>`, which is the default fallback for the package build workflow. |
| `clean` | `true` | Whether to clean the SDK build directory before rebuilding. |

Linux can be built directly from Qt 5.15.x qtbase and OpenSSL 1.1.1w source. The default script paths are:

```text
/mnt/data/qt-2080ti-sync/archives/qtbase-everywhere-src-5.15.2.tar.xz
/mnt/data/qt-2080ti-sync/archives/openssl-1.1.1w.tar.gz
```

Build and upload local x86_64 Linux:

```bash
scripts/build-qt5-linux-sdk.sh \
  --target linux-x86_64 \
  --archive \
  --upload \
  --set-secret \
  --upload-proxy http://127.0.0.1:7890
```

Other Linux architectures compile the same source inside target-architecture Ubuntu containers through Docker/QEMU:

```bash
scripts/build-qt5-linux-sdk.sh --target linux-x86_32 --docker --archive --upload --set-secret
scripts/build-qt5-linux-sdk.sh --target linux-arm64 --docker --archive --upload --set-secret
scripts/build-qt5-linux-sdk.sh --target linux-arm32 --docker --archive --upload --set-secret
```

If you already have a Qt SDK and only need to archive it and set the secret without rebuilding:

```bash
scripts/archive-qt-sdk.sh \
  --qt-root /path/to/qt5 \
  --target linux-x86_64 \
  --upload \
  --set-secret \
  --upload-proxy http://127.0.0.1:7890
```

macOS builds natively on runners/machines of the corresponding architecture, uniformly using Qt 5.15.x qtbase + qttools source:

```bash
scripts/build-qt5-macos-sdk.sh \
  --target macos-x86_64 \
  --qtbase-source-archive /path/to/qtbase-everywhere-src-5.15.2.tar.xz \
  --qttools-source-archive /path/to/qttools-everywhere-src-5.15.2.tar.xz \
  --archive \
  --upload \
  --set-secret

scripts/build-qt5-macos-sdk.sh \
  --target macos-aarch64 \
  --qtbase-source-archive /path/to/qtbase-everywhere-src-5.15.x.tar.xz \
  --qttools-source-archive /path/to/qttools-everywhere-src-5.15.x.tar.xz \
  --archive \
  --upload \
  --set-secret
```

Windows CI recommends using a MinGW cross Qt SDK. The SDK structure is Linux host `moc/rcc/uic` plus Windows target `Qt5*.dll/import libs`. The official SDK should preferably be generated from source by `build-qt5-windows-mingw-sdk.sh` or the `Qt SDK Archives` workflow, to avoid binding to a manual directory from a specific machine.

If an available SDK already exists, you can also only archive and upload it:

```bash
scripts/archive-qt-sdk.sh \
  --qt-root /path/to/qt-5.15.x-mingw64-posix \
  --target windows-x86_64 \
  --mingw-runtime-dir /path/to/mingw-gcc-runtime \
  --mingw-runtime-dir /path/to/mingw-sysroot/lib \
  --upload \
  --set-secret \
  --upload-proxy http://127.0.0.1:7890
```

Here `--mingw-runtime-dir` places the `*.dll` files matching the Qt builder into the archive's `runtime/mingw`, and packaging preferentially copies these DLLs. x86_32 needs a separately prepared 32-bit MinGW Qt SDK and changes `--target` to `windows-x86_32`; Windows ARM64 uses `windows-arm64`.

If you need to rebuild the Windows MinGW Qt SDK from source, use:

```bash
scripts/build-qt5-windows-mingw-sdk.sh \
  --target windows-x86_64 \
  --archive \
  --upload \
  --set-secret \
  --upload-proxy http://127.0.0.1:7890
```

The script uses existing local source archives by default:

```text
/mnt/data/qt-2080ti-sync/archives/qtbase-everywhere-src-5.15.2.tar.xz
/mnt/data/qt-2080ti-sync/archives/openssl-1.1.1w.tar.gz
```

It generates Linux host `moc/rcc/uic` and Windows target `Qt5*.dll` according to Qt cross build, and enables QtNetwork HTTPS support with `-openssl-runtime`. For x86_32 use:

```bash
scripts/build-qt5-windows-mingw-sdk.sh --target windows-x86_32 --archive --upload --set-secret
```

Windows ARM64 uses llvm-mingw:

```bash
scripts/build-qt5-windows-mingw-sdk.sh --target windows-arm64 --archive --upload --set-secret
```

The native Windows/MSVC SDK can also be kept as a backup route. Windows builds natively in the corresponding MSVC developer environment, using an x64 shell for x86_64, an x86 shell for x86_32, and an ARM64 shell for ARM64:

```powershell
powershell -ExecutionPolicy Bypass -File scripts/build-qt5-windows-sdk.ps1 `
  -Target windows-x86_64 `
  -QtBaseSourceArchive C:\src\qtbase-everywhere-src-5.15.2.tar.xz `
  -OpenSslRoot C:\OpenSSL-Win64 `
  -Archive `
  -Upload `
  -SetSecret `
  -UploadProxy http://127.0.0.1:7890
```

If Windows already has an available MSVC Qt SDK, you can also only archive and upload it, but the current GitHub Actions Windows job expects a MinGW cross SDK by default, not an MSVC SDK:

```powershell
powershell -ExecutionPolicy Bypass -File scripts/archive-qt-sdk.ps1 `
  -QtRoot C:\Qt\5.15.2\msvc2019_64 `
  -Target windows-x86_64 `
  -Upload `
  -SetSecret
```

`--upload-proxy` / `-UploadProxy` is only used on the local machine executing the upload command. Do not configure GitHub Actions `DOWNLOAD_PROXY` as `127.0.0.1:7890`, because `127.0.0.1` on the GitHub runner is not your machine.

## License

The project source code uses the MIT License. The bundled QUI components and font resources in the repository are described in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md), and third-party files remain under their respective upstream licenses.
