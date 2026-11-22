# CodeTrans · Android 接收端计划

> 状态(2026-11-20):A0–A4 代码已完成,debug / release 均可构建;仪器测试 `NativeCoreTest`(录制帧 → JNI → 结果逐字节一致)在 API 36 模拟器上 6/6 通过。A5 Decimen 已完成:v3 的 LT 和 Cauchy RS·96(0.5.3-fixed)都能接收,扫描器加入了对比度重试;仪器测试增至 10 项,全部通过。尚缺:真机扫 Windows 发送端的端到端验证、A6 性能校准。release 已签名(密钥在 `android/signing/`,不进版本库,配置见 `app/build.gradle.kts`)。
> 解码核心是纯 C,和 Windows 发送端共用仓库根目录的 `core/`(协议、FEC、容器),Android 端再加上 quirc 识别二维码和 JNI 胶水。
> Kotlin 只负责相机、界面和分享。

---

## 1. 目标与范围

**要做的**

- 用摄像头连续扫描 Windows 端播放的二维码流,一帧画面里可以同时有多个码。实时显示进度,收齐后还原出文本、图片或文件。
- 只有一个主界面:相机取景 + 结果预览 + 快速分享按钮(调起系统分享面板,发到其他 App)。右上角设置按钮进入设置页。
- 同时识别两种协议:
  - **CodeTrans**(RaptorQ,默认)
  - **Decimen v3**:可以接收 decimen.app 或 decimen-cli 发出的码
- 顺带识别**普通二维码**,直接把内容当文本显示。

**不做的(保持极简)**

- 不发送,不记历史:只保留最近一次的结果,来了新结果就替换。
- 不联网:**不申请 INTERNET 权限**。
- 不做登录、广告、统计。

## 2. 决定清单

| 项                  | 决定                                                                                                                 | 来源                     |
| ------------------ | ------------------------------------------------------------------------------------------------------------------ | ---------------------- |
| 解码核心               | 纯 C:quirc(识别二维码,一帧多码)+ nanorq(RaptorQ)+ miniz + 自带 SHA-256                                                         | 你已确认                   |
| 其余部分               | Kotlin                                                                                                             | 你已确认                   |
| UI                 | Material Design 2:Material Components for Android 的 `Theme.MaterialComponents.*`,不用 M3 主题;XML View,不用 Compose,体积更小 | 你已确认 MD2;用 View 是我定的默认 |
| 协议                 | CodeTrans(RaptorQ)+ Decimen v3 兼容,按 magic 自动区分                                                                      | 你已确认                   |
| 界面                 | 单个主界面(相机 + 预览 + 分享),右上角进入设置                                                                                        | 你已确认                   |
| minSdk / targetSdk | minSdk 24(Android 7.0)/ targetSdk 36 / compileSdk 37。原定 23,但 CameraX 1.7 全部构件要求 API 24;要保 Android 6 只能退回旧版 CameraX | 默认,可改                  |
| 收到文本               | 自动复制到剪切板(可关)                                                                                                       | 默认,可改                  |
| 收到文件或图片            | 存在应用缓存里,通过分享发出;"自动保存到 下载/CodeTrans"默认关(Android 10+ 才提供)                                                             | 默认,可改                  |
| 普通二维码              | 识别后当文本显示(可关)                                                                                                       | 默认,可改                  |
| 包名                 | `com.codetrans`(占位,待定)                                                                                              | 待你定                    |

## 3. 总体架构

```
CameraX ImageAnalysis (YUV_420_888, KEEP_ONLY_LATEST)
   │ 分析线程:只取 Y 平面 → JNI memcpy 到空闲的原生帧缓冲 → 立刻 image.close()
   │          没有空闲缓冲 → 直接丢掉这一帧(背压)
   ▼
解码线程池(N 个线程,每个线程一个 quirc 实例)
   │ JNI scan():quirc 检测并解码帧里的所有码 → 按 magic 分发
   │   0xC0 0xDE → CodeTrans 帧:CRC32 → RaptorQ 会话
   │   0xD1 0xC3 → Decimen v3 帧 → LT 会话
   │   其他       → 普通二维码文本
   ▼
cp_rx(C,内部加锁):会话管理 → 收齐 → 解码 → SHA-256 校验 → 解容器 → 写入 cacheDir
   │ 返回事件位(PROGRESS / COMPLETE / PLAIN / FAILED)
   ▼
ScanViewModel(StateFlow,进度节流到约 10 Hz)──► MainActivity:进度条、预览卡片、分享按钮
```

