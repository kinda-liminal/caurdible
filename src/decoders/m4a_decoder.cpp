#include "m4a_decoder.hpp"
#include "mp4_track.hpp"

#include "aacdec.h"

#include <cstring>
#include <initializer_list>
#include <new>

#ifdef UNIT_TEST
#include "mp3_test_file.hpp"
#else
#include <SD.h>
#endif

namespace {

constexpr size_t kWindowBytes = 4096;
// SBR doubles the frame length.
constexpr size_t kMaxPcmSamples = AAC_MAX_NSAMPS * AAC_MAX_NCHANS * 2;
constexpr int kMaxBadFrames = 8;

}  // namespace

struct M4aDecoder::Impl {
  HAACDecoder aac = nullptr;
  DecoderInput* data = nullptr;
  DecoderInput* meta = nullptr;
  DecoderInput* sizes = nullptr;
  bool ownsInputs = false;
  Mp4Track track;
  AudioFormat fmt{};
  uint32_t outRate = 0;
  uint8_t window[kWindowBytes]{};
  uint32_t windowStart = 0;
  uint32_t windowLen = 0;
  int16_t pcm[kMaxPcmSamples]{};
  size_t pcmFrames = 0;
  size_t pcmOffset = 0;
  uint8_t pcmChannels = 1;
  uint64_t pcmStartTicks = 0;
  int badFrames = 0;
  bool sbrReady = false;
  bool sbrMissing = false;

  uint32_t ticksToMs(uint64_t ticks) const {
    return static_cast<uint32_t>(ticks * 1000u / track.timescale());
  }
};

uint8_t* M4aDecoder::sampleBytes(Impl& d, uint32_t offset, uint32_t size) {
  if (size == 0 || size > kWindowBytes) return nullptr;
  if (offset >= d.windowStart && offset - d.windowStart <= d.windowLen &&
      size <= d.windowLen - (offset - d.windowStart)) {
    return d.window + (offset - d.windowStart);
  }
  const uint32_t fileSize = d.data->size();
  if (offset >= fileSize || size > fileSize - offset) {
    d.windowLen = 0;
    return nullptr;
  }

  // Keep the tail of the window and continue the read at the current file position.
  // A backward seek on FAT walks the cluster chain from the start of the file.
  size_t keep = 0;
  const uint32_t windowEnd = d.windowStart + d.windowLen;
  if (d.windowLen != 0 && offset >= d.windowStart && offset < windowEnd &&
      d.data->position() == windowEnd) {
    keep = windowEnd - offset;
    std::memmove(d.window, d.window + (offset - d.windowStart), keep);
  } else if (d.data->position() != offset && !d.data->seek(offset)) {
    d.windowLen = 0;
    return nullptr;
  }
  const uint32_t readFrom = offset + static_cast<uint32_t>(keep);
  size_t want = kWindowBytes - keep;
  if (fileSize - readFrom < want) want = fileSize - readFrom;
  const size_t got = d.data->read(d.window + keep, want);
  d.windowStart = offset;
  d.windowLen = static_cast<uint32_t>(keep + got);
  return d.windowLen >= size ? d.window : nullptr;
}

// Allocate the SBR state (about 50 KB) and restart at the first sample.
bool M4aDecoder::enableSbr(Impl& d) {
  const Mp4AudioConfig& cfg = d.track.config();
  AACFrameInfo raw{};
  raw.nChans = cfg.channels;
  raw.sampRateCore = static_cast<int>(cfg.coreSampleRate);
  raw.profile = AAC_PROFILE_LC;
  if (AACEnableSBR(d.aac) != ERR_AAC_NONE ||
      AACSetRawBlockParams(d.aac, 0, &raw) != ERR_AAC_NONE || !d.track.seekToSample(0)) {
    return false;
  }
  AACFlushCodec(d.aac);
  d.sbrReady = true;
  d.sbrMissing = false;
  d.badFrames = 0;
  return true;
}

