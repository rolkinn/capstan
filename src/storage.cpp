// storage.cpp — SD card streaming, 666s linear WAV recording/overdubbing, and manual song persistence
#include "storage.h"
#include "midi_map.h"
#include "instruments.h"
#include "inst_sampler.h"
#include "app.h"
#include "ui.h"
#include "pages.h"
#include "page_instrument_edit.h"
#include "input.h"
#include "usb_drive.h"
#include "track_edit.h"
#include <Arduino.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <math.h>

namespace looper {

volatile const char *debugStage = "init";

// IMPORTANT: chunk[] MUST reside in RAM1 (not DMAMEM) because SdFat's SDIO DMA
// engine transfers directly to/from it without cache maintenance.
alignas(32) static uint32_t chunk[CHUNK_FRAMES];

uint32_t *storageSharedChunkBuf() {
  return chunk;
}

// List of saved song folder names in RAM2 (DMAMEM) — never passed directly to SdFat DMA
DMAMEM static char songList[MAX_SONGS][MAX_SONG_NAME_LEN + 1];
static int numSavedSongs = 0;

static char activeSongName[MAX_SONG_NAME_LEN + 1] = "";
static char lastActiveSongName[MAX_SONG_NAME_LEN + 1] = "";
static bool songSaved = false;
static bool songDirty = false;

void markSongDirty() {
  songDirty = true;
}

bool songIsSaved() {
  return songSaved;
}

bool songHasUnsavedChanges() {
  return songDirty;
}

const char *currentSongName() {
  return activeSongName;
}

int songCount() {
  return numSavedSongs;
}

const char *songNameAt(int index) {
  if (index < 0 || index >= numSavedSongs) return "";
  return songList[index];
}

FLASHMEM int findSongIndex(const char *name) {
  if (!name || !name[0]) return -1;
  for (int i = 0; i < numSavedSongs; i++) {
    if (strcasecmp(songList[i], name) == 0) return i;
  }
  return -1;
}

// Sanitize a user-supplied song or instrument identifier (uppercase A-Z, 0-9, '_', '-')
FLASHMEM void sanitizeIdentifierName(const char *in, char *out, size_t outSz, bool trimTrailingPunct) {
  if (outSz == 0) return;
  size_t maxLen = min(outSz - 1, (size_t)MAX_SONG_NAME_LEN);
  size_t w = 0;
  if (in) {
    for (size_t i = 0; in[i] != '\0' && w < maxLen; i++) {
      char c = in[i];
      if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
      if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-') {
        out[w++] = c;
      } else if (c == ' ') {
        out[w++] = '_';
      }
    }
  }
  if (trimTrailingPunct) {
    while (w > 0 && (out[w - 1] == '_' || out[w - 1] == '-')) {
      w--;
    }
  }
  out[w] = '\0';
}

FLASHMEM static void sanitizeSongName(const char *in, char *out, size_t outSz) {
  sanitizeIdentifierName(in, out, outSz, true);
}

FLASHMEM static void ensureSongsRootDir() {
  if (!SD.sdfs.exists(ROOT_DIR)) SD.sdfs.mkdir(ROOT_DIR);
  if (!SD.sdfs.exists(SONGS_DIR)) SD.sdfs.mkdir(SONGS_DIR);
  if (!SD.sdfs.exists(PROJECTS_DIR)) SD.sdfs.mkdir(PROJECTS_DIR);
}

FLASHMEM static void songDirByName(const char *name, char *out, size_t sz) {
  snprintf(out, sz, "%s/%s", SONGS_DIR, name);
}

FLASHMEM static void audioDirByName(const char *name, char *out, size_t sz) {
  snprintf(out, sz, "%s/%s/AUDIO", SONGS_DIR, name);
}

FLASHMEM static void ensureSongAudioDir(const char *name) {
  ensureSongsRootDir();
  char dir[64], aDir[64];
  songDirByName(name, dir, sizeof(dir));
  if (!SD.sdfs.exists(dir)) SD.sdfs.mkdir(dir);
  audioDirByName(name, aDir, sizeof(aDir));
  if (!SD.sdfs.exists(aDir)) SD.sdfs.mkdir(aDir);
}

FLASHMEM static void trackWavByName(const char *name, int t, char *out, size_t sz) {
  snprintf(out, sz, "%s/%s/AUDIO/TRACK%d.WAV", SONGS_DIR, name, t + 1);
}

FLASHMEM static void songDatByName(const char *name, char *out, size_t sz) {
  snprintf(out, sz, "%s/%s/SONG.DAT", SONGS_DIR, name);
}

FLASHMEM static void midiDatByName(const char *name, char *out, size_t sz) {
  snprintf(out, sz, "%s/%s/MIDI.DAT", SONGS_DIR, name);
}

FLASHMEM static void instDatByName(const char *name, char *out, size_t sz) {
  snprintf(out, sz, "%s/%s/INST.DAT", SONGS_DIR, name);
}

FLASHMEM static void nextDefaultSongName(char *out, size_t sz) {
  for (int n = 1; n <= 999; n++) {
    char cand[MAX_SONG_NAME_LEN + 1];
    snprintf(cand, sizeof(cand), "SONG_%03u", (unsigned)(n % 1000));
    if (findSongIndex(cand) < 0) {
      char dPath[48];
      songDirByName(cand, dPath, sizeof(dPath));
      if (!SD.sdfs.exists(dPath)) {
        memcpy(out, cand, min(sz, sizeof(cand)));
        if (sz > 0) out[sz - 1] = '\0';
        return;
      }
    }
  }
  snprintf(out, sz, "SONG_001");
}

static inline uint64_t trackWavByteOffset(int t, uint32_t frame) {
  return (uint64_t)WAV_DATA_OFFSET + (uint64_t)frame * audioTrackBytesPerFrame(t);
}

// Write a standard 44-byte RIFF WAVE header padded to WAV_DATA_OFFSET (512 bytes)
FLASHMEM static bool writeWavHeader(FsFile &f, uint32_t frames, int trackIdx = 0) {
  if (!f.isOpen()) return false;
  uint8_t hdr[WAV_DATA_OFFSET];
  memset(hdr, 0, sizeof(hdr));

  uint16_t numCh = (uint16_t)audioTrackChannels(trackIdx);
  uint16_t blockAlign = (uint16_t)audioTrackBytesPerFrame(trackIdx);
  uint32_t byteRate = SAMPLE_RATE_HZ * (uint32_t)blockAlign;
  uint32_t dataBytes = frames * (uint32_t)blockAlign;
  uint32_t riffSize = (WAV_DATA_OFFSET - 8) + dataBytes;
  uint32_t junkSize = WAV_DATA_OFFSET - 44 - 8;

  auto put32 = [&](int off, uint32_t v) {
    hdr[off + 0] = (uint8_t)(v & 0xFF);
    hdr[off + 1] = (uint8_t)((v >> 8) & 0xFF);
    hdr[off + 2] = (uint8_t)((v >> 16) & 0xFF);
    hdr[off + 3] = (uint8_t)((v >> 24) & 0xFF);
  };
  auto put16 = [&](int off, uint16_t v) {
    hdr[off + 0] = (uint8_t)(v & 0xFF);
    hdr[off + 1] = (uint8_t)((v >> 8) & 0xFF);
  };

  memcpy(hdr + 0, "RIFF", 4);
  put32(4, riffSize);
  memcpy(hdr + 8, "WAVE", 4);
  memcpy(hdr + 12, "fmt ", 4);
  put32(16, 16);                 // PCM fmt chunk size
  put16(20, 1);                  // AudioFormat = 1 (PCM)
  put16(22, numCh);              // NumChannels = 2 (Stereo) or 1 (Mono)
  put32(24, SAMPLE_RATE_HZ);     // SampleRate = 44100
  put32(28, byteRate);           // ByteRate = 176400 (Stereo) or 88200 (Mono)
  put16(32, blockAlign);         // BlockAlign = 4 (Stereo) or 2 (Mono)
  put16(34, 16);                 // BitsPerSample = 16
  memcpy(hdr + 36, "JUNK", 4);
  put32(40, junkSize);
  memcpy(hdr + WAV_DATA_OFFSET - 8, "data", 4);
  put32(WAV_DATA_OFFSET - 4, dataBytes);

  if (!f.seekSet(0)) return false;
  return f.write(hdr, sizeof(hdr)) == (int)sizeof(hdr);
}

FLASHMEM bool cardResponds() {
  return SD.sdfs.card() && SD.sdfs.card()->status() != 0xFFFFFFFFu;
}

FLASHMEM bool cardRestart() {
  return SD.sdfs.begin(SdioConfig(DMA_SDIO)) && cardResponds();
}

FLASHMEM void storageForget() {
  for (int t = 0; t < NUM_AUDIO_TRACKS; t++) {
    tracks[t].file = FsFile();
    tracks[t].state = TrackState::Empty;
    tracks[t].solo = false;
    tracks[t].recordedFrames = 0;
    tracks[t].live = false;
    tracks[t].head = tracks[t].tail = 0;
    memset(tracks[t].wavePeaks, 0, sizeof(tracks[t].wavePeaks));
    tracks[t].overviewVersion++;
  }
}

FLASHMEM uint32_t songMaxFrames() {
  uint32_t minSpan = beatsToFrames(16 * BEATS_PER_BAR, bpm); // at least 16 bars visible
  uint32_t m = max(minSpan, transport.tapeFrame + SAMPLE_RATE_HZ * 4);
  for (int t = 0; t < NUM_AUDIO_TRACKS; t++) {
    if (tracks[t].recordedFrames > m) m = tracks[t].recordedFrames;
  }
  for (int t = 0; t < NUM_MIDI_TRACKS; t++) {
    uint32_t cnt = 0;
    const MidiEvent *evs = midiSeqEvents(t, cnt);
    if (cnt > 0 && evs) {
      uint32_t endF = midiTicksToFrames(evs[cnt - 1].tick, bpm);
      if (endF > m) m = endF;
    }
  }
  if ((transport.punchEnabled || transport.loopEnabled) && transport.punchOutFrame > m) {
    m = transport.punchOutFrame;
  }
  return min(m, MAX_REC_FRAMES);
}

// Update waveform overview columns for a newly written chunk of frames
FLASHMEM static void updateTrackOverviewChunk(int t, uint32_t startFrame, const uint32_t *frames, uint32_t count) {
  Track &tr = tracks[t];
  if (count == 0) return;

  uint16_t maxAbs = 0;
  if (isAudioTrackMono(t)) {
    const int16_t *mono = reinterpret_cast<const int16_t *>(frames);
    for (uint32_t i = 0; i < count; i += 4) {
      uint16_t a = (uint16_t)abs((int)mono[i]);
      if (a > maxAbs) maxAbs = a;
    }
  } else {
    for (uint32_t i = 0; i < count; i += 4) {
      uint32_t v = frames[i];
      int16_t l = (int16_t)(v & 0xFFFF);
      int16_t r = (int16_t)(v >> 16);
      uint16_t a = (uint16_t)max(abs((int)l), abs((int)r));
      if (a > maxAbs) maxAbs = a;
    }
  }
  uint8_t level = (uint8_t)min(255, (int)(sqrtf((float)maxAbs / 32768.0f) * 255.0f + 0.5f));

  // Update fixed-resolution peak bins across the 666s tape
  int b0 = constrain((int)(startFrame / FRAMES_PER_PEAK_BIN), 0, WAVE_PEAK_BINS - 1);
  int b1 = constrain((int)((startFrame + count - 1) / FRAMES_PER_PEAK_BIN), b0, WAVE_PEAK_BINS - 1);
  for (int b = b0; b <= b1; b++) {
    uint8_t curPeak = tr.getWavePeak(b);
    uint8_t nextPeak = level;
    if (recording.overdub || b <= recording.lastOverviewCol) {
      nextPeak = max(curPeak, level);
    } else {
      recording.lastOverviewCol = (int16_t)b;
    }
    tr.setWavePeak(b, nextPeak);
  }
  tr.overviewVersion++;
}

// ---- Song & System Settings serialization structs
struct DiskSystem {
  char magic[4]; // "M8SY"
  uint16_t version; // 4
  SystemSettings ss;
  char lastSongName[MAX_SONG_NAME_LEN + 1];
  MidiCcMapEntry globalMidiMap[MIDI_TARGET_COUNT];
};

struct DiskAudioTrack {
  int8_t volume;
  int8_t balance;
  uint8_t muted;
  uint8_t trigMode;
  int8_t threshDb;
  uint8_t armMode;
  uint8_t monoInput;
  uint8_t solo;
  uint32_t recordedFrames;
  uint8_t overview[OVERVIEW_COLS]; // Legacy v1 field retained for SONG.DAT v2 layout compatibility (superseded by Track::wavePeaks)
};

struct DiskSong {
  char magic[4]; // "M8PR"
  uint16_t version; // 2
  SongSettings ps;
  uint32_t tapeFrame;
  uint8_t selectedAudioTrack;
  uint8_t selectedMidiTrack;
  uint8_t reserved[2];
  DiskAudioTrack audio[NUM_AUDIO_TRACKS];
};

static_assert(sizeof(DiskSystem) <= 512, "DiskSystem must fit in one 512-byte sector");
static_assert(sizeof(DiskSong) <= sizeof(chunk), "DiskSong must fit in RAM1 chunk buffer");

FLASHMEM static void syncSdCard() {
  if (SD.sdfs.card()) {
    SD.sdfs.card()->syncDevice();
  }
}

FLASHMEM bool saveSystemSettings() {
  ensureSongsRootDir();
  if (!SD.sdfs.exists(INST_PRESETS_DIR)) {
    SD.sdfs.mkdir(INST_PRESETS_DIR);
  }
  char path[32];
  snprintf(path, sizeof(path), "%s/SYSTEM.DAT", ROOT_DIR);
  FsFile f = SD.sdfs.open(path, O_RDWR | O_CREAT | O_TRUNC);
  if (!f.isOpen()) return false;
  DiskSystem ds{};
  memcpy(ds.magic, "M8SY", 4);
  ds.version = 4;
  ds.ss = systemSettings();
  snprintf(ds.lastSongName, sizeof(ds.lastSongName), "%s", lastActiveSongName);
  memcpy(ds.globalMidiMap, midiMapGlobalTable(), sizeof(ds.globalMidiMap));
  memset(chunk, 0, 512);
  memcpy(chunk, &ds, sizeof(ds));
  bool ok = (f.write(chunk, sizeof(ds)) == (int)sizeof(ds));
  f.truncate();
  f.sync();
  f.close();
  syncSdCard();
  return ok;
}

FLASHMEM static bool loadSystemSettingsFromDisk() {
  char path[32];
  snprintf(path, sizeof(path), "%s/SYSTEM.DAT", ROOT_DIR);
  FsFile f = SD.sdfs.open(path, O_RDONLY);
  if (!f.isOpen()) return false;
  memset(chunk, 0, 512);
  int rd = f.read(chunk, 512);
  f.close();
  if (rd < 6) return false;

  char magic[4];
  uint16_t ver = 0;
  memcpy(magic, chunk, 4);
  memcpy(&ver, (const uint8_t *)chunk + 4, 2);
  if (memcmp(magic, "M8SY", 4) != 0) return false;

  SystemSettings &ss = systemSettings();
  constexpr int LEGACY_V4_SIZE = 4 + 2 + (int)sizeof(SystemSettings);
  if (ver == 4 && rd >= LEGACY_V4_SIZE) {
    DiskSystem ds{};
    memcpy(&ds, chunk, min((size_t)rd, sizeof(DiskSystem)));
    ds.ss.brightness = (uint8_t)constrain((int)ds.ss.brightness, (int)MIN_BRIGHTNESS, (int)MAX_BRIGHTNESS);
    ds.ss.metroVolume = (uint8_t)constrain((int)ds.ss.metroVolume, 0, (int)VOLUME_MAX);
    ss = ds.ss;
    ds.lastSongName[MAX_SONG_NAME_LEN] = '\0';
    sanitizeSongName(ds.lastSongName, lastActiveSongName, sizeof(lastActiveSongName));
    MidiCcMapEntry *gMap = midiMapGlobalTableMut();
    for (int i = 0; i < MIDI_TARGET_COUNT; i++) {
      uint8_t ch = ds.globalMidiMap[i].channel;
      if (ch > MIDI_MAP_CH_ALL) ch = MIDI_MAP_CH_OFF;
      gMap[i].channel = ch;
      gMap[i].cc = ds.globalMidiMap[i].cc & 0x7F;
    }
    midiMapRebuildMask();
    applySystemSettings();
    return true;
  }
  if (ver == 3 && rd >= 7) {
    uint8_t oldBright = *((const uint8_t *)chunk + 6);
    ss.brightness = (oldBright == 125)
                        ? DEFAULT_BRIGHTNESS
                        : (uint8_t)constrain(((int)oldBright * 100 + 127) / 255,
                                             (int)MIN_BRIGHTNESS, (int)MAX_BRIGHTNESS);
    ss.metroVolume = DEFAULT_METRO_VOLUME;
    applySystemSettings();
    return true;
  }
  return false;
}