- 图像数据不经过 Java 堆:Y 平面是 direct ByteBuffer,在 C 里按 rowStride 逐行拷贝进原生缓冲。
- 不需要旋转图像:二维码检测本身与方向无关。
- 大文件由 C 直接写到磁盘,Kotlin 只拿到路径和元数据,不会把几十 MB 搬进 Java 堆。

## 4. 目录结构

```
CodeTrans/
├─ core/                                 # 共用 C 核心(结构见 windows/PLAN.md §4)
│  ├─ include/cp.h   src/rx.c  src/frame_cp.c  src/fec_rq.c  src/container.c  src/d3_*.c
│  ├─ third_party/  nanorq/  quirc/  miniz/
│  └─ tests/                             # 桌面回环测试、golden vectors、模糊测试
└─ android/
   ├─ PLAN.md
   ├─ settings.gradle.kts  build.gradle.kts  gradle/libs.versions.toml
   └─ app/
      ├─ build.gradle.kts
      └─ src/main/
         ├─ AndroidManifest.xml
         ├─ cpp/
         │  ├─ CMakeLists.txt             # add_subdirectory(${CP_CORE}),CP_CORE = ../../../../../core
         │  ├─ jni_bridge.c               # JNI 入口、类和方法缓存
         │  ├─ jni_tx.c                   # 发送端(手机 → 电脑):cp_tx + qrcodegen 模块矩阵
         │  └─ (core/scan/scanner.c)      # quirc 封装:Y 平面 → 多码 payload,与 Windows 摄像头接收共用
         ├─ kotlin/com/codetrans/
         │  ├─ MainActivity.kt            # 主界面
         │  ├─ ScanViewModel.kt           # 状态:Idle / Receiving / Done / Error
         │  ├─ CameraController.kt        # CameraX 绑定、点按对焦、双指缩放、曝光、帧率
         │  ├─ DecodePipeline.kt          # 帧缓冲池、解码线程池、事件节流
         │  ├─ NativeCore.kt              # external fun 声明
         │  ├─ ResultStore.kt             # 当前结果、缓存清理、自动保存
         │  ├─ ShareHelper.kt             # 分享 Intent、FileProvider
         │  ├─ SettingsActivity.kt        # PreferenceFragmentCompat
         │  └─ Prefs.kt
         └─ res/  layout/ layout-land/ xml/(preferences、file_paths)  values/(MD2 主题、中英文字符串)
```

## 5. 协议(接收端视角)

> 帧格式和容器格式的完整定义见 `windows/PLAN.md` §5,定稿后统一放进 `core/PROTOCOL.md`。这里只列接收端需要处理的部分。

### 5.1 CodeTrans v1 帧(大端序,每帧开销 28 字节)

| 偏移 | 长度 | 字段 |
|---|---|---|
| 0 | 2 | magic `0xC0 0xDE` |
| 2 | 1 | version = 1 |
| 3 | 1 | flags(低 4 位必须理解,高 4 位可忽略) |
| 4 | 4 | session_id |
| 8 | 8 | RFC 6330 Common FEC OTI(F、T) |
| 16 | 4 | RFC 6330 Scheme-Specific OTI(Z、N、Al) |
| 20 | 4 | payload_id(SBN 8 位 + ESI 24 位) |
| 24 | T | symbol |
| 24+T | 4 | crc32(覆盖前面所有字节) |

**收到一帧时按顺序检查**:

1. 长度不少于 28 字节
2. magic 正确
3. version 支持
4. 没有未知的"必须理解"位
5. CRC32 正确
6. OTI 在上限内:F ≤ 256 MB,T ≤ 4096,Z ≤ 255

任何一项不通过就丢弃这一帧。所有字段都当作**不可信输入**,先检查边界再分配内存。

### 5.2 Decimen v3 帧(小端序,22 字节帧头)

