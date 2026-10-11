# 0005. Read encoded 7z headers with liblzma

- Status: accepted
- Date: 2026-10-11
- Issue: #17

## Context

`/api/probe` reports a download's uncompressed size before it starts, so the UI can say whether the archive fits. For 7z it reads the archive index with HTTP range requests. Most 7z archives — everything 7-Zip writes by default, and libarchive's default too — store their header **encoded**: the next header begins with `kEncodedHeader` (`0x17`) and a StreamsInfo describing one packed, LZMA-compressed header stream. Until this decision the probe answered `uncompressedKnown:false` for those archives, which is nearly all of them.

Two ways to read an encoded header were considered:

- **Feed libarchive ranged reads.** A read/seek callback over the fetch seam would sum `archive_entry_size()` over the entries and give one reader for every archive format. But a solid 7z folder holds all of its files in one compressed stream, and libarchive must consume (decode) that stream to advance to the next entry, so summing entry sizes would download and decompress the whole archive — gigabytes of traffic and minutes of work for a size estimate.
- **Decode the header stream here.** The header stream is small (kibibytes), costs one range request, and its decoded content is an ordinary plain header the parser already handles.

## Decision

- The probe parses the encoded header's StreamsInfo (PackPos, one pack stream, one folder), fetches the packed header stream (capped at 4 MiB), decodes it in memory (decoded header capped at 16 MiB) and parses the decoded bytes with the same code that reads a plain header, so both paths share one parser and one set of caps.
- Decoding uses liblzma: `lzma_properties_decode()` understands 7z's property encodings for LZMA1, LZMA2, Delta and the BCJ filters, and `lzma_raw_buffer_decode()` runs the chain in memory. A 7z LZMA1 stream carries no end-of-payload marker, so it is decoded as `LZMA_FILTER_LZMA1EXT` with the folder's declared unpacked size.
- The dependency is optional, exactly like OpenSSL: the Makefile defines `RAIJU_HAVE_LZMA` when pkg-config finds liblzma, and a stub reports the size as unknown otherwise. liblzma is public domain/0BSD, is already a libarchive dependency, and is GPL-3.0-or-later compatible.
- Anything unexpected — more than one pack stream or folder, a stream over either cap, a codec with no liblzma equivalent (BCJ2) — is reported as an unknown size, never guessed.

## Consequences

- A real 7-Zip or libarchive `.7z` now yields its true uncompressed size; the test writes one with libarchive and asserts the exact sum, which `bsdtar -tvf` confirms independently.
- The gateway links liblzma where it is present, and the CI gateway job installs it on both platforms. A host without liblzma still builds and answers `uncompressedKnown:false`.
- The caps keep a hostile archive from making the probe download or allocate without bound, and they cover the plain-header path too.
- A folder with more than one coder (BCJ plus LZMA, say) is decoded in the order the format stores them, reversed; only single-coder header streams are exercised, because that is what writers emit for a header.