FLASHMEM static void rebuildTrackOverviewFromFile(int t) {
  Track &tr = tracks[t];
  memset(tr.wavePeaks, 0, sizeof(tr.wavePeaks));
  if (!tr.file.isOpen() || tr.recordedFrames == 0) {
    tr.overviewVersion++;
    return;
  }
  const uint32_t bpf = audioTrackBytesPerFrame(t);
  const bool isMono = isAudioTrackMono(t);
  int maxBin = constrain((int)((tr.recordedFrames - 1) / FRAMES_PER_PEAK_BIN), 0, WAVE_PEAK_BINS - 1);
  int step = max(1, (maxBin + 1) / 480);
  for (int b = 0; b <= maxBin; b += step) {
    uint32_t fStart = (uint32_t)b * FRAMES_PER_PEAK_BIN;
    if (fStart >= tr.recordedFrames) break;
    uint32_t toRead = min(512u, tr.recordedFrames - fStart);
    uint64_t bytePos = trackWavByteOffset(t, fStart);
    if (tr.file.seekSet(bytePos)) {
      int rd = tr.file.read(chunk, toRead * bpf);
      uint32_t got = (rd > 0) ? ((uint32_t)rd / bpf) : 0;
      uint16_t maxAbs = 0;
      if (isMono) {
        const int16_t *mono = reinterpret_cast<const int16_t *>(chunk);
        for (uint32_t i = 0; i < got; i++) {
          uint16_t a = (uint16_t)abs((int)mono[i]);
          if (a > maxAbs) maxAbs = a;
        }
      } else {
        for (uint32_t i = 0; i < got; i++) {
          uint32_t v = chunk[i];
          int16_t l = (int16_t)(v & 0xFFFF);
          int16_t r = (int16_t)(v >> 16);
          uint16_t a = (uint16_t)max(abs((int)l), abs((int)r));
          if (a > maxAbs) maxAbs = a;
        }
      }
      uint8_t pk = (uint8_t)min(255, (int)(sqrtf((float)maxAbs / 32768.0f) * 255.0f + 0.5f));
      for (int k = 0; k < step && (b + k) <= maxBin; k++) {
        tr.setWavePeak(b + k, pk);
      }
    }
  }
  tr.overviewVersion++;
}

FLASHMEM static void removeSongSubfolder(const char *dPath, const char *subName) {
  char subDir[80];
  snprintf(subDir, sizeof(subDir), "%s/%s", dPath, subName);
  if (!SD.sdfs.exists(subDir)) return;
  FsFile sub = SD.sdfs.open(subDir, O_RDONLY);
  if (sub.isOpen()) {
    FsFile ent;
    while (ent.openNext(&sub, O_RDONLY)) {
      char fName[96] = {};
      ent.getName(fName, sizeof(fName));
      ent.close();
      if (fName[0] != '\0') {
        char fPath[176];
        snprintf(fPath, sizeof(fPath), "%s/%s", subDir, fName);
        SD.sdfs.remove(fPath);
      }
    }
    sub.close();
  }
  SD.sdfs.rmdir(subDir);
}

// Remove all song files inside /Capstan/<folderName> and attempt to rmdir
FLASHMEM static void removeSongFolderFiles(const char *folderName) {
  if (!folderName || !folderName[0]) return;
  char mPath[64], iPath[64], pPath[64], dPath[64], expPath[96];
  songDirByName(folderName, dPath, sizeof(dPath));
  for (int t = 0; t < NUM_AUDIO_TRACKS; t++) {
    char wPath[80];
    trackWavByName(folderName, t, wPath, sizeof(wPath));
    if (SD.sdfs.exists(wPath)) SD.sdfs.remove(wPath);
    snprintf(wPath, sizeof(wPath), "%s/TRACK%d.WAV", dPath, t + 1);
    if (SD.sdfs.exists(wPath)) SD.sdfs.remove(wPath);
  }
  midiDatByName(folderName, mPath, sizeof(mPath));
  if (SD.sdfs.exists(mPath)) SD.sdfs.remove(mPath);
  instDatByName(folderName, iPath, sizeof(iPath));
  if (SD.sdfs.exists(iPath)) SD.sdfs.remove(iPath);
  songDatByName(folderName, pPath, sizeof(pPath));
  if (SD.sdfs.exists(pPath)) SD.sdfs.remove(pPath);
  snprintf(expPath, sizeof(expPath), "%s/%s.wav", dPath, folderName);
  if (SD.sdfs.exists(expPath)) SD.sdfs.remove(expPath);
  removeSongSubfolder(dPath, "AUDIO");
  removeSongSubfolder(dPath, "INSTRUMENTS");
  removeSongSubfolder(dPath, "MIDI");
  removeSongSubfolder(dPath, "SAMPLES");
  if (SD.sdfs.exists(dPath)) SD.sdfs.rmdir(dPath);
}

// Check if /Capstan/<folderName> is a valid saved song on disk
FLASHMEM static bool inspectSongFolder(const char *folderName, bool cleanUnsavedOrGhost) {
  char pPath[64];
  songDatByName(folderName, pPath, sizeof(pPath));
  if (!SD.sdfs.exists(pPath)) {
    if (cleanUnsavedOrGhost) {
      // Unsaved temporary folder without SONG.DAT: remove any leftover scratch files
      removeSongFolderFiles(folderName);
    }
    return false;
  }

  FsFile pf = SD.sdfs.open(pPath, O_RDONLY);
  if (!pf.isOpen()) return false;

  bool hasValidDat = false;
  char magic[4] = {};
  uint16_t ver = 0;
  if (pf.read(magic, 4) == 4 && memcmp(magic, "M8PR", 4) == 0 &&
      pf.read(&ver, 2) == 2 && ver == 2) {
    hasValidDat = true;
  }
  pf.close();

  if (!hasValidDat && cleanUnsavedOrGhost) {
    removeSongFolderFiles(folderName);
  }
  return hasValidDat;
}

FLASHMEM void scanSongFolders() {
  numSavedSongs = 0;
  ensureSongsRootDir();

  FsFile root = SD.sdfs.open(SONGS_DIR, O_RDONLY);
  if (!root.isOpen()) return;

  FsFile entry;
  while (entry.openNext(&root, O_RDONLY)) {
    bool isSubDir = entry.isDir();
    char nameBuf[64] = {};
    entry.getName(nameBuf, sizeof(nameBuf));
    entry.close();

    if (!isSubDir) continue;
    if (nameBuf[0] == '\0' || nameBuf[0] == '.' || nameBuf[0] == '_') continue;
    if (strlen(nameBuf) > MAX_SONG_NAME_LEN) continue;

    char clean[MAX_SONG_NAME_LEN + 1];
    sanitizeSongName(nameBuf, clean, sizeof(clean));
    if (clean[0] == '\0' || strcasecmp(clean, nameBuf) != 0) continue;
    if (strcasecmp(clean, "SAMPLES") == 0 || strcasecmp(clean, "INSTRUMENTS") == 0) continue;

    // Do not delete the currently active unsaved session folder while scanning in-session
    bool isActiveUnsaved = (!songSaved && strcasecmp(clean, activeSongName) == 0);
    if (inspectSongFolder(clean, !isActiveUnsaved)) {
      if (numSavedSongs < MAX_SONGS) {
        snprintf(songList[numSavedSongs], MAX_SONG_NAME_LEN + 1, "%s", clean);
        numSavedSongs++;
      }
    }
  }
  root.close();

  // Sort songList alphabetically (case-insensitive insertion sort)
  for (int i = 1; i < numSavedSongs; i++) {
    char key[MAX_SONG_NAME_LEN + 1];
    memcpy(key, songList[i], sizeof(key));
    int j = i - 1;
    while (j >= 0 && strcasecmp(songList[j], key) > 0) {
      memcpy(songList[j + 1], songList[j], sizeof(key));
      j--;
    }
    memcpy(songList[j + 1], key, sizeof(key));
  }
}

// ============================================================================
// Project Files (/Capstan/PROJECTS/<NAME>.proj — Ordered List of Song Names)
// ============================================================================
DMAMEM static char projectList[MAX_PROJECTS][MAX_PROJECT_NAME_LEN + 1];
static int numSavedProjects = 0;

FLASHMEM static void projectFileByName(const char *name, char *out, size_t sz) {
  snprintf(out, sz, "%s/%s.proj", PROJECTS_DIR, name);
}

FLASHMEM int projectCount() {
  return numSavedProjects;
}

FLASHMEM const char *projectNameAt(int index) {
  if (index < 0 || index >= numSavedProjects) return "";
  return projectList[index];
}

FLASHMEM int findProjectIndex(const char *name) {
  if (!name || !name[0]) return -1;
  for (int i = 0; i < numSavedProjects; i++) {
    if (strcasecmp(projectList[i], name) == 0) return i;
  }
  return -1;
}

FLASHMEM static void nextDefaultProjectName(char *out, size_t sz) {
  for (int n = 1; n <= 999; n++) {
    char cand[MAX_PROJECT_NAME_LEN + 1];
    snprintf(cand, sizeof(cand), "PROJ_%03u", (unsigned)(n % 1000));
    if (findProjectIndex(cand) < 0) {
      char pPath[64];
      projectFileByName(cand, pPath, sizeof(pPath));
      if (!SD.sdfs.exists(pPath)) {
        snprintf(out, sz, "%s", cand);
        return;
      }
    }
  }
  snprintf(out, sz, "PROJ_001");
}

FLASHMEM void scanProjectFiles() {
  numSavedProjects = 0;
  ensureSongsRootDir();

  FsFile dir = SD.sdfs.open(PROJECTS_DIR, O_RDONLY);
  if (!dir.isOpen()) return;

  FsFile entry;
  while (entry.openNext(&dir, O_RDONLY)) {
    bool isSubDir = entry.isDir();
    char nameBuf[64] = {};
    entry.getName(nameBuf, sizeof(nameBuf));
    entry.close();

    if (isSubDir || nameBuf[0] == '\0' || nameBuf[0] == '.') continue;
    size_t len = strlen(nameBuf);
    if (len < 6 || strcasecmp(nameBuf + len - 5, ".proj") != 0) continue;

    size_t baseLen = len - 5;
    if (baseLen > MAX_PROJECT_NAME_LEN) continue;
    char base[MAX_PROJECT_NAME_LEN + 1] = {};
    memcpy(base, nameBuf, baseLen);
    base[baseLen] = '\0';

    char clean[MAX_PROJECT_NAME_LEN + 1] = {};
    sanitizeSongName(base, clean, sizeof(clean));
    if (clean[0] == '\0' || strcasecmp(clean, base) != 0) continue;

    if (numSavedProjects < MAX_PROJECTS) {
      snprintf(projectList[numSavedProjects], MAX_PROJECT_NAME_LEN + 1, "%s", clean);
      numSavedProjects++;
    }
  }
  dir.close();

  for (int i = 1; i < numSavedProjects; i++) {
    char key[MAX_PROJECT_NAME_LEN + 1];
    memcpy(key, projectList[i], sizeof(key));
    int j = i - 1;
    while (j >= 0 && strcasecmp(projectList[j], key) > 0) {
      memcpy(projectList[j + 1], projectList[j], sizeof(key));
      j--;
    }
    memcpy(projectList[j + 1], key, sizeof(key));
  }
}

FLASHMEM bool saveProjectSongs(const char *projName, const char songs[][MAX_SONG_NAME_LEN + 1], int count) {
  char clean[MAX_PROJECT_NAME_LEN + 1] = {};
  sanitizeSongName(projName, clean, sizeof(clean));
  if (clean[0] == '\0') return false;

  ensureSongsRootDir();
  char pPath[64];
  projectFileByName(clean, pPath, sizeof(pPath));

  // Stage text content through RAM1 chunk for DMA_SDIO safety
  char *buf = reinterpret_cast<char *>(chunk);
  const size_t cap = sizeof(chunk);
  size_t pos = 0;

  if (songs != nullptr && count > 0) {
    int limit = min(count, MAX_SONGS_PER_PROJECT);
    for (int i = 0; i < limit; i++) {
      char sClean[MAX_SONG_NAME_LEN + 1] = {};
      sanitizeSongName(songs[i], sClean, sizeof(sClean));
      if (sClean[0] == '\0') continue;
      size_t sLen = strlen(sClean);
      if (pos + sLen + 2 >= cap) break;
      memcpy(buf + pos, sClean, sLen);
      pos += sLen;
      buf[pos++] = '\n';
    }
  }

  FsFile f = SD.sdfs.open(pPath, O_RDWR | O_CREAT | O_TRUNC);
  if (!f.isOpen()) return false;
  bool ok = true;
  if (pos > 0) {
    ok = (f.write(buf, pos) == pos);
  }
  f.truncate();
  f.sync();
  f.close();
  syncSdCard();
  return ok;
}

FLASHMEM int loadProjectSongs(const char *projName, char outSongs[][MAX_SONG_NAME_LEN + 1], int maxSongs) {
  if (!outSongs || maxSongs <= 0) return 0;
  char clean[MAX_PROJECT_NAME_LEN + 1] = {};
  sanitizeSongName(projName, clean, sizeof(clean));
  if (clean[0] == '\0') return 0;

  char pPath[64];
  projectFileByName(clean, pPath, sizeof(pPath));
  FsFile f = SD.sdfs.open(pPath, O_RDONLY);
  if (!f.isOpen()) return 0;

  char *buf = reinterpret_cast<char *>(chunk);
  int rd = f.read(buf, sizeof(chunk) - 1);
  f.close();
  if (rd <= 0) return 0;
  buf[rd] = '\0';

  int count = 0;
  int i = 0;
  while (i < rd && count < maxSongs) {
    while (i < rd && (buf[i] == '\r' || buf[i] == '\n' || buf[i] == ' ' || buf[i] == '\t')) i++;
    if (i >= rd) break;
    int start = i;
    while (i < rd && buf[i] != '\r' && buf[i] != '\n') i++;
    int len = i - start;
    char line[32] = {};
    int copyLen = min(len, (int)sizeof(line) - 1);
    memcpy(line, buf + start, copyLen);
    line[copyLen] = '\0';

    char sClean[MAX_SONG_NAME_LEN + 1] = {};
    sanitizeSongName(line, sClean, sizeof(sClean));
    if (sClean[0] != '\0') {
      snprintf(outSongs[count], MAX_SONG_NAME_LEN + 1, "%s", sClean);
      count++;
    }
  }
  return count;
}

FLASHMEM bool createEmptyProjectFile(const char *name, char *outCleanName, size_t outSz) {
  ensureSongsRootDir();
  scanProjectFiles();

  char clean[MAX_PROJECT_NAME_LEN + 1] = {};
  sanitizeSongName(name, clean, sizeof(clean));
  if (clean[0] == '\0') {
    nextDefaultProjectName(clean, sizeof(clean));
  }
  if (!saveProjectSongs(clean, nullptr, 0)) return false;
  scanProjectFiles();
  if (outCleanName && outSz > 0) {
    snprintf(outCleanName, outSz, "%s", clean);
  }
  return true;
}

FLASHMEM bool renameProjectFile(const char *oldName, const char *newName, char *outCleanName, size_t outSz) {
  char oldClean[MAX_PROJECT_NAME_LEN + 1] = {};
  char newClean[MAX_PROJECT_NAME_LEN + 1] = {};
  sanitizeSongName(oldName, oldClean, sizeof(oldClean));
  sanitizeSongName(newName, newClean, sizeof(newClean));
  if (oldClean[0] == '\0' || newClean[0] == '\0') return false;

  if (strcasecmp(oldClean, newClean) == 0) {
    if (outCleanName && outSz > 0) snprintf(outCleanName, outSz, "%s", oldClean);
    return true;
  }

  char oldPath[64], newPath[64];
  projectFileByName(oldClean, oldPath, sizeof(oldPath));
  projectFileByName(newClean, newPath, sizeof(newPath));
  if (!SD.sdfs.exists(oldPath)) return false;
  if (SD.sdfs.exists(newPath)) {
    SD.sdfs.remove(newPath);
  }
  if (!SD.sdfs.rename(oldPath, newPath)) return false;
  syncSdCard();
  scanProjectFiles();
  if (outCleanName && outSz > 0) {
    snprintf(outCleanName, outSz, "%s", newClean);
  }
  return true;
}

FLASHMEM bool deleteProjectFile(const char *name) {
  char clean[MAX_PROJECT_NAME_LEN + 1] = {};
  sanitizeSongName(name, clean, sizeof(clean));
  if (clean[0] == '\0') return false;

  char pPath[64];
  projectFileByName(clean, pPath, sizeof(pPath));
  if (!SD.sdfs.exists(pPath)) return false;
  bool ok = SD.sdfs.remove(pPath);
  syncSdCard();
  scanProjectFiles();
  return ok;
}

FLASHMEM static void resetSongToDefaults() {
  SongSettings &ps = songSettings();
  ps.bpm = DEFAULT_BPM;
  ps.clockSource = ClockSource::Internal;
  ps.metroMode = MetroMode::RecOnly;
  ps.countInBeats = 4;
  ps.punchEnabled = false;
  ps.punchInFrame = 0;
  ps.punchOutFrame = beatsToFrames(4 * BEATS_PER_BAR, DEFAULT_BPM);
  ps.masterChorus = 0;
  ps.masterDelay = 0;
  ps.masterDelayFeedback = 0;
  ps.masterReverb = 0;
  ps.masterTape = 0;
  ps.masterComp = 0;
  ps.masterVolume = MASTER_VOLUME_DEFAULT;
  ps.audioInputSource = AudioInputSource::LineAndSynth;
  ps.inputMonitor = true;
  ps.usbInputMonitor = false;
  ps.defaultThreshDb = DEFAULT_THRESH_DB;
  ps.midiThru = false;
  ps.midiClockOut = MidiClockOut::Off;
  ps.loopEnabled = false;
  ps.recLineIn = true;
  ps.recUsbIn = false;
  ps.recInternalInstr = true;
  ps.lineMonitor = MonitorMode::On;
  ps.usbMonitor = MonitorMode::Off;
  ps.midiTransportIn = MidiTransportIn::Auto;
  applySongSettings();
  resetMasterFxState();

  bpm = ps.bpm;
  transport.spb = spbForBpm(ps.bpm);
  transport.speedQuarter = DEFAULT_SPEED_QUARTER;
  transport.subFrame = 0;
  transport.punchEnabled = ps.punchEnabled;
  transport.loopEnabled = ps.loopEnabled;
  transport.punchInFrame = ps.punchInFrame;
  transport.punchOutFrame = ps.punchOutFrame;

  for (int t = 0; t < NUM_AUDIO_TRACKS; t++) {
    Track &tr = tracks[t];
    tr.state = TrackState::Empty;
    tr.armMode = ArmMode::Off;
    tr.monoInput = MonoInput::Left;
    tr.solo = false;
    tr.recSrc = AudioRecSrc::Off;
    tr.trigMode = TrigMode::Manual;
    tr.threshDb = ps.defaultThreshDb;
    tr.recordedFrames = 0;
    memset(tr.wavePeaks, 0, sizeof(tr.wavePeaks));
    tr.overviewVersion++;
    setTrackMix(t, VOLUME_UNITY, 0);
  }
  midiSeqClearAll();
  fmSynthResetAll();
  trackEditReset();
  audioRow = 0;
  audioScroll = 0;
  audioCell = AUD_CELL_WAVE;
  midiRow = 0;
  midiScroll = 0;
  midiCell = MID_CELL_WAVE;
  masterRow = 0;
  masterCell = 0;
}

