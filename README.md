# CodeTrans

[![Build](https://github.com/Terryxtl/CodeTrans/actions/workflows/build.yml/badge.svg)](https://github.com/Terryxtl/CodeTrans/actions/workflows/build.yml)

用连续播放的二维码在电脑和手机之间离线传输文本、图片和文件:电脑端把内容编码成二维码动画在屏幕上播放,手机摄像头扫完即可还原。全程不联网。

> **内容不加密。** 屏幕上的码任何摄像头都能读到,不要在别人看得到你屏幕的地方发送敏感内容。

| 目录 | 内容 |
|---|---|
| [`windows/`](windows/README.md) | Windows 发送端:单个便携 exe,支持 Win7 SP1 – Win11(32/64 位) |
| [`android/`](android/PLAN.md) | Android 接收端(minSdk 24),CameraX + quirc / ZXing 扫码 |
| `core/` | 两端共用的 C11 协议核心:分帧、RaptorQ 喷泉码、容器格式,以及 Decimen v3 兼容层 |

支持两种协议:CodeTrans v1(RaptorQ, RFC 6330)和 Decimen v3(LT / Cauchy RS-96),可与 decimen.app / decimen-cli 互通。

## 下载

[Actions](https://github.com/Terryxtl/CodeTrans/actions/workflows/build.yml) 里每次构建都会产出 `CodeTrans.exe` 和 APK;推送 `v*` 标签时会自动发布到 [Releases](https://github.com/Terryxtl/CodeTrans/releases)。

## 构建

- **Windows**:需要 [llvm-mingw](https://github.com/mstorsjo/llvm-mingw) 的 msvcrt 版、CMake ≥ 3.20、Ninja,运行 `windows/build.sh`。详见 [windows/README.md](windows/README.md)。
- **Android**:JDK 17+(推荐 21)、Android SDK 37、NDK 30.0.14904198、CMake 3.22.1,在 `android/` 下运行 `./gradlew assembleRelease`。release 签名配置放在 `android/signing/signing.properties`(不入库),没有时产出未签名包。

### CI 签名(可选)

在仓库 Settings → Secrets and variables → Actions 中添加以下 Secrets 后,CI 产出的 release APK 会自动签名:

| Secret | 内容 |
|---|---|
| `SIGNING_KEYSTORE_BASE64` | keystore 文件的 base64(`base64 -w0 release.jks`) |
| `SIGNING_STORE_PASSWORD` | keystore 密码 |
| `SIGNING_KEY_ALIAS` | key 别名 |
| `SIGNING_KEY_PASSWORD` | key 密码 |

## 许可证

[AGPL-3.0-or-later](LICENSE)。`core/src/d3.c` 的 Decimen v3 兼容实现移植自 Decimen Optical Transfer v0.5.3(AGPL-3.0-or-later)。

第三方库保留各自的许可证:[miniz](core/third_party/miniz/LICENSE)(MIT)、[nanorq](core/third_party/nanorq/LICENSE)(MIT)、[qrcodegen](core/third_party/qrcodegen/LICENSE)(MIT)、[quirc](core/third_party/quirc/LICENSE)(ISC)、[stb](core/third_party/stb/LICENSE)(MIT / 公有领域)、[zxing-cpp](android/app/src/main/cpp/third_party/zxing-cpp/LICENSE)(Apache-2.0)。