- **识别**:`0xD1 0xC3` 开头,version 为 3,flags 中没有未知的必须理解位(`0x0F`)。
- **会话标识**:sessionId + k + blockLen + totalLen + payloadFnv;flags 中的可忽略位不参与区分。
- **解码**:LT 剥离解码,接收端大约需要 1.15·k 个不同的帧。可选优化:剥离卡住时用高斯消元兜底,能降低所需帧数。
- **已实现(2026-11-20)**:`core/src/d3.c`,算法照 Decimen 0.5.3-fixed 的 AGPL 源码移植(许可证后果见 `windows/PLAN.md` §5.4)。
  - **两种外码**:flags 0x02 走 Cauchy RS·96,flags 0 走 LT 轮播,接收端按 flags 自动选择。
  - **会话**:同时最多 2 个活跃会话,与 CodeTrans 会话共用这个名额。每个会话一把锁,只保护它的解码器。
  - **进度**:RS 为有用行数 / k;LT 沿用 Decimen 的估算曲线,把有效帧数和已解出块数结合起来。
  - **结果**:snippet → 文本;`image/*` → 图片;其他 → 单个文件(保留原文件名)。结果目录为 `d3xxxxxx`,不会和 CodeTrans 会话冲突。
  - **提示**:v1/v2/v4+、未知的必须理解位(如加密)、超过上限时返回 `EV_UNSUPPORTED`,界面提示"版本或功能不兼容";格式错误的帧静默丢弃。
- **扫描器对比度重试**:移植自 0.5.3-fixed 的 `contrast-retry.ts`。
  - **做法**:一帧什么都没解出时,用原分辨率做一次 Otsu 阈值 + 高增益锐化(12·Y − 距离 2 的四邻域),再交给 quirc 解一次。
  - **限制**:每个扫描线程每 100 ms 最多重试一次,只处理 ≤ 2304×1296 的帧;而且只在画面里已检测到码,或 2 s 内成功解过码时才重试,镜头空对着别处时不额外耗电。
  - **效果**:测试帧 `d3hard.png`(σ = 1.4 px 高斯模糊,3 px/模块)单靠 quirc 读不出,重试后能读出。

### 5.3 会话管理(`core/src/rx.c`)

- **活跃会话**:最多同时保留 2 个(当前的和最新出现的)。出现第 3 个时,淘汰最久没见到帧的那个。界面只显示最近有帧到达的会话的进度。
- **去重**:每个源块用一个 ESI 位图记录已收到的符号(按需扩容)。手机相机帧率高于屏幕换码频率,同一个码会被扫到好几次。
- **何时解码**:某个块收到的符号数 ≥ K_i 时尝试 RaptorQ 解码;失败就每再多收 1 个符号重试一次。所有块都解出后拼接,做 SHA-256 校验,然后解容器。
- **已完成会话**:session_id 放进一个 32 项的 LRU。之后再扫到同一个会话的帧直接忽略,界面不会重新开始。
- **校验失败**:丢弃这个会话,发出 `FAILED` 事件(界面弹 Snackbar),然后从头重新收集;发送端一直在循环播放。
- **普通二维码**:quirc 解出的内容不以两种 magic 开头时,原始字节交给 Kotlin。先按 UTF-8 解码,失败再按 GB18030。同一内容连续出现只触发一次。

### 5.4 进度显示

- **CodeTrans**:进度 = Σ min(已收符号_i, K_i) / ΣK_i。RaptorQ 收到约 K+2 个符号就能解出,所以进度基本是线性的。
- **Decimen**:LT 剥离解码在最后阶段才集中完成,块进度会长时间不动,然后突然跳满(Decimen 文档也提到这一点)。所以显示 max(已解块数 / k, 不同帧数 / 1.15k),作为估计值。
- **状态文字**:协议 · 类型 · 大小 · 已收 / 所需 · 最近 2 秒的 KB/s · 当前帧里识别到的码数(帮助对准)。

## 6. C 核心与 JNI

### 6.1 Android 专用 C 代码

- `scanner.c`:每个解码线程一个 `quirc*`,尺寸变化时调用 `quirc_resize`。
  - 流程:`quirc_begin` → 拷入 Y 平面 → `quirc_end` → 对每个检测到的码 `quirc_extract` + `quirc_decode` → 把 payload 交给 `cp_rx_push()`。
  - 后置摄像头的画面不是镜像,不需要 `quirc_flip`。
- 1080p 及以上分辨率需要定义 `QUIRC_MAX_REGIONS=65534`(quirc README 的要求)。

### 6.2 JNI 接口(`NativeCore.kt`)

