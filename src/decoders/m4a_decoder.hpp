#pragma once

#include "decoder.hpp"
#include "decoder_input.hpp"
#include <cstdint>

// AAC-LC and HE-AAC in MP4, M4A, and M4B files. The Helix decoder produces PCM.
class M4aDecoder : public IDecoder {
 public:
  bool open(const char* path) override;
  void close() override;
  ~M4aDecoder() { close(); }
  AudioFormat format() const override;
  DecodeStatus decode(int16_t* outStereo, size_t maxFrames, size_t* gotFrames) override;
  bool seekMs(uint32_t ms) override;
  uint32_t positionMs() const override;

#ifdef UNIT_TEST
  // Test seam: the caller keeps ownership of the three inputs.
  bool openWithInputs(DecoderInput& data, DecoderInput& meta, DecoderInput& sizes);
#endif

 private:
  struct Impl;
  bool openInputs(DecoderInput* data, DecoderInput* meta, DecoderInput* sizes);
  static bool enableSbr(Impl& d);
  static DecodeStatus firstFrame(Impl& d);
  static DecodeStatus decodeFrame(Impl& d);
  static uint8_t* sampleBytes(Impl& d, uint32_t offset, uint32_t size);
  Impl* impl_ = nullptr;
};
