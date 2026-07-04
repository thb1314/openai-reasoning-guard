# Qt Client Optimization Items

本文记录 Qt/C++ 客户端审阅后的优化项，按优先级排列。标记为“已修复”的条目已有实现和 QtTest 覆盖。

## 中高优先级

### 1. [已修复] 协议拆分上游代理字段仍可能被提升成通用代理

位置：

- `src/core/app_config.cpp`
- `src/gui/main_window.cpp`

现象：

`loadConfig()` 在 `upstream_proxy` 为空时，会把 `upstream_https_proxy`、`upstream_http_proxy` 或 `upstream_socks_proxy` 折叠进 `config.upstreamProxy`。GUI 又只显示单一 `upstreamProxyEdit_` 输入框，保存时会把这个 effective proxy 写回通用 `upstream_proxy`。

案例：

配置文件只设置：

```json
{
  "upstream_proxy": "",
  "upstream_https_proxy": "http://127.0.0.1:8443",
  "upstream_http_proxy": "",
  "upstream_socks_proxy": ""
}
```

GUI 打开并保存一次后，可能多出通用 `upstream_proxy=http://127.0.0.1:8443`。后续如果切换 HTTP/HTTPS 上游，原本的协议选择语义会被改变。

建议：

- `AppConfig` 保持原始字段，不在配置层把拆分字段折叠进 `upstreamProxy`。
- 在运行时 `ProxySettings` 或 `HttpProxyServer::configureUpstreamProxy()` 中计算 effective proxy。
- 增加 split-only 代理字段 round-trip QtTest，覆盖“只设置 `upstream_https_proxy` 保存后不生成 `upstream_proxy`”。

修复状态：

- 配置读取不再把拆分字段折叠进通用 `upstreamProxy`。
- CLI/GUI 保存时保留原始拆分字段。
- 已增加 split-only round-trip QtTest。

## 中优先级

### 2. [已修复] 不支持 `Transfer-Encoding: chunked` 请求体，当前会静默丢 body

位置：

- `src/core/http_proxy_server.cpp`

现象：

请求头解析只读取 `Content-Length`。如果请求使用 `Transfer-Encoding: chunked` 且没有 `Content-Length`，当前逻辑会把 `contentLength_` 当成 `0`，请求体不会被正确读取和转发。

案例：

客户端发送 chunked POST JSON：

```http
POST /v1/responses HTTP/1.1
Transfer-Encoding: chunked
Content-Type: application/json

...
```

代理会按空 body 转发，上游收到坏请求或空 JSON。

建议：

- 实现 chunked 请求体解码，并继续应用 `request_body_limit_bytes` 和 `buffer_timeout_sec`。
- 如果暂不支持，应明确返回 `411 Length Required` 或 `501 Not Implemented`，不要静默转发空 body。
- 增加 chunked 请求体 QtTest。

修复状态：

- 已实现 chunked 请求体解码。
- 解码过程继续受 `request_body_limit_bytes` 和 `buffer_timeout_sec` 约束，大小上限按解码后的请求体计算。
- 已增加 chunked body 转发、大小限制和非法 chunked body QtTest。

### 3. [已修复] `upstream_timeout_sec` 缺少独立错误类型和统计

位置：

- `src/core/http_proxy_server.cpp`

现象：

上游超时当前通过 timer 直接 `abort()`，最终大概率被记录成 `proxy_error`。这会让运行状态无法区分网络错误、DNS 错误和真正的上游响应超时。

案例：

上游已建立连接但一直不返回，超过 `upstream_timeout_sec` 后，用户只看到 `502/proxy_error`，无法判断是 timeout。

建议：

- 增加 `upstreamTimedOut_` 标记。
- 超时时返回 `504` 或保留 `502` 但写入 `error_type=upstream_timeout`。
- 增加 `upstream_timeout_total` 运行统计。
- 增加 upstream timeout QtTest。

修复状态：

- 上游请求超时现在返回 `504`。
- 记录 `error_type=upstream_timeout` 和 `upstream_timeout_total`。
- 已增加 upstream timeout QtTest。