```kotlin
object NativeCore {
    external fun rxNew(outDir: String, maxBytes: Long, enableD3: Boolean, enablePlain: Boolean): Long
    external fun rxFree(rx: Long)
    external fun rxReset(rx: Long)
    external fun scannerNew(): Long                      // 每个解码线程一个(内部是 quirc 实例)
    external fun scannerFree(s: Long)
    external fun copyPlane(src: ByteBuffer, rowStride: Int, w: Int, h: Int, dst: ByteBuffer)  // C 里 memcpy
    external fun scan(s: Long, rx: Long, y: ByteBuffer, w: Int, h: Int): Int                 // 返回事件位
    external fun progress(rx: Long, out: LongArray)      // proto, kind, have, need, bytes, codesInFrame
    external fun takePlain(rx: Long): ByteArray?
    external fun takeResult(rx: Long): NativeResult?     // C 已经把文件写进 outDir
}
@Keep class NativeResult(val kind: Int, val mime: String, val text: String?, val paths: Array<String>, val names: Array<String>, val sizes: LongArray)
```

- `JNI_OnLoad` 里缓存 `NativeResult` 的类引用和构造方法。R8 保留规则用 `@Keep` 加 proguard-rules 中的 `-keepclasseswithmembernames class * { native <methods>; }`。
- `rx` 是线程安全的,内部一把互斥锁。RaptorQ 解码可能耗时几十 ms,在推入最后一个符号的那个解码线程里同步完成,其他线程照常扫描。
- `-ffast-math` **禁止**用于 `d3_lt.c`:Decimen 的度分布依赖逐位一致的 IEEE-754 运算,快速数学会改变结果,导致和发送端对不上。

## 7. Kotlin 层

- **CameraController.kt**:直接用 `ProcessCameraProvider` 绑定 `Preview` + `ImageAnalysis`,没有用更省事的 `LifecycleCameraController`,因为要通过 `camera2Interop {}`(CameraX 1.7 起替代 `Camera2Interop`)设置 AE 目标帧率范围。
  - **分析流**:`ResolutionSelector` 优先 1920×1080(可在设置中改);`STRATEGY_KEEP_ONLY_LATEST`;`OUTPUT_IMAGE_FORMAT_YUV_420_888`。
  - **对焦**:默认连续自动对焦;点按对焦用 `FocusMeteringAction`(约 20 行)。
  - **缩放**:双指缩放调用 `setZoomRatio`(约 20 行)。屏幕离得远、码显得小时很有用。
  - **曝光**:曝光补偿调用 `setExposureCompensationIndex`。对着亮屏时,往往 −0.5 到 −1 EV 识别率更高。
- **DecodePipeline.kt**:
  - 帧缓冲池:N+2 个 `ByteBuffer.allocateDirect(w*h)`,用完归还。
  - 解码线程池:`Executors.newFixedThreadPool(N)`,每个线程用 ThreadLocal 持有自己的 scanner。
  - 把 `scan()` 返回的事件转成 `ScanViewModel` 的状态更新,进度更新节流到约 10 Hz。
- **ScanViewModel.kt**:状态 `Idle`、`Receiving(progress)`、`Done(result)`、`Error(msg)`,用 `StateFlow` 暴露,Activity 在 `repeatOnLifecycle(STARTED)` 中收集。
- **生命周期**:
  - Activity 进入后台时解绑相机、停止线程池,但不清空 `rx`,回到前台可以继续收。
  - 只有用户在设置里改了相机参数才重新绑定相机。

## 8. 界面(MD2)

```
竖屏                                          横屏 / 大屏(sw ≥ 600dp)
┌──────────────────────────────┐             ┌───────────────────────────┬──────────────────┐
│ CodeTrans                  ⚙  │ MaterialToolbar│ CodeTrans                                  ⚙ │
├──────────────────────────────┤             ├───────────────────────────┼──────────────────┤
│                              │             │                           │ ┌ 预览 ─────────┐ │
│        PreviewView           │             │       PreviewView         │ │               │ │
│        (相机取景)             │             │                           │ │               │ │
│ ▓▓▓▓▓▓▓▓▓▓░░░░░░  62%        │ 进度条叠在    │ ▓▓▓▓▓▓░░░░ 62%            │ └───────────────┘ │
│ CodeTrans·图片·286KB·45KB/s·2码│ 取景框底部     │ CodeTrans·图片·45KB/s·2码  │       [⤴ 分享]   │
├──────────────────────────────┤             └───────────────────────────┴──────────────────┘
│ ┌ 预览 MaterialCardView ────┐ │
│ │ 文本:可选中复制,可滚动    │ │
│ │ 图片:降采样后的缩略图      │ │
│ │ 文件:图标 + 文件名 + 大小  │ │
│ └──────────────────────────┘ │
│                   [ ⤴ 分享 ] │ ExtendedFloatingActionButton
└──────────────────────────────┘
```

