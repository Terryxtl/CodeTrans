# CodeTrans · Windows 发送端计划

> 状态(2026-11-20):W0–W6 已实现,本机 Win11 上通过自动测试和端到端冒烟测试;W6 按 Decimen 0.5.3-fixed 的算法移植(RS96 + LT,见 §5.4,**许可证后果见 §5.4**);W7 的 Win7 / 真机矩阵尚未做。详见第 12 节。
> 协议核心(打包、FEC、分帧)和 Android 端共用一份纯 C 代码,放在仓库根目录的 `core/` 下(见第 4、5 节)。两端协议必须逐字节一致,所以不要各写一份。

---

## 1. 目标与范围

**要做的**

- 把以下三类内容编码成连续变化的二维码在屏幕上播放,手机摄像头扫完就能还原:
  - 剪切板里的**文本**
  - 剪切板里的**图片 / 截图**
  - **拖进窗口的文件**
- 后台常驻监听剪切板,内容一变立即换码。
- 系统没有 Win+Shift+S 时(Win7、Win8/8.1、Win10 1703 之前),提供同样的热键和区域截图。截好的图写进剪切板,然后自动发送。
- 支持两种协议:
  - 默认 **Decimen v3 · RS96**(版本 40、30 fps);另可选 CodeTrans(基于 RaptorQ,RFC 6330)和 Decimen v3 · LT。
  - 可切换 **Decimen v3 兼容**,基于 LT 喷泉码。手机不装 App,浏览器打开 decimen.app 就能接收。

**不做的(保持极简)**

- 不接收,不解码。
- 不联网,没有账号,不记历史,不做安装程序(单个便携 exe)。
- 不处理资源管理器里"复制"的文件(CF_HDROP),发文件只能拖拽。剪切板里只有 CF_HDROP 时直接忽略,当前的码保持不变。
- 不加密。屏幕上的码任何摄像头都能读到,这一点写进 README。

## 2. 决定清单

| 项           | 决定                                                                                                | 来源    |
| ----------- | ------------------------------------------------------------------------------------------------- | ----- |
| 最低系统        | Win7 SP1(x86 / x64)。Vista 以后才有的 API 直接用;Win8 以后才有的 API 用 `GetProcAddress` 动态取                     | 你已确认  |
| 发送内容        | 文本、图片/截图、拖拽文件                                                                                     | 你已确认  |
| 三方库         | 只用纯 C 库:nanorq(RaptorQ)、qrcodegen(生成二维码)、miniz(deflate/zip)、stb_image_write(PNG/JPEG)、quirc(仅测试用) | 你已确认  |
| 协议          | 三种:Decimen v3 · RS96 为默认,另有 Decimen v3 · LT 和 CodeTrans(RaptorQ)                                                     | 你已确认  |
| 布局          | 码区 + 一行状态;设置栏默认收起,点 ⚙ 或按 F2 展开                                                                    | 你已确认  |
| 剪切板更新时      | 窗口自动弹出并置顶,不抢键盘焦点;按 Esc 收回托盘                                                                       | 你已确认  |
| 共享核心目录      | 仓库根目录 `core/`,两端都从这里编译                                                                            | 默认,可改 |
| 工具链         | MinGW-w64(msvcrt 运行时,见 §7)+ CMake + Ninja                                                         | 默认,可改 |
| 剪切板同时有文本和图片 | 优先发文本。Office 复制表格时会同时放文本和位图,应当发文本;截图和浏览器"复制图片"只有图片                                                | 默认,可改 |
| 图片格式        | 默认 PNG,可选 JPEG(质量可调)                                                                              | 默认,可改 |
| 关闭按钮        | 收到托盘,不退出;退出走托盘菜单                                                                                  | 默认,可改 |
| 密码管理器标记的内容  | 默认跳过(带 `ExcludeClipboardContentFromMonitorProcessing` / `Clipboard Viewer Ignore` 标记的),可关         | 默认,可改 |
| 配置存储        | exe 同目录的 `codetrans.ini`;目录不可写时退到 `%APPDATA%\CodeTrans\`                                            | 默认,可改 |

## 3. 总体架构

```
 ┌───────────── UI 线程(消息循环)─────────────┐
 │ WM_CLIPBOARDUPDATE ─┐                       │
 │ WM_DROPFILES ───────┼─► 内容采集(防抖、去重)─┼──► 生产者线程 ──────────► 帧环形缓冲 ──► 节拍线程 ──► 码区子窗口
 │ 截图热键 → 截图浮层 ─┘     (截图结果写剪切板)│     core: 打包/压缩/SHA-256      (预渲染好的     (DwmFlush 或
 │ 设置栏 / 托盘 / 热键 / INI                   │     core: RaptorQ 或 LT           整帧位图)       高精度定时器)
 └─────────────────────────────────────────────┘     core: 帧头 + CRC32 → qrcodegen