static void drainRecordingBuffer(bool flushAll);

// Copy up to `numBytes` from `srcPath` to `dstPath` using RAM1 `chunk[]`
FLASHMEM static bool copyFileChunked(const char *srcPath, const char *dstPath, uint64_t numBytes) {
  FsFile src = SD.sdfs.open(srcPath, O_RDONLY);
  if (!src.isOpen()) return false;
  FsFile dst = SD.sdfs.open(dstPath, O_RDWR | O_CREAT | O_TRUNC);
  if (!dst.isOpen()) {
    src.close();
    return false;
  }
  uint64_t rem = min(numBytes, src.fileSize());
  bool ok = true;
  while (rem > 0) {
    size_t toCopy = (size_t)min(rem, (uint64_t)sizeof(chunk));
    int rd = src.read(chunk, toCopy);
    if (rd <= 0) {
      ok = false;
      break;
    }
    if (dst.write(chunk, (size_t)rd) != (size_t)rd) {
      ok = false;
      break;
    }
    rem -= (uint64_t)rd;
  }
  dst.truncate();
  dst.sync();
  dst.close();
  src.close();
  return ok;
}

FLASHMEM static void copySongSubfolder(const char *srcFolder, const char *dstFolder, const char *subName) {
  char srcSub[80], dstSub[80];
  snprintf(srcSub, sizeof(srcSub), "%s/%s/%s", SONGS_DIR, srcFolder, subName);
  snprintf(dstSub, sizeof(dstSub), "%s/%s/%s", SONGS_DIR, dstFolder, subName);
  if (!SD.sdfs.exists(srcSub)) return;
  if (!SD.sdfs.exists(dstSub)) SD.sdfs.mkdir(dstSub);

  FsFile dir = SD.sdfs.open(srcSub, O_RDONLY);
  if (!dir.isOpen()) return;
  FsFile ent;
  while (ent.openNext(&dir, O_RDONLY)) {
    bool isSub = ent.isDir();
    uint64_t fSize = isSub ? 0 : ent.fileSize();
    char fName[96] = {};
    ent.getName(fName, sizeof(fName));
    ent.close();
    if (!isSub && fName[0] != '\0' && fSize > 0) {
      char sFile[176], dFile[176];
      snprintf(sFile, sizeof(sFile), "%s/%s", srcSub, fName);
      snprintf(dFile, sizeof(dFile), "%s/%s", dstSub, fName);
      copyFileChunked(sFile, dFile, fSize);
    }
  }
  dir.close();
}

FLASHMEM static void copySongInstrumentsSubfolder(const char *srcFolder, const char *dstFolder) {
  copySongSubfolder(srcFolder, dstFolder, "INSTRUMENTS");
  copySongSubfolder(srcFolder, dstFolder, "MIDI");
  copySongSubfolder(srcFolder, dstFolder, "SAMPLES");
}

// Read and deserialize SONG.DAT (version 2) from `/Capstan/<folderName>` directly into `chunk`
FLASHMEM static const DiskSong *readDiskSongFromFolder(const char *folderName) {
  char pPath[64];
  songDatByName(folderName, pPath, sizeof(pPath));
  FsFile pf = SD.sdfs.open(pPath, O_RDONLY);
  if (!pf.isOpen()) return nullptr;
  DiskSong &outDp = *reinterpret_cast<DiskSong *>(chunk);
  uint8_t *raw = reinterpret_cast<uint8_t *>(chunk) + sizeof(DiskSong);
  outDp = DiskSong{};
  for (int t = 0; t < NUM_AUDIO_TRACKS; t++) {
    outDp.audio[t].volume = VOLUME_UNITY;
    outDp.audio[t].threshDb = DEFAULT_THRESH_DB;
  }
  int rd = pf.read(raw, sizeof(DiskSong));
  pf.close();

  constexpr int LEGACY_4TRK_SIZE = (int)(sizeof(DiskSong) - 4 * sizeof(DiskAudioTrack));
  constexpr int OLD_PS_DELTA = 4;
  if (rd == (int)sizeof(DiskSong) || rd == LEGACY_4TRK_SIZE) {
    memcpy(static_cast<void *>(&outDp), raw, (size_t)rd);
    return (memcmp(outDp.magic, "M8PR", 4) == 0 && outDp.version == 2) ? &outDp : nullptr;
  }
  if (rd == (int)sizeof(DiskSong) - OLD_PS_DELTA || rd == LEGACY_4TRK_SIZE - OLD_PS_DELTA) {
    memcpy(static_cast<void *>(&outDp), raw, 8 + 30);
    AudioInputSource src = outDp.ps.audioInputSource;
    outDp.ps.recLineIn = (src == AudioInputSource::LineAndSynth || src == AudioInputSource::All ||
                          src == AudioInputSource::LineOnly || (uint8_t)src >= (uint8_t)SELECTABLE_AUDIO_INPUT_SOURCES);
    outDp.ps.recUsbIn = (src == AudioInputSource::UsbAndSynth || src == AudioInputSource::All ||
                         src == AudioInputSource::UsbOnly);
    outDp.ps.recInternalInstr = (src == AudioInputSource::LineAndSynth || src == AudioInputSource::UsbAndSynth ||
                                 src == AudioInputSource::All || src == AudioInputSource::SynthOnly ||
                                 (uint8_t)src >= (uint8_t)SELECTABLE_AUDIO_INPUT_SOURCES);
    outDp.ps.lineMonitor = outDp.ps.inputMonitor ? MonitorMode::On : MonitorMode::Off;
    outDp.ps.usbMonitor = outDp.ps.usbInputMonitor ? MonitorMode::On : MonitorMode::Off;
    size_t tailBytes = (size_t)rd - (8 + 32);
    memcpy(static_cast<void *>(&outDp.tapeFrame), raw + 8 + 32, tailBytes);
    return (memcmp(outDp.magic, "M8PR", 4) == 0 && outDp.version == 2) ? &outDp : nullptr;
  }
  return nullptr;
}

// Restore a saved song's WAV files to match its saved SONG.DAT lengths (used after Save-As)
FLASHMEM static void restoreSavedFolderWavLengths(const char *folderName) {
  const DiskSong *dp = readDiskSongFromFolder(folderName);
  if (!dp) return;
  const DiskAudioTrack *savedAudio = dp->audio;

  for (int t = 0; t < NUM_AUDIO_TRACKS; t++) {
    char wPath[64];
    trackWavByName(folderName, t, wPath, sizeof(wPath));
    uint32_t savedFrames = min(savedAudio[t].recordedFrames, MAX_REC_FRAMES);
    if (savedFrames == 0) {
      if (SD.sdfs.exists(wPath)) SD.sdfs.remove(wPath);
    } else if (SD.sdfs.exists(wPath)) {
      FsFile wf = SD.sdfs.open(wPath, O_RDWR);
      if (wf.isOpen()) {
        uint64_t validBytes = trackWavByteOffset(t, savedFrames);
        if (wf.fileSize() > validBytes && wf.seekSet(validBytes)) {
          wf.truncate();
        }
        writeWavHeader(wf, savedFrames, t);
        wf.sync();
        wf.close();
      }
    }
  }
}

FLASHMEM bool saveSongAs(const char *newName) {
  if (transport.mode != TransportMode::Stopped) {
    storageStopTransport();
  } else if (recording.activeInWindow || recording.windowExited) {
    drainRecordingBuffer(true);
  }

  char targetName[MAX_SONG_NAME_LEN + 1];
  sanitizeSongName(newName, targetName, sizeof(targetName));
  if (targetName[0] == '\0') {
    sanitizeSongName(activeSongName, targetName, sizeof(targetName));
  }
  if (targetName[0] == '\0') {
    nextDefaultSongName(targetName, sizeof(targetName));
  }

  bool sameFolder = (strcasecmp(targetName, activeSongName) == 0);
  char oldDir[64], newDir[64], pPath[64], mPath[64];
  songDirByName(activeSongName, oldDir, sizeof(oldDir));
  songDirByName(targetName, newDir, sizeof(newDir));

  if (!sameFolder) {
    // Close open WAV handles before renaming or copying
    for (int t = 0; t < NUM_AUDIO_TRACKS; t++) {
      if (tracks[t].file.isOpen()) tracks[t].file.close();
    }

    // If the target folder already exists on disk, clear its old files first
    if (SD.sdfs.exists(newDir)) {
      removeSongFolderFiles(targetName);
    }

    if (!songSaved) {
      // Current song was an unsaved session: rename its temporary folder directly if it exists
      if (SD.sdfs.exists(oldDir)) {
        if (!SD.sdfs.rename(oldDir, newDir)) {
          ensureSongAudioDir(targetName);
          for (int t = 0; t < NUM_AUDIO_TRACKS; t++) {
            if (tracks[t].recordedFrames > 0) {
              char srcW[64], dstW[64];
              trackWavByName(activeSongName, t, srcW, sizeof(srcW));
              trackWavByName(targetName, t, dstW, sizeof(dstW));
              uint64_t bytes = trackWavByteOffset(t, tracks[t].recordedFrames);
              copyFileChunked(srcW, dstW, bytes);
            }
          }
          copySongInstrumentsSubfolder(activeSongName, targetName);
          removeSongFolderFiles(activeSongName);
        }
      }
    } else {
      // Current song was already saved: preserve old folder and copy active WAVs to new folder
      ensureSongAudioDir(targetName);
      for (int t = 0; t < NUM_AUDIO_TRACKS; t++) {
        if (tracks[t].recordedFrames > 0) {
          char srcW[64], dstW[64];
          trackWavByName(activeSongName, t, srcW, sizeof(srcW));
          trackWavByName(targetName, t, dstW, sizeof(dstW));
          uint64_t bytes = trackWavByteOffset(t, tracks[t].recordedFrames);
          copyFileChunked(srcW, dstW, bytes);
        }
      }
      copySongInstrumentsSubfolder(activeSongName, targetName);
      // Restore the original saved song's WAV lengths in case new audio was recorded before Save-As
      restoreSavedFolderWavLengths(activeSongName);
    }

    samplerRemapSongFolder(activeSongName, targetName);
    snprintf(activeSongName, sizeof(activeSongName), "%s", targetName);
  }

  ensureSongAudioDir(activeSongName);
  char songInstDir[80];
  snprintf(songInstDir, sizeof(songInstDir), "%s/INSTRUMENTS", newDir);
  if (!SD.sdfs.exists(songInstDir)) {
    SD.sdfs.mkdir(songInstDir);
  }

  // Sync and finalize all 4 audio track WAV files in the target folder
  for (int t = 0; t < NUM_AUDIO_TRACKS; t++) {
    Track &tr = tracks[t];
    char wPath[64];
    trackWavByName(activeSongName, t, wPath, sizeof(wPath));
    if (tr.recordedFrames > 0) {
      if (!tr.file.isOpen()) {
        tr.file = SD.sdfs.open(wPath, O_RDWR);
      }
      if (tr.file.isOpen()) {
        uint64_t validBytes = trackWavByteOffset(t, tr.recordedFrames);
        if (tr.file.fileSize() > validBytes && tr.file.seekSet(validBytes)) {
          tr.file.truncate();
        }
        writeWavHeader(tr.file, tr.recordedFrames, t);
        tr.file.sync();
      }
    } else {
      if (tr.file.isOpen()) {
        tr.file.close();
      }
      if (SD.sdfs.exists(wPath)) {
        SD.sdfs.remove(wPath);
      }
    }
  }

  // Write SONG.DAT (version 2) staged directly inside RAM1 chunk
  songDatByName(activeSongName, pPath, sizeof(pPath));
  FsFile pf = SD.sdfs.open(pPath, O_RDWR | O_CREAT | O_TRUNC);
  if (!pf.isOpen()) return false;

  DiskSong &dp = *reinterpret_cast<DiskSong *>(chunk);
  dp = DiskSong{};
  memcpy(dp.magic, "M8PR", 4);
  dp.version = 2;
  dp.ps = songSettings();
  dp.tapeFrame = transport.tapeFrame;
  dp.selectedAudioTrack = (uint8_t)constrain(audioRow, 0, PAGE_ROWS - 1);
  dp.selectedMidiTrack = (uint8_t)constrain(midiRow, 0, PAGE_ROWS - 1);
  for (int t = 0; t < NUM_AUDIO_TRACKS; t++) {
    const Track &tr = tracks[t];
    dp.audio[t].volume = tr.volume;
    dp.audio[t].balance = tr.balance;
    dp.audio[t].muted = (tr.state == TrackState::Muted) ? 1 : 0;
    dp.audio[t].trigMode = (uint8_t)tr.trigMode;
    dp.audio[t].threshDb = tr.threshDb;
    dp.audio[t].armMode = (uint8_t)tr.armMode;
    dp.audio[t].monoInput = (uint8_t)tr.monoInput;
    dp.audio[t].solo = tr.solo ? 1 : 0;
    dp.audio[t].recordedFrames = tr.recordedFrames;
    dp.audio[t].overview[0] = (uint8_t)tr.recSrc;
    dp.audio[t].overview[1] = 0xCA;
  }
  bool ok = (pf.write(chunk, sizeof(DiskSong)) == (int)sizeof(DiskSong));
  pf.truncate();
  pf.sync();
  pf.close();

  // Write MIDI.DAT (version 2)
  midiDatByName(activeSongName, mPath, sizeof(mPath));
  FsFile mf = SD.sdfs.open(mPath, O_RDWR | O_CREAT | O_TRUNC);
  if (mf.isOpen()) {
    if (!midiSeqSave(mf)) ok = false;
    mf.close();
  } else {
    ok = false;
  }

  // Write INST.DAT (song's 8 2-Op FM Synth instances)
  char iPath[64];
  instDatByName(activeSongName, iPath, sizeof(iPath));
  FsFile instFile = SD.sdfs.open(iPath, O_RDWR | O_CREAT | O_TRUNC);
  if (instFile.isOpen()) {
    if (!fmSynthSaveSong(instFile)) ok = false;
    instFile.close();
  } else {
    ok = false;
  }

  songSaved = true;
  songDirty = false;
  scanSongFolders();

  snprintf(lastActiveSongName, sizeof(lastActiveSongName), "%s", activeSongName);
  saveSystemSettings();

  applySongSettings();
  syncSdCard();
  return ok;
}

FLASHMEM static void closeAllAudioTrackFiles(bool syncFirst = false) {
  for (int t = 0; t < NUM_AUDIO_TRACKS; t++) {
    if (tracks[t].file.isOpen()) {
      if (syncFirst) tracks[t].file.sync();
      tracks[t].file.close();
    }
  }
}

FLASHMEM static void discardUnsavedSessionChanges(const char *incomingFolder = nullptr) {
  closeAllAudioTrackFiles(false);
  if (!songSaved && activeSongName[0] != '\0' &&
      (!incomingFolder || strcasecmp(activeSongName, incomingFolder) != 0)) {
    removeSongFolderFiles(activeSongName);
  } else if (songSaved && songDirty && activeSongName[0] != '\0') {
    restoreSavedFolderWavLengths(activeSongName);
  }
}

FLASHMEM bool saveCurrentSong() {
  return saveSongAs(activeSongName);
}

