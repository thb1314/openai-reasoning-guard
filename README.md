> 打个广告：模型部署优化、模型加速（云端、端侧、边缘侧）、计算机视觉相关需求欢迎联系 zhuilewang@163.com
> 面向行业与厂家：工业质检、表面缺陷检测、SOP行为检测等


# OpenAI Reasoning Guard

简体中文 | [English](README_en.md)

这个项目主要用于缓解 Codex / OpenAI 兼容接口调用中的GPT模型“降智”问题：同一个模型、同一个请求，有时上游会返回明显推理不足、结构异常、空文本、缺 usage 或疑似低质量的响应。客户端如果直接接收这类响应，就会表现为答案变短、逻辑变差、任务中断或空结果。

本项目放在客户端和真实上游之间，作为本地智能网关使用。客户端仍然请求本地 OpenAI 兼容地址，代理负责把请求转发到上游；当代理判断本次响应疑似降智或不可用时，会在本地吞掉这次响应并重新请求上游。只有通过检查的响应才会返回给客户端。

本项目是一个独立的 Qt/C++11 本地 OpenAI 兼容智能代理实现，用 CMake 管理，同时产出 CLI 和 GUI 两个可执行文件。
它把 Codex 或其它 OpenAI 兼容客户端的请求转发到真实上游，并在本地检查响应里的 `reasoning_tokens`、流式 SSE 结构和上游异常；必要时代理会在内部重试，避免把可疑响应直接交给客户端。

![OpenAI Reasoning Guard 图形界面](docs/images/gui-main-window.png)

> 截图中的网址和本机路径已脱敏。



## 工作原理

> 当reason token长度为 518*n - 2 时，GPT大概率出现降智状态

核心判断信号来自响应里的 `usage.output_tokens_details.reasoning_tokens`。在已有观测中，`516`、`1034`、`1552` 这类固定 reasoning token 值经常和低质量或异常响应相关，因此默认把它们作为 guard 集合。代理解析 JSON 或 SSE 响应后，如果命中这些值，就重新请求；重试耗尽后才返回本地错误状态。

这个项目不改变模型、不修改 prompt，也不做协议互转。它解决的是请求链路里的质量守门问题：用可观测的响应结构和 reasoning token 信号，把疑似降智响应挡在客户端之前，并用自动重试争取拿到更正常的一次上游结果。

## OpenAI 返回中的 `reasoning_tokens`

`reasoning_tokens` 可以理解为“推理 token 的数量/计数”，但它不是可见回答文本的字符长度，也不是本项目按字符串、字节数或 tokenizer 自己重新计算出来的长度。它是 OpenAI 在 API 响应 `usage` 元数据里返回的一个计数字段。

OpenAI 官方说明里，Responses API 的 reasoning token 数量出现在 `usage.output_tokens_details.reasoning_tokens`。示例结构如下：

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

Chat Completions API 则使用 `usage.completion_tokens_details.reasoning_tokens`：

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

因此本项目的 guard 不是“计算回答长度”，而是读取上游返回的 usage 字段：Responses 路径读 `output_tokens_details.reasoning_tokens`，Chat Completions 路径读 `completion_tokens_details.reasoning_tokens`。OpenAI 官方文档还说明 reasoning tokens 不会作为可见内容返回，但会占用上下文窗口并按 output tokens 计费。

参考：

- OpenAI Reasoning models 文档：`https://developers.openai.com/api/docs/guides/reasoning`
- OpenAI Chat Completions API Reference：`https://developers.openai.com/api/reference/resources/chat/subresources/completions/methods/create`

## 组件

- `openai-reasoning-guard-cli`: 安装后的 headless 命令行代理入口。
- `openai-reasoning-guard-gui`: 安装后的 Qt Widgets 图形界面入口。
- `net-tunnel-core`: CLI/GUI 共用核心库，包含全局配置、SQLite 上游配置、HTTP 代理、拦截策略和统计逻辑。

核心能力：

- OpenAI 兼容 HTTP 代理，支持 `/v1/responses`、`/v1/chat/completions` 等路径。
- JSON/SSE 响应缓冲和检查。
- 按 OpenAI usage 固定路径识别 `reasoning_tokens`。
- 默认 guard 集合：`516,1034,1552`。
- 命中 guard 后先内部重试；中间重试只计 retry，不计 blocked/failed；重试耗尽后才返回配置的错误状态。
- 流式 SSE 异常检测：`200` 但流不完整、缺 terminal event、缺 usage 或出现 failed/error event 时，不当作成功透传。
- 请求体和响应体资源限制、请求/响应缓冲超时、流式首 token 超时重试、上游请求超时。
- 支持 `Content-Length` 和 `Transfer-Encoding: chunked` 请求体。
- 多上游配置管理，支持 GUI/CLI 增删改查、分页、搜索、排序、选择和 JSON 导入导出。
- 每条上游配置使用一个 HTTP 或 SOCKS5 系代理地址；留空表示直连。
- SQLite 事务、WAL 和跨进程运行锁，防止运行期间切换或改坏当前配置。
- 控制接口：`/healthz`、`/status`、`/version`、`/props`。



## 下载与安装

普通用户不需要从源码构建。请到 GitHub Releases 下载对应平台的发布包：

```text
https://github.com/thb1314/openai-reasoning-guard/releases
```

推荐下载 `latest` 或 `nightly` 中和系统匹配的资产：

| 平台 | 推荐资产 | 安装方式 |
| --- | --- | --- |
| Linux Debian/Ubuntu x86_64 | `openai-reasoning-guard_<version>_amd64.deb` | `sudo apt install ./openai-reasoning-guard_<version>_amd64.deb` |
| Linux Debian/Ubuntu arm64 | `openai-reasoning-guard_<version>_arm64.deb` | `sudo apt install ./openai-reasoning-guard_<version>_arm64.deb` |
| Linux RPM 系发行版 | `openai-reasoning-guard-<version>-1.<arch>.rpm` | `sudo dnf install ./openai-reasoning-guard-<version>-1.<arch>.rpm` 或 `sudo rpm -Uvh ...` |
| Linux 免安装 | `openai-reasoning-guard-gui-<version>-<arch>.AppImage` / `openai-reasoning-guard-cli-<version>-<arch>.AppImage` | `chmod +x *.AppImage` 后直接运行 |
| Windows | `openai-reasoning-guard-windows-x86_64-<version>-installer.exe` | 双击安装；免安装版下载 `portable.zip` |
| Windows ARM64 | `openai-reasoning-guard-windows-arm64-<version>-installer.exe` | 双击安装；免安装版下载 `portable.zip` |
| macOS Apple Silicon | `openai-reasoning-guard-macos-aarch64-<version>-installer.sh` | `bash openai-reasoning-guard-macos-aarch64-<version>-installer.sh` |
| macOS Intel | `openai-reasoning-guard-macos-x86_64-<version>-installer.sh` | `bash openai-reasoning-guard-macos-x86_64-<version>-installer.sh` |

Linux deb/rpm 安装后可直接运行：

```bash
openai-reasoning-guard-gui
openai-reasoning-guard-cli --help
```

macOS shell installer 会请求 `sudo` 权限，把 app 安装到 `/Applications/OpenAI Reasoning Guard.app`，并安装 CLI 包装脚本到 `/usr/local/bin/openai-reasoning-guard-cli`。

## CLI

首次使用先创建上游配置。第一条配置会自动成为当前配置：

```bash
openai-reasoning-guard-cli profile add \
  --name "主线路" \
  --base-url https://api.openai.com/v1 \
  --api-key sk-example

openai-reasoning-guard-cli profile select "主线路"
```

