# testdata-gen

`gen_frames.c` renders the QR frame sequences used by the instrumented test
`app/src/androidTest/kotlin/com/codetrans/NativeCoreTest.kt` and checks each
sequence with the real receiver (`core/scan/scanner.c` + `core/`) before
writing it. The output is committed under `app/src/androidTest/assets/frames/`.
Regenerate it only when the wire format changes.

Build and run from `android/`, with any C11 host compiler (here llvm-mingw `clang`):

```sh
C=../core; TP=$C/third_party; O=build/testdata-gen
mkdir -p $O && clang -O2 -w -o $O/gen_frames.exe \
  -I$C/include -I$C/src -I$C/scan -I$TP/qrcodegen -I$TP/stb -I$TP/quirc/lib -I$TP/miniz \
  -I$TP/nanorq/include -I$TP/nanorq/deps -I$TP/nanorq/deps/obl \
  -DQUIRC_MAX_REGIONS=65534 -DMINIZ_NO_ARCHIVE_APIS -DMINIZ_NO_STDIO -DMINIZ_NO_TIME -DNDEBUG \
  testdata-gen/gen_frames.c $C/scan/scanner.c $C/scan/tracked_grid.c $C/src/*.c $TP/qrcodegen/qrcodegen.c $TP/quirc/lib/*.c \
  $TP/miniz/miniz.c $(ls $TP/nanorq/lib/*.c | grep -v /io.c) $TP/nanorq/deps/obl/*.c
rm -rf app/src/androidTest/assets/frames && mkdir -p app/src/androidTest/assets/frames
$O/gen_frames.exe app/src/androidTest/assets/frames $O/verify
```

The output is deterministic, apart from the random session ids.
