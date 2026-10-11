# 0002. 7-Zip's LZMA2 decoder for 7z extraction

- Status: accepted
- Date: 2026-10-11
- Issue: #6

## Context

The extraction stage reads every archive with libarchive, built with zstd, xz/lzma, zlib and bzip2.
#6 asked whether that is fast enough on the console, or whether Raiju should decode 7z's LZMA2 with
7-Zip's own decoder, whose `Lzma2DecMt` decodes LZMA2 blocks on several threads.

Measured on the console (firmware 13600007) with `payload/spikes/libarchive`, extracting samples of
70% incompressible and 30% compressible content into `/data/raiju`. The four 1 GiB runs each wrote
1,048,576,000 bytes:

| format | archive | seconds | bytes written per second |
|---|---|---|---|
| zip (deflate) | 735 MB | 15.08 | 69.53 MB/s |
| tar.zst (zstd `-3`) | 734 MB | 24.30 | 43.15 MB/s |
| 7z (LZMA2, `-mx=5`) | 734 MB | 48.68 | 21.54 MB/s |
| rar5 (`-m5`) | 147 MB | 12.57 | 16.69 MB/s |

Three things follow from the runs rather than from the table alone.

- **The write path is not the limit.** Every format wrote the same bytes to the same filesystem, and
  zip sustained 69.53 MB/s doing it, so the 7z figure is set by the decoder.
- **7z cannot hide that time behind the download.** It needs random access to its index, so it cannot
  be extracted straight out of a download the way `tar.zst` can, which puts LZMA2 decode on the
  critical path instead of alongside it: 20 GB takes about 16 minutes to extract at 21.54 MB/s, and a
  50 GB dump about 39.
- **Only one core is used.** `liblzma` decodes LZMA2 on a single thread, so the rest of the console's
  cores idle for the whole extraction.

The same runs show the network is not the constraint: samples arrived at 22.2–22.5 MB/s on one
connection, while the product downloads with 16.

## Decision

Add 7-Zip's LZMA2 decoder (the LZMA SDK's `LzmaDec` and `Lzma2DecMt`, public domain) as a decode path
that 7z alone uses, keeping libarchive for every other format and as the fallback. This is the trigger
ARCHITECTURE.md already named: the benchmark shows 7z LZMA2 is the bottleneck.

`Lzma2DecMt` splits its work across LZMA2 blocks, so the gain depends on how the archive was written:
one made with several blocks decodes in parallel, a single-block one does not.

## Consequences

- The two decoders must agree byte for byte. A conformance test compares them over the same archives,
  and 7z falls back to libarchive for any coder or block structure the fast path does not handle, so a
  failure costs time, never correctness.
- Each decode thread holds its own LZMA2 dictionary, so memory grows with the thread count — a 64 MiB
  dictionary across four threads is 256 MiB. The count needs a cap that leaves room for the download
  buffers and the rest of the pipeline.
- The 7z container then has to be parsed by us or by the SDK's reader, so 7z stops being a format
  libarchive fully owns. That parsing is the riskiest part of the change and is what the extraction
  tests have to cover.
- LZMA SDK and the four libraries here are public domain or BSD, so the GPL-3.0-or-later position does
  not change.
- The measurement stops short of criterion 3 in #6. Criterion 3 asks for at least 20 GB, and on a
  16 GB console anything smaller sits in the page cache; the runs above are 1 GiB with one 4 GiB pass.
  Decode holds a steady rate across them and the write path is common to all four formats, so the
  conclusion is not expected to move, but a full-scale pass on a real dump is still owed before the
  beta leans on 7z.
