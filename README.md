# CodeTrans

[![Build](https://github.com/Terryxtl/CodeTrans/actions/workflows/build.yml/badge.svg)](https://github.com/Terryxtl/CodeTrans/actions/workflows/build.yml)

用连续播放的二维码在电脑和手机之间离线传输文本、图片和文件:电脑端把内容编码成二维码动画在屏幕上播放,手机摄像头扫完即可还原。全程不联网。

> **内容不加密。** 屏幕上的码任何摄像头都能读到,不要在别人看得到你屏幕的地方发送敏感内容。

| 目录 | 内容 |
|---|---|
| [`windows/`](windows/README.md) | Windows 端:单个便携 exe,支持 Win7 SP1 – Win11(32/64 位);播放二维码,也能用摄像头接收手机发来的码(F3) |
| [`android/`](android/PLAN.md) | Android 端（minSdk 24）。「发送到电脑」可选择 Decimen v3 RS96（默认）、光传码面横带或分块（实验），支持同屏显示 1–2 个码。 |
| `core/` | 两端共用的 C11 协议核心:分帧、RaptorQ 喷泉码、容器格式(自适应压缩)、Decimen v3 兼容层、相机扫码器(`core/scan/`),以及实验性的光传码面 |

两端均可接收 CodeTrans v1（RaptorQ，RFC 6330）、Decimen v3（LT / Cauchy RS-96）和实验性的光传码面。Decimen v3 支持与相应版本的 decimen.app / decimen-cli 互通。手机发送多文件时，RS96 将文件打成一个 ZIP，光传码面保留独立文件。横带和分块使用固定的 177 × 177 模块，需要 CodeTrans 接收端。[光传码面说明](core/OPTICAL_BOARD.md)

## 下载

[Releases](https://github.com/Terryxtl/CodeTrans/releases) 提供 Windows 便携版 `CodeTrans.exe`，以及 Android 签名版 `CodeTrans-armv7-release.apk`（32 位 ARM）和 `CodeTrans-arm64-release.apk`（64 位 ARM）。

[Actions](https://github.com/Terryxtl/CodeTrans/actions/workflows/build.yml) 会在推送到 `main` 或向 `main` 提交 PR 时运行测试，并上传 Windows EXE 和 Android 调试 APK。推送 `v*` 版本标签，或手动运行 `Build` 并填写 `release_tag`（例如 `v0.1.0`），会构建并发布上述三个文件。

## 构建

- **Windows**:需要 [llvm-mingw](https://github.com/mstorsjo/llvm-mingw) 的 msvcrt 版、CMake ≥ 3.20、Ninja,运行 `windows/build.sh`。详见 [windows/README.md](windows/README.md)。
- **Android**:JDK 17+(推荐 21)、Android SDK 37、NDK 30.0.14904198、CMake 3.22.1,在 `android/` 下运行 `./gradlew :app:assembleRelease -PsplitReleaseApks=true` 生成两个 ARM APK。release 签名配置放在 `android/signing/signing.properties`(不入库),没有时产出未签名包。

### 发布签名

发布流程使用仓库 Settings → Secrets and variables → Actions 中的以下 Secrets 为 APK 签名。调试构建使用 Android 的默认调试签名。

| Secret | 内容 |
|---|---|
| `SIGNING_KEYSTORE_BASE64` | keystore 文件的 base64(`base64 -w0 release.jks`) |
| `SIGNING_STORE_PASSWORD` | keystore 密码 |
| `SIGNING_KEY_ALIAS` | key 别名 |
| `SIGNING_KEY_PASSWORD` | key 密码 |

## 许可证

[AGPL-3.0-or-later](LICENSE)。`core/src/d3.c` 的 Decimen v3 兼容实现移植自 Decimen Optical Transfer v0.5.3(AGPL-3.0-or-later)。

第三方库保留各自的许可证:[miniz](core/third_party/miniz/LICENSE)(MIT)、[nanorq](core/third_party/nanorq/LICENSE)(MIT)、[qrcodegen](core/third_party/qrcodegen/LICENSE)(MIT)、[quirc](core/third_party/quirc/LICENSE)(ISC)、[stb](core/third_party/stb/LICENSE)(MIT / 公有领域)、[zxing-cpp](android/app/src/main/cpp/third_party/zxing-cpp/LICENSE)(Apache-2.0)。
