# Support status and development roadmap

> Documented version: `0.4.2.dev4`.

Contributions, bug reports, and issues are welcome.

## Near-term priorities

- **Improve decoder error handling (highest priority).** The current code contains many exception paths, some of which terminate and recreate a decoding session even when recovery might be possible. An invalid pipeline configuration can lead to an endless restart/error loop because restarting cannot fix it.
- Allow explicit selection of a CUDA device.
- Separate CUDA and CPU build configurations; remove the CUDA Toolkit requirement for CPU-only or VAAPI-only builds.
- Improve handling of color metadata in CPU, VAAPI, and CUDA postprocessing; add GPU conversion support for more matrices and full-range sources.
- Support additional input `sw_format` values and 10-bit formats, and provide broader Python access to native planar data.
- Make the VAAPI render node configurable.

## Current testing focus

VAAPI has received the most development and testing effort. D3D11VA is experimental and has seen very little testing. Other combinations may work, but **allowed**, **implemented**, and **tested** are not equivalent. See [the documentation index](README.md) and [compatibility matrix](compatibility.md).