然后使用当前配置启动智能代理：

```bash
openai-reasoning-guard-cli --proxy-host 127.0.0.1 --proxy-port 8010
```

不带 `profile` 子命令时，CLI 从 SQLite 加载最后选中的上游配置。如果已选 UUID 缺失或已失效，且数据库中仍有配置、选择锁也可用，程序会自动选中更新时间最新的一条；只有无法恢复出可用配置时才报错。旧脚本仍可通过 `--upstream-base-url` 临时启动，所有旧 `--upstream-*` 启动参数只覆盖本次运行，不会修改已保存的上游配置。无已选配置的一次性运行仍会持有选择锁；运行期间可以新增配置，但停止代理前不能把它设为当前。

常用管理命令：

```bash
openai-reasoning-guard-cli profile list --page 1 --page-size 20
openai-reasoning-guard-cli profile show "主线路"
openai-reasoning-guard-cli profile update "主线路" --upstream-timeout 1200
openai-reasoning-guard-cli profile delete "备用线路"
```

查询已运行代理的状态：

```bash
openai-reasoning-guard-cli --query-status --proxy-host 127.0.0.1 --proxy-port 8010
```

也可以显式指定完整状态地址：

```bash
openai-reasoning-guard-cli --query-status --query-url http://127.0.0.1:8010/status
```

只把监听、缓冲和 Guard 等全局启动设置写回 `config.json`：

```bash
openai-reasoning-guard-cli --keep-config ...
```

`--keep-config` 不保存 `--upstream-base-url`、`--upstream-api-key`、上游代理、User-Agent 或上游超时；永久修改这些字段必须使用 `profile update`。`--api-proxy` 仍是兼容标记，CLI 始终运行代理模式；`--reasoning-516-retries` 是 `--guard-retry-attempts` 的兼容别名。

## GUI

GUI 第一屏分为四块：

- 顶部运行目标概览。
- 左侧当前上游配置、只读连接参数和可编辑的全局防护设置。
- 右上实时统计。
- 右侧信息面板与控制台日志。

主窗口的四条边和四个角均可拖拽调整尺寸，并优先使用系统窗口管理器的原生移动和缩放。调整窗口大小只会改变布局，不会改变字体或控件缩放。菜单中的“界面设置”可选择跟随系统默认字体，或设置并持久化 `8-20 pt` 的固定字体大小；大字号导致左侧配置表单超出可见宽度时，面板底部会按需显示可拖动的横向滚动条。

菜单中的“上游配置”会打开模态管理窗口。窗口支持新增、查看、编辑、删除、按显示名称或 Base URL 搜索、表头排序，以及每页 10、20、50、100 条的分页。选中表格行只表示操作对象；选择未被运行锁占用时，可点击“设为当前”显式切换。空库新增或导入第一条配置时会自动设为当前；删除当前配置时会按更新时间倒序自动补选下一条，没有下一条则选择上一条。

菜单中的“关于”会打开与主窗口风格一致的自绘窗口，其中提供作者 `thb1314` 和项目 GitHub 地址。

主窗口保留原连接字段布局，但 Base URL、API Key、User-Agent、转发 User-Agent、上游代理、上游超时和首 Token 超时均由下拉框中的配置载入并设为只读。监听地址、缓冲限制、拦截规则和 `stream_action` 等全局防护设置仍可直接编辑。

代理运行时，主窗口的上游配置下拉框可直接切换。程序会先停止代理并中断在途请求或流，释放旧配置运行锁，保存新选择后按新配置自动重启；自动重启失败时代理保持停止状态。管理窗口中正在使用的配置仍不能修改或删除，其他未使用配置仍可新增、查看和修改。配置列表为空时，“启动代理”不可用。

信息面板会展示当前配置名称、代理监听地址、上游地址、路径前缀、控制端点、buffer limit、首 Token 超时、stream action 和 reasoning guard 策略。

项目提供一键重启脚本：

```bash
scripts/restart-gui.sh
```

脚本会停掉当前用户下已有的 GUI 进程，继承旧进程的显示环境后启动新二进制。

## 配置与数据文件

全局设置与上游连接设置使用两个独立数据源：

- `config.json`：监听、缓冲限制、Guard 和界面语言等全局设置。
- `upstream-profiles.sqlite3`：上游配置、API Key 和最后选中的配置 UUID。

默认存储位置：

| 平台 | 目录示例 |
| --- | --- |
| Linux | `${XDG_CONFIG_HOME:-$HOME/.config}/OpenAI Reasoning Guard/` |
| Windows | `%APPDATA%\OpenAI Reasoning Guard\` |
| macOS | `~/Library/Application Support/OpenAI Reasoning Guard/` |

可以显式指定全局 JSON；数据库会自动放在该 JSON 的同一目录：

```bash
NET_TUNNEL_CONFIG=/srv/reasoning-guard/config.json openai-reasoning-guard-cli
openai-reasoning-guard-cli --config /srv/reasoning-guard/config.json
```

两种写法都会使用 `/srv/reasoning-guard/upstream-profiles.sqlite3`。程序会原子写入 `config.json`。在 POSIX 系统上，本程序新建的配置目录，以及数据库、WAL/SHM、配置、备份和导出文件会限制为仅属主可访问；既有自定义目录的权限保持不变并由管理员负责。Windows 默认依赖当前用户 `%APPDATA%` 的 ACL；若通过 `--config` 或 `NET_TUNNEL_CONFIG` 使用自定义或共享目录，其 ACL 隔离由用户或管理员负责。

### 全局配置示例

`config.example.json` 不包含上游 URL 或密钥，因为这些字段由 SQLite 上游配置管理。完整全局示例：

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
  "reasoning_516_retry_count": 3,
  "retry_upstream_capacity_errors": true,
  "guard_endpoints": ["/responses", "/chat/completions", "/v1/responses", "/v1/chat/completions"],
  "intercept_streaming": true,
  "intercept_non_streaming": true,
  "non_stream_status_code": 502,
  "stream_action": "strict_502"
}
```

### 旧配置迁移

首次升级时，如果用户目录尚无配置，程序会先复制可执行文件旁的旧 `config.json`。当上游配置数据库为空且旧字段能够组成一条有效上游配置时，程序会把原文件备份为 `config.json.pre-upstream-profiles.bak`，再创建名为“已迁移配置”的配置并设为当前，同时写入旧 API Key、User-Agent、转发开关、上游代理和两个超时。迁移前会按当前上游配置规则校验 Base URL、代理和两个超时；校验失败时不会改写原 JSON，需修正旧字段后重试。

旧版的 `upstream_proxy`、`upstream_http_proxy`、`upstream_https_proxy`、`upstream_socks_proxy` 会根据 Base URL 协议和旧优先级折算成一个实际代理。数据库事务成功后，旧 `upstream_*` 字段才会从当前 JSON 中移除。Base URL 为空且其余遗留字段均为空或保持旧默认值时，程序会先备份 JSON，再原子清理这些占位字段，不创建配置；如果 API Key、代理、非默认 User-Agent、转发开关或超时等仍有实际值，则迁移失败并原样保留 JSON，要求用户先补全 Base URL 或人工处理。这样升级后只有 SQLite 是上游配置的数据源，备份仍可用于人工恢复。

## 全局配置项

