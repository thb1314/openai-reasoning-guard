# OpenAI Reasoning Guard

这个项目主要用于缓解 Codex / OpenAI 兼容接口调用中的“降智”问题：同一个模型、同一个请求，有时上游会返回明显推理不足、结构异常、空文本、缺 usage 或疑似低质量的响应。客户端如果直接接收这类响应，就会表现为答案变短、逻辑变差、任务中断或空结果。

本项目放在客户端和真实上游之间，作为本地智能网关使用。客户端仍然请求本地 OpenAI 兼容地址，代理负责把请求转发到上游；当代理判断本次响应疑似降智或不可用时，会在本地吞掉这次响应并重新请求上游。只有通过检查的响应才会返回给客户端。

这是一个独立的 Qt/C++11 本地 OpenAI 兼容智能代理实现，用 CMake 管理，同时产出 CLI 和 GUI 两个可执行文件。它把 Codex 或其它 OpenAI 兼容客户端的请求转发到真实上游，并在本地检查响应里的 `reasoning_tokens`、流式 SSE 结构和上游异常；必要时代理会在内部重试，避免把可疑响应直接交给客户端。



## 工作原理

核心判断信号来自响应里的 `usage.output_tokens_details.reasoning_tokens`。在已有观测中，`516`、`1034`、`1552` 这类固定 reasoning token 值经常和低质量或异常响应相关，因此默认把它们作为 guard 集合。代理解析 JSON 或 SSE 响应后，如果命中这些值，就按 `guard_retry_attempts` 在内部重试；重试耗尽后才返回本地错误状态。

流式响应默认不会边收边盲目放行。`stream_action=strict_502` 时，代理会先缓冲整条 SSE，确认没有命中 guard、没有 failed/error event、没有缺 terminal/usage 等异常后再透传。这样可以避免“前半段已经发给客户端，最后才发现命中降智信号”的问题。

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
- `net-tunnel-core`: CLI/GUI 共用核心库，包含配置、HTTP 代理、拦截策略和统计逻辑。

核心能力：

- OpenAI 兼容 HTTP 代理，支持 `/v1/responses`、`/v1/chat/completions` 等路径。
- JSON/SSE 响应缓冲和检查。
- 按 OpenAI usage 固定路径识别 `reasoning_tokens`。
- 默认 guard 集合：`516,1034,1552`。
- 命中 guard 后先内部重试；中间重试只计 retry，不计 blocked/failed；重试耗尽后才返回配置的错误状态。
- 流式 SSE 异常检测：`200` 但流不完整、缺 terminal event、缺 usage 或出现 failed/error event 时，不当作成功透传。
- 请求体和响应体资源限制、请求/响应缓冲超时、上游请求超时。
- 支持 `Content-Length` 和 `Transfer-Encoding: chunked` 请求体。
- 支持通用上游代理和 HTTP/HTTPS/SOCKS 拆分代理字段。
- 控制接口：`/healthz`、`/status`、`/version`、`/props`。

## 构建

```bash
cmake -S . -B build
cmake --build build -j2
```

默认使用当前环境里的显式 Qt 5 SDK。CI 统一按 Qt 5.15.x 生产 SDK archive：

```text
/mnt/data/qt-2080ti-sync/qt5-openssl
```

如需替换 Qt 路径：

```bash
cmake -S . -B build \
  -DNET_TUNNEL_QT_SDK_ROOT=/path/to/qt5
```

本机开发构建默认使用 `/mnt/data/qt-2080ti-sync` 下的自编译 Qt5，不使用系统 Qt5。

## 打包

项目提供三个本地打包入口：

- Linux: [scripts/package-linux.sh](scripts/package-linux.sh)，产出 `.deb`、`.rpm`、GUI AppImage 和 CLI AppImage。
- Windows: [scripts/package-windows-mingw.sh](scripts/package-windows-mingw.sh)，在 Linux 上用 MinGW 交叉编译并产出 installer `.exe` 和 portable `.zip`；[scripts/package-windows.ps1](scripts/package-windows.ps1) 保留给原生 Windows/MSVC 构建。
- macOS: [scripts/package-macos.sh](scripts/package-macos.sh)，产出包含 GUI app 和 CLI 的 dmg。

