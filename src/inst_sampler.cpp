// inst_sampler.cpp — SD Card WAV Sampler Instrument Engine (4-Octave Keyboard C3..B6)
#include "inst_sampler.h"
#include "storage.h"
#include "paged_dir_scanner.h"
#include "ui.h"
#include <Arduino.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>

namespace looper {

static constexpr const char *SAMPLES_DIR = "/Capstan/SAMPLES";
static constexpr size_t SAMPLES_DIR_PREFIX_LEN = 17; // strlen("/Capstan/SAMPLES/")
static constexpr int NUM_SAMPLER_DIRS = 12;

PROGMEM static const char *const SAMPLER_PRESS_NAMES[NUM_SAMPLER_PRESS_MODES] = {
  "STOP", "ONCE", "REP", "REPTHRU"
};

PROGMEM static const char *const SAMPLER_KEY_NAMES[SAMPLER_NUM_KEYS] = {
  "C3",  "C#3", "D3",  "D#3", "E3",  "F3",  "F#3", "G3",  "G#3", "A3",  "A#3", "B3",
  "C4",  "C#4", "D4",  "D#4", "E4",  "F4",  "F#4", "G4",  "G#4", "A4",  "A#4", "B4",
  "C5",  "C#5", "D5",  "D#5", "E5",  "F5",  "F#5", "G5",  "G#5", "A5",  "A#5", "B5",
  "C6",  "C#6", "D6",  "D#6", "E6",  "F6",  "F#6", "G6",  "G#6", "A6",  "A#6", "B6"
};

PROGMEM static const char *const SAMPLER_KEY_SHORT[SAMPLER_NUM_KEYS] = {
  "C3", "C#", "D3", "D#", "E3", "F3", "F#", "G3", "G#", "A3", "A#", "B3",
  "C4", "C#", "D4", "D#", "E4", "F4", "F#", "G4", "G#", "A4", "A#", "B4",
  "C5", "C#", "D5", "D#", "E5", "F5", "F#", "G5", "G#", "A5", "A#", "B5",
  "C6", "C#", "D6", "D#", "E6", "F6", "F#", "G6", "G#", "A6", "A#", "B6"
};

PROGMEM static const uint8_t SAMPLER_BLACK_KEY[12] = {
  0, 1, 0, 1, 0, 0, 1, 0, 1, 0, 1, 0
};

PROGMEM static const FmPatch SAMPLER_DEFAULT_PATCH = {
  "SMP_01", (uint8_t)InstType::Sampler, 0, (uint8_t)SamplerPressMode::Once,
  { { 0, 100, 100, 0, {0, 0, 0} },
    { 0, 100, 100, 0, {0, 0, 0} } },
  { 127,  0,   0, 127,  0 },  // AMP: wide-open instant attack/sustain, 4ms de-click release
  { 127,  0,   0, 127, 127 }, // FLT: wide-open bypass unless shaped in General View
  {   0,  0,   0,   0,  0 },  // FX: dry by default
  0
};

// Shared directory pool and per-instrument, per-key directory index & filename
// (0xFF = "/Capstan/SAMPLES", 0..NUM_SAMPLER_DIRS-1 = custom directory path in samplerDirs[])
static char samplerDirs[NUM_SAMPLER_DIRS][SAMPLER_DIR_MAX];
static uint8_t samplerKeyDirIdx[NUM_INSTRUMENTS][SAMPLER_NUM_KEYS];
DMAMEM static char samplerKeyFile[NUM_INSTRUMENTS][SAMPLER_NUM_KEYS][SAMPLER_FILE_MAX];
static uint64_t samplerPopulatedMask[NUM_INSTRUMENTS] = {};
static uint64_t samplerFailedMask[NUM_INSTRUMENTS] = {};

static constexpr const char *CLIP_SAMPLER_PATH = "/Capstan/_CLIPSMP.TMP";
static int8_t clipboardSamplerSrcSlot = -1; // -1 = none, 0..7 = live slot, -2 = spilled to CLIP_SAMPLER_PATH

// ============================================================================
// Lock-Free SD WAV & SF2 Streaming Pool (8 concurrent streams, 32 KB total ring)
// ============================================================================
static constexpr int NUM_SAMPLER_STREAMS = 8;
static constexpr uint32_t SAMPLER_RING_FRAMES = 1024;
static constexpr uint32_t SAMPLER_RING_MASK = SAMPLER_RING_FRAMES - 1;

static SamplerStream samplerStreams[NUM_SAMPLER_STREAMS];
static uint32_t streamGenCounter = 1;
static volatile int8_t fillingStreamIdx = -1;

SamplerStream *samplerStreamSlot(uint32_t streamIdx) {
  return (streamIdx < (uint32_t)NUM_SAMPLER_STREAMS) ? &samplerStreams[streamIdx] : nullptr;
}

// ============================================================================
// Sample Browser Modal State (32-Entry Sliding Window)
// ============================================================================
static constexpr int MAX_BROWSER_ENTRIES = 32;
static SamplerBrowserEntry *const browserEntries = reinterpret_cast<SamplerBrowserEntry *>(sharedBrowserPool);
static int browserEntryCount = 0;
static int browserWindowStart = 0;
static int browserWindowCount = 0;
static int browserCursor = 0;
static int browserScroll = 0;
static int browserInstIdx = 0;
static int browserKeyIdx = 0;
static bool browserOpen = false;
static char browserDir[SAMPLER_DIR_MAX] = "/Capstan";

// ============================================================================
// Basic Queries & Path Helpers
// ============================================================================
FLASHMEM const char *samplerPressModeName(uint8_t mode) {
  return SAMPLER_PRESS_NAMES[constrain((int)mode, 0, NUM_SAMPLER_PRESS_MODES - 1)];
}

FLASHMEM const char *samplerKeyNoteName(int keyIdx) {
  return SAMPLER_KEY_NAMES[constrain(keyIdx, 0, SAMPLER_NUM_KEYS - 1)];
}

FLASHMEM const char *samplerKeyShortLabel(int keyIdx) {
  return SAMPLER_KEY_SHORT[constrain(keyIdx, 0, SAMPLER_NUM_KEYS - 1)];
}

FLASHMEM bool samplerIsBlackKey(int keyIdx) {
  if (keyIdx < 0 || keyIdx >= SAMPLER_NUM_KEYS) return false;
  return SAMPLER_BLACK_KEY[keyIdx % 12] != 0;
}

FLASHMEM int samplerNoteToKey(uint8_t midiNote) {
  int k = (int)midiNote - (int)SAMPLER_BASE_NOTE;
  if (k >= 0 && k < SAMPLER_NUM_KEYS) return k;
  return -1;
}

FLASHMEM bool samplerHasSample(int instIdx, int keyIdx) {
  if (instIdx < 0 || instIdx >= NUM_INSTRUMENTS) return false;
  if (keyIdx < 0 || keyIdx >= SAMPLER_NUM_KEYS) return false;
  return (samplerPopulatedMask[instIdx] & (1ULL << keyIdx)) != 0;
}

FLASHMEM uint64_t samplerKeyMask(int instIdx) {
  if (instIdx < 0 || instIdx >= NUM_INSTRUMENTS) return 0;
  return samplerPopulatedMask[instIdx];
}

FLASHMEM static void rebuildSamplerSlotMask(int instIdx) {
  if (instIdx < 0 || instIdx >= NUM_INSTRUMENTS) return;
  uint64_t m = 0;
  for (int k = 0; k < SAMPLER_NUM_KEYS; k++) {
    if (samplerKeyFile[instIdx][k][0] != '\0') {
      m |= (1ULL << k);
    }
  }
  samplerPopulatedMask[instIdx] = m;
  samplerFailedMask[instIdx] = 0;
}

FLASHMEM int samplerNoteToKeyForInst(int instIdx, uint8_t midiNote) {
  (void)instIdx;
  return samplerNoteToKey(midiNote);
}

FLASHMEM static bool isDirSlotReferenced(uint8_t dirSlot) {
  for (int i = 0; i < NUM_INSTRUMENTS; i++) {
    uint64_t m = samplerPopulatedMask[i];
    if (!m) continue;
    for (int k = 0; k < SAMPLER_NUM_KEYS; k++) {
      if ((m & (1ULL << k)) && samplerKeyDirIdx[i][k] == dirSlot) {
        return true;
      }
    }
  }
  return false;
}

static constexpr uint8_t SAMPLER_DIR_POOL_FULL = 0xFEu;

FLASHMEM static uint8_t allocOrFindSamplerDir(const char *dirPath) {
  if (!dirPath || !dirPath[0] || strcasecmp(dirPath, SAMPLES_DIR) == 0) {
    return 0xFFu;
  }
  for (int d = 0; d < NUM_SAMPLER_DIRS; d++) {
    if (samplerDirs[d][0] != '\0' && strcasecmp(samplerDirs[d], dirPath) == 0) {
      return (uint8_t)d;
    }
  }
  for (int d = 0; d < NUM_SAMPLER_DIRS; d++) {
    if (samplerDirs[d][0] == '\0') {
      snprintf(samplerDirs[d], SAMPLER_DIR_MAX, "%s", dirPath);
      return (uint8_t)d;
    }
  }
  for (int d = 0; d < NUM_SAMPLER_DIRS; d++) {
    if (!isDirSlotReferenced((uint8_t)d)) {
      snprintf(samplerDirs[d], SAMPLER_DIR_MAX, "%s", dirPath);
      return (uint8_t)d;
    }
  }
  return SAMPLER_DIR_POOL_FULL;
}

FLASHMEM static void resolveKeyFullPath(int instIdx, int keyIdx, char *outFull, size_t outSz) {
  if (!outFull || outSz == 0) return;
  if (!samplerHasSample(instIdx, keyIdx)) {
    outFull[0] = '\0';
    return;
  }
  const char *fName = samplerKeyFile[instIdx][keyIdx];
  if (fName[0] == '/') {
    snprintf(outFull, outSz, "%s", fName);
    return;
  }
  uint8_t dIdx = samplerKeyDirIdx[instIdx][keyIdx];
  const char *dir = (dIdx < NUM_SAMPLER_DIRS && samplerDirs[dIdx][0] != '\0')
                        ? samplerDirs[dIdx]
                        : SAMPLES_DIR;
  if (strcmp(dir, "/") == 0) {
    snprintf(outFull, outSz, "/%s", fName);
  } else {
    snprintf(outFull, outSz, "%s/%s", dir, fName);
  }
}

FLASHMEM const char *samplerSampleDisplayName(int instIdx, int keyIdx) {
  if (!samplerHasSample(instIdx, keyIdx)) return "---";
  const char *p = samplerKeyFile[instIdx][keyIdx];
  const char *slash = strrchr(p, '/');
  return slash ? (slash + 1) : p;
}

FLASHMEM bool samplerIsKeyPlaying(int instIdx, int keyIdx) {
  if (instIdx < 0 || instIdx >= NUM_INSTRUMENTS) return false;
  if (keyIdx < 0 || keyIdx >= SAMPLER_NUM_KEYS) return false;
  for (int s = 0; s < NUM_SAMPLER_STREAMS; s++) {
    if (samplerStreams[s].streamKind == (uint8_t)SharedStreamKind::WavSampler &&
        (samplerStreams[s].active || samplerStreams[s].pendingVoice) &&
        (int)samplerStreams[s].instIdx == instIdx &&
        (int)samplerStreams[s].keyIdx == keyIdx) {
      return true;
    }
  }
  return fmSynthIsNoteActive(instIdx, (uint8_t)(SAMPLER_BASE_NOTE + keyIdx));
}

FLASHMEM uint64_t samplerPlayingKeyMask(int instIdx) {
  if (instIdx < 0 || instIdx >= NUM_INSTRUMENTS) return 0;
  uint64_t mask = 0;
  for (int s = 0; s < NUM_SAMPLER_STREAMS; s++) {
    if (samplerStreams[s].streamKind == (uint8_t)SharedStreamKind::WavSampler &&
        (samplerStreams[s].active || samplerStreams[s].pendingVoice) &&
        (int)samplerStreams[s].instIdx == instIdx) {
      uint8_t k = samplerStreams[s].keyIdx;
      if (k < SAMPLER_NUM_KEYS) mask |= (1ULL << k);
    }
  }
  for (int vIdx = 0; vIdx < NUM_FM_VOICES; vIdx++) {
    const FmVoice *v = fmSynthVoiceAt(vIdx);
    if (v && v->active && (int)v->instIdx == instIdx) {
      int k = (int)v->note - (int)SAMPLER_BASE_NOTE;
      if (k >= 0 && k < SAMPLER_NUM_KEYS) mask |= (1ULL << k);
    }
  }
  return mask;
}

FLASHMEM static void invalidateCachedStreamsForKey(int instIdx, int keyIdx) {
  if (instIdx < 0) {
    memset(samplerFailedMask, 0, sizeof(samplerFailedMask));
  } else if (instIdx < NUM_INSTRUMENTS) {
    if (keyIdx < 0) {
      samplerFailedMask[instIdx] = 0;
    } else if (keyIdx < SAMPLER_NUM_KEYS) {
      samplerFailedMask[instIdx] &= ~(1ULL << keyIdx);
    }
  }
  for (int s = 0; s < NUM_SAMPLER_STREAMS; s++) {
    SamplerStream &st = samplerStreams[s];
    if ((instIdx < 0 || st.cachedInstIdx == instIdx) &&
        (keyIdx < 0 || st.cachedKeyIdx == keyIdx)) {
      noInterrupts();
      st.active = false;
      st.pendingVoice = false;
      st.prewarmed = false;
      st.eofReached = true;
      st.loopLockedInRing = false;
      st.prevOutL = 0;
      st.prevOutR = 0;
      st.declickRem = 0;
      interrupts();
      if (st.file.isOpen()) {
        st.file.close();
      }
      st.cachedInstIdx = -1;
      st.cachedKeyIdx = -1;
      if (instIdx < 0) {
        st.cachedSf2Tag = 0;
        st.streamKind = 0;
      }
      st.totalFrames = 0;
    }
  }
}

FLASHMEM void samplerInvalidateSf2Tag(uint16_t sf2FileTag) {
  for (int s = 0; s < NUM_SAMPLER_STREAMS; s++) {
    SamplerStream &st = samplerStreams[s];
    if (st.streamKind == (uint8_t)SharedStreamKind::WavSampler && st.cachedSf2Tag == 0) continue;
    if (sf2FileTag != 0 && st.cachedSf2Tag != sf2FileTag) continue;
    noInterrupts();
    st.active = false;
    st.pendingVoice = false;
    st.prewarmed = false;
    st.eofReached = true;
    st.loopLockedInRing = false;
    st.prevOutL = 0;
    st.prevOutR = 0;
    st.declickRem = 0;
    interrupts();
    if (st.file.isOpen()) {
      st.file.close();
    }
    st.cachedSf2Tag = 0;
    st.cachedInstIdx = -1;
    st.cachedKeyIdx = -1;
    st.streamKind = 0;
    st.totalFrames = 0;
  }
}

// ============================================================================
// WAV Header Parser & Chunk Reader (RAM1 Stack Staged for DMA_SDIO Safety)
// ============================================================================
static inline uint16_t rdLe16(const uint8_t *p) {
  return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static inline uint32_t rdLe32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

FLASHMEM static bool parseStreamWavHeader(SamplerStream &st) {
  if (!st.file.isOpen()) return false;
  uint64_t fSize = st.file.fileSize();
  if (fSize < 44) return false;

  alignas(4) uint8_t hdr[44];
  if (!st.file.seekSet(0) || st.file.read(hdr, 12) != 12) return false;
  if (memcmp(hdr, "RIFF", 4) != 0 || memcmp(hdr + 8, "WAVE", 4) != 0) return false;

  bool gotFmt = false;
  uint16_t audioFmt = 1;
  uint16_t numCh = 1;
  uint32_t sRate = SAMPLE_RATE_HZ;
  uint16_t blockAlign = 0;
  uint16_t bits = 16;

  uint64_t pos = 12;
  while (pos + 8 <= fSize) {
    if (!st.file.seekSet(pos)) return false;
    alignas(4) uint8_t chdr[8];
    if (st.file.read(chdr, 8) != 8) return false;
    uint32_t cSize = rdLe32(chdr + 4);
    uint64_t dataPos = pos + 8;

    if (memcmp(chdr, "fmt ", 4) == 0 && cSize >= 16) {
      size_t toRead = (size_t)min((uint32_t)sizeof(hdr), cSize);
      if (st.file.read(hdr, toRead) != (int)toRead) return false;
      audioFmt   = rdLe16(hdr + 0);
      numCh      = rdLe16(hdr + 2);
      sRate      = rdLe32(hdr + 4);
      blockAlign = rdLe16(hdr + 12);
      bits       = rdLe16(hdr + 14);
      if (audioFmt == 0xFFFEu && toRead >= 26) {
        audioFmt = rdLe16(hdr + 24);
      }
      gotFmt = true;
    } else if (memcmp(chdr, "data", 4) == 0 && gotFmt) {
      if (audioFmt != 1 && audioFmt != 3) return false;
      if (numCh < 1 || numCh > 4) return false;
      if (bits != 8 && bits != 16 && bits != 24 && bits != 32) return false;
      if (audioFmt == 3 && bits != 32) return false;
      if (sRate < 4000 || sRate > 192000) return false;

      uint8_t bytesPerCh = (uint8_t)((bits + 7u) / 8u);
      uint8_t bpf = (uint8_t)(numCh * bytesPerCh);
      if (blockAlign >= bpf && blockAlign <= 16 && (blockAlign % numCh) == 0) {
        bpf = (uint8_t)blockAlign;
      }
      uint64_t remBytes = (fSize > dataPos) ? (fSize - dataPos) : 0;
      uint64_t availBytes = (cSize == 0 || cSize > remBytes) ? remBytes : (uint64_t)cSize;
      uint32_t frames = (bpf > 0) ? (uint32_t)(availBytes / bpf) : 0;
      if (frames == 0) return false;

      st.dataOffset = (uint32_t)dataPos;
      st.totalFrames = frames;
      st.sampleRate = sRate;
      st.baseStepFp = (uint32_t)(((uint64_t)sRate * 65536ULL + (SAMPLE_RATE_HZ / 2)) / SAMPLE_RATE_HZ);
      if (st.baseStepFp == 0) st.baseStepFp = 1;
      st.channels = (uint8_t)numCh;
      st.bitsPerSample = (uint8_t)bits;
      st.audioFormat = (uint8_t)audioFmt;
      st.bytesPerFrame = bpf;
      return true;
    }

    uint64_t nextPos = dataPos + (uint64_t)cSize + (cSize & 1u);
    if ((cSize & 1u) && (dataPos + cSize + 8 <= fSize)) {
      // Handle non-compliant WAV writers that omit the 1-byte RIFF pad on odd-sized chunks
      alignas(4) uint8_t peek[4];
      if (st.file.seekSet(dataPos + cSize) && st.file.read(peek, 4) == 4) {
        if (memcmp(peek, "data", 4) == 0 || memcmp(peek, "fmt ", 4) == 0) {
          nextPos = dataPos + cSize;
        }
      }
    }
    if (nextPos <= pos) break;
    pos = nextPos;
  }
  return false;
}

static inline int16_t decodeSampleChannel(const uint8_t *p, uint8_t bits, uint8_t audioFmt) {
  if (bits == 16) {
    return (int16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
  }
  if (bits == 24) {
    return (int16_t)((uint16_t)p[1] | ((uint16_t)p[2] << 8));
  }
  if (bits == 8) {
    return (int16_t)(((int)p[0] - 128) << 8);
  }
  if (bits == 32) {
    if (audioFmt == 3) {
      float fv = 0.0f;
      memcpy(&fv, p, sizeof(float));
      if (!(fv >= -4.0f && fv <= 4.0f)) return 0; // Guard against NaN/Inf
      int32_t iv = (int32_t)(fv * 32767.0f);
      return (int16_t)constrain(iv, -32768, 32767);
    }
    return (int16_t)((uint16_t)p[2] | ((uint16_t)p[3] << 8));
  }
  return 0;
}

static inline void packStereoSpan(uint32_t *dst, const int16_t *srcL, const int16_t *srcR, uint32_t n) {
  for (uint32_t i = 0; i < n; i++) {
    dst[i] = (uint32_t)(uint16_t)srcL[i] | ((uint32_t)(uint16_t)srcR[i] << 16);
  }
}

FLASHMEM static bool readSf2FramesToRing(SamplerStream &st, uint8_t *rawBuf, uint32_t srcFrame,
                                         uint32_t count, uint32_t dstHead, bool isStereo, uint32_t ringMask) {
  if (count == 0) return true;
  const size_t wantBytes = (size_t)count * 2u;
  const uint64_t posL = (uint64_t)st.dataOffset + (uint64_t)srcFrame * 2ULL;
  if (st.file.curPosition() != posL && !st.file.seekSet(posL)) return false;
  if (st.file.read(rawBuf, wantBytes) != (int)wantBytes) return false;

  const uint32_t ringCap = ringMask + 1u;
  const uint32_t idx0 = dstHead & ringMask;
  const uint32_t first = min(count, ringCap - idx0);

  if (!isStereo) {
    // Native little-endian 16-bit PCM in 4-byte-aligned RAM1 chunk -> zero-shift span memcpy
    int16_t *mRing = reinterpret_cast<int16_t *>(st.ring);
    memcpy(&mRing[idx0], rawBuf, (size_t)first * 2u);
    if (count > first) {
      memcpy(&mRing[0], rawBuf + (size_t)first * 2u, (size_t)(count - first) * 2u);
    }
    return true;
  }

  // Stage Right channel in upper slice [4096..8191] of 16 KB RAM1 chunk (supports up to 2,048 stereo frames)
  uint8_t *rawR = rawBuf + 4096;
  const uint64_t posR = (uint64_t)st.dataOffsetR + (uint64_t)srcFrame * 2ULL;
  if (st.file.curPosition() != posR && !st.file.seekSet(posR)) return false;
  if (st.file.read(rawR, wantBytes) != (int)wantBytes) return false;

  const int16_t *srcL = reinterpret_cast<const int16_t *>(rawBuf);
  const int16_t *srcR = reinterpret_cast<const int16_t *>(rawR);
  packStereoSpan(&st.ring[idx0], srcL, srcR, first);
  if (count > first) {
    packStereoSpan(&st.ring[0], srcL + first, srcR + first, count - first);
  }
  return true;
}

FLASHMEM static void fillSf2Stream(SamplerStream &st, int maxPasses, bool forceHeld, bool topUp = false) {
  if (!st.file.isOpen() || st.totalFrames == 0) {
    st.eofReached = true;
    return;
  }
  if (st.loopLockedInRing) return;

  const bool isStereo = (st.streamKind == (uint8_t)SharedStreamKind::Sf2StereoLink);
  const uint32_t ringCap = isStereo ? SHARED_STEREO_RING_FRAMES : SHARED_MONO_RING_FRAMES;
  const uint32_t ringMask = ringCap - 1u;
  // Coalesce steady-state refills into one larger contiguous SDIO transfer (up to 768 stereo = 1.5 KB/ch
  // or 1,536 mono = 3 KB) to halve non-interleaved stereo L/R seek overhead.
  const uint32_t maxFramesPerRead = forceHeld ? 512u : (isStereo ? 768u : 1536u);
  uint8_t *rawBuf = reinterpret_cast<uint8_t *>(storageSharedChunkBuf());
  const uint32_t minFree = forceHeld ? 128u : (topUp ? (isStereo ? 256u : 512u) : (isStereo ? 384u : 768u));
  const uint32_t startHead = st.head;
  int effPasses = forceHeld ? maxPasses : 1;

  for (int pass = 0; pass < effPasses; pass++) {
    if (st.loopLockedInRing) break;

    const FmVoice *v = fmSynthVoiceAt(st.voiceIdx);
    const bool held = forceHeld || (st.sf2LoopMode == 1) ||
                      ((v != nullptr) && v->active && (v->gate || v->sustained));
    const bool hasLoop = (st.sf2LoopMode != 0) &&
                         !(st.sf2LoopMode == 3u && st.loopEndFrame <= 640u && st.fileFrame >= 640u) &&
                         (st.loopEndFrame > st.loopStartFrame) &&
                         (st.loopEndFrame <= st.totalFrames);
    const uint32_t loopLen = hasLoop ? (st.loopEndFrame - st.loopStartFrame) : 0u;
    const uint32_t maxLockLen = ringCap >> 1; // <= 1024 mono frames or <= 512 stereo frames
    const bool canLockInRing = hasLoop && held && (loopLen > 0u) && (loopLen <= maxLockLen);

    uint32_t queued = st.head - st.tail;

    // Lock short/single-cycle sustain loops directly inside st.ring (0 SD reads/sec during sustain)
    if (canLockInRing && st.fileFrame == st.loopStartFrame) {
      if (queued + loopLen + 4u > ringCap) {
        break;
      }
      uint32_t remLoop = loopLen;
      uint32_t curF = st.loopStartFrame;
      uint32_t writeH = st.head;
      bool ok = true;
      while (remLoop > 0u) {
        uint32_t n = min(remLoop, maxFramesPerRead);
        if (!readSf2FramesToRing(st, rawBuf, curF, n, writeH, isStereo, ringMask)) {
          ok = false;
          break;
        }
        curF += n;
        writeH += n;
        remLoop -= n;
      }
      if (!ok) {
        st.eofReached = true;
        break;
      }
      const uint32_t g1Idx = (loopLen > 1u) ? (st.head + 1u) : st.head;
      if (isStereo) {
        st.ring[writeH & ringMask]        = st.ring[st.head & ringMask];
        st.ring[(writeH + 1u) & ringMask] = st.ring[g1Idx & ringMask];
      } else {
        int16_t *mRing = reinterpret_cast<int16_t *>(st.ring);
        mRing[writeH & ringMask]        = mRing[st.head & ringMask];
        mRing[(writeH + 1u) & ringMask] = mRing[g1Idx & ringMask];
      }
      __asm__ volatile("" ::: "memory");
      uint32_t irq = saveIrq();
      if ((st.active || st.pendingVoice) && st.generation != 0) {
        st.ringLoopStart = st.head;
        st.ringLoopLen = (uint16_t)loopLen;
        st.head = writeH;
        st.fileFrame = st.loopEndFrame;
        st.loopLockedInRing = true;
      }
      restoreIrq(irq);
      break;
    }

    if (queued + minFree > ringCap && !(pass > 0 && (st.head - startHead) < 256u)) break;

    uint32_t limitFrame = st.totalFrames;
    if (hasLoop && held) {
      if (canLockInRing && st.fileFrame < st.loopStartFrame) {
        limitFrame = st.loopStartFrame;
      } else if (st.fileFrame < st.loopEndFrame) {
        limitFrame = st.loopEndFrame;
      } else {
        st.fileFrame = st.loopStartFrame;
        if (canLockInRing) continue;
        limitFrame = st.loopEndFrame;
      }
    } else if (st.fileFrame >= st.totalFrames) {
      st.eofReached = true;
      break;
    }

    uint32_t room = (ringCap > queued + 4u) ? (ringCap - 4u - queued) : 0u;
    uint32_t rem = (limitFrame > st.fileFrame) ? (limitFrame - st.fileFrame) : 0u;
    uint32_t toRead = min(min(room, maxFramesPerRead), rem);
    if (toRead == 0u) {
      if (!hasLoop || !held) st.eofReached = true;
      break;
    }

    if (!readSf2FramesToRing(st, rawBuf, st.fileFrame, toRead, st.head, isStereo, ringMask)) {
      st.eofReached = true;
      break;
    }

    const uint32_t nextHead = st.head + toRead;
    if (!hasLoop || !held) {
      // Pad 2 trailing guard samples with the final sample for clean Hermite EOF clamping
      if (st.fileFrame + toRead >= st.totalFrames) {
        if (isStereo) {
          uint32_t lastVal = st.ring[(nextHead - 1u) & ringMask];
          st.ring[nextHead & ringMask]        = lastVal;
          st.ring[(nextHead + 1u) & ringMask] = lastVal;
        } else {
          int16_t *mRing = reinterpret_cast<int16_t *>(st.ring);
          int16_t lastVal = mRing[(nextHead - 1u) & ringMask];
          mRing[nextHead & ringMask]        = lastVal;
          mRing[(nextHead + 1u) & ringMask] = lastVal;
        }
      }
    }

    __asm__ volatile("" ::: "memory");
    st.head = nextHead;
    st.fileFrame += toRead;
    if (hasLoop && held && st.fileFrame >= st.loopEndFrame && !canLockInRing) {
      st.fileFrame = st.loopStartFrame;
      // If we hit loopEndFrame with only a short tail (< 256 frames), immediately wrap and read loopStartFrame
      if ((st.head - startHead) < 256u && effPasses < 3) {
        effPasses++;
      }
    } else if ((!hasLoop || !held) && st.fileFrame >= st.totalFrames) {
      st.eofReached = true;
      break;
    }
  }
}

FLASHMEM static void fillSamplerStream(SamplerStream &st, int maxPasses, bool forceHeld = false, bool topUp = false) {
  if (st.streamKind != (uint8_t)SharedStreamKind::WavSampler) {
    fillSf2Stream(st, maxPasses, forceHeld, topUp);
    return;
  }
  if (!st.file.isOpen() || st.totalFrames == 0 || st.bytesPerFrame == 0) {
    st.eofReached = true;
    return;
  }

  // Stage multi-sector SD reads (up to 2,048 bytes = 4 sectors) through the shared RAM1 chunk buffer
  // for DMA_SDIO cache safety and low SDIO command overhead across up to 8 concurrent streams.
  static constexpr size_t SAMPLER_READ_BUF_BYTES = 2048;
  uint8_t *rawBuf = reinterpret_cast<uint8_t *>(storageSharedChunkBuf());
  const uint32_t maxFramesPerRead = max(1u, (uint32_t)(SAMPLER_READ_BUF_BYTES / st.bytesPerFrame));

  const uint32_t minFree = forceHeld ? 64u : (topUp ? 192u : 256u);
  for (int pass = 0; pass < maxPasses; pass++) {
    uint32_t queued = st.head - st.tail;
    if (queued + minFree > SAMPLER_RING_FRAMES) break;

    if (st.fileFrame >= st.totalFrames) {
      const FmPatch &p = fmSynthPatch(st.instIdx);
      SamplerPressMode mode = (SamplerPressMode)constrain((int)p.algorithm, 0, NUM_SAMPLER_PRESS_MODES - 1);
      const FmVoice *v = fmSynthVoiceAt(st.voiceIdx);
      bool held = forceHeld || ((v != nullptr) && v->active && (v->gate || v->sustained));

      if (mode == SamplerPressMode::Rep && held) {
        st.fileFrame = 0;
      } else if (mode == SamplerPressMode::RepThru && held) {
        // Wait until ring buffer holds <= ~2.5 audio blocks at current pitch before wrapping so
        // releasing the key exits the loop cleanly without starving pitched-up playback
        const uint32_t wrapMargin = constrain(
            (uint32_t)(((uint64_t)st.baseStepFp * (uint64_t)AUDIO_BLOCK_SAMPLES * 5ULL) >> 17),
            192u, (uint32_t)(SAMPLER_RING_FRAMES >> 1));
        if (queued > wrapMargin && st.totalFrames > wrapMargin) {
          break;
        }
        st.fileFrame = 0;
      } else {
        st.eofReached = true;
        break;
      }
    }

    uint32_t room = SAMPLER_RING_FRAMES - queued;
    uint32_t remFile = st.totalFrames - st.fileFrame;
    uint32_t toRead = min(min(room, maxFramesPerRead), remFile);
    if (toRead == 0) break;

    uint64_t bytePos = (uint64_t)st.dataOffset + (uint64_t)st.fileFrame * st.bytesPerFrame;
    size_t wantBytes = (size_t)toRead * st.bytesPerFrame;
    int rd = 0;
    if (st.file.curPosition() == bytePos || st.file.seekSet(bytePos)) {
      rd = st.file.read(rawBuf, wantBytes);
    }
    uint32_t gotFrames = (rd > 0) ? ((uint32_t)rd / st.bytesPerFrame) : 0;
    if (gotFrames == 0) {
      st.eofReached = true;
      break;
    }

    const uint8_t bpf = st.bytesPerFrame;
    const uint8_t ch = st.channels;
    const uint8_t bits = st.bitsPerSample;
    const uint8_t fmt = st.audioFormat;
    const uint8_t chStride = (ch > 0) ? (bpf / ch) : (bits / 8);
    uint32_t h = st.head;

    for (uint32_t i = 0; i < gotFrames; i++) {
      const uint8_t *fp = rawBuf + i * bpf;
      int16_t sL = decodeSampleChannel(fp, bits, fmt);
      int16_t sR = (ch >= 2) ? decodeSampleChannel(fp + chStride, bits, fmt) : sL;
      st.ring[(h + i) & SAMPLER_RING_MASK] = (uint32_t)(uint16_t)sL | ((uint32_t)(uint16_t)sR << 16);
    }

    __asm__ volatile("" ::: "memory");
    st.head = h + gotFrames;
    st.fileFrame += gotFrames;
  }
}

static inline uint32_t nextSharedStreamGen() {
  streamGenCounter = (streamGenCounter >= 0x1FFEu) ? 1u : (streamGenCounter + 1u);
  return streamGenCounter;
}

static inline uint32_t sharedStreamAge(uint32_t curGen, uint32_t slotGen) {
  if (slotGen == 0) return 0xFFFFFFFFu;
  return (curGen >= slotGen) ? (curGen - slotGen) : (curGen + 0x1FFEu - slotGen);
}

FLASHMEM static void reapOrphanedActiveStreams() {
  for (int s = 0; s < NUM_SAMPLER_STREAMS; s++) {
    SamplerStream &st = samplerStreams[s];
    if (!st.active) continue;
    const FmVoice *v = fmSynthVoiceAt(st.voiceIdx);
    if (!v || !v->active || v->opPhase[0] != (uint32_t)s || v->opPhase[1] != st.generation) {
      st.active = false;
      st.loopLockedInRing = false;
    }
  }
}

FLASHMEM static int pickOldestStreamSlot() {
  int chosen = 0;
  uint32_t bestAge = sharedStreamAge(streamGenCounter, samplerStreams[0].generation);
  for (int s = 1; s < NUM_SAMPLER_STREAMS; s++) {
    uint32_t age = sharedStreamAge(streamGenCounter, samplerStreams[s].generation);
    if (age > bestAge) {
      bestAge = age;
      chosen = s;
    }
  }
  return chosen;
}

FLASHMEM static bool prepareSamplerStreamSlow(int instIdx, int keyIdx, uint32_t &outStreamIdx, uint32_t &outStreamGen) {
  reapOrphanedActiveStreams();
  int chosen = -1;
  // 1. Prefer an idle stream slot that already has this exact (instIdx, keyIdx) cached
  for (int s = 0; s < NUM_SAMPLER_STREAMS; s++) {
    if (!samplerStreams[s].active && !samplerStreams[s].pendingVoice &&
        samplerStreams[s].cachedInstIdx == instIdx &&
        samplerStreams[s].cachedKeyIdx == keyIdx &&
        samplerStreams[s].file.isOpen()) {
      chosen = s;
      break;
    }
  }

  // 2. Otherwise pick any idle stream slot
  if (chosen < 0) {
    for (int s = 0; s < NUM_SAMPLER_STREAMS; s++) {
      if (!samplerStreams[s].active && !samplerStreams[s].pendingVoice) {
        chosen = s;
        break;
      }
    }
  }

  // 3. Otherwise steal the oldest stream slot
  if (chosen < 0) {
    chosen = pickOldestStreamSlot();
  }

  SamplerStream &st = samplerStreams[chosen];
  fillingStreamIdx = (int8_t)chosen;
  int16_t savedPrevL = 0, savedPrevR = 0;
  bool wasActiveStream = false;
  noInterrupts();
  wasActiveStream = st.active;
  savedPrevL = st.prevOutL;
  savedPrevR = st.prevOutR;
  st.active = false;
  st.pendingVoice = false;
  st.prewarmed = false;
  st.prevOutL = 0;
  st.prevOutR = 0;
  interrupts();

  if (!st.file.isOpen() || st.streamKind != (uint8_t)SharedStreamKind::WavSampler ||
      st.cachedInstIdx != instIdx || st.cachedKeyIdx != keyIdx || st.totalFrames == 0) {
    if (st.file.isOpen()) {
      st.file.close();
    }
    st.cachedInstIdx = -1;
    st.cachedKeyIdx = -1;
    st.cachedSf2Tag = 0;
    st.streamKind = (uint8_t)SharedStreamKind::WavSampler;
    st.totalFrames = 0;

    char fullPath[SAMPLER_PATH_MAX];
    resolveKeyFullPath(instIdx, keyIdx, fullPath, sizeof(fullPath));
    st.file = SD.sdfs.open(fullPath, O_RDONLY);
    if (!st.file.isOpen() || !parseStreamWavHeader(st)) {
      if (st.file.isOpen()) st.file.close();
      samplerFailedMask[instIdx] |= (1ULL << keyIdx);
      fillingStreamIdx = -1;
      return false;
    }
    st.cachedInstIdx = (int8_t)instIdx;
    st.cachedKeyIdx = (int8_t)keyIdx;
  }

  noInterrupts();
  uint32_t gen = nextSharedStreamGen();
  st.streamKind = (uint8_t)SharedStreamKind::WavSampler;
  st.loopLockedInRing = false;
  st.instIdx = (uint8_t)instIdx;
  st.keyIdx = (uint8_t)keyIdx;
  st.generation = gen;
  st.head = 0;
  st.tail = 0;
  st.fileFrame = 0;
  st.fracFp = 0;
  st.lastL = 0;
  st.lastR = 0;
  st.declickL = wasActiveStream ? savedPrevL : 0;
  st.declickR = wasActiveStream ? savedPrevR : 0;
  st.declickRem = (wasActiveStream && (savedPrevL != 0 || savedPrevR != 0)) ? 32 : 0;
  st.eofReached = false;
  interrupts();

  // Pre-fill initial chunks before voice activation so the very first ISR tick has audio ready
  fillSamplerStream(st, 4, true);
  if (st.head == 0) {
    fillingStreamIdx = -1;
    return false;
  }

  noInterrupts();
  st.pendingVoice = true;
  interrupts();
  fillingStreamIdx = -1;

  outStreamIdx = (uint32_t)chosen;
  outStreamGen = gen;
  return true;
}

FLASHMEM void samplerPrewarmSf2File(const char *sf2Path, uint16_t sf2FileTag) {
  if (!sf2Path || !sf2Path[0] || sf2FileTag == 0) return;
  reapOrphanedActiveStreams();
  for (int s = 0; s < NUM_SAMPLER_STREAMS; s++) {
    SamplerStream &st = samplerStreams[s];
    if (st.active || st.pendingVoice) continue;
    if (st.file.isOpen() && st.cachedSf2Tag == sf2FileTag) continue;
    // Keep any already-cached WAV Sampler key intact if that Sampler slot is still populated
    if (st.file.isOpen() && st.streamKind == (uint8_t)SharedStreamKind::WavSampler &&
        st.cachedInstIdx >= 0 && st.cachedInstIdx < NUM_INSTRUMENTS &&
        (InstType)fmSynthPatch(st.cachedInstIdx).type == InstType::Sampler) {
      continue;
    }
    if (st.file.isOpen()) {
      st.file.close();
    }
    st.cachedInstIdx = -1;
    st.cachedKeyIdx = -1;
    st.cachedSf2Tag = 0;
    st.prewarmed = false;
    st.file = SD.sdfs.open(sf2Path, O_RDONLY);
    if (st.file.isOpen()) {
      st.cachedSf2Tag = sf2FileTag;
    }
  }
}

FLASHMEM bool samplerOpenSf2Stream(const SharedSf2StreamReq &req, uint32_t &outStreamIdx, uint32_t &outStreamGen, int initialPasses) {
  if (!req.sf2Path || !req.sf2Path[0] || req.totalFrames == 0 || req.sf2FileTag == 0) {
    return false;
  }

  reapOrphanedActiveStreams();

  int chosen = -1;
  // 1. Prefer an idle stream slot that already has this exact .sf2 file open!
  for (int s = 0; s < NUM_SAMPLER_STREAMS; s++) {
    if (!samplerStreams[s].active && !samplerStreams[s].pendingVoice &&
        samplerStreams[s].cachedSf2Tag == req.sf2FileTag &&
        samplerStreams[s].file.isOpen()) {
      chosen = s;
      break;
    }
  }
  // 2. Prefer any completely idle stream slot without an open WAV cache
  if (chosen < 0) {
    for (int s = 0; s < NUM_SAMPLER_STREAMS; s++) {
      if (!samplerStreams[s].active && !samplerStreams[s].pendingVoice &&
          !samplerStreams[s].file.isOpen()) {
        chosen = s;
        break;
      }
    }
  }
  // 3. Otherwise pick any idle stream slot
  if (chosen < 0) {
    for (int s = 0; s < NUM_SAMPLER_STREAMS; s++) {
      if (!samplerStreams[s].active && !samplerStreams[s].pendingVoice) {
        chosen = s;
        break;
      }
    }
  }
  // 4. Otherwise steal the truly oldest stream slot (wraparound-safe)
  if (chosen < 0) {
    chosen = pickOldestStreamSlot();
  }

  SamplerStream &st = samplerStreams[chosen];
  fillingStreamIdx = (int8_t)chosen;
  noInterrupts();
  st.active = false;
  st.pendingVoice = false;
  st.prewarmed = false;
  st.loopLockedInRing = false;
  st.prevOutL = 0;
  st.prevOutR = 0;
  st.declickRem = 0;
  interrupts();

  if (!st.file.isOpen() || st.cachedSf2Tag != req.sf2FileTag) {
    if (st.file.isOpen()) {
      st.file.close();
    }
    st.cachedInstIdx = -1;
    st.cachedKeyIdx = -1;
    st.cachedSf2Tag = 0;
    st.file = SD.sdfs.open(req.sf2Path, O_RDONLY);
    if (!st.file.isOpen()) {
      fillingStreamIdx = -1;
      return false;
    }
    st.cachedSf2Tag = req.sf2FileTag;
  }

  uint32_t gen = 0;
  noInterrupts();
  gen = nextSharedStreamGen();
  st.cachedInstIdx = -1;
  st.cachedKeyIdx = -1;
  st.streamKind = (uint8_t)req.kind;
  st.sf2LoopMode = req.sf2LoopMode;
  st.dataOffset = req.dataOffsetL;
  st.dataOffsetR = req.dataOffsetR;
  st.totalFrames = req.totalFrames;
  st.loopStartFrame = req.loopStartFrame;
  st.loopEndFrame = req.loopEndFrame;
  st.bytesPerFrame = (req.kind == SharedStreamKind::Sf2StereoLink) ? 4u : 2u;
  st.instIdx = (uint8_t)req.instIdx;
  st.keyIdx = 0;
  st.voiceIdx = (req.voiceIdx >= 0) ? (uint8_t)req.voiceIdx : 0u;
  st.generation = gen;
  st.head = 0;
  st.tail = 0;
  st.ringLoopStart = 0;
  st.ringLoopLen = 0;
  st.fileFrame = min(req.startFrame, req.totalFrames);
  st.fracFp = 0;
  st.lastL = 0;
  st.lastR = 0;
  st.eofReached = false;
  st.loopLockedInRing = false;
  interrupts();

  fillSf2Stream(st, constrain(initialPasses, 1, 4), true);
  if (st.head == 0 && !st.eofReached) {
    fillingStreamIdx = -1;
    return false;
  }

  noInterrupts();
  if (req.voiceIdx >= 0) {
    st.voiceIdx = (uint8_t)req.voiceIdx;
    st.active = true;
    st.pendingVoice = false;
  } else {
    st.pendingVoice = true;
  }
  interrupts();
  fillingStreamIdx = -1;

  outStreamIdx = (uint32_t)chosen;
  outStreamGen = gen;
  return true;
}

FLASHMEM bool samplerPrepareVoiceOn(int instIdx, uint8_t note, uint32_t &outStreamIdx, uint32_t &outStreamGen, bool allowSdIo) {
  if (instIdx < 0 || instIdx >= NUM_INSTRUMENTS) return false;
  int keyIdx = (int)note - (int)SAMPLER_BASE_NOTE;
  if (keyIdx < 0 || keyIdx >= SAMPLER_NUM_KEYS) return false;
  if ((samplerPopulatedMask[instIdx] & (1ULL << keyIdx)) == 0) {
    return false;
  }

  // Fast Path (Zero SD I/O — safe from ISR or loop()):
  // If this (instIdx, keyIdx) is already cached and its initial frames [0..head) are intact in st.ring,
  // claim or rewind the stream in-place in < 1 us without touching SdFat or chunk.
  uint32_t irq = saveIrq();
#pragma GCC unroll 1
  for (int s = 0; s < NUM_SAMPLER_STREAMS; s++) {
    if (s == (int)fillingStreamIdx) continue;
    SamplerStream &st = samplerStreams[s];
    if (st.streamKind == (uint8_t)SharedStreamKind::WavSampler &&
        st.cachedInstIdx == instIdx &&
        st.cachedKeyIdx == keyIdx &&
        st.totalFrames > 0 &&
        st.head > 0 &&
        st.head <= SAMPLER_RING_FRAMES &&
        st.fileFrame == st.head) {
      uint32_t gen = nextSharedStreamGen();
      const bool wasActive = st.active;
      const int16_t prevL = st.prevOutL;
      const int16_t prevR = st.prevOutR;
      st.active = false;
      st.pendingVoice = true;
      st.prewarmed = false;
      st.instIdx = (uint8_t)instIdx;
      st.keyIdx = (uint8_t)keyIdx;
      st.generation = gen;
      st.tail = 0;
      st.fracFp = 0;
      st.lastL = 0;
      st.lastR = 0;
      st.prevOutL = 0;
      st.prevOutR = 0;
      st.declickL = wasActive ? prevL : 0;
      st.declickR = wasActive ? prevR : 0;
      st.declickRem = (wasActive && (prevL != 0 || prevR != 0)) ? 32 : 0;
      st.eofReached = (st.fileFrame >= st.totalFrames);
      restoreIrq(irq);
      outStreamIdx = (uint32_t)s;
      outStreamGen = gen;
      return true;
    }
  }
  restoreIrq(irq);

  if (!allowSdIo) {
    return false;
  }
  return prepareSamplerStreamSlow(instIdx, keyIdx, outStreamIdx, outStreamGen);
}

void samplerActivateStream(uint32_t streamIdx, uint32_t streamGen, int voiceIdx) {
  if (streamIdx >= (uint32_t)NUM_SAMPLER_STREAMS) return;
  SamplerStream &st = samplerStreams[streamIdx];
  if (st.generation == streamGen) {
    st.voiceIdx = (uint8_t)voiceIdx;
    st.active = true;
    st.pendingVoice = false;
  }
}

FLASHMEM void samplerServiceActiveStreams(bool topUp) {
  if (fillingStreamIdx >= 0) return;
  uint8_t servicedMask = 0;

  // Two-tier urgency scheduling: Pass 0 refills nearly-empty (< ~6 ms) streams first so
  // high-pitched voices never starve behind half-full voices during 16 KB audio track writes.
  for (int tier = 0; tier < 2; tier++) {
    for (int s = 0; s < NUM_SAMPLER_STREAMS; s++) {
      if (servicedMask & (uint8_t)(1u << s)) continue;
      SamplerStream &st = samplerStreams[s];
      const bool isMonoSf2 = (st.streamKind == (uint8_t)SharedStreamKind::Sf2Mono16);
      const uint32_t ringCap = isMonoSf2 ? SHARED_MONO_RING_FRAMES : SHARED_STEREO_RING_FRAMES;
      const uint32_t urgentThresh = isMonoSf2 ? 512u : 256u;

      if (st.pendingVoice) {
        if (st.eofReached || st.loopLockedInRing) {
          servicedMask |= (uint8_t)(1u << s);
          continue;
        }
        uint32_t queued = st.head - st.tail;
        if (tier == 0 && queued >= urgentThresh) continue;
        servicedMask |= (uint8_t)(1u << s);
        if (queued < (ringCap - 128u)) {
          fillingStreamIdx = (int8_t)s;
          fillSamplerStream(st, 2, true);
          fillingStreamIdx = -1;
        }
        continue;
      }

      if (!st.active) {
        servicedMask |= (uint8_t)(1u << s);
        continue;
      }
      const FmVoice *v = fmSynthVoiceAt(st.voiceIdx);
      if (!v || !v->active || v->opPhase[0] != (uint32_t)s || v->opPhase[1] != st.generation) {
        st.active = false;
        st.loopLockedInRing = false;
        servicedMask |= (uint8_t)(1u << s);
        continue;
      }
      if (st.eofReached || st.loopLockedInRing) {
        servicedMask |= (uint8_t)(1u << s);
        continue;
      }
      uint32_t queued = st.head - st.tail;
      if (tier == 0 && queued >= urgentThresh) continue;
      servicedMask |= (uint8_t)(1u << s);
      fillingStreamIdx = (int8_t)s;
      fillSamplerStream(st, 2, false, topUp);
      fillingStreamIdx = -1;
    }
  }
}

FLASHMEM void samplerServiceStreams() {
  if (fillingStreamIdx >= 0) return;
  samplerServiceActiveStreams();

  // Pre-warm at most 1 idle stream slot per pass so up to 8 Sampler keys trigger in ISR with 0 SD I/O
  for (int s = 0; s < NUM_SAMPLER_STREAMS; s++) {
    SamplerStream &st = samplerStreams[s];
    if (st.active || st.pendingVoice || st.prewarmed) continue;

    if (st.file.isOpen() && st.streamKind == (uint8_t)SharedStreamKind::WavSampler &&
        st.cachedInstIdx >= 0 && st.cachedKeyIdx >= 0 && st.totalFrames > 0) {
      fillingStreamIdx = (int8_t)s;
      if (st.head > 0 && st.head <= SAMPLER_RING_FRAMES && st.fileFrame == st.head) {
        if (!st.eofReached && st.head < SAMPLER_RING_FRAMES) {
          fillSamplerStream(st, 2, true);
        }
        noInterrupts();
        if (!st.active && !st.pendingVoice) {
          st.tail = 0;
          st.fracFp = 0;
          st.lastL = 0;
          st.lastR = 0;
          st.eofReached = (st.fileFrame >= st.totalFrames);
          st.prewarmed = true;
        }
        interrupts();
      } else {
        noInterrupts();
        if (!st.active && !st.pendingVoice) {
          st.head = 0;
          st.tail = 0;
          st.fileFrame = 0;
          st.fracFp = 0;
          st.lastL = 0;
          st.lastR = 0;
          st.eofReached = false;
        }
        interrupts();
        fillSamplerStream(st, 4, true);
        noInterrupts();
        if (!st.active && !st.pendingVoice && st.head > 0) {
          st.prewarmed = true;
        }
        interrupts();
      }
      fillingStreamIdx = -1;
      return;
    }

    // Slot has no WAV file open yet: look for an uncached populated key on any Sampler instrument
    for (int inst = 0; inst < NUM_INSTRUMENTS; inst++) {
      if ((InstType)fmSynthPatch(inst).type != InstType::Sampler) continue;
      uint64_t availMask = samplerPopulatedMask[inst] & ~samplerFailedMask[inst];
      if (!availMask) continue;
      for (int k = 0; k < SAMPLER_NUM_KEYS; k++) {
        if ((availMask & (1ULL << k)) == 0) continue;
        bool alreadyCached = false;
        for (int j = 0; j < NUM_SAMPLER_STREAMS; j++) {
          if (samplerStreams[j].streamKind == (uint8_t)SharedStreamKind::WavSampler &&
              samplerStreams[j].cachedInstIdx == inst &&
              samplerStreams[j].cachedKeyIdx == k &&
              samplerStreams[j].file.isOpen()) {
            alreadyCached = true;
            break;
          }
        }
        if (alreadyCached) continue;

        fillingStreamIdx = (int8_t)s;
        if (st.file.isOpen()) {
          st.file.close();
        }
        st.cachedSf2Tag = 0;
        st.streamKind = (uint8_t)SharedStreamKind::WavSampler;
        st.loopLockedInRing = false;
        char fullPath[SAMPLER_PATH_MAX];
        resolveKeyFullPath(inst, k, fullPath, sizeof(fullPath));
        st.file = SD.sdfs.open(fullPath, O_RDONLY);
        if (!st.file.isOpen() || !parseStreamWavHeader(st)) {
          if (st.file.isOpen()) st.file.close();
          samplerFailedMask[inst] |= (1ULL << k);
          fillingStreamIdx = -1;
          return;
        }
        st.cachedInstIdx = (int8_t)inst;
        st.cachedKeyIdx = (int8_t)k;
        st.instIdx = (uint8_t)inst;
        st.keyIdx = (uint8_t)k;
        st.head = 0;
        st.tail = 0;
        st.fileFrame = 0;
        st.fracFp = 0;
        st.lastL = 0;
        st.lastR = 0;
        st.eofReached = false;
        fillSamplerStream(st, 4, true);
        noInterrupts();
        if (!st.active && !st.pendingVoice && st.head > 0) {
          st.prewarmed = true;
        }
        interrupts();
        fillingStreamIdx = -1;
        return;
      }
    }
  }
}

FLASHMEM void samplerSilenceInst(int instIdx) {
  for (int s = 0; s < NUM_SAMPLER_STREAMS; s++) {
    if ((int)samplerStreams[s].instIdx == instIdx &&
        (samplerStreams[s].active || samplerStreams[s].pendingVoice)) {
      samplerStreams[s].active = false;
      samplerStreams[s].pendingVoice = false;
      samplerStreams[s].loopLockedInRing = false;
      samplerStreams[s].prevOutL = 0;
      samplerStreams[s].prevOutR = 0;
      samplerStreams[s].declickRem = 0;
    }
  }
}

FLASHMEM void samplerStopAllStreams() {
  for (int s = 0; s < NUM_SAMPLER_STREAMS; s++) {
    samplerStreams[s].active = false;
    samplerStreams[s].pendingVoice = false;
    samplerStreams[s].loopLockedInRing = false;
    samplerStreams[s].prevOutL = 0;
    samplerStreams[s].prevOutR = 0;
    samplerStreams[s].declickRem = 0;
  }
}

// ============================================================================
// Slot Lifecycle & Sample Assignment Helpers
// ============================================================================
FLASHMEM static void spillClipboardIfSlot(int instIdx) {
  if (clipboardSamplerSrcSlot != instIdx || instIdx < 0 || instIdx >= NUM_INSTRUMENTS) return;
  FsFile f = SD.sdfs.open(CLIP_SAMPLER_PATH, O_RDWR | O_CREAT | O_TRUNC);
  if (f.isOpen()) {
    bool ok = (f.write(samplerKeyDirIdx[instIdx], SAMPLER_NUM_KEYS) == SAMPLER_NUM_KEYS);
    for (int k = 0; k < SAMPLER_NUM_KEYS && ok; k += 4) {
      char stage[4][SAMPLER_FILE_MAX];
      memcpy(stage, samplerKeyFile[instIdx][k], sizeof(stage));
      ok = (f.write(stage, sizeof(stage)) == (int)sizeof(stage));
    }
    f.close();
    clipboardSamplerSrcSlot = ok ? -2 : -1;
  } else {
    clipboardSamplerSrcSlot = -1;
  }
}

FLASHMEM bool samplerAssignSample(int instIdx, int keyIdx, const char *fullOrRelPath) {
  if (instIdx < 0 || instIdx >= NUM_INSTRUMENTS) return false;
  if (keyIdx < 0 || keyIdx >= SAMPLER_NUM_KEYS) return false;
  spillClipboardIfSlot(instIdx);
  invalidateCachedStreamsForKey(instIdx, keyIdx);
  if (!fullOrRelPath || !fullOrRelPath[0]) {
    samplerKeyDirIdx[instIdx][keyIdx] = 0xFFu;
    samplerKeyFile[instIdx][keyIdx][0] = '\0';
    samplerPopulatedMask[instIdx] &= ~(1ULL << keyIdx);
    return true;
  }

  const char *p = fullOrRelPath;
  if (strncasecmp(p, "/Capstan/SAMPLES/", SAMPLES_DIR_PREFIX_LEN) == 0 &&
      p[SAMPLES_DIR_PREFIX_LEN] != '\0') {
    p += SAMPLES_DIR_PREFIX_LEN;
  }

  const char *lastSlash = strrchr(p, '/');
  if (!lastSlash) {
    samplerKeyDirIdx[instIdx][keyIdx] = 0xFFu;
    snprintf(samplerKeyFile[instIdx][keyIdx], SAMPLER_FILE_MAX, "%s", p);
  } else {
    char dirPart[SAMPLER_DIR_MAX];
    if (lastSlash == p) {
      snprintf(dirPart, sizeof(dirPart), "/");
    } else if (p[0] == '/') {
      size_t dLen = min((size_t)(lastSlash - p), sizeof(dirPart) - 1);
      memcpy(dirPart, p, dLen);
      dirPart[dLen] = '\0';
    } else {
      // Relative subfolder inside /Capstan/SAMPLES (e.g. "Drums/Kick.wav")
      size_t subLen = (size_t)(lastSlash - p);
      snprintf(dirPart, sizeof(dirPart), "%s/%.*s", SAMPLES_DIR, (int)subLen, p);
    }
    uint8_t dIdx = allocOrFindSamplerDir(dirPart);
    if (dIdx == SAMPLER_DIR_POOL_FULL) {
      uiShowToast("DIR POOL FULL");
      uiForceRedraw();
      return false;
    }
    samplerKeyDirIdx[instIdx][keyIdx] = dIdx;
    snprintf(samplerKeyFile[instIdx][keyIdx], SAMPLER_FILE_MAX, "%s", lastSlash + 1);
  }
  if (samplerKeyFile[instIdx][keyIdx][0] != '\0') {
    samplerPopulatedMask[instIdx] |= (1ULL << keyIdx);
  } else {
    samplerPopulatedMask[instIdx] &= ~(1ULL << keyIdx);
  }
  return true;
}

FLASHMEM void samplerClearKey(int instIdx, int keyIdx) {
  if (instIdx < 0 || instIdx >= NUM_INSTRUMENTS) return;
  if (keyIdx < 0 || keyIdx >= SAMPLER_NUM_KEYS) return;
  spillClipboardIfSlot(instIdx);
  fmSynthNoteOff(instIdx, (uint8_t)(SAMPLER_BASE_NOTE + keyIdx));
  invalidateCachedStreamsForKey(instIdx, keyIdx);
  samplerKeyDirIdx[instIdx][keyIdx] = 0xFFu;
  samplerKeyFile[instIdx][keyIdx][0] = '\0';
  samplerPopulatedMask[instIdx] &= ~(1ULL << keyIdx);
}

FLASHMEM void samplerResetAll() {
  browserOpen = false;
  invalidateCachedStreamsForKey(-1, -1);
  memset(samplerDirs, 0, sizeof(samplerDirs));
  memset(samplerKeyDirIdx, 0xFF, sizeof(samplerKeyDirIdx));
  memset(samplerKeyFile, 0, sizeof(samplerKeyFile));
  memset(samplerPopulatedMask, 0, sizeof(samplerPopulatedMask));
  clipboardSamplerSrcSlot = -1;
}

FLASHMEM void samplerClearSlot(int instIdx) {
  if (instIdx < 0 || instIdx >= NUM_INSTRUMENTS) return;
  spillClipboardIfSlot(instIdx);
  invalidateCachedStreamsForKey(instIdx, -1);
  memset(samplerKeyDirIdx[instIdx], 0xFF, sizeof(samplerKeyDirIdx[instIdx]));
  memset(samplerKeyFile[instIdx], 0, sizeof(samplerKeyFile[instIdx]));
  samplerPopulatedMask[instIdx] = 0;
}

FLASHMEM void samplerCopySlot(int instIdx) {
  if (instIdx < 0 || instIdx >= NUM_INSTRUMENTS) return;
  clipboardSamplerSrcSlot = (int8_t)instIdx;
}

FLASHMEM void samplerPasteSlot(int instIdx) {
  if (instIdx < 0 || instIdx >= NUM_INSTRUMENTS) return;
  invalidateCachedStreamsForKey(instIdx, -1);
  if (clipboardSamplerSrcSlot >= 0 && clipboardSamplerSrcSlot < NUM_INSTRUMENTS) {
    if (clipboardSamplerSrcSlot != instIdx) {
      memcpy(samplerKeyDirIdx[instIdx], samplerKeyDirIdx[clipboardSamplerSrcSlot], sizeof(samplerKeyDirIdx[instIdx]));
      memcpy(samplerKeyFile[instIdx], samplerKeyFile[clipboardSamplerSrcSlot], sizeof(samplerKeyFile[instIdx]));
      samplerPopulatedMask[instIdx] = samplerPopulatedMask[clipboardSamplerSrcSlot];
    }
  } else if (clipboardSamplerSrcSlot == -2) {
    memset(samplerKeyDirIdx[instIdx], 0xFF, sizeof(samplerKeyDirIdx[instIdx]));
    memset(samplerKeyFile[instIdx], 0, sizeof(samplerKeyFile[instIdx]));
    FsFile f = SD.sdfs.open(CLIP_SAMPLER_PATH, O_RDONLY);
    if (f.isOpen()) {
      f.read(samplerKeyDirIdx[instIdx], SAMPLER_NUM_KEYS);
      for (int k = 0; k < SAMPLER_NUM_KEYS; k += 4) {
        char stage[4][SAMPLER_FILE_MAX] = {};
        if (f.read(stage, sizeof(stage)) == (int)sizeof(stage)) {
          memcpy(samplerKeyFile[instIdx][k], stage, sizeof(stage));
        }
      }
      f.close();
      SD.sdfs.remove(CLIP_SAMPLER_PATH);
    }
    rebuildSamplerSlotMask(instIdx);
  }
  clipboardSamplerSrcSlot = -1;
}

FLASHMEM void samplerRemapSongFolder(const char *oldSongName, const char *newSongName) {
  if (!oldSongName || !oldSongName[0] || !newSongName || !newSongName[0]) return;
  if (strcasecmp(oldSongName, newSongName) == 0) return;
  char oldPrefix[64], newPrefix[64];
  snprintf(oldPrefix, sizeof(oldPrefix), "%s/%s/", SONGS_DIR, oldSongName);
  snprintf(newPrefix, sizeof(newPrefix), "%s/%s/", SONGS_DIR, newSongName);
  size_t oldLen = strlen(oldPrefix);
  size_t newLen = strlen(newPrefix);
  for (int d = 0; d < NUM_SAMPLER_DIRS; d++) {
    if (strncasecmp(samplerDirs[d], oldPrefix, oldLen) == 0) {
      char tail[SAMPLER_DIR_MAX];
      snprintf(tail, sizeof(tail), "%s", samplerDirs[d] + oldLen);
      size_t maxTail = (newLen < (size_t)(SAMPLER_DIR_MAX - 1)) ? ((size_t)(SAMPLER_DIR_MAX - 1) - newLen) : 0;
      snprintf(samplerDirs[d], SAMPLER_DIR_MAX, "%.*s%.*s", (int)newLen, newPrefix, (int)maxTail, tail);
    }
  }
  invalidateCachedStreamsForKey(-1, -1);
}

// ============================================================================
// SD Card Persistence (Appended to INST.DAT and .FMI via RAM1 Buffers)
// ============================================================================
struct DiskSamplerHeader {
  char magic[4];    // "SMP2" (or legacy "SMP1")
  uint16_t version; // 2 (or 1)
  uint16_t slotMask;
};

struct DiskSamplerSlotV2 {
  uint8_t slotIdx;
  uint8_t numKeys;
  uint8_t dirIdx[SAMPLER_NUM_KEYS];
  char files[SAMPLER_NUM_KEYS][SAMPLER_FILE_MAX];
};

struct LegacyDiskSamplerSlotV1 {
  uint8_t slotIdx;
  uint8_t numKeys;
  uint16_t reserved;
  char paths[18][24];
};

FLASHMEM bool samplerSaveSongData(FsFile &f) {
  if (!f.isOpen()) return false;
  uint16_t mask = 0;
  for (int i = 0; i < NUM_INSTRUMENTS; i++) {
    if ((InstType)fmSynthPatch(i).type == InstType::Sampler) {
      mask |= (uint16_t)(1u << i);
    }
  }
  if (mask == 0) return true;

  DiskSamplerHeader hdr{};
  memcpy(hdr.magic, "SMP2", 4);
  hdr.version = 2;
  hdr.slotMask = mask;
  if (f.write(&hdr, sizeof(hdr)) != (int)sizeof(hdr)) return false;

  for (int d = 0; d < NUM_SAMPLER_DIRS; d++) {
    if (f.write(samplerDirs[d], SAMPLER_DIR_MAX) != SAMPLER_DIR_MAX) return false;
  }

  uint8_t *stage = reinterpret_cast<uint8_t *>(storageSharedChunkBuf());
  for (int i = 0; i < NUM_INSTRUMENTS; i++) {
    if (!(mask & (1u << i))) continue;
    uint8_t slotHdr[2] = { (uint8_t)i, (uint8_t)SAMPLER_NUM_KEYS };
    if (f.write(slotHdr, 2) != 2) return false;
    if (f.write(samplerKeyDirIdx[i], SAMPLER_NUM_KEYS) != SAMPLER_NUM_KEYS) return false;
    memcpy(stage, samplerKeyFile[i], sizeof(samplerKeyFile[i]));
    if (f.write(stage, sizeof(samplerKeyFile[i])) != (int)sizeof(samplerKeyFile[i])) return false;
  }
  return true;
}

FLASHMEM bool samplerLoadSongData(FsFile &f) {
  invalidateCachedStreamsForKey(-1, -1);
  memset(samplerDirs, 0, sizeof(samplerDirs));
  memset(samplerKeyDirIdx, 0xFF, sizeof(samplerKeyDirIdx));
  memset(samplerKeyFile, 0, sizeof(samplerKeyFile));
  memset(samplerPopulatedMask, 0, sizeof(samplerPopulatedMask));
  if (!f.isOpen()) return false;

  uint64_t startPos = f.curPosition();
  DiskSamplerHeader hdr{};
  int rd = f.read(&hdr, sizeof(hdr));
  if (rd != (int)sizeof(hdr)) {
    return true; // Songs without sampler extension are valid
  }

  if (memcmp(hdr.magic, "SMP2", 4) == 0 && hdr.version == 2) {
    for (int d = 0; d < NUM_SAMPLER_DIRS; d++) {
      if (f.read(samplerDirs[d], SAMPLER_DIR_MAX) != SAMPLER_DIR_MAX) return true;
      samplerDirs[d][SAMPLER_DIR_MAX - 1] = '\0';
    }
    for (int i = 0; i < NUM_INSTRUMENTS; i++) {
      if (!(hdr.slotMask & (1u << i))) continue;
      uint8_t slotHdr[2] = {};
      if (f.read(slotHdr, 2) != 2) break;
      int dstIdx = (int)slotHdr[0];
      int fileKeys = (int)slotHdr[1];
      if (fileKeys <= 0 || fileKeys > SAMPLER_NUM_KEYS) break;

      uint8_t tmpDir[SAMPLER_NUM_KEYS];
      if (f.read(tmpDir, fileKeys) != fileKeys) break;

      char (*tmpFiles)[SAMPLER_FILE_MAX] = reinterpret_cast<char (*)[SAMPLER_FILE_MAX]>(storageSharedChunkBuf());
      int filesBytes = fileKeys * SAMPLER_FILE_MAX;
      if (f.read(tmpFiles, filesBytes) != filesBytes) break;

      if (dstIdx >= 0 && dstIdx < NUM_INSTRUMENTS) {
        int keyBase = (fileKeys == 18) ? 12 : 0; // Map legacy 18-key (C4..F5) slots to C4..F5
        for (int k = 0; k < fileKeys && (keyBase + k) < SAMPLER_NUM_KEYS; k++) {
          tmpFiles[k][SAMPLER_FILE_MAX - 1] = '\0';
          samplerKeyDirIdx[dstIdx][keyBase + k] = tmpDir[k];
          memcpy(samplerKeyFile[dstIdx][keyBase + k], tmpFiles[k], SAMPLER_FILE_MAX);
        }
        rebuildSamplerSlotMask(dstIdx);
      }
    }
    return true;
  }

  if (memcmp(hdr.magic, "SMP1", 4) == 0 && hdr.version == 1) {
    for (int i = 0; i < NUM_INSTRUMENTS; i++) {
      if (!(hdr.slotMask & (1u << i))) continue;
      LegacyDiskSamplerSlotV1 slotBuf{};
      if (f.read(&slotBuf, sizeof(slotBuf)) != (int)sizeof(slotBuf)) break;
      int dstIdx = (int)slotBuf.slotIdx;
      if (dstIdx >= 0 && dstIdx < NUM_INSTRUMENTS) {
        int nKeys = min((int)slotBuf.numKeys, 18);
        for (int k = 0; k < nKeys; k++) {
          slotBuf.paths[k][23] = '\0';
          samplerAssignSample(dstIdx, 12 + k, slotBuf.paths[k]);
        }
      }
    }
    return true;
  }

  f.seekSet(startPos);
  return true;
}

FLASHMEM bool samplerSaveSingleInstData(FsFile &f, int instIdx) {
  if (!f.isOpen() || instIdx < 0 || instIdx >= NUM_INSTRUMENTS) return false;
  if ((InstType)fmSynthPatch(instIdx).type != InstType::Sampler) return true;

  uint8_t slotHdr[2] = { (uint8_t)instIdx, (uint8_t)SAMPLER_NUM_KEYS };
  if (f.write(slotHdr, 2) != 2) return false;
  if (f.write(samplerKeyDirIdx[instIdx], SAMPLER_NUM_KEYS) != SAMPLER_NUM_KEYS) return false;
  uint8_t *stage = reinterpret_cast<uint8_t *>(storageSharedChunkBuf());
  memcpy(stage, samplerKeyFile[instIdx], sizeof(samplerKeyFile[instIdx]));
  if (f.write(stage, sizeof(samplerKeyFile[instIdx])) != (int)sizeof(samplerKeyFile[instIdx])) return false;

  char dirMagic[4] = { 'S', 'M', 'D', '2' };
  if (f.write(dirMagic, sizeof(dirMagic)) != (int)sizeof(dirMagic)) return false;
  for (int d = 0; d < NUM_SAMPLER_DIRS; d++) {
    if (f.write(samplerDirs[d], SAMPLER_DIR_MAX) != SAMPLER_DIR_MAX) return false;
  }
  return true;
}

FLASHMEM bool samplerLoadSingleInstData(FsFile &f, int instIdx) {
  if (instIdx < 0 || instIdx >= NUM_INSTRUMENTS) return false;
  samplerClearSlot(instIdx);
  if (!f.isOpen()) return false;
  if ((InstType)fmSynthPatch(instIdx).type != InstType::Sampler) return true;

  uint8_t slotHdr[2] = {};
  if (f.read(slotHdr, 2) != 2) return true;
  int fileKeys = (int)slotHdr[1];
  if (fileKeys <= 0 || fileKeys > SAMPLER_NUM_KEYS) return true;

  uint8_t tmpDir[SAMPLER_NUM_KEYS];
  if (f.read(tmpDir, fileKeys) != fileKeys) return true;

  char (*tmpFiles)[SAMPLER_FILE_MAX] = reinterpret_cast<char (*)[SAMPLER_FILE_MAX]>(storageSharedChunkBuf());
  int filesBytes = fileKeys * SAMPLER_FILE_MAX;
  if (f.read(tmpFiles, filesBytes) != filesBytes) return true;

  uint8_t dirRemap[NUM_SAMPLER_DIRS];
  for (int d = 0; d < NUM_SAMPLER_DIRS; d++) {
    dirRemap[d] = (uint8_t)d;
  }
  uint64_t dirMagicPos = f.curPosition();
  char dirMagic[4] = {};
  int rdMagic = f.read(dirMagic, sizeof(dirMagic));
  if (rdMagic == (int)sizeof(dirMagic) && memcmp(dirMagic, "SMD2", 4) == 0) {
    for (int d = 0; d < NUM_SAMPLER_DIRS; d++) {
      char dirBuf[SAMPLER_DIR_MAX] = {};
      if (f.read(dirBuf, sizeof(dirBuf)) != (int)sizeof(dirBuf)) break;
      dirBuf[SAMPLER_DIR_MAX - 1] = '\0';
      bool usedDir = false;
      for (int k = 0; k < fileKeys; k++) {
        if (tmpFiles[k][0] != '\0' && tmpDir[k] == (uint8_t)d) {
          usedDir = true;
          break;
        }
      }
      dirRemap[d] = (usedDir && dirBuf[0] != '\0') ? allocOrFindSamplerDir(dirBuf) : 0xFFu;
    }
  } else if (rdMagic > 0) {
    f.seekSet(dirMagicPos);
  }

  int keyBase = (fileKeys == 18) ? 12 : 0; // Map legacy 18-key (C4..F5) presets to C4..F5
  bool dirPoolOverflow = false;
  for (int k = 0; k < fileKeys && (keyBase + k) < SAMPLER_NUM_KEYS; k++) {
    tmpFiles[k][SAMPLER_FILE_MAX - 1] = '\0';
    int dstK = keyBase + k;
    if (tmpFiles[k][0] == '\0') {
      samplerKeyDirIdx[instIdx][dstK] = 0xFFu;
      samplerKeyFile[instIdx][dstK][0] = '\0';
      continue;
    }
    uint8_t rawDir = tmpDir[k];
    if (rawDir < NUM_SAMPLER_DIRS && dirRemap[rawDir] == SAMPLER_DIR_POOL_FULL) {
      dirPoolOverflow = true;
      samplerKeyDirIdx[instIdx][dstK] = 0xFFu;
      samplerKeyFile[instIdx][dstK][0] = '\0';
      continue;
    }
    samplerKeyDirIdx[instIdx][dstK] = (rawDir < NUM_SAMPLER_DIRS) ? dirRemap[rawDir] : 0xFFu;
    memcpy(samplerKeyFile[instIdx][dstK], tmpFiles[k], SAMPLER_FILE_MAX);
  }
  rebuildSamplerSlotMask(instIdx);
  if (dirPoolOverflow) {
    uiShowToast("DIR POOL FULL");
    uiForceRedraw();
  }
  return true;
}

// ============================================================================
// SD Card Sample Browser Modal Implementation (32-Entry Sliding Window)
// ============================================================================
FLASHMEM static bool hasWavExtension(const char *name) {
  if (!name) return false;
  size_t len = strlen(name);
  if (len < 5) return false;
  return strcasecmp(name + len - 4, ".wav") == 0;
}

FLASHMEM static bool acceptSamplerWavFile(const char *name, uint64_t) {
  return hasWavExtension(name);
}

FLASHMEM static void scanBrowserDirectoryWindow(const SamplerBrowserEntry *pivot, int pivotIdx, int wantStart,
                                                const char *matchFile = nullptr, int *outMatchIdx = nullptr,
                                                SamplerBrowserEntry *outMatchEntry = nullptr) {
  if (!SD.sdfs.exists(ROOT_DIR)) {
    SD.sdfs.mkdir(ROOT_DIR);
  }
  if (!SD.sdfs.exists(browserDir)) {
    snprintf(browserDir, sizeof(browserDir), "%s", ROOT_DIR);
  }
  pagedScanDirWindow<SamplerBrowserEntry>(
      browserDir, browserEntries, MAX_BROWSER_ENTRIES,
      browserEntryCount, browserWindowStart, browserWindowCount,
      acceptSamplerWavFile, pivot, pivotIdx, wantStart,
      matchFile, outMatchIdx, outMatchEntry);
}

FLASHMEM static void rescanSamplerBrowserWindow(const SamplerBrowserEntry *pivot, int pivotIdx, int wantStart) {
  scanBrowserDirectoryWindow(pivot, pivotIdx, wantStart);
}

FLASHMEM static void scanBrowserDirectory() {
  browserCursor = 0;
  browserScroll = 0;
  scanBrowserDirectoryWindow(nullptr, 0, 0);
}

FLASHMEM bool isSamplerBrowserOpen() {
  return browserOpen;
}

FLASHMEM int samplerBrowserTargetKey() {
  return browserKeyIdx;
}

FLASHMEM const char *samplerBrowserCurrentDir() {
  return browserDir;
}

FLASHMEM int samplerBrowserEntryCount() {
  return browserEntryCount;
}

FLASHMEM int samplerBrowserCursor() {
  return browserCursor;
}

FLASHMEM int samplerBrowserScroll() {
  return browserScroll;
}

FLASHMEM const SamplerBrowserEntry *samplerBrowserEntryAt(int idx) {
  if (idx < browserWindowStart || idx >= browserWindowStart + browserWindowCount) return nullptr;
  return &browserEntries[idx - browserWindowStart];
}

FLASHMEM static void syncBrowserScroll() {
  pagedSyncWindowScroll(browserEntryCount, SAMPLER_BROWSER_VISIBLE_ROWS, MAX_BROWSER_ENTRIES,
                        browserCursor, browserScroll, browserWindowStart, browserWindowCount,
                        browserEntries, rescanSamplerBrowserWindow);
}

FLASHMEM void openSamplerBrowser(int instIdx, int keyIdx) {
  browserInstIdx = constrain(instIdx, 0, NUM_INSTRUMENTS - 1);
  browserKeyIdx = constrain(keyIdx, 0, SAMPLER_NUM_KEYS - 1);

  if (!SD.sdfs.exists(ROOT_DIR)) {
    SD.sdfs.mkdir(ROOT_DIR);
  }

  // If the key already has an assigned sample, start in its directory and highlight it
  const char *selectedFile = nullptr;
  char fullAssigned[SAMPLER_PATH_MAX] = {};
  if (samplerHasSample(browserInstIdx, browserKeyIdx)) {
    resolveKeyFullPath(browserInstIdx, browserKeyIdx, fullAssigned, sizeof(fullAssigned));
    char *lastSlash = strrchr(fullAssigned, '/');
    if (lastSlash) {
      selectedFile = lastSlash + 1;
      if (lastSlash == fullAssigned) {
        snprintf(browserDir, sizeof(browserDir), "/");
      } else {
        *lastSlash = '\0';
        if (SD.sdfs.exists(fullAssigned)) {
          snprintf(browserDir, sizeof(browserDir), "%s", fullAssigned);
        }
      }
    }
  } else if (browserDir[0] == '\0' || !SD.sdfs.exists(browserDir)) {
    snprintf(browserDir, sizeof(browserDir), "%s", ROOT_DIR);
  }

  int matchedIdx = -1;
  SamplerBrowserEntry matchedEntry{};
  browserCursor = 0;
  browserScroll = 0;
  scanBrowserDirectoryWindow(nullptr, 0, 0, selectedFile, &matchedIdx, &matchedEntry);

  if (matchedIdx >= 0) {
    browserCursor = matchedIdx;
    if (matchedIdx >= browserWindowCount) {
      int maxScroll = max(0, browserEntryCount - SAMPLER_BROWSER_VISIBLE_ROWS);
      browserScroll = constrain(browserCursor - SAMPLER_BROWSER_VISIBLE_ROWS + 1, 0, maxScroll);
      int maxWinStart = max(0, browserEntryCount - MAX_BROWSER_ENTRIES);
      int wantStart = constrain(browserScroll - 12, 0, maxWinStart);
      scanBrowserDirectoryWindow(&matchedEntry, matchedIdx, wantStart);
    }
  } else {
    // Default cursor to first .WAV file or first folder after ".."
    if (browserEntryCount > 1 && browserWindowCount > 0 && strcmp(browserEntries[0].name, "..") == 0) {
      browserCursor = 1;
    }
  }
  syncBrowserScroll();
  browserOpen = true;
}

FLASHMEM void closeSamplerBrowser() {
  browserOpen = false;
}

FLASHMEM void samplerBrowserMove(int dy, int pageDelta) {
  if (!browserOpen || browserEntryCount <= 0) return;
  int step = dy + pageDelta * SAMPLER_BROWSER_VISIBLE_ROWS;
  if (step == 0) return;
  browserCursor = constrain(browserCursor + step, 0, browserEntryCount - 1);
  syncBrowserScroll();
}

FLASHMEM bool samplerBrowserConfirm() {
  if (!browserOpen || browserEntryCount <= 0) return false;
  const int idx = constrain(browserCursor, 0, browserEntryCount - 1);
  const SamplerBrowserEntry *ent = samplerBrowserEntryAt(idx);
  if (!ent) return false;
  char entName[sizeof(browserEntries[0].name)];
  const bool isDir = (ent->isDir != 0);
  memcpy(entName, ent->name, sizeof(entName));
  entName[sizeof(entName) - 1] = '\0';

  if (isDir) {
    if (!pagedStepBrowserDir(browserDir, sizeof(browserDir), entName)) return false;
    scanBrowserDirectory();
    if (browserEntryCount > 1 && browserWindowCount > 0 && strcmp(browserEntries[0].name, "..") == 0) {
      browserCursor = 1;
    }
    syncBrowserScroll();
    return false;
  }

  // Selected a .WAV file: connect it to the target note
  char fullPath[SAMPLER_PATH_MAX];
  if (strcmp(browserDir, "/") == 0) {
    snprintf(fullPath, sizeof(fullPath), "/%.42s", entName);
  } else {
    snprintf(fullPath, sizeof(fullPath), "%.95s/%.42s", browserDir, entName);
  }
  if (!samplerAssignSample(browserInstIdx, browserKeyIdx, fullPath)) {
    return false;
  }
  browserOpen = false;
  return true;
}

// ============================================================================
// InstEngineOps Callbacks & Audio ISR Voice Renderer
// ============================================================================
FLASHMEM static const FmPatch &samplerDefaultPatch() {
  return SAMPLER_DEFAULT_PATCH;
}

FLASHMEM static void samplerValidatePatch(FmPatch &p) {
  p.type = (uint8_t)InstType::Sampler;
  p.algorithm = (uint8_t)constrain((int)p.algorithm, 0, NUM_SAMPLER_PRESS_MODES - 1);
}

static void samplerRenderVoice(FmVoice &v, const FmPatch &p, const FmInstRuntime &rt,
                               int32_t *outL, int32_t *outR, int numFrames) {
  (void)p;
  uint32_t sIdx = v.opPhase[0];
  uint32_t sGen = v.opPhase[1];
  if (sIdx >= (uint32_t)NUM_SAMPLER_STREAMS) {
    v.active = false;
    v.ampStage = EnvStage::Idle;
    return;
  }

  SamplerStream &st = samplerStreams[sIdx];
  if (!st.active || st.generation != sGen) {
    v.active = false;
    v.ampStage = EnvStage::Idle;
    return;
  }

  uint32_t stepFp = st.baseStepFp;
  if (rt.pitchBendRatio != 1.0f) {
    stepFp = max(1u, (uint32_t)((float)stepFp * rt.pitchBendRatio + 0.5f));
  }
  static constexpr float SAMPLER_VOICE_BASE_GAIN = 0.25f;
  const float voiceGain = v.velScale * SAMPLER_VOICE_BASE_GAIN;

  int startIdx = (v.startSampleOffset < (uint8_t)numFrames) ? (int)v.startSampleOffset : 0;
  v.startSampleOffset = 0;
  for (int i = startIdx; i < numFrames; i++) {
    if (!instAdvanceVoiceEnvelopes(v)) {
      st.active = false;
      st.prevOutL = 0;
      st.prevOutR = 0;
      st.declickRem = 0;
      break;
    }

    uint32_t t = st.tail;
    uint32_t h = st.head;
    uint32_t avail = h - t;
    float rawL = 0.0f;
    float rawR = 0.0f;

    if (avail == 0) {
      if (st.eofReached) {
        v.active = false;
        v.ampStage = EnvStage::Idle;
        st.active = false;
        st.prevOutL = 0;
        st.prevOutR = 0;
        st.declickRem = 0;
        break;
      }
      rawL = (float)st.lastL * (1.0f / 32768.0f);
      rawR = (float)st.lastR * (1.0f / 32768.0f);
    } else {
      uint32_t pk0 = st.ring[t & SAMPLER_RING_MASK];
      int16_t s0L = (int16_t)(pk0 & 0xFFFFu);
      int16_t s0R = (int16_t)(pk0 >> 16);
      int16_t s1L = s0L;
      int16_t s1R = s0R;
      if (avail >= 2) {
        uint32_t pk1 = st.ring[(t + 1) & SAMPLER_RING_MASK];
        s1L = (int16_t)(pk1 & 0xFFFFu);
        s1R = (int16_t)(pk1 >> 16);
      }
      float frac = (float)(st.fracFp & 0xFFFFu) * (1.0f / 65536.0f);
      float interpL = (float)s0L + frac * (float)(s1L - s0L);
      float interpR = (float)s0R + frac * (float)(s1R - s0R);
      st.lastL = (int16_t)interpL;
      st.lastR = (int16_t)interpR;
      rawL = interpL * (1.0f / 32768.0f);
      rawR = interpR * (1.0f / 32768.0f);

      st.fracFp += stepFp;
      uint32_t adv = st.fracFp >> 16;
      st.fracFp &= 0xFFFFu;
      if (adv > avail) adv = avail;
      st.tail = t + adv;
    }

    float outSampleL = 0.0f, outSampleR = 0.0f;
    instApplyVoiceStereoFilterAndAmp(v, rawL, rawR, voiceGain, outSampleL, outSampleR);
    int32_t outSL = (int32_t)(outSampleL * 32768.0f);
    int32_t outSR = (int32_t)(outSampleR * 32768.0f);
    if (st.declickRem > 0) {
      int32_t w = (int32_t)st.declickRem;
      outSL += ((int32_t)st.declickL * w) >> 5;
      outSR += ((int32_t)st.declickR * w) >> 5;
      st.declickRem--;
    }
    st.prevOutL = (int16_t)constrain(outSL, -32768, 32767);
    st.prevOutR = (int16_t)constrain(outSR, -32768, 32767);
    outL[i] += outSL;
    outR[i] += outSR;
  }
}

PROGMEM static const InstEngineOps SAMPLER_ENGINE_OPS = {
  InstType::Sampler,
  "SAMPLER",
  "SMP",
  "SMP",
  "PRESS",
  "KEYS",
  nullptr,
  samplerDefaultPatch,
  samplerValidatePatch,
  nullptr,
  nullptr,
  nullptr,
  samplerRenderVoice
};

const InstEngineOps &instSamplerEngine() {
  return SAMPLER_ENGINE_OPS;
}

}  // namespace looper