| 字段 | 默认值 | 示例 | 说明 |
| --- | --- | --- | --- |
| `lang` | `zh` | `"en"` | GUI 语言。`zh` 显示中文，`en` 显示英文；只影响界面显示，不影响代理行为。 |
| `ui_font_point_size` | `0` | `14` | GUI 固定字体点数。`0` 表示跟随系统默认字体；也可在“界面设置”中选择 `8-20 pt`。调整窗口大小不会改变此值或字体、控件的缩放比例。 |
| `proxy_host` | `127.0.0.1` | `"0.0.0.0"` | 本地代理监听地址。只给本机客户端使用时保持 `127.0.0.1`；需要局域网其它机器访问时可设为 `0.0.0.0`。 |
| `proxy_port` | `8010` | `8011` | 本地代理监听端口。客户端 base URL 需要使用这个端口，例如 `http://127.0.0.1:8011/v1`。 |
| `proxy_prefix` | `/v1` | `""` 或 `"/api"` | 客户端访问代理的业务路径前缀。`"/v1"` 表示客户端请求 `/v1/responses`；空字符串表示 root proxy，请求 `/responses` 直接转发。root proxy 下 `GET /` 会转发上游根路径，健康检查用 `/healthz`。 |
| `buffer_timeout_sec` | `180` | `360` | 请求体缓冲和上游响应缓冲的等待秒数。请求体未收齐返回 `408`；响应缓冲超时返回 `502`；均记录 `error_type=buffer_timeout`。 |
| `request_body_limit_bytes` | `104857600` | `10485760` | 客户端请求体最大缓冲字节数。示例为 10MB；默认 100MB。超限返回 `413`，不会请求上游。 |
| `response_buffer_limit_bytes` | `104857600` | `209715200` | 上游响应最大缓冲字节数。示例为 200MB；默认 100MB。严格流式和非流式检查都受这个限制保护，超限返回 `502`。 |
| `intercept_rule_mode` | `reasoning_tokens` | `"final_answer_only_high_xhigh"` | 拦截规则模式。`reasoning_tokens` 按 `reasoning_equals` 精确命中；`final_answer_only_high_xhigh` 是实验模式，用 high/xhigh 且只有最终答案结构作为命中特征。 |
| `reasoning_equals` | `[516,1034,1552]` | `[516, 1034]` 或 `"516,1034"` | 需要拦截的 `reasoning_tokens` 集合。配置文件推荐 JSON 数组；CLI/GUI 可用逗号或空格分隔。 |
| `guard_retry_attempts` | `3` | `10` | 每个客户端请求的共享内部重试预算，用于 guard 命中、首 token 超时和指定 capacity 错误。中间 retry 不返回给客户端；各错误在预算耗尽后按自身最终策略返回。 |
| `reasoning_516_retry_count` | `3` | `10` | 兼容字段，含义等同 `guard_retry_attempts`。保存配置时会和 `guard_retry_attempts` 保持一致。 |
| `retry_upstream_capacity_errors` | `true` | `false` | 是否对特定上游 capacity 错误做内部重试。只匹配明确的 capacity 文案，不泛化重试普通 `429/502`；重试耗尽后会原样转发最后一次上游状态码、响应头和错误 body。 |
| `guard_endpoints` | `/responses`, `/chat/completions`, `/v1/responses`, `/v1/chat/completions` | `["/responses", "/v1/responses"]` | 需要检查 reasoning guard 的路径集合。匹配时会同时看原始路径和去掉 `proxy_prefix` 后的业务路径。 |
| `intercept_streaming` | `true` | `false` | 是否对流式 SSE 响应实际拦截。关闭后仍可观察统计，但命中不会阻断客户端响应。 |
| `intercept_non_streaming` | `true` | `false` | 是否对非流式 JSON 响应实际拦截。关闭后仍可观察统计，但命中不会阻断客户端响应。 |
| `non_stream_status_code` | `502` | `503` | guard 重试耗尽或本地拦截最终返回给客户端的状态码。字段名沿用 `non_stream`，当前流式严格模式也会使用它。 |
| `stream_action` | `strict_502` | `"disconnect"` | 流式命中后的动作。`strict_502` 会整条缓冲、命中后整条丢弃并重试或返回错误；`disconnect` 在有 retry 预算时同样整条丢弃重试，预算耗尽后才边透传边扫描，若命中发生在已透传之后会丢弃命中 chunk 并断开连接。 |

## 上游配置字段

| 字段 | 默认值 | 示例 | 说明 |
| --- | --- | --- | --- |
| `id` | 自动生成 | `"550e8400-e29b-41d4-a716-446655440000"` | 不可变 UUID，用于在改名后仍稳定识别配置。GUI 不要求用户填写；CLI 的 show/update/delete/select 都可用名称或 UUID 定位。 |
| `display_name` | 空，必填 | `"主线路"` | 给人看的名称。保存时去除首尾空格，并按不区分大小写规则保持唯一，例如 `Main` 与 `main` 不能并存；不能含控制字符，最长 `512` 个 UTF-8 字节。 |
| `base_url` | 空，必填 | `"https://api.openai.com/v1"` | 完整的 HTTP/HTTPS 上游 Base URL，必须含主机名，可含路径，不能含 userinfo/凭据、query 或 fragment；保存时去除末尾多余 `/`。例如 `https://user:pass@api.example.com/v1` 会被拒绝。 |
| `api_key` | 空 | `"sk-..."` | 保存时去除首尾空白；裁剪后非空时固定使用 `Bearer <api_key>` 覆盖客户端授权，裁剪后为空则透传客户端 `Authorization`。不限制 Key 前缀，但不能含控制字符，最长 `8192` 个 UTF-8 字节。 |
| `user_agent` | `curl/8.7.1` | `"codex-cli/0.1"` | `forward_user_agent=false` 时使用本配置的 `User-Agent`。保存值允许为空，但运行时空值（包括仅空白）会回退为 `curl/8.7.1`；非空值不能含控制字符，最长 `8192` 个 UTF-8 字节。 |
| `forward_user_agent` | `false` | `true` | 为 `true` 时优先透传客户端请求的 `User-Agent`；为 `false` 时使用本配置的 `user_agent`。 |
| `upstream_proxy` | 空 | `"http://127.0.0.1:7890"` 或 `"socks5://127.0.0.1:7890"` | 当前配置唯一的上游代理。仅支持 `http`、`socks`、`socks5`、`socks5h` scheme；不支持 HTTPS proxy scheme 或 SOCKS4，也不允许 userinfo/凭据、query、fragment。留空表示直连，没有 scheme 时按 HTTP 代理规范化。 |
| `upstream_timeout_sec` | `1800` | `600` | 单次上游请求最长等待秒数，范围 `1..86400`。超时返回 `504`，并记录 `error_type=upstream_timeout`。 |
| `first_token_timeout_sec` | `30` | `10` 或 `0` | 流式请求收到首个非空上游 body 字节的等待时间，范围 `0..3600`。超时占用共享重试预算；`0` 表示禁用。 |

## API Key 与导入导出

上游配置的 API Key 以明文保存在当前用户的 `upstream-profiles.sqlite3`。程序通过用户私有目录和文件权限降低误读风险，但这不是系统密钥链或加密保险库：拥有该用户文件读取权限的人仍能取得 Key。

- GUI 编辑框默认掩码，提供显式显示/隐藏和复制按钮；列表只显示“已配置 API Key”或“透传客户端授权”。
- CLI `profile list`、默认 `profile show`、日志和状态接口均不输出完整 Key；`--json` 本身不会取消脱敏。
- 只有 `profile show ... --show-secret` 才显示完整 Key，包括同时使用 `--json` 的输出。
- API Key 为空时，代理透传客户端 `Authorization`。Codex 可以在发请求前从自身 `auth.json` 加载令牌，但本程序不会读取 `auth.json`。

