// inst_sampler.cpp — SD Card WAV Sampler Instrument Engine (4-Octave Keyboard C3..B6)
#include "inst_sampler.h"
#include "storage.h"
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
// Lock-Free SD WAV Streaming Pool (4 concurrent stereo streams)
// ============================================================================
static constexpr int NUM_SAMPLER_STREAMS = 4;
static constexpr uint32_t SAMPLER_RING_FRAMES = 2048;
static constexpr uint32_t SAMPLER_RING_MASK = SAMPLER_RING_FRAMES - 1;

struct SamplerStream {
  FsFile file;
  int8_t cachedInstIdx = -1;
  int8_t cachedKeyIdx = -1;
  uint32_t dataOffset = 0;
  uint32_t totalFrames = 0;
  uint32_t sampleRate = SAMPLE_RATE_HZ;
  uint32_t baseStepFp = 65536u; // 16.16 fixed-point step (65536 = 1.0x at 44.1 kHz)
  uint8_t channels = 2;
  uint8_t bitsPerSample = 16;
  uint8_t audioFormat = 1;      // 1 = PCM, 3 = IEEE float
  uint8_t bytesPerFrame = 4;

  volatile bool active = false;
  volatile bool pendingVoice = false;
  volatile bool prewarmed = false;
  volatile bool eofReached = false;
  volatile uint8_t instIdx = 0;
  volatile uint8_t keyIdx = 0;
  volatile uint8_t voiceIdx = 0;
  volatile uint32_t generation = 0;
  volatile uint32_t head = 0;
  volatile uint32_t tail = 0;
  uint32_t fileFrame = 0;
  uint32_t fracFp = 0;
  int16_t lastL = 0;
  int16_t lastR = 0;

  uint32_t ring[SAMPLER_RING_FRAMES];
};

static SamplerStream samplerStreams[NUM_SAMPLER_STREAMS];
static uint32_t streamGenCounter = 1;
static volatile int8_t fillingStreamIdx = -1;

// ============================================================================
// Sample Browser Modal State (32-Entry Sliding Window)
// ============================================================================
static constexpr int MAX_BROWSER_ENTRIES = 32;
static SamplerBrowserEntry *const browserEntries = reinterpret_cast<SamplerBrowserEntry *>(sharedBrowserPoolBuf());
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

int samplerNoteToKey(uint8_t midiNote) {
  int k = (int)midiNote - (int)SAMPLER_BASE_NOTE;
  if (k >= 0 && k < SAMPLER_NUM_KEYS) return k;
  return -1;
}