所有平台都按同一个原则处理 Qt：构建和打包只使用显式 Qt SDK，不自动使用系统 Qt5。本机 Linux 默认搜索 `/mnt/data/qt-2080ti-sync` 下的自编译 Qt5；CI 必须通过各架构 Qt SDK archive secret 提供 Qt。

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
| `QT_ROOT` | `/mnt/data/qt-2080ti-sync/qt5-openssl` | Qt SDK 根目录，必须包含 `bin/moc`、`lib/libQt5Core.so.5` 和 `plugins/platforms/libqxcb.so`。 |
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

运行时 Qt 库会被打进 `/opt/openai-reasoning-guard/qt`。安装后的正式入口是 `/usr/bin/openai-reasoning-guard-gui` 和 `/usr/bin/openai-reasoning-guard-cli`，同时保留 `/usr/bin/net-tunnel-gui` 和 `/usr/bin/net-tunnel-cli` 兼容 symlink。包内 wrapper 会把配置文件放到 `${XDG_CONFIG_HOME:-$HOME/.config}/openai-reasoning-guard/config.json`，旧版 `${XDG_CONFIG_HOME:-$HOME/.config}/net-tunnel-cpp-client/config.json` 存在时会自动复制一次，避免写入 `/opt`。

### Windows

CI 默认使用 MinGW 路线：在 Linux 上安装 MinGW 编译器，配合 Windows MinGW Qt SDK 交叉编译。Qt SDK 必须同时包含 Linux 可执行的 Qt host tools 和 Windows 目标库：

- `bin/moc`、`bin/rcc`、`bin/uic`：Linux host 工具，供 CMake 的 automoc/autorcc/autouic 使用。
- `bin/Qt5Core.dll`、`bin/Qt5Network.dll`、`bin/Qt5Gui.dll`、`bin/Qt5Widgets.dll`：Windows runtime DLL。
- `plugins/platforms/qwindows.dll`：Windows Qt platform plugin。
- `lib/cmake/Qt5/Qt5Config.cmake` 和 `lib/libQt5Core.a` 或 `lib/libQt5Core.dll.a`：CMake package 和 MinGW import lib。

打包脚本会生成两个文件：

- installer `.exe`：NSIS 安装包。
- portable `.zip`：单文件便携包，解压后直接运行。

包内包含：

- `openai-reasoning-guard-gui.exe`
- `openai-reasoning-guard-cli.exe`
- Qt DLL、`plugins/platforms/qwindows.dll`、字体、配置示例和说明文件

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

macOS 打包脚本会创建 `.app` bundle，使用 `macdeployqt` 收集 Qt frameworks，并生成 dmg。CLI 二进制会放在 app 的 `Contents/Resources/bin` 中，dmg 根目录也会提供一个 `bin/openai-reasoning-guard-cli` 启动脚本。

示例：

```bash
QT_ROOT=/path/to/Qt/5.15.2/clang_64 scripts/package-macos.sh --arch x86_64 --clean
QT_ROOT=/path/to/Qt/5.15.2/macos scripts/package-macos.sh --arch aarch64 --clean
```

输出示例：

```text
dist/openai-reasoning-guard-macos-x86_64-0.1.0.dmg
dist/openai-reasoning-guard-macos-aarch64-0.1.0.dmg
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
| macOS x86_64 | `macos-15-intel` | dmg | `QT_MACOS_X86_64_URL` |
| macOS aarch64 | `macos-14` | dmg | `QT_MACOS_ARM64_URL` |

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

- Linux archive：包含 `bin/moc`、`lib/libQt5Core.so.5`、`plugins/platforms/libqxcb.so`，建议同时包含 `lib/cmake/Qt5`。
- Windows MinGW archive：包含 Linux host 工具 `bin/moc`、`bin/rcc`、`bin/uic`，Windows target runtime `bin/Qt5Core.dll`、`bin/Qt5Network.dll`、`bin/Qt5Gui.dll`、`bin/Qt5Widgets.dll`，`plugins/platforms/qwindows.dll`、`lib/cmake/Qt5` 和 MinGW import lib。建议把匹配 Qt 构建器的 runtime DLL 放进 `runtime/mingw`；GCC MinGW 通常是 `libgcc_s_*.dll`、`libstdc++-6.dll`、`libwinpthread-1.dll`，Windows ARM64 的 llvm-mingw 通常是 `libc++.dll`、`libc++abi.dll`、`libunwind.dll`、`libwinpthread-1.dll`。
- macOS archive：包含 `bin/moc`、`bin/macdeployqt` 和 Qt frameworks/CMake package。

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

## CLI

启动智能代理：

开发构建的 CMake target 仍保留原名：

```bash
build/net-tunnel-cli \
  --proxy-host 127.0.0.1 \
  --proxy-port 8010 \
  --proxy-prefix /v1 \
  --upstream-base-url https://ai.input.im/v1 \
  --reasoning-equals 516,1034,1552 \
  --guard-retry-attempts 3