GUI 和 CLI 都支持 JSON 导入导出。默认导出完全省略 `api_key` 字段；只有显式选择“包含 API Key”或使用 `--include-secrets` 才会导出明文密钥。不含密钥的文件以 `overwrite` 覆盖已有配置时会保留数据库中的原 Key，新建配置则使用空 Key。导入按 UUID 或不区分大小写的显示名称检测冲突，并要求选择 `skip` 或 `overwrite`；整个导入使用单个事务。导出不包含当前选择；导入非空数据库不会切换当前配置，导入空数据库时若选择锁可用，会把第一条新增配置设为当前。

不含密钥的导出结构示例（`api_key_configured` 只表示是否已配置，不包含 Key 内容）：

```json
{
  "schema_version": 1,
  "exported_at_utc": "2026-07-25T12:00:00.000Z",
  "secrets_included": false,
  "profiles": [
    {
      "id": "550e8400-e29b-41d4-a716-446655440000",
      "display_name": "主线路",
      "base_url": "https://api.example.com/v1",
      "user_agent": "curl/8.7.1",
      "forward_user_agent": false,
      "upstream_proxy": "",
      "upstream_timeout_sec": 1800,
      "first_token_timeout_sec": 30,
      "created_at_utc": "2026-07-25T11:00:00.000Z",
      "updated_at_utc": "2026-07-25T11:30:00.000Z",
      "api_key_configured": true
    }
  ]
}
```

```bash
openai-reasoning-guard-cli profile export --output profiles.json
openai-reasoning-guard-cli profile export --output profiles-with-keys.json --include-secrets
openai-reasoning-guard-cli profile import --input profiles.json --conflict skip
openai-reasoning-guard-cli profile import --input profiles.json --conflict overwrite
```

## CLI 上游配置命令

| 命令 | 示例 | 说明 |
| --- | --- | --- |
| `profile list` | `profile list --page 1 --page-size 20 --search openai --sort updated-at --order desc` | 分页列出配置。每页只允许 `10`、`20`、`50`、`100`；搜索显示名称和 Base URL；排序字段为 `name`、`base-url`、`updated-at`。 |
| `profile show <名称或UUID>` | `profile show "主线路" --json` | 查看一条配置，默认只显示授权状态；加 `--show-secret` 才输出完整 Key。 |
| `profile add` | `profile add --name "主线路" --base-url https://api.example.com/v1 --api-key sk-...` | 新增配置。名称和 Base URL 必填；第一条配置在没有运行锁时自动成为当前配置。 |
| `profile update <名称或UUID>` | `profile update "主线路" --proxy http://127.0.0.1:7890 --no-forward-user-agent` | 只更新显式给出的字段。使用 `--api-key=` 清空 Key，使用 `--proxy=` 改为直连。 |
| `profile delete <名称或UUID>` | `profile delete "备用线路"` | 删除配置。删除当前配置后按更新时间倒序选择下一条，没有下一条则选择上一条；正在运行的配置不能删除。 |
| `profile select <名称或UUID>` | `profile select "主线路"` | 保存最后选择；代理运行期间不能切换。 |
| `profile export` | `profile export --output profiles.json --include-secrets` | 导出全部配置；默认不含 Key，`--include-secrets` 明文导出。 |
| `profile import` | `profile import --input profiles.json --conflict overwrite` | 导入全部配置；`--conflict skip|overwrite` 必须显式指定。 |

所有管理命令都接受 `--config <path>`；`list/show/add/update/delete/select/import/export` 均支持 `--json` 机器可读输出。`add`/`update` 的字段选项还包括 `--user-agent`、`--forward-user-agent`、`--no-forward-user-agent`、`--upstream-timeout` 和 `--first-token-timeout`，其中 `--upstream-*` 拼写可作为 Base URL、Key、User-Agent 和单一代理的兼容别名。

## CLI 启动参数

| CLI 参数 | 示例 | 说明 |
| --- | --- | --- |
| `--config <path>` | `--config /srv/guard/config.json` | 指定全局 JSON；profile 数据库使用同目录的 `upstream-profiles.sqlite3`。 |
| `--api-proxy` | `--api-proxy` | 兼容标记；CLI 始终运行代理模式，不改变任何配置。 |
| `--proxy-host` | `--proxy-host 127.0.0.1` | 覆盖 `proxy_host`。 |
| `--proxy-port` | `--proxy-port 8011` | 覆盖 `proxy_port`。 |
| `--proxy-prefix` | `--proxy-prefix /v1` 或 `--proxy-prefix ""` | 覆盖 `proxy_prefix`；空字符串表示 root proxy。 |
| `--upstream-base-url` | `--upstream-base-url https://api.openai.com/v1` | 临时覆盖当前 profile 的 Base URL；没有 profile 时也可用于一次性启动。不会写入数据库。 |
| `--upstream-api-key` | `--upstream-api-key sk-...` | 临时覆盖 profile Key；运行时去除首尾空白，裁剪后非空则强制覆盖客户端 Authorization，为空则透传。 |
| `--upstream-user-agent` | `--upstream-user-agent curl/8.7.1` | 临时覆盖 profile User-Agent。 |
| `--forward-user-agent` / `--no-forward-user-agent` | `--forward-user-agent` | 临时启用/关闭客户端 `User-Agent` 透传，两者互斥。 |
| `--upstream-proxy` | `--upstream-proxy http://127.0.0.1:7890` | 临时覆盖 profile 的单一代理；空值表示直连。 |
| `--upstream-http-proxy` | `--upstream-http-proxy http://127.0.0.1:7890` | 旧脚本兼容的本次运行 HTTP 代理字段，不持久化。 |
| `--upstream-https-proxy` | `--upstream-https-proxy http://127.0.0.1:7890` | 旧脚本兼容的本次运行 HTTPS 代理字段，不持久化。 |
| `--upstream-socks-proxy` | `--upstream-socks-proxy socks5://127.0.0.1:7890` | 旧脚本兼容的本次运行 SOCKS 代理字段，不持久化。 |
| `--upstream-timeout` | `--upstream-timeout 1800` | 临时覆盖 profile 的上游超时。 |
| `--first-token-timeout` | `--first-token-timeout 30` 或 `--first-token-timeout 0` | 临时覆盖 profile 的首 Token 超时；`--upstream-first-byte-timeout` 是兼容别名。 |
| `--buffer-timeout` | `--buffer-timeout 360` | 覆盖 `buffer_timeout_sec`。 |
| `--request-body-limit-bytes` | `--request-body-limit-bytes 104857600` | 覆盖 `request_body_limit_bytes`。 |
| `--response-buffer-limit-bytes` | `--response-buffer-limit-bytes 104857600` | 覆盖 `response_buffer_limit_bytes`。 |
| `--intercept-rule-mode` | `--intercept-rule-mode reasoning_tokens` | 覆盖 `intercept_rule_mode`。 |
| `--reasoning-equals` | `--reasoning-equals 516,1034,1552` | 覆盖 `reasoning_equals`。 |
| `--guard-retry-attempts` | `--guard-retry-attempts 10` | 覆盖 `guard_retry_attempts`。 |
| `--reasoning-516-retries` | `--reasoning-516-retries 10` | 兼容别名，等同设置 `guard_retry_attempts`。 |
| `--retry-upstream-capacity-errors` | `--retry-upstream-capacity-errors` | 设置 `retry_upstream_capacity_errors=true`。 |
| `--no-retry-upstream-capacity-errors` | `--no-retry-upstream-capacity-errors` | 设置 `retry_upstream_capacity_errors=false`。 |
| `--guard-endpoints` | `--guard-endpoints /responses,/v1/responses` | 覆盖 `guard_endpoints`。 |
| `--no-intercept-streaming` | `--no-intercept-streaming` | 临时关闭 `intercept_streaming`。 |
| `--no-intercept-non-streaming` | `--no-intercept-non-streaming` | 临时关闭 `intercept_non_streaming`。 |
| `--non-stream-status-code` | `--non-stream-status-code 502` | 覆盖 `non_stream_status_code`。 |
| `--stream-action` | `--stream-action strict_502` 或 `--stream-action disconnect` | 覆盖 `stream_action`。 |
| `--query-status` | `--query-status` | 查询已运行代理并退出。 |
| `--query-url` | `--query-url http://127.0.0.1:8010/status` | 指定查询状态 URL。 |
| `--status-json` | `--status-json` | 启动后打印一次状态 JSON。 |
| `--keep-config` | `--keep-config` | 只把最终全局启动设置写回 `config.json`，不保存任何上游临时覆盖。 |

