Gyroflow telemetry schema, downloaded 2026-11-05 from:
https://github.com/AdrianEddy/telemetry-parser/blob/master/src/gyroflow/gyroflow.proto

telemetry-parser is dual-licensed MIT OR Apache-2.0. The schema is used to retain
all supported telemetry fields when reading binary protobuf tracks as JSONL.

MotionCam Fuse stores large camera.additionalData values across header-only
protobuf packets to avoid a duration-dependent 64 MiB packet limit. The main
header carries {"motioncamTelemetryChunks":{"version":1,"count":N}}. Extension
headers carry {"motioncamTelemetryChunk":{"version":1,"index":I,"text":"..."}}.
The text pieces, ordered by index, reconstruct the exact original additionalData
string. Each piece is at most 1 MiB and ends on a UTF-8 boundary. Import rejects
missing or duplicate pieces. Frame packets retain their video timestamps and all
standard Gyroflow fields; consumers can read those without using this extension.
JSONL export reconstructs the full additionalData value.
