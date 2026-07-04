# Architecture

## 分层

`net-tunnel-core` 是唯一承载业务行为的静态库。

- `app_config`: 读取/保存 JSON 配置。
- `json_utils`: 按 OpenAI usage 固定路径扫描 JSON/SSE 内容中的 `reasoning_tokens`。
- `http_proxy_server`: 本地 HTTP 代理、控制端点、上游转发、reasoning guard 重试、拦截策略和运行统计。

`src/cli` 和 `src/gui` 只负责入口层：

- CLI 将参数解析成 core settings，然后进入 Qt event loop。
- GUI 将控件状态解析成 core settings，并把 core signal 显示到界面。

当前版本刻意不包含公网隧道/内外穿透模块。

## 智能拦截流程

1. `QTcpServer` 接收本地 HTTP 请求。
2. 控制路径直接返回本地 JSON：
   - `/healthz`
   - `/status`
   - `/version`
   - `/props`
3. 业务路径按 `proxy_prefix` 去前缀，再拼到 `upstream_base_url`。`proxy_prefix` 为空字符串时表示根路径 `/`，客户端 `/` 会转发到上游根路径；此时 health 只使用显式 `/healthz`。
4. 使用 `QNetworkAccessManager` 转发到上游。
5. 只对 `guard_endpoints` 配置的路径检查响应。匹配时同时检查客户端原始路径和去掉 `proxy_prefix` 后的业务路径，避免自定义前缀绕过默认 guard。默认端点对齐 `codex-retry-gateway`：
   - `/responses`
   - `/chat/completions`
   - `/v1/responses`
   - `/v1/chat/completions`
6. 对 JSON 或 SSE 响应进行扫描。
7. 如果发现 `reasoning_tokens` 命中 `reasoning_equals`，默认集合为 `516,1034,1552`，按 `guard_retry_attempts` 重试同一个上游请求。
8. 重试期间保留本项目语义：中间重试只计 `guard_retry_total`，不计 `blocked_response_count` 或 `failed_requests_total`。
9. 如果重试后仍然命中 guard，返回：

```json
{
  "error": {
    "message": "codex retry gateway blocked suspicious reasoning response on /v1/responses",
    "code": "reasoning_guard_triggered",
    "reasoning_tokens": 516,
    "status_code": 502,
    "type": "codex_retry_gateway"
  }
}
```

状态码为 `502`。

流式响应动作由 `stream_action` 控制：

- `strict_502`: 默认模式，先缓存完整 SSE 响应，确认安全后再透传；命中 guard 时丢弃本次响应并按重试策略重新请求，重试耗尽后返回本地错误。
- `disconnect`: 兼容旧行为；还有 `guard_retry_attempts` 预算时先缓存当前尝试，命中 guard 后丢弃本次响应并重试。预算耗尽后才边透传边扫描；命中发生在已有 chunk 透传之后时取消上游请求并断开客户端连接，当前命中 chunk 不继续写回，记录 `reasoning_guard_triggered`。

缓冲保护：

- `buffer_timeout_sec` 同时约束客户端请求体缓冲和上游响应缓冲。
- `request_body_limit_bytes` 超限时本地返回 `413`，错误类型为 `request_body_limit_exceeded`。
- `response_buffer_limit_bytes` 超限时本地返回 `502`，错误类型为 `response_buffer_limit_exceeded`。
- `upstream_timeout_sec` 超时时本地返回 `504`，错误类型为 `upstream_timeout`。
- 客户端请求体支持 `Content-Length` 和 `Transfer-Encoding: chunked`，chunked 请求会先解码再转发上游，`request_body_limit_bytes` 按解码后的 body 计算。
- 客户端提前断开会取消当前上游请求，并记录 `client_connection_error`。

上游代理字段：

- `upstream_proxy` 是通用代理字段。
- `upstream_http_proxy`、`upstream_https_proxy`、`upstream_socks_proxy` 是协议拆分兼容字段，读取和保存时都会保留，不会在配置层折叠进 `upstream_proxy`。

## 统计模型

代理运行时维护以下关键计数：

- `requests_total`
- `intercepted_requests_total`
- `successful_requests_total`
- `failed_requests_total`
- `proxy_error_total`
- `upstream_http_error_total`
- `client_connection_error_total`
- `buffer_timeout_total`
- `upstream_timeout_total`
- `local_proxy_error_total`
- `guard_retry_total`
- `blocked_response_count`
- `matched_response_count`
- `observed_reasoning_counts`
- `reasoning_tokens_516_total`，兼容旧字段，只统计最终耗尽且 reasoning 为 516 的失败
- `reasoning_tokens_516_retry_total`，兼容旧字段，只统计 reasoning 为 516 的中间重试
- `consecutive_failures`
- `status_code_counts`
- `last_result`
- `last_failure`

GUI 直接读取 `HttpProxyServer::statusPayload()`，CLI 可通过 `/status` 查看同一份数据。

## 后续扩展边界

- IP 诊断可以新增 `src/core/ip_diagnostics.*`，不要放进 GUI。
- 如后续重新需要公网隧道，应新增独立 core 模块，并继续保持 CLI/GUI 复用 core。
- 打包脚本放到 `scripts/`，不应改变 core/cli/gui 的职责边界。