- **主题**:`Theme.MaterialComponents.DayNight.NoActionBar`,跟随系统深浅色。
- **用到的组件**:`MaterialToolbar`、`MaterialCardView`、`LinearProgressIndicator`、`ExtendedFloatingActionButton`、`Snackbar`。不用任何 M3 主题或组件。
- **各状态的样子**:

  | 状态 | 预览卡片 | 进度条 | 分享按钮 |
  |---|---|---|---|
  | 空闲 | 提示"将相机对准电脑上的二维码" | 隐藏 | 禁用 |
  | 接收中 | 保持上一次的结果 | 显示,并显示状态文字 | 可用(分享上一次的结果) |
  | 完成 | 替换为新结果 | 隐藏 | 可用 |
  | 普通二维码 | 显示文本,网址可以点击 | 隐藏 | 可用 |

  完成时振动一次,文本自动复制(可关)。
- **文本预览**:`textIsSelectable`,长按选中即可复制,所以不单独做复制按钮。只渲染前 20 000 个字符,完整内容通过分享发出。
- **边到边显示**:targetSdk 35 起强制开启,用 `ViewCompat.setOnApplyWindowInsetsListener` 处理状态栏(Toolbar)和导航栏(分享按钮)的内边距。
- **大屏**:targetSdk 36 在大屏设备上会忽略方向锁定,所以必须提供 `layout-land/` 布局,不能只靠锁竖屏。
- **常亮**:扫码时 `FLAG_KEEP_SCREEN_ON`(可关)。
- **相机权限被拒**:取景区换成说明文字和"授予权限"按钮;永久拒绝时跳到系统设置。

## 9. 设置页(`PreferenceFragmentCompat`,MD2 样式)

| 分组 | 设置 | 选项 | 默认 |
|---|---|---|---|
| 相机 | 分析分辨率 | 720p / 1080p / 1440p / 2160p | 1080p |
| 相机 | 相机帧率 | 30 / 60(机型支持才显示) | 30 |
| 相机 | 曝光补偿 | −2 … +2 EV | 0 |
| 解码 | 解码线程数 | 自动 / 1–6 | 自动 = min(4, 大核数) |
| 解码 | 接收 Decimen 码 | 开 / 关 | 开 |
| 解码 | 识别普通二维码 | 开 / 关 | 开 |
| 结果 | 收到文本自动复制 | 开 / 关 | 开 |
| 结果 | 文件自动保存到 下载/CodeTrans(Android 10+) | 开 / 关 | 关 |
| 结果 | 完成时振动 / 提示音 | 开 / 关 | 振动开,提示音关 |
| 其他 | 扫码时屏幕常亮 | 开 / 关 | 开 |
| 关于 | 版本、开源许可 | nanorq MIT、quirc ISC、miniz MIT、Material Components 与 AndroidX Apache-2.0 | — |

返回主界面时立即生效。改了分辨率或帧率会重新绑定相机;其他设置不中断接收。

## 10. 结果处理与分享(`ResultStore.kt` / `ShareHelper.kt`)

- **缓存**:结果写在 `cacheDir/rx/<session>/`。来了新结果就删掉上一个结果的目录(不保留历史);App 启动时清空 `cacheDir/rx/`。
- **分享**:都用 `Intent.createChooser` 调起系统分享面板。

  | 内容 | 分享方式 |
  |---|---|
  | 文本 ≤ 100 KB | `ACTION_SEND` + `EXTRA_TEXT` |
  | 文本 > 100 KB | 存成 `.txt` 再按文件分享。Binder 单次事务约 1 MB 上限,大文本直接放进 Intent 会抛 `TransactionTooLargeException` |
  | 图片或单个文件 | `ACTION_SEND` + FileProvider 的 `content://` URI,加 `FLAG_GRANT_READ_URI_PERMISSION`,并设置 `ClipData`,分享面板里会显示缩略图 |
  | 多个文件 | `ACTION_SEND_MULTIPLE`;MIME 类型不一致时用 `*/*` |