## 控制接口

- `GET /healthz`：轻量健康检查，返回当前配置摘要。
- `GET /status` 或 `GET /metrics`：返回健康信息和运行统计。
- `GET /version` 或 `GET /v1/version`：返回代理版本和配置能力。
- `GET /props` 或 `GET /v1/props`：返回客户端可探测的功能开关。

`proxy_prefix` 为空时，`GET /` 不作为健康检查固定占用，而是作为业务请求转发上游根路径；请使用 `/healthz` 做健康检查。

## 运行统计口径

`/status` 的 `runtime` 会展示本次代理启动以来的统计：

实时概览分别显示业务请求、控制请求和处理中请求。业务请求口径为 `intercepted_requests_total = successful_requests_total + failed_requests_total + in_flight_proxy_requests`；总 HTTP 请求口径则是 `requests_total = control_requests_total + intercepted_requests_total`，所以控制接口和尚未结束的业务请求会让“成功 + 失败”暂时小于总请求数。

- `requests_total`：所有请求总数，包含控制接口。
- `control_requests_total`：控制接口请求总数。
- `health_requests_total`：健康检查请求数。
- `status_requests_total`：状态、版本和 props 请求数。
- `intercepted_requests_total`：进入代理转发路径的业务请求数。
- `completed_proxy_requests_total`：已经产生最终结果的业务请求数，等于成功数与失败数之和。
- `in_flight_proxy_requests`：当前仍在处理中的业务请求数。
- `upstream_attempts_total`：实际发往上游的尝试次数，内部重试会增加这个值。
- `successful_requests_total` / `failed_requests_total`：最终对客户端完成的代理请求成功/失败数。
- `proxy_error_total`：网络代理层错误数。
- `upstream_http_error_total`：上游 HTTP 错误统计，本地 limit、bad request、timeout 不计入这个口径。
- `client_connection_error_total`：客户端提前断开或连接异常数。
- `buffer_timeout_total`：请求体或响应缓冲超时数。
- `upstream_timeout_total`：上游请求超时数。
- `first_token_timeout_total`：流式请求未在配置时间内收到首个上游响应 body 字节的尝试次数，包含随后重试成功的尝试。
- `first_token_timeout_retry_total`：首 token 超时实际触发的内部重试次数。
- `local_proxy_error_total`：本地请求/响应限制、bad request 等本地错误数。
- `inspected_response_count`：被 guard 检查过的响应数。
- `bypassed_proxy_request_count`：未进入 guard 检查的业务请求数。
- `matched_response_count`：命中当前拦截规则的响应数。
- `guard_match_rate`：Guard 命中率，计算方式为 `matched_response_count / inspected_response_count`；按被检查的上游响应计数，包含内部重试产生的响应。
- `matched_streaming_count` / `matched_non_streaming_count`：命中当前拦截规则的流式/非流式响应次数。
- `blocked_response_count`：重试耗尽后最终阻断客户端响应的次数；GUI 显示为“最终阻断”，不等同于 Guard 命中次数。
- `blocked_streaming_count` / `blocked_non_streaming_count`：最终实际拦截的流式/非流式响应次数。
- `guard_retry_total`：guard 或受保护异常触发的内部重试次数。
- `reasoning_tokens_516_retry_total`：因 `reasoning_tokens=516` 触发的内部重试次数。
- `observed_reasoning_counts`：观察到的 reasoning token 计数分布。
- `status_code_counts`：最终返回给客户端的状态码分布。
- `last_result` / `last_failure`：最近一次代理结果和最近一次失败详情，包含 `error_type`、状态码和耗时。

## 目录结构

```text
CMakeLists.txt
config.example.json
CONTEXT.md
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
  core/   # 全局配置、SQLite 上游配置、HTTP 代理、拦截与统计
  cli/    # 命令行入口和 profile 管理命令
  gui/    # Qt Widgets 主窗体和上游配置管理窗口
tests/
  cli_profile_commands_test.cpp
  http_proxy_server_test.cpp
  main_window_profile_test.cpp
  upstream_profile_dialog_test.cpp
  upstream_profile_store_test.cpp
docs/
  ARCHITECTURE.md
  adr/
```

## 验证

```bash
cmake --build build -j2
cd build
ctest --output-on-failure
```

当前 QtTest 覆盖请求体/响应体限制、缓冲与上游超时、chunked 解码、客户端断开取消上游、root proxy、流式 guard 重试、SSE 完整性和授权优先级；上游配置测试覆盖 CRUD、UUID/名称唯一性、URL 校验、分页/搜索/排序、选择恢复、旧字段迁移与代理折算、运行锁、密钥脱敏、事务导入导出、CLI 管理命令和主窗口只读映射。发布包测试还检查 QtSql runtime 与 QSQLITE driver 是否实际存在。


## 构建

```bash
cmake -S . -B build
cmake --build build -j2
```

默认使用当前环境里的显式 Qt 5 SDK。项目需要 Qt Core、Network、Gui、Widgets 和 Sql；构建测试时还需要 QtTest，运行时必须包含 QSQLITE driver。CI 统一按 Qt 5.15.x 生产 SDK archive：

```text
/mnt/data/qt-2080ti-sync/qt5.15.2-openssl
```

如需替换 Qt 路径：

```bash
cmake -S . -B build \
  -DNET_TUNNEL_QT_SDK_ROOT=/path/to/qt5
```

本机开发构建默认使用 `/mnt/data/qt-2080ti-sync` 下的自编译 Qt5，不使用系统 Qt5。

QSQLITE 使用 Qt SDK 随附的 SQLite driver，最终用户无需额外安装 `sqlite3` 命令。自定义 Qt SDK 必须同时提供 QtSql 库、Qt5Sql CMake package 和对应平台的 `sqldrivers` 插件。

源码开发构建的 CMake target 内部仍保留历史名称。正式安装包里的用户入口是 `openai-reasoning-guard-cli` 和 `openai-reasoning-guard-gui`。

## 打包

项目提供三个本地打包入口：

- Linux: [scripts/package-linux.sh](scripts/package-linux.sh)，产出 `.deb`、`.rpm`、GUI AppImage 和 CLI AppImage。
- Windows: [scripts/package-windows-mingw.sh](scripts/package-windows-mingw.sh)，在 Linux 上用 MinGW 交叉编译并产出 installer `.exe` 和 portable `.zip`；[scripts/package-windows.ps1](scripts/package-windows.ps1) 保留给原生 Windows/MSVC 构建。
- macOS: [scripts/package-macos.sh](scripts/package-macos.sh)，产出自解压 shell installer，内部包含 GUI app、CLI 和临时 DMG 载荷。

