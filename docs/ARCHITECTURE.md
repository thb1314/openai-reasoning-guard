# Architecture

## 分层

`net-tunnel-core` 是 CLI 和 GUI 共用的业务核心。

- `app_config`：读取和原子保存 `config.json` 中的全局监听、缓冲及 reasoning guard 设置。
- `upstream_profile`：使用 QtSql/QSQLITE 管理上游配置、最后选择、分页查询、导入导出、旧配置迁移和跨进程运行锁。
- `json_utils`：按 OpenAI usage 固定路径扫描 JSON/SSE 内容中的 `reasoning_tokens`。
- `http_proxy_server`：本地 HTTP 代理、控制端点、上游转发、reasoning guard 重试、拦截策略和运行统计。

`src/cli` 和 `src/gui` 只负责入口层：

- CLI 把启动参数或 `profile` 管理子命令映射到 core API。
- GUI 把全局控件状态和选中的上游配置映射到 core settings，并显示 core signal。

当前版本刻意不包含公网隧道/内外穿透模块。

## 配置模型

配置分成两个独立数据源：

- `config.json` 只保存全局设置，例如监听地址、路径前缀、缓冲限制和拦截策略。
- `upstream-profiles.sqlite3` 保存上游配置及最后选中的配置 UUID。

一条上游配置包含显示名称、Base URL、API Key、User-Agent、是否转发客户端 User-Agent、单一上游代理、上游超时和首 Token 超时。显示名称和 Base URL 必填；显示名称使用不区分大小写的唯一约束，内部 ID 是不可变 UUID。显示名称不能含控制字符且最多 `512` 个 UTF-8 字节；作为出站头值的 API Key 和 User-Agent 不能含控制字符且最多 `8192` 个 UTF-8 字节。Base URL 只允许带主机的 HTTP/HTTPS URL，不允许 userinfo/凭据、query 或 fragment。

单一上游代理为空表示直连；非空值仅允许 `http`、`socks`、`socks5`、`socks5h` scheme，必须含主机，不允许 userinfo/凭据、query 或 fragment。HTTPS proxy scheme 和 SOCKS4 不受支持；没有 scheme 的兼容输入按 HTTP 代理规范化。配置校验失败会阻止保存或启动，不会静默降级为直连。

SQLite 使用 schema version、事务、WAL 和 5 秒 busy timeout。代理运行时始终持有选择锁；使用 SQLite 中已选配置启动时还持有当前配置锁。因此其它 GUI/CLI 进程不能切换当前配置，也不能修改或删除正在使用的配置。无已选配置的 `--upstream-base-url` 临时启动只持有选择锁，因为此时没有配置记录可锁；未使用的配置仍可新增、查看和修改。`QLockFile` 根据锁内记录的 PID、主机名和进程身份判断持有者是否仍存在，不设置按锁文件年龄自动过期；因此长时间运行的代理不会被其它进程抢占锁，异常退出留下的锁仍可在确认原进程不存在后恢复。

API Key 以明文保存在当前用户私有数据库中。GUI 默认掩码；普通列表、日志、状态接口和默认 CLI 输出不返回 Key。空 Key 表示透传客户端的 `Authorization`，代理本身不读取 Codex 的 `auth.json`。

## 存储位置

默认情况下，`config.json` 和 `upstream-profiles.sqlite3` 位于同一个当前用户配置目录：