```

- **UI 线程**:窗口、剪切板监听、热键、托盘、设置栏。不做任何重计算。
- **生产者线程**:内容一变,先 `cp_tx_new()` 建发送会话,然后不停生成帧,提前填满约 2 秒的环形缓冲。每帧有 N 个码,预先渲染成一张位图。
- **节拍线程**:按目标帧率把下一帧 BitBlt 到码区。帧切换尽量对齐屏幕刷新(§6.6)。
- 截图不直接送进发送管线。截好的图写入剪切板,由剪切板监听接手。这样系统自带的 Win+Shift+S 和内置截图走同一条路。

## 4. 目录结构

```
CodeTrans/
├─ core/                          # 两端共用的纯 C 协议核心(C11,无平台依赖)
│  ├─ include/cp.h                # 对外 API:cp_payload / cp_tx(发送)/ cp_rx(接收)
│  ├─ src/
│  │  ├─ container.c              # 载荷容器:类型、元数据、deflate、SHA-256
│  │  ├─ frame_cp.c               # CodeTrans v1 帧头编解码 + CRC32
│  │  ├─ fec_rq.c                 # 对 nanorq 的薄封装(以后要换实现只改这里)
│  │  ├─ tx.c / rx.c              # 发送调度 / 接收会话
│  │  ├─ d3_frame.c d3_lt.c d3_container.c   # Decimen v3 兼容层(§5.4,有许可证前置条件)
│  │  └─ sha256.c crc32.c
│  ├─ third_party/  nanorq/  miniz/  qrcodegen/  quirc/  stb/
│  └─ tests/                      # 桌面回环测试(编码 → 渲染 → quirc 解码 → 还原),Decimen golden vectors
├─ windows/
│  ├─ PLAN.md
│  ├─ CMakeLists.txt
│  ├─ build.sh                    # 默认构建发布版 x86(all = 再加 x64)并跑测试
│  ├─ src/
│  │  ├─ main.c                   # wWinMain、单实例、初始化
│  │  ├─ app_window.c             # 主窗口、布局、尺寸计算、状态栏、键盘、拖拽、剪切板流程
│  │  ├─ settings_bar.c           # 可折叠设置栏
│  │  ├─ qr_view.c                # 码区子窗口(WM_PAINT 只 blit 当前帧)
│  │  ├─ producer.c               # 生产者线程 + 帧环形缓冲
│  │  ├─ pacer.c                  # 节拍线程
│  │  ├─ clipboard.c              # 监听、读取文本/图片、写入截图
│  │  ├─ content.c                # 待发内容(UI 线程只拷字节),在生产者线程打包
│  │  ├─ frame_render.c           # N 个码 → 每模块 1 字节的帧位图
│  │  ├─ qr_geom.c                # QR 容量表、码区几何
│  │  ├─ image_enc.c              # DIB → BGRA → PNG/JPEG(stb + miniz)
│  │  ├─ snip.c                   # 区域截图浮层
│  │  ├─ hotkey.c                 # 系统版本检测、注册热键
│  │  ├─ tray.c                   # 托盘图标与菜单
│  │  ├─ config.c                 # INI 读写、开机自启
│  │  └─ compat.c                 # Win7 之后的 API 动态加载、DPI
│  ├─ tests/  test_main.c  qrscan.c
│  └─ res/  app.rc  app.manifest  codetrans.ico  make_icon.py
└─ android/                       # 见 android/PLAN.md
```

## 5. 协议(两端共用,定稿后移到 `core/PROTOCOL.md` 作为唯一权威)

### 5.1 CodeTrans v1 帧(一个二维码 = 一帧,QR 字节模式,全部大端序)

| 偏移 | 长度 | 字段 | 说明 |
|---|---|---|---|
| 0 | 2 | magic | `0xC0 0xDE`,和 Decimen 的 `0xD1 0xC3` 区分开 |
| 2 | 1 | version | `1` |
| 3 | 1 | flags | 低 4 位"必须理解",出现未知位就丢帧;高 4 位可忽略。v1 全为 0 |
| 4 | 4 | session_id | 每开始发一份新内容就随机生成 |
| 8 | 8 | oti_common | RFC 6330 Common FEC OTI:F(40 bit)+ 保留(8)+ T(16) |
| 16 | 4 | oti_specific | RFC 6330 Scheme-Specific OTI:Z(8)+ N(16)+ Al(8) |
| 20 | 4 | payload_id | SBN(8)+ ESI(24) |
| 24 | T | symbol | 编码符号 |
| 24+T | 4 | crc32 | 覆盖前面所有字节 |

- 每帧开销 28 字节。T = ⌊(QR 容量 − 28) / Al⌋ × Al,QR 容量由版本和纠错等级决定。
- 每帧都带完整的 OTI。接收端从任意一帧开始都能建解码器,不需要单独的"头帧"。
- 为什么还要 CRC32:二维码自带 RS 校验,但传一个大文件要读几千个码。万一有一帧误纠错,整份 RaptorQ 解码结果就会被污染,最后 SHA-256 失败,只能重来。多花 4 字节(约 0.3%)把这种情况挡在门外。

### 5.2 载荷容器(FEC 之前的整体对象,大端序)

| 偏移 | 长度 | 字段 | 说明 |
|---|---|---|---|
| 0 | 4 | magic | `"CPO1"` |
| 4 | 1 | kind | 1 = 文本,2 = 图片,3 = 文件(一个或多个) |
| 5 | 1 | flags | bit0 表示 body 经过 raw deflate 压缩 |
| 6 | 2 | meta_len | 元数据长度 |
| 8 | 8 | raw_len | 解压后的 body 长度 |
| 16 | 32 | sha256 | 解压后 body 的 SHA-256 |
| 48 | meta_len | meta | UTF-8 文本,每行一个 `key=value`,如 `mime=image/png`、`name=a.txt`、`count=3` |
| … | … | body | 见下 |

- **文本**:body 是 UTF-8,不带 BOM。
- **图片**:body 是 PNG 或 JPEG 字节,`mime` 写明格式。
- **文件**:body 是若干条目依次排列,每条 = `u16 name_len | name(UTF-8,只存文件名不带路径) | u64 size | bytes`。
- **压缩**:先用 deflate level 6 试压,结果不超过原大小的 95% 才采用。已经压缩过的类型(png/jpg/zip/7z/mp4 等,按 MIME 和文件头魔数判断)直接跳过;大文件只用前 64 KB 采样判断。

### 5.3 发送调度(CodeTrans)

- **分块**:源块大小 K ≤ 4096 个符号,Z = ⌈总符号数 / 4096⌉。小块解码更快,而且收齐一块就能解一块,进度更平滑。
- **发送顺序**:ESI 单调递增,永不重复。每块先发 K 个源符号,再无限发修复符号,各块之间轮转(SBN 0..Z-1)。屏幕上每帧放 N 个码,依次取后面的 N 个包。中途才开始扫的手机也照常工作,任意 K+ε 个符号即可解出。
- **何时换会话**:
  - 内容变了,或者 T 变了(改版本或纠错等级),才新建 session_id。
  - 只改模块像素、码数、帧率时,沿用当前会话继续发,手机端已经收到的进度不会丢。
- **无限发送**:没有回传通道,所以一直播放,直到内容变化或用户暂停。24 位的 ESI 即使每秒 60 个码也能用 70 多个小时。

### 5.4 Decimen v3 兼容(已实现,2026-11-20)

> **实现说明(取代下面的原始调研)**:`core/src/d3.c` 按 Decimen Optical Transfer 0.5.3-fixed.1 的源码移植,两端共用。
>
> - **帧**:22 字节小端帧头,按参考实现分类(foreign / older / newer / unsupported-flags / malformed)。比参考实现多一条检查:两种外码都要求 k = ⌈totalLen / blockLen⌉,这样解码缓冲区的上限由 totalLen 决定。流的身份 = sessionId + k + blockLen + totalLen + payloadFnv + flags 的必须理解位。
> - **外码**:
>   - **Cauchy RS·96**(flags 0x02,发送端默认):GF(256),多项式 0x11d;每组最多 96 块,组间交织,每 8 轮轮换组和屏幕位置的对应关系;偶数轮发系统行,奇数轮发轮换的修复行 n..255。一组只要收到任意 n 个不同的行就能解出。
>   - **LT 轮播**(flags 0,原版 v0.5.3):每 2k 帧前 k 帧逐块发送;后 k 帧是修复帧,每帧异或 4–24 个不同的块,块的选取由 splitmix32(frameSeed(sessionId, seq)) 决定,接收端剥离解码。全程只用整数运算。
> - **容器**:DCF2 = 49 字节头(magic、gzip 标志、名字和类型长度、原文件长度、传输长度、SHA-256)+ 名字 + 类型 + 数据。gzip 只在省下 64 字节以上时才用;接收端按 fileLen 限长解压,并校验 CRC、ISIZE 和 SHA-256。
> - **发送端映射**:文本 → snippet(`application/vnd.decimen.snippet`);图片 → `image.<ext>`;单个文件原样发送;多个文件(或一个空文件)→ 不压缩的 zip(`CodeTrans-<n>files.zip`,UTF-8 文件名)。内容上限 64 MB,k ≤ 65535。
> - **固定几何**:blockLen = 所选版本在所选纠错等级下的满容量 − 22,每帧都把二维码填满。
> - **协议下拉框**:`Decimen v3 · RS96` 只能被认识 flag 0x02 的接收端解码,包括 CodeTrans Android 和 0.5.3-fixed 的 `decimen-receiver.html`;`Decimen v3 · LT` 任何 v3 接收端都能解码。INI 中分别存为 `DecimenV3` / `DecimenV3-LT`。
> - **验证**:`core/tests/data/` 里是参考实现生成的 golden vectors;`cp_d3_tool` 与参考实现(TypeScript)双向互解,覆盖 LT / RS、gzip、1–4 组、20–50% 丢帧、中途加入,x64 和 x86 各 21/21。
> - **许可证**:Decimen 是 AGPL-3.0-or-later,`d3.c` 是照它的源码移植的(即下面的方案 1)。**分发 CodeTrans(exe / apk)就必须按 AGPL-3.0 提供完整源码。** Android 的"开源许可"页已经列出这一项。如果不接受这一点,应改走方案 2(净室重写)或方案 4(去掉 `d3.c`)。

- **已从公开文档确认的部分**
  - 22 字节帧头,**小端序**:
    - 0:magic `0xD1`
    - 1:magic `0xC3`
    - 2:version `3`
    - 3:flags(`0x0F` 必须理解,`0xF0` 可忽略)
    - 4:u16 sessionId
    - 6:u32 seq
    - 10:u16 k
    - 12:u16 blockLen
    - 14:u32 totalLen
    - 18:u32 payloadFnv
  - 有 golden vector:`d1 c3 03 00 ef be 04 03 02 01 11 01 06 00 ba dc fe 00 ef cd ab 89 01 02 03 04 05 06`。
  - 文档还说明:LT 码采用鲁棒孤子度分布,块子集由 seq 确定性推出,有"系统帧 + 修复帧"的轮播,gzip 只在有收益时使用,整体做 SHA-256,接收端需要约 1.15·k 个不同的帧。
- **公开文档里没有的部分**(只在 AGPL 源码和测试里):
  - PRNG 算法
  - 度分布参数(c、δ)
  - 由 seq 选邻居块的方法
  - 它为保证跨引擎一致而自己实现的确定性 log
  - 容器字节布局
  - 轮播调度
- **许可证问题**:Decimen 从 v0.4.0 起改为 AGPL-3.0,而 wire v3 是 0.5.0 才出现的,所以 **v3 从来没有 MIT 版本的代码可以参考**。开始 W6 之前,需要你在以下方案中选一个:
  1. 整个 CodeTrans 用 AGPL-3.0 开源,可以直接参考它的源码;
  2. 净室实现:一个人读源码,写出参数规格书;另一个人只看规格书写代码;
  3. 联系作者,争取一份可用于非 AGPL 实现的规格或授权;
  4. 放弃兼容层。
- **兼容模式下的行为**:
  - 帧大小 = QR 容量 − 22。
  - 默认版本切到 40、纠错 L,与 decimen.app 的默认一致;仍然可以手动改小。
  - 多个文件先用 miniz 打成不压缩的 zip(`CodeTrans-<n>files.zip`)。
  - 文件上限 64 MB,与 Decimen 一致。

### 5.5 容量与理论吞吐

吞吐 ≈ (QR 容量 − 帧开销) × 码数 × fps。以下按 CodeTrans 计算,假设不丢帧:

| 版本 / 纠错 | 模块数 | QR 字节容量 | 每帧有效数据 | 示例配置 | 理论吞吐 |
|---|---|---|---|---|---|
| v15-L | 77 | 520 | ≈492 | 1 码 × 15 fps | ≈7 KB/s |
| v20-L | 97 | 858 | ≈828 | 2 码 × 15 fps | ≈24 KB/s |
| **v25-L(默认)** | 117 | 1273 | ≈1244 | **2 码 × 15 fps** | **≈37 KB/s** |
| v30-L | 137 | 1732 | ≈1700 | 4 码 × 20 fps | ≈136 KB/s |
| v40-L(Decimen 默认) | 177 | 2953 | 2931(兼容模式) | 4 码 × 30 fps | ≈350 KB/s(实际受手机解码能力限制) |

状态栏显示的"预计时间"按 (ΣK + 2Z) / (码数 × fps) 计算,即不丢帧时的下限。

## 6. 模块设计

### 6.1 主窗口(`app_window.c` / `settings_bar.c` / `qr_view.c`)

```
收起(默认)                                     展开(点 ⚙ 或 F2)
┌─ CodeTrans ─────────────────────────┐         ┌─ CodeTrans ───────────────────────────────────┐
│  ██████████████   ██████████████   │         │  ██████████████   ██████████████             │
│  ██   QR #1  ██   ██   QR #2  ██   │         │  ██   QR #1  ██   ██   QR #2  ██             │
│  ██████████████   ██████████████   │         │  ██████████████   ██████████████             │
│ ⚙ 文本 12.3KB→4.1KB · 2码×15fps · ≈0.3s │     │ ⚙ 图片 PNG 286KB · K=232 · 2码×15fps · ≈8s   │
└────────────────────────────────────┘         ├──────────────────────────────────────────────┤
                                               │ 协议[CodeTrans▾] 版本[25▾]1273B 纠错[L▾] 模块[4▴▾]px │
                                               │ 码数[2▾] FPS[15▴▾] 图片[PNG▾] ☑置顶 ☑自动弹出 ☐暂停监听 │
                                               └──────────────────────────────────────────────┘