DecodeStatus M4aDecoder::firstFrame(Impl& d) {
  DecodeStatus st = DecodeStatus::Ok;
  do {
    st = decodeFrame(d);
  } while (st == DecodeStatus::Ok && d.pcmFrames == 0);
  return st;
}

DecodeStatus M4aDecoder::decodeFrame(Impl& d) {
  d.pcmFrames = 0;
  d.pcmOffset = 0;
  if (d.track.atEnd()) return DecodeStatus::Finished;

  const Mp4Cursor c = d.track.cursor();
  uint8_t* src = sampleBytes(d, c.offset, c.size);
  if (!src || !d.track.advance()) return DecodeStatus::Error;

  int left = static_cast<int>(c.size);
  const int err = AACDecode(d.aac, &src, &left, d.pcm);
  if (err == ERR_AAC_SBR_BITSTREAM && !d.sbrReady) {
    d.sbrMissing = true;
    return DecodeStatus::Error;
  }
  if (err != ERR_AAC_NONE) {
    return ++d.badFrames > kMaxBadFrames ? DecodeStatus::Error : DecodeStatus::Ok;
  }
  d.badFrames = 0;

  AACFrameInfo info{};
  AACGetLastFrameInfo(d.aac, &info);
  if (info.nChans < 1 || info.nChans > 2 || info.outputSamps <= 0) return DecodeStatus::Error;
  d.pcmChannels = static_cast<uint8_t>(info.nChans);
  d.pcmFrames = static_cast<size_t>(info.outputSamps / info.nChans);
  d.pcmStartTicks = c.dts;
  return DecodeStatus::Ok;
}

bool M4aDecoder::openInputs(DecoderInput* data, DecoderInput* meta, DecoderInput* sizes) {
  close();
  auto* p = new (std::nothrow) Impl();
  if (!p) return false;
  p->data = data;
  p->meta = meta;
  p->sizes = sizes;
  impl_ = p;

  if (!p->track.open(meta, sizes)) {
    close();
    return false;
  }
  const Mp4AudioConfig& cfg = p->track.config();
  p->aac = AACInitDecoder();
  AACFrameInfo raw{};
  raw.nChans = cfg.channels;
  raw.sampRateCore = static_cast<int>(cfg.coreSampleRate);
  raw.profile = AAC_PROFILE_LC;
  if (!p->aac || AACSetRawBlockParams(p->aac, 0, &raw) != ERR_AAC_NONE ||
      (cfg.sbr && !enableSbr(*p))) {
    close();
    return false;
  }

  // Decode the first frame to learn the output rate. SBR doubles the core rate.
  // Implicit HE-AAC shows its SBR data only in the frames.
  DecodeStatus st = firstFrame(*p);
  if (st == DecodeStatus::Error && p->sbrMissing) {
    if (!enableSbr(*p)) {
      close();
      return false;
    }
    st = firstFrame(*p);
  }
  AACFrameInfo info{};
  AACGetLastFrameInfo(p->aac, &info);
  if (st != DecodeStatus::Ok || info.sampRateOut <= 0) {
    close();
    return false;
  }

  p->outRate = static_cast<uint32_t>(info.sampRateOut);
  p->fmt.sampleRate = p->outRate;
  p->fmt.channels = 2;
  p->fmt.sourceChannels = cfg.channels;
  p->fmt.bitsPerSample = 16;
  p->fmt.durationMs = p->ticksToMs(p->track.durationTicks());
  p->fmt.durationEstimated = false;
  return true;
}

#ifdef UNIT_TEST
bool M4aDecoder::openWithInputs(DecoderInput& data, DecoderInput& meta, DecoderInput& sizes) {
  return openInputs(&data, &meta, &sizes);
}