- Linux：`${XDG_CONFIG_HOME:-$HOME/.config}/OpenAI Reasoning Guard/`
- Windows：`%APPDATA%\OpenAI Reasoning Guard\`
- macOS：`~/Library/Application Support/OpenAI Reasoning Guard/`

设置 `NET_TUNNEL_CONFIG` 或 CLI `--config` 后，SQLite 数据库跟随该 JSON 所在目录。首次升级会把可执行文件旁的旧 `config.json` 复制到新的用户目录。

POSIX 系统中，本程序新建的默认配置目录，以及数据库、WAL/SHM、配置、迁移备份和导出文件会强制为仅属主可访问；既有自定义目录本身的权限不会被程序收紧。Windows 默认依赖当前用户 AppData 目录继承的 ACL；显式选择自定义或共享目录时，目录 ACL 的隔离由用户或管理员负责。

## 旧配置迁移

加载配置时仍能识别旧版扁平 `upstream_*` 字段，但正常保存不再写回这些字段。当 profile 数据库为空且旧字段能够组成一条有效上游配置时，迁移流程会：

1. 把原文件备份为 `config.json.pre-upstream-profiles.bak`。
2. 把 URL、API Key、User-Agent、转发开关和两个超时写入名为“已迁移配置”的上游配置。
3. 按旧优先级和 Base URL 协议，把通用/HTTP/HTTPS/SOCKS 字段折算成一个实际上游代理。
4. 在数据库事务成功后从 JSON 删除旧上游字段。

迁移前会按当前规则校验 Base URL、代理、头字段和两个超时；校验失败时不会改写原 JSON，需修正旧字段后重试。旧 Base URL 为空且其它遗留值均为空或保持旧默认值时，会先备份 JSON，再原子清除占位字段而不创建配置；如果 Key、代理、非默认 User-Agent、转发开关或超时仍有实际值，则迁移失败并保留原 JSON，避免静默丢失孤立设置。SQLite 提交和 JSON 清理无法组成同一个跨文件事务，因此流程会先写 pending marker：JSON 重写失败时尝试补偿数据库事务。若进程在两步之间中断或补偿失败，下次启动仅在 pending 配置仍存在且旧字段指纹匹配时继续清理；若用户已修改旧字段或 pending 配置丢失，则清除 marker 并保留 JSON，交由人工确认，且不会重复创建配置。迁移可能短暂保留“数据库配置 + 旧 JSON 字段”的可恢复中间态。

## 智能拦截流程

1. `QTcpServer` 接收本地 HTTP 请求。
2. 控制路径 `/healthz`、`/status`、`/version` 和 `/props` 直接返回本地 JSON。
3. 业务路径按 `proxy_prefix` 去前缀，再拼到当前上游配置的 Base URL。`proxy_prefix` 为空字符串时表示根路径 `/`，客户端 `/` 会转发到上游根路径。
4. 使用 `QNetworkAccessManager` 和当前上游配置的授权、User-Agent、单一代理及超时转发请求。
5. 只对 `guard_endpoints` 配置的路径检查响应；匹配时同时检查原始路径和去掉 `proxy_prefix` 后的业务路径。
6. 对 JSON 或 SSE 响应进行扫描。如果 `reasoning_tokens` 命中 `reasoning_equals`，默认集合为 `516,1034,1552`，则按 `guard_retry_attempts` 重试同一个请求。
7. 中间重试只增加 `guard_retry_total`，不增加 `blocked_response_count` 或 `failed_requests_total`。
8. 重试耗尽后返回配置的本地错误，或按对应异常的最终转发策略返回上游错误。

流式响应动作由 `stream_action` 控制：

- `strict_502`：先缓存完整 SSE 响应，确认安全后再透传；命中 guard 时丢弃本次完整响应并重试，预算耗尽后返回本地错误。
- `disconnect`：有重试预算时同样缓存并整条丢弃；预算耗尽后才边透传边扫描，命中发生在已有数据透传之后时取消上游请求并断开客户端连接，当前命中 chunk 不继续写回。
- `retryable_sse`：保持完整缓冲；对流式 Responses 请求，把最终可重试的本地错误或上游 `429/5xx` 编码为 HTTP 200 的 `response.failed` 事件。事件使用 `rate_limit_exceeded` 和包含 `Please try again in N seconds` 的消息，使 Codex 0.147+ 将上游配置的 `retry_after_override_sec` 传入可见的流重试延迟。非流式请求及非 Responses 路径保持原 HTTP 状态语义。

## 资源保护

- `buffer_timeout_sec` 同时约束客户端请求体缓冲和上游响应缓冲。
- `request_body_limit_bytes` 超限时本地返回 `413`，错误类型为 `request_body_limit_exceeded`。
- `response_buffer_limit_bytes` 超限时本地返回 `502`，错误类型为 `response_buffer_limit_exceeded`。
- 上游配置的 `upstream_timeout_sec` 超时时本地返回 `504`，错误类型为 `upstream_timeout`。
- 上游配置的 `first_token_timeout_sec` 只约束流式请求的首个非空响应 body 字节，`0` 表示禁用。
- 客户端请求体支持 `Content-Length` 和 `Transfer-Encoding: chunked`，大小上限按解码后的 body 计算。
- 客户端提前断开会取消当前上游请求，并记录 `client_connection_error`。

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
- `first_token_timeout_total`
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

GUI 直接读取 `HttpProxyServer::statusPayload()`，CLI 可通过 `/status` 查看同一份数据。状态数据只包含当前连接设置摘要，不包含 API Key。

## 部署约束

core 链接 `Qt5::Sql`，运行时必须能加载 QSQLITE driver。Linux 发布包需要包含 `libQt5Sql.so.5` 和 `plugins/sqldrivers/libqsqlite.so`；Windows 需要 `Qt5Sql.dll` 和 `plugins/sqldrivers/qsqlite.dll`；macOS 由 `macdeployqt` 收集 QtSql framework 与 SQLite driver。GUI 和 CLI 两种 AppImage 都必须携带这些文件，用户无需另行安装 `sqlite3` 命令。

## 后续扩展边界

- IP 诊断可以新增 `src/core/ip_diagnostics.*`，不要放进 GUI。
- 如后续重新需要公网隧道，应新增独立 core 模块，并继续保持 CLI/GUI 复用 core。
- 打包脚本放到 `scripts/`，不应改变 core/cli/gui 的职责边界。
