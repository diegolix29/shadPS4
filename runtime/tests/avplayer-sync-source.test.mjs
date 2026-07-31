import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { dirname, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import test from "node:test";

const root = resolve(dirname(fileURLToPath(import.meta.url)), "../..");
const read = (relative) => readFileSync(resolve(root, relative), "utf8");

const section = (source, startMarker, endMarker) => {
  const start = source.indexOf(startMarker);
  assert.notEqual(start, -1, `missing start marker: ${startMarker}`);
  const end = source.indexOf(endMarker, start);
  assert.notEqual(end, -1, `missing end marker: ${endMarker}`);
  return source.slice(start, end);
};

test("AvPlayerSource includes BACHATA_AVPLAYER_TRACE rate-limited diagnostics for GetVideoData boundaries", () => {
  const cpp = read("src/core/libraries/avplayer/avplayer_source.cpp");
  const getVideoData = section(
    cpp,
    "bool AvPlayerSource::GetVideoData(AvPlayerFrameInfoEx& video_info)",
    "bool AvPlayerSource::GetAudioData(AvPlayerFrameInfo& audio_info)"
  );

  assert.match(getVideoData, /BACHATA_AVPLAYER_TRACE/);
  assert.match(getVideoData, /stage=get_video_data_inactive_paused/);
  assert.match(getVideoData, /stage=get_video_data_empty/);
  assert.match(getVideoData, /stage=get_video_data_ahead_of_audio/);
  assert.match(getVideoData, /stage=get_video_data_success/);

  // Exact stage to counter mapping verification
  assert.match(
    getVideoData,
    /m_trace_video_data_inactive_count[\s\S]*stage=get_video_data_inactive_paused/
  );
  assert.match(
    getVideoData,
    /m_trace_video_data_empty_count[\s\S]*stage=get_video_data_empty/
  );
  assert.match(
    getVideoData,
    /m_trace_video_data_ahead_count[\s\S]*stage=get_video_data_ahead_of_audio/
  );
  assert.match(
    getVideoData,
    /m_trace_video_data_success_count[\s\S]*stage=get_video_data_success/
  );

  // Exposes last_audio_ts, eof, call count
  assert.match(getVideoData, /last_audio_ts=/);
  assert.match(getVideoData, /eof=/);
  assert.match(getVideoData, /count=/);
});

test("AvPlayerSource includes BACHATA_AVPLAYER_TRACE rate-limited diagnostics for GetAudioData boundaries", () => {
  const cpp = read("src/core/libraries/avplayer/avplayer_source.cpp");
  const getAudioData = section(
    cpp,
    "bool AvPlayerSource::GetAudioData(AvPlayerFrameInfo& audio_info)",
    "u64 AvPlayerSource::DurationMillis() const"
  );

  assert.match(getAudioData, /BACHATA_AVPLAYER_TRACE/);
  assert.match(getAudioData, /stage=get_audio_data_inactive_paused/);
  assert.match(getAudioData, /stage=get_audio_data_empty/);
  assert.match(getAudioData, /stage=get_audio_data_success/);

  // Exact stage to counter mapping verification
  assert.match(
    getAudioData,
    /m_trace_audio_data_inactive_count[\s\S]*stage=get_audio_data_inactive_paused/
  );
  assert.match(
    getAudioData,
    /m_trace_audio_data_empty_count[\s\S]*stage=get_audio_data_empty/
  );
  assert.match(
    getAudioData,
    /m_trace_audio_data_success_count[\s\S]*stage=get_audio_data_success/
  );

  assert.match(getAudioData, /last_audio_ts=/);
  assert.match(getAudioData, /eof=/);
  assert.match(getAudioData, /count=/);
});

test("AvPlayerSource includes BACHATA_AVPLAYER_TRACE rate-limited diagnostics for VideoDecoderThread frame queuing", () => {
  const cpp = read("src/core/libraries/avplayer/avplayer_source.cpp");
  const videoDecoderThread = section(
    cpp,
    "void AvPlayerSource::VideoDecoderThread(std::stop_token stop)",
    "AvPlayerSource::AVFramePtr AvPlayerSource::ConvertAudioFrame(const AVFrame& frame)"
  );

  assert.match(videoDecoderThread, /BACHATA_AVPLAYER_TRACE/);
  assert.match(videoDecoderThread, /stage=video_frame_queued/);
  assert.match(
    videoDecoderThread,
    /m_trace_video_frame_queued_count[\s\S]*stage=video_frame_queued/
  );
  assert.match(videoDecoderThread, /frame_ts=/);
  assert.match(videoDecoderThread, /last_audio_ts=/);
  assert.match(videoDecoderThread, /eof=/);
  assert.match(videoDecoderThread, /count=/);
});

test("AvPlayerSource includes BACHATA_AVPLAYER_TRACE rate-limited diagnostics for DemuxerThread EOF state", () => {
  const cpp = read("src/core/libraries/avplayer/avplayer_source.cpp");
  const demuxerThread = section(
    cpp,
    "void AvPlayerSource::DemuxerThread(std::stop_token stop)",
    "AvPlayerSource::AVFramePtr AvPlayerSource::ConvertVideoFrame(const AVFrame& frame)"
  );

  assert.match(demuxerThread, /BACHATA_AVPLAYER_TRACE/);
  assert.match(demuxerThread, /stage=demux_eof/);
  assert.match(
    demuxerThread,
    /m_trace_demux_eof_count[\s\S]*stage=demux_eof/
  );
  assert.match(demuxerThread, /last_audio_ts=/);
  assert.match(demuxerThread, /eof=/);
  assert.match(demuxerThread, /count=/);
});

test("AvPlayerSource bounding/rate-limiting logic is exponential powers-of-two without periodic modulo or unsafe cross-thread reads", () => {
  const header = read("src/core/libraries/avplayer/avplayer_source.h");
  const cpp = read("src/core/libraries/avplayer/avplayer_source.cpp");

  assert.match(cpp, /ShouldTraceCount/);
  // Powers-of-two / exponential test
  assert.match(cpp, /\(count\s*&\s*\(count\s*-\s*1\)\)\s*==\s*0/);
  // Forbid periodic modulo limiter in ShouldTraceCount
  const shouldTraceSec = section(cpp, "static bool ShouldTraceCount", "AvPlayerSource::AvPlayerSource");
  assert.doesNotMatch(shouldTraceSec, /%/);

  // Dedicated atomic counter for audio inactive
  assert.match(header, /std::atomic<u64>\s+m_trace_audio_data_inactive_count/);
  assert.match(header, /std::atomic<u64>\s+m_atomic_last_audio_ts/);

  // Forbid non-atomic m_last_audio_ts.value_or in DemuxerThread and VideoDecoderThread
  const demuxerThread = section(
    cpp,
    "void AvPlayerSource::DemuxerThread(std::stop_token stop)",
    "AvPlayerSource::AVFramePtr AvPlayerSource::ConvertVideoFrame(const AVFrame& frame)"
  );
  assert.doesNotMatch(demuxerThread, /m_last_audio_ts\.value_or/);

  const videoDecoderThread = section(
    cpp,
    "void AvPlayerSource::VideoDecoderThread(std::stop_token stop)",
    "AvPlayerSource::AVFramePtr AvPlayerSource::ConvertAudioFrame(const AVFrame& frame)"
  );
  assert.doesNotMatch(videoDecoderThread, /m_last_audio_ts\.value_or/);

  // Forbid AvPlayerQueue Size calls in trace logs
  const traceLogs = cpp.split("\n").filter(line => line.includes("BACHATA_AVPLAYER_TRACE")).join("\n");
  assert.doesNotMatch(traceLogs, /\.Size\(\)/);
});

test("AvPlayerSource preserves original synchronization, predicates, return values, and public API", () => {
  const header = read("src/core/libraries/avplayer/avplayer_source.h");
  const cpp = read("src/core/libraries/avplayer/avplayer_source.cpp");

  // Public API methods in header intact
  assert.match(header, /bool GetAudioData\(AvPlayerFrameInfo& audio_info\);/);
  assert.match(header, /bool GetVideoData\(AvPlayerFrameInfo& video_info\);/);
  assert.match(header, /bool GetVideoData\(AvPlayerFrameInfoEx& video_info\);/);

  // Sync predicate and return logic intact
  assert.match(cpp, /new_frame\.info\.timestamp > m_last_audio_ts\.value_or\(0\)/);
  assert.match(cpp, /m_state\.GetSyncMode\(\) == AvPlayerAvSyncMode::Default/);
});