- **自动复制**:文本 ≤ 100 KB 才复制,理由同上;超过时用 Snackbar 提示"内容过大,请用分享"。Android 13+ 系统会自己弹复制提示,不需要再弹 Toast。
- **自动保存**(Android 10+):通过 `MediaStore.Downloads` 写入 `RELATIVE_PATH = Download/CodeTrans`,不需要存储权限。Android 6–9 上隐藏这个选项,用户可以通过分享保存。
- **文件名清洗**(文件名来自扫码,不可信):
  - 去掉路径分隔符、`..`、控制字符
  - 长度限制在 120 以内
  - 重名时加 ` (1)` 后缀
  - FileProvider 只开放 `cache/rx/`

## 11. 权限、隐私与安全

- **权限**:只申请 `CAMERA` 和 `VIBRATE`(普通权限)。**不申请 INTERNET**,因此 App 没有能力把数据发出去。
- **不可信输入**:任何出现在镜头前的二维码都可能是恶意构造的。
  - C 解析器对每个字段做边界检查,并设上限:CodeTrans 总大小 ≤ 256 MB,Decimen ≤ 64 MB,同时只保留 2 个活跃会话。
  - `core/tests` 里用 libFuzzer 对 `cp_rx_push()` 做模糊测试。
  - 收到的内容只显示和分享,不会自动打开或执行;链接需要用户自己点。
- **组件导出**:只导出启动用的 `MainActivity`;FileProvider 设为 `exported=false`,通过 `grantUriPermissions` 授权。

## 12. 构建配置

- **Gradle 与语言**:AGP 9.x + Gradle 9.x,使用 AGP 9 内置的 Kotlin 支持(不再需要 `org.jetbrains.kotlin.android` 插件)。版本统一写在 `libs.versions.toml`。
- **依赖**:
  - `androidx.core:core-ktx`、`androidx.appcompat`、`androidx.activity:activity-ktx`、`androidx.lifecycle:lifecycle-runtime-ktx`
  - `com.google.android.material:material`(使用其中的 MaterialComponents 主题)
  - `androidx.preference:preference-ktx`
  - `androidx.camera:camera-core / camera-camera2 / camera-lifecycle / camera-view`(1.7.x)
  - `kotlinx-coroutines-android`
- **NDK**:r28 及以上,默认按 16 KB 页对齐。Google Play 要求面向 Android 15+ 的应用支持 16 KB 页。
  - `externalNativeBuild` 使用 CMake 3.22+。
  - 编译参数:`-O3`,arm64 默认带 NEON;`d3_lt.c` 单独禁用 `-ffast-math`。
- **ABI**:发布 `arm64-v8a` 和 `armeabi-v7a`;调试时额外打 `x86_64`(模拟器用)。
- **发布**:开启 R8 代码混淆和资源压缩;目标 APK 小于 5 MB(主要体积来自 Material 和 CameraX)。

## 13. 性能目标与调优

| 指标 | 目标(中端机,1080p 分析) |
|---|---|
| quirc 单帧检测 + 解码(含 2–4 个码) | ≤ 40 ms / 帧 / 线程 |
| 分析吞吐(2–4 线程) | 跟上相机的 30 fps |
| RaptorQ 解一块(K=4096,T≈1244) | ≤ 100 ms |
| 默认配置(v25-L,2 码,15 fps)有效吞吐 | ≥ 30 KB/s |
| 10 KB 文本,从对准到出结果 | < 1.5 s |

- **调优手段**:分析分辨率、线程数、曝光补偿、AE 帧率范围。另外做一个只在 debug 版出现的"导出 Y 帧"开关,把真实拍屏的帧序列存下来,拿到桌面上反复调 quirc 参数。
- **发热**:Android 10+ 用 `PowerManager.addThermalStatusListener` 监听。状态到 `SEVERE` 时自动降到 1 个线程、720p,并提示用户。
- **摩尔纹、过曝、卷帘快门**:
  - 建议发送端模块像素 ≥ 3,刷新率不超过相机帧率的一半。
  - 卷帘快门拍到的半新半旧的帧,会被二维码自身的校验和帧 CRC32 丢掉,只损失效率,不影响正确性。

