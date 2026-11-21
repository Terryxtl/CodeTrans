# Decimen golden vectors

Produced by the Decimen Optical Transfer **0.5.3-fixed.1** reference implementation
(`shared/protocol.ts`, `fountain.ts`, `cauchy.ts`, `snippet.ts`), not by this
repository's encoder, so `cp_core_tests` checks interoperability rather than
self-consistency.

`*.frames` is a sequence of `[u16 little-endian length][frame bytes]`, exactly the
subset of frames the reference receiver needed to finish.

| file | stream | payload | blockLen | sessionId | first seq | dropped |
|---|---|---|---|---|---|---|
| `d3_rs.frames` | Cauchy RS-96 (flags `0x02`) | `d3_file.expect` as `测试 data.bin`, DCF2 without gzip | 200 | `0x1234` | 0 | every 4th |
| `d3_lt.frames` | LT carousel (flags `0x00`) | same | 200 | `0x1234` | 0 | every 4th |
| `d3_snip.frames` | Cauchy RS-96 | `d3_snip.expect` as a text snippet, gzip (`CompressionStream`) | 300 | 7 | 37 | every 3rd |

`d3_file.expect` is 5000 bytes of `splitmix32(0x5eed) & 255`.
