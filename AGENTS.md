# Repository Guidelines

## Project Structure

CodeTrans transfers files, images, and text offline between Windows and Android using a screen and camera.

- Put shared C11 protocol logic in `core/src/` and camera scanning code in `core/scan/`. Both clients use the public API in `core/include/cp.h`. When adding a core source file, update both `core/CMakeLists.txt` and `android/app/src/main/cpp/CMakeLists.txt`.
- Keep Win32 application code in `windows/src/` and icons, manifests, and resource definitions in `windows/res/`.
- Under `android/app/src/main/`, use `kotlin/com/codetrans/` for application code, `cpp/` for the JNI bridge, and `res/` for layouts and strings. Default strings are Chinese, with English translations in `res/values-en/`.

## Build and Run

- From the repository root in Git Bash on Windows, run `windows/build.sh` with CMake 3.20+, Ninja, and the **msvcrt** build of llvm-mingw. It builds x86 and runs Windows and quick core tests. Launch `windows/build-x86/CodeTrans.exe`. Use `windows/build.sh all` to also build and test x64.
- From `android/`, run `./gradlew --no-configuration-cache :app:testDebugUnitTest :app:assembleDebug` to test and build the debug APK. Use `./gradlew :app:installDebug` to install it on a connected device. In PowerShell, use `.\gradlew.bat`. See the root README for SDK and NDK versions.

## Coding Style

Use four-space indentation in C and Kotlin. Use `snake_case` for C functions and the `cp_` prefix for public core symbols. Match Kotlin class and file names, and use `camelCase` for functions and properties. Run Android Lint from `android/` with `./gradlew :app:lintDebug`.

## Testing

Add native `test_*` cases to `core/tests/test_core.c` or `windows/tests/test_main.c` and register them in the test runner. Use the reference frames in `core/tests/data/` for Decimen compatibility changes. Android uses JUnit 4 and `*Test.kt` classes under `src/test/` and `src/androidTest/` in the app module. Run device tests with `./gradlew :app:connectedDebugAndroidTest` from `android/`. CI sets no numeric coverage threshold.

## Commits and Pull Requests

Recent commits use short imperative subjects such as “Build separate ARMv7 and ARM64 release APKs.” In a pull request, identify the affected clients, describe the behavior change, and list the tests run. Link a related issue when available and include a screenshot for visible UI changes.
