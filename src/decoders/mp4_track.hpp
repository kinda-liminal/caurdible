#pragma once

#include "decoder_input.hpp"

#include <cstddef>
#include <cstdint>

// Reads big-endian entries of one sample table through a one-page cache.
// Tables stay on the SD card. Memory use does not depend on the file length.
class Mp4Table {
 public:
  static constexpr size_t kPageBytes = 1024;

  void init(DecoderInput* in, uint32_t offset, uint32_t count, uint8_t entrySize);
  uint32_t count() const { return count_; }
  bool word(uint32_t index, uint8_t wordIndex, uint32_t* out);
  bool dword(uint32_t index, uint64_t* out);

 private:
  bool load(uint32_t index);
  const uint8_t* entry(uint32_t index);

  DecoderInput* in_ = nullptr;
  uint32_t offset_ = 0;
  uint32_t count_ = 0;
  uint8_t entrySize_ = 0;
  uint32_t pageFirst_ = 0;
  uint32_t pageEntries_ = 0;
  uint8_t page_[kPageBytes]{};
};

struct Mp4AudioConfig {
  uint8_t objectType = 0;  // core audio object type; 2 is AAC LC
  uint32_t coreSampleRate = 0;
  uint8_t channels = 0;
  bool sbr = false;  // explicit HE-AAC signalling
};

// Position of the next sample to read.
struct Mp4Cursor {
  uint32_t sample = 0;
  uint32_t chunk = 0;  // zero-based
  uint32_t inChunk = 0;
  uint32_t chunkSamples = 0;
  uint32_t stscIndex = 0;
  uint32_t stscNextFirst = 0;  // one-based first chunk of the next stsc entry
  uint32_t sttsIndex = 0;
  uint32_t sttsLeft = 0;
  uint32_t sttsDelta = 0;
  uint32_t offset = 0;
  uint32_t size = 0;
  uint64_t dts = 0;
};

// The first AAC audio track of an MP4, M4A, or M4B file.
// `meta` reads boxes, stts, stsc, and chunk offsets. `sizes` reads stsz.
// Separate inputs keep each file position moving forward during playback.
class Mp4Track {
 public:
  bool open(DecoderInput* meta, DecoderInput* sizes);

  const Mp4AudioConfig& config() const { return config_; }
  uint32_t timescale() const { return timescale_; }
  uint64_t durationTicks() const { return durationTicks_; }
  uint32_t sampleCount() const { return sampleCount_; }

  const Mp4Cursor& cursor() const { return cur_; }
  void restore(const Mp4Cursor& c) { cur_ = c; }
  bool atEnd() const { return cur_.sample >= sampleCount_; }

  bool seekToSample(uint32_t sample);
  bool advance();
  // First sample whose time span contains ticks. Returns sampleCount past the end.
  bool sampleAtTicks(uint64_t ticks, uint32_t* sample);

 private:
  bool parseMoov(uint64_t start, uint64_t end);
  bool parseTrak(uint64_t start, uint64_t end);
  bool parseStsd(uint64_t start, uint64_t end);
  bool parseEsds(uint64_t start, uint64_t end);
  bool sampleSize(uint32_t sample, uint32_t* out);
  bool chunkOffset(uint32_t chunk, uint32_t* out);
  bool loadStsc(uint32_t index, uint32_t* spc, uint32_t* nextFirst);

  DecoderInput* meta_ = nullptr;
  DecoderInput* sizes_ = nullptr;
  Mp4AudioConfig config_{};
  uint32_t timescale_ = 0;
  uint64_t durationTicks_ = 0;
  uint32_t sampleCount_ = 0;
  uint32_t fixedSampleSize_ = 0;
  uint32_t chunkCount_ = 0;
  bool co64_ = false;
  bool found_ = false;
  Mp4Table stts_;
  Mp4Table stsc_;
  Mp4Table stsz_;
  Mp4Table stco_;
  Mp4Cursor cur_{};
};