bool M4aDecoder::open(const char* path) {
  if (!path) return false;
  DecoderInput* in[3] = {};
  bool ok = true;
  for (auto*& slot : in) {
    auto* f = new (std::nothrow) Mp3TestFile();
    slot = f;
    ok = ok && f && f->openFile(path);
  }
  if (!ok || !openInputs(in[0], in[1], in[2])) {
    for (auto* slot : in) delete slot;
    return false;
  }
  impl_->ownsInputs = true;
  return true;
}
#else
bool M4aDecoder::open(const char* path) {
  if (!path) return false;
  SdInput* in[3] = {};
  bool ok = true;
  for (auto*& slot : in) {
    slot = new (std::nothrow) SdInput();
    if (slot) slot->file = SD.open(path, FILE_READ);
    ok = ok && slot && slot->valid();
  }
  if (!ok || !openInputs(in[0], in[1], in[2])) {
    for (auto* slot : in) {
      if (slot) slot->close();
      delete slot;
    }
    return false;
  }
  impl_->ownsInputs = true;
  return true;
}
#endif

void M4aDecoder::close() {
  if (!impl_) return;
  if (impl_->aac) AACFreeDecoder(impl_->aac);
  if (impl_->ownsInputs) {
    for (DecoderInput* in : {impl_->data, impl_->meta, impl_->sizes}) {
      in->close();
      delete in;
    }
  }
  delete impl_;
  impl_ = nullptr;
}

AudioFormat M4aDecoder::format() const {
  return impl_ ? impl_->fmt : AudioFormat{};
}

DecodeStatus M4aDecoder::decode(int16_t* outStereo, size_t maxFrames, size_t* gotFrames) {
  *gotFrames = 0;
  if (!impl_) return DecodeStatus::Error;
  Impl& d = *impl_;
  size_t produced = 0;
  while (produced < maxFrames) {
    if (d.pcmOffset >= d.pcmFrames) {
      const DecodeStatus st = decodeFrame(d);
      if (st != DecodeStatus::Ok) {
        *gotFrames = produced;
        return st;
      }
      continue;
    }
    size_t n = d.pcmFrames - d.pcmOffset;
    if (n > maxFrames - produced) n = maxFrames - produced;
    int16_t* out = outStereo + produced * 2;
    if (d.pcmChannels == 1) {
      const int16_t* in = d.pcm + d.pcmOffset;
      for (size_t i = 0; i < n; ++i) {
        out[2 * i] = in[i];
        out[2 * i + 1] = in[i];
      }
    } else {
      std::memcpy(out, d.pcm + d.pcmOffset * 2, n * 2 * sizeof(int16_t));
    }
    produced += n;
    d.pcmOffset += n;
  }
  *gotFrames = produced;
  return DecodeStatus::Ok;
}

bool M4aDecoder::seekMs(uint32_t ms) {
  if (!impl_) return false;
  Impl& d = *impl_;
  const uint64_t ticks = static_cast<uint64_t>(ms) * d.track.timescale() / 1000u;
  uint32_t target = 0;
  if (!d.track.sampleAtTicks(ticks, &target)) return false;

  // Decode one frame before the target. AAC output needs the previous frame's overlap.
  const Mp4Cursor saved = d.track.cursor();
  const uint32_t preroll = target > 0 ? target - 1 : 0;
  if (!d.track.seekToSample(preroll)) {
    d.track.restore(saved);
    return false;
  }
  AACFlushCodec(d.aac);
  d.pcmFrames = 0;
  d.pcmOffset = 0;
  d.badFrames = 0;
  if (target > 0 && !d.track.atEnd()) {
    if (decodeFrame(d) == DecodeStatus::Error) {
      d.track.restore(saved);
      return false;
    }
    d.pcmFrames = 0;
    d.pcmOffset = 0;
  }
  return true;
}

uint32_t M4aDecoder::positionMs() const {
  if (!impl_) return 0;
  const Impl& d = *impl_;
  if (d.pcmOffset < d.pcmFrames) {
    return d.ticksToMs(d.pcmStartTicks) +
           static_cast<uint32_t>(static_cast<uint64_t>(d.pcmOffset) * 1000u / d.outRate);
  }
  return d.ticksToMs(d.track.cursor().dts);
}