### 4. [已修复] 本地限制错误会污染 `upstream_http_error_total`

位置：

- `src/core/http_proxy_server.cpp`

现象：

`recordResult()` 对未知失败类型统一计入 `upstreamHttpErrorTotal_`。因此本地错误也可能被统计成上游 HTTP 错误。

案例：

- `request_body_limit_exceeded`
- `response_buffer_limit_exceeded`

这些错误不是上游返回的 HTTP 错误，但当前会进入 upstream error 统计。

建议：

- 为本地错误增加独立分类，例如 `local_proxy_error_total` 或更细的 limit 统计。
- 至少将本地 error type 从 `upstream_http_error_total` 中排除。
- 增加统计口径 QtTest，断言本地 limit 错误不会增加 upstream error。

修复状态：

- 新增 `local_proxy_error_total`。
- `request_body_limit_exceeded`、`response_buffer_limit_exceeded`、`bad_request` 不再增加 `upstream_http_error_total`。
- 已增加统计口径 QtTest。

## 中低优先级

### 5. [已修复] GUI 还不能编辑新字段

位置：

- `src/gui/main_window.cpp`
- `src/gui/main_window.h`

现象：

GUI 当前只透传配置里的部分新字段，没有控件可编辑：

- `request_body_limit_bytes`
- `response_buffer_limit_bytes`
- `stream_action`

案例：

用户想把流式命中行为切到 `disconnect`，或调小响应缓冲上限，只能手动编辑 JSON 或用 CLI 参数。

建议：

- 增加两个字节上限输入控件。
- 增加 `stream_action` 下拉框，选项为 `strict_502` 和 `disconnect`。
- 信息面板展示当前 `stream_action` 和 buffer limit。
- 增加 GUI 配置收集/保存相关测试或至少增加 core config round-trip 测试。

修复状态：

- GUI 已增加 `request_body_limit_bytes`、`response_buffer_limit_bytes` 数值输入。
- GUI 已增加 `stream_action` 下拉框，支持 `strict_502` 和 `disconnect`。
- 信息面板展示 buffer limit 和 stream action。
- 已增加 core config round-trip QtTest，覆盖三个字段保存/读取。

### 6. [已修复] 空 `proxy_prefix` 下 `/` 仍固定作为 health 控制路径

位置：

- `src/core/http_proxy_server.cpp`

现象：

当 `proxy_prefix=""` 表示根路径 `/` 时，业务请求理论上可从根路径开始。但当前 `forwardOrHandleControl()` 固定把 `/` 作为 health 响应，导致无法把客户端 `/` 转发到上游根路径。

案例：

配置：

```json
{
  "proxy_prefix": "",
  "upstream_base_url": "http://127.0.0.1:9000"
}
```

客户端请求 `GET /` 时，代理返回本地 health，而不是转发到上游 `/`。

建议：

- 如果目标是完整 root proxy，控制端点应只使用显式路径，例如 `/healthz`、`/status`、`/version`、`/props`。
- 或者只让 `GET /` 作为 health，其他方法的 `/` 继续转发上游。
- 增加空 prefix + 根路径转发 QtTest。

修复状态：

- `proxy_prefix=""` 时客户端 `/` 现在作为业务路径转发上游。
- 显式控制路径 `/healthz`、`/status`、`/version`、`/props` 保持本地处理。
- 已增加空 prefix + `GET /` 转发 QtTest，以及 `/healthz` 本地处理 QtTest。

## 建议修复顺序

1. 已完成：修复协议拆分代理字段配置语义，避免保存配置改变用户意图。
2. 已完成：处理 chunked 请求体，实现解码。
3. 已完成：增加 upstream timeout 独立错误类型和统计。
4. 已完成：修正本地错误统计口径。
5. 已完成：补齐 GUI 可编辑字段。
6. 已完成：明确空 `proxy_prefix` 下 `/` 的控制路径/业务路径优先级。


https://github.com/haowang02/codex-candy-eval