bool samplerHasSample(int instIdx, int keyIdx) {
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

int samplerNoteToKeyForInst(int instIdx, uint8_t midiNote) {
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
  snprintf(samplerDirs[0], SAMPLER_DIR_MAX, "%s", dirPath);
  return 0u;
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
    if ((samplerStreams[s].active || samplerStreams[s].pendingVoice) &&
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
    if ((samplerStreams[s].active || samplerStreams[s].pendingVoice) &&
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
      interrupts();
      if (st.file.isOpen()) {
        st.file.close();
      }
      st.cachedInstIdx = -1;
      st.cachedKeyIdx = -1;
      st.totalFrames = 0;
    }
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

FLASHMEM static void fillSamplerStream(SamplerStream &st, int maxPasses, bool forceHeld = false) {
  if (!st.file.isOpen() || st.totalFrames == 0 || st.bytesPerFrame == 0) {
    st.eofReached = true;
    return;
  }

  // Stage multi-sector SD reads (up to 4,096 bytes = 8 sectors) through the shared RAM1 chunk buffer
  // for DMA_SDIO cache safety and low SDIO command overhead.
  static constexpr size_t SAMPLER_READ_BUF_BYTES = 4096;
  uint8_t *rawBuf = reinterpret_cast<uint8_t *>(storageSharedChunkBuf());
  const uint32_t maxFramesPerRead = max(1u, (uint32_t)(SAMPLER_READ_BUF_BYTES / st.bytesPerFrame));

  const uint32_t minFree = forceHeld ? 64u : 512u;
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
        // Wait until ring buffer has <= 144 frames (~3.2 ms) before wrapping so
        // releasing the key during the sample exits the loop cleanly at the end
        if (queued > 144 && st.totalFrames > 144) {
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

FLASHMEM static bool prepareSamplerStreamSlow(int instIdx, int keyIdx, uint32_t &outStreamIdx, uint32_t &outStreamGen) {
  int chosen = -1;
  // 1. Prefer an existing stream slot that already has this exact (instIdx, keyIdx) cached
  for (int s = 0; s < NUM_SAMPLER_STREAMS; s++) {
    if (samplerStreams[s].cachedInstIdx == instIdx &&
        samplerStreams[s].cachedKeyIdx == keyIdx &&
        samplerStreams[s].file.isOpen()) {
      chosen = s;
      break;
    }
  }

  // 2. Otherwise pick an idle stream slot
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
    chosen = 0;
    uint32_t minGen = samplerStreams[0].generation;
    for (int s = 1; s < NUM_SAMPLER_STREAMS; s++) {
      if (samplerStreams[s].generation < minGen) {
        minGen = samplerStreams[s].generation;
        chosen = s;
      }
    }
  }

  SamplerStream &st = samplerStreams[chosen];
  fillingStreamIdx = (int8_t)chosen;
  noInterrupts();
  st.active = false;
  st.pendingVoice = false;
  st.prewarmed = false;
  interrupts();

  if (!st.file.isOpen() || st.cachedInstIdx != instIdx || st.cachedKeyIdx != keyIdx || st.totalFrames == 0) {
    if (st.file.isOpen()) {
      st.file.close();
    }
    st.cachedInstIdx = -1;
    st.cachedKeyIdx = -1;
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
  uint32_t gen = (streamGenCounter & 0xFFFFu) + 1u;
  streamGenCounter = gen;
  st.instIdx = (uint8_t)instIdx;
  st.keyIdx = (uint8_t)keyIdx;
  st.generation = gen;
  st.head = 0;
  st.tail = 0;
  st.fileFrame = 0;
  st.fracFp = 0;
  st.lastL = 0;
  st.lastR = 0;
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

bool samplerPrepareVoiceOn(int instIdx, uint8_t note, uint32_t &outStreamIdx, uint32_t &outStreamGen, bool allowSdIo) {
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
  for (int s = 0; s < NUM_SAMPLER_STREAMS; s++) {
    if (s == (int)fillingStreamIdx) continue;
    SamplerStream &st = samplerStreams[s];
    if (st.cachedInstIdx == instIdx &&
        st.cachedKeyIdx == keyIdx &&
        st.totalFrames > 0 &&
        st.head > 0 &&
        st.head <= SAMPLER_RING_FRAMES &&
        st.fileFrame == st.head) {
      uint32_t gen = (streamGenCounter & 0xFFFFu) + 1u;
      streamGenCounter = gen;
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

FLASHMEM void samplerServiceActiveStreams() {
  if (fillingStreamIdx >= 0) return;
  for (int s = 0; s < NUM_SAMPLER_STREAMS; s++) {
    SamplerStream &st = samplerStreams[s];
    if (st.pendingVoice) {
      if (!st.eofReached && (st.head - st.tail) < (SAMPLER_RING_FRAMES - 256u)) {
        fillingStreamIdx = (int8_t)s;
        fillSamplerStream(st, 2, true);
        fillingStreamIdx = -1;
      }
      continue;
    }
    if (!st.active) continue;
    const FmVoice *v = fmSynthVoiceAt(st.voiceIdx);
    if (!v || !v->active || v->opPhase[0] != (uint32_t)s || v->opPhase[1] != st.generation) {
      st.active = false;
      continue;
    }
    if (!st.eofReached) {
      fillingStreamIdx = (int8_t)s;
      fillSamplerStream(st, 3, false);
      fillingStreamIdx = -1;
    }
  }
}

FLASHMEM void samplerServiceStreams() {
  if (fillingStreamIdx >= 0) return;
  samplerServiceActiveStreams();

  // Pre-warm at most 1 idle stream slot per pass so the first 4 Sampler keys trigger in ISR with 0 SD I/O
  for (int s = 0; s < NUM_SAMPLER_STREAMS; s++) {
    SamplerStream &st = samplerStreams[s];
    if (st.active || st.pendingVoice || st.prewarmed) continue;

    if (st.file.isOpen() && st.cachedInstIdx >= 0 && st.cachedKeyIdx >= 0 && st.totalFrames > 0) {
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

    // Slot has no file open yet: look for an uncached populated key on any Sampler instrument
    for (int inst = 0; inst < NUM_INSTRUMENTS; inst++) {
      if ((InstType)fmSynthPatch(inst).type != InstType::Sampler) continue;
      uint64_t availMask = samplerPopulatedMask[inst] & ~samplerFailedMask[inst];
      if (!availMask) continue;
      for (int k = 0; k < SAMPLER_NUM_KEYS; k++) {
        if ((availMask & (1ULL << k)) == 0) continue;
        bool alreadyCached = false;
        for (int j = 0; j < NUM_SAMPLER_STREAMS; j++) {
          if (samplerStreams[j].cachedInstIdx == inst &&
              samplerStreams[j].cachedKeyIdx == k &&
              samplerStreams[j].file.isOpen()) {
            alreadyCached = true;
            break;
          }
        }
        if (alreadyCached) continue;

        fillingStreamIdx = (int8_t)s;
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

void samplerSilenceInst(int instIdx) {
  for (int s = 0; s < NUM_SAMPLER_STREAMS; s++) {
    if ((int)samplerStreams[s].instIdx == instIdx &&
        (samplerStreams[s].active || samplerStreams[s].pendingVoice)) {
      samplerStreams[s].active = false;
      samplerStreams[s].pendingVoice = false;
    }
  }
}

void samplerStopAllStreams() {
  for (int s = 0; s < NUM_SAMPLER_STREAMS; s++) {
    samplerStreams[s].active = false;
    samplerStreams[s].pendingVoice = false;
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
    samplerKeyDirIdx[instIdx][keyIdx] = allocOrFindSamplerDir(dirPart);
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
  for (int k = 0; k < fileKeys && (keyBase + k) < SAMPLER_NUM_KEYS; k++) {
    tmpFiles[k][SAMPLER_FILE_MAX - 1] = '\0';
    int dstK = keyBase + k;
    if (tmpFiles[k][0] == '\0') {
      samplerKeyDirIdx[instIdx][dstK] = 0xFFu;
      samplerKeyFile[instIdx][dstK][0] = '\0';
      continue;
    }
    uint8_t rawDir = tmpDir[k];
    samplerKeyDirIdx[instIdx][dstK] = (rawDir < NUM_SAMPLER_DIRS) ? dirRemap[rawDir] : 0xFFu;
    memcpy(samplerKeyFile[instIdx][dstK], tmpFiles[k], SAMPLER_FILE_MAX);
  }
  rebuildSamplerSlotMask(instIdx);
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

static inline bool samplerEntryComesBefore(const SamplerBrowserEntry &a, const SamplerBrowserEntry &b) {
  bool aParent = (a.isDir != 0) && (strcmp(a.name, "..") == 0);
  bool bParent = (b.isDir != 0) && (strcmp(b.name, "..") == 0);
  if (aParent != bParent) return aParent;
  if ((a.isDir != 0) != (b.isDir != 0)) return a.isDir != 0;
  int cmp = strcasecmp(a.name, b.name);
  if (cmp != 0) return cmp < 0;
  return strcmp(a.name, b.name) < 0;
}

FLASHMEM static void insertSamplerSmallest(SamplerBrowserEntry *arr, int &count, int cap,
                                           const SamplerBrowserEntry &item) {
  if (cap <= 0) return;
  if (count >= cap && !samplerEntryComesBefore(item, arr[cap - 1])) return;
  int ins = (count < cap) ? count++ : (cap - 1);
  while (ins > 0 && samplerEntryComesBefore(item, arr[ins - 1])) {
    arr[ins] = arr[ins - 1];
    ins--;
  }
  arr[ins] = item;
}

FLASHMEM static void insertSamplerLargest(SamplerBrowserEntry *arr, int &count, int cap,
                                          const SamplerBrowserEntry &item) {
  if (cap <= 0) return;
  if (count >= cap && !samplerEntryComesBefore(arr[0], item)) return;
  if (count < cap) {
    int ins = count++;
    while (ins > 0 && samplerEntryComesBefore(item, arr[ins - 1])) {
      arr[ins] = arr[ins - 1];
      ins--;
    }
    arr[ins] = item;
  } else {
    int ins = 0;
    while (ins + 1 < cap && samplerEntryComesBefore(arr[ins + 1], item)) {
      arr[ins] = arr[ins + 1];
      ins++;
    }
    arr[ins] = item;
  }
}

FLASHMEM static void scanBrowserDirectoryWindow(const SamplerBrowserEntry *pivot, int pivotIdx, int wantStart,
                                                const char *matchFile = nullptr, int *outMatchIdx = nullptr,
                                                SamplerBrowserEntry *outMatchEntry = nullptr) {
  if (outMatchIdx) *outMatchIdx = -1;

  if (!SD.sdfs.exists(ROOT_DIR)) {
    SD.sdfs.mkdir(ROOT_DIR);
  }
  if (!SD.sdfs.exists(browserDir)) {
    snprintf(browserDir, sizeof(browserDir), "%s", ROOT_DIR);
  }

  SamplerBrowserEntry pivotCopy{};
  const bool usePivot = (pivot != nullptr);
  int pOff = 0;
  int leftCap = 0;
  int rightCap = MAX_BROWSER_ENTRIES;
  int leftCount = 0;
  int rightCount = 0;
  int totalCount = 0;

  if (usePivot) {
    pivotCopy = *pivot;
    pOff = constrain(pivotIdx - wantStart, 0, MAX_BROWSER_ENTRIES - 1);
    leftCap = pOff;
    rightCap = (MAX_BROWSER_ENTRIES - 1) - pOff;
  }

  SamplerBrowserEntry matchTarget{};
  const bool hasMatchTarget = (matchFile != nullptr && matchFile[0] != '\0' &&
                               strlen(matchFile) < sizeof(matchTarget.name));
  int matchRank = 0;
  bool matchFound = false;
  if (hasMatchTarget) {
    snprintf(matchTarget.name, sizeof(matchTarget.name), "%s", matchFile);
    matchTarget.isDir = 0;
  }

  auto considerItem = [&](const SamplerBrowserEntry &item) {
    totalCount++;
    if (hasMatchTarget) {
      if (!item.isDir && strcasecmp(item.name, matchTarget.name) == 0) {
        matchFound = true;
        if (outMatchEntry) *outMatchEntry = item;
      } else if (samplerEntryComesBefore(item, matchTarget)) {
        matchRank++;
      }
    }
    if (!usePivot) {
      insertSamplerSmallest(browserEntries, rightCount, rightCap, item);
    } else if (samplerEntryComesBefore(item, pivotCopy)) {
      insertSamplerLargest(browserEntries, leftCount, leftCap, item);
    } else if (samplerEntryComesBefore(pivotCopy, item)) {
      insertSamplerSmallest(&browserEntries[pOff + 1], rightCount, rightCap, item);
    }
  };

  // Add ".." parent folder entry whenever not at SD root "/"
  if (strcmp(browserDir, "/") != 0) {
    SamplerBrowserEntry parentItem{};
    snprintf(parentItem.name, sizeof(parentItem.name), "..");
    parentItem.isDir = 1;
    considerItem(parentItem);
  }

  FsFile dir = SD.sdfs.open(browserDir, O_RDONLY);
  if (dir.isOpen() && dir.isDir()) {
    FsFile entry;
    while (entry.openNext(&dir, O_RDONLY)) {
      char nameBuf[64] = {};
      entry.getName(nameBuf, sizeof(nameBuf));
      bool isSubDir = entry.isDir();
      bool isHidden = entry.isHidden() || (nameBuf[0] == '.') ||
                      (strcasecmp(nameBuf, "System Volume Information") == 0);
      entry.close();

      if (isHidden || nameBuf[0] == '\0') continue;
      if (!isSubDir && !hasWavExtension(nameBuf)) continue;
      if (strlen(nameBuf) >= sizeof(browserEntries[0].name)) continue;

      SamplerBrowserEntry item{};
      snprintf(item.name, sizeof(item.name), "%s", nameBuf);
      item.isDir = isSubDir ? 1 : 0;
      considerItem(item);
      keepStreaming();
    }
  }
  if (dir.isOpen()) dir.close();

  browserEntryCount = totalCount;
  if (!usePivot) {
    browserWindowStart = 0;
    browserWindowCount = rightCount;
  } else {
    browserEntries[leftCount] = pivotCopy;
    if (leftCount < pOff && rightCount > 0) {
      memmove(&browserEntries[leftCount + 1], &browserEntries[pOff + 1],
              (size_t)rightCount * sizeof(SamplerBrowserEntry));
    }
    browserWindowStart = max(0, pivotIdx - leftCount);
    browserWindowCount = leftCount + 1 + rightCount;
  }

  if (matchFound && outMatchIdx) {
    *outMatchIdx = matchRank;
  }
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
  if (browserEntryCount <= 0) {
    browserCursor = 0;
    browserScroll = 0;
    browserWindowStart = 0;
    browserWindowCount = 0;
    return;
  }
  browserCursor = constrain(browserCursor, 0, browserEntryCount - 1);
  if (browserCursor < browserScroll) {
    browserScroll = browserCursor;
  } else if (browserCursor >= browserScroll + SAMPLER_BROWSER_VISIBLE_ROWS) {
    browserScroll = browserCursor - SAMPLER_BROWSER_VISIBLE_ROWS + 1;
  }
  int maxScroll = max(0, browserEntryCount - SAMPLER_BROWSER_VISIBLE_ROWS);
  browserScroll = constrain(browserScroll, 0, maxScroll);

  int visEnd = min(browserEntryCount - 1, browserScroll + SAMPLER_BROWSER_VISIBLE_ROWS - 1);
  if (browserScroll < browserWindowStart || visEnd >= browserWindowStart + browserWindowCount) {
    int maxWinStart = max(0, browserEntryCount - MAX_BROWSER_ENTRIES);
    int wantStart = constrain(browserScroll - 12, 0, maxWinStart);
    if (wantStart == 0) {
      scanBrowserDirectoryWindow(nullptr, 0, 0);
    } else {
      int overlapLo = max(browserWindowStart, wantStart);
      int overlapHi = min(browserWindowStart + browserWindowCount - 1, wantStart + MAX_BROWSER_ENTRIES - 1);
      if (browserWindowCount > 0 && overlapLo <= overlapHi) {
        int pivotIdx = (wantStart < browserWindowStart) ? overlapHi : overlapLo;
        SamplerBrowserEntry pivot = browserEntries[pivotIdx - browserWindowStart];
        scanBrowserDirectoryWindow(&pivot, pivotIdx, wantStart);
      } else {
        scanBrowserDirectoryWindow(nullptr, 0, 0);
      }
    }
  }
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
    if (strcmp(entName, "..") == 0) {
      char *lastSlash = strrchr(browserDir, '/');
      if (!lastSlash || lastSlash == browserDir) {
        snprintf(browserDir, sizeof(browserDir), "/");
      } else {
        *lastSlash = '\0';
      }
    } else {
      char nextDir[SAMPLER_DIR_MAX];
      if (strcmp(browserDir, "/") == 0) {
        snprintf(nextDir, sizeof(nextDir), "/%.42s", entName);
      } else {
        snprintf(nextDir, sizeof(nextDir), "%.51s/%.42s", browserDir, entName);
      }
      snprintf(browserDir, sizeof(browserDir), "%s", nextDir);
    }
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
  samplerAssignSample(browserInstIdx, browserKeyIdx, fullPath);
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
  const float voiceGain = v.velScale * 0.90f;

  int startIdx = (v.startSampleOffset < (uint8_t)numFrames) ? (int)v.startSampleOffset : 0;
  v.startSampleOffset = 0;
  for (int i = startIdx; i < numFrames; i++) {
    if (!instAdvanceVoiceEnvelopes(v)) {
      st.active = false;
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
    outL[i] += (int32_t)(outSampleL * 32768.0f);
    outR[i] += (int32_t)(outSampleR * 32768.0f);
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

FLASHMEM const InstEngineOps &instSamplerEngine() {
  return SAMPLER_ENGINE_OPS;
}

}  // namespace looper