FLASHMEM bool loadSongByName(const char *name) {
  char clean[MAX_SONG_NAME_LEN + 1];
  sanitizeSongName(name, clean, sizeof(clean));
  if (clean[0] == '\0') return false;

  char pPath[64], mPath[64];
  songDatByName(clean, pPath, sizeof(pPath));
  if (!SD.sdfs.exists(pPath)) return false;

  // Stop transport WITHOUT auto-saving
  storageStopTransport();
  discardUnsavedSessionChanges(clean);

  storageForget();
  snprintf(activeSongName, sizeof(activeSongName), "%s", clean);
  songSaved = true;
  songDirty = false;

  resetSongToDefaults();
  SongSettings &ps = songSettings();
  uint32_t restoredTapeFrame = 0;
  bool mutedFlags[NUM_AUDIO_TRACKS] = {};
  bool loadedV2 = false;

  if (const DiskSong *dpPtr = readDiskSongFromFolder(clean)) {
    const DiskSong &dp = *dpPtr;
    loadedV2 = true;
    ps = dp.ps;
    restoredTapeFrame = min(dp.tapeFrame, MAX_REC_FRAMES);
    audioRow = constrain((int)dp.selectedAudioTrack, 0, PAGE_ROWS - 1);
    midiRow = constrain((int)dp.selectedMidiTrack, 0, PAGE_ROWS - 1);
    syncAudioScroll();
    syncMidiScroll();
    for (int t = 0; t < NUM_AUDIO_TRACKS; t++) {
      Track &tr = tracks[t];
      uint8_t tm = dp.audio[t].trigMode;
      tr.trigMode = (tm == (uint8_t)TrigMode::Threshold) ? TrigMode::Threshold : TrigMode::Manual;
      uint8_t rs = 0;
      if (dp.audio[t].overview[1] == 0xCA) {
        rs = dp.audio[t].overview[0];
      } else if (tm == 2) {
        rs = (uint8_t)AudioRecSrc::ResamplePost;
      }
      if (rs >= (uint8_t)AudioRecSrc::Count || rs == (uint8_t)AudioRecSrc::Audio1 + t) {
        rs = (uint8_t)AudioRecSrc::Off;
      }
      tr.recSrc = (AudioRecSrc)rs;
      tr.threshDb = (int8_t)constrain((int)dp.audio[t].threshDb, MIN_THRESH_DB, MAX_THRESH_DB);
      uint8_t arm = dp.audio[t].armMode;
      tr.armMode = (arm == (uint8_t)ArmMode::Record)  ? ArmMode::Record :
                   (arm == (uint8_t)ArmMode::Overdub) ? ArmMode::Overdub : ArmMode::Off;
      uint8_t mi = dp.audio[t].monoInput;
      tr.monoInput = (mi == (uint8_t)MonoInput::Right) ? MonoInput::Right :
                     (mi == (uint8_t)MonoInput::SumLR) ? MonoInput::SumLR : MonoInput::Left;
      tr.solo = (dp.audio[t].solo != 0);
      tr.recordedFrames = min(dp.audio[t].recordedFrames, MAX_REC_FRAMES);
      tr.overviewVersion++;
      setTrackMix(t, dp.audio[t].volume, dp.audio[t].balance);
      mutedFlags[t] = (dp.audio[t].muted != 0);
    }
  }

  ps.bpm = constrain(ps.bpm, MIN_BPM, MAX_BPM);
  if ((uint8_t)ps.clockSource > (uint8_t)ClockSource::ExternalTrs) {
    ps.clockSource = ClockSource::Internal;
  }
  ps.countInBeats = (uint8_t)constrain((int)ps.countInBeats, 0, 16);
  if (ps.punchOutFrame <= ps.punchInFrame) {
    ps.punchOutFrame = min(MAX_REC_FRAMES, ps.punchInFrame + SAMPLE_RATE_HZ * 2);
  }
  ps.masterChorus = (uint8_t)constrain((int)ps.masterChorus, 0, 100);
  ps.masterDelay = (uint8_t)constrain((int)ps.masterDelay, 0, 100);
  ps.masterDelayFeedback = (uint8_t)constrain((int)ps.masterDelayFeedback, 0, 100);
  ps.masterReverb = (uint8_t)constrain((int)ps.masterReverb, 0, 100);
  ps.masterTape = (uint8_t)constrain((int)ps.masterTape, 0, 100);
  ps.masterComp = (uint8_t)constrain((int)ps.masterComp, 0, 100);
  ps.masterVolume = (uint8_t)constrain((int)ps.masterVolume, 0, MASTER_VOLUME_MAX);
  if ((uint8_t)ps.audioInputSource >= (uint8_t)SELECTABLE_AUDIO_INPUT_SOURCES) {
    ps.audioInputSource = AudioInputSource::LineAndSynth;
  }
  ps.defaultThreshDb = (int8_t)constrain((int)ps.defaultThreshDb, MIN_THRESH_DB, MAX_THRESH_DB);
  if ((uint8_t)ps.midiClockOut > (uint8_t)MidiClockOut::Both) {
    ps.midiClockOut = MidiClockOut::Off;
  }
  if ((uint8_t)ps.lineMonitor > (uint8_t)MonitorMode::Arm) {
    ps.lineMonitor = ps.inputMonitor ? MonitorMode::On : MonitorMode::Off;
  }
  if ((uint8_t)ps.usbMonitor > (uint8_t)MonitorMode::Arm) {
    ps.usbMonitor = ps.usbInputMonitor ? MonitorMode::On : MonitorMode::Off;
  }
  if ((uint8_t)ps.midiTransportIn > (uint8_t)MidiTransportIn::Off) {
    ps.midiTransportIn = MidiTransportIn::Auto;
  }
  applySongSettings();
  resetMasterFxState();

  bpm = ps.bpm;
  transport.spb = spbForBpm(ps.bpm);
  transport.speedQuarter = DEFAULT_SPEED_QUARTER;
  transport.subFrame = 0;
  transport.punchEnabled = ps.punchEnabled;
  transport.loopEnabled = ps.loopEnabled;
  transport.punchInFrame = ps.punchInFrame;
  transport.punchOutFrame = ps.punchOutFrame;

  // Open audio track WAV files matching the saved song state
  for (int t = 0; t < NUM_AUDIO_TRACKS; t++) {
    Track &tr = tracks[t];
    char wPath[64];
    trackWavByName(clean, t, wPath, sizeof(wPath));
    if (!SD.sdfs.exists(wPath)) {
      char legPath[64];
      snprintf(legPath, sizeof(legPath), "%s/%s/TRACK%d.WAV", SONGS_DIR, clean, t + 1);
      if (SD.sdfs.exists(legPath)) {
        ensureSongAudioDir(clean);
        SD.sdfs.rename(legPath, wPath);
      }
    }
    if (SD.sdfs.exists(wPath)) {
      if (loadedV2 && tr.recordedFrames == 0) {
        // Track was empty when song was last saved; remove any unsaved WAV file
        SD.sdfs.remove(wPath);
        tr.state = mutedFlags[t] ? TrackState::Muted : TrackState::Empty;
        continue;
      }
      tr.file = SD.sdfs.open(wPath, O_RDWR);
      if (tr.file.isOpen()) {
        const uint32_t bpf = audioTrackBytesPerFrame(t);
        uint64_t fsz = tr.file.fileSize();
        uint32_t fileFrames = (fsz > WAV_DATA_OFFSET)
            ? (uint32_t)min((uint64_t)MAX_REC_FRAMES, (fsz - WAV_DATA_OFFSET) / bpf)
            : 0;
        if (!loadedV2 && tr.recordedFrames == 0 && fileFrames > 0) {
          tr.recordedFrames = fileFrames;
        } else if (tr.recordedFrames > fileFrames) {
          tr.recordedFrames = fileFrames;
        } else if (loadedV2 && fileFrames > tr.recordedFrames && tr.recordedFrames > 0) {
          // Truncate any unsaved audio recorded past the last manual save point
          uint64_t validBytes = trackWavByteOffset(t, tr.recordedFrames);
          if (tr.file.seekSet(validBytes)) {
            tr.file.truncate();
          }
          writeWavHeader(tr.file, tr.recordedFrames, t);
          tr.file.sync();
        }
        if (tr.recordedFrames > 0) {
          tr.state = mutedFlags[t] ? TrackState::Muted : TrackState::Playing;
          rebuildTrackOverviewFromFile(t);
        } else {
          tr.file.close();
          tr.state = mutedFlags[t] ? TrackState::Muted : TrackState::Empty;
        }
      } else {
        tr.recordedFrames = 0;
        tr.state = mutedFlags[t] ? TrackState::Muted : TrackState::Empty;
      }
    } else {
      tr.recordedFrames = 0;
      tr.state = mutedFlags[t] ? TrackState::Muted : TrackState::Empty;
    }
  }

  midiDatByName(clean, mPath, sizeof(mPath));
  FsFile mf = SD.sdfs.open(mPath, O_RDONLY);
  if (mf.isOpen()) {
    midiSeqLoad(mf, ps.bpm, songMaxFrames());
    mf.close();
  }
  midiSeqRebuildAllOverviews(ps.bpm, songMaxFrames());

  char iPath[64];
  instDatByName(clean, iPath, sizeof(iPath));
  FsFile instFile = SD.sdfs.open(iPath, O_RDONLY);
  if (instFile.isOpen()) {
    fmSynthLoadSong(instFile);
    instFile.close();
  }

  storageSeekTape(restoredTapeFrame);
  songDirty = false;
  if (strcasecmp(lastActiveSongName, clean) != 0) {
    snprintf(lastActiveSongName, sizeof(lastActiveSongName), "%s", clean);
    saveSystemSettings();
  }
  return true;
}

FLASHMEM bool loadSong(int slot) {
  // Stop transport WITHOUT saving onto the incoming song
  storageStopTransport();

  if (slot < 0 || slot >= numSavedSongs) return false;
  char target[MAX_SONG_NAME_LEN + 1];
  snprintf(target, sizeof(target), "%s", songList[slot]);
  return loadSongByName(target);
}

FLASHMEM bool createNewSong() {
  storageStopTransport();
  discardUnsavedSessionChanges(nullptr);

  storageForget();
  scanSongFolders();

  char nextName[MAX_SONG_NAME_LEN + 1];
  nextDefaultSongName(nextName, sizeof(nextName));
  snprintf(activeSongName, sizeof(activeSongName), "%s", nextName);

  songSaved = false;
  songDirty = false;

  resetSongToDefaults();
  storageSeekTape(0);
  applySystemSettings();
  return true;
}

FLASHMEM bool deleteSongByName(const char *name) {
  if (!name || !name[0]) return false;
  char clean[MAX_SONG_NAME_LEN + 1];
  sanitizeSongName(name, clean, sizeof(clean));
  if (clean[0] == '\0') return false;

  bool isCurrent = (strcasecmp(clean, activeSongName) == 0);
  if (isCurrent) {
    storageStopTransport();
    for (int t = 0; t < NUM_AUDIO_TRACKS; t++) {
      Track &tr = tracks[t];
      noInterrupts();
      tr.live = false;
      tr.state = TrackState::Empty;
      tr.solo = false;
      tr.recordedFrames = 0;
      tr.head = tr.tail = 0;
      interrupts();
      if (tr.file.isOpen()) tr.file.close();
    }
  }

  removeSongFolderFiles(clean);
  syncSdCard();
  scanSongFolders();
  if (strcasecmp(clean, lastActiveSongName) == 0) {
    lastActiveSongName[0] = '\0';
    saveSystemSettings();
  }

  if (isCurrent) {
    songSaved = false;
    songDirty = false;
    resetSongToDefaults();
    storageSeekTape(0);
    char nextName[MAX_SONG_NAME_LEN + 1];
    nextDefaultSongName(nextName, sizeof(nextName));
    snprintf(activeSongName, sizeof(activeSongName), "%s", nextName);
    applySystemSettings();
  }
  return true;
}

FLASHMEM bool deleteSongAt(int index) {
  if (index < 0 || index >= numSavedSongs) return false;
  char target[MAX_SONG_NAME_LEN + 1];
  snprintf(target, sizeof(target), "%s", songList[index]);
  return deleteSongByName(target);
}

FLASHMEM void clearAudioTrack(int t) {
  if (t < 0 || t >= NUM_AUDIO_TRACKS) return;
  if (transport.mode != TransportMode::Stopped) {
    storageStopTransport();
  }
  Track &tr = tracks[t];
  noInterrupts();
  tr.live = false;
  tr.state = TrackState::Empty;
  tr.solo = false;
  tr.recordedFrames = 0;
  tr.head = tr.tail = 0;
  tr.peakL = 0.0f;
  tr.peakR = 0.0f;
  memset(tr.curMix, 0, sizeof(tr.curMix));
  interrupts();
  tr.fileFrame = transport.tapeFrame;

  if (tr.file.isOpen()) {
    if (tr.file.seekSet(0)) {
      tr.file.truncate();
      tr.file.sync();
    }
    tr.file.close();
  }
  if (activeSongName[0] != '\0') {
    char wPath[64];
    trackWavByName(activeSongName, t, wPath, sizeof(wPath));
    if (SD.sdfs.exists(wPath) && !SD.sdfs.remove(wPath)) {
      FsFile wf = SD.sdfs.open(wPath, O_RDWR | O_TRUNC);
      if (wf.isOpen()) {
        wf.truncate();
        wf.sync();
        wf.close();
      }
    }
    char legPath[64];
    snprintf(legPath, sizeof(legPath), "%s/%s/TRACK%d.WAV", SONGS_DIR, activeSongName, t + 1);
    if (SD.sdfs.exists(legPath)) {
      SD.sdfs.remove(legPath);
    }
    syncSdCard();
  }
  memset(tr.wavePeaks, 0, sizeof(tr.wavePeaks));
  tr.overviewVersion++;
  songDirty = true;
}

static constexpr const char *AUDIO_CLIP_PATH = "/Capstan/_CLIP.TMP";
static constexpr const char *MIDI_CLIP_PATH  = "/Capstan/_CLIPMID.TMP";

static void ensureTrackWavOpen(int recTrk);
static bool padTrackFileToBytePos(FsFile &f, uint64_t targetBytePos, bool keepStreamsAlive, int trackIdx);

FLASHMEM void storageRemoveClipboardFiles() {
  if (SD.sdfs.exists(AUDIO_CLIP_PATH)) {
    SD.sdfs.remove(AUDIO_CLIP_PATH);
  }
  if (SD.sdfs.exists(MIDI_CLIP_PATH)) {
    SD.sdfs.remove(MIDI_CLIP_PATH);
  }
}

FLASHMEM bool storageCopyAudioRegionToClip(int srcTrack, uint32_t startFrame, uint32_t endFrame) {
  if (srcTrack < 0 || srcTrack >= NUM_AUDIO_TRACKS || startFrame >= endFrame) return false;
  if (transport.mode != TransportMode::Stopped) {
    storageStopTransport();
  }
  endFrame = min(endFrame, (uint32_t)MAX_REC_FRAMES);
  if (startFrame >= endFrame) return false;

  FsFile clipFile = SD.sdfs.open(AUDIO_CLIP_PATH, O_RDWR | O_CREAT | O_TRUNC);
  if (!clipFile.isOpen()) return false;

  Track &tr = tracks[srcTrack];
  const uint32_t bpf = audioTrackBytesPerFrame(srcTrack);
  const bool isMono = isAudioTrackMono(srcTrack);
  uint32_t curFrame = startFrame;
  bool ok = true;

  while (curFrame < endFrame) {
    uint32_t n = min((uint32_t)CHUNK_FRAMES, endFrame - curFrame);
    if (tr.file.isOpen() && curFrame < tr.recordedFrames) {
      uint32_t availFile = min(n, tr.recordedFrames - curFrame);
      uint64_t bytePos = trackWavByteOffset(srcTrack, curFrame);
      uint32_t gotFrames = 0;
      if (tr.file.seekSet(bytePos)) {
        int rd = tr.file.read(chunk, availFile * bpf);
        gotFrames = (rd > 0) ? ((uint32_t)rd / bpf) : 0;
      }
      if (gotFrames < n) {
        memset((uint8_t *)chunk + gotFrames * bpf, 0, (n - gotFrames) * bpf);
      }
      if (isMono) {
        int16_t *mono = reinterpret_cast<int16_t *>(chunk);
        for (int i = (int)n - 1; i >= 0; i--) {
          uint16_t s = (uint16_t)mono[i];
          chunk[i] = (uint32_t)s | ((uint32_t)s << 16);
        }
      }
    } else {
      memset(chunk, 0, n * sizeof(uint32_t));
    }

    size_t writeBytes = (size_t)n * sizeof(uint32_t);
    if (clipFile.write(chunk, writeBytes) != writeBytes) {
      ok = false;
      break;
    }
    curFrame += n;
  }

  clipFile.truncate();
  clipFile.sync();
  clipFile.close();
  return ok;
}

FLASHMEM bool storagePasteAudioClipToTrack(int dstTrack, uint32_t destStartFrame, uint32_t frameCount) {
  if (dstTrack < 0 || dstTrack >= NUM_AUDIO_TRACKS || frameCount == 0 || destStartFrame >= MAX_REC_FRAMES) {
    return false;
  }
  if (transport.mode != TransportMode::Stopped) {
    storageStopTransport();
  }

  FsFile clipFile = SD.sdfs.open(AUDIO_CLIP_PATH, O_RDONLY);
  if (!clipFile.isOpen()) return false;

  ensureTrackWavOpen(dstTrack);
  Track &tr = tracks[dstTrack];
  if (!tr.file.isOpen()) {
    clipFile.close();
    return false;
  }

  uint32_t framesToPaste = min(frameCount, (uint32_t)MAX_REC_FRAMES - destStartFrame);
  uint64_t startBytePos = trackWavByteOffset(dstTrack, destStartFrame);
  if (tr.file.fileSize() < startBytePos) {
    if (!padTrackFileToBytePos(tr.file, startBytePos, false, dstTrack)) {
      clipFile.close();
      return false;
    }
  }

  const uint32_t bpf = audioTrackBytesPerFrame(dstTrack);
  const bool isMono = isAudioTrackMono(dstTrack);
  uint32_t rem = framesToPaste;
  uint32_t curFrame = destStartFrame;
  bool ok = true;

  while (rem > 0) {
    uint32_t n = min((uint32_t)CHUNK_FRAMES, rem);
    size_t clipBytes = (size_t)n * sizeof(uint32_t);
    int rd = clipFile.read(chunk, clipBytes);
    uint32_t got = (rd > 0) ? ((uint32_t)rd / sizeof(uint32_t)) : 0;
    if (got < n) {
      memset(chunk + got, 0, (n - got) * sizeof(uint32_t));
    }
    if (isMono) {
      int16_t *mono = reinterpret_cast<int16_t *>(chunk);
      for (uint32_t i = 0; i < n; i++) {
        uint32_t v = chunk[i];
        int16_t l = (int16_t)(v & 0xFFFF);
        int16_t r = (int16_t)(v >> 16);
        mono[i] = (int16_t)(((int32_t)l + (int32_t)r) >> 1);
      }
    }

    uint64_t bytePos = trackWavByteOffset(dstTrack, curFrame);
    size_t writeBytes = (size_t)n * bpf;
    if (!tr.file.seekSet(bytePos) || tr.file.write(chunk, writeBytes) != writeBytes) {
      ok = false;
      break;
    }
    curFrame += n;
    rem -= n;
  }

  clipFile.close();

  if (curFrame > tr.recordedFrames) {
    tr.recordedFrames = curFrame;
  }
  if (tr.recordedFrames > 0 && tr.state == TrackState::Empty) {
    tr.state = TrackState::Playing;
  }
  uint64_t validBytes = trackWavByteOffset(dstTrack, tr.recordedFrames);
  if (tr.file.fileSize() > validBytes && tr.file.seekSet(validBytes)) {
    tr.file.truncate();
  }
  writeWavHeader(tr.file, tr.recordedFrames, dstTrack);
  tr.file.sync();
  rebuildTrackOverviewFromFile(dstTrack);
  prefillTrackStreams(transport.tapeFrame);
  songDirty = true;
  return ok;
}

