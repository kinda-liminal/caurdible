#include "mp4_track.hpp"

#include <cstring>

namespace {

constexpr uint32_t fourcc(const char (&s)[5]) {
  return (static_cast<uint32_t>(static_cast<uint8_t>(s[0])) << 24) |
         (static_cast<uint32_t>(static_cast<uint8_t>(s[1])) << 16) |
         (static_cast<uint32_t>(static_cast<uint8_t>(s[2])) << 8) |
         static_cast<uint32_t>(static_cast<uint8_t>(s[3]));
}

uint16_t be16(const uint8_t* p) {
  return static_cast<uint16_t>((p[0] << 8) | p[1]);
}

uint32_t be32(const uint8_t* p) {
  return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
         (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}

uint64_t be64(const uint8_t* p) {
  return (static_cast<uint64_t>(be32(p)) << 32) | be32(p + 4);
}

bool readAt(DecoderInput& in, uint64_t pos, uint8_t* dst, size_t n) {
  if (pos > UINT32_MAX || !in.seek(static_cast<uint32_t>(pos))) return false;
  return in.read(dst, n) == n;
}

struct Box {
  uint32_t type = 0;
  uint64_t payload = 0;
  uint64_t end = 0;
};

bool readBox(DecoderInput& in, uint64_t pos, uint64_t limit, Box* b) {
  uint8_t h[16];
  if (pos + 8 > limit || !readAt(in, pos, h, 8)) return false;
  uint64_t size = be32(h);
  b->type = be32(h + 4);
  b->payload = pos + 8;
  if (size == 1) {
    if (pos + 16 > limit || in.read(h + 8, 8) != 8) return false;
    size = be64(h + 8);
    b->payload = pos + 16;
  } else if (size == 0) {
    size = limit - pos;
  }
  if (size < b->payload - pos || size > limit - pos) return false;
  b->end = pos + size;
  return true;
}

bool findChild(DecoderInput& in, uint64_t start, uint64_t end, uint32_t type, Box* out) {
  Box b;
  for (uint64_t pos = start; readBox(in, pos, end, &b); pos = b.end) {
    if (b.type == type) {
      *out = b;
      return true;
    }
  }
  return false;
}

struct TableRef {
  uint32_t offset = 0;
  uint32_t count = 0;
};

// Reads the entry count of a full box table and checks that the entries fit.
bool tableRef(DecoderInput& in, const Box& b, uint32_t headerBytes, uint8_t entrySize,
              TableRef* out) {
  uint8_t h[12];
  if (headerBytes > sizeof(h) || b.payload + headerBytes > b.end ||
      !readAt(in, b.payload, h, headerBytes)) {
    return false;
  }
  const uint32_t count = be32(h + headerBytes - 4);
  const uint64_t first = b.payload + headerBytes;
  if (static_cast<uint64_t>(count) * entrySize > b.end - first || first > UINT32_MAX) {
    return false;
  }
  out->offset = static_cast<uint32_t>(first);
  out->count = count;
  return true;
}

class BitReader {
 public:
  BitReader(const uint8_t* data, size_t size) : data_(data), size_(size) {}
  uint32_t bits(uint8_t n) {
    uint32_t v = 0;
    for (uint8_t i = 0; i < n; ++i) {
      const size_t byte = pos_ >> 3;
      if (byte >= size_) {
        overrun_ = true;
        return 0;
      }
      v = (v << 1) | ((data_[byte] >> (7 - (pos_ & 7))) & 1u);
      ++pos_;
    }
    return v;
  }
  bool overrun() const { return overrun_; }

 private:
  const uint8_t* data_;
  size_t size_;
  size_t pos_ = 0;
  bool overrun_ = false;
};

constexpr uint32_t kAacRates[] = {96000, 88200, 64000, 48000, 44100, 32000, 24000,
                                  22050, 16000, 12000, 11025, 8000,  7350};

uint32_t readObjectType(BitReader& br) {
  uint32_t aot = br.bits(5);
  if (aot == 31) aot = 32 + br.bits(6);
  return aot;
}

uint32_t readRate(BitReader& br) {
  const uint32_t index = br.bits(4);
  if (index == 15) return br.bits(24);
  return index < sizeof(kAacRates) / sizeof(kAacRates[0]) ? kAacRates[index] : 0;
}

// ISO/IEC 14496-1 descriptor header: one tag byte and up to four 7-bit length bytes.
bool readDescriptor(const uint8_t* buf, size_t n, size_t* p, uint8_t* tag, uint32_t* len) {
  if (*p >= n) return false;
  *tag = buf[(*p)++];
  *len = 0;
  for (int i = 0; i < 4; ++i) {
    if (*p >= n) return false;
    const uint8_t b = buf[(*p)++];
    *len = (*len << 7) | (b & 0x7Fu);
    if ((b & 0x80u) == 0) break;
  }
  return true;
}

}  // namespace

void Mp4Table::init(DecoderInput* in, uint32_t offset, uint32_t count, uint8_t entrySize) {
  in_ = in;
  offset_ = offset;
  count_ = count;
  entrySize_ = entrySize;
  pageFirst_ = 0;
  pageEntries_ = 0;
}

bool Mp4Table::load(uint32_t index) {
  const uint32_t perPage = static_cast<uint32_t>(kPageBytes / entrySize_);
  const uint32_t first = count_ <= perPage ? 0 : index;
  const uint32_t n = count_ - first < perPage ? count_ - first : perPage;
  const size_t bytes = static_cast<size_t>(n) * entrySize_;
  const uint64_t pos = offset_ + static_cast<uint64_t>(first) * entrySize_;
  if (pos > UINT32_MAX || !in_->seek(static_cast<uint32_t>(pos)) ||
      in_->read(page_, bytes) != bytes) {
    pageEntries_ = 0;
    return false;
  }
  pageFirst_ = first;
  pageEntries_ = n;
  return true;
}

const uint8_t* Mp4Table::entry(uint32_t index) {
  if (!in_ || index >= count_) return nullptr;
  if (index < pageFirst_ || index - pageFirst_ >= pageEntries_) {
    if (!load(index)) return nullptr;
  }
  return page_ + static_cast<size_t>(index - pageFirst_) * entrySize_;
}

bool Mp4Table::word(uint32_t index, uint8_t wordIndex, uint32_t* out) {
  if ((wordIndex + 1u) * 4u > entrySize_) return false;
  const uint8_t* p = entry(index);
  if (!p) return false;
  *out = be32(p + wordIndex * 4u);
  return true;
}

bool Mp4Table::dword(uint32_t index, uint64_t* out) {
  if (entrySize_ < 8) return false;
  const uint8_t* p = entry(index);
  if (!p) return false;
  *out = be64(p);
  return true;
}

bool Mp4Track::open(DecoderInput* meta, DecoderInput* sizes) {
  if (!meta || !sizes) return false;
  meta_ = meta;
  sizes_ = sizes;
  const uint64_t fileEnd = meta->size();
  Box b;
  for (uint64_t pos = 0; !found_ && readBox(*meta, pos, fileEnd, &b); pos = b.end) {
    if (b.type == fourcc("moov")) {
      parseMoov(b.payload, b.end);
      break;
    }
  }
  return found_ && seekToSample(0);
}

bool Mp4Track::parseMoov(uint64_t start, uint64_t end) {
  Box b;
  for (uint64_t pos = start; readBox(*meta_, pos, end, &b); pos = b.end) {
    if (b.type == fourcc("trak") && parseTrak(b.payload, b.end)) {
      found_ = true;
      return true;
    }
  }
  return false;
}

bool Mp4Track::parseTrak(uint64_t start, uint64_t end) {
  DecoderInput& in = *meta_;
  Box mdia, hdlr, mdhd, minf, stbl;
  if (!findChild(in, start, end, fourcc("mdia"), &mdia) ||
      !findChild(in, mdia.payload, mdia.end, fourcc("hdlr"), &hdlr) ||
      !findChild(in, mdia.payload, mdia.end, fourcc("mdhd"), &mdhd) ||
      !findChild(in, mdia.payload, mdia.end, fourcc("minf"), &minf) ||
      !findChild(in, minf.payload, minf.end, fourcc("stbl"), &stbl)) {
    return false;
  }

  uint8_t h[32];
  if (hdlr.payload + 12 > hdlr.end || !readAt(in, hdlr.payload, h, 12) ||
      be32(h + 8) != fourcc("soun")) {
    return false;
  }

  const size_t mdhdBytes = mdhd.end - mdhd.payload < sizeof(h)
                               ? static_cast<size_t>(mdhd.end - mdhd.payload)
                               : sizeof(h);
  if (mdhdBytes < 20 || !readAt(in, mdhd.payload, h, mdhdBytes)) return false;
  uint32_t timescale = 0;
  uint64_t duration = 0;
  if (h[0] == 1) {
    if (mdhdBytes < 32) return false;
    timescale = be32(h + 20);
    duration = be64(h + 24);
  } else {
    timescale = be32(h + 12);
    duration = be32(h + 16);
    if (duration == UINT32_MAX) duration = 0;
  }
  if (timescale == 0) return false;

  Box stsd, stts, stsc, stsz, stco;
  bool co64 = false;
  if (!findChild(in, stbl.payload, stbl.end, fourcc("stsd"), &stsd) ||
      !findChild(in, stbl.payload, stbl.end, fourcc("stts"), &stts) ||
      !findChild(in, stbl.payload, stbl.end, fourcc("stsc"), &stsc) ||
      !findChild(in, stbl.payload, stbl.end, fourcc("stsz"), &stsz)) {
    return false;
  }
  if (!findChild(in, stbl.payload, stbl.end, fourcc("stco"), &stco)) {
    if (!findChild(in, stbl.payload, stbl.end, fourcc("co64"), &stco)) return false;
    co64 = true;
  }
  if (!parseStsd(stsd.payload, stsd.end)) return false;

  TableRef sttsRef, stscRef, stcoRef, stszRef;
  if (!tableRef(in, stts, 8, 8, &sttsRef) || !tableRef(in, stsc, 8, 12, &stscRef) ||
      !tableRef(in, stco, 8, co64 ? 8 : 4, &stcoRef)) {
    return false;
  }
  if (stsz.payload + 12 > stsz.end || !readAt(in, stsz.payload, h, 12)) return false;
  const uint32_t fixedSize = be32(h + 4);
  if (fixedSize == 0) {
    if (!tableRef(in, stsz, 12, 4, &stszRef)) return false;
  } else {
    stszRef.count = be32(h + 8);
  }
  if (stszRef.count == 0 || sttsRef.count == 0 || stscRef.count == 0 || stcoRef.count == 0) {
    return false;
  }

  stts_.init(meta_, sttsRef.offset, sttsRef.count, 8);
  stsc_.init(meta_, stscRef.offset, stscRef.count, 12);
  stco_.init(meta_, stcoRef.offset, stcoRef.count, co64 ? 8 : 4);
  stsz_.init(sizes_, stszRef.offset, fixedSize == 0 ? stszRef.count : 0, 4);
  timescale_ = timescale;
  sampleCount_ = stszRef.count;
  fixedSampleSize_ = fixedSize;
  chunkCount_ = stcoRef.count;
  co64_ = co64;

  if (duration == 0) {
    for (uint32_t i = 0; i < stts_.count(); ++i) {
      uint32_t count = 0, delta = 0;
      if (!stts_.word(i, 0, &count) || !stts_.word(i, 1, &delta)) return false;
      duration += static_cast<uint64_t>(count) * delta;
    }
  }
  durationTicks_ = duration;
  return true;
}

bool Mp4Track::parseStsd(uint64_t start, uint64_t end) {
  DecoderInput& in = *meta_;
  Box entry;
  if (!readBox(in, start + 8, end, &entry) || entry.type != fourcc("mp4a")) return false;
  uint8_t h[28];
  if (entry.payload + sizeof(h) > entry.end || !readAt(in, entry.payload, h, sizeof(h))) {
    return false;
  }
  const uint16_t version = be16(h + 8);
  const uint16_t channelCount = be16(h + 16);
  uint64_t children = entry.payload + sizeof(h);
  if (version == 1) children += 16;
  if (version == 2) children += 36;

  Box esds, wave;
  if (!findChild(in, children, entry.end, fourcc("esds"), &esds)) {
    if (!findChild(in, children, entry.end, fourcc("wave"), &wave) ||
        !findChild(in, wave.payload, wave.end, fourcc("esds"), &esds)) {
      return false;
    }
  }
  if (!parseEsds(esds.payload, esds.end)) return false;
  if (config_.channels == 0) config_.channels = static_cast<uint8_t>(channelCount);
  return config_.channels == 1 || config_.channels == 2;
}

bool Mp4Track::parseEsds(uint64_t start, uint64_t end) {
  uint8_t buf[128];
  const size_t n = end - start < sizeof(buf) ? static_cast<size_t>(end - start) : sizeof(buf);
  if (n < 4 || !readAt(*meta_, start, buf, n)) return false;

  size_t p = 4;
  uint8_t tag = 0;
  uint32_t len = 0;
  if (!readDescriptor(buf, n, &p, &tag, &len)) return false;
  if (tag == 0x03) {
    p += 2;
    if (p >= n) return false;
    const uint8_t flags = buf[p++];
    if (flags & 0x80u) p += 2;
    if ((flags & 0x40u) && p < n) p += 1u + buf[p];
    if (flags & 0x20u) p += 2;
    if (!readDescriptor(buf, n, &p, &tag, &len)) return false;
  }
  if (tag != 0x04 || p + 13 > n) return false;
  const uint8_t objectTypeIndication = buf[p];
  if (objectTypeIndication != 0x40 && (objectTypeIndication < 0x66 || objectTypeIndication > 0x68)) {
    return false;
  }
  p += 13;
  if (!readDescriptor(buf, n, &p, &tag, &len) || tag != 0x05 || len < 2 || p + len > n) {
    return false;
  }

  BitReader br(buf + p, len);
  uint32_t aot = readObjectType(br);
  const uint32_t rate = readRate(br);
  const uint32_t channels = br.bits(4);
  bool sbr = false;
  if (aot == 5 || aot == 29) {
    sbr = true;
    readRate(br);
    aot = readObjectType(br);
  }
  if (objectTypeIndication != 0x40) aot = 2;
  if (br.overrun() || aot != 2 || rate == 0) return false;

  config_.objectType = static_cast<uint8_t>(aot);
  config_.coreSampleRate = rate;
  config_.channels = static_cast<uint8_t>(channels);
  config_.sbr = sbr;
  return true;
}

bool Mp4Track::sampleSize(uint32_t sample, uint32_t* out) {
  if (fixedSampleSize_ != 0) {
    *out = fixedSampleSize_;
    return true;
  }
  return stsz_.word(sample, 0, out);
}

bool Mp4Track::chunkOffset(uint32_t chunk, uint32_t* out) {
  if (!co64_) return stco_.word(chunk, 0, out);
  uint64_t v = 0;
  if (!stco_.dword(chunk, &v) || v > UINT32_MAX) return false;
  *out = static_cast<uint32_t>(v);
  return true;
}

bool Mp4Track::loadStsc(uint32_t index, uint32_t* spc, uint32_t* nextFirst) {
  uint32_t first = 0;
  if (!stsc_.word(index, 0, &first) || !stsc_.word(index, 1, spc) || *spc == 0) return false;
  if (index + 1 < stsc_.count()) {
    if (!stsc_.word(index + 1, 0, nextFirst)) return false;
  } else {
    *nextFirst = chunkCount_ + 1;
  }
  return first != 0 && *nextFirst > first;
}

bool Mp4Track::seekToSample(uint32_t sample) {
  Mp4Cursor c;
  if (sample >= sampleCount_) {
    c.sample = sampleCount_;
    c.dts = durationTicks_;
    cur_ = c;
    return true;
  }
  c.sample = sample;

  uint64_t base = 0;
  uint64_t ticks = 0;
  bool timed = false;
  for (uint32_t i = 0; i < stts_.count(); ++i) {
    uint32_t count = 0, delta = 0;
    if (!stts_.word(i, 0, &count) || !stts_.word(i, 1, &delta)) return false;
    if (sample < base + count) {
      c.sttsIndex = i;
      c.sttsLeft = static_cast<uint32_t>(base + count - sample);
      c.sttsDelta = delta;
      c.dts = ticks + (sample - base) * delta;
      timed = true;
      break;
    }
    base += count;
    ticks += static_cast<uint64_t>(count) * delta;
  }
  if (!timed) return false;

  base = 0;
  bool placed = false;
  for (uint32_t i = 0; i < stsc_.count(); ++i) {
    uint32_t first = 0, spc = 0, nextFirst = 0;
    if (!stsc_.word(i, 0, &first) || !loadStsc(i, &spc, &nextFirst)) return false;
    const uint64_t run = static_cast<uint64_t>(nextFirst - first) * spc;
    if (sample < base + run) {
      const uint64_t rel = sample - base;
      c.chunk = static_cast<uint32_t>(first - 1 + rel / spc);
      c.inChunk = static_cast<uint32_t>(rel % spc);
      c.chunkSamples = spc;
      c.stscIndex = i;
      c.stscNextFirst = nextFirst;
      placed = true;
      break;
    }
    base += run;
  }
  if (!placed || c.chunk >= chunkCount_) return false;

  uint32_t offset = 0;
  if (!chunkOffset(c.chunk, &offset)) return false;
  uint64_t pos = offset;
  for (uint32_t k = sample - c.inChunk; k < sample; ++k) {
    uint32_t size = 0;
    if (!sampleSize(k, &size)) return false;
    pos += size;
  }
  if (pos > UINT32_MAX || !sampleSize(sample, &c.size)) return false;
  c.offset = static_cast<uint32_t>(pos);
  cur_ = c;
  return true;
}

bool Mp4Track::advance() {
  if (cur_.sample >= sampleCount_) return true;
  Mp4Cursor c = cur_;
  ++c.sample;
  c.dts += c.sttsDelta;
  if (c.sample >= sampleCount_) {
    cur_ = c;
    return true;
  }

  if (--c.sttsLeft == 0) {
    uint32_t count = 0;
    do {
      if (++c.sttsIndex >= stts_.count() || !stts_.word(c.sttsIndex, 0, &count) ||
          !stts_.word(c.sttsIndex, 1, &c.sttsDelta)) {
        return false;
      }
    } while (count == 0);
    c.sttsLeft = count;
  }

  const uint64_t next = static_cast<uint64_t>(c.offset) + c.size;
  if (next > UINT32_MAX) return false;
  c.offset = static_cast<uint32_t>(next);
  if (++c.inChunk >= c.chunkSamples) {
    ++c.chunk;
    c.inChunk = 0;
    if (c.chunk + 1 == c.stscNextFirst &&
        !loadStsc(++c.stscIndex, &c.chunkSamples, &c.stscNextFirst)) {
      return false;
    }
    if (!chunkOffset(c.chunk, &c.offset)) return false;
  }
  if (!sampleSize(c.sample, &c.size)) return false;
  cur_ = c;
  return true;
}

bool Mp4Track::sampleAtTicks(uint64_t ticks, uint32_t* sample) {
  uint64_t base = 0;
  uint64_t t = 0;
  for (uint32_t i = 0; i < stts_.count(); ++i) {
    uint32_t count = 0, delta = 0;
    if (!stts_.word(i, 0, &count) || !stts_.word(i, 1, &delta)) return false;
    const uint64_t span = static_cast<uint64_t>(count) * delta;
    if (ticks < t + span) {
      const uint64_t s = base + (delta ? (ticks - t) / delta : 0);
      *sample = s < sampleCount_ ? static_cast<uint32_t>(s) : sampleCount_;
      return true;
    }
    t += span;
    base += count;
  }
  *sample = sampleCount_;
  return true;
}