所有平台都按同一个原则处理 Qt：构建和打包只使用显式 Qt SDK，不自动使用系统 Qt5。本机 Linux 默认搜索 `/mnt/data/qt-2080ti-sync` 下的自编译 Qt5；CI 通过各架构 Qt SDK archive 提供 Qt，优先使用 secret URL，未配置时回退到标准 SDK Release 资产。SDK 和最终包都必须包含 QtSql 及 QSQLITE 插件，GUI 与 CLI AppImage 使用同一套数据库运行时。

### Linux

本机一键构建 `.deb`、`.rpm`、GUI AppImage 和 CLI AppImage：

```bash
scripts/package-linux.sh --all --clean
```

只构建其中一种：

```bash
scripts/package-linux.sh --deb --clean
scripts/package-linux.sh --rpm --clean
scripts/package-linux.sh --appimage --clean
```

产物输出：

```text
dist/openai-reasoning-guard_<version>_amd64.deb
dist/openai-reasoning-guard-<version>-1.x86_64.rpm
dist/openai-reasoning-guard-gui-<version>-x86_64.AppImage
dist/openai-reasoning-guard-cli-<version>-x86_64.AppImage
```

脚本默认只在 `/mnt/data/qt-2080ti-sync` 下寻找自编译 Qt5，不会自动退回系统 Qt5。CI 或其它机器需要显式传入 Qt SDK 路径：

```bash
QT_ROOT=/path/to/qt5 scripts/package-linux.sh --all --clean
```

可用环境变量：

| 变量 | 示例 | 说明 |
| --- | --- | --- |
| `PACKAGE_ID` | `openai-reasoning-guard` | Linux package 名、安装目录名、AppImage 文件名前缀，以及 Windows/macOS 默认包名前缀。 |
| `APP_NAME` | `"OpenAI Reasoning Guard"` | desktop 文件里的应用显示名。 |
| `GUI_COMMAND` | `openai-reasoning-guard-gui` | 安装后的 GUI 命令名。 |
| `CLI_COMMAND` | `openai-reasoning-guard-cli` | 安装后的 CLI 命令名。 |
| `ICON_SOURCE` | `assets/openai-reasoning-guard-icon-1024.png` | 打包用应用图标。不存在时脚本会回退到内置 SVG。 |
| `QT_ROOT` | `/mnt/data/qt-2080ti-sync/qt5.15.2-openssl` | Qt SDK 根目录，必须包含 `bin/moc`、`lib/libQt5Core.so.5`、`lib/libQt5Sql.so.5`、`plugins/platforms/libqxcb.so` 和 `plugins/sqldrivers/libqsqlite.so`。 |
| `LOCAL_QT_BASE` | `/mnt/data/qt-2080ti-sync` | 本机默认 Qt 搜索根目录；只有 `QT_ROOT` 为空时使用。 |
| `OPENSSL_ROOT` | `/path/to/openssl` | 可选 OpenSSL runtime 根目录；未设置时优先从 Qt 的 `lib` 目录拷贝。 |
| `DEB_ARCH` | `amd64` | 覆盖 deb 架构名。自动值通常是 `amd64`、`i386`、`arm64` 或 `armhf`。 |
| `RPM_ARCH` | `x86_64` | 覆盖 rpm 架构名。自动值通常是 `x86_64`、`i686`、`aarch64` 或 `armv7hl`。 |
| `RPM_RELEASE` | `1` | rpm release 字段，也会出现在 rpm 文件名中。 |
| `APPIMAGE_ARCH` | `x86_64` | 覆盖 AppImage 架构名。自动值通常是 `x86_64`、`i686`、`aarch64` 或 `armhf`。 |
| `VERSION` | `0.1.0` | 覆盖包版本号。默认读取 CMake project version。 |
| `BUILD_DIR` | `build-package` | Release 构建目录。 |
| `DIST_DIR` | `dist` | 包产物输出目录。 |
| `WORK_DIR` | `.package-work` | 打包 staging 临时目录。 |
| `TOOL_DIR` | `$HOME/.cache/openai-reasoning-guard/package-tools` | `appimagetool` 缓存目录；`--clean` 不会删除这里。 |
| `APPIMAGETOOL` | `/path/to/appimagetool` | 显式指定本机已有的 `appimagetool`，可完全跳过下载。 |
| `JOBS` | `8` | 并行编译任务数。 |
| `BUILD_TESTS` | `ON` | 打包构建时是否编译 QtTest。默认 `OFF`。 |
| `SKIP_BUILD` | `1` | 跳过编译，直接使用 `BUILD_DIR` 里的现有二进制出包。 |
| `DOWNLOAD_PROXY` | `http://127.0.0.1:7890` | 下载 `appimagetool` 时使用的代理。 |

运行时 Qt 库会被打进 `/opt/openai-reasoning-guard/qt`，其中包含 `libQt5Sql.so.5` 和 `plugins/sqldrivers/libqsqlite.so`。安装后的正式入口是 `/usr/bin/openai-reasoning-guard-gui` 和 `/usr/bin/openai-reasoning-guard-cli`，同时保留 `/usr/bin/net-tunnel-gui` 和 `/usr/bin/net-tunnel-cli` 兼容 symlink。包内 wrapper 使用 `${XDG_CONFIG_HOME:-$HOME/.config}/OpenAI Reasoning Guard/`，并会从旧的 `openai-reasoning-guard` 或 `net-tunnel-cpp-client` 目录复制一次 `config.json`；SQLite 数据库随后在新目录创建。

### Windows

CI 默认使用 MinGW 路线：在 Linux 上安装 MinGW 编译器，配合 Windows MinGW Qt SDK 交叉编译。Qt SDK 必须同时包含 Linux 可执行的 Qt host tools 和 Windows 目标库：

- `bin/moc`、`bin/rcc`、`bin/uic`：Linux host 工具，供 CMake 的 automoc/autorcc/autouic 使用。
- `bin/Qt5Core.dll`、`bin/Qt5Network.dll`、`bin/Qt5Gui.dll`、`bin/Qt5Widgets.dll`、`bin/Qt5Sql.dll`：Windows runtime DLL。
- `plugins/platforms/qwindows.dll` 和 `plugins/sqldrivers/qsqlite.dll`：Windows 平台及 SQLite 插件。
- `lib/cmake/Qt5/Qt5Config.cmake`、`lib/cmake/Qt5Sql/Qt5SqlConfig.cmake` 和对应 Core/Sql MinGW import lib。

打包脚本会生成两个文件：

- installer `.exe`：NSIS 安装包。
- portable `.zip`：自包含便携归档，解压后直接运行。

包内包含：

- `openai-reasoning-guard-gui.exe`
- `openai-reasoning-guard-cli.exe`
- Qt DLL、`plugins/platforms/qwindows.dll`、`plugins/sqldrivers/qsqlite.dll`、字体、配置示例和说明文件

Linux/MinGW 示例：

```bash
QT_ROOT=/path/to/qt-5.15.x-mingw64-posix \
MINGW_TRIPLE=x86_64-w64-mingw32 \
scripts/package-windows-mingw.sh --arch x86_64 --clean
```

Windows ARM64 使用 `llvm-mingw` 的 `aarch64-w64-mingw32` 工具链：

```bash
QT_ROOT=/path/to/qt-5.15.x-mingw-arm64 \
MINGW_TRIPLE=aarch64-w64-mingw32 \
MINGW_BIN_DIR=/path/to/llvm-mingw/bin \
scripts/package-windows-mingw.sh --arch arm64 --clean
```

输出示例：

```text
dist/openai-reasoning-guard-windows-x86_64-0.1.0-installer.exe
dist/openai-reasoning-guard-windows-x86_64-0.1.0-portable.zip
```

原生 Windows/MSVC 路线仍可用，适合已有 MSVC Qt SDK 的机器：