## 14. 里程碑

| 阶段 | 内容 | 验收 |
|---|---|---|
| A0 骨架 | AGP 9 工程、MD2 主题、主界面和设置页布局、CMake 链接 `core`、能调通一个 JNI 函数 | 在 minSdk 23 模拟器和一台真机上能启动 |
| A1 C 解码核心 | quirc 集成、`cp_rx`(CodeTrans)、容器、写文件;桌面回环测试复用 Windows W1 生成的帧 PNG | 仪器测试:喂入帧 PNG 序列,结果和原文件逐字节一致 |
| A2 相机管线 | CameraX 绑定、Y 平面拷贝、帧缓冲池、解码线程池、进度状态 | 真机扫 Windows 发送端(W2 之后)端到端成功 |
| A3 预览与分享 | 三种结果的预览、分享、自动复制、振动、缓存清理、自动保存 | 文本、图片、多文件都能分享到微信、文件管理器等常见 App |
| A4 设置页 | 第 9 节全部设置项实时生效 | 改分辨率后相机重新绑定,接收中的进度不丢 |
| A5 Decimen 兼容 | ✅ LT 剥离解码 + Cauchy RS·96 + DCF2 容器 + 对比度重试(许可证:AGPL 移植,见 windows/PLAN §5.4) | 参考实现生成的帧与 C 互解逐字节一致;仪器测试 RS / LT / 重试帧通过;待真机扫 decimen.app |
| A6 性能与打磨 | 默认参数校准、发热降级、横屏和大屏、R8、签名发布 | 达到第 13 节的目标;5 分钟连续接收不崩溃、不明显降频 |

## 15. 测试计划

- **core 共用测试**(桌面端):容器、帧、RaptorQ 回环、Decimen golden vector;对 `cp_rx_push` 做 libFuzzer 模糊测试。
- **Android 仪器测试**:
  - 把预先录好的帧序列(PNG 或导出的 Y 帧)喂给 `DecodePipeline`,断言结果正确。
  - 相机部分用 CameraX 测试工具和 Fake,不用 Mockito,与项目里 `.agents/skills/camerax` 的建议一致。
- **真机矩阵**:
  - 低端(4 核 A53 级)、中端、旗舰各至少一台
  - Android 6 / 10 / 14 / 16
  - 与 Windows 端一起跑吞吐矩阵(版本 × 模块像素 × 码数 × fps),结果用来确定两端的默认值

## 16. 风险与待定事项

| 风险 | 影响 | 应对 |
|---|---|---|
| quirc 识别高密度码(v30–40)不够稳:它只用一个校正图形做透视变换,镜头畸变大时容易失败。Decimen 默认用 v40 | CodeTrans 只能用中等版本;Decimen 兼容的识别率可能偏低 | CodeTrans 默认 v25 + 多码弥补吞吐。A2 时实测 v40 的识别率;不达标就再决定是否只为兼容模式引入 zxing-cpp(C++,Apache-2.0) |
| Decimen v3 的参数只在 AGPL 源码里 | A5 无法开工 | 与 Windows 端 W6 一起定许可证方案 |
| 低端机 1080p 的 YUV 分析太慢 | 丢帧多,吞吐低 | 自动线程数;设置里可降到 720p;发送端少放几个码、换用更低版本 |
| 发热降频 | 长时间传输变慢 | 监听热状态并自动降级 |
| 原生内存:最大 256 MB 的结果放在内存里 | 低内存设备上被系统杀掉 | 第一版按内容大小设上限;以后可以改成按块边解码边落盘 |

## 17. 参考

- Decimen Optical Transfer <https://github.com/bashalarmistalt/decimen-optical-transfer>;decimen-cli <https://github.com/jackbauertv24-droid/decimen-cli>
- RFC 6330 <https://www.rfc-editor.org/rfc/rfc6330>;nanorq <https://github.com/sleepybishop/nanorq>;quirc <https://github.com/dlbeer/quirc>;miniz <https://github.com/richgel999/miniz>
- CameraX 指南见项目内 `android/.agents/skills/camerax/`;AGP 9 迁移说明见 `android/.agents/skills/agp-9-upgrade/`
- Material Components for Android(MD2 主题)<https://github.com/material-components/material-components-android>