```

- **尺寸**:窗口大小由设置推出来,不能拖动缩放。
  - 码区宽 = 列数 × (模块数 + 2×静区) × 模块像素 + 间距;码区高同理。
  - 设置栏展开时,窗口最小宽度等于设置栏宽度,码区居中。
  - 算出来超过显示器工作区时,把窗口夹到工作区内,并在状态栏提示减小模块像素或码数。
- **码数与布局**:1(1×1)、2(2×1)、3(3×1)、4(2×2)、6(3×2)。
- **空闲时**:码区留白,状态栏显示"等待剪切板… · Win+Shift+S 截图 · 拖入文件"。
- **快捷键**(窗口有焦点时):
  - F2:展开或收起设置栏
  - Space:暂停或继续播放
  - Esc:收回托盘
  - Ctrl+V:重新读取剪切板并发送
  - `+` / `-`:模块像素 ±1
- **拖拽**:用 `DragAcceptFiles` + `WM_DROPFILES`,不走 OLE。
  - 拖进文件夹时跳过,状态栏提示"暂不支持文件夹"。
  - 程序不要以管理员身份运行(manifest 设为 `asInvoker`),否则 UIPI 会拦截从资源管理器的拖拽。
- **DPI**:manifest 声明 `PerMonitorV2`,并加 `dpiAware true/pm` 回退,否则系统会拉伸窗口,码会变糊。
  - 模块像素按**物理像素**计,不随 DPI 缩放。
  - 设置栏字体跟随 DPI;Win8.1 及以上处理 `WM_DPICHANGED`。
  - 字体取 `NONCLIENTMETRICS.lfMessageFont`;中文由系统字体链接自动处理。
- **控件**:ComboBox、UpDown + Edit、CheckBox,通过 manifest 启用 comctl32 v6 视觉样式。改动立即生效,并写回 INI。

### 6.2 设置项

| 设置 | 位置 | 范围 | 默认 | 改动后 |
|---|---|---|---|---|
| 协议 | 设置栏 | CodeTrans / Decimen v3 · RS96 / Decimen v3 · LT | Decimen v3 · RS96 | 新会话 |
| 版本(码的大小) | 设置栏 | 5–40,旁边显示每码字节数 | 40(切到 CodeTrans 时为 25) | 新会话 |
| 纠错等级 | 设置栏 | L / M / Q / H | L(喷泉码已经能扛丢帧) | 新会话 |
| 模块像素 | 设置栏 | 1–16 物理像素 | 4 | 沿用会话 |
| 码数 | 设置栏 | 1 / 2 / 3 / 4 / 6 | 2 | 沿用会话 |
| 刷新率 | 设置栏 | 1–60 fps | 30(建议不超过手机相机帧率的一半,手机端相机帧率默认"自动",支持时为 60) | 沿用会话 |
| 图片格式 | 设置栏 | PNG / JPEG(质量 50–95,INI 中设置) | PNG | 新会话 |
| 置顶 | 设置栏 | 开 / 关 | 开 | — |
| 剪切板更新时自动弹出 | 设置栏 | 开 / 关 | 开 | — |
| 暂停监听 | 设置栏、托盘 | 开 / 关 | 关 | — |
| 开机自启 | 托盘 | 开 / 关 | 关 | 写 HKCU `...\Run` |
| 截图热键 | INI | 任意组合 | Win+Shift+S | — |
| 强制使用内置截图 | 托盘 | 开 / 关 | 关 | 即使 Win10 1703+ 也接管热键 |
| 跳过密码管理器标记的内容 | INI | 开 / 关 | 开 | — |
| 静区宽度 | INI | 1–4 模块 | 4 | 沿用会话 |
| 大内容确认阈值 | INI | MB | 20 | 超过阈值时状态栏提示"预计 X 分钟,按 Enter 开始" |

### 6.3 剪切板监听(`clipboard.c`)

- 用 `AddClipboardFormatListener` 接收 `WM_CLIPBOARDUPDATE`(Vista 及以上,Win7 可直接用)。
- **防抖**:收到更新后等 80 ms 再读。很多程序会分几次写入不同格式,或者使用延迟渲染。
- **重试**:`OpenClipboard` 失败(被别的程序占着)时每隔 20 ms 重试,最多 10 次。
- **快速去重**:用 `GetClipboardSequenceNumber`;读到内容后再算 SHA-256,内容和当前一样时不重启会话。用户重复复制同一段话不会打断手机端的进度。
- **读取优先级**:
  1. 带密码管理器标记(且该选项开启)→ 跳过。
  2. `CF_UNICODETEXT` → 转 UTF-8(`WideCharToMultiByte(CP_UTF8)`),去掉末尾 NUL;空串忽略。
  3. 注册格式 `"PNG"`,且图片格式选的是 PNG → 原样使用这些字节(无损,保留透明)。
  4. `CF_DIBV5` / `CF_DIB` → 交给 `image_enc.c`。
  5. 只有 `CF_HDROP` 或其他格式 → 忽略。
- 内容确认变化后通知生产者线程;如果开启了自动弹出,执行 §6.8 的弹出逻辑。

### 6.4 内容 → 载荷(`image_enc.c` 等)

- **图片**:先用 GDI 把任意位深的 DIB 统一转成 32bpp 自上而下的 BGRA(`CreateDIBSection` + `SetDIBitsToDevice`)。
  - 转 PNG:用 stb_image_write,通过 `STBIW_ZLIB_COMPRESS` 接 miniz 的 level 9 压缩,体积接近系统编码器。
  - 转 JPEG:用 stb 的 baseline JPEG。
  - 只有 `CF_DIBV5` 带 alpha 掩码时才保留透明度,截图都按不透明处理。
- **文件**:读入内存,条目名只保留文件名。单个文件超过阈值时先确认;总大小硬上限 256 MB(Decimen 模式 64 MB)。
- 打包、压缩、SHA-256 都在生产者线程里调用 `core` 完成,UI 线程只把原始字节交过去。

### 6.5 生产者线程(`producer.c`)

```c
cp_payload *p  = cp_payload_text(utf8, n);       /* 或 cp_payload_image / cp_payload_files */
cp_tx      *tx = cp_tx_new(CP_PROTO_RQ /* 或 CP_PROTO_D3 */, p, qr_capacity_bytes, &opts);
for (;;) {                                         /* 填满环形缓冲为止 */
    n = cp_tx_next(tx, buf, sizeof buf);           /* 一个码的二进制内容 */
    qrcodegen_encodeBinary(buf, n, qr, ecl, ver, ver, qrcodegen_Mask_AUTO, false);
    /* 按模块像素把 N 个码渲染进当前帧的 8bpp DIB section */
}
```

- 帧环形缓冲约 2 秒,生产者领先节拍线程。缓冲满了就等,内容或设置变化时清空重来。
- 掩码默认 AUTO,扫描更稳,而且生成是在后台线程,成本可以接受。如果帧率太高 CPU 跟不上,可以像 Decimen 那样固定掩码,速度约快 4 倍。
- 一帧位图 = 码区大小的 8bpp DIB section(两色调色板)。逐模块整数倍填充,保证边缘锐利,没有插值。

### 6.6 渲染与节拍(`pacer.c`)

- 目标:帧切换和屏幕刷新对齐,每帧停留时间一致,不出现半帧撕裂。
- **DWM 开启时**(Win8 起总是开启;Win7 用 Aero 主题时也开启):循环调用 `DwmFlush()`,每次调用等到下一次合成。
  - 刷新率 R 用 `DwmGetCompositionTimingInfo` 或 `EnumDisplaySettings` 获取。
  - 每 round(R / fps) 次 vblank 切一帧,所以实际 fps 会取到 R 的约数(60 Hz 下为 60/30/20/15/12/10)。状态栏显示实际 fps。
- **DWM 关闭时**(Win7 Basic 主题):
  - 用可等待定时器;Win10 1803 及以上加 `CREATE_WAITABLE_TIMER_HIGH_RESOLUTION`。
  - 没有这个标志时,用 `timeBeginPeriod(1)` 配合普通定时器。
  - 这种模式可能出现撕裂。被撕裂的码会被二维码本身的校验和帧 CRC32 丢掉,只损失一点效率。
- **绘制**:节拍线程 `GetDC(码区)` → `BitBlt` 当前帧 → `ReleaseDC`。`WM_PAINT` 只在临界区里 blit 当前帧,保证重绘正确。
- **播放期间**:
  - 调用 `SetThreadExecutionState(ES_CONTINUOUS | ES_DISPLAY_REQUIRED)`,防止屏幕熄灭或进入屏保。
  - Win10 1709 及以上动态调用 `SetProcessInformation(ProcessPowerThrottling)`,退出 EcoQoS 节流,避免后台降频导致掉帧。

### 6.7 区域截图(`snip.c` / `hotkey.c`)

- **何时启用**:用 `RtlGetVersion` 取真实 build 号。
  - build < 15063(Win10 1703 首次加入 Win+Shift+S)或开启"强制内置截图"时:注册热键 `RegisterHotKey(MOD_WIN | MOD_SHIFT | MOD_NOREPEAT, 'S')`。
  - 1703 及以上默认不接管热键,系统截图结果进入剪切板后自然被发送。
- **热键冲突**:注册失败时在状态栏提示,并改用 INI 中配置的备用热键。已知 OneNote 2010/2013 可能占用 Win+Shift+S。
- **流程**:
  1. 先隐藏 CodeTrans 窗口,等合成刷新一次(`DwmFlush`,或等 50 ms)。
  2. 截取整个虚拟屏幕:`SM_XVIRTUALSCREEN` 等四个值确定范围,`BitBlt` 带 `CAPTUREBLT`,结果作为冻结画面。
  3. 打开覆盖整个虚拟屏幕的置顶 `WS_POPUP` 浮层:冻结画面整体压暗,选区显示原亮度,并标注选区尺寸,光标为十字。
  4. 左键拖动选择矩形;Esc 或右键取消。
  5. 松开鼠标后裁剪选区,同时写入剪切板 `CF_DIB` 和 `"PNG"` 两种格式。粘贴到别的程序也可用;剪切板监听随后接手发送,最后恢复窗口。
- 只做矩形选区,不做自由形状或窗口截图。多显示器和混合 DPI 依赖 per-monitor DPI 感知,保证拿到物理像素。

### 6.8 托盘、弹出、单实例、配置

- **托盘**:用 `Shell_NotifyIconW`。
  - 左键单击显示或隐藏窗口。
  - 右键菜单:显示/隐藏、截图、暂停监听、强制内置截图、开机自启、退出。
  - 监听 `TaskbarCreated` 消息,资源管理器重启后重新添加图标。
- **自动弹出**:剪切板内容变化,窗口处于隐藏或最小化时:
  1. `ShowWindow(SW_SHOWNOACTIVATE)`
  2. `SetWindowPos(HWND_TOPMOST, …, SWP_NOACTIVATE | SWP_SHOWWINDOW)`

  这样窗口出现在最上层,但不抢焦点,不会打断正在打字的程序。按 Esc 收回托盘。
- **单实例**:命名互斥量 `Local\CodeTrans.SingleInstance`。第二个进程启动时给已有窗口发"显示"消息,然后自己退出。
- **配置**:用 `GetPrivateProfile*W` / `WritePrivateProfileStringW` 读写 INI。记住窗口位置;启动时检查位置是否还在某个显示器内(`MonitorFromRect`),不在就移回主屏。

## 7. 构建与工具链

- **工具链**:MinGW-w64 GCC 或 llvm-mingw,选 **msvcrt** 运行时版本。UCRT 版在 Win7 上需要先装 KB2999226,没装的机器会无法启动。
  - 不选 MSVC 作为主工具链的原因:nanorq 只提供 Makefile,并依赖 GCC/Clang 特性(`-march=native`)。用 MinGW 改动最小。
  - 能用 clang-cl 编通的话,可以再加一个 MSVC 构建作为备选。
- **构建**:CMake + Ninja。发布只有一个 `CodeTrans.exe`(x86 构建,32/64 位 Windows 通用);`CodeTrans-x64.exe` 只用于开发。构建后检查导入表,只允许系统自带 DLL。
  - 编译参数:`-municode -mwindows -O2 -static -static-libgcc`,宏 `_WIN32_WINNT=0x0601`、`UNICODE`、`_UNICODE`。
  - 线程用 Win32 API,不用 pthread。
- **CPU 基线**:x64 用默认 SSE2;x86 加 `-msse2`。**不要**用 `-march=native`,否则换一台机器就可能崩溃。发送端每秒最多生成几百个修复符号,不需要 AVX。
- **浮点**:全工程禁止 `-ffast-math`。Decimen 的度分布依赖逐位一致的 IEEE-754 运算,一旦开启,两端算出的 LT 邻居就可能对不上。x86 要加 `-mfpmath=sse`,避免 x87 的 80 位中间精度带来差异。
- **链接库**:user32、gdi32、shell32、comctl32、dwmapi、winmm、advapi32。这些 Win7 都自带,不需要任何运行库。
- **manifest**:
  - `supportedOS` 声明 Win7 到 Win11
  - `dpiAwareness` PerMonitorV2 + `dpiAware` true/pm
  - comctl32 v6
  - `requestedExecutionLevel asInvoker`
- **体积目标**:单个 exe 小于 1 MB,不需要安装。

## 8. 里程碑

| 阶段 | 内容 | 验收 |
|---|---|---|
| W0 骨架 | CMake + MinGW、manifest、空窗口、托盘、INI、单实例 | Win7 x86 虚拟机和 Win11 上都能启动;高 DPI 下不模糊 |
| W1 core 编码链路 | 载荷容器、deflate、SHA-256、nanorq 封装、CodeTrans 帧、qrcodegen;`core/tests` 回环测试 | 随机内容(1 B – 50 MB)在 0–50% 随机丢帧下都能原样还原;接收约 K+2 个符号的解码成功率 ≥ 99% |
| W2 显示 | 码区多码排布、整数缩放、节拍线程、可折叠设置栏、状态栏、窗口尺寸计算 | 60 Hz 屏幕上 15/30 fps 稳定不抖;用高速摄像或手机慢动作检查无半帧 |
| W3 剪切板 | 监听、防抖、文本/图片读取、去重、密码标记、自动弹出置顶、暂停 | Office、浏览器、截图工具、记事本复制的内容都能正确识别类型;重复复制不重启会话 |
| W4 拖拽文件 | 单文件和多文件容器、大内容确认 | 多文件在手机上按原名还原 |
| W5 区域截图 | 版本检测、热键、截图浮层、写剪切板 | Win7 / Win8.1 多显示器下截图正确;Win11 上不和系统截图冲突 |
| W6 Decimen 兼容 | **先定许可证方案(§5.4)**;然后实现 v3 帧头、LT 编码、容器、轮播 | 通过 golden vector;手机浏览器打开 decimen.app 能接收文本和文件 |
| W7 打磨 | 开机自启、错误提示、x86/x64 发布构建、Win7 SP1 裸机测试 | Win7 SP1(无额外补丁)到 Win11 都能运行 |

W1 的回环测试要先于 Android 开发完成。只要 `core` 在桌面上能做到"编码 → 渲染成图 → quirc 解码 → 还原",Android 端剩下的就是相机和界面。

## 9. 测试计划

- **core 单元测试**(桌面端,每次提交都跑):
  - 容器编解码、CRC32 / SHA-256 测试向量
  - RaptorQ 在各种 K / T / 丢帧率下的回环
  - 帧头字段边界值
  - Decimen golden vector
- **渲染回环**:把生成的帧位图存成 PNG,用 quirc 解码,检查每个版本、纠错等级和模块像素组合都能读回。
- **真机矩阵**:Windows 发送 × 2–3 台 Android 机型接收,覆盖版本 15/20/25/30/40、模块像素 2–6、码数 1/2/4、fps 10/15/20/30。记录实际吞吐和首次出结果时间,用来校准默认值。
- **系统矩阵**:Win7 SP1 x86(Basic 主题和 Aero 主题各一次)、Win8.1、Win10 1607(没有 Win+Shift+S)、Win10 22H2、Win11。

## 10. 风险与待定事项

| 风险 | 影响 | 应对 |
|---|---|---|
| Decimen v3 的参数只在 AGPL 源码里有(§5.4) | W6 无法开工 | 到 W6 前你从 4 个方案中选一个;CodeTrans 主协议不受影响 |
| nanorq 在 Windows 上编不过(GCC 特有写法、`posix_memalign`、`-march=native`) | W1 延期 | 在 `third_party` 里打小补丁(`_aligned_malloc` 等);实在不行就按 RFC 6330 自己实现。`fec_rq.c` 已经把它隔离开了 |
| Win7 关闭 DWM 时节拍不准或撕裂 | 吞吐下降 | 高精度定时器兜底;撕裂的帧会被校验丢弃 |
| 热键被其他程序占用 | 截图热键无效 | 状态栏提示 + 备用热键 + 托盘菜单入口 |
| 剪切板被占用或延迟渲染 | 偶尔漏读 | 防抖 + 重试;Ctrl+V 手动重读 |
| DPI 虚拟化让码变糊 | 识别率骤降 | manifest 声明 PerMonitorV2,W0 就验收 |
| 后台节流导致掉帧(EcoQoS / 定时器合并) | fps 不稳 | 关闭节流 + 播放期间保持屏幕常亮 |

## 11. 实现中与原计划的差异

- **帧位图按模块存**:环形缓冲里每个模块占 1 字节,节拍线程用 `StretchDIBits`(COLORONCOLOR)按模块像素做整数倍放大。好处是改模块像素不用重新渲染,2 秒缓冲在 v40×6 码时也只有约 25 MB。
- **图片编码移到生产者线程**:UI 线程只把 `PNG` / `CF_DIBV5` / `CF_DIB` 的原始字节拷出来,PNG/JPEG 编码、读文件、deflate、SHA-256 都在生产者线程完成。剪切板去重用 64 位快速哈希,不用 SHA-256,避免 4K 截图卡住 UI。
- **剪切板优先级**:`"PNG"` 格式总是优先于 DIB(无损、保留透明);图片格式选 JPEG 时,由生产者解码后再编码成 JPEG。还额外识别 `CanIncludeInClipboardHistory = 0` 作为密码管理器标记。
- **DWM 节拍**:每次 `DwmFlush` 返回后按 QPC 时间判断该不该切帧;如果 `DwmFlush` 不阻塞,就 `Sleep(1)`,避免空转。
- **置顶时最小化**:直接收回托盘,因为置顶窗口最小化之后没有用。

## 12. 实现状态(2026-11-20)

| 阶段 | 状态 | 已验证 | 还需要 |
|---|---|---|---|
| W0 骨架 | 完成 | x64/x86 均编译通过,零警告;exe 约 420 KB;只依赖 Win7 自带 DLL + msvcrt,静态导入表中没有 Win8 以后的 API | Win7 x86 虚拟机上启动;高 DPI 显示器目测 |
| W1 core | 完成 | `cp_core_tests`:1 B–50 MB、Z=1/2/5/6、0–50% 丢帧全部还原;K+2 解码成功率 300/300;8 种版本/纠错组合的渲染回环 | — |
| W2 显示 | 完成 | 多码排布、窗口尺寸计算、设置栏、状态栏;Win11 上截图解码 | 高速摄像检查无半帧 |
| W3 剪切板 | 完成 | 文本 126 KB → 屏幕截图 → `cpwin_qrscan` 逐字节还原;剪切板位图 → PNG 端到端还原 | Office、浏览器等各来源逐个核对 |
| W4 拖拽文件 | 完成 | 多文件容器(单元测试) | 真机按原名还原 |
| W5 区域截图 | 完成 | 强制内置模式下:热键 → 拖选 → 剪切板 → 自动发送 | Win7 / Win8.1 多显示器 |
| W6 Decimen | 完成 | RS96 / LT 两种外码;`cp_core_tests` 中有 golden vectors、Cauchy 修复向量、k-of-2k 全子集、20–50% 丢帧回环、V27/V32/V40 渲染回环;`cpwin_tests` 跑 frame_render → quirc 回环;与参考实现双向互解 21/21 | 真机用手机浏览器打开 `decimen-receiver.html` 扫码;许可证确认(§5.4) |
| W7 打磨 | 部分 | 开机自启、错误提示、x86/x64 构建 | Win7 SP1 裸机、真机矩阵(§9) |

## 13. 参考

- RaptorQ:RFC 6330 <https://www.rfc-editor.org/rfc/rfc6330>;nanorq(MIT)<https://github.com/sleepybishop/nanorq>
- Decimen Optical Transfer <https://github.com/bashalarmistalt/decimen-optical-transfer>(协议文档:`docs/technical/protocol.md`、`versioning.md`、`golden-vectors.md`);decimen-cli <https://github.com/jackbauertv24-droid/decimen-cli>
- qrcodegen(MIT)<https://github.com/nayuki/QR-Code-generator>;quirc(ISC)<https://github.com/dlbeer/quirc>
- miniz(MIT)<https://github.com/richgel999/miniz>;stb_image_write(公有领域 / MIT)<https://github.com/nothings/stb>