```powershell
powershell -ExecutionPolicy Bypass -File scripts/package-windows.ps1 `
  -Arch x86_64 `
  -QtRoot C:\Qt\5.15.2\msvc2019_64 `
  -Clean
```

`--arch x86_32` / `-Arch x86_32` 用于 32 位 Windows 包，`--arch arm64` / `-Arch arm64` 用于 Windows ARM64 包。Qt SDK 必须和当前 C++ 编译器 ABI 一致，例如 MinGW x86_64 使用 64 位 MinGW Qt，Windows ARM64 使用 `aarch64-w64-mingw32` Qt，MSVC x86_64 使用 64 位 MSVC Qt。

### macOS

macOS 打包脚本会创建 `.app` bundle，使用 `macdeployqt` 收集 Qt frameworks、`QtSql.framework` 和 `PlugIns/sqldrivers/libqsqlite.dylib`，先生成临时 DMG 载荷，再把该 DMG 嵌入一个自解压 shell installer。最终发布给用户的是 `.sh`，执行后会请求 `sudo` 权限，挂载内部 DMG，把 app 安装到 `/Applications`，移除 app 的 quarantine 标记，并安装 `/usr/local/bin/openai-reasoning-guard-cli` CLI 包装脚本。

示例：

```bash
QT_ROOT=/path/to/Qt/5.15.2/clang_64 scripts/package-macos.sh --arch x86_64 --clean
QT_ROOT=/path/to/Qt/5.15.2/macos scripts/package-macos.sh --arch aarch64 --clean
```

输出示例：

```text
dist/openai-reasoning-guard-macos-x86_64-0.1.0-installer.sh
dist/openai-reasoning-guard-macos-aarch64-0.1.0-installer.sh
```

macOS 分发有两种签名模式：

- 默认模式：使用 ad-hoc 签名，保证 bundle 内 Qt framework、plugin、GUI 和 CLI 的签名结构完整。installer 运行时会请求 `sudo` 权限，并对安装到 `/Applications` 的本地副本执行 `xattr -dr com.apple.quarantine`。
- 正式分发模式：使用 Apple Developer Program 的 `Developer ID Application` 证书签名，并提交 Apple notarization。公证成功并 stapled 后，内部 DMG/app 具备正式分发签名；installer 仍按同一路径安装到 `/Applications`。

DMG 外观默认按 Finder 图标视图布置为“把 app 拖到 Applications”的安装盘样式，并生成浅色背景图。相关开关：

```bash
MACOS_DMG_STYLE=1              # 生成可拖拽安装布局，默认开启
MACOS_DMG_STYLE_STRICT=0       # Finder 布局失败时是否让打包失败，默认只警告
MACOS_DMG_BACKGROUND=1         # 生成 DMG 背景图，默认开启
MACOS_KEEP_DMG=0               # 默认只发布 shell installer；设为 1 时同时保留内部 DMG
```

用户安装示例：

```bash
bash openai-reasoning-guard-macos-aarch64-0.1.0-installer.sh
```

installer 环境开关：

```bash
OPEN_AFTER_INSTALL=0 bash openai-reasoning-guard-macos-aarch64-0.1.0-installer.sh
INSTALL_CLI_SYMLINK=0 bash openai-reasoning-guard-macos-aarch64-0.1.0-installer.sh
```

GitHub Actions 若要启用正式分发模式，需要配置以下 secrets：

```text
MACOS_CERTIFICATE_P12_BASE64   # Developer ID Application 证书 .p12 的 base64
MACOS_CERTIFICATE_PASSWORD     # .p12 密码
MACOS_CODESIGN_IDENTITY        # 例如 Developer ID Application: Your Name (TEAMID)
MACOS_NOTARY_APPLE_ID          # Apple ID
MACOS_NOTARY_TEAM_ID           # Team ID
MACOS_NOTARY_PASSWORD          # app-specific password
```

也可以使用已保存的 notarytool profile：

```text
MACOS_NOTARY_PROFILE
```

### GitHub Actions

项目有两条 CI 流水线：

- [.github/workflows/qt-sdk.yml](.github/workflows/qt-sdk.yml)：从 Qt/OpenSSL 源码构建可复用 Qt SDK archive，并上传到标准 GitHub Release tag。
- [.github/workflows/linux-packages.yml](.github/workflows/linux-packages.yml)：使用 Qt SDK archive 构建最终用户安装包，可手动触发，也会在 `v*` tag 上触发。

打包 workflow 手动触发时 `target` 默认只构建 `linux-x86_64`，也可以选择单个目标或 `all`。workflow 覆盖以下目标：

| 目标 | runner/容器 | 产物 | Qt SDK secret |
| --- | --- | --- | --- |
| Linux x86_64 | `linux/amd64` Docker | deb、rpm、GUI AppImage、CLI AppImage | `QT_LINUX_X86_64_URL` |
| Linux x86_32 | `linux/386` Docker | deb、rpm、GUI AppImage、CLI AppImage | `QT_LINUX_X86_32_URL` |
| Linux arm64 | `linux/arm64` Docker | deb、rpm、GUI AppImage、CLI AppImage | `QT_LINUX_ARM64_URL` |
| Linux arm32 | `linux/arm/v7` Docker | deb、rpm、GUI AppImage、CLI AppImage | `QT_LINUX_ARM32_URL` |
| Windows x86_64 | Ubuntu runner + MinGW `x86_64-w64-mingw32` | installer `.exe`、portable `.zip` | `QT_WINDOWS_X86_64_URL` |
| Windows x86_32 | Ubuntu runner + MinGW `i686-w64-mingw32` | installer `.exe`、portable `.zip` | `QT_WINDOWS_X86_32_URL` |
| Windows ARM64 | Ubuntu runner + llvm-mingw `aarch64-w64-mingw32` | installer `.exe`、portable `.zip` | `QT_WINDOWS_ARM64_URL` |
| macOS x86_64 | `macos-15-intel` | shell installer | `QT_MACOS_X86_64_URL` |
| macOS aarch64 | `macos-14` | shell installer | `QT_MACOS_ARM64_URL` |

默认每个成功 job 都会先上传 Actions artifact，artifact 适合检查构建结果但会过期。要把产物放进 GitHub Release：

- 推送 `v*` tag 时自动发布到同名 Release，例如 `v0.1.0`。
- 手动触发时把 `publish_release` 设为 `true`，默认发布到 `nightly` prerelease，也可以填写其它 `release_tag`。

每个 Qt SDK secret 的值是一个可下载 archive URL，支持 `tar`、`tar.gz`、`tar.xz`、`tgz` 或 `zip`。如果 secret 为空，打包 workflow 会尝试从本仓库标准 Release 读取：

| 目标 | fallback Release asset |
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

archive 解压后需要能找到对应平台的 Qt 工具和 runtime：

- Linux archive：包含 `bin/moc`、`lib/libQt5Core.so.5`、`lib/libQt5Sql.so.5`、`plugins/platforms/libqxcb.so`、`plugins/sqldrivers/libqsqlite.so` 和 `lib/cmake/Qt5Sql`。
- Windows MinGW archive：包含 Linux host 工具 `bin/moc`、`bin/rcc`、`bin/uic`，Windows target runtime `bin/Qt5Core.dll`、`bin/Qt5Network.dll`、`bin/Qt5Gui.dll`、`bin/Qt5Widgets.dll`、`bin/Qt5Sql.dll`，`plugins/platforms/qwindows.dll`、`plugins/sqldrivers/qsqlite.dll`、Qt/QtSql CMake package 和 Core/Sql MinGW import lib。建议把匹配 Qt 构建器的 runtime DLL 放进 `runtime/mingw`；GCC MinGW 通常是 `libgcc_s_*.dll`、`libstdc++-6.dll`、`libwinpthread-1.dll`，Windows ARM64 的 llvm-mingw 通常是 `libc++.dll`、`libc++abi.dll`、`libunwind.dll`、`libwinpthread-1.dll`。
- macOS archive：包含 `bin/moc`、`bin/macdeployqt`、`QtSql.framework`、`plugins/sqldrivers/libqsqlite.dylib` 和 Qt5Sql CMake package。

可选 secret：

- `DOWNLOAD_PROXY`：下载 Qt SDK 或 `appimagetool` 时使用的代理，例如 `http://127.0.0.1:7890`。

