# OpenAI Reasoning Guard

本项目通过可切换的上游服务连接，识别并处理模型降智响应，同时保持代理防护策略的一致性。

## Language

**上游配置（Upstream Profile）**:
一组可复用的上游服务连接参数，必须具有显示名称和上游 Base URL，并包含带有预设默认值的凭据及连接设置。
_Avoid_: 账户、账号、用户

**选中上游配置（Selected Upstream Profile）**:
当前由主界面选中，并作为下一次代理启动时连接参数来源的上游配置。
_Avoid_: 当前账户

**显式上游授权（Explicit Upstream Authorization）**:
上游配置提供 API Key，由代理使用该 Key 构造上游授权信息并覆盖客户端传入的授权信息。

**客户端授权透传（Client Authorization Passthrough）**:
上游配置未提供 API Key，由代理原样转发客户端的授权信息；Codex 可以在请求发出前从自身 `auth.json` 取得该信息，但代理本身不读取 `auth.json`。
_Avoid_: 代理读取 auth.json
