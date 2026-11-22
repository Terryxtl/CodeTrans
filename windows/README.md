# CodeTrans · Windows 端

把剪切板里的文本、图片/截图,或拖进窗口的文件,编码成连续变化的二维码在屏幕上播放;手机摄像头扫完即可还原。反方向也可以:手机播放二维码,电脑摄像头接收。单个便携 exe,不联网,不安装,支持 Win7 SP1 到 Win11,32 位和 64 位系统都用同一个 `CodeTrans.exe`。

> **安全提示:内容不加密。** 屏幕上的码任何摄像头都能读到,不要在别人看得到你屏幕的地方发送敏感内容。带密码管理器标记的剪切板内容默认会被跳过。

## 使用

| 操作 | 说明 |
|---|---|
| 复制文本 / 图片 | 窗口自动弹出(不抢焦点)并开始播放 |
| 拖入文件 | 一个或多个文件(不支持文件夹),总大小上限 256 MB |
| 截图 | Win10 1703 及以上直接用系统 Win+Shift+S;更老的系统由 CodeTrans 接管同一热键(被占用时改用 `FallbackHotkey`,默认 Ctrl+Alt+S),托盘菜单也可截图 |
| F2 / ⚙(左下角) | 展开或收起设置栏 |
| F3 / 相机按钮(右下角) | 打开「摄像头接收」窗口(手机 → 电脑) |
| Space | 暂停或继续播放 |
| Esc / 关闭按钮 | 收回托盘(退出请用托盘菜单) |
| Ctrl+V | 重新读取剪切板 |
| `+` / `-` | 模块像素 ±1 |
| Enter | 超过 `ConfirmMB` 的大内容需要按 Enter 才开始 |

重复复制同样的内容不会重启会话,手机端已收到的进度不会丢。改模块像素、码数、帧率时沿用当前会话;改协议、版本、纠错等级或图片格式时开始新会话。

图片格式「自动」(新安装的默认值):截图、图表、文字类图片保持 PNG 无损;照片类图片只有在 JPEG 不到 PNG 一半大时才改用 JPEG。容器正文的压缩也是自适应的:按内容的开头、中间、结尾取样,在几种 deflate 配置(快速 / 标准 / 最高 / 过滤 / 仅 Huffman / 仅 RLE)里选最小的,压不动就原样发送;线上仍是标准 deflate,旧版手机端照样能收。

### 摄像头接收(手机 → 电脑)

点主窗口右下角的相机按钮(或按 F3、托盘菜单「摄像头接收」)打开。窗口显示镜像的摄像头画面:绿框是已读出的码,黄框是看到但读不出的码,底部是进度。收到的内容保存在「下载\CodeTrans」:文本和图片自动用默认程序打开,其他文件只在资源管理器中选中显示(不会自动运行收到的程序)。窗口内按 C 切换摄像头、O 打开保存目录、Esc 关闭。需要 Media Foundation(Windows N 版需安装“媒体功能包”)。

## 配置

设置写在用户目录下的 `%APPDATA%\CodeTrans\codetrans.ini`(即 `C:\Users\<用户名>\AppData\Roaming\CodeTrans\`)。旧版放在 exe 旁边的 `codetrans.ini` 会在首次启动时复制过去;只有用户目录不可写时才退回 exe 所在目录。只能在 INI 里改的项:

| 键 | 默认 | 说明 |
|---|---|---|
| `JpegQuality` | 85 | 50–95 |
| `SkipPasswordManagers` | 1 | 跳过带 `ExcludeClipboardContentFromMonitorProcessing` / `Clipboard Viewer Ignore` 标记的内容 |
| `QuietZone` | 4 | 静区宽度(模块),1–4 |
| `ConfirmMB` | 20 | 大内容确认阈值 |
| `SnipHotkey` / `FallbackHotkey` | `Win+Shift+S` / `Ctrl+Alt+S` | 如 `Ctrl+Shift+F9` |
| `RecvAutoOpen` | 1 | 摄像头接收:0 = 只保存,不打开 |
| `CameraIndex` | 0 | 摄像头接收使用的设备序号(窗口里按 C 切换时自动保存) |

启动参数 `--tray`:只放进托盘,不显示窗口(开机自启用的就是这个)。

摄像头分析默认优先选用 1920×1080，并在该分辨率下优先选用不超过 60 fps 的最高帧率。接收器根据处理器数量使用 1–4 个解码线程，共享已确认的二维码位置。quirc 漏读时会按 Android 相同的条件调用 ZXing-C++ 补扫。

## 构建

需要 [llvm-mingw](https://github.com/mstorsjo/llvm-mingw) 的 **msvcrt** 版(UCRT 版在没装 KB2999226 的 Win7 上无法启动)、CMake ≥ 3.20、Ninja。

```sh
windows/build.sh            # 发布版(x86),并运行测试
windows/build.sh all        # 另外再构建 x64 开发版
windows/build.sh x64 --no-test
```

发布只用一个文件:`windows/build-x86/CodeTrans.exe`(32 位,在 32 位和 64 位 Windows 上都能运行)。`windows/build-x64/CodeTrans-x64.exe` 仅供开发调试。构建结束时会检查 exe 的导入表,只要依赖了系统自带以外的 DLL(如 libgcc、libwinpthread、UCRT),构建就会失败。工具链文件依次在 PATH、`$LLVM_MINGW_ROOT/bin` 和 WinGet 默认安装位置查找编译器。加 `-DCP_DEBUG_LOG=ON` 会在 exe 旁边写 `codetrans-debug.log`。

测试:

- `cpwin_tests.exe`:QR 容量表、布局、热键字符串、INI 读写、DIB/PNG/JPEG 转换、图片格式自动选择、内容打包、多码帧渲染 → quirc → `cp_rx` 回环,以及摄像头接收(三种像素格式取亮度、打开策略、接收线程回环)
- `core/cp_core_tests.exe [--quick] [--big]`:协议核心(CRC/SHA 向量、容器、自适应压缩、帧头、RaptorQ、1 B–50 MB 丢帧回环、渲染回环、接收诊断计数、多码部分失败时的增强重试)
- `cpwin_qrscan.exe shot*.png`:开发工具,把窗口截图按手机的方式解码,用于端到端检查
- `cpwin_camtest.exe [序号] [秒数] [out.pgm]`:开发工具,通过 camera.c 打开摄像头,报告设备、格式和实际帧率

## 第三方代码

nanorq(MIT)、Nayuki QR Code generator(MIT)、miniz(MIT)、stb_image / stb_image_write(公有领域 / MIT)、quirc(ISC)、ZXing-C++(Apache-2.0)。