CI 的 Linux 架构会先编译并运行 QtTest，再调用对应打包脚本。Linux 任务默认在 Debian bookworm 目标架构容器内运行，并通过 QEMU 覆盖 x86_32、arm64、arm32；Windows 任务在 Ubuntu runner 里用 MinGW 交叉编译，默认不运行 Windows exe；macOS 使用 GitHub 托管 runner 的原生编译器。workflow 不会安装或使用系统 Qt5。Linux 容器版本需要不低于 Qt SDK 构建时使用的 glibc 版本；如果要兼容更老发行版，应先在更老的目标容器里重新构建 Qt SDK。

### 准备 CI 用 Qt SDK

CI 的包构建依赖“目标平台可运行”的 Qt SDK archive。推荐流程是先运行 `Qt SDK Archives` workflow，从源码生成标准 SDK Release，然后直接触发包构建 workflow。只有 SDK 存放在外部地址或私有地址时，才需要把 asset URL 写入 `QT_*_URL` secret。

SDK workflow 输入：

| 输入 | 示例 | 说明 |
| --- | --- | --- |
| `target` | `windows-x86_64` | 要构建的 SDK 目标；也可以选 `all`，但会非常耗时。 |
| `qtbase_url` | `https://download.qt.io/archive/qt/5.15/5.15.2/submodules/qtbase-everywhere-src-5.15.2.tar.xz` | Qt 5.15.x qtbase 源码 archive 下载地址。 |
| `qttools_url` | `https://download.qt.io/archive/qt/5.15/5.15.2/submodules/qttools-everywhere-src-5.15.2.tar.xz` | macOS SDK 需要，用来构建 `macdeployqt`。 |
| `openssl_url` | `https://github.com/openssl/openssl/releases/download/OpenSSL_1_1_1w/openssl-1.1.1w.tar.gz` | Linux 和 Windows MinGW SDK 需要；macOS 使用 SecureTransport，不需要。 |
| `llvm_mingw_url` | `https://github.com/mstorsjo/llvm-mingw/releases/download/20260616/llvm-mingw-20260616-ucrt-ubuntu-22.04-x86_64.tar.xz` | Windows ARM64 SDK 需要，用来提供 `aarch64-w64-mingw32` cross toolchain。 |
| `release_tag` | 空 | 留空时自动使用 `qt-sdk-<target>`，这是包构建 workflow 的默认 fallback。 |
| `clean` | `true` | 是否清理 SDK build 目录后重建。 |

Linux 可以直接从 Qt 5.15.x qtbase 和 OpenSSL 1.1.1w 源码构建。脚本默认路径是：

```text
/mnt/data/qt-2080ti-sync/archives/qtbase-everywhere-src-5.15.2.tar.xz
/mnt/data/qt-2080ti-sync/archives/openssl-1.1.1w.tar.gz
```

本机 x86_64 Linux 构建并上传：

```bash
scripts/build-qt5-linux-sdk.sh \
  --target linux-x86_64 \
  --archive \
  --upload \
  --set-secret \
  --upload-proxy http://127.0.0.1:7890
```

Linux 其它架构通过 Docker/QEMU 在目标架构 Ubuntu 容器里编译同一份源码：

```bash
scripts/build-qt5-linux-sdk.sh --target linux-x86_32 --docker --archive --upload --set-secret
scripts/build-qt5-linux-sdk.sh --target linux-arm64 --docker --archive --upload --set-secret
scripts/build-qt5-linux-sdk.sh --target linux-arm32 --docker --archive --upload --set-secret
```

如果只是已有 Qt SDK，需要归档并设置 secret，不重新编译：

```bash
scripts/archive-qt-sdk.sh \
  --qt-root /path/to/qt5 \
  --target linux-x86_64 \
  --upload \
  --set-secret \
  --upload-proxy http://127.0.0.1:7890
```

macOS 在对应架构 runner/机器上原生编译，统一使用 Qt 5.15.x 的 qtbase + qttools 源码：

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

Windows CI 推荐使用 MinGW cross Qt SDK。SDK 结构是 Linux host `moc/rcc/uic` 加 Windows target `Qt5*.dll/import libs`。正式 SDK 应优先由 `build-qt5-windows-mingw-sdk.sh` 或 `Qt SDK Archives` workflow 从源码生成，避免绑定某台机器的手工目录。

已有可用 SDK 时，也可以只归档上传：

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

这里的 `--mingw-runtime-dir` 会把匹配 Qt 构建器的 `*.dll` 放进 archive 的 `runtime/mingw`，打包时优先复制这些 DLL。x86_32 需要单独准备 32 位 MinGW Qt SDK，并把 `--target` 改为 `windows-x86_32`；Windows ARM64 使用 `windows-arm64`。

如果需要从源码重新构建 Windows MinGW Qt SDK，使用：

```bash
scripts/build-qt5-windows-mingw-sdk.sh \
  --target windows-x86_64 \
  --archive \
  --upload \
  --set-secret \
  --upload-proxy http://127.0.0.1:7890
```

脚本默认使用本机已有源码：

```text
/mnt/data/qt-2080ti-sync/archives/qtbase-everywhere-src-5.15.2.tar.xz
/mnt/data/qt-2080ti-sync/archives/openssl-1.1.1w.tar.gz
```

它会按 Qt cross build 方式生成 Linux host `moc/rcc/uic` 和 Windows target `Qt5*.dll`，并用 `-openssl-runtime` 打开 QtNetwork HTTPS 支持。x86_32 使用：

```bash
scripts/build-qt5-windows-mingw-sdk.sh --target windows-x86_32 --archive --upload --set-secret
```

Windows ARM64 使用 llvm-mingw：

```bash
scripts/build-qt5-windows-mingw-sdk.sh --target windows-arm64 --archive --upload --set-secret
```

原生 Windows/MSVC SDK 也可保留作为备用路线。Windows 在对应 MSVC 开发者环境中原生编译，x86_64 用 x64 shell，x86_32 用 x86 shell，ARM64 用 ARM64 shell：

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

Windows 如果已经有可用 MSVC Qt SDK，也可以只归档上传，但当前 GitHub Actions 默认 Windows job 期待的是 MinGW cross SDK，不是 MSVC SDK：

```powershell
powershell -ExecutionPolicy Bypass -File scripts/archive-qt-sdk.ps1 `
  -QtRoot C:\Qt\5.15.2\msvc2019_64 `
  -Target windows-x86_64 `
  -Upload `
  -SetSecret
```

`--upload-proxy` / `-UploadProxy` 只用于执行上传命令的本机。不要把 GitHub Actions 的 `DOWNLOAD_PROXY` 配成 `127.0.0.1:7890`，因为 GitHub runner 上的 `127.0.0.1` 不是你的机器。

## License

项目源码采用 MIT License。仓库内 bundled 的 QUI 组件和字体资源见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)，第三方文件仍按各自上游许可使用。