```

`--api-proxy` 作为兼容标记仍可传入，但 CLI 当前始终只运行代理模式。`--reasoning-516-retries` 是 `--guard-retry-attempts` 的兼容别名。

查询已运行代理的状态：

```bash
build/net-tunnel-cli --query-status --proxy-host 127.0.0.1 --proxy-port 8010
```

也可以显式指定完整状态地址：

```bash
build/net-tunnel-cli --query-status --query-url http://127.0.0.1:8010/status
```

把最终启动配置写回配置文件：

```bash
build/net-tunnel-cli --config build/config.json --keep-config ...
```

## GUI

开发构建的 CMake target 仍保留原名：

```bash
build/net-tunnel-gui
```

GUI 第一屏分为四块：

- 顶部运行目标概览。
- 左侧智能拦截与上游转发配置。
- 右上实时统计。
- 右侧信息面板与控制台日志。

信息面板会展示当前代理监听地址、上游地址、路径前缀、控制端点、buffer limit、stream action 和 reasoning guard 策略。GUI 可直接编辑 `request_body_limit_bytes`、`response_buffer_limit_bytes` 和 `stream_action`。

项目提供一键重启脚本：

```bash
scripts/restart-gui.sh
```

脚本会停掉当前用户下已有的 `net-tunnel-gui`，继承旧进程的显示环境后启动新二进制。

## 配置文件

默认配置文件位于可执行文件同级目录。开发构建通常是：

```text
build/config.json
```

也可以显式指定配置：

```bash
build/net-tunnel-cli --config config.example.json --api-proxy
```

完整示例：

```json
{
  "lang": "zh",
  "proxy_host": "127.0.0.1",
  "proxy_port": "8010",
  "proxy_prefix": "/v1",
  "upstream_base_url": "https://ai.input.im/v1",
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

## 配置项说明

| 字段 | 默认值 | 示例 | 说明 |
| --- | --- | --- | --- |
| `lang` | `zh` | `"en"` | GUI 语言。`zh` 显示中文，`en` 显示英文；只影响界面显示，不影响代理行为。 |
| `proxy_host` | `127.0.0.1` | `"0.0.0.0"` | 本地代理监听地址。只给本机客户端使用时保持 `127.0.0.1`；需要局域网其它机器访问时可设为 `0.0.0.0`。 |
| `proxy_port` | `8010` | `8011` | 本地代理监听端口。客户端 base URL 需要使用这个端口，例如 `http://127.0.0.1:8011/v1`。 |
| `proxy_prefix` | `/v1` | `""` 或 `"/api"` | 客户端访问代理的业务路径前缀。`"/v1"` 表示客户端请求 `/v1/responses`；空字符串表示 root proxy，请求 `/responses` 直接转发。root proxy 下 `GET /` 会转发上游根路径，健康检查用 `/healthz`。 |
| `upstream_base_url` | `https://ai.input.im/v1` | `"https://api.openai.com/v1"` | 真实上游 OpenAI 兼容 API base URL。代理会把去掉 `proxy_prefix` 后的业务路径拼到这个 base URL 后面。 |
| `upstream_api_key` | 空 | `"sk-..."` | 显式上游 Bearer token。非空时上游 `Authorization` 固定为 `Bearer sk-...`；为空时透传客户端 `Authorization`，也就是让 Codex 继续使用自己的 `auth.json`。 |
| `upstream_user_agent` | `curl/8.7.1` | `"codex-cli/0.1"` | 发往上游的默认 `User-Agent`。`forward_user_agent=false` 时使用该值。 |
| `forward_user_agent` | `false` | `true` | 是否把客户端请求里的 `User-Agent` 原样转给上游。为 `false` 时使用 `upstream_user_agent`。 |
| `upstream_proxy` | 空 | `"http://127.0.0.1:7890"` | 通用上游代理地址。也可填 `"socks5://127.0.0.1:7890"`。非空时优先于拆分代理字段。 |
| `upstream_http_proxy` | 空 | `"http://127.0.0.1:7890"` | HTTP 上游代理字段。当前实现也会在 HTTPS 上游缺少 `upstream_https_proxy` 时作为备选使用。保存配置时不会自动折叠进 `upstream_proxy`。 |
| `upstream_https_proxy` | 空 | `"http://127.0.0.1:7890"` | HTTPS 上游代理字段。仅在 `upstream_proxy` 为空且上游 base URL 是 HTTPS 时优先使用。 |
| `upstream_socks_proxy` | 空 | `"127.0.0.1:7890"` 或 `"socks5://127.0.0.1:7890"` | SOCKS 上游代理字段。没有 scheme 时默认补 `socks5://`。作为通用/协议代理都为空时的最后备选。 |
| `upstream_timeout_sec` | `1800` | `600` | 单次上游请求最长等待秒数。超时返回 `504`，记录 `error_type=upstream_timeout` 和 `upstream_timeout_total`。长推理场景建议保持较大值。 |
| `buffer_timeout_sec` | `180` | `360` | 请求体缓冲和上游响应缓冲的等待秒数。请求体未收齐返回 `408`；响应缓冲超时返回 `502`；均记录 `error_type=buffer_timeout`。 |
| `request_body_limit_bytes` | `104857600` | `10485760` | 客户端请求体最大缓冲字节数。示例为 10MB；默认 100MB。超限返回 `413`，不会请求上游。 |
| `response_buffer_limit_bytes` | `104857600` | `209715200` | 上游响应最大缓冲字节数。示例为 200MB；默认 100MB。严格流式和非流式检查都受这个限制保护，超限返回 `502`。 |
| `intercept_rule_mode` | `reasoning_tokens` | `"final_answer_only_high_xhigh"` | 拦截规则模式。`reasoning_tokens` 按 `reasoning_equals` 精确命中；`final_answer_only_high_xhigh` 是实验模式，用 high/xhigh 且只有最终答案结构作为命中特征。 |
| `reasoning_equals` | `[516,1034,1552]` | `[516, 1034]` 或 `"516,1034"` | 需要拦截的 `reasoning_tokens` 集合。配置文件推荐 JSON 数组；CLI/GUI 可用逗号或空格分隔。 |
| `guard_retry_attempts` | `3` | `10` | 命中 guard 后代理内部重新请求上游的次数。中间 retry 不返回给客户端；耗尽后才返回 `non_stream_status_code`。 |
| `reasoning_516_retry_count` | `3` | `10` | 兼容字段，含义等同 `guard_retry_attempts`。保存配置时会和 `guard_retry_attempts` 保持一致。 |
| `retry_upstream_capacity_errors` | `true` | `false` | 是否对特定上游 capacity 错误做内部重试。只匹配明确的 capacity 文案，不泛化重试普通 `429/502`。 |
| `guard_endpoints` | `/responses`, `/chat/completions`, `/v1/responses`, `/v1/chat/completions` | `["/responses", "/v1/responses"]` | 需要检查 reasoning guard 的路径集合。匹配时会同时看原始路径和去掉 `proxy_prefix` 后的业务路径。 |
| `intercept_streaming` | `true` | `false` | 是否对流式 SSE 响应实际拦截。关闭后仍可观察统计，但命中不会阻断客户端响应。 |
| `intercept_non_streaming` | `true` | `false` | 是否对非流式 JSON 响应实际拦截。关闭后仍可观察统计，但命中不会阻断客户端响应。 |
| `non_stream_status_code` | `502` | `503` | guard 重试耗尽或本地拦截最终返回给客户端的状态码。字段名沿用 `non_stream`，当前流式严格模式也会使用它。 |
| `stream_action` | `strict_502` | `"disconnect"` | 流式命中后的动作。`strict_502` 会整条缓冲、命中后整条丢弃并重试或返回错误；`disconnect` 在有 retry 预算时同样整条丢弃重试，预算耗尽后才边透传边扫描，若命中发生在已透传之后会丢弃命中 chunk 并断开连接。 |

## API Key 行为

`upstream_api_key` 是本地代理的显式上游密钥覆盖项。

- 如果 `upstream_api_key` 非空，代理发给上游的 `Authorization` 固定为 `Bearer <upstream_api_key>`，即使客户端请求里已经带了 `Authorization` 也会被覆盖。
- 如果 `upstream_api_key` 为空，代理不会自己生成密钥，而是透传客户端请求里的 `Authorization`。Codex 场景下，这通常就是 Codex 根据自身 `auth.json` 带上的 token。

因此：想强制走某个备用 key，就填写 `upstream_api_key`；想继续使用 Codex 原本的 `auth.json`，就把 `upstream_api_key` 留空。

## CLI 参数对照

| CLI 参数 | 示例 | 说明 |
| --- | --- | --- |
| `--config <path>` | `--config build/config.json` | 指定配置文件路径。 |
| `--proxy-host` | `--proxy-host 127.0.0.1` | 覆盖 `proxy_host`。 |
| `--proxy-port` | `--proxy-port 8011` | 覆盖 `proxy_port`。 |
| `--proxy-prefix` | `--proxy-prefix /v1` 或 `--proxy-prefix ""` | 覆盖 `proxy_prefix`；空字符串表示 root proxy。 |
| `--upstream-base-url` | `--upstream-base-url https://ai.input.im/v1` | 覆盖 `upstream_base_url`。 |
| `--upstream-api-key` | `--upstream-api-key sk-...` | 覆盖 `upstream_api_key`；非空时强制覆盖客户端 Authorization。 |
| `--upstream-user-agent` | `--upstream-user-agent curl/8.7.1` | 覆盖 `upstream_user_agent`。 |
| `--forward-user-agent` | `--forward-user-agent` | 将客户端 `User-Agent` 转发给上游。 |
| `--upstream-proxy` | `--upstream-proxy http://127.0.0.1:7890` | 覆盖 `upstream_proxy`。 |
| `--upstream-http-proxy` | `--upstream-http-proxy http://127.0.0.1:7890` | 覆盖 `upstream_http_proxy`。 |
| `--upstream-https-proxy` | `--upstream-https-proxy http://127.0.0.1:7890` | 覆盖 `upstream_https_proxy`。 |
| `--upstream-socks-proxy` | `--upstream-socks-proxy socks5://127.0.0.1:7890` | 覆盖 `upstream_socks_proxy`。 |
| `--upstream-timeout` | `--upstream-timeout 1800` | 覆盖 `upstream_timeout_sec`。 |
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
| `--keep-config` | `--keep-config` | 把最终启动配置写回 `config.json`。 |

## 控制接口

- `GET /healthz`：轻量健康检查，返回当前配置摘要。
- `GET /status` 或 `GET /metrics`：返回健康信息和运行统计。
- `GET /version` 或 `GET /v1/version`：返回代理版本和配置能力。
- `GET /props` 或 `GET /v1/props`：返回客户端可探测的功能开关。

`proxy_prefix` 为空时，`GET /` 不作为健康检查固定占用，而是作为业务请求转发上游根路径；请使用 `/healthz` 做健康检查。

## 运行统计口径

`/status` 的 `runtime` 会展示本次代理启动以来的统计：

- `requests_total`：所有请求总数，包含控制接口。
- `control_requests_total`：控制接口请求总数。
- `health_requests_total`：健康检查请求数。
- `status_requests_total`：状态、版本和 props 请求数。
- `intercepted_requests_total`：进入代理转发路径的业务请求数。
- `upstream_attempts_total`：实际发往上游的尝试次数，内部重试会增加这个值。
- `successful_requests_total` / `failed_requests_total`：最终对客户端完成的代理请求成功/失败数。
- `proxy_error_total`：网络代理层错误数。
- `upstream_http_error_total`：上游 HTTP 错误统计，本地 limit、bad request、timeout 不计入这个口径。
- `client_connection_error_total`：客户端提前断开或连接异常数。
- `buffer_timeout_total`：请求体或响应缓冲超时数。
- `upstream_timeout_total`：上游请求超时数。
- `local_proxy_error_total`：本地请求/响应限制、bad request 等本地错误数。
- `inspected_response_count`：被 guard 检查过的响应数。
- `bypassed_proxy_request_count`：未进入 guard 检查的业务请求数。
- `matched_response_count`：命中当前拦截规则的响应数。
- `matched_streaming_count` / `matched_non_streaming_count`：命中当前拦截规则的流式/非流式响应次数。
- `blocked_response_count`：最终实际拦截的响应总数。
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
  core/   # 业务核心：配置、HTTP 代理、拦截策略、统计
  cli/    # 命令行入口
  gui/    # Qt Widgets 主窗体
tests/
  http_proxy_server_test.cpp
docs/
  ARCHITECTURE.md
```

## 验证

```bash
cmake --build build -j2
cd build
ctest --output-on-failure
```

当前 QtTest 覆盖请求体/响应体限制、缓冲超时、上游超时、chunked 请求体解码、客户端断开取消上游、root proxy 根路径转发、拆分代理字段 round-trip、流式 guard 重试、SSE 不完整响应重试和授权优先级。

## License

项目源码采用 MIT License。仓库内 bundled 的 QUI 组件和字体资源见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)，第三方文件仍按各自上游许可使用。