static inline int16_t decodeClipWavSample(const uint8_t *p, uint16_t bits, uint16_t audioFmt) {
  if (bits == 16) return (int16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
  if (bits == 24) return (int16_t)((uint16_t)p[1] | ((uint16_t)p[2] << 8));
  if (bits == 8)  return (int16_t)(((int)p[0] - 128) << 8);
  if (bits == 32) {
    if (audioFmt == 3) {
      float fv = 0.0f;
      memcpy(&fv, p, sizeof(float));
      if (!(fv >= -4.0f && fv <= 4.0f)) return 0;
      return (int16_t)constrain((int32_t)(fv * 32767.0f), -32768, 32767);
    }
    return (int16_t)((uint16_t)p[2] | ((uint16_t)p[3] << 8));
  }
  return 0;
}

FLASHMEM bool storageLoadWavFileToTrack(int dstTrack, uint32_t destStartFrame, const char *fullPath) {
  if (dstTrack < 0 || dstTrack >= NUM_AUDIO_TRACKS || destStartFrame >= MAX_REC_FRAMES) return false;
  if (!fullPath || !fullPath[0]) return false;
  if (transport.mode != TransportMode::Stopped) {
    storageStopTransport();
  }

  FsFile srcFile = SD.sdfs.open(fullPath, O_RDONLY);
  if (!srcFile.isOpen()) return false;
  uint64_t fSize = srcFile.fileSize();
  if (fSize < 44) {
    srcFile.close();
    return false;
  }

  alignas(4) uint8_t hdr[44];
  if (!srcFile.seekSet(0) || srcFile.read(hdr, 12) != 12 ||
      memcmp(hdr, "RIFF", 4) != 0 || memcmp(hdr + 8, "WAVE", 4) != 0) {
    srcFile.close();
    return false;
  }

  bool gotFmt = false;
  uint16_t audioFmt = 1, numCh = 1, blockAlign = 0, bits = 16;
  uint32_t sRate = SAMPLE_RATE_HZ;
  uint64_t dataOffset = 0;
  uint32_t srcTotalFrames = 0;
  uint8_t srcBpf = 4;

  uint64_t pos = 12;
  while (pos + 8 <= fSize) {
    if (!srcFile.seekSet(pos)) break;
    alignas(4) uint8_t chdr[8];
    if (srcFile.read(chdr, 8) != 8) break;
    uint32_t cSize = (uint32_t)chdr[4] | ((uint32_t)chdr[5] << 8) |
                     ((uint32_t)chdr[6] << 16) | ((uint32_t)chdr[7] << 24);
    uint64_t dataPos = pos + 8;

    if (memcmp(chdr, "fmt ", 4) == 0 && cSize >= 16) {
      size_t toRead = (size_t)min((uint32_t)sizeof(hdr), cSize);
      if (srcFile.read(hdr, toRead) != (int)toRead) break;
      audioFmt   = (uint16_t)hdr[0] | ((uint16_t)hdr[1] << 8);
      numCh      = (uint16_t)hdr[2] | ((uint16_t)hdr[3] << 8);
      sRate      = (uint32_t)hdr[4] | ((uint32_t)hdr[5] << 8) | ((uint32_t)hdr[6] << 16) | ((uint32_t)hdr[7] << 24);
      blockAlign = (uint16_t)hdr[12] | ((uint16_t)hdr[13] << 8);
      bits       = (uint16_t)hdr[14] | ((uint16_t)hdr[15] << 8);
      if (audioFmt == 0xFFFEu && toRead >= 26) {
        audioFmt = (uint16_t)hdr[24] | ((uint16_t)hdr[25] << 8);
      }
      gotFmt = true;
    } else if (memcmp(chdr, "data", 4) == 0 && gotFmt) {
      if ((audioFmt == 1 || audioFmt == 3) &&
          numCh >= 1 && numCh <= 4 &&
          (bits == 8 || bits == 16 || bits == 24 || bits == 32) &&
          (audioFmt != 3 || bits == 32) &&
          sRate >= 4000 && sRate <= 192000) {
        uint8_t bytesPerCh = (uint8_t)((bits + 7u) / 8u);
        uint8_t bpf = (uint8_t)(numCh * bytesPerCh);
        if (blockAlign >= bpf && blockAlign <= 16 && (blockAlign % numCh) == 0) {
          bpf = (uint8_t)blockAlign;
        }
        uint64_t remBytes = (fSize > dataPos) ? (fSize - dataPos) : 0;
        uint64_t availBytes = (cSize == 0 || cSize > remBytes) ? remBytes : (uint64_t)cSize;
        uint32_t frames = (bpf > 0) ? (uint32_t)(availBytes / bpf) : 0;
        if (frames > 0) {
          dataOffset = dataPos;
          srcTotalFrames = frames;
          srcBpf = bpf;
        }
      }
      break;
    }
    uint64_t nextPos = dataPos + (uint64_t)cSize + (cSize & 1u);
    if (nextPos <= pos) break;
    pos = nextPos;
  }

  if (srcTotalFrames == 0 || !srcFile.seekSet(dataOffset)) {
    srcFile.close();
    return false;
  }

  FsFile clipFile = SD.sdfs.open(AUDIO_CLIP_PATH, O_RDWR | O_CREAT | O_TRUNC);
  if (!clipFile.isOpen()) {
    srcFile.close();
    return false;
  }

  const uint32_t maxOutFrames = (uint32_t)MAX_REC_FRAMES - destStartFrame;
  const uint32_t srcLimitFrames = min(srcTotalFrames, maxOutFrames);
  const uint8_t chStride = (numCh > 0) ? (srcBpf / numCh) : (uint8_t)(bits / 8);
  uint8_t *rawIn = reinterpret_cast<uint8_t *>(chunk) + 8192;
  const uint32_t maxBatch = min(2048u, (uint32_t)(8192u / srcBpf));
  uint32_t remFrames = srcLimitFrames;
  uint32_t writtenFrames = 0;
  bool ok = true;

  while (remFrames > 0) {
    uint32_t n = min(remFrames, maxBatch);
    size_t rdBytes = (size_t)n * srcBpf;
    int rd = srcFile.read(rawIn, rdBytes);
    uint32_t got = (rd > 0) ? ((uint32_t)rd / srcBpf) : 0;
    if (got == 0) break;

    for (uint32_t i = 0; i < got; i++) {
      const uint8_t *fp = rawIn + i * srcBpf;
      int16_t sL = decodeClipWavSample(fp, bits, audioFmt);
      int16_t sR = (numCh >= 2) ? decodeClipWavSample(fp + chStride, bits, audioFmt) : sL;
      chunk[i] = (uint32_t)(uint16_t)sL | ((uint32_t)(uint16_t)sR << 16);
    }

    size_t wrBytes = (size_t)got * sizeof(uint32_t);
    if (clipFile.write(chunk, wrBytes) != wrBytes) {
      ok = false;
      break;
    }
    writtenFrames += got;
    remFrames -= got;
  }

  srcFile.close();
  clipFile.truncate();
  clipFile.sync();
  clipFile.close();

  if (!ok || writtenFrames == 0) {
    storageRemoveClipboardFiles();
    return false;
  }

  bool pasted = storagePasteAudioClipToTrack(dstTrack, destStartFrame, writtenFrames);
  storageRemoveClipboardFiles();
  return pasted;
}

FLASHMEM bool storageDeleteAudioRegion(int t, uint32_t startFrame, uint32_t endFrame) {
  if (t < 0 || t >= NUM_AUDIO_TRACKS || startFrame >= endFrame) return false;
  if (transport.mode != TransportMode::Stopped) {
    storageStopTransport();
  }

  Track &tr = tracks[t];
  if (tr.recordedFrames == 0 || startFrame >= tr.recordedFrames) {
    return true;
  }

  uint32_t delEnd = min(endFrame, tr.recordedFrames);
  if (startFrame == 0 && delEnd >= tr.recordedFrames) {
    clearAudioTrack(t);
    return true;
  }

  if (delEnd >= tr.recordedFrames) {
    tr.recordedFrames = startFrame;
    if (tr.file.isOpen()) {
      uint64_t validBytes = trackWavByteOffset(t, tr.recordedFrames);
      if (tr.file.fileSize() > validBytes && tr.file.seekSet(validBytes)) {
        tr.file.truncate();
      }
      writeWavHeader(tr.file, tr.recordedFrames, t);
      tr.file.sync();
    }
  } else if (tr.file.isOpen()) {
    const uint32_t bpf = audioTrackBytesPerFrame(t);
    uint32_t curFrame = startFrame;
    memset(chunk, 0, sizeof(chunk));
    while (curFrame < delEnd) {
      uint32_t n = min((uint32_t)CHUNK_FRAMES, delEnd - curFrame);
      uint64_t bytePos = trackWavByteOffset(t, curFrame);
      if (!tr.file.seekSet(bytePos) || tr.file.write(chunk, n * bpf) != (size_t)(n * bpf)) {
        break;
      }
      curFrame += n;
    }
    tr.file.sync();
  }

  rebuildTrackOverviewFromFile(t);
  prefillTrackStreams(transport.tapeFrame);
  songDirty = true;
  return true;
}

// Apply a 16-sample linear edge ramp to `chunk` to prevent clicks between scrub grains
FLASHMEM static void taperScrubChunk(uint32_t n, bool isMono) {
  constexpr uint32_t TAPER = 16;
  if (n < 2 * TAPER) return;
  if (isMono) {
    int16_t *s16 = reinterpret_cast<int16_t *>(chunk);
    for (uint32_t i = 0; i < TAPER; i++) {
      int32_t g = (int32_t)i;
      s16[i] = (int16_t)(((int32_t)s16[i] * g) >> 4);
      uint32_t j = n - 1u - i;
      s16[j] = (int16_t)(((int32_t)s16[j] * g) >> 4);
    }
  } else {
    for (uint32_t i = 0; i < TAPER; i++) {
      int32_t g = (int32_t)i;
      uint32_t f0 = chunk[i];
      int16_t l0 = (int16_t)(((int32_t)(int16_t)(f0 & 0xFFFFu) * g) >> 4);
      int16_t r0 = (int16_t)(((int32_t)(int16_t)(f0 >> 16) * g) >> 4);
      chunk[i] = (uint16_t)l0 | ((uint32_t)(uint16_t)r0 << 16);

      uint32_t j = n - 1u - i;
      uint32_t f1 = chunk[j];
      int16_t l1 = (int16_t)(((int32_t)(int16_t)(f1 & 0xFFFFu) * g) >> 4);
      int16_t r1 = (int16_t)(((int32_t)(int16_t)(f1 >> 16) * g) >> 4);
      chunk[j] = (uint16_t)l1 | ((uint32_t)(uint16_t)r1 << 16);
    }
  }
}

// Fill up to `maxChunks` (4096 frames each; or 1024-frame scrub grains when |speed| > 2.00x)
// into track t's ring buffer from tr.fileFrame
FLASHMEM static void fillTrackRing(int t, int maxChunks) {
  Track &tr = tracks[t];
  const uint32_t bpf = audioTrackBytesPerFrame(t);
  const bool isMono = isAudioTrackMono(t);
  const int8_t sq = transport.speedQuarter;
  const bool rev = (sq < 0);
  const uint32_t absQ = (uint32_t)(rev ? -sq : sq);
  const bool scrubMode = (absQ > 8u);
  constexpr uint32_t SCRUB_GRAIN_FRAMES = 1024;
  const uint32_t maxBlock = scrubMode ? SCRUB_GRAIN_FRAMES : CHUNK_FRAMES;
  const int maxIters = scrubMode ? (maxChunks * 4) : maxChunks;
  const bool loopOn = transport.loopEnabled && (transport.punchOutFrame > transport.punchInFrame);
  for (int c = 0; c < maxIters; c++) {
    uint32_t queued = tr.head - tr.tail;
    if (queued + maxBlock > STREAM_FRAMES) break;

    if (!rev) {
      uint32_t n = maxBlock;
      if (!scrubMode && loopOn && tr.fileFrame < transport.punchOutFrame) {
        uint32_t toLoopEnd = transport.punchOutFrame - tr.fileFrame;
        if (toLoopEnd < n) n = toLoopEnd;
      }
      const bool skipReplaceRead =
          (t == recording.track && recording.activeInWindow && !recording.overdub &&
           (!transport.punchEnabled || (tr.fileFrame >= transport.punchInFrame && tr.fileFrame + n <= transport.punchOutFrame)));
      if (!skipReplaceRead && tr.file.isOpen() && tr.fileFrame < tr.recordedFrames) {
        uint32_t availFile = tr.recordedFrames - tr.fileFrame;
        uint32_t toRead = min(n, availFile);
        uint64_t bytePos = trackWavByteOffset(t, tr.fileFrame);
        if (tr.file.curPosition() == bytePos || tr.file.seekSet(bytePos)) {
          int rd = tr.file.read(chunk, toRead * bpf);
          uint32_t gotFrames = (rd > 0) ? ((uint32_t)rd / bpf) : 0;
          if (gotFrames < n) {
            memset((uint8_t *)chunk + gotFrames * bpf, 0, (n - gotFrames) * bpf);
          }
        } else {
          memset(chunk, 0, n * bpf);
        }
      } else {
        memset(chunk, 0, n * bpf);
      }

      if (scrubMode) {
        taperScrubChunk(n, isMono);
      }

      uint32_t h = tr.head;
      if (isMono) {
        int16_t *dstRing = reinterpret_cast<int16_t *>(tr.ring);
        const int16_t *srcChunk = reinterpret_cast<const int16_t *>(chunk);
        for (uint32_t i = 0; i < n; i++) {
          dstRing[(h + i) % RING_FRAMES] = srcChunk[i];
        }
      } else {
        for (uint32_t i = 0; i < n; i++) {
          tr.ring[(h + i) % RING_FRAMES] = chunk[i];
        }
      }
      __asm__ volatile("" ::: "memory");
      tr.head = h + n;

      const uint32_t stepFile = scrubMode ? max(n, (n * absQ) >> 3) : n;
      if (loopOn && tr.fileFrame < transport.punchOutFrame &&
          tr.fileFrame + stepFile >= transport.punchOutFrame) {
        tr.fileFrame = transport.punchInFrame;
      } else {
        tr.fileFrame += stepFile;
      }
    } else {
      // Reverse playback ring filling (reads contiguous block [firstFrame..tr.fileFrame] and reverses into ring)
      if (tr.fileFrame == 0xFFFFFFFFu) {
        uint32_t n = maxBlock;
        memset(chunk, 0, n * bpf);
        uint32_t h = tr.head;
        if (isMono) {
          int16_t *dstRing = reinterpret_cast<int16_t *>(tr.ring);
          for (uint32_t i = 0; i < n; i++) dstRing[(h + i) % RING_FRAMES] = 0;
        } else {
          for (uint32_t i = 0; i < n; i++) tr.ring[(h + i) % RING_FRAMES] = 0;
        }
        __asm__ volatile("" ::: "memory");
        tr.head = h + n;
        continue;
      }

      uint32_t n = min(maxBlock, tr.fileFrame + 1u);
      if (!scrubMode && loopOn && tr.fileFrame >= transport.punchInFrame) {
        uint32_t toLoopStart = (tr.fileFrame - transport.punchInFrame) + 1u;
        if (toLoopStart < n) n = toLoopStart;
      }
      uint32_t firstFrame = tr.fileFrame + 1u - n;
      const bool skipReplaceRead =
          (t == recording.track && recording.activeInWindow && !recording.overdub &&
           (!transport.punchEnabled || (firstFrame >= transport.punchInFrame && tr.fileFrame < transport.punchOutFrame)));

      if (!skipReplaceRead && tr.file.isOpen() && firstFrame < tr.recordedFrames) {
        uint32_t availFile = min(n, tr.recordedFrames - firstFrame);
        uint64_t bytePos = trackWavByteOffset(t, firstFrame);
        if (tr.file.curPosition() == bytePos || tr.file.seekSet(bytePos)) {
          int rd = tr.file.read(chunk, availFile * bpf);
          uint32_t gotFrames = (rd > 0) ? ((uint32_t)rd / bpf) : 0;
          if (gotFrames < n) {
            memset((uint8_t *)chunk + gotFrames * bpf, 0, (n - gotFrames) * bpf);
          }
        } else {
          memset(chunk, 0, n * bpf);
        }
      } else {
        memset(chunk, 0, n * bpf);
      }

      if (scrubMode) {
        taperScrubChunk(n, isMono);
      }

      uint32_t h = tr.head;
      if (isMono) {
        int16_t *dstRing = reinterpret_cast<int16_t *>(tr.ring);
        const int16_t *srcChunk = reinterpret_cast<const int16_t *>(chunk);
        for (uint32_t i = 0; i < n; i++) {
          dstRing[(h + i) % RING_FRAMES] = srcChunk[n - 1u - i];
        }
      } else {
        for (uint32_t i = 0; i < n; i++) {
          tr.ring[(h + i) % RING_FRAMES] = chunk[n - 1u - i];
        }
      }
      __asm__ volatile("" ::: "memory");
      tr.head = h + n;

      const uint32_t stepFile = scrubMode ? max(n, (n * absQ) >> 3) : n;
      if (loopOn && tr.fileFrame >= transport.punchInFrame &&
          (tr.fileFrame - transport.punchInFrame) < stepFile) {
        tr.fileFrame = (transport.punchOutFrame > 0) ? (transport.punchOutFrame - 1u) : 0u;
      } else if (tr.fileFrame >= stepFile) {
        tr.fileFrame -= stepFile;
      } else {
        tr.fileFrame = 0xFFFFFFFFu;
      }
    }
  }
}

FLASHMEM void prefillTrackStreams(uint32_t fromTapeFrame) {
  for (int t = 0; t < NUM_AUDIO_TRACKS; t++) {
    Track &tr = tracks[t];
    noInterrupts();
    tr.live = false;
    tr.head = 0;
    tr.tail = 0;
    memset(tr.curMix, 0, sizeof(tr.curMix));
    interrupts();
    tr.fileFrame = fromTapeFrame;
    if (tr.recordedFrames > 0 || tr.armMode != ArmMode::Off) {
      fillTrackRing(t, 2); // pre-fill 8,192 frames (~186 ms)
      noInterrupts();
      tr.live = true;
      interrupts();
    }
  }
}

// Extend `f` with zeroed audio frames from `f.fileSize()` up to `targetBytePos`.
// Required because SdFat's `seekSet(pos)` returns false whenever `pos > f.fileSize()`,
// which otherwise prevents writing at `punchInFrame > tr.recordedFrames`.
FLASHMEM static bool padTrackFileToBytePos(FsFile &f, uint64_t targetBytePos, bool keepStreamsAlive, int trackIdx = 0) {
  if (!f.isOpen()) return false;
  uint64_t curSize = f.fileSize();
  if (curSize >= targetBytePos) return true;

  if (curSize < WAV_DATA_OFFSET) {
    if (!writeWavHeader(f, 0, trackIdx)) return false;
    curSize = f.fileSize();
    if (curSize >= targetBytePos) return true;
  }

  while (curSize < targetBytePos) {
    if (f.curPosition() != curSize && !f.seekSet(curSize)) return false;
    uint64_t rem = targetBytePos - curSize;
    size_t toWrite = (size_t)min(rem, (uint64_t)sizeof(chunk));
    memset(chunk, 0, toWrite);
    size_t wr = f.write(chunk, toWrite);
    if (wr == 0) return false;
    curSize += (uint64_t)wr;
    if (keepStreamsAlive) {
      keepStreaming();
    }
  }
  return true;
}

FLASHMEM void storageSeekTape(uint32_t tapeFrame) {
  tapeFrame = min(tapeFrame, MAX_REC_FRAMES);
  bool wasRunning = (transport.mode == TransportMode::Running);
  bool wasRecWindow = false;
  if (wasRunning) {
    uint32_t stopNow = transport.now;
    noInterrupts();
    transport.mode = TransportMode::Stopped;
    transport.running = false;
    if (recording.activeInWindow) {
      recording.activeInWindow = false;
      recording.mode = Recording::Finishing;
      recording.endNow = stopNow;
      recording.windowExited = true;
      wasRecWindow = true;
    }
    interrupts();
  }
  if (wasRecWindow || recording.windowExited) {
    drainRecordingBuffer(true);
    int recTrk = recording.track;
    if (recTrk >= 0 && tracks[recTrk].file.isOpen() && tracks[recTrk].recordedFrames > 0) {
      writeWavHeader(tracks[recTrk].file, tracks[recTrk].recordedFrames, recTrk);
      tracks[recTrk].file.sync();
    }
    recording.windowExited = false;
  }

  noInterrupts();
  transport.tapeFrame = tapeFrame;
  transport.subFrame = 0;
  transport.beat = (int32_t)framesToBeats(tapeFrame, bpm);
  transport.sampleInBeat = 0;
  if (recording.track >= 0 && recording.mode != Recording::Idle) {
    recording.reverse = (transport.speedQuarter < 0);
    recording.startTapeFrame = tapeFrame;
    recording.writeTapeFrame = tapeFrame;
    recording.readNow = transport.now;
    recording.lastOverviewCol = -1;
    recording.mode = Recording::Armed;
  }
  interrupts();

  midiSeqOnSeek(tapeFrame, bpm);
  prefillTrackStreams(tapeFrame);

  if (wasRunning) {
    noInterrupts();
    transport.mode = TransportMode::Running;
    transport.running = true;
    interrupts();
  }
}

FLASHMEM void storageSetPlaybackSpeedQuarter(int8_t speedQuarter) {
  if (speedQuarter < MIN_SPEED_QUARTER) speedQuarter = MIN_SPEED_QUARTER;
  if (speedQuarter > MAX_SPEED_QUARTER) speedQuarter = MAX_SPEED_QUARTER;
  int8_t oldQ = transport.speedQuarter;
  if (speedQuarter == oldQ) return;

  if (transport.mode != TransportMode::Running) {
    noInterrupts();
    transport.speedQuarter = speedQuarter;
    transport.subFrame = 0;
    interrupts();
    return;
  }

  if ((oldQ > 0 && speedQuarter > 0) || (oldQ < 0 && speedQuarter < 0)) {
    const bool wasScrub = (abs((int)oldQ) > 8);
    const bool nowScrub = (abs((int)speedQuarter) > 8);
    noInterrupts();
    transport.speedQuarter = speedQuarter;
    interrupts();
    if (wasScrub != nowScrub) {
      prefillTrackStreams(transport.tapeFrame);
    }
  } else if (speedQuarter == 0) {
    noInterrupts();
    transport.speedQuarter = 0;
    transport.subFrame = 0;
    interrupts();
    midiSeqAllNotesOff();
  } else {
    noInterrupts();
    transport.speedQuarter = speedQuarter;
    transport.subFrame = 0;
    interrupts();
    storageSeekTape(transport.tapeFrame);
  }
}

FLASHMEM static void ensureTrackWavOpen(int recTrk) {
  Track &tr = tracks[recTrk];
  if (!tr.file.isOpen()) {
    char wPath[64];
    ensureSongAudioDir(activeSongName);
    trackWavByName(activeSongName, recTrk, wPath, sizeof(wPath));
    const oflag_t openFlags = (tr.recordedFrames == 0) ? (O_RDWR | O_CREAT | O_TRUNC) : (O_RDWR | O_CREAT);
    tr.file = SD.sdfs.open(wPath, openFlags);
    if (tr.file.isOpen() && tr.recordedFrames == 0) {
      writeWavHeader(tr.file, 0, recTrk);
      tr.file.truncate();
      tr.file.sync();
    }
  }
  if (tr.file.isOpen()) {
    const uint64_t validBytes = trackWavByteOffset(recTrk, tr.recordedFrames);
    if (tr.file.fileSize() > validBytes && tr.file.seekSet(validBytes)) {
      tr.file.truncate();
      writeWavHeader(tr.file, tr.recordedFrames, recTrk);
      tr.file.sync();
    }
  }
}

FLASHMEM static void armRecordingStateForTrack(int recTrk, uint32_t tf) {
  noInterrupts();
  if (recTrk >= 0) {
    AudioRecSrc src = tracks[recTrk].recSrc;
    bool autoMute = isAutoMuteRecSrc(src);
    recording.track = recTrk;
    recording.overdub = (tracks[recTrk].armMode == ArmMode::Overdub);
    recording.src = src;
    recording.autoMute = autoMute;
    recording.monoInput = tracks[recTrk].monoInput;
    recording.reverse = (transport.speedQuarter < 0);
    if (autoMute) {
      tracks[recTrk].state = TrackState::Muted;
    }
    recording.activeInWindow = false;
    recording.windowEntered = false;
    recording.windowExited = false;
    recording.readNow = transport.now;
    recording.startTapeFrame = tf;
    recording.writeTapeFrame = tf;
    recording.writtenFrames = 0;
    recording.lastOverviewCol = -1;
    recording.mode = Recording::Armed;
  } else {
    recording.track = -1;
    recording.src = AudioRecSrc::Off;
    recording.autoMute = false;
    recording.reverse = false;
    recording.mode = Recording::Idle;
    recording.activeInWindow = false;
  }
  interrupts();
}

FLASHMEM void storageSyncArmedAudioTrack() {
  if (transport.mode == TransportMode::Stopped) return;

  int newRecTrk = -1;
  for (int t = 0; t < NUM_AUDIO_TRACKS; t++) {
    if (tracks[t].armMode != ArmMode::Off && tracks[t].recSrc != AudioRecSrc::Off) {
      newRecTrk = t;
      break;
    }
  }

  int oldRecTrk = recording.track;
  if (newRecTrk == oldRecTrk && newRecTrk >= 0) {
    AudioRecSrc newSrc = tracks[newRecTrk].recSrc;
    bool newAutoMute = isAutoMuteRecSrc(newSrc);
    noInterrupts();
    if (recording.autoMute && !newAutoMute) {
      tracks[newRecTrk].state = (tracks[newRecTrk].recordedFrames > 0) ? TrackState::Playing : TrackState::Empty;
    } else if (newAutoMute) {
      tracks[newRecTrk].state = TrackState::Muted;
    }
    recording.overdub = (tracks[newRecTrk].armMode == ArmMode::Overdub);
    recording.src = newSrc;
    recording.autoMute = newAutoMute;
    recording.monoInput = tracks[newRecTrk].monoInput;
    recording.reverse = (transport.speedQuarter < 0);
    if (recording.mode == Recording::Idle) {
      recording.startTapeFrame = transport.tapeFrame;
      recording.writeTapeFrame = transport.tapeFrame;
      recording.readNow = transport.now;
      recording.lastOverviewCol = -1;
      recording.mode = Recording::Armed;
    }
    interrupts();
    return;
  }

  if (oldRecTrk >= 0 && (recording.activeInWindow || recording.windowExited)) {
    uint32_t stopNow = transport.now;
    noInterrupts();
    if (recording.activeInWindow) {
      recording.activeInWindow = false;
      recording.mode = Recording::Finishing;
      recording.endNow = stopNow;
      recording.windowExited = true;
    }
    interrupts();
    drainRecordingBuffer(true);
    if (tracks[oldRecTrk].file.isOpen()) {
      if (tracks[oldRecTrk].recordedFrames > 0) {
        uint64_t validBytes = trackWavByteOffset(oldRecTrk, tracks[oldRecTrk].recordedFrames);
        if (tracks[oldRecTrk].file.fileSize() > validBytes && tracks[oldRecTrk].file.seekSet(validBytes)) {
          tracks[oldRecTrk].file.truncate();
        }
        writeWavHeader(tracks[oldRecTrk].file, tracks[oldRecTrk].recordedFrames, oldRecTrk);
        tracks[oldRecTrk].file.sync();
      } else {
        tracks[oldRecTrk].file.close();
        char wPath[64];
        trackWavByName(activeSongName, oldRecTrk, wPath, sizeof(wPath));
        if (SD.sdfs.exists(wPath)) {
          SD.sdfs.remove(wPath);
        }
      }
    }
    recording.windowExited = false;
  }
  if (oldRecTrk >= 0 && oldRecTrk < NUM_AUDIO_TRACKS && recording.autoMute) {
    tracks[oldRecTrk].state = (tracks[oldRecTrk].recordedFrames > 0) ? TrackState::Playing : TrackState::Empty;
    recording.autoMute = false;
  }

  if (newRecTrk >= 0) {
    Track &tr = tracks[newRecTrk];
    ensureTrackWavOpen(newRecTrk);
    if (!tr.live) {
      uint32_t curTf = transport.tapeFrame;
      tr.fileFrame = curTf;
      fillTrackRing(newRecTrk, 2);
      noInterrupts();
      tr.live = true;
      interrupts();
    }
    armRecordingStateForTrack(newRecTrk, transport.tapeFrame);
  } else {
    armRecordingStateForTrack(-1, 0);
  }

  if (transport.mode == TransportMode::WaitThreshold && transport.waitAudioThresh) {
    if (newRecTrk < 0 || tracks[newRecTrk].trigMode != TrigMode::Threshold) {
      noInterrupts();
      transport.waitAudioThresh = false;
      if (!transport.waitMidiThresh && transport.mode == TransportMode::WaitThreshold) {
        transport.mode = TransportMode::Running;
        transport.running = true;
        transport.sampleInBeat = 0;
        midiSendNow(0xFA);
      }
      interrupts();
    }
  }
}

FLASHMEM void storageSyncLoopState() {
  const SongSettings &ps = songSettings();
  transport.punchEnabled = ps.punchEnabled;
  transport.loopEnabled = ps.loopEnabled;
  transport.punchInFrame = ps.punchInFrame;
  transport.punchOutFrame = ps.punchOutFrame;
  if (transport.mode == TransportMode::Running) {
    if (ps.loopEnabled && ps.punchOutFrame > ps.punchInFrame) {
      if (transport.speedQuarter < 0) {
        if (transport.tapeFrame < ps.punchInFrame || transport.tapeFrame >= ps.punchOutFrame) {
          storageSeekTape(ps.punchOutFrame > 0 ? (ps.punchOutFrame - 1u) : 0u);
          return;
        }
      } else if (transport.tapeFrame >= ps.punchOutFrame) {
        storageSeekTape(ps.punchInFrame);
        return;
      }
    }
    prefillTrackStreams(transport.tapeFrame);
  }
}

FLASHMEM void storageStartTransport(bool countIn, bool waitAudio, bool waitMidi) {
  int recTrk = -1;
  for (int t = 0; t < NUM_AUDIO_TRACKS; t++) {
    if (tracks[t].armMode != ArmMode::Off && tracks[t].recSrc != AudioRecSrc::Off) {
      recTrk = t;
      break;
    }
  }
  bool midiArmed = false;
  for (int t = 0; t < NUM_MIDI_TRACKS; t++) {
    if (midiTracks[t].armMode != ArmMode::Off) {
      midiArmed = true;
      break;
    }
  }
  bool anyArmed = (recTrk >= 0) || midiArmed;
  if (anyArmed &&
      transport.speedQuarter != DEFAULT_SPEED_QUARTER &&
      transport.speedQuarter != REVERSE_1X_SPEED_QUARTER) {
    transport.speedQuarter = DEFAULT_SPEED_QUARTER;
  }

  uint32_t tf = transport.tapeFrame;
  const bool loopOn = transport.loopEnabled && (transport.punchOutFrame > transport.punchInFrame);
  const bool rev = (transport.speedQuarter < 0);

  uint32_t maxContent = 0;
  for (int t = 0; t < NUM_AUDIO_TRACKS; t++) {
    if (tracks[t].recordedFrames > maxContent) maxContent = tracks[t].recordedFrames;
  }
  for (int t = 0; t < NUM_MIDI_TRACKS; t++) {
    uint32_t cnt = 0;
    const MidiEvent *evs = midiSeqEvents(t, cnt);
    if (cnt > 0 && evs) {
      uint32_t endF = midiTicksToFrames(evs[cnt - 1].tick, bpm);
      if (endF > maxContent) maxContent = endF;
    }
  }

  if (rev) {
    if (tf >= MAX_REC_FRAMES) {
      tf = loopOn ? (transport.punchOutFrame > 0 ? (uint32_t)transport.punchOutFrame - 1u : 0u) : (MAX_REC_FRAMES - 1u);
    } else if (anyArmed && transport.punchEnabled && tf < transport.punchOutFrame) {
      tf = transport.punchOutFrame;
    } else if (loopOn && (tf <= transport.punchInFrame || tf >= transport.punchOutFrame)) {
      tf = (transport.punchOutFrame > 0) ? ((uint32_t)transport.punchOutFrame - 1u) : 0u;
    } else if (!loopOn && tf == 0) {
      if (maxContent > 0) {
        tf = min(maxContent, MAX_REC_FRAMES - 1u);
      } else if (anyArmed && transport.punchEnabled && transport.punchOutFrame > 0) {
        tf = transport.punchOutFrame;
      }
    }
  } else {
    if (tf >= MAX_REC_FRAMES) {
      tf = loopOn ? (uint32_t)transport.punchInFrame : 0;
    } else if (anyArmed && transport.punchEnabled && tf > transport.punchInFrame) {
      tf = 0;
    } else if (loopOn && tf >= transport.punchOutFrame) {
      tf = transport.punchInFrame;
    } else if (!anyArmed && !loopOn) {
      if (maxContent > 0 && tf >= maxContent) {
        tf = 0;
      }
    }
  }
  transport.tapeFrame = tf;
  transport.subFrame = 0;

  if (recTrk >= 0) {
    Track &tr = tracks[recTrk];
    ensureTrackWavOpen(recTrk);
    // Pre-pad WAV file with silence up to the frame where recording will start
    uint32_t firstRecFrame = tf;
    if (transport.punchEnabled) {
      firstRecFrame = rev ? min(tf, (uint32_t)transport.punchOutFrame) : max(tf, (uint32_t)transport.punchInFrame);
    }
    uint64_t neededBytes = trackWavByteOffset(recTrk, firstRecFrame);
    if (tr.file.isOpen() && tr.file.fileSize() < neededBytes) {
      padTrackFileToBytePos(tr.file, neededBytes, false, recTrk);
      tr.file.sync();
    }
  }
  armRecordingStateForTrack(recTrk, tf);

  prefillTrackStreams(tf);
  midiSeqOnTransportStart(tf, bpm);

  float threshLin = 0.0316f;
  if (recTrk >= 0) {
    threshLin = powf(10.0f, (float)tracks[recTrk].threshDb / 20.0f);
  }

  noInterrupts();
  transport.beat = (int32_t)framesToBeats(tf, bpm);
  transport.sampleInBeat = 0;
  transport.waitAudioThresh = waitAudio;
  transport.waitMidiThresh = waitMidi;
  transport.audioThreshLinear = threshLin;

  if (waitAudio || waitMidi) {
    transport.mode = TransportMode::WaitThreshold;
    transport.running = false;
  } else if (countIn && songSettings().countInBeats > 0) {
    transport.countInTotalBeats = songSettings().countInBeats;
    transport.countInRemainingBeats = songSettings().countInBeats;
    transport.mode = TransportMode::CountIn;
    transport.running = false;
  } else {
    transport.mode = TransportMode::Running;
    transport.running = true;
    midiSendNow(0xFA);
  }
  interrupts();
}

FLASHMEM static void drainRecordingBuffer(bool flushAll) {
  int recTrk = recording.track;
  if (recTrk < 0 || recTrk >= NUM_AUDIO_TRACKS) return;
  Track &tr = tracks[recTrk];
  if (!tr.file.isOpen()) return;

  // Always latch `windowEntered` before computing `avail` so `readNow` and `writeTapeFrame`
  // are never stale even if the Audio ISR entered the punch window during `fillTrackRing()`.
  if (recording.windowEntered) {
    noInterrupts();
    recording.windowEntered = false;
    uint32_t sNow = recording.startNow;
    uint32_t sTape = recording.startTapeFrame;
    interrupts();
    recording.readNow = sNow;
    recording.writeTapeFrame = sTape;
    recording.lastOverviewCol = -1;
  }

  const bool rev = recording.reverse;
  for (;;) {
    uint32_t limitNow = recording.activeInWindow ? transport.now : recording.endNow;
    int32_t avail = (int32_t)(limitNow - recording.readNow);
    if (avail <= 0) break;
    if (avail > (int32_t)INPUT_FRAMES) {
      uint32_t drop = (uint32_t)avail - INPUT_FRAMES;
      recording.readNow += drop;
      if (rev) {
        recording.writeTapeFrame = (recording.writeTapeFrame >= drop) ? (recording.writeTapeFrame - drop) : 0u;
      } else {
        recording.writeTapeFrame += drop;
      }
      avail = (int32_t)INPUT_FRAMES;
    }
    if (!flushAll && (uint32_t)avail < CHUNK_FRAMES) break;

    const uint32_t bpf = audioTrackBytesPerFrame(recTrk);
    const bool isMono = isAudioTrackMono(recTrk);
    uint32_t n = min((uint32_t)avail, CHUNK_FRAMES);
    if (rev) {
      uint32_t maxRev = recording.writeTapeFrame + 1u;
      if (n > maxRev) n = maxRev;
      if (n == 0) {
        recording.readNow += (uint32_t)avail;
        break;
      }
    }

    uint32_t firstTapeFrame = rev ? (recording.writeTapeFrame + 1u - n) : recording.writeTapeFrame;
    uint64_t bytePos = trackWavByteOffset(recTrk, firstTapeFrame);

    if (tr.file.fileSize() < bytePos) {
      padTrackFileToBytePos(tr.file, bytePos, !flushAll, recTrk);
    }

    uint32_t rNow = recording.readNow;
    if (isMono) {
      int16_t *monoChunk = reinterpret_cast<int16_t *>(chunk);
      for (uint32_t i = 0; i < n; i++) {
        uint32_t srcIdx = rev ? (rNow + (n - 1u - i)) : (rNow + i);
        monoChunk[i] = (int16_t)(inputRing[srcIdx % INPUT_FRAMES] & 0xFFFF);
      }
    } else {
      for (uint32_t i = 0; i < n; i++) {
        uint32_t srcIdx = rev ? (rNow + (n - 1u - i)) : (rNow + i);
        chunk[i] = inputRing[srcIdx % INPUT_FRAMES];
      }
    }

    bool wroteOk = false;
    if (tr.file.curPosition() == bytePos || tr.file.seekSet(bytePos)) {
      wroteOk = (tr.file.write(chunk, n * bpf) == (size_t)(n * bpf));
    }

    updateTrackOverviewChunk(recTrk, firstTapeFrame, chunk, n);

    recording.readNow += n;
    if (rev) {
      recording.writeTapeFrame = (recording.writeTapeFrame >= n) ? (recording.writeTapeFrame - n) : 0u;
    } else {
      recording.writeTapeFrame += n;
    }
    if (wroteOk) {
      recording.writtenFrames += n;
      uint32_t endFrame = firstTapeFrame + n;
      if (endFrame > tr.recordedFrames) {
        tr.recordedFrames = endFrame;
      }
      if (tr.state == TrackState::Empty) {
        tr.state = TrackState::Playing;
      }
      songDirty = true;
    }
    if (!flushAll) break;
  }
}

FLASHMEM void storageStopTransport() {
  // Manual save only: stopping transport flushes active WAV stream but never auto-saves SONG.DAT / MIDI.DAT
  bool wasRunning = (transport.mode != TransportMode::Stopped);
  bool wasActive = recording.activeInWindow;
  uint32_t stopNow = transport.now;
  noInterrupts();
  transport.mode = TransportMode::Stopped;
  transport.running = false;
  if (recording.activeInWindow) {
    recording.activeInWindow = false;
    recording.mode = Recording::Finishing;
    recording.endNow = stopNow;
    recording.windowExited = true;
  }
  interrupts();

  if (wasRunning) {
    midiSendNow(0xFC);
  }
  midiSeqOnTransportStop();

  int recTrk = recording.track;
  if (wasActive || recording.windowExited || recording.mode == Recording::Finishing) {
    drainRecordingBuffer(true);
    if (recTrk >= 0 && tracks[recTrk].file.isOpen() && tracks[recTrk].recordedFrames > 0) {
      uint64_t validBytes = trackWavByteOffset(recTrk, tracks[recTrk].recordedFrames);
      if (tracks[recTrk].file.fileSize() > validBytes && tracks[recTrk].file.seekSet(validBytes)) {
        tracks[recTrk].file.truncate();
      }
      writeWavHeader(tracks[recTrk].file, tracks[recTrk].recordedFrames, recTrk);
      tracks[recTrk].file.sync();
      songDirty = true;
    }
    recording.windowExited = false;
  }

  if (recTrk >= 0 && recTrk < NUM_AUDIO_TRACKS) {
    Track &tr = tracks[recTrk];
    if (recording.writtenFrames > 0) {
      tr.armMode = ArmMode::Off;
    }
    if (tr.recordedFrames == 0) {
      if (tr.file.isOpen()) {
        tr.file.close();
      }
      char wPath[64];
      trackWavByName(activeSongName, recTrk, wPath, sizeof(wPath));
      if (SD.sdfs.exists(wPath)) {
        SD.sdfs.remove(wPath);
      }
    } else if (tr.file.isOpen()) {
      uint64_t validBytes = trackWavByteOffset(recTrk, tr.recordedFrames);
      if (tr.file.fileSize() > validBytes && tr.file.seekSet(validBytes)) {
        tr.file.truncate();
        writeWavHeader(tr.file, tr.recordedFrames, recTrk);
        tr.file.sync();
      }
    }
    if (recording.autoMute) {
      tr.state = (tr.recordedFrames > 0) ? TrackState::Playing : TrackState::Empty;
      recording.autoMute = false;
    }
  }

  recording.src = AudioRecSrc::Off;
  recording.autoMute = false;
  recording.reverse = false;
  recording.mode = Recording::Idle;
  recording.track = -1;
}

static bool inKeepStreaming = false;
static bool smpRecOpen = false;

FLASHMEM void keepStreaming() {
  if (inKeepStreaming) return;
  inKeepStreaming = true;
  midiSeqFastService();
  if (transport.mode == TransportMode::Running) {
    int chunks = (abs((int)transport.speedQuarter) > DEFAULT_SPEED_QUARTER) ? 2 : 1;
    for (int t = 0; t < NUM_AUDIO_TRACKS; t++) {
      if (tracks[t].live) fillTrackRing(t, chunks);
    }
  }
  if (!smpRecOpen) {
    samplerServiceActiveStreams();
  }
  inKeepStreaming = false;
}

// ============================================================================
// Sampler Piano Key Threshold-Gated Audio-In Recording (<Song>/SAMPLES/)
// ============================================================================
static FsFile smpRecFile;
static char smpRecFinalPath[SAMPLER_PATH_MAX] = {};
static char smpRecTempPath[96] = {};
static int smpRecInst = 0;
static int smpRecKey = 0;
static bool smpRecTriggered = false;
static SamplerRecMode smpRecMode = SamplerRecMode::ThreshAutoStartStop;
static int16_t smpRecThreshAmp = 1000;
static uint32_t smpRecReadNow = 0;
static uint32_t smpRecWrittenFrames = 0;
static uint32_t smpRecBelowFrames = 0;
static uint32_t smpRecLastAboveFrame = 0;

FLASHMEM bool storageIsSamplerKeyRecordingOpen() {
  return smpRecOpen;
}

FLASHMEM bool storageIsSamplerKeyRecordTriggered() {
  return smpRecOpen && smpRecTriggered;
}

FLASHMEM SamplerRecMode storageSamplerKeyRecMode() {
  return smpRecMode;
}

FLASHMEM void storageCancelSamplerKeyRecord() {
  noInterrupts();
  samplerRecActive = false;
  interrupts();
  if (smpRecFile.isOpen()) {
    smpRecFile.close();
  }
  if (smpRecTempPath[0] != '\0' && SD.sdfs.exists(smpRecTempPath)) {
    SD.sdfs.remove(smpRecTempPath);
  }
  smpRecOpen = false;
  smpRecTriggered = false;
  smpRecMode = SamplerRecMode::ThreshAutoStartStop;
}

FLASHMEM void storageTriggerSamplerKeyRecordNow() {
  if (!smpRecOpen || smpRecTriggered) return;
  uint32_t curNow = transport.now;
  int32_t avail = (int32_t)(curNow - smpRecReadNow);
  if (avail > 32) {
    smpRecReadNow = curNow - 32u;
  } else if (avail < 0) {
    smpRecReadNow = curNow;
  }
  smpRecTriggered = true;
  smpRecWrittenFrames = 0;
  smpRecBelowFrames = 0;
  smpRecLastAboveFrame = 0;
  noteUserActivity();
  drawSamplerKeyModal(false);
  uiForceRedraw();
}

FLASHMEM static bool finalizeSamplerKeyRecord() {
  if (!smpRecOpen) return false;
  noInterrupts();
  samplerRecActive = false;
  interrupts();

  constexpr uint32_t SMP_TAIL_PAD_FRAMES = 512u;
  uint32_t keepFrames = smpRecWrittenFrames;
  if (smpRecMode == SamplerRecMode::ThreshAutoStartStop && smpRecLastAboveFrame > 0) {
    keepFrames = min(smpRecWrittenFrames, smpRecLastAboveFrame + SMP_TAIL_PAD_FRAMES);
  }
  keepFrames = max(keepFrames, 1u);

  bool ok = false;
  if (smpRecFile.isOpen()) {
    uint64_t validBytes = (uint64_t)WAV_DATA_OFFSET + (uint64_t)keepFrames * sizeof(uint32_t);
    if (smpRecFile.fileSize() > validBytes && smpRecFile.seekSet(validBytes)) {
      smpRecFile.truncate();
    }
    ok = writeWavHeader(smpRecFile, keepFrames, 0);
    smpRecFile.sync();
    smpRecFile.close();
  }

  if (ok) {
    samplerClearKey(smpRecInst, smpRecKey);
    if (SD.sdfs.exists(smpRecFinalPath)) {
      SD.sdfs.remove(smpRecFinalPath);
    }
    ok = SD.sdfs.rename(smpRecTempPath, smpRecFinalPath);
  }
  if (!ok && smpRecTempPath[0] != '\0' && SD.sdfs.exists(smpRecTempPath)) {
    SD.sdfs.remove(smpRecTempPath);
  }
  syncSdCard();

  smpRecOpen = false;
  smpRecTriggered = false;
  smpRecMode = SamplerRecMode::ThreshAutoStartStop;

  if (ok) {
    samplerAssignSample(smpRecInst, smpRecKey, smpRecFinalPath);
    songDirty = true;
  } else {
    uiShowToast("REC WRITE ERR");
  }
  closeSamplerKeyModal();
  noteUserActivity();
  uiForceRedraw();
  return ok;
}

FLASHMEM bool storageFinishSamplerKeyRecordNow() {
  if (!smpRecOpen || !smpRecTriggered) return false;
  uint32_t curNow = transport.now;
  int32_t avail = (int32_t)(curNow - smpRecReadNow);
  if (avail > (int32_t)INPUT_FRAMES) {
    smpRecReadNow = curNow - INPUT_FRAMES;
    avail = (int32_t)INPUT_FRAMES;
  }
  while (avail > 0 && smpRecFile.isOpen()) {
    uint32_t n = min((uint32_t)avail, (uint32_t)CHUNK_FRAMES);
    for (uint32_t k = 0; k < n; k++) {
      chunk[k] = inputRing[(smpRecReadNow + k) % INPUT_FRAMES];
    }
    if (smpRecFile.write(chunk, n * sizeof(uint32_t)) != (size_t)(n * sizeof(uint32_t))) {
      break;
    }
    smpRecReadNow += n;
    smpRecWrittenFrames += n;
    avail -= (int32_t)n;
  }
  if (smpRecWrittenFrames == 0 && smpRecFile.isOpen()) {
    chunk[0] = 0;
    if (smpRecFile.write(chunk, sizeof(uint32_t)) == sizeof(uint32_t)) {
      smpRecWrittenFrames = 1;
    }
  }
  smpRecLastAboveFrame = smpRecWrittenFrames;
  return finalizeSamplerKeyRecord();
}

FLASHMEM bool storageStartSamplerKeyRecord(int instIdx, int keyIdx, SamplerRecMode mode) {
  storageCancelSamplerKeyRecord();
  if (instIdx < 0 || instIdx >= NUM_INSTRUMENTS) return false;
  if (keyIdx < 0 || keyIdx >= SAMPLER_NUM_KEYS) return false;

  if (transport.mode != TransportMode::Stopped) {
    storageStopTransport();
  }
  fmSynthAllNotesOff();
  samplerStopAllStreams();

  const char *songName = (activeSongName[0] != '\0') ? activeSongName : "SONG_001";
  ensureSongsRootDir();
  char sDir[64], smpDir[80];
  songDirByName(songName, sDir, sizeof(sDir));
  if (!SD.sdfs.exists(sDir)) SD.sdfs.mkdir(sDir);
  snprintf(smpDir, sizeof(smpDir), "%s/SAMPLES", sDir);
  if (!SD.sdfs.exists(smpDir)) SD.sdfs.mkdir(smpDir);

  const FmPatch &p = fmSynthPatch(instIdx);
  char instName[16];
  if (p.name[0] != '\0') {
    snprintf(instName, sizeof(instName), "%s", p.name);
  } else {
    snprintf(instName, sizeof(instName), "SMP_%02d", instIdx + 1);
  }
  const char *keyName = samplerKeyNoteName(keyIdx);
  snprintf(smpRecFinalPath, sizeof(smpRecFinalPath), "%s/%s - %s.wav", smpDir, instName, keyName);
  snprintf(smpRecTempPath, sizeof(smpRecTempPath), "%s/_SMPREC.TMP", smpDir);

  smpRecFile = SD.sdfs.open(smpRecTempPath, O_RDWR | O_CREAT | O_TRUNC);
  if (!smpRecFile.isOpen() || !writeWavHeader(smpRecFile, 0, 0)) {
    if (smpRecFile.isOpen()) smpRecFile.close();
    if (SD.sdfs.exists(smpRecTempPath)) SD.sdfs.remove(smpRecTempPath);
    return false;
  }
  smpRecFile.sync();

  float threshLin = powf(10.0f, (float)songSettings().defaultThreshDb / 20.0f);
  smpRecThreshAmp = (int16_t)constrain((int32_t)(threshLin * 32768.0f + 0.5f), 1, 32767);
  smpRecInst = instIdx;
  smpRecKey = keyIdx;
  smpRecTriggered = false;
  smpRecMode = mode;
  smpRecWrittenFrames = 0;
  smpRecBelowFrames = 0;
  smpRecLastAboveFrame = 0;

  noInterrupts();
  samplerRecActive = true;
  smpRecReadNow = transport.now;
  interrupts();
  smpRecOpen = true;
  return true;
}

FLASHMEM static void serviceSamplerKeyRecord() {
  if (!smpRecOpen) return;

  uint32_t curNow = transport.now;
  int32_t avail = (int32_t)(curNow - smpRecReadNow);
  if (avail <= 0) return;
  if (avail > (int32_t)INPUT_FRAMES) {
    smpRecReadNow = curNow - INPUT_FRAMES;
    avail = (int32_t)INPUT_FRAMES;
  }

  if (!smpRecTriggered) {
    if (smpRecMode == SamplerRecMode::ManualStartStop) {
      if (avail > 32) {
        smpRecReadNow = curNow - 32u;
      }
      return;
    }
    for (int32_t i = 0; i < avail; i++) {
      uint32_t v = inputRing[(smpRecReadNow + (uint32_t)i) % INPUT_FRAMES];
      int16_t sL = (int16_t)(v & 0xFFFF);
      int16_t sR = (int16_t)(v >> 16);
      int32_t pk = max(abs((int32_t)sL), abs((int32_t)sR));
      if (pk >= (int32_t)smpRecThreshAmp) {
        uint32_t preRoll = min((uint32_t)i, 32u);
        smpRecReadNow += ((uint32_t)i - preRoll);
        avail = (int32_t)(curNow - smpRecReadNow);
        smpRecTriggered = true;
        smpRecWrittenFrames = 0;
        smpRecBelowFrames = 0;
        smpRecLastAboveFrame = 0;
        noteUserActivity();
        drawSamplerKeyModal(false);
        uiForceRedraw();
        break;
      }
    }
    if (!smpRecTriggered) {
      if (avail > 32) {
        smpRecReadNow = curNow - 32u;
      }
      return;
    }
  }

  constexpr uint32_t SMP_MIN_REC_FRAMES = SAMPLE_RATE_HZ / 20u;        // 50 ms minimum recording
  constexpr uint32_t SMP_BELOW_HOLD_FRAMES = (SAMPLE_RATE_HZ * 3u) / 20u; // 150 ms release hold below threshold

  while (avail >= 1024) {
    uint32_t n = min((uint32_t)avail, (uint32_t)CHUNK_FRAMES);
    bool stopHere = false;
    for (uint32_t k = 0; k < n; k++) {
      uint32_t v = inputRing[(smpRecReadNow + k) % INPUT_FRAMES];
      chunk[k] = v;
      int16_t sL = (int16_t)(v & 0xFFFF);
      int16_t sR = (int16_t)(v >> 16);
      int32_t pk = max(abs((int32_t)sL), abs((int32_t)sR));
      uint32_t frameIdx = smpRecWrittenFrames + k + 1u;
      if (pk >= (int32_t)smpRecThreshAmp) {
        smpRecBelowFrames = 0;
        smpRecLastAboveFrame = frameIdx;
      } else if (smpRecMode == SamplerRecMode::ThreshAutoStartStop) {
        smpRecBelowFrames++;
        if (smpRecBelowFrames >= SMP_BELOW_HOLD_FRAMES && frameIdx >= SMP_MIN_REC_FRAMES) {
          n = k + 1u;
          stopHere = true;
          break;
        }
      }
    }

    if (!smpRecFile.isOpen() ||
        smpRecFile.write(chunk, n * sizeof(uint32_t)) != (size_t)(n * sizeof(uint32_t))) {
      storageCancelSamplerKeyRecord();
      closeSamplerKeyModal();
      uiShowToast("REC WRITE ERR");
      uiForceRedraw();
      return;
    }

    smpRecReadNow += n;
    smpRecWrittenFrames += n;
    avail = (int32_t)(transport.now - smpRecReadNow);

    if (stopHere || smpRecWrittenFrames >= MAX_REC_FRAMES) {
      finalizeSamplerKeyRecord();
      return;
    }
  }
}

FLASHMEM void serviceStorage() {
  if (smpRecOpen) {
    serviceSamplerKeyRecord();
    return;
  }

  // 1. Handle entry into active audio recording window (start or punch-in)
  if (recording.windowEntered) {
    noInterrupts();
    recording.windowEntered = false;
    uint32_t sNow = recording.startNow;
    uint32_t sTape = recording.startTapeFrame;
    interrupts();
    recording.readNow = sNow;
    recording.writeTapeFrame = sTape;
    recording.lastOverviewCol = -1;
  }

  // 2. Top up all playing audio track and sampler ring buffers
  samplerServiceStreams();
  if (transport.mode == TransportMode::Running) {
    int chunks = (abs((int)transport.speedQuarter) > DEFAULT_SPEED_QUARTER) ? 2 : 1;
    for (int t = 0; t < NUM_AUDIO_TRACKS; t++) {
      if (tracks[t].live) {
        fillTrackRing(t, chunks);
      }
    }
  }

  // 2b. If armed and approaching punch window, incrementally pre-pad WAV file with silence
  if (transport.mode == TransportMode::Running && recording.mode == Recording::Armed &&
      !recording.activeInWindow && transport.punchEnabled && recording.track >= 0) {
    int recTrk = recording.track;
    Track &tr = tracks[recTrk];
    uint32_t targetFrame = (transport.speedQuarter < 0) ? transport.punchOutFrame : transport.punchInFrame;
    uint64_t targetPos = trackWavByteOffset(recTrk, targetFrame);
    if (tr.file.isOpen() && tr.file.fileSize() < targetPos) {
      uint64_t curSz = tr.file.fileSize();
      uint64_t stepPos = min(targetPos, curSz + (uint64_t)sizeof(chunk));
      padTrackFileToBytePos(tr.file, stepPos, false, recTrk);
    }
  }

  // 3. Write captured audio chunks to SD card
  if (recording.mode == Recording::Active || recording.mode == Recording::Finishing || recording.windowExited) {
    bool exited = recording.windowExited && !recording.activeInWindow;
    drainRecordingBuffer(exited);
    if (exited && (int32_t)(recording.endNow - recording.readNow) <= 0) {
      recording.windowExited = false;
      int recTrk = recording.track;
      if (recTrk >= 0 && tracks[recTrk].file.isOpen()) {
        uint64_t validBytes = trackWavByteOffset(recTrk, tracks[recTrk].recordedFrames);
        if (tracks[recTrk].file.fileSize() > validBytes && tracks[recTrk].file.seekSet(validBytes)) {
          tracks[recTrk].file.truncate();
        }
        writeWavHeader(tracks[recTrk].file, tracks[recTrk].recordedFrames, recTrk);
        tracks[recTrk].file.sync();
      }
      if (recTrk >= 0 && recTrk < NUM_AUDIO_TRACKS) {
        tracks[recTrk].armMode = ArmMode::Off;
        if (recording.autoMute) {
          tracks[recTrk].state = (tracks[recTrk].recordedFrames > 0) ? TrackState::Playing : TrackState::Empty;
          recording.autoMute = false;
        }
        if (transport.mode == TransportMode::Running && transport.loopEnabled &&
            (transport.punchOutFrame - transport.punchInFrame) <= (RING_FRAMES + CHUNK_FRAMES)) {
          Track &tr = tracks[recTrk];
          noInterrupts();
          tr.live = false;
          tr.head = 0;
          tr.tail = 0;
          uint32_t curTf = transport.tapeFrame;
          interrupts();
          tr.fileFrame = curTf;
          fillTrackRing(recTrk, 2);
          noInterrupts();
          tr.live = true;
          interrupts();
        }
      }
      recording.src = AudioRecSrc::Off;
      recording.autoMute = false;
      recording.mode = Recording::Idle;
      recording.track = -1;
      songDirty = true;
      uiForceRedraw();
    }
  }
}

FLASHMEM static void drawMasterExportOverlay() {
  using namespace m8;
  const int ovX = SX(32), ovW = SX(256);
  const int r0Y = SY(80), rH = SY(24), r1Y = r0Y + rH;
  {
    Surface s(ovW, rH);
    s.fillRect(0, 0, ovW, rH, DARK);
    s.fillRect(0, 0, ovW, PX, ACCENT);
    s.fillRect(0, 0, PX, rH, ACCENT);
    s.fillRect(ovW - PX, 0, PX, rH, ACCENT);
    s.fillRect(PX, rH - 1, ovW - 2 * PX, 1, MID);
    const char *t1 = "EXPORTING MASTER AUDIO";
    s.drawText(max(SX(6), (ovW - textWidth(t1)) / 2), TY(rH), t1, ACCENT);
    s.blit(ovX, r0Y);
  }
  {
    Surface s(ovW, rH);
    s.fillRect(0, 0, ovW, rH, PANEL);
    s.fillRect(0, 0, PX, rH, ACCENT);
    s.fillRect(ovW - PX, 0, PX, rH, ACCENT);
    s.fillRect(0, rH - PX, ovW, PX, ACCENT);
    const char *t2 = "PRESS ANY KEY TO CANCEL";
    s.drawText(max(SX(6), (ovW - textWidth(t2)) / 2), TY(rH) - (PX / 2), t2, INK);
    s.blit(ovX, r1Y);
  }
}

FLASHMEM uint32_t storageSongMaxAudibleFrame() {
  uint32_t maxDataFrame = 0;
  const bool anySolo = anyTrackSolo();
  for (int t = 0; t < NUM_AUDIO_TRACKS; t++) {
    bool audible = (tracks[t].state == TrackState::Playing) && (!anySolo || tracks[t].solo);
    if (audible && tracks[t].recordedFrames > maxDataFrame) {
      maxDataFrame = tracks[t].recordedFrames;
    }
  }
  for (int t = 0; t < NUM_MIDI_TRACKS; t++) {
    bool audible = !midiTracks[t].muted && (!anySolo || midiTracks[t].solo);
    if (!audible) continue;
    uint32_t cnt = 0;
    const MidiEvent *evs = midiSeqEvents(t, cnt);
    if (cnt > 0 && evs) {
      uint32_t endF = midiTicksToFrames(evs[cnt - 1].tick, bpm);
      if (endF > maxDataFrame) maxDataFrame = endF;
    }
  }
  return min(maxDataFrame, (uint32_t)MAX_REC_FRAMES);
}

FLASHMEM MasterExportResult storageExportMasterAudioWav() {
  if (transport.mode != TransportMode::Stopped) {
    storageStopTransport();
  }

  uint32_t maxDataFrame = storageSongMaxAudibleFrame();
  if (maxDataFrame == 0) {
    return MasterExportResult::Empty;
  }

  const char *songName = (activeSongName[0] != '\0') ? activeSongName : "SONG_001";
  char dirPath[64], wavPath[96];
  songDirByName(songName, dirPath, sizeof(dirPath));
  if (!SD.sdfs.exists(ROOT_DIR)) SD.sdfs.mkdir(ROOT_DIR);
  if (!SD.sdfs.exists(SONGS_DIR)) SD.sdfs.mkdir(SONGS_DIR);
  if (!SD.sdfs.exists(dirPath)) SD.sdfs.mkdir(dirPath);
  snprintf(wavPath, sizeof(wavPath), "%s/%s.wav", dirPath, songName);

  FsFile outFile = SD.sdfs.open(wavPath, O_RDWR | O_CREAT | O_TRUNC);
  if (!outFile.isOpen() || !writeWavHeader(outFile, 0, 0)) {
    if (outFile.isOpen()) outFile.close();
    return MasterExportResult::Error;
  }

  drawMasterExportOverlay();

  // Save pre-export transport & arm state
  const uint32_t prevTapeFrame = transport.tapeFrame;
  const int8_t prevSpeedQuarter = transport.speedQuarter;
  const bool prevLoop = transport.loopEnabled;
  const bool prevPunch = transport.punchEnabled;
  ArmMode prevAudioArm[NUM_AUDIO_TRACKS];
  ArmMode prevMidiArm[NUM_MIDI_TRACKS];
  for (int t = 0; t < NUM_AUDIO_TRACKS; t++) {
    prevAudioArm[t] = tracks[t].armMode;
    tracks[t].armMode = ArmMode::Off;
  }
  for (int t = 0; t < NUM_MIDI_TRACKS; t++) {
    prevMidiArm[t] = midiTracks[t].armMode;
    midiTracks[t].armMode = ArmMode::Off;
  }

  fmSynthSetExportTailPhase(false);
  fmSynthAllNotesOff();
  resetMasterFxState();

  transport.speedQuarter = DEFAULT_SPEED_QUARTER;
  transport.loopEnabled = false;
  transport.punchEnabled = false;
  metronomeOn = false;
  storageSeekTape(0);

  noInterrupts();
  metronomeOn = false;
  masterExportActive = true;
  interrupts();
  storageStartTransport(false, false, false);
  noInterrupts();
  metronomeOn = false;
  uint32_t exportReadNow = transport.now;
  interrupts();

  uint8_t prevKeys = board.keys();
  uint32_t writtenFrames = 0;
  uint32_t silentRunFrames = 0;
  bool tailPhaseEntered = false;
  bool cancelled = false;
  bool writeErr = false;

  for (;;) {
    if (usbDriveTick) usbDriveTick();

    uint8_t curKeys = board.keys();
    if ((curKeys & ~prevKeys) != 0) {
      cancelled = true;
      break;
    }
    prevKeys = curKeys;

    keepStreaming();
    midiSeqService(bpm, -1, false);

    if (!tailPhaseEntered && transport.tapeFrame >= maxDataFrame) {
      tailPhaseEntered = true;
      fmSynthSetExportTailPhase(true);
    }

    uint32_t curNow = transport.now;
    int32_t avail = (int32_t)(curNow - exportReadNow);
    if (avail > (int32_t)INPUT_FRAMES) {
      uint32_t drop = (uint32_t)avail - INPUT_FRAMES;
      exportReadNow += drop;
      avail = (int32_t)INPUT_FRAMES;
    }

    bool flushingTail = tailPhaseEntered && (writtenFrames >= maxDataFrame);
    uint32_t minBatch = flushingTail ? 512u : 1024u;
    if ((uint32_t)avail >= minBatch || (transport.mode != TransportMode::Running && avail > 0)) {
      uint32_t n = min((uint32_t)avail, (uint32_t)CHUNK_FRAMES);
      if (!flushingTail && n > 2048u) n = 2048u;
      for (uint32_t i = 0; i < n; i++) {
        chunk[i] = inputRing[(exportReadNow + i) % INPUT_FRAMES];
      }
      exportReadNow += n;

      const SongSettings &ps = songSettings();
      const bool hasDelayOrVerb = (ps.masterDelay > 0 || ps.masterReverb > 0 || fmSynthHasActiveOutput());
      const uint32_t reqSilent = hasDelayOrVerb ? 16384u : 2048u;
      uint32_t framesToKeep = n;
      bool doneNow = false;

      for (uint32_t i = 0; i < n; i++) {
        uint32_t frameIdx = writtenFrames + i;
        if (frameIdx >= maxDataFrame) {
          uint32_t packed = chunk[i];
          int16_t sL = (int16_t)(packed & 0xFFFF);
          int16_t sR = (int16_t)(packed >> 16);
          if (abs((int)sL) <= 4 && abs((int)sR) <= 4) {
            silentRunFrames++;
            if (silentRunFrames >= reqSilent && !fmSynthHasActiveOutput()) {
              framesToKeep = i + 1;
              doneNow = true;
              break;
            }
          } else {
            silentRunFrames = 0;
          }
        }
      }

      if (outFile.write(chunk, framesToKeep * sizeof(uint32_t)) != (size_t)(framesToKeep * sizeof(uint32_t))) {
        writeErr = true;
        break;
      }
      writtenFrames += framesToKeep;

      if (doneNow || writtenFrames >= MAX_REC_FRAMES ||
          (writtenFrames > maxDataFrame && (writtenFrames - maxDataFrame) >= 15u * SAMPLE_RATE_HZ) ||
          transport.mode != TransportMode::Running) {
        break;
      }
    } else if (transport.mode != TransportMode::Running) {
      break;
    }
  }

  noInterrupts();
  masterExportActive = false;
  interrupts();
  fmSynthSetExportTailPhase(false);
  storageStopTransport();
  fmSynthAllNotesOff();
  resetMasterFxState();

  for (int t = 0; t < NUM_AUDIO_TRACKS; t++) {
    tracks[t].armMode = prevAudioArm[t];
  }
  for (int t = 0; t < NUM_MIDI_TRACKS; t++) {
    midiTracks[t].armMode = prevMidiArm[t];
  }
  transport.loopEnabled = prevLoop;
  transport.punchEnabled = prevPunch;
  transport.speedQuarter = prevSpeedQuarter;
  storageSeekTape(prevTapeFrame);
  updateMetronomeState();

  if (cancelled || writeErr) {
    outFile.close();
    if (SD.sdfs.exists(wavPath)) {
      SD.sdfs.remove(wavPath);
    }
  } else {
    outFile.truncate();
    writeWavHeader(outFile, writtenFrames, 0);
    outFile.sync();
    outFile.close();
  }
  syncSdCard();

  noteUserActivity();
  inputSwallowModalKeys();
  inputClearRepeat();
  board.tft().fillRect(SX(32), SY(80), SX(256), SY(48), BG);
  invalidateAllPagesCache();
  uiForceRedraw();

  if (cancelled) return MasterExportResult::Cancelled;
  if (writeErr) return MasterExportResult::Error;
  return MasterExportResult::Ok;
}

static bool lastSessionWasSaved = true;

FLASHMEM void flushHeaders() {
  // Manual save only: stop transport and close open files cleanly without overwriting SONG.DAT / MIDI.DAT
  storageCancelSamplerKeyRecord();
  storageStopTransport();
  trackEditReset();
  samplerStopAllStreams();
  lastSessionWasSaved = songSaved;
  discardUnsavedSessionChanges(nullptr);
  saveSystemSettings();
  syncSdCard();
}

FLASHMEM bool storageBegin(void (*progress)(const char *)) {
  if (progress) progress("Mounting SD card...");
  if (!SD.sdfs.begin(SdioConfig(DMA_SDIO)) || !cardResponds()) {
    return false;
  }
  ensureSongsRootDir();
  if (!SD.sdfs.exists(INST_PRESETS_DIR)) {
    SD.sdfs.mkdir(INST_PRESETS_DIR);
  }
  midiSeqInit();
  midiMapInit();
  fmSynthInit();
  songSaved = true; // allow boot scan to clean up any orphaned unsaved folders
  loadSystemSettingsFromDisk();
  scanSongFolders();
  scanProjectFiles();

  if (progress) progress("Loading song...");
  if (lastSessionWasSaved && activeSongName[0] != '\0' && findSongIndex(activeSongName) >= 0) {
    loadSongByName(activeSongName);
  } else if (lastSessionWasSaved && lastActiveSongName[0] != '\0' && findSongIndex(lastActiveSongName) >= 0) {
    loadSongByName(lastActiveSongName);
  } else if (lastSessionWasSaved && numSavedSongs > 0) {
    loadSong(0);
  } else {
    createNewSong();
  }
  return true;
}

}  // namespace looper
