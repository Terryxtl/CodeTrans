# CodeTrans · Windows 发送端

把剪切板里的文本、图片/截图,或拖进窗口的文件,编码成连续变化的二维码在屏幕上播放;手机摄像头扫完即可还原。单个便携 exe,不联网,不安装,支持 Win7 SP1 到 Win11,32 位和 64 位系统都用同一个 `CodeTrans.exe`。

> **安全提示:内容不加密。** 屏幕上的码任何摄像头都能读到,不要在别人看得到你屏幕的地方发送敏感内容。带密码管理器标记的剪切板内容默认会被跳过。

## 使用

| 操作 | 说明 |
|---|---|
| 复制文本 / 图片 | 窗口自动弹出(不抢焦点)并开始播放 |
| 拖入文件 | 一个或多个文件(不支持文件夹),总大小上限 256 MB |
| 截图 | Win10 1703 及以上直接用系统 Win+Shift+S;更老的系统由 CodeTrans 接管同一热键(被占用时改用 `FallbackHotkey`,默认 Ctrl+Alt+S),托盘菜单也可截图 |
| F2 / ⚙ | 展开或收起设置栏 |
| Space | 暂停或继续播放 |
| Esc / 关闭按钮 | 收回托盘(退出请用托盘菜单) |
| Ctrl+V | 重新读取剪切板 |
| `+` / `-` | 模块像素 ±1 |
| Enter | 超过 `ConfirmMB` 的大内容需要按 Enter 才开始 |

重复复制同样的内容不会重启会话,手机端已收到的进度不会丢。改模块像素、码数、帧率时沿用当前会话;改协议、版本、纠错等级或图片格式时开始新会话。

## 配置

设置写在 exe 同目录的 `codetrans.ini`;目录不可写时改用 `%APPDATA%\CodeTrans\codetrans.ini`。只能在 INI 里改的项:

| 键 | 默认 | 说明 |
|---|---|---|
| `JpegQuality` | 85 | 50–95 |
| `SkipPasswordManagers` | 1 | 跳过带 `ExcludeClipboardContentFromMonitorProcessing` / `Clipboard Viewer Ignore` 标记的内容 |
| `QuietZone` | 4 | 静区宽度(模块),1–4 |
| `ConfirmMB` | 20 | 大内容确认阈值 |
| `SnipHotkey` / `FallbackHotkey` | `Win+Shift+S` / `Ctrl+Alt+S` | 如 `Ctrl+Shift+F9` |

启动参数 `--tray`:只放进托盘,不显示窗口(开机自启用的就是这个)。

## 构建

需要 [llvm-mingw](https://github.com/mstorsjo/llvm-mingw) 的 **msvcrt** 版(UCRT 版在没装 KB2999226 的 Win7 上无法启动)、CMake ≥ 3.20、Ninja。

```sh
windows/build.sh            # 发布版(x86),并运行测试
windows/build.sh all        # 另外再构建 x64 开发版
windows/build.sh x64 --no-test
```

发布只用一个文件:`windows/build-x86/CodeTrans.exe`(32 位,在 32 位和 64 位 Windows 上都能运行)。`windows/build-x64/CodeTrans-x64.exe` 仅供开发调试。构建结束时会检查 exe 的导入表,只要依赖了系统自带以外的 DLL(如 libgcc、libwinpthread、UCRT),构建就会失败。工具链文件依次在 PATH、`$LLVM_MINGW_ROOT/bin` 和 WinGet 默认安装位置查找编译器。加 `-DCP_DEBUG_LOG=ON` 会在 exe 旁边写 `codetrans-debug.log`。

测试:

- `cpwin_tests.exe`:QR 容量表、布局、热键字符串、INI 读写、DIB/PNG/JPEG 转换、内容打包,以及多码帧渲染 → quirc → `cp_rx` 回环
- `core/cp_core_tests.exe [--quick] [--big]`:协议核心(CRC/SHA 向量、容器、帧头、RaptorQ、1 B–50 MB 丢帧回环、渲染回环)
- `cpwin_qrscan.exe shot*.png`:开发工具,把窗口截图按手机的方式解码,用于端到端检查

## 第三方代码

nanorq(MIT)、Nayuki QR Code generator(MIT)、miniz(MIT)、stb_image / stb_image_write(公有领域 / MIT)、quirc(ISC,仅测试和开发工具使用)。
