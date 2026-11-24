// inst_sf2.cpp — SoundFont 2.x (.sf2) Internal Instrument Engine
// Hybrid RAM1 Attack/Micro-Loop Pre-Roll Cache + Shared Ring-Locked SDIO Streamer
#include "inst_sf2.h"
#include "inst_sampler.h"
#include "storage.h"
#include "paged_dir_scanner.h"
#include "ui.h"
#include <Arduino.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>

namespace looper {

static constexpr size_t SOUNDFONTS_DIR_PREFIX_LEN = 20; // strlen("/Capstan/SOUNDFONTS/")
static constexpr const char *CLIP_SF2_PATH = "/Capstan/_CLIPSF2.TMP";

// ============================================================================
// Memory Budgets:
//   - RAM1 (DTCM, uncached): 32 pre-roll slots x (640 + 2 guard) int16_t = 41,088 B + 32 voice states
//   - RAM2 (DMAMEM, cached): 254 flattened Sf2Zones x 48 B = 12,192 B
//   - Shared Stream Rings:   Reuses inst_sampler.cpp's 8-slot SamplerStream pool (0 B new)
//   - Shared Browser Pool:   Reuses instruments.cpp's 2,048 B sharedBrowserPool (0 B new)
// ============================================================================
static constexpr int SF2_PREROLL_SLOTS        = 32;
static constexpr uint32_t SF2_PREROLL_LEN     = 640;
static constexpr uint32_t SF2_PREROLL_BUF_LEN = SF2_PREROLL_LEN + 2; // +2 guard samples for Hermite
static constexpr int SF2_MAX_ZONES            = 254; // 0..253 (0xFF reserved as invalid sentinel)
static constexpr int SF2_MAX_ZONES_PER_INST   = 254;

static constexpr uint8_t SF2_FLAG_MODE_MASK = 0x03u; // 0=NoLoop, 1=ContLoop, 3=LoopUntilRel
static constexpr uint8_t SF2_FLAG_STEREO    = 0x04u; // Paired L/R stereo sample zone
static constexpr uint8_t SF2_FLAG_RATE_X4   = 0x08u; // origRateHz stored divided by 4 (> 65535 Hz)
static constexpr uint8_t SF2_FLAG_LFO_SHIFT = 4u;    // Bits [4..7] = LFO frequency index 0..15

struct Sf2Zone {
  uint32_t sampleStart;       // Frame index in smpl chunk (Mono or Left)
  uint32_t totalFrames;       // Sample length in frames (dwEnd - dwStart)
  uint32_t loopStart;         // Loop start offset relative to sampleStart (0..totalFrames)
  uint32_t loopEnd;           // Loop end offset relative to sampleStart (0..totalFrames)
  uint32_t rightStart;        // Frame index in smpl chunk for Right channel when SF2_FLAG_STEREO
  uint16_t origRateHz;        // Original sample rate in Hz (or Hz >> 2 when SF2_FLAG_RATE_X4)
  uint8_t  keyLo;             // 0..127
  uint8_t  keyHi;             // 0..127
  uint8_t  velLo;             // 0..127
  uint8_t  velHi;             // 0..127
  uint8_t  rootKey;           // 0..127 MIDI root key
  int8_t   coarseTune;        // semitones (-64..+64)
  int8_t   fineTuneCents;     // cents (-100..+100, includes shdr.chPitchCorrection)
  uint8_t  scaleTuning;       // 0..120 (100 = 12-TET, 0 = unpitched drum/FX)
  int8_t   pan;               // -100..+100
  uint8_t  gainQ7;            // 1..180 linear gain from initialAttenuation (128 = 1.0f)
  uint8_t  flags;             // Low 4b = SF2_FLAG_*, High 4b = LFO freq nibble (0..15)
  uint8_t  exclClass;         // 0 = none, 1..127 = drum choke / mute group
  uint8_t  preRollSlot;       // 0..SF2_PREROLL_SLOTS-1, or 0xFF if uncached
  uint8_t  atkParam127;       // Pre-mapped 0..127 SF2 volEnv attack
  uint8_t  decParam127;       // Pre-mapped 0..127 SF2 volEnv decay timecents index
  uint8_t  susParam127;       // Pre-mapped 0..127 SF2 volEnv sustain level
  uint8_t  relParam127;       // Pre-mapped 0..127 SF2 volEnv release timecents index
  uint8_t  fltCutoff127;      // Pre-mapped 0..127 SF2 initialFilterFc in linear cents (1500..13500)
  uint8_t  holdParam127;      // 0 = none, 1..127 = SF2 volEnv hold timecents index
  uint8_t  dlyVolAndLfoNib;   // High 4b = delayVolEnv (0..15), Low 4b = delayVibLFO (0..15)
  uint8_t  keyHoldDecNib;     // High 4b = keynumToVolEnvHold, Low 4b = keynumToVolEnvDecay (-3..+12 * 10 tc/key)
  uint8_t  fltRes127;         // 0..127 resonance mapped from initialFilterQ (0..960 cB)
  int8_t   modEnvToFltSemi;   // -120..+120 semitones filter cutoff sweep from modEnvToFilterFc
  uint8_t  modAtkDecNib;      // High 4b = modEnv attack (0..15), Low 4b = modEnv hold+decay (0..15)
  uint8_t  modSusRelNib;      // High 4b = modEnv sustain (0..15), Low 4b = modEnv release (0..15)
  int8_t   lfoPitchCentsDiv2; // -125..+125 (* 2 = -250..+250 cents vibrato / modLfo pitch depth)
};
static_assert(sizeof(Sf2Zone) == 48, "Sf2Zone must be 48 bytes");

static inline uint32_t zoneSampleRateHz(const Sf2Zone &z) {
  return ((z.flags & SF2_FLAG_RATE_X4) != 0) ? ((uint32_t)z.origRateHz << 2) : (uint32_t)z.origRateHz;
}

struct Sf2HydraIndex {
  uint32_t smplOffset = 0;
  uint32_t smplFrames = 0;
  uint32_t phdrOffset = 0;
  uint16_t phdrCount  = 0; // Excludes terminal EOP record
  uint32_t pbagOffset = 0;
  uint16_t pbagCount  = 0; // Excludes terminal record
  uint32_t pmodOffset = 0;
  uint32_t pmodCount  = 0;
  uint32_t pgenOffset = 0;
  uint32_t pgenCount  = 0;
  uint32_t instOffset = 0;
  uint16_t instCount  = 0; // Excludes terminal EOI record
  uint32_t ibagOffset = 0;
  uint16_t ibagCount  = 0; // Excludes terminal record
  uint32_t imodOffset = 0;
  uint32_t imodCount  = 0;
  uint32_t igenOffset = 0;
  uint32_t igenCount  = 0;
  uint32_t shdrOffset = 0;
  uint16_t shdrCount  = 0; // Excludes terminal EOS record
};

static constexpr int SF2_MAX_MOD_POOL           = 128; // 0..127 (0xFF reserved as invalid sentinel)
static constexpr int SF2_MAX_MOD_RULES_PER_INST = 32;

struct Sf2ZoneAux {
  int16_t modEnvToPitchCents; // -12000..+12000 cents static modEnvToPitch (gen 7)
  int8_t  modLfoToFltSemi;    // -120..+120 semitones static modLfoToFilterFc (gen 10)
  int8_t  modLfoToVolCbDiv2;  // -120..+120 (* 2 = -240..+240 cB) static modLfoToVolume (gen 13)
};
static_assert(sizeof(Sf2ZoneAux) == 4, "Sf2ZoneAux must be 4 bytes");

static constexpr uint8_t SF2_DEF_OVR_VEL_ATTEN    = 0x01u;
static constexpr uint8_t SF2_DEF_OVR_VEL_FLT      = 0x02u;
static constexpr uint8_t SF2_DEF_OVR_MODWHEEL_VIB = 0x04u;
static constexpr uint8_t SF2_DEF_OVR_PITCHWHEEL   = 0x08u;

static constexpr uint8_t SF2_NONCC_PITCHWHEEL     = 0x02u;

static constexpr uint8_t SF2_MOD_FLAG_PRESET      = 0x40u;
static constexpr uint8_t SF2_MOD_FLAG_ABS_TRANS   = 0x80u;
static constexpr uint8_t SF2_MOD_DEST_MASK        = 0x3Fu; // SF2 generators 0..58

struct Sf2ModRule {
  uint16_t srcOper;      // SF2 sfModSrcOper
  uint16_t amtSrcOper;   // SF2 sfModAmtSrcOper
  int16_t  amount;       // SF2 modAmount
  uint8_t  destAndFlags; // Low 6b = destGen (0..58), bit 6 = isPresetMod, bit 7 = absTransform
  uint8_t  modGroup;     // 0 = all zones in slot, 1..254 = specific layer/zone group
};
static_assert(sizeof(Sf2ModRule) == 8, "Sf2ModRule must be 8 bytes");

struct Sf2SlotMeta {
  bool valid = false;
  uint8_t maxKeySpan = 127;
  uint16_t fileTag = 0;
  uint16_t curBank = 0;
  uint16_t curPresetNum = 0;
  uint16_t curPresetOrderIdx = 0;
  uint16_t totalPresets = 0;
  uint16_t zoneCount = 0;
  uint8_t modRuleCount = 0;
  uint8_t defOverrideMask = 0;
  uint8_t nonCcUsedMask = 0;
  uint8_t presetModCount = 0;
  uint8_t instModCount = 0;
  uint8_t presetModCcs[SF2_MAX_UI_MOD_CCS] = {};
  uint8_t presetModTags[SF2_MAX_UI_MOD_CCS] = {};
  uint8_t instModCcs[SF2_MAX_UI_MOD_CCS] = {};
  uint8_t instModTags[SF2_MAX_UI_MOD_CCS] = {};
  char presetName[21] = "---";
  Sf2HydraIndex hydra;
};

struct Sf2PreRollMeta {
  uint32_t sampleStart = 0;
  uint32_t rightStart = 0;
  uint32_t loopStart = 0;
  uint32_t loopEnd = 0;
  uint16_t fileTag = 0;
  uint8_t isStereo = 0;
  uint8_t wrapsInPre = 0;
  uint8_t refCount = 0;
};

struct Sf2VoiceRuntime {
  uint32_t voiceAge = 0;
  uint32_t baseStepFp = 65536u;
  uint32_t totalFrames = 0;
  uint32_t preRollPos = 0;
  uint32_t fracFp = 0;
  uint32_t envDelayRem = 0;
  uint32_t envHoldRem = 0;
  uint32_t lfoPhase = 0;
  uint32_t lfoStepPerBlock = 0;
  float ampAtkInc = 0.01f;
  float ampDecMul = 0.999f;
  float ampSusLvl = 1.0f;
  float ampRelMul = 0.999f;
  float modAtkInc16 = 0.1f;
  float modDecMul16 = 0.99f;
  float modSusLvl = 0.0f;
  float modRelMul16 = 0.99f;
  float modEnv = 0.0f;
  float zoneGain = 1.0f;
  float liveGainMul = 1.0f;
  float livePitchCents = 0.0f;
  float baseFltCents = 13500.0f;
  float modFltCents = 0.0f;
  float fltFbG = 0.0f;
  float fltQGain = 1.0f;
  float lfoEnv = 0.0f;
  uint16_t lfoDelayBlocks = 0;
  int16_t lfoPitchCents = 0;
  int16_t liveVibPitchCents = 0;
  int16_t liveFltCents = 0;
  int16_t lfoToFltCents = 0;
  int16_t curLfoFltCents = 0;
  int16_t lfoToVolCb = 0;
  int16_t modPitchCents = 0;
  int16_t prevL = 0;
  int16_t prevR = 0;
  int16_t lastL = 0;
  int16_t lastR = 0;
  int16_t prevOutL = 0;
  int16_t prevOutR = 0;
  int16_t declickL = 0;
  int16_t declickR = 0;
  uint8_t declickRem = 0;
  uint8_t zonePoolIdx = 0xFFu;
  uint8_t exclClass = 0;
  uint8_t modGroup = 0;
  int8_t zonePan = 0;
  int8_t livePanOff = 0;
  EnvStage modStage = EnvStage::Idle;
  volatile bool inPreRoll = false;
  volatile bool wantStream = false;
  volatile bool streamReady = false;
  bool preRollHold = false;
  bool choked = false;
};

// Uncached RAM1 Attack/Micro-Loop Pre-Roll buffer & hot ISR Voice Runtime State (zero-wait-state DTCM)
alignas(4) int16_t sf2PreRoll[SF2_PREROLL_SLOTS][SF2_PREROLL_BUF_LEN];
static Sf2VoiceRuntime sf2Voices[NUM_FM_VOICES];
static Sf2PreRollMeta sf2PreRollInfo[SF2_PREROLL_SLOTS];

// Cached RAM2 Flattened Zone/Modulator Pools & Path Tables (never passed directly to SdFat DMA)
DMAMEM Sf2Zone sf2ZonePool[SF2_MAX_ZONES];
DMAMEM static uint8_t sf2InstZoneIds[NUM_INSTRUMENTS][SF2_MAX_ZONES_PER_INST];
DMAMEM static uint8_t sf2ZoneModGroup[SF2_MAX_ZONES];
DMAMEM static Sf2ModRule sf2ModPool[SF2_MAX_MOD_POOL];
DMAMEM static uint8_t sf2InstModIds[NUM_INSTRUMENTS][SF2_MAX_MOD_RULES_PER_INST];
DMAMEM static char sf2SlotFile[NUM_INSTRUMENTS][SAMPLER_FILE_MAX];

static char sf2SlotDir[NUM_INSTRUMENTS][SAMPLER_DIR_MAX];
static Sf2SlotMeta sf2Slots[NUM_INSTRUMENTS];
static Sf2ZoneAux sf2ZoneAux[SF2_MAX_ZONES];
static uint8_t sf2CcVal[NUM_INSTRUMENTS][128];
static uint32_t sf2UsedCcMask[NUM_INSTRUMENTS][4] = {};
static uint16_t sf2PitchWheel14[NUM_INSTRUMENTS] = {};
static uint32_t freeZoneBits[(SF2_MAX_ZONES + 31) / 32] = {};
static uint32_t freeModBits[(SF2_MAX_MOD_POOL + 31) / 32] = {};
static uint64_t freePreRollBits = 0;
static uint16_t nextSf2FileTag = 1;
static int8_t clipboardSf2SrcSlot = -1; // -1 = none, 0..7 = live slot, -2 = spilled to CLIP_SF2_PATH
static uint16_t clipboardSf2Bank = 0;
static uint16_t clipboardSf2Preset = 0;
static uint8_t clipboardSf2CcCount = 0;
static uint8_t clipboardSf2CcPairs[64] = {};
static volatile int8_t pendingCcPreset[NUM_INSTRUMENTS] = { -1, -1, -1, -1, -1, -1, -1, -1 };

// Fast O(1) ISR/Note-On envelope, hold, delay, LFO, and SVF cutoff LUTs in Flash (PROGMEM)
PROGMEM static const float sf2AtkIncLut[128] = {
  2.83446712e-02f, 2.04294933e-02f, 1.11166250e-02f, 6.31713696e-03f, 3.93729558e-03f, 2.65251286e-03f,
  1.89624420e-03f, 1.41833277e-03f, 1.09879755e-03f, 8.75307204e-04f, 7.13183338e-04f, 5.91992834e-04f,
  4.99103372e-04f, 4.26382147e-04f, 3.68409255e-04f, 3.21463921e-04f, 2.82925206e-04f, 2.50904841e-04f,
  2.24015132e-04f, 2.01218125e-04f, 1.81725139e-04f, 1.64928365e-04f, 1.50353363e-04f, 1.37625480e-04f,
  1.26445720e-04f, 1.16573146e-04f, 1.07811878e-04f, 1.00001357e-04f, 9.30089753e-05f, 8.67244440e-05f,
  8.10554326e-05f, 7.59241763e-05f, 7.12648042e-05f, 6.70212235e-05f, 6.31454291e-05f, 5.95961444e-05f,
  5.63377215e-05f, 5.33392449e-05f, 5.05737978e-05f, 4.80178574e-05f, 4.56507941e-05f, 4.34544554e-05f,
  4.14128169e-05f, 3.95116906e-05f, 3.77384777e-05f, 3.60819597e-05f, 3.45321212e-05f, 3.30799984e-05f,
  3.17175491e-05f, 3.04375414e-05f, 2.92334579e-05f, 2.80994124e-05f, 2.70300783e-05f, 2.60206255e-05f,
  2.50666664e-05f, 2.41642078e-05f, 2.33096092e-05f, 2.24995461e-05f, 2.17309777e-05f, 2.10011183e-05f,
  2.03074120e-05f, 1.96475105e-05f, 1.90192529e-05f, 1.84206483e-05f, 1.78498599e-05f, 1.73051910e-05f,
  1.67850722e-05f, 1.62880502e-05f, 1.58127777e-05f, 1.53580042e-05f, 1.49225677e-05f, 1.45053875e-05f,
  1.41054573e-05f, 1.37218392e-05f, 1.33536580e-05f, 1.30000967e-05f, 1.26603914e-05f, 1.23338275e-05f,
  1.20197361e-05f, 1.17174899e-05f, 1.14265008e-05f, 1.11462169e-05f, 1.08761193e-05f, 1.06157204e-05f,
  1.03645615e-05f, 1.01222106e-05f, 9.88826063e-06f, 9.66232775e-06f, 9.44404986e-06f, 9.23308505e-06f,
  9.02911026e-06f, 8.83182011e-06f, 8.64092569e-06f, 8.45615354e-06f, 8.27724465e-06f, 8.10395357e-06f,
  7.93604757e-06f, 7.77330582e-06f, 7.61551871e-06f, 7.46248716e-06f, 7.31402199e-06f, 7.16994331e-06f,
  7.03008004e-06f, 6.89426933e-06f, 6.76235612e-06f, 6.63419270e-06f, 6.50963831e-06f, 6.38855870e-06f,
  6.27082582e-06f, 6.15631748e-06f, 6.04491698e-06f, 5.93651289e-06f, 5.83099869e-06f, 5.72827260e-06f,
  5.62823722e-06f, 5.53079943e-06f, 5.43587006e-06f, 5.34336374e-06f, 5.25319872e-06f, 5.16529665e-06f,
  5.07958244e-06f, 4.99598408e-06f, 4.91443250e-06f, 4.83486144e-06f, 4.75720728e-06f, 4.68140895e-06f,
  4.60740777e-06f, 4.53514739e-06f
};

PROGMEM static const float sf2TcDecMulLut[128] = {
  9.39267114e-01f, 9.39267114e-01f, 9.39267114e-01f, 9.39267114e-01f, 9.39267114e-01f, 9.39267114e-01f,
  9.39267114e-01f, 9.39267114e-01f, 9.39267114e-01f, 9.39267114e-01f, 9.39267114e-01f, 9.41018971e-01f,
  9.44981413e-01f, 9.48684963e-01f, 9.52145567e-01f, 9.55378317e-01f, 9.58397481e-01f, 9.61216534e-01f,
  9.63848185e-01f, 9.66304410e-01f, 9.68596485e-01f, 9.70735018e-01f, 9.72729977e-01f, 9.74590728e-01f,
  9.76326063e-01f, 9.77944228e-01f, 9.79452955e-01f, 9.80859489e-01f, 9.82170620e-01f, 9.83392699e-01f,
  9.84531674e-01f, 9.85593109e-01f, 9.86582206e-01f, 9.87503827e-01f, 9.88362520e-01f, 9.89162531e-01f,
  9.89907827e-01f, 9.90602112e-01f, 9.91248846e-01f, 9.91851256e-01f, 9.92412357e-01f, 9.92934960e-01f,
  9.93421688e-01f, 9.93874987e-01f, 9.94297140e-01f, 9.94690275e-01f, 9.95056376e-01f, 9.95397293e-01f,
  9.95714751e-01f, 9.96010357e-01f, 9.96285609e-01f, 9.96541905e-01f, 9.96780544e-01f, 9.97002740e-01f,
  9.97209622e-01f, 9.97402243e-01f, 9.97581583e-01f, 9.97748557e-01f, 9.97904014e-01f, 9.98048748e-01f,
  9.98183496e-01f, 9.98308947e-01f, 9.98425741e-01f, 9.98534475e-01f, 9.98635703e-01f, 9.98729944e-01f,
  9.98817678e-01f, 9.98899356e-01f, 9.98975394e-01f, 9.99046181e-01f, 9.99112080e-01f, 9.99173428e-01f,
  9.99230539e-01f, 9.99283705e-01f, 9.99333199e-01f, 9.99379274e-01f, 9.99422167e-01f, 9.99462096e-01f,
  9.99499267e-01f, 9.99533869e-01f, 9.99566081e-01f, 9.99596068e-01f, 9.99623983e-01f, 9.99649969e-01f,
  9.99674159e-01f, 9.99696678e-01f, 9.99717640e-01f, 9.99737155e-01f, 9.99755320e-01f, 9.99772231e-01f,
  9.99787973e-01f, 9.99802627e-01f, 9.99816268e-01f, 9.99828966e-01f, 9.99840787e-01f, 9.99851791e-01f,
  9.99862035e-01f, 9.99871570e-01f, 9.99880447e-01f, 9.99888710e-01f, 9.99896402e-01f, 9.99903562e-01f,
  9.99910228e-01f, 9.99916433e-01f, 9.99922209e-01f, 9.99927585e-01f, 9.99932591e-01f, 9.99937250e-01f,
  9.99941587e-01f, 9.99945624e-01f, 9.99949383e-01f, 9.99952881e-01f, 9.99956138e-01f, 9.99959170e-01f,
  9.99961992e-01f, 9.99964619e-01f, 9.99967065e-01f, 9.99969341e-01f, 9.99971460e-01f, 9.99973433e-01f,
  9.99975269e-01f, 9.99976978e-01f, 9.99978570e-01f, 9.99980051e-01f, 9.99981430e-01f, 9.99982713e-01f,
  9.99983908e-01f, 9.99985021e-01f
};

PROGMEM static const uint32_t sf2HoldFramesLut[128] = {
  0u, 57u, 62u, 66u, 71u, 76u, 82u, 88u,
  95u, 101u, 109u, 117u, 126u, 135u, 145u, 155u,
  167u, 179u, 192u, 207u, 222u, 238u, 256u, 274u,
  295u, 316u, 340u, 365u, 391u, 420u, 451u, 484u,
  520u, 558u, 600u, 644u, 691u, 742u, 797u, 855u,
  918u, 986u, 1058u, 1136u, 1220u, 1310u, 1406u, 1510u,
  1621u, 1740u, 1868u, 2006u, 2154u, 2312u, 2483u, 2665u,
  2862u, 3072u, 3299u, 3542u, 3802u, 4082u, 4383u, 4706u,
  5052u, 5424u, 5823u, 6252u, 6713u, 7207u, 7738u, 8307u,
  8919u, 9576u, 10281u, 11038u, 11850u, 12723u, 13660u, 14666u,
  15746u, 16905u, 18150u, 19486u, 20921u, 22461u, 24115u, 25891u,
  27797u, 29844u, 32042u, 34401u, 36934u, 39653u, 42573u, 45708u,
  49074u, 52687u, 56566u, 60732u, 65203u, 70004u, 75159u, 80693u,
  86635u, 93014u, 99863u, 107216u, 115110u, 123586u, 132686u, 142456u,
  152945u, 164207u, 176298u, 189279u, 203216u, 218180u, 234245u, 251493u,
  270011u, 289892u, 311238u, 334155u, 358760u, 385176u, 413537u, 443987u
};

PROGMEM static const float sf2FilterAlphaLut[128] = {
  3.50000000e-03f, 3.50000000e-03f, 3.50000000e-03f, 3.50000000e-03f, 3.50000000e-03f, 3.63316057e-03f,
  3.83657271e-03f, 4.05135025e-03f, 4.27812554e-03f, 4.51756582e-03f, 4.77037508e-03f, 5.03729608e-03f,
  5.31911239e-03f, 5.61665063e-03f, 5.93078276e-03f, 6.26242853e-03f, 6.61255796e-03f, 6.98219410e-03f,
  7.37241581e-03f, 7.78436066e-03f, 8.21922812e-03f, 8.67828270e-03f, 9.16285745e-03f, 9.67435744e-03f,
  1.02142635e-02f, 1.07841363e-02f, 1.13856201e-02f, 1.20204473e-02f, 1.26904428e-02f, 1.33975286e-02f,
  1.41437290e-02f, 1.49311749e-02f, 1.57621101e-02f, 1.66388960e-02f, 1.75640175e-02f, 1.85400892e-02f,
  1.95698611e-02f, 2.06562254e-02f, 2.18022229e-02f, 2.30110496e-02f, 2.42860641e-02f, 2.56307945e-02f,
  2.70489460e-02f, 2.85444083e-02f, 3.01212636e-02f, 3.17837944e-02f, 3.35364916e-02f, 3.53840623e-02f,
  3.73314386e-02f, 3.93837856e-02f, 4.15465095e-02f, 4.38252659e-02f, 4.62259682e-02f, 4.87547949e-02f,
  5.14181978e-02f, 5.42229094e-02f, 5.71759491e-02f, 6.02846307e-02f, 6.35565672e-02f, 6.69996761e-02f,
  7.06221838e-02f, 7.44326282e-02f, 7.84398602e-02f, 8.26530447e-02f, 8.70816584e-02f, 9.17354867e-02f,
  9.66246182e-02f, 1.01759437e-01f, 1.07150610e-01f, 1.12809076e-01f, 1.18746026e-01f, 1.24972882e-01f,
  1.31501273e-01f, 1.38343002e-01f, 1.45510013e-01f, 1.53014347e-01f, 1.60868098e-01f, 1.69083354e-01f,
  1.77672142e-01f, 1.86646352e-01f, 1.96017663e-01f, 2.05797458e-01f, 2.15996723e-01f, 2.26625947e-01f,
  2.37695001e-01f, 2.49213017e-01f, 2.61188246e-01f, 2.73627912e-01f, 2.86538052e-01f, 2.99923350e-01f,
  3.13786949e-01f, 3.28130270e-01f, 3.42952809e-01f, 3.58251932e-01f, 3.74022665e-01f, 3.90257476e-01f,
  4.06946066e-01f, 4.24075148e-01f, 4.41628240e-01f, 4.59585472e-01f, 4.77923395e-01f, 4.96614824e-01f,
  5.15628693e-01f, 5.34929956e-01f, 5.54479515e-01f, 5.74234202e-01f, 5.94146815e-01f, 6.14166209e-01f,
  6.34237465e-01f, 6.54302128e-01f, 6.74298532e-01f, 6.94162211e-01f, 7.13826400e-01f, 7.33222630e-01f,
  7.52281417e-01f, 7.70933039e-01f, 7.89108393e-01f, 8.06739929e-01f, 8.23762634e-01f, 8.40115058e-01f,
  8.55740364e-01f, 8.70587350e-01f, 8.84611451e-01f, 8.97775650e-01f, 9.10051294e-01f, 9.21418765e-01f,
  9.31867971e-01f, 1.00000000e+00f
};

PROGMEM static const float sf2ModAtkInc16Lut[16] = {
  1.85759637e-01f, 1.04254071e-01f, 5.85106193e-02f, 3.28379748e-02f,
  1.84296902e-02f, 1.03433139e-02f, 5.80498866e-03f, 3.25793973e-03f,
  1.82845685e-03f, 1.02618671e-03f, 5.75927819e-04f, 3.23228560e-04f,
  1.81405896e-04f, 1.01810617e-04f, 5.71392766e-05f, 3.20683347e-05f
};

PROGMEM static const float sf2ModDecMul16Lut[16] = {
  4.33698803e-01f, 4.33698803e-01f, 5.09855273e-01f, 6.85188606e-01f,
  8.08819387e-01f, 8.87735183e-01f, 9.35351954e-01f, 9.63186312e-01f,
  9.79169137e-01f, 9.88255105e-01f, 9.93391320e-01f, 9.96285609e-01f,
  9.97913667e-01f, 9.98828549e-01f, 9.99342376e-01f, 9.99630868e-01f
};

PROGMEM static const uint32_t SF2_DELAY_FRAMES_LUT[16] = {
  0u, 353u, 542u, 832u, 1278u, 1962u, 3013u, 4626u,
  7104u, 10909u, 16752u, 25724u, 39502u, 60660u, 93150u, 143043u
};

PROGMEM static const uint32_t SF2_LFO_BLOCK_STEP_LUT[16] = {
  12760098u,  15583029u,  19030482u,  23240620u,
  28382169u,  34661194u,  42329336u,  51693909u,
  63130218u,  77096588u,  94152762u, 114982297u,
  140420006u, 171485327u, 209423261u, 255754292u
};

PROGMEM static const char *const SF2_LOOP_NAMES[NUM_SF2_LOOP_MODES] = {
  "SF2", "OFF", "CONT", "REL"
};

PROGMEM static const char *const SF2_ENV_NAMES[NUM_SF2_ENV_MODES] = {
  "SF2", "ADSR"
};

PROGMEM static const char *const SF2_INTERP_NAMES[NUM_SF2_INTERP_MODES] = {
  "HQ", "LIN"
};

PROGMEM static const FmPatch SF2_DEFAULT_PATCH = {
  "SF_01", (uint8_t)InstType::SoundFont, 0, (uint8_t)Sf2LoopOverride::FromFile,
  { { (uint8_t)Sf2EnvMode::Sf2Trim, 100, SF2_TUNE_ZERO, 50, {0, 0, 0} },
    { (uint8_t)Sf2InterpMode::HermiteHq, 100, 0, 0, {0, 0, (uint8_t)(int8_t)0} } },
  { 127, 0, 64, 64, 64 },   // AMP: neutral macro trims for SF2 mode (ATK +0, DEC 1x, SUS 0dB, REL 1x)
  { 127, 0,  0, 127, 127 }, // FLT: wide-open neutral unless shaped
  {   0, 0,  0,   0,   0 }, // FX: dry by default
  0
};

// Semitone pitch ratios (2^(k/12)) for fast ISR baseStepFp calculation without powf()
PROGMEM static const float SF2_SEMI_RATIO_LUT[12] = {
  1.00000000f, 1.05946309f, 1.12246205f, 1.18920712f,
  1.25992105f, 1.33483985f, 1.41421356f, 1.49830708f,
  1.58740105f, 1.68179283f, 1.78179744f, 1.88774863f
};

static inline float sf2FastExp2Frac(float x) {
  // Accurate on [-0.5, +0.5] for fine cents / pitch bend
  return 1.0f + x * (0.6931472f + x * (0.2402265f + x * 0.0555041f));
}

static inline float sf2PitchRatioFromSemis(float semis) {
  semis = constrain(semis, -72.0f, 72.0f);
  int whole = (int)floorf(semis);
  float frac = semis - (float)whole; // [0, 1)
  int oct = (whole >= 0) ? (whole / 12) : ((whole - 11) / 12);
  int semi = whole - oct * 12; // 0..11
  float r = SF2_SEMI_RATIO_LUT[semi] * sf2FastExp2Frac(frac * (1.0f / 12.0f));
  if (oct > 0) {
    r *= (float)(1u << min(oct, 8));
  } else if (oct < 0) {
    r *= 1.0f / (float)(1u << min(-oct, 8));
  }
  return r;
}

static inline float sf2FastExp2(float x) {
  x = constrain(x, -20.0f, 12.0f);
  int k = (int)floorf(x);
  float f = x - (float)k;
  float poly = 1.0f + f * (0.693153f + f * (0.240153f + f * (0.055828f + f * 0.008989f)));
  union { uint32_t u; float flt; } scale;
  scale.u = (uint32_t)((127 + k) << 23);
  return poly * scale.flt;
}

static inline float sf2FastDbToLin(float db) {
  if (db <= -90.0f) return 0.0f;
  return sf2FastExp2(constrain(db, -90.0f, 24.0f) * 0.16609640f);
}

// SF2.01 Concave Curve LUT (-40/96 * log10((127 - k)/127), scaled 0..65535)
PROGMEM static const uint16_t SF2_CONCAVE_LUT[128] = {
      0u,    94u,   188u,   283u,   380u,   476u,   574u,   672u,
    772u,   872u,   973u,  1074u,  1177u,  1281u,  1385u,  1491u,
   1597u,  1704u,  1813u,  1922u,  2032u,  2143u,  2256u,  2369u,
   2484u,  2600u,  2716u,  2834u,  2954u,  3074u,  3196u,  3319u,
   3443u,  3568u,  3695u,  3823u,  3953u,  4084u,  4216u,  4350u,
   4486u,  4623u,  4762u,  4902u,  5044u,  5188u,  5333u,  5481u,
   5630u,  5781u,  5934u,  6089u,  6246u,  6405u,  6567u,  6730u,
   6896u,  7064u,  7235u,  7408u,  7584u,  7762u,  7943u,  8127u,
   8314u,  8503u,  8696u,  8892u,  9092u,  9294u,  9501u,  9711u,
   9924u, 10142u, 10363u, 10589u, 10820u, 11054u, 11294u, 11539u,
  11788u, 12043u, 12304u, 12570u, 12843u, 13122u, 13408u, 13701u,
  14001u, 14309u, 14625u, 14950u, 15284u, 15628u, 15982u, 16347u,
  16723u, 17112u, 17514u, 17931u, 18362u, 18809u, 19274u, 19759u,
  20263u, 20790u, 21342u, 21921u, 22529u, 23170u, 23848u, 24567u,
  25332u, 26151u, 27029u, 27979u, 29010u, 30141u, 31390u, 32787u,
  34371u, 36199u, 38361u, 41007u, 44419u, 49227u, 57447u, 65535u
};

enum Sf2ModTagId : uint8_t {
  SF2_TAG_FLT  = 0,
  SF2_TAG_RES  = 1,
  SF2_TAG_VIB  = 2,
  SF2_TAG_MLFO = 3,
  SF2_TAG_PENV = 4,
  SF2_TAG_FLFO = 5,
  SF2_TAG_FENV = 6,
  SF2_TAG_TREM = 7,
  SF2_TAG_PAN  = 8,
  SF2_TAG_MDLY = 9,
  SF2_TAG_MRAT = 10,
  SF2_TAG_VDLY = 11,
  SF2_TAG_VRAT = 12,
  SF2_TAG_EATK = 13,
  SF2_TAG_EHLD = 14,
  SF2_TAG_EDEC = 15,
  SF2_TAG_ESUS = 16,
  SF2_TAG_EREL = 17,
  SF2_TAG_DLY  = 18,
  SF2_TAG_ATK  = 19,
  SF2_TAG_HLD  = 20,
  SF2_TAG_DEC  = 21,
  SF2_TAG_SUS  = 22,
  SF2_TAG_REL  = 23,
  SF2_TAG_VOL  = 24,
  SF2_TAG_SEMI = 25,
  SF2_TAG_FINE = 26,
  SF2_TAG_MOD  = 27,
  SF2_TAG_COUNT = 28,
  SF2_TAG_NONE  = 0xFFu
};

PROGMEM static const char *const SF2_MOD_TAG_NAMES[SF2_TAG_COUNT] = {
  "FLT",  "RES",  "VIB",  "MLFO", "PENV", "FLFO", "FENV",
  "TREM", "PAN",  "MDLY", "MRAT", "VDLY", "VRAT", "EATK",
  "EHLD", "EDEC", "ESUS", "EREL", "DLY",  "ATK",  "HLD",
  "DEC",  "SUS",  "REL",  "VOL",  "SEMI", "FINE", "MOD"
};

// ============================================================================
// Basic Parameter & Label Helpers
// ============================================================================
FLASHMEM const char *sf2LoopModeName(uint8_t mode) {
  return SF2_LOOP_NAMES[constrain((int)mode, 0, NUM_SF2_LOOP_MODES - 1)];
}

FLASHMEM const char *sf2EnvModeName(uint8_t mode) {
  return SF2_ENV_NAMES[constrain((int)mode, 0, NUM_SF2_ENV_MODES - 1)];
}

FLASHMEM const char *sf2InterpModeName(uint8_t mode) {
  return SF2_INTERP_NAMES[constrain((int)mode, 0, NUM_SF2_INTERP_MODES - 1)];
}

FLASHMEM bool sf2HasFile(int instIdx) {
  if (instIdx < 0 || instIdx >= NUM_INSTRUMENTS) return false;
  return sf2Slots[instIdx].valid && sf2SlotFile[instIdx][0] != '\0';
}

FLASHMEM const char *sf2FileDisplayName(int instIdx) {
  if (instIdx < 0 || instIdx >= NUM_INSTRUMENTS) return "---";
  if (sf2SlotFile[instIdx][0] == '\0') return "---";
  const char *p = sf2SlotFile[instIdx];
  const char *slash = strrchr(p, '/');
  return slash ? (slash + 1) : p;
}

FLASHMEM int sf2PresetCount(int instIdx) {
  if (instIdx < 0 || instIdx >= NUM_INSTRUMENTS || !sf2Slots[instIdx].valid) return 0;
  return (int)sf2Slots[instIdx].totalPresets;
}

FLASHMEM uint16_t sf2CurrentBank(int instIdx) {
  if (instIdx < 0 || instIdx >= NUM_INSTRUMENTS) return 0;
  return sf2Slots[instIdx].curBank;
}

FLASHMEM uint16_t sf2CurrentPresetNum(int instIdx) {
  if (instIdx < 0 || instIdx >= NUM_INSTRUMENTS) return 0;
  return sf2Slots[instIdx].curPresetNum;
}

FLASHMEM const char *sf2CurrentPresetName(int instIdx) {
  if (instIdx < 0 || instIdx >= NUM_INSTRUMENTS || !sf2Slots[instIdx].valid) return "---";
  return sf2Slots[instIdx].presetName;
}

FLASHMEM int sf2ActiveZoneCount(int instIdx) {
  if (instIdx < 0 || instIdx >= NUM_INSTRUMENTS || !sf2Slots[instIdx].valid) return 0;
  return (int)sf2Slots[instIdx].zoneCount;
}

FLASHMEM static void resolveSf2FullPath(int instIdx, char *outFull, size_t outSz) {
  if (!outFull || outSz == 0) return;
  if (instIdx < 0 || instIdx >= NUM_INSTRUMENTS || sf2SlotFile[instIdx][0] == '\0') {
    outFull[0] = '\0';
    return;
  }
  const char *fName = sf2SlotFile[instIdx];
  if (fName[0] == '/') {
    snprintf(outFull, outSz, "%s", fName);
    return;
  }
  const char *dir = (sf2SlotDir[instIdx][0] != '\0') ? sf2SlotDir[instIdx] : SOUNDFONTS_DIR;
  if (strcmp(dir, "/") == 0) {
    snprintf(outFull, outSz, "/%s", fName);
  } else {
    snprintf(outFull, outSz, "%s/%s", dir, fName);
  }
}

// ============================================================================
// Pool Bitmap Allocators (Per-Slot Isolation — Zero Live Compaction)
// ============================================================================
FLASHMEM static int allocZonePoolSlot() {
  for (int i = 0; i < SF2_MAX_ZONES; i++) {
    uint32_t word = (uint32_t)(i >> 5);
    uint32_t bit = 1u << (i & 31);
    if ((freeZoneBits[word] & bit) != 0) {
      freeZoneBits[word] &= ~bit;
      return i;
    }
  }
  return -1;
}

FLASHMEM static void freeZonePoolSlot(uint8_t zIdx) {
  if (zIdx >= SF2_MAX_ZONES) return;
  freeZoneBits[zIdx >> 5] |= (1u << (zIdx & 31));
}

FLASHMEM static int allocModPoolSlot() {
  for (int i = 0; i < SF2_MAX_MOD_POOL; i++) {
    uint32_t word = (uint32_t)(i >> 5);
    uint32_t bit = 1u << (i & 31);
    if ((freeModBits[word] & bit) != 0) {
      freeModBits[word] &= ~bit;
      return i;
    }
  }
  return -1;
}

FLASHMEM static void freeModPoolSlot(uint8_t mIdx) {
  if (mIdx >= SF2_MAX_MOD_POOL) return;
  freeModBits[mIdx >> 5] |= (1u << (mIdx & 31));
}

FLASHMEM static int allocPreRollSlots(int count) {
  if (count <= 0 || count > 2) return -1;
  for (int i = 0; i <= SF2_PREROLL_SLOTS - count; i++) {
    uint64_t mask = (count == 2) ? (3ULL << i) : (1ULL << i);
    if ((freePreRollBits & mask) == mask) {
      freePreRollBits &= ~mask;
      return i;
    }
  }
  return -1;
}

FLASHMEM static void freePreRollSlots(uint8_t slot, bool isStereo) {
  if (slot >= SF2_PREROLL_SLOTS) return;
  if (sf2PreRollInfo[slot].refCount > 1u) {
    sf2PreRollInfo[slot].refCount--;
    return;
  }
  sf2PreRollInfo[slot] = Sf2PreRollMeta{};
  freePreRollBits |= (1ULL << slot);
  if (isStereo && (slot + 1u) < (uint8_t)SF2_PREROLL_SLOTS) {
    sf2PreRollInfo[slot + 1u] = Sf2PreRollMeta{};
    freePreRollBits |= (1ULL << (slot + 1u));
  }
}

FLASHMEM static void releaseSlotZonesAndStreams(int instIdx, bool closeSf2Tag) {
  if (instIdx < 0 || instIdx >= NUM_INSTRUMENTS) return;
  Sf2SlotMeta &meta = sf2Slots[instIdx];
  noInterrupts();
  for (int v = 0; v < NUM_FM_VOICES; v++) {
    const FmVoice *fv = fmSynthVoiceAt(v);
    if (fv && fv->instIdx == (uint8_t)instIdx) {
      sf2Voices[v].wantStream = false;
      sf2Voices[v].streamReady = false;
      sf2Voices[v].inPreRoll = false;
      sf2Voices[v].zonePoolIdx = 0xFFu;
    }
  }
  interrupts();
  samplerSilenceInst(instIdx);
  if (closeSf2Tag && meta.fileTag != 0) {
    samplerInvalidateSf2Tag(meta.fileTag);
    meta.fileTag = 0;
  }
  for (int k = 0; k < (int)meta.zoneCount; k++) {
    uint8_t zId = sf2InstZoneIds[instIdx][k];
    if (zId < SF2_MAX_ZONES) {
      const Sf2Zone &z = sf2ZonePool[zId];
      if (z.preRollSlot != 0xFFu) {
        freePreRollSlots(z.preRollSlot, (z.flags & SF2_FLAG_STEREO) != 0);
      }
      sf2ZoneModGroup[zId] = 0;
      sf2ZoneAux[zId] = Sf2ZoneAux{};
      freeZonePoolSlot(zId);
    }
    sf2InstZoneIds[instIdx][k] = 0xFFu;
  }
  meta.zoneCount = 0;
  for (int m = 0; m < (int)meta.modRuleCount; m++) {
    uint8_t mId = sf2InstModIds[instIdx][m];
    if (mId < SF2_MAX_MOD_POOL) {
      freeModPoolSlot(mId);
    }
    sf2InstModIds[instIdx][m] = 0xFFu;
  }
  meta.modRuleCount = 0;
  meta.defOverrideMask = 0;
  meta.nonCcUsedMask = 0;
  meta.presetModCount = 0;
  meta.instModCount = 0;
  memset(sf2UsedCcMask[instIdx], 0, sizeof(sf2UsedCcMask[instIdx]));
}

// ============================================================================
// RIFF "sfbk" & Hydra Indexed On-Disk Parser (FLASHMEM, RAM1 Staged)
// ============================================================================
static inline uint16_t rdU16Le(const uint8_t *p) {
  return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static inline int16_t rdS16Le(const uint8_t *p) {
  return (int16_t)rdU16Le(p);
}

static inline uint32_t rdU32Le(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

FLASHMEM static bool parseSf2HydraIndex(FsFile &f, Sf2HydraIndex &out) {
  out = Sf2HydraIndex{};
  if (!f.isOpen()) return false;
  uint64_t fSize = f.fileSize();
  if (fSize < 64) return false;

  alignas(4) uint8_t hdr[12];
  if (!f.seekSet(0) || f.read(hdr, 12) != 12) return false;
  if (memcmp(hdr, "RIFF", 4) != 0 || memcmp(hdr + 8, "sfbk", 4) != 0) return false;

  uint64_t pos = 12;
  while (pos + 8 <= fSize) {
    if (!f.seekSet(pos)) break;
    if (f.read(hdr, 8) != 8) break;
    uint32_t cSize = rdU32Le(hdr + 4);
    uint64_t listEnd = min(fSize, pos + 8ULL + (uint64_t)cSize);

    if (memcmp(hdr, "LIST", 4) == 0 && cSize >= 4 && pos + 12 <= fSize) {
      alignas(4) uint8_t listType[4];
      if (f.read(listType, 4) != 4) break;
      const bool isSdta = (memcmp(listType, "sdta", 4) == 0);
      const bool isPdta = (memcmp(listType, "pdta", 4) == 0);

      if (isSdta || isPdta) {
        uint64_t subPos = pos + 12;
        while (subPos + 8 <= listEnd) {
          if (!f.seekSet(subPos)) break;
          alignas(4) uint8_t sh[8];
          if (f.read(sh, 8) != 8) break;
          uint32_t sSize = rdU32Le(sh + 4);
          uint64_t dataPos = subPos + 8;

          if (isSdta && memcmp(sh, "smpl", 4) == 0 && dataPos <= 0xFFFFFFFFULL) {
            out.smplOffset = (uint32_t)dataPos;
            uint64_t avail = (listEnd > dataPos) ? min((uint64_t)sSize, listEnd - dataPos) : 0;
            out.smplFrames = (uint32_t)(avail / 2ULL);
          } else if (isPdta && dataPos <= 0xFFFFFFFFULL) {
            if (memcmp(sh, "phdr", 4) == 0 && sSize >= 76) {
              out.phdrOffset = (uint32_t)dataPos;
              out.phdrCount  = (uint16_t)min((uint32_t)(sSize / 38u) - 1u, 16384u);
            } else if (memcmp(sh, "pbag", 4) == 0 && sSize >= 8) {
              out.pbagOffset = (uint32_t)dataPos;
              out.pbagCount  = (uint16_t)min((uint32_t)(sSize / 4u) - 1u, 65534u);
            } else if (memcmp(sh, "pmod", 4) == 0 && sSize >= 10) {
              out.pmodOffset = (uint32_t)dataPos;
              out.pmodCount  = sSize / 10u;
            } else if (memcmp(sh, "pgen", 4) == 0 && sSize >= 4) {
              out.pgenOffset = (uint32_t)dataPos;
              out.pgenCount  = sSize / 4u;
            } else if (memcmp(sh, "inst", 4) == 0 && sSize >= 44) {
              out.instOffset = (uint32_t)dataPos;
              out.instCount  = (uint16_t)min((uint32_t)(sSize / 22u) - 1u, 16384u);
            } else if (memcmp(sh, "ibag", 4) == 0 && sSize >= 8) {
              out.ibagOffset = (uint32_t)dataPos;
              out.ibagCount  = (uint16_t)min((uint32_t)(sSize / 4u) - 1u, 65534u);
            } else if (memcmp(sh, "imod", 4) == 0 && sSize >= 10) {
              out.imodOffset = (uint32_t)dataPos;
              out.imodCount  = sSize / 10u;
            } else if (memcmp(sh, "igen", 4) == 0 && sSize >= 4) {
              out.igenOffset = (uint32_t)dataPos;
              out.igenCount  = sSize / 4u;
            } else if (memcmp(sh, "shdr", 4) == 0 && sSize >= 92) {
              out.shdrOffset = (uint32_t)dataPos;
              out.shdrCount  = (uint16_t)min((uint32_t)(sSize / 46u) - 1u, 32767u);
            }
          }

          uint64_t nextSub = dataPos + (uint64_t)sSize + (sSize & 1u);
          if (nextSub <= subPos) break;
          subPos = nextSub;
        }
      }
    }

    uint64_t nextPos = pos + 8ULL + (uint64_t)cSize + (cSize & 1u);
    if (nextPos <= pos) break;
    pos = nextPos;
  }

  return out.smplOffset > 0 && out.smplFrames > 0 &&
         out.phdrCount > 0 && out.pbagCount > 0 &&
         out.instCount > 0 && out.ibagCount > 0 && out.shdrCount > 0;
}

struct Sf2GenState {
  uint8_t keyLo = 0;
  uint8_t keyHi = 127;
  uint8_t velLo = 0;
  uint8_t velHi = 127;
  int32_t startOff = 0;
  int32_t endOff = 0;
  int32_t loopStartOff = 0;
  int32_t loopEndOff = 0;
  int16_t coarseTune = 0;
  int16_t fineTune = 0;
  int16_t scaleTuning = 100;
  int16_t attenCb = 0;
  int16_t panTenth = 0;       // -500..+500
  int16_t filterFcCents = 13500;
  int16_t filterQCb = 0;      // 0..960 centibels (0..96 dB)
  int16_t modEnvToPitchCents = 0;
  int16_t modLfoToFilterCents = 0;
  int16_t modEnvToFilterCents = 0;
  int16_t modLfoToVolCb = 0;
  int16_t modAtkTc = -12000;
  int16_t modHoldTc = -12000;
  int16_t modDecTc = -12000;
  int16_t modSusTenths = 0;   // 0..1000 (0.1% decrease from peak)
  int16_t modRelTc = -12000;
  int16_t delayVolTc = -12000;
  int16_t atkTc = -12000;
  int16_t holdTc = -12000;
  int16_t decTc = -12000;
  int16_t susCb = 0;
  int16_t relTc = -12000;
  int16_t keyToVolHold = 0;   // timecents / MIDI key
  int16_t keyToVolDecay = 0;  // timecents / MIDI key
  int16_t delayModLfoTc = -12000;
  int16_t freqModLfoCents = 0;
  int16_t modLfoToPitch = 0;
  int16_t delayVibLfoTc = -12000;
  int16_t freqVibLfoCents = 0;
  int16_t vibLfoToPitch = 0;
  int16_t rootKeyOverride = -1;
  uint8_t sampleModes = 0;
  uint8_t exclClass = 0;
  int32_t targetId = -1;      // instrument ID (pgen 41) or sampleID (igen 53)
};

FLASHMEM static void applySf2GenRecord(Sf2GenState &st, uint16_t oper, int16_t amt, bool isPresetLevel) {
  const uint16_t uAmt = (uint16_t)amt;
  switch (oper) {
    case 0:  if (!isPresetLevel) st.startOff     += (int32_t)amt; break;
    case 1:  if (!isPresetLevel) st.endOff       += (int32_t)amt; break;
    case 2:  if (!isPresetLevel) st.loopStartOff += (int32_t)amt; break;
    case 3:  if (!isPresetLevel) st.loopEndOff   += (int32_t)amt; break;
    case 4:  if (!isPresetLevel) st.startOff     += (int32_t)amt * 32768L; break;
    case 5:  st.modLfoToPitch       = amt; break;
    case 6:  st.vibLfoToPitch       = amt; break;
    case 7:  st.modEnvToPitchCents  = amt; break;
    case 8:  st.filterFcCents       = amt; break;
    case 9:  st.filterQCb           = amt; break;
    case 10: st.modLfoToFilterCents = amt; break;
    case 11: st.modEnvToFilterCents = amt; break;
    case 12: if (!isPresetLevel) st.endOff       += (int32_t)amt * 32768L; break;
    case 13: st.modLfoToVolCb       = amt; break;
    case 17: st.panTenth            = amt; break;
    case 21: st.delayModLfoTc       = amt; break;
    case 22: st.freqModLfoCents     = amt; break;
    case 23: st.delayVibLfoTc       = amt; break;
    case 24: st.freqVibLfoCents     = amt; break;
    case 26: st.modAtkTc            = amt; break;
    case 27: st.modHoldTc           = amt; break;
    case 28: st.modDecTc            = amt; break;
    case 29: st.modSusTenths        = amt; break;
    case 30: st.modRelTc            = amt; break;
    case 33: st.delayVolTc          = amt; break;
    case 34: st.atkTc               = amt; break;
    case 35: st.holdTc              = amt; break;
    case 36: st.decTc               = amt; break;
    case 37: st.susCb               = amt; break;
    case 38: st.relTc               = amt; break;
    case 39: st.keyToVolHold        = amt; break;
    case 40: st.keyToVolDecay       = amt; break;
    case 41: if (isPresetLevel)  st.targetId     = (int32_t)uAmt; break;
    case 43: {
      uint8_t lo = (uint8_t)(uAmt & 0xFFu);
      uint8_t hi = (uint8_t)((uAmt >> 8) & 0xFFu);
      if (lo > hi) { uint8_t t = lo; lo = hi; hi = t; }
      st.keyLo = min(lo, (uint8_t)127u);
      st.keyHi = min(hi, (uint8_t)127u);
      break;
    }
    case 44: {
      uint8_t lo = (uint8_t)(uAmt & 0xFFu);
      uint8_t hi = (uint8_t)((uAmt >> 8) & 0xFFu);
      if (lo > hi) { uint8_t t = lo; lo = hi; hi = t; }
      st.velLo = min(lo, (uint8_t)127u);
      st.velHi = min(hi, (uint8_t)127u);
      break;
    }
    case 45: if (!isPresetLevel) st.loopStartOff += (int32_t)amt * 32768L; break;
    case 48: st.attenCb         = amt; break;
    case 50: if (!isPresetLevel) st.loopEndOff   += (int32_t)amt * 32768L; break;
    case 51: st.coarseTune      = amt; break;
    case 52: st.fineTune        = amt; break;
    case 53: if (!isPresetLevel) st.targetId     = (int32_t)uAmt; break;
    case 54: if (!isPresetLevel) st.sampleModes  = (uint8_t)(uAmt & 3u); break;
    case 56: st.scaleTuning     = amt; break;
    case 57: if (!isPresetLevel) st.exclClass    = (uint8_t)(uAmt & 0x7Fu); break;
    case 58: if (!isPresetLevel && amt >= 0 && amt <= 127) st.rootKeyOverride = amt; break;
    default: break;
  }
}

FLASHMEM static uint8_t sf2TimecentsToAttack127(int32_t tc) {
  if (tc <= -11500) return 0;
  float sec = powf(2.0f, constrain((float)tc, -12000.0f, 4000.0f) * (1.0f / 1200.0f));
  float norm = sqrtf(constrain((sec - 0.0008f) * (1.0f / 4.9992f), 0.0f, 1.0f));
  return (uint8_t)constrain((int)(norm * 127.0f + 0.5f), 0, 127);
}

FLASHMEM static uint8_t sf2TimecentsToHold127(int32_t tc) {
  if (tc <= -11500) return 0;
  return (uint8_t)constrain(1 + (int)((tc + 11500L + 61L) / 123L), 1, 127);
}

FLASHMEM static uint8_t sf2TimecentsToDecRel127(int32_t tc) {
  if (tc <= -10800) return 0;
  return (uint8_t)constrain((int)((tc + 10800L + 62L) / 124L), 0, 127);
}

FLASHMEM static uint8_t sf2TimecentsToDelayNib(int32_t tc) {
  if (tc <= -9200) return 0;
  return (uint8_t)constrain(1 + (int)((tc + 8400L + 371L) / 743L), 1, 15);
}

FLASHMEM static uint8_t sf2EncodeKeyScaleNib(int32_t tcPerKey) {
  // Biased -3..+12 * 10 tc/key (-30..+120 tc/key, nib=3 -> 0 tc/key)
  int step10 = (tcPerKey >= 0) ? ((int)(tcPerKey + 5L) / 10) : ((int)(tcPerKey - 5L) / 10);
  return (uint8_t)constrain(step10 + 3, 0, 15);
}

static inline int sf2DecodeKeyScaleTc(uint8_t nib) {
  return ((int)(nib & 0x0Fu) - 3) * 10;
}

FLASHMEM static uint8_t sf2TimecentsToModNib(int32_t tc) {
  if (tc <= -10800) return 0;
  return (uint8_t)constrain((int)((tc + 10800L + 500L) / 1000L), 0, 15);
}

FLASHMEM static uint8_t sf2LfoFreqToNib(int32_t freqCents) {
  // -3600 cents (~1.02 Hz) .. +1600 cents (~20.6 Hz), nib=8 -> ~6.1 Hz, nib=9 -> ~7.5 Hz
  return (uint8_t)constrain((int)((freqCents + 3600L + 173L) / 347L), 0, 15);
}

FLASHMEM static uint8_t sf2FilterCentsTo127(int32_t fcCents) {
  if (fcCents >= 13450) return 127;
  // Linear in cents across [1500 .. 13500] (~19.5 Hz .. 20 kHz) so modEnv & vel-to-filter add linearly
  return (uint8_t)constrain((int)(((fcCents - 1500L) * 127L + 6000L) / 12000L), 0, 126);
}

FLASHMEM static uint8_t sf2FilterQTo127(int32_t qCb) {
  if (qCb <= 0) return 0;
  // Clamp resonance to 0..240 cB (0..24.0 dB musical range; prevents extreme piercing self-oscillation)
  return (uint8_t)constrain((int)((min(qCb, 240L) * 127L + 120L) / 240L), 0, 127);
}

FLASHMEM static void sanitizeSf2Label(const uint8_t *raw20, char *out21) {
  memcpy(out21, raw20, 20);
  out21[20] = '\0';
  for (int i = 0; i < 20 && out21[i] != '\0'; i++) {
    unsigned char c = (unsigned char)out21[i];
    if (c < 32 || c > 126) out21[i] = ' ';
  }
  int len = (int)strlen(out21);
  while (len > 0 && out21[len - 1] == ' ') {
    out21[--len] = '\0';
  }
  if (out21[0] == '\0') {
    snprintf(out21, 21, "UNTITLED");
  }
}

struct Sf2PhdrItem {
  uint16_t bank;
  uint16_t preset;
  uint16_t phdrIdx;
  uint16_t pbagStart;
  uint16_t pbagEnd;
  char name[21];
};

static inline bool phdrItemLess(uint16_t bankA, uint16_t prstA, uint16_t idxA,
                                uint16_t bankB, uint16_t prstB, uint16_t idxB) {
  if (bankA != bankB) return bankA < bankB;
  if (prstA != prstB) return prstA < prstB;
  return idxA < idxB;
}

// Find preset by exact (bank, preset), by sorted order index (0..totalPresets-1), or first sorted preset
FLASHMEM static bool findSf2PresetHeader(FsFile &f, const Sf2HydraIndex &hydra,
                                         int wantBank, int wantPreset, int wantOrderIdx,
                                         Sf2PhdrItem &outItem, uint16_t &outOrderIdx, uint16_t &outTotalValid) {
  outTotalValid = 0;
  outOrderIdx = 0;
  if (!f.isOpen() || hydra.phdrCount == 0) return false;

  // Partition 16 KB RAM1 chunk into:
  //   [0 .. 4095]     : batched phdr reads (101 * 38 = 3,838 B <= 4 KB)
  //   [4096 .. 16383] : PhdrKey[2048] (6 B * 2,048 = 12,288 B) for single-pass O(N log N) Shellsort
  struct PhdrKey { uint16_t bank, prst, gIdx; };
  uint8_t *rawChunk = reinterpret_cast<uint8_t *>(storageSharedChunkBuf());
  PhdrKey *keys = reinterpret_cast<PhdrKey *>(rawChunk + 4096);
  static constexpr uint16_t BATCH   = 100u;
  static constexpr uint16_t KEY_CAP = (16384u - 4096u) / sizeof(PhdrKey); // 2,048 presets

  bool foundExact = false;
  Sf2PhdrItem bestFirst{};
  bool hasFirst = false;
  uint16_t keyCnt = 0;

  for (uint16_t base = 0; base < hydra.phdrCount; base += BATCH) {
    uint16_t cnt = min((uint16_t)(hydra.phdrCount - base), BATCH);
    size_t rdBytes = (size_t)(cnt + 1u) * 38u; // +1 for next wPresetBagNdx
    if (!f.seekSet((uint64_t)hydra.phdrOffset + (uint64_t)base * 38ULL)) return false;
    if (f.read(rawChunk, rdBytes) != (int)rdBytes) return false;

    for (uint16_t i = 0; i < cnt; i++) {
      const uint8_t *rec = rawChunk + i * 38u;
      const uint8_t *nxt = rec + 38u;
      uint16_t pbag0 = rdU16Le(rec + 24);
      uint16_t pbag1 = rdU16Le(nxt + 24);
      if (pbag1 <= pbag0 || pbag1 > hydra.pbagCount) continue;

      uint16_t prst = rdU16Le(rec + 20);
      uint16_t bank = rdU16Le(rec + 22);
      uint16_t gIdx = (uint16_t)(base + i);
      outTotalValid++;
      if (keyCnt < KEY_CAP) {
        keys[keyCnt++] = PhdrKey{bank, prst, gIdx};
      }

      if (!hasFirst || phdrItemLess(bank, prst, gIdx, bestFirst.bank, bestFirst.preset, bestFirst.phdrIdx)) {
        bestFirst.bank = bank;
        bestFirst.preset = prst;
        bestFirst.phdrIdx = gIdx;
        bestFirst.pbagStart = pbag0;
        bestFirst.pbagEnd = pbag1;
        sanitizeSf2Label(rec, bestFirst.name);
        hasFirst = true;
      }
      if (!foundExact && wantOrderIdx < 0 && wantBank >= 0 && wantPreset >= 0 &&
          bank == (uint16_t)wantBank && prst == (uint16_t)wantPreset) {
        outItem.bank = bank;
        outItem.preset = prst;
        outItem.phdrIdx = gIdx;
        outItem.pbagStart = pbag0;
        outItem.pbagEnd = pbag1;
        sanitizeSf2Label(rec, outItem.name);
        foundExact = true;
      }
    }
  }

  if (outTotalValid == 0 || !hasFirst) return false;

  // Sort collected preset keys in-place via O(N log N) Shellsort in uncached RAM1
  for (uint16_t gap = keyCnt >> 1; gap > 0; gap >>= 1) {
    for (uint16_t i = gap; i < keyCnt; i++) {
      PhdrKey tmp = keys[i];
      uint16_t j = i;
      while (j >= gap && phdrItemLess(tmp.bank, tmp.prst, tmp.gIdx,
                                      keys[j - gap].bank, keys[j - gap].prst, keys[j - gap].gIdx)) {
        keys[j] = keys[j - gap];
        j -= gap;
      }
      keys[j] = tmp;
    }
  }

  if (wantOrderIdx >= 0) {
    uint16_t targetRank = (uint16_t)constrain(wantOrderIdx, 0, (int)outTotalValid - 1);
    if (targetRank == 0) {
      outItem = bestFirst;
      outOrderIdx = 0;
      return true;
    }
    if (targetRank < keyCnt) {
      const PhdrKey &chosen = keys[targetRank];
      alignas(4) uint8_t pairRec[76];
      if (f.seekSet((uint64_t)hydra.phdrOffset + (uint64_t)chosen.gIdx * 38ULL) &&
          f.read(pairRec, sizeof(pairRec)) == (int)sizeof(pairRec)) {
        outItem.bank = chosen.bank;
        outItem.preset = chosen.prst;
        outItem.phdrIdx = chosen.gIdx;
        outItem.pbagStart = rdU16Le(pairRec + 24);
        outItem.pbagEnd = rdU16Le(pairRec + 38u + 24);
        sanitizeSf2Label(pairRec, outItem.name);
        outOrderIdx = targetRank;
        return true;
      }
    }
  }

  if (!foundExact) {
    outItem = bestFirst;
    outOrderIdx = 0;
    return true;
  }

  if (outTotalValid == keyCnt) {
    uint16_t lo = 0, hi = keyCnt;
    while (lo < hi) {
      uint16_t mid = (uint16_t)((lo + hi) >> 1);
      if (phdrItemLess(keys[mid].bank, keys[mid].prst, keys[mid].gIdx,
                       outItem.bank, outItem.preset, outItem.phdrIdx)) {
        lo = mid + 1u;
      } else {
        hi = mid;
      }
    }
    outOrderIdx = lo;
    return true;
  }

  // Fallback rank scan only if a bank contains > 2,048 valid presets
  uint16_t rank = 0;
  for (uint16_t base = 0; base < hydra.phdrCount; base += BATCH) {
    uint16_t cnt = min((uint16_t)(hydra.phdrCount - base), BATCH);
    size_t rdBytes = (size_t)(cnt + 1u) * 38u;
    if (!f.seekSet((uint64_t)hydra.phdrOffset + (uint64_t)base * 38ULL)) break;
    if (f.read(rawChunk, rdBytes) != (int)rdBytes) break;
    for (uint16_t i = 0; i < cnt; i++) {
      uint16_t gIdx = (uint16_t)(base + i);
      if (gIdx == outItem.phdrIdx) continue;
      const uint8_t *rec = rawChunk + i * 38u;
      uint16_t pbag0 = rdU16Le(rec + 24);
      uint16_t pbag1 = rdU16Le(rec + 38u + 24);
      if (pbag1 <= pbag0 || pbag1 > hydra.pbagCount) continue;
      if (phdrItemLess(rdU16Le(rec + 22), rdU16Le(rec + 20), gIdx,
                       outItem.bank, outItem.preset, outItem.phdrIdx)) {
        rank++;
      }
    }
  }
  outOrderIdx = rank;
  return true;
}

FLASHMEM static void populateZonePreRolls(FsFile &f, int instIdx) {
  Sf2SlotMeta &meta = sf2Slots[instIdx];
  uint8_t *rawBuf = reinterpret_cast<uint8_t *>(storageSharedChunkBuf());

  for (int k = 0; k < (int)meta.zoneCount; k++) {
    uint8_t zId = sf2InstZoneIds[instIdx][k];
    if (zId < SF2_MAX_ZONES) {
      sf2ZonePool[zId].preRollSlot = 0xFFu;
    }
  }

  int sdLoads = 0;
  // Prioritize playable middle octaves first (Pass 0: F3..G5 = 53..79 [27 keys <= 32 slots],
  // Pass 1: F2..G6 = 41..91, Pass 2: remaining outer keys) and deduplicate zones sharing
  // identical sample regions so multi-velocity / layered SoundFonts share pre-roll slots.
  for (int pass = 0; pass < 3; pass++) {
    for (int k = 0; k < (int)meta.zoneCount; k++) {
      uint8_t zId = sf2InstZoneIds[instIdx][k];
      if (zId >= SF2_MAX_ZONES) continue;
      Sf2Zone &z = sf2ZonePool[zId];
      if (z.preRollSlot != 0xFFu) continue;
      if (pass == 0 && !(z.keyHi >= 53u && z.keyLo <= 79u)) continue;
      if (pass == 1 && !(z.keyHi >= 41u && z.keyLo <= 91u)) continue;

      const uint32_t totalF = z.totalFrames;
      if (totalF == 0) continue;
      const bool isStereo = (z.flags & SF2_FLAG_STEREO) != 0;
      const uint32_t nRead = min(totalF, SF2_PREROLL_LEN);
      const uint8_t sMode = z.flags & SF2_FLAG_MODE_MASK;
      const uint8_t wrapsInPre = ((sMode != 0) && (z.loopEnd > z.loopStart) && (z.loopEnd == nRead)) ? 1u : 0u;

      // 1. Reuse an already-populated pre-roll slot if another zone references the exact same sample data
      int sharedSlot = -1;
      for (int s = 0; s < SF2_PREROLL_SLOTS; s++) {
        const Sf2PreRollMeta &pm = sf2PreRollInfo[s];
        if (pm.refCount == 0 || pm.refCount >= 250u) continue;
        if (pm.fileTag == meta.fileTag &&
            pm.isStereo == (isStereo ? 1u : 0u) &&
            pm.sampleStart == z.sampleStart &&
            (!isStereo || pm.rightStart == z.rightStart) &&
            pm.wrapsInPre == wrapsInPre &&
            (!wrapsInPre || (pm.loopStart == z.loopStart && pm.loopEnd == z.loopEnd))) {
          sharedSlot = s;
          break;
        }
      }
      if (sharedSlot >= 0) {
        sf2PreRollInfo[sharedSlot].refCount++;
        z.preRollSlot = (uint8_t)sharedSlot;
        continue;
      }

      if (freePreRollBits == 0) continue;
      int prSlot = allocPreRollSlots(isStereo ? 2 : 1);
      if (prSlot < 0) continue;

      size_t rdBytes = (size_t)nRead * 2u;
      uint64_t posL = (uint64_t)meta.hydra.smplOffset + (uint64_t)z.sampleStart * 2ULL;
      if (!f.seekSet(posL) || f.read(rawBuf, rdBytes) != (int)rdBytes) {
        freePreRollSlots((uint8_t)prSlot, isStereo);
        continue;
      }
      memcpy(sf2PreRoll[prSlot], rawBuf, rdBytes);

      if (isStereo) {
        uint64_t posR = (uint64_t)meta.hydra.smplOffset + (uint64_t)z.rightStart * 2ULL;
        if (!f.seekSet(posR) || f.read(rawBuf, rdBytes) != (int)rdBytes) {
          freePreRollSlots((uint8_t)prSlot, true);
          continue;
        }
        memcpy(sf2PreRoll[prSlot + 1], rawBuf, rdBytes);
      }

      // Populate the 2 trailing guard samples [nRead, nRead + 1] for branchless 4-point Hermite
      for (int ch = 0; ch < (isStereo ? 2 : 1); ch++) {
        int16_t *dst = sf2PreRoll[prSlot + ch];
        if (wrapsInPre) {
          uint32_t ls0 = min(z.loopStart, nRead - 1u);
          uint32_t ls1 = min(z.loopStart + 1u, nRead - 1u);
          dst[nRead]      = dst[ls0];
          dst[nRead + 1u] = dst[ls1];
        } else if (totalF > nRead) {
          // Read 2 lookahead samples right after pre-roll so pre-roll -> stream handoff is exact C1 Hermite
          alignas(4) uint8_t tail4[4] = {};
          uint32_t baseF = (ch == 0) ? z.sampleStart : z.rightStart;
          uint64_t lookPos = (uint64_t)meta.hydra.smplOffset + (uint64_t)(baseF + nRead) * 2ULL;
          uint32_t remAfter = totalF - nRead;
          size_t wantLook = (remAfter >= 2u) ? 4u : 2u;
          if (f.seekSet(lookPos) && f.read(tail4, wantLook) == (int)wantLook) {
            dst[nRead]      = rdS16Le(tail4);
            dst[nRead + 1u] = (wantLook == 4u) ? rdS16Le(tail4 + 2u) : dst[nRead];
          } else {
            dst[nRead]      = dst[nRead - 1u];
            dst[nRead + 1u] = dst[nRead - 1u];
          }
        } else {
          int16_t lastS = dst[nRead - 1u];
          dst[nRead]      = lastS;
          dst[nRead + 1u] = lastS;
        }
        for (uint32_t k2 = nRead + 2u; k2 < SF2_PREROLL_BUF_LEN; k2++) {
          dst[k2] = dst[nRead + 1u];
        }
      }

      Sf2PreRollMeta &pm = sf2PreRollInfo[prSlot];
      pm.sampleStart = z.sampleStart;
      pm.rightStart  = isStereo ? z.rightStart : 0u;
      pm.loopStart   = z.loopStart;
      pm.loopEnd     = z.loopEnd;
      pm.fileTag     = meta.fileTag;
      pm.isStereo    = isStereo ? 1u : 0u;
      pm.wrapsInPre  = wrapsInPre;
      pm.refCount    = 1u;
      z.preRollSlot  = (uint8_t)prSlot;

      if ((++sdLoads & 7) == 0) {
        storageKeepTracksStreaming();
      }
    }
  }
}

// ============================================================================
// SoundFont 2.01 Custom Modulator Engine (pmod / imod Parsing & Evaluation)
// ============================================================================
struct Sf2RawMod {
  uint16_t srcOper;
  uint16_t destOper;
  int16_t  amount;
  uint16_t amtSrcOper;
  uint16_t transOper;
};

struct Sf2EvalMods {
  float pitchCents    = 0.0f;
  float modPitchCents = 0.0f;
  float filterCents   = 0.0f;
  float filterQCb     = 0.0f;
  float attenCb       = 0.0f;
  float panTenth      = 0.0f;
  float vibPitchCents = 0.0f;
  float vibFreqCents  = 0.0f;
  float vibDelayTc    = 0.0f;
  float lfoToFltCents = 0.0f;
  float lfoToVolCb    = 0.0f;
  float volDelayTc    = 0.0f;
  float volAtkTc      = 0.0f;
  float volHoldTc     = 0.0f;
  float volDecTc      = 0.0f;
  float volSusCb      = 0.0f;
  float volRelTc      = 0.0f;
  float modFltCents   = 0.0f;
  float modAtkTc      = 0.0f;
  float modDecTc      = 0.0f;
  float modSusTenths  = 0.0f;
  float modRelTc      = 0.0f;
};

FLASHMEM static uint8_t sf2DestGenTagIdx(uint16_t destOper) {
  switch (destOper) {
    case 5:  return SF2_TAG_MLFO; // modLfoToPitch
    case 6:  return SF2_TAG_VIB;  // vibLfoToPitch
    case 7:  return SF2_TAG_PENV; // modEnvToPitch
    case 8:  return SF2_TAG_FLT;  // initialFilterFc
    case 9:  return SF2_TAG_RES;  // initialFilterQ
    case 10: return SF2_TAG_FLFO; // modLfoToFilterFc
    case 11: return SF2_TAG_FENV; // modEnvToFilterFc
    case 13: return SF2_TAG_TREM; // modLfoToVolume
    // Note: 15 (chorusEffectsSend) & 16 (reverbEffectsSend) intentionally ignored (FX row controls sends)
    case 17: return SF2_TAG_PAN;  // pan
    case 21: return SF2_TAG_MDLY; // delayModLFO
    case 22: return SF2_TAG_MRAT; // freqModLFO
    case 23: return SF2_TAG_VDLY; // delayVibLFO
    case 24: return SF2_TAG_VRAT; // freqVibLFO
    case 26: return SF2_TAG_EATK; // attackModEnv
    case 27: return SF2_TAG_EHLD; // holdModEnv
    case 28: return SF2_TAG_EDEC; // decayModEnv
    case 29: return SF2_TAG_ESUS; // sustainModEnv
    case 30: return SF2_TAG_EREL; // releaseModEnv
    case 33: return SF2_TAG_DLY;  // delayVolEnv
    case 34: return SF2_TAG_ATK;  // attackVolEnv
    case 35: return SF2_TAG_HLD;  // holdVolEnv
    case 36: return SF2_TAG_DEC;  // decayVolEnv
    case 37: return SF2_TAG_SUS;  // sustainVolEnv
    case 38: return SF2_TAG_REL;  // releaseVolEnv
    case 48: return SF2_TAG_VOL;  // initialAttenuation
    case 51: return SF2_TAG_SEMI; // coarseTune
    case 52: return SF2_TAG_FINE; // fineTune
    default: return SF2_TAG_NONE;
  }
}

FLASHMEM static bool sf2IsModSrcSupported(uint16_t oper) {
  uint8_t idx = (uint8_t)(oper & 0x7Fu);
  if ((oper & 0x0080u) != 0) {
    // MIDI Continuous Controller: exclude Bank Select (0, 32) and Channel Mode CCs (120..127)
    return (idx != 0u && idx != 32u && idx < 120u);
  }
  // SF2 General Controllers: None(0), NoteOnVel(2), NoteOnKey(3), PitchWheel(14), PitchWheelSens(16)
  // (Aftertouch 10/13 is intentionally excluded because Capstan's sequencer drops 0xA0/0xD0)
  return (idx == 0u || idx == 2u || idx == 3u || idx == 14u || idx == 16u);
}

FLASHMEM static int readSf2ModRange(FsFile &f, uint32_t chunkOff, uint32_t chunkTotalRecs,
                                    uint16_t m0, uint16_t m1, Sf2RawMod *outArr, int maxOut) {
  if (chunkOff == 0 || m1 <= m0 || m1 > chunkTotalRecs || maxOut <= 0) return 0;
  uint16_t want = min((uint16_t)(m1 - m0), (uint16_t)min(maxOut, 32));
  alignas(4) uint8_t raw[32 * 10];
  size_t rdBytes = (size_t)want * 10u;
  if (!f.seekSet((uint64_t)chunkOff + (uint64_t)m0 * 10ULL)) return 0;
  if (f.read(raw, rdBytes) != (int)rdBytes) return 0;

  int count = 0;
  for (uint16_t i = 0; i < want; i++) {
    const uint8_t *p = raw + i * 10u;
    uint16_t srcOper   = rdU16Le(p + 0);
    uint16_t destOper  = rdU16Le(p + 2);
    int16_t  amount    = rdS16Le(p + 4);
    uint16_t amtSrc    = rdU16Le(p + 6);
    uint16_t transOper = rdU16Le(p + 8);
    if ((destOper & 0x8000u) != 0) continue; // Skip linked modulators
    if (sf2DestGenTagIdx(destOper) == SF2_TAG_NONE) continue;
    if (!sf2IsModSrcSupported(srcOper) || !sf2IsModSrcSupported(amtSrc)) continue;
    outArr[count++] = Sf2RawMod{srcOper, destOper, amount, amtSrc, transOper};
  }
  return count;
}

// Merge global + local SF2 modulator lists: identical (srcOper, destOper, amtSrcOper, transOper) overrides global
FLASHMEM static int mergeSf2ModLists(const Sf2RawMod *glob, int globCnt,
                                     const Sf2RawMod *loc, int locCnt,
                                     Sf2RawMod *out, int maxOut) {
  int cnt = 0;
  for (int i = 0; i < globCnt && cnt < maxOut; i++) {
    out[cnt++] = glob[i];
  }
  for (int j = 0; j < locCnt; j++) {
    const Sf2RawMod &m = loc[j];
    bool replaced = false;
    for (int i = 0; i < cnt; i++) {
      if (out[i].srcOper == m.srcOper && out[i].destOper == m.destOper &&
          out[i].amtSrcOper == m.amtSrcOper && out[i].transOper == m.transOper) {
        out[i].amount = m.amount;
        replaced = true;
        break;
      }
    }
    if (!replaced && cnt < maxOut) {
      out[cnt++] = m;
    }
  }
  return cnt;
}

FLASHMEM static void sf2NoteImodDefaultOverrides(Sf2SlotMeta &meta, const Sf2RawMod *imods, int cnt) {
  for (int i = 0; i < cnt; i++) {
    const Sf2RawMod &m = imods[i];
    uint8_t srcLow = (uint8_t)(m.srcOper & 0xFFu);
    if (srcLow == 0x02u && m.destOper == 48u && m.amtSrcOper == 0u) {
      meta.defOverrideMask |= SF2_DEF_OVR_VEL_ATTEN;
    } else if (srcLow == 0x02u && m.destOper == 8u) {
      meta.defOverrideMask |= SF2_DEF_OVR_VEL_FLT;
    } else if (m.srcOper == 0x0081u && (m.destOper == 6u || m.destOper == 5u) && m.amtSrcOper == 0u) {
      meta.defOverrideMask |= SF2_DEF_OVR_MODWHEEL_VIB;
    } else if (srcLow == 0x0Eu && (m.destOper == 52u || m.destOper == 51u)) {
      meta.defOverrideMask |= SF2_DEF_OVR_PITCHWHEEL;
    }
  }
}

FLASHMEM static void sf2RegisterUiModOperand(int instIdx, Sf2SlotMeta &meta, uint16_t oper,
                                             uint8_t tagIdx, bool isPresetMod) {
  if ((oper & 0x0080u) != 0) {
    uint8_t cc = (uint8_t)(oper & 0x7Fu);
    if (cc == 0u || cc == 32u || cc >= 120u) return;
    sf2UsedCcMask[instIdx][cc >> 5] |= (1u << (cc & 31));
    uint8_t &cnt = isPresetMod ? meta.presetModCount : meta.instModCount;
    uint8_t *ccs = isPresetMod ? meta.presetModCcs   : meta.instModCcs;
    uint8_t *tgs = isPresetMod ? meta.presetModTags  : meta.instModTags;
    for (uint8_t k = 0; k < cnt; k++) {
      if (ccs[k] == cc) {
        if (tgs[k] != tagIdx) {
          tgs[k] = SF2_TAG_MOD;
        }
        return;
      }
    }
    if (cnt < (uint8_t)SF2_MAX_UI_MOD_CCS) {
      ccs[cnt] = cc;
      tgs[cnt] = tagIdx;
      cnt++;
    }
  } else {
    uint8_t ctrl = (uint8_t)(oper & 0x7Fu);
    if (ctrl == 14u) {
      meta.nonCcUsedMask |= SF2_NONCC_PITCHWHEEL;
    }
  }
}

FLASHMEM static void sf2EmitModListToSlot(int instIdx, Sf2SlotMeta &meta,
                                          const Sf2RawMod *list, int cnt,
                                          bool isPresetMod, uint8_t modGroup) {
  for (int i = 0; i < cnt; i++) {
    const Sf2RawMod &m = list[i];
    if (m.amount == 0) continue;
    uint8_t tagIdx = sf2DestGenTagIdx(m.destOper);
    if (tagIdx == SF2_TAG_NONE) continue;
    if (meta.modRuleCount >= (uint8_t)SF2_MAX_MOD_RULES_PER_INST) break;
    int poolIdx = allocModPoolSlot();
    if (poolIdx < 0) break;

    uint8_t df = (uint8_t)(m.destOper & SF2_MOD_DEST_MASK);
    if (isPresetMod) df |= SF2_MOD_FLAG_PRESET;
    if (m.transOper == 2u) df |= SF2_MOD_FLAG_ABS_TRANS;

    Sf2ModRule &dst = sf2ModPool[poolIdx];
    dst.srcOper      = m.srcOper;
    dst.amtSrcOper   = m.amtSrcOper;
    dst.amount       = m.amount;
    dst.destAndFlags = df;
    dst.modGroup     = modGroup;
    sf2InstModIds[instIdx][meta.modRuleCount++] = (uint8_t)poolIdx;

    sf2RegisterUiModOperand(instIdx, meta, m.srcOper, tagIdx, isPresetMod);
    if (m.amtSrcOper != 0u) {
      sf2RegisterUiModOperand(instIdx, meta, m.amtSrcOper, tagIdx, isPresetMod);
    }
  }
}

// Emit a zone/layer modulator group, reusing an existing modGroup ID when the rule set is identical
FLASHMEM static uint8_t sf2EmitOrReuseModGroup(int instIdx, Sf2SlotMeta &meta,
                                               const Sf2RawMod *pList, int pCnt,
                                               const Sf2RawMod *iList, int iCnt,
                                               uint8_t &nextModGroup) {
  Sf2ModRule cand[SF2_MAX_MOD_RULES_PER_INST];
  int candCnt = 0;
  for (int pass = 0; pass < 2; pass++) {
    const Sf2RawMod *src = (pass == 0) ? pList : iList;
    const int n = (pass == 0) ? pCnt : iCnt;
    const bool isPrst = (pass == 0);
    for (int k = 0; k < n && candCnt < SF2_MAX_MOD_RULES_PER_INST; k++) {
      const Sf2RawMod &m = src[k];
      if (m.amount == 0) continue;
      if (sf2DestGenTagIdx(m.destOper) == SF2_TAG_NONE) continue;
      uint8_t df = (uint8_t)(m.destOper & SF2_MOD_DEST_MASK);
      if (isPrst) df |= SF2_MOD_FLAG_PRESET;
      if (m.transOper == 2u) df |= SF2_MOD_FLAG_ABS_TRANS;
      cand[candCnt++] = Sf2ModRule{m.srcOper, m.amtSrcOper, m.amount, df, 0u};
    }
  }
  if (candCnt <= 0) return 0u;

  for (uint8_t g = 1u; g < nextModGroup; g++) {
    int matchIdx = 0;
    bool ok = true;
    for (uint8_t r = 0; r < meta.modRuleCount; r++) {
      uint8_t mId = sf2InstModIds[instIdx][r];
      if (mId >= SF2_MAX_MOD_POOL) continue;
      const Sf2ModRule &ex = sf2ModPool[mId];
      if (ex.modGroup != g) continue;
      if (matchIdx >= candCnt ||
          ex.srcOper != cand[matchIdx].srcOper ||
          ex.amtSrcOper != cand[matchIdx].amtSrcOper ||
          ex.amount != cand[matchIdx].amount ||
          ex.destAndFlags != cand[matchIdx].destAndFlags) {
        ok = false;
        break;
      }
      matchIdx++;
    }
    if (ok && matchIdx == candCnt) {
      return g;
    }
  }

  if (nextModGroup >= 255u || meta.modRuleCount >= (uint8_t)SF2_MAX_MOD_RULES_PER_INST) {
    return 0u;
  }
  uint8_t grp = nextModGroup++;
  sf2EmitModListToSlot(instIdx, meta, pList, pCnt, true, grp);
  sf2EmitModListToSlot(instIdx, meta, iList, iCnt, false, grp);
  return grp;
}

FLASHMEM static uint8_t sf2DefaultCcValue(int instIdx, uint8_t cc) {
  if (instIdx >= 0 && instIdx < NUM_INSTRUMENTS) {
    const Sf2SlotMeta &meta = sf2Slots[instIdx];
    bool sawRef = false;
    bool sawBipolar = false;
    bool sawUniInv = false;
    for (uint8_t i = 0; i < meta.modRuleCount; i++) {
      uint8_t mId = sf2InstModIds[instIdx][i];
      if (mId >= SF2_MAX_MOD_POOL) continue;
      const Sf2ModRule &r = sf2ModPool[mId];
      const uint16_t ops[2] = { r.srcOper, r.amtSrcOper };
      for (int k = 0; k < 2; k++) {
        uint16_t op = ops[k];
        if ((op & 0x0080u) != 0 && (uint8_t)(op & 0x7Fu) == cc) {
          sawRef = true;
          uint8_t type = (uint8_t)((op >> 10) & 0x3Fu);
          bool pol = (op & 0x0200u) != 0;
          bool dir = (op & 0x0100u) != 0;
          if (pol && type != 3u) {
            sawBipolar = true;
          } else if (!pol && dir) {
            sawUniInv = true;
          }
        }
      }
    }
    if (sawRef) {
      if (sawBipolar) return 64u;
      if (sawUniInv)  return 127u;
      return 0u;
    }
  }
  if (cc == 7u)  return 100u;
  if (cc == 10u) return 64u;
  if (cc == 11u) return 127u;
  return 0u;
}

FLASHMEM static void sf2ResetModControllers(int instIdx) {
  if (instIdx < 0 || instIdx >= NUM_INSTRUMENTS) return;
  for (int cc = 0; cc < 128; cc++) {
    sf2CcVal[instIdx][cc] = sf2DefaultCcValue(instIdx, (uint8_t)cc);
  }
  sf2PitchWheel14[instIdx] = 8192u;
}

static inline float sf2LookupConcave01(float u) {
  if (u <= 0.0f) return 0.0f;
  if (u >= 1.0f) return 1.0f;
  float pos = u * 127.0f;
  int idx = (int)pos;
  if (idx >= 127) return 1.0f;
  float frac = pos - (float)idx;
  float y0 = (float)SF2_CONCAVE_LUT[idx];
  float y1 = (float)SF2_CONCAVE_LUT[idx + 1];
  return (y0 + frac * (y1 - y0)) * (1.0f / 65535.0f);
}

static inline float sf2ShapeCurve01(float u, uint8_t curveType) {
  if (curveType == 1u) return sf2LookupConcave01(u);
  if (curveType == 2u) return 1.0f - sf2LookupConcave01(1.0f - u);
  return u;
}

FLASHMEM static float sf2EvalModSource(int instIdx, uint16_t oper, uint8_t note, uint8_t velocity) {
  if (oper == 0u) return 1.0f; // SF2 No-Controller = 1.0 unity
  const uint8_t idx = (uint8_t)(oper & 0x7Fu);
  const bool isCc = (oper & 0x0080u) != 0;
  const bool invDir = (oper & 0x0100u) != 0;
  const bool bipolar = (oper & 0x0200u) != 0;
  const uint8_t curve = (uint8_t)((oper >> 10) & 0x3Fu);

  if (!isCc && idx == 14u) {
    // 14-bit Pitch Wheel (0..16383, exact center 8192)
    int pw = (int)sf2PitchWheel14[instIdx];
    if (curve == 3u) {
      bool hi = (pw >= 8192);
      if (invDir) hi = !hi;
      return bipolar ? (hi ? 1.0f : -1.0f) : (hi ? 1.0f : 0.0f);
    }
    if (bipolar) {
      if (pw == 8192) return 0.0f;
      float t = (pw > 8192) ? ((float)(pw - 8192) * (1.0f / 8191.0f))
                            : ((float)(8192 - pw) * (1.0f / 8192.0f));
      float shaped = sf2ShapeCurve01(t, curve);
      float sgn = (pw > 8192) ? 1.0f : -1.0f;
      return invDir ? (-sgn * shaped) : (sgn * shaped);
    }
    float u = (float)pw * (1.0f / 16383.0f);
    if (invDir) u = 1.0f - u;
    return sf2ShapeCurve01(u, curve);
  }

  uint8_t val7 = 0u;
  if (isCc) {
    val7 = sf2CcVal[instIdx][idx];
  } else {
    switch (idx) {
      case 0u:  return 1.0f;
      case 2u:  val7 = velocity & 0x7Fu; break;
      case 3u:  val7 = note & 0x7Fu; break;
      case 16u: val7 = 2u; break; // Default 2-semitone Pitch Wheel Sensitivity
      default:  return 0.0f;
    }
  }

  if (curve == 3u) {
    bool hi = (val7 >= 64u);
    if (invDir) hi = !hi;
    return bipolar ? (hi ? 1.0f : -1.0f) : (hi ? 1.0f : 0.0f);
  }

  if (bipolar) {
    // Exact zero at MIDI center 64 so bipolar CC modulators are 100% neutral at default 64
    if (val7 == 64u) return 0.0f;
    float t = (val7 > 64u) ? ((float)(val7 - 64u) * (1.0f / 63.0f))
                           : ((float)(64u - val7) * (1.0f / 64.0f));
    float shaped = sf2ShapeCurve01(t, curve);
    float sgn = (val7 > 64u) ? 1.0f : -1.0f;
    return invDir ? (-sgn * shaped) : (sgn * shaped);
  }

  float u = (float)(invDir ? (127u - val7) : val7) * (1.0f / 127.0f);
  return sf2ShapeCurve01(u, curve);
}

FLASHMEM static void sf2AccumulateVoiceMods(int instIdx, uint8_t voiceModGroup,
                                            uint8_t note, uint8_t velocity,
                                            Sf2EvalMods &out) {
  out = Sf2EvalMods{};
  if (instIdx < 0 || instIdx >= NUM_INSTRUMENTS) return;
  const Sf2SlotMeta &meta = sf2Slots[instIdx];
  const uint8_t cnt = meta.modRuleCount;
  if (cnt == 0u) return;

  for (uint8_t i = 0; i < cnt; i++) {
    uint8_t mId = sf2InstModIds[instIdx][i];
    if (mId >= SF2_MAX_MOD_POOL) continue;
    const Sf2ModRule &r = sf2ModPool[mId];
    if (r.modGroup != 0u && r.modGroup != voiceModGroup) continue;

    float s1 = sf2EvalModSource(instIdx, r.srcOper, note, velocity);
    if (s1 == 0.0f) continue;
    float s2 = (r.amtSrcOper == 0u) ? 1.0f : sf2EvalModSource(instIdx, r.amtSrcOper, note, velocity);
    if (s2 == 0.0f) continue;

    float prod = (float)r.amount * s1 * s2;
    if ((r.destAndFlags & SF2_MOD_FLAG_ABS_TRANS) != 0 && prod < 0.0f) {
      prod = -prod;
    }

    switch (r.destAndFlags & SF2_MOD_DEST_MASK) {
      case 5:  out.vibPitchCents += prod; break;
      case 6:  out.vibPitchCents += prod; break;
      case 7:  out.modPitchCents += prod; break;
      case 8:  out.filterCents   += prod; break;
      case 9:  out.filterQCb     += prod; break;
      case 10: out.lfoToFltCents += prod; break;
      case 11: out.modFltCents   += prod; break;
      case 13: out.lfoToVolCb    += prod; break;
      case 17: out.panTenth      += prod; break;
      case 21: out.vibDelayTc    += prod; break;
      case 22: out.vibFreqCents  += prod; break;
      case 23: out.vibDelayTc    += prod; break;
      case 24: out.vibFreqCents  += prod; break;
      case 26: out.modAtkTc      += prod; break;
      case 27: out.modDecTc      += prod; break;
      case 28: out.modDecTc      += prod; break;
      case 29: out.modSusTenths  += prod; break;
      case 30: out.modRelTc      += prod; break;
      case 33: out.volDelayTc    += prod; break;
      case 34: out.volAtkTc      += prod; break;
      case 35: out.volHoldTc     += prod; break;
      case 36: out.volDecTc      += prod; break;
      case 37: out.volSusCb      += prod; break;
      case 38: out.volRelTc      += prod; break;
      case 48: out.attenCb       += prod; break;
      case 51: out.pitchCents    += prod * 100.0f; break;
      case 52: out.pitchCents    += prod; break;
      default: break;
    }
  }
}

FLASHMEM static void sf2ApplyVoiceMods(FmVoice &v, Sf2VoiceRuntime &sv, const Sf2Zone &z,
                                       const FmPatch &p, uint8_t defOvrMask, bool isNoteOn) {
  Sf2EvalMods mods;
  sf2AccumulateVoiceMods((int)v.instIdx, sv.modGroup, v.note, v.velocity, mods);

  const Sf2ZoneAux &za = (sv.zonePoolIdx < SF2_MAX_ZONES) ? sf2ZoneAux[sv.zonePoolIdx] : Sf2ZoneAux{};
  const float velSens = constrain((int)p.op[0].level, 0, 100) * 0.01f;
  const float velNorm = constrain((int)v.velocity, 1, 127) * (1.0f / 127.0f);

  // Recomputed on both Note-On and live patch edits so VEL sensitivity responds on held notes
  float velFactor = 1.0f;
  if ((defOvrMask & SF2_DEF_OVR_VEL_ATTEN) == 0u) {
    float velCurve = max(0.012f, velNorm * velNorm);
    velFactor = (1.0f - velSens) + velSens * velCurve;
  }
  sv.zoneGain = ((float)z.gainQ7 * (1.0f / 128.0f)) * velFactor;

  float fcBase = 1500.0f + (float)z.fltCutoff127 * (12000.0f / 127.0f);
  if ((defOvrMask & SF2_DEF_OVR_VEL_FLT) == 0u && z.velLo <= 1u && z.velHi >= 126u && velSens > 0.0f) {
    float invVel = 1.0f - velNorm;
    fcBase -= 2400.0f * velSens * (invVel * invVel);
  }
  sv.baseFltCents = constrain(fcBase, 1500.0f, 13500.0f);

  // Note: static SF2 initialAttenuation uses 0.04 dB/cB in z.gainQ7 (FluidSynth/E-MU real-world balance),
  // while live CC/modulator attenuation uses standard SF2.01 0.10 dB/cB so CC7/CC11 sweep full 0..-96 dB.
  sv.livePitchCents = constrain(mods.pitchCents, -4800.0f, 4800.0f);
  sv.liveGainMul = (mods.attenCb == 0.0f) ? 1.0f : sf2FastDbToLin(constrain(-mods.attenCb * 0.1f, -96.0f, 12.0f));
  sv.livePanOff = (int8_t)constrain((int)lroundf(mods.panTenth * 0.2f), -120, 120);
  sv.liveFltCents = (int16_t)constrain((int)lroundf(mods.filterCents), -24000, 24000);
  sv.lfoToFltCents = (int16_t)constrain((int)za.modLfoToFltSemi * 100 + (int)lroundf(mods.lfoToFltCents), -12000, 12000);
  sv.lfoToVolCb = (int16_t)constrain((int)za.modLfoToVolCbDiv2 * 2 + (int)lroundf(mods.lfoToVolCb), -240, 960);

  // Filter resonance Q (0..240 cB = 0..24 dB safe musical SVF range)
  float effQCb = constrain((float)z.fltRes127 * (240.0f / 127.0f) + mods.filterQCb, 0.0f, 240.0f);
  if (effQCb <= 0.5f) {
    sv.fltFbG = 0.0f;
    sv.fltQGain = 1.0f;
  } else {
    float qDb = effQCb * 0.1f;
    sv.fltFbG = min(1.85f, 2.0f * (1.0f - sf2FastDbToLin(-qDb)));
    sv.fltQGain = 1.0f / (1.0f + 0.35f * sv.fltFbG);
  }

  // Modulation Envelope -> Pitch (modEnvToPitch, gen 7) & Filter Cutoff (modEnvToFilterFc, gen 11)
  sv.modPitchCents = (int16_t)constrain((int)za.modEnvToPitchCents + (int)lroundf(mods.modPitchCents), -12000, 12000);
  sv.modFltCents = constrain((float)z.modEnvToFltSemi * 100.0f + mods.modFltCents, -12000.0f, 12000.0f);
  if (sv.modFltCents != 0.0f || sv.modPitchCents != 0) {
    int mAtkNib = constrain((int)(z.modAtkDecNib >> 4) + (int)lroundf(mods.modAtkTc * 0.001f), 0, 15);
    int mDecNib = constrain((int)(z.modAtkDecNib & 0x0Fu) + (int)lroundf(mods.modDecTc * 0.001f), 0, 15);
    int mRelNib = constrain((int)(z.modSusRelNib & 0x0Fu) + (int)lroundf(mods.modRelTc * 0.001f), 0, 15);
    sv.modAtkInc16 = sf2ModAtkInc16Lut[mAtkNib];
    sv.modDecMul16 = sf2ModDecMul16Lut[mDecNib];
    sv.modSusLvl = constrain((float)(z.modSusRelNib >> 4) * (1.0f / 15.0f) - mods.modSusTenths * 0.001f, 0.0f, 1.0f);
    sv.modRelMul16 = sf2ModDecMul16Lut[mRelNib];
    if (isNoteOn) {
      if (mAtkNib == 0) {
        sv.modEnv = 1.0f;
        sv.modStage = EnvStage::Decay;
      } else {
        sv.modEnv = 0.0f;
        sv.modStage = EnvStage::Attack;
      }
    } else if (sv.modStage == EnvStage::Idle && (v.gate || v.sustained)) {
      sv.modEnv = sv.modSusLvl;
      sv.modStage = EnvStage::Sustain;
    }
  } else if (isNoteOn) {
    sv.modEnv = 0.0f;
    sv.modStage = EnvStage::Idle;
  }

  // 6-Stage DAHD-SR Volume Envelope
  const int keyDelta = 60 - (int)v.note;
  if (isNoteOn) {
    uint8_t dVolNib = z.dlyVolAndLfoNib >> 4;
    if (mods.volDelayTc == 0.0f) {
      sv.envDelayRem = SF2_DELAY_FRAMES_LUT[dVolNib];
    } else {
      float baseDlyTc = (dVolNib == 0u) ? -12000.0f : (-8400.0f + (float)(dVolNib - 1u) * 743.0f);
      float totDlyTc = baseDlyTc + mods.volDelayTc;
      sv.envDelayRem = (totDlyTc <= -9200.0f)
                           ? 0u
                           : (uint32_t)(constrain(sf2FastExp2(totDlyTc * (1.0f / 1200.0f)), 0.0f, 10.0f) *
                                        (float)SAMPLE_RATE_HZ + 0.5f);
    }

    int kHoldTc = keyDelta * sf2DecodeKeyScaleTc(z.keyHoldDecNib >> 4);
    if (mods.volHoldTc == 0.0f) {
      if (z.holdParam127 > 0u) {
        int hIdx = constrain((int)z.holdParam127 + kHoldTc / 123, 1, 127);
        sv.envHoldRem = sf2HoldFramesLut[hIdx];
      } else {
        sv.envHoldRem = 0u;
      }
    } else {
      float baseHTc = (z.holdParam127 == 0u) ? -12000.0f : (-11500.0f + (float)(z.holdParam127 - 1u) * 123.0f);
      float totHTc = baseHTc + (float)kHoldTc + mods.volHoldTc;
      sv.envHoldRem = (totHTc <= -11500.0f)
                          ? 0u
                          : (uint32_t)(constrain(sf2FastExp2(totHTc * (1.0f / 1200.0f)), 0.0f, 15.0f) *
                                       (float)SAMPLE_RATE_HZ + 0.5f);
    }
  }

  if (isNoteOn || v.ampStage == EnvStage::Attack) {
    if (mods.volAtkTc == 0.0f) {
      int atkIdx = constrain((int)z.atkParam127 + (int)p.amp.attack, 0, 127);
      sv.ampAtkInc = sf2AtkIncLut[atkIdx];
    } else {
      float baseAtkSec = 0.0009765625f; // SF2 -12000 timecents (~0.98 ms)
      if (z.atkParam127 > 0u) {
        float norm = (float)z.atkParam127 * (1.0f / 127.0f);
        baseAtkSec = 0.0008f + 4.9992f * (norm * norm);
      }
      float modAtkSec = constrain(baseAtkSec * sf2FastExp2(mods.volAtkTc * (1.0f / 1200.0f)), 0.0008f, 20.0f);
      if (p.amp.attack > 0u) {
        float mNorm = (float)p.amp.attack * (1.0f / 127.0f);
        modAtkSec += 4.9992f * (mNorm * mNorm);
      }
      sv.ampAtkInc = 1.0f / max(1.0f, modAtkSec * (float)SAMPLE_RATE_HZ);
    }
  }

  int baseDecSteps = (z.decParam127 == 0u && mods.volDecTc != 0.0f) ? -8 : (int)z.decParam127;
  int decModSteps  = (mods.volDecTc == 0.0f) ? 0 : (int)lroundf(mods.volDecTc * (1.0f / 124.0f));
  int decIdx = constrain(baseDecSteps + (keyDelta * sf2DecodeKeyScaleTc(z.keyHoldDecNib & 0x0Fu)) / 124 +
                         ((int)p.amp.decay - 64) + decModSteps, 0, 127);

  int baseRelSteps = (z.relParam127 == 0u && mods.volRelTc != 0.0f) ? -8 : (int)z.relParam127;
  int relModSteps  = (mods.volRelTc == 0.0f) ? 0 : (int)lroundf(mods.volRelTc * (1.0f / 124.0f));
  int relIdx = constrain(baseRelSteps + ((int)p.amp.release - 64) + relModSteps, 0, 127);

  float baseSus = (float)z.susParam127 * (1.0f / 127.0f);
  if (mods.volSusCb != 0.0f) {
    baseSus = constrain(baseSus * sf2FastDbToLin(-mods.volSusCb * 0.1f), 0.0f, 1.0f);
  }
  int pSus = constrain((int)p.amp.sustain, 0, 127);
  sv.ampSusLvl = (pSus <= 64)
                     ? (baseSus * ((float)pSus * (1.0f / 64.0f)))
                     : (baseSus + (1.0f - baseSus) * ((float)(pSus - 64) * (1.0f / 63.0f)));
  sv.ampDecMul = sf2TcDecMulLut[decIdx];
  sv.ampRelMul = sf2TcDecMulLut[relIdx];

  // Vibrato / Mod LFO Delay, Rate & Pitch Depth
  if (isNoteOn) {
    sv.lfoPhase = 0u;
    sv.lfoEnv = 0.0f;
    sv.curLfoFltCents = 0;
    uint8_t dLfoNib = z.dlyVolAndLfoNib & 0x0Fu;
    if (mods.vibDelayTc == 0.0f) {
      sv.lfoDelayBlocks = (uint16_t)(SF2_DELAY_FRAMES_LUT[dLfoNib] >> 7);
    } else {
      float baseLfoDlyTc = (dLfoNib == 0u) ? -12000.0f : (-8400.0f + (float)(dLfoNib - 1u) * 743.0f);
      float totLfoDlyTc = baseLfoDlyTc + mods.vibDelayTc;
      uint32_t dFrames = (totLfoDlyTc <= -9200.0f)
                             ? 0u
                             : (uint32_t)(constrain(sf2FastExp2(totLfoDlyTc * (1.0f / 1200.0f)), 0.0f, 10.0f) *
                                          (float)SAMPLE_RATE_HZ + 0.5f);
      sv.lfoDelayBlocks = (uint16_t)min(dFrames >> 7, 65535u);
    }
    sv.lfoPitchCents = (int16_t)z.lfoPitchCentsDiv2 * 2;
  }

  uint32_t baseLfoStep = SF2_LFO_BLOCK_STEP_LUT[(z.flags >> SF2_FLAG_LFO_SHIFT) & 0x0Fu];
  if (mods.vibFreqCents == 0.0f) {
    sv.lfoStepPerBlock = baseLfoStep;
  } else {
    float freqMul = sf2FastExp2(constrain(mods.vibFreqCents * (1.0f / 1200.0f), -6.0f, 5.0f));
    sv.lfoStepPerBlock = (uint32_t)constrain((float)baseLfoStep * freqMul, 65536.0f, 1073741824.0f);
  }
  sv.liveVibPitchCents = (int16_t)constrain((int)lroundf(mods.vibPitchCents), -2400, 2400);
}

FLASHMEM static void sf2RecomputeLiveMods(int instIdx) {
  if (instIdx < 0 || instIdx >= NUM_INSTRUMENTS) return;
  const Sf2SlotMeta &meta = sf2Slots[instIdx];
  if (!meta.valid) return;
  const FmPatch &p = fmSynthPatch(instIdx);
  for (int i = 0; i < NUM_FM_VOICES; i++) {
    const FmVoice *fv = fmSynthVoiceAt(i);
    if (!fv || !fv->active || fv->instIdx != (uint8_t)instIdx) continue;
    Sf2VoiceRuntime &sv = sf2Voices[i];
    if (sv.zonePoolIdx >= SF2_MAX_ZONES) continue;
    sf2ApplyVoiceMods(const_cast<FmVoice &>(*fv), sv, sf2ZonePool[sv.zonePoolIdx], p, meta.defOverrideMask, false);
  }
}

FLASHMEM int sf2PresetModCount(int instIdx) {
  if (instIdx < 0 || instIdx >= NUM_INSTRUMENTS || !sf2Slots[instIdx].valid) return 0;
  return (int)sf2Slots[instIdx].presetModCount;
}

FLASHMEM int sf2InstModCount(int instIdx) {
  if (instIdx < 0 || instIdx >= NUM_INSTRUMENTS || !sf2Slots[instIdx].valid) return 0;
  return (int)sf2Slots[instIdx].instModCount;
}

FLASHMEM int sf2PresetModRowCount(int instIdx) {
  int cnt = sf2PresetModCount(instIdx);
  return max(1, (cnt + SF2_MOD_COLS_PER_ROW - 1) / SF2_MOD_COLS_PER_ROW);
}

FLASHMEM int sf2InstModRowCount(int instIdx) {
  int cnt = sf2InstModCount(instIdx);
  return max(1, (cnt + SF2_MOD_COLS_PER_ROW - 1) / SF2_MOD_COLS_PER_ROW);
}

FLASHMEM bool sf2GetModCellInfo(int instIdx, bool isPresetSection, int idx,
                                char *outLabel, size_t lblSz, uint8_t &outCc, uint8_t &outVal) {
  if (outLabel && lblSz > 0) outLabel[0] = '\0';
  outCc = 0;
  outVal = 0;
  if (instIdx < 0 || instIdx >= NUM_INSTRUMENTS || !sf2Slots[instIdx].valid) return false;
  const Sf2SlotMeta &meta = sf2Slots[instIdx];
  int cnt = isPresetSection ? (int)meta.presetModCount : (int)meta.instModCount;
  if (idx < 0 || idx >= cnt) return false;

  uint8_t cc = isPresetSection ? meta.presetModCcs[idx] : meta.instModCcs[idx];
  uint8_t tag = isPresetSection ? meta.presetModTags[idx] : meta.instModTags[idx];
  const char *tagStr = (tag < SF2_TAG_COUNT) ? SF2_MOD_TAG_NAMES[tag] : "MOD";
  outCc = cc;
  outVal = sf2CcVal[instIdx][cc & 0x7Fu];
  if (outLabel && lblSz > 0) {
    snprintf(outLabel, lblSz, "%s", tagStr);
  }
  return true;
}

FLASHMEM bool sf2AdjustModCell(int instIdx, bool isPresetSection, int idx, int step) {
  if (instIdx < 0 || instIdx >= NUM_INSTRUMENTS || !sf2Slots[instIdx].valid || step == 0) return false;
  const Sf2SlotMeta &meta = sf2Slots[instIdx];
  int cnt = isPresetSection ? (int)meta.presetModCount : (int)meta.instModCount;
  if (idx < 0 || idx >= cnt) return false;

  uint8_t cc = (isPresetSection ? meta.presetModCcs[idx] : meta.instModCcs[idx]) & 0x7Fu;
  uint8_t cur = sf2CcVal[instIdx][cc];
  uint8_t next = (uint8_t)constrain((int)cur + step, 0, 127);
  if (next == cur) return false;

  uint32_t irq = saveIrq();
  sf2CcVal[instIdx][cc] = next;
  sf2RecomputeLiveMods(instIdx);
  restoreIrq(irq);
  return true;
}

FLASHMEM bool sf2ResetModCell(int instIdx, bool isPresetSection, int idx) {
  if (instIdx < 0 || instIdx >= NUM_INSTRUMENTS || !sf2Slots[instIdx].valid) return false;
  const Sf2SlotMeta &meta = sf2Slots[instIdx];
  int cnt = isPresetSection ? (int)meta.presetModCount : (int)meta.instModCount;
  if (idx < 0 || idx >= cnt) return false;

  uint8_t cc = (isPresetSection ? meta.presetModCcs[idx] : meta.instModCcs[idx]) & 0x7Fu;
  uint8_t defVal = sf2DefaultCcValue(instIdx, cc);
  if (sf2CcVal[instIdx][cc] == defVal) return false;

  uint32_t irq = saveIrq();
  sf2CcVal[instIdx][cc] = defVal;
  sf2RecomputeLiveMods(instIdx);
  restoreIrq(irq);
  return true;
}

FLASHMEM void sf2HandleCcInIsr(int instIdx, uint8_t cc, uint8_t val) {
  if (instIdx < 0 || instIdx >= NUM_INSTRUMENTS) return;
  cc &= 0x7Fu;
  val &= 0x7Fu;
  if (sf2CcVal[instIdx][cc] == val) return;
  sf2CcVal[instIdx][cc] = val;
  if ((sf2UsedCcMask[instIdx][cc >> 5] & (1u << (cc & 31))) != 0u) {
    sf2RecomputeLiveMods(instIdx);
  }
}

FLASHMEM void sf2HandlePitchBendInIsr(int instIdx, uint8_t lsb, uint8_t msb) {
  if (instIdx < 0 || instIdx >= NUM_INSTRUMENTS) return;
  uint16_t pw = (uint16_t)(((uint16_t)(msb & 0x7Fu) << 7) | (uint16_t)(lsb & 0x7Fu));
  if (sf2PitchWheel14[instIdx] == pw) return;
  sf2PitchWheel14[instIdx] = pw;
  if ((sf2Slots[instIdx].nonCcUsedMask & SF2_NONCC_PITCHWHEEL) != 0u) {
    sf2RecomputeLiveMods(instIdx);
  }
}

FLASHMEM void sf2ResetPerformanceControllersInIsr(int instIdx) {
  if (instIdx < 0 || instIdx >= NUM_INSTRUMENTS) return;
  bool dirty = (sf2PitchWheel14[instIdx] != 8192u && (sf2Slots[instIdx].nonCcUsedMask & SF2_NONCC_PITCHWHEEL) != 0u);
  sf2PitchWheel14[instIdx] = 8192u;
  if (dirty) {
    sf2RecomputeLiveMods(instIdx);
  }
}

FLASHMEM void sf2RefreshLivePatchParams(int instIdx) {
  if (instIdx < 0 || instIdx >= NUM_INSTRUMENTS) return;
  if ((InstType)fmSynthPatch(instIdx).type != InstType::SoundFont || !sf2Slots[instIdx].valid) return;
  uint32_t irq = saveIrq();
  sf2RecomputeLiveMods(instIdx);
  restoreIrq(irq);
}

FLASHMEM static bool loadSf2PresetForSlot(int instIdx, FsFile &f,
                                          int wantBank, int wantPreset, int wantOrderIdx,
                                          bool *outSawCompressedSf3 = nullptr) {
  if (outSawCompressedSf3) *outSawCompressedSf3 = false;
  if (instIdx < 0 || instIdx >= NUM_INSTRUMENTS || !f.isOpen()) return false;

  Sf2SlotMeta &meta = sf2Slots[instIdx];
  releaseSlotZonesAndStreams(instIdx, false);

  Sf2PhdrItem chosen{};
  uint16_t orderIdx = 0;
  uint16_t totalValid = 0;
  if (!findSf2PresetHeader(f, meta.hydra, wantBank, wantPreset, wantOrderIdx, chosen, orderIdx, totalValid)) {
    return false;
  }

  meta.curBank = chosen.bank;
  meta.curPresetNum = chosen.preset;
  meta.curPresetOrderIdx = orderIdx;
  meta.totalPresets = totalValid;
  meta.maxKeySpan = 0;
  snprintf(meta.presetName, sizeof(meta.presetName), "%s", chosen.name);

  FmPatch &patch = fmSynthPatch(instIdx);
  patch.op[1].ratioX100 = (uint16_t)(chosen.preset & 0xFFu) | ((uint16_t)(chosen.bank & 0xFFu) << 8);

  if (chosen.pbagEnd <= chosen.pbagStart) {
    sf2ResetModControllers(instIdx);
    return true;
  }
  const uint16_t pbagCnt = min((uint16_t)(chosen.pbagEnd - chosen.pbagStart), (uint16_t)256u);

  // Read pbag[pbagStart .. pbagStart + pbagCnt] (up to 257 * 4 = 1,028 B on stack/RAM1)
  alignas(4) uint8_t pbagBuf[257 * 4];
  size_t pbagBytes = (size_t)(pbagCnt + 1u) * 4u;
  if (!f.seekSet((uint64_t)meta.hydra.pbagOffset + (uint64_t)chosen.pbagStart * 4ULL)) return false;
  if (f.read(pbagBuf, pbagBytes) != (int)pbagBytes) return false;

  Sf2GenState pGlobal{};
  pGlobal.coarseTune = 0;
  pGlobal.fineTune = 0;
  pGlobal.scaleTuning = 0; // Preset level uses additive deltas!
  pGlobal.attenCb = 0;
  pGlobal.panTenth = 0;
  pGlobal.filterFcCents = 0;
  pGlobal.filterQCb = 0;
  pGlobal.modEnvToPitchCents = 0;
  pGlobal.modLfoToFilterCents = 0;
  pGlobal.modEnvToFilterCents = 0;
  pGlobal.modLfoToVolCb = 0;
  pGlobal.modAtkTc = 0;
  pGlobal.modHoldTc = 0;
  pGlobal.modDecTc = 0;
  pGlobal.modSusTenths = 0;
  pGlobal.modRelTc = 0;
  pGlobal.delayVolTc = 0;
  pGlobal.atkTc = 0;
  pGlobal.holdTc = 0;
  pGlobal.decTc = 0;
  pGlobal.susCb = 0;
  pGlobal.relTc = 0;
  pGlobal.keyToVolHold = 0;
  pGlobal.keyToVolDecay = 0;
  pGlobal.delayModLfoTc = 0;
  pGlobal.freqModLfoCents = 0;
  pGlobal.modLfoToPitch = 0;
  pGlobal.delayVibLfoTc = 0;
  pGlobal.freqVibLfoCents = 0;
  pGlobal.vibLfoToPitch = 0;

  uint16_t zoneSampleIds[SF2_MAX_ZONES_PER_INST] = {};
  uint16_t zoneSampleLinks[SF2_MAX_ZONES_PER_INST] = {};
  uint16_t zoneSampleTypes[SF2_MAX_ZONES_PER_INST] = {};

  bool anySubsequentPmod = false;
  for (uint16_t pz = 1; pz < pbagCnt; pz++) {
    uint16_t pm0 = rdU16Le(pbagBuf + pz * 4u + 2u);
    uint16_t pm1 = rdU16Le(pbagBuf + (pz + 1u) * 4u + 2u);
    if (pm1 > pm0 && pm1 <= meta.hydra.pmodCount) {
      anySubsequentPmod = true;
      break;
    }
  }

  Sf2RawMod pGlobalMods[32];
  int pGlobalModCnt = 0;
  bool hasPGlobal = false;
  bool emittedGlobalPmods = false;
  uint8_t nextModGroup = 1u;

  for (uint16_t pz = 0; pz < pbagCnt; pz++) {
    uint16_t pg0 = rdU16Le(pbagBuf + pz * 4u);
    uint16_t pm0 = rdU16Le(pbagBuf + pz * 4u + 2u);
    uint16_t pg1 = rdU16Le(pbagBuf + (pz + 1u) * 4u);
    uint16_t pm1 = rdU16Le(pbagBuf + (pz + 1u) * 4u + 2u);
    if (pg1 < pg0 || pg1 > meta.hydra.pgenCount) continue;

    Sf2GenState pZone = pGlobal;
    pZone.targetId = -1;
    uint16_t pgCount = min((uint16_t)(pg1 - pg0), (uint16_t)128u);
    if (pgCount > 0) {
      alignas(4) uint8_t pgenBuf[128 * 4];
      if (!f.seekSet((uint64_t)meta.hydra.pgenOffset + (uint64_t)pg0 * 4ULL)) continue;
      if (f.read(pgenBuf, (size_t)pgCount * 4u) != (int)(pgCount * 4u)) continue;
      for (uint16_t g = 0; g < pgCount; g++) {
        uint16_t oper = rdU16Le(pgenBuf + g * 4u);
        int16_t amt   = rdS16Le(pgenBuf + g * 4u + 2u);
        applySf2GenRecord(pZone, oper, amt, true);
      }
    }

    Sf2RawMod pLocalMods[32];
    int pLocalModCnt = readSf2ModRange(f, meta.hydra.pmodOffset, meta.hydra.pmodCount,
                                       pm0, pm1, pLocalMods, 32);

    if (pZone.targetId < 0) {
      if (pz == 0) {
        pGlobal = pZone; // First zone without instrument generator is Preset Global Zone
        hasPGlobal = true;
        pGlobalModCnt = pLocalModCnt;
        if (pGlobalModCnt > 0) {
          memcpy(pGlobalMods, pLocalMods, (size_t)pGlobalModCnt * sizeof(Sf2RawMod));
          if (!anySubsequentPmod) {
            sf2EmitModListToSlot(instIdx, meta, pGlobalMods, pGlobalModCnt, true, 0u);
            emittedGlobalPmods = true;
          }
        }
      }
      continue;
    }

    const bool isSingleInstPreset = (!hasPGlobal && pbagCnt == 1u) || (hasPGlobal && pbagCnt == 2u);
    Sf2RawMod pEffMods[32];
    int pEffModCnt = 0;
    if (!anySubsequentPmod) {
      if (!hasPGlobal && pz == 0 && pLocalModCnt > 0 && !emittedGlobalPmods) {
        sf2EmitModListToSlot(instIdx, meta, pLocalMods, pLocalModCnt, true, 0u);
        emittedGlobalPmods = true;
      }
    } else {
      pEffModCnt = mergeSf2ModLists(pGlobalMods, pGlobalModCnt, pLocalMods, pLocalModCnt, pEffMods, 32);
    }

    uint16_t instId = (uint16_t)pZone.targetId;
    if (instId >= meta.hydra.instCount) continue;

    // Read inst[instId .. instId + 1] (44 bytes)
    alignas(4) uint8_t instHdr[44];
    if (!f.seekSet((uint64_t)meta.hydra.instOffset + (uint64_t)instId * 22ULL)) continue;
    if (f.read(instHdr, 44) != 44) continue;
    uint16_t ibag0 = rdU16Le(instHdr + 20);
    uint16_t ibag1 = rdU16Le(instHdr + 22 + 20);
    if (ibag1 <= ibag0 || ibag1 > meta.hydra.ibagCount) continue;

    // Partition 16 KB RAM1 chunk into non-overlapping slices for batched Hydra parsing:
    //   [0 .. 4099]     : initial ibag read (<= 4,100 B before split), then 512-record igen cache (2,048 B)
    //   [6000 .. 8049]  : unpacked uint16_t ibagModIdx[1025] (2,050 B)
    //   [8192 .. 10241] : unpacked uint16_t ibagIdx[1025] (2,050 B)
    //   [10248 .. 16135]: 128-record shdr cache (128 * 46 = 5,888 B)
    uint16_t ibagCnt = min((uint16_t)(ibag1 - ibag0), (uint16_t)1024u);
    uint8_t *chunkBytes = reinterpret_cast<uint8_t *>(storageSharedChunkBuf());
    size_t ibagBytes = (size_t)(ibagCnt + 1u) * 4u;
    if (!f.seekSet((uint64_t)meta.hydra.ibagOffset + (uint64_t)ibag0 * 4ULL)) continue;
    if (f.read(chunkBytes, ibagBytes) != (int)ibagBytes) continue;

    uint16_t *ibagModIdx = reinterpret_cast<uint16_t *>(chunkBytes + 6000);
    uint16_t *ibagIdx    = reinterpret_cast<uint16_t *>(chunkBytes + 8192);
    for (uint16_t k = 0; k <= ibagCnt; k++) {
      ibagIdx[k]    = rdU16Le(chunkBytes + k * 4u);
      ibagModIdx[k] = rdU16Le(chunkBytes + k * 4u + 2u);
    }

    bool anySubsequentImod = false;
    for (uint16_t iz = 1; iz < ibagCnt; iz++) {
      if (ibagModIdx[iz + 1u] > ibagModIdx[iz] && ibagModIdx[iz + 1u] <= meta.hydra.imodCount) {
        anySubsequentImod = true;
        break;
      }
    }

    uint8_t *igenCache = chunkBytes;
    uint32_t cachedIgenStart = 0xFFFFFFFFu;
    uint32_t cachedIgenCount = 0;

    uint8_t *shdrCache = chunkBytes + 10248;
    uint16_t cachedShdrStart = 0xFFFFu;
    uint16_t cachedShdrCount = 0;

    Sf2GenState iGlobal{};
    Sf2RawMod iGlobalMods[32];
    int iGlobalModCnt = 0;
    bool hasIGlobal = false;
    bool layerGroupBuilt = false;
    uint8_t layerModGroup = 0u;

    for (uint16_t iz = 0; iz < ibagCnt; iz++) {
      uint16_t ig0 = ibagIdx[iz];
      uint16_t ig1 = ibagIdx[iz + 1u];
      uint16_t im0 = ibagModIdx[iz];
      uint16_t im1 = ibagModIdx[iz + 1u];
      if (ig1 < ig0 || ig1 > meta.hydra.igenCount) continue;

      Sf2GenState iZone = iGlobal;
      iZone.targetId = -1;
      uint16_t igCount = min((uint16_t)(ig1 - ig0), (uint16_t)128u);
      if (igCount > 0) {
        if (!(ig0 >= cachedIgenStart && (uint32_t)ig0 + igCount <= cachedIgenStart + cachedIgenCount)) {
          uint32_t batchCnt = min(meta.hydra.igenCount - (uint32_t)ig0, 512u);
          size_t rdIgen = (size_t)batchCnt * 4u;
          if (!f.seekSet((uint64_t)meta.hydra.igenOffset + (uint64_t)ig0 * 4ULL) ||
              f.read(igenCache, rdIgen) != (int)rdIgen) {
            cachedIgenStart = 0xFFFFFFFFu;
            cachedIgenCount = 0;
            continue;
          }
          cachedIgenStart = ig0;
          cachedIgenCount = batchCnt;
        }
        const uint8_t *igPtr = igenCache + (size_t)(ig0 - cachedIgenStart) * 4u;
        for (uint16_t g = 0; g < igCount; g++) {
          uint16_t oper = rdU16Le(igPtr + g * 4u);
          int16_t amt   = rdS16Le(igPtr + g * 4u + 2u);
          applySf2GenRecord(iZone, oper, amt, false);
        }
      }

      Sf2RawMod iLocalMods[32];
      int iLocalModCnt = readSf2ModRange(f, meta.hydra.imodOffset, meta.hydra.imodCount,
                                         im0, im1, iLocalMods, 32);

      if (iZone.targetId < 0) {
        if (iz == 0) {
          iGlobal = iZone; // First zone without sampleID is Instrument Global Zone
          hasIGlobal = true;
          iGlobalModCnt = iLocalModCnt;
          if (iGlobalModCnt > 0) {
            memcpy(iGlobalMods, iLocalMods, (size_t)iGlobalModCnt * sizeof(Sf2RawMod));
            sf2NoteImodDefaultOverrides(meta, iGlobalMods, iGlobalModCnt);
            if (isSingleInstPreset && !anySubsequentPmod && !anySubsequentImod) {
              sf2EmitModListToSlot(instIdx, meta, iGlobalMods, iGlobalModCnt, false, 0u);
              layerGroupBuilt = true;
              layerModGroup = 0u;
            }
          }
        }
        continue;
      }

      uint8_t keyLo = max(pZone.keyLo, iZone.keyLo);
      uint8_t keyHi = min(pZone.keyHi, iZone.keyHi);
      uint8_t velLo = max(pZone.velLo, iZone.velLo);
      uint8_t velHi = min(pZone.velHi, iZone.velHi);
      if (keyLo > keyHi || velLo > velHi) continue;

      uint16_t smpId = (uint16_t)iZone.targetId;
      if (smpId >= meta.hydra.shdrCount) continue;

      if (!(smpId >= cachedShdrStart && smpId < cachedShdrStart + cachedShdrCount)) {
        uint16_t bStart = (smpId >= 8u && smpId < cachedShdrStart) ? (uint16_t)(smpId - 8u) : smpId;
        uint16_t bCnt = min((uint16_t)(meta.hydra.shdrCount - bStart), (uint16_t)128u);
        size_t rdShdr = (size_t)bCnt * 46u;
        if (!f.seekSet((uint64_t)meta.hydra.shdrOffset + (uint64_t)bStart * 46ULL) ||
            f.read(shdrCache, rdShdr) != (int)rdShdr) {
          cachedShdrStart = 0xFFFFu;
          cachedShdrCount = 0;
          continue;
        }
        cachedShdrStart = bStart;
        cachedShdrCount = bCnt;
      }
      const uint8_t *shdrBuf = shdrCache + (size_t)(smpId - cachedShdrStart) * 46u;

      uint16_t smpType = rdU16Le(shdrBuf + 44);
      if ((smpType & 0x0010u) != 0) {
        if (outSawCompressedSf3) *outSawCompressedSf3 = true;
        continue;
      }
      if ((smpType & 0x8000u) != 0) continue; // Skip ROM samples
      const uint16_t smpTypeClean = smpType & 0x0FFFu;

      int64_t dwStart     = (int64_t)rdU32Le(shdrBuf + 20) + iZone.startOff;
      int64_t dwEnd       = (int64_t)rdU32Le(shdrBuf + 24) + iZone.endOff;
      int64_t dwStartLoop = (int64_t)rdU32Le(shdrBuf + 28) + iZone.loopStartOff;
      int64_t dwEndLoop   = (int64_t)rdU32Le(shdrBuf + 32) + iZone.loopEndOff;
      uint32_t sRate      = constrain(rdU32Le(shdrBuf + 36), 4000u, 192000u);
      uint8_t origPitch   = shdrBuf[40];
      if (origPitch > 127u) origPitch = 60u;
      int8_t pitchCorr    = (int8_t)shdrBuf[41];
      uint16_t smpLink    = rdU16Le(shdrBuf + 42);

      dwStart = constrain(dwStart, (int64_t)0, (int64_t)meta.hydra.smplFrames);
      dwEnd   = constrain(dwEnd,   dwStart,    (int64_t)meta.hydra.smplFrames);
      if (dwEnd - dwStart < 2) continue;

      uint32_t totalF = (uint32_t)(dwEnd - dwStart);
      uint32_t relLoopStart = (uint32_t)constrain(dwStartLoop - dwStart, (int64_t)0, (int64_t)totalF);
      uint32_t relLoopEnd   = (uint32_t)constrain(dwEndLoop   - dwStart, (int64_t)relLoopStart, (int64_t)totalF);

      uint8_t sMode = iZone.sampleModes & 3u;
      if (sMode == 2u) sMode = 0u;
      if (relLoopEnd <= relLoopStart) sMode = 0u;

      uint8_t rootKey = (iZone.rootKeyOverride >= 0 && iZone.rootKeyOverride <= 127)
                            ? (uint8_t)iZone.rootKeyOverride
                            : origPitch;
      int32_t totalCents = (int32_t)(iZone.coarseTune + pZone.coarseTune) * 100L +
                           (int32_t)iZone.fineTune + (int32_t)pZone.fineTune + (int32_t)pitchCorr;
      int32_t cSemi = totalCents / 100L;
      int32_t fCent = totalCents - cSemi * 100L;
      int8_t coarseClamped = (int8_t)constrain(cSemi, -64L, 64L);
      int8_t fineClamped   = (int8_t)constrain(fCent, -99L, 99L);

      uint8_t scaleTune = (uint8_t)constrain((int)iZone.scaleTuning + (int)pZone.scaleTuning, 0, 120);
      int8_t pan100     = (int8_t)constrain(((int)iZone.panTenth + (int)pZone.panTenth) / 5, -100, 100);

      // SF2 initialAttenuation: 0.04 dB per centibel (matches FluidSynth / E-MU real-world balance)
      int32_t attenCb = max(0L, (int32_t)iZone.attenCb + (int32_t)pZone.attenCb);
      float attenDb = (float)attenCb * 0.04f;
      float linGain = (attenDb >= 72.0f) ? 0.0f : powf(10.0f, -attenDb * (1.0f / 20.0f));
      uint8_t gainQ7 = (uint8_t)constrain((int)(linGain * 128.0f + 0.5f), 1, 180);

      int32_t dlyVolTc = (int32_t)iZone.delayVolTc + (int32_t)pZone.delayVolTc;
      int32_t atkTc    = (int32_t)iZone.atkTc      + (int32_t)pZone.atkTc;
      int32_t holdTc   = (int32_t)iZone.holdTc     + (int32_t)pZone.holdTc;
      int32_t decTc    = (int32_t)iZone.decTc      + (int32_t)pZone.decTc;
      int32_t susCb    = constrain((int32_t)iZone.susCb + (int32_t)pZone.susCb, 0L, 1440L);
      int32_t relTc    = (int32_t)iZone.relTc      + (int32_t)pZone.relTc;
      int32_t kHold    = (int32_t)iZone.keyToVolHold  + (int32_t)pZone.keyToVolHold;
      int32_t kDec     = (int32_t)iZone.keyToVolDecay + (int32_t)pZone.keyToVolDecay;

      int32_t fcCent   = constrain((int32_t)iZone.filterFcCents + (int32_t)pZone.filterFcCents, 1500L, 13500L);
      int32_t qCb      = constrain((int32_t)iZone.filterQCb     + (int32_t)pZone.filterQCb,     0L,    960L);
      int32_t modEnvP  = constrain((int32_t)iZone.modEnvToPitchCents  + (int32_t)pZone.modEnvToPitchCents,  -12000L, 12000L);
      int32_t modLfoFc = constrain((int32_t)iZone.modLfoToFilterCents + (int32_t)pZone.modLfoToFilterCents, -12000L, 12000L);
      int32_t modFc    = constrain((int32_t)iZone.modEnvToFilterCents + (int32_t)pZone.modEnvToFilterCents, -12000L, 12000L);
      int32_t modLfoV  = constrain((int32_t)iZone.modLfoToVolCb       + (int32_t)pZone.modLfoToVolCb,       -240L,   240L);

      int32_t modAtkTc  = (int32_t)iZone.modAtkTc  + (int32_t)pZone.modAtkTc;
      int32_t modHoldTc = (int32_t)iZone.modHoldTc + (int32_t)pZone.modHoldTc;
      int32_t modDecTc  = (int32_t)iZone.modDecTc  + (int32_t)pZone.modDecTc;
      int32_t modSusTen = constrain((int32_t)iZone.modSusTenths + (int32_t)pZone.modSusTenths, 0L, 1000L);
      int32_t modRelTc  = (int32_t)iZone.modRelTc  + (int32_t)pZone.modRelTc;
      if (modHoldTc > -11000 && modHoldTc > modDecTc) {
        modDecTc = modHoldTc;
      }

      // Authored Vibrato / Mod LFO (prefer vibLfoToPitch gen 6, fallback to Mod LFO when gen 5/10/13 active)
      int32_t vibPitch = (int32_t)iZone.vibLfoToPitch + (int32_t)pZone.vibLfoToPitch;
      int32_t lfoDlyTc = (int32_t)iZone.delayVibLfoTc + (int32_t)pZone.delayVibLfoTc;
      int32_t lfoFreqC = (int32_t)iZone.freqVibLfoCents + (int32_t)pZone.freqVibLfoCents;
      int32_t modPitch = (int32_t)iZone.modLfoToPitch + (int32_t)pZone.modLfoToPitch;
      if (vibPitch == 0 && (modPitch != 0 || modLfoFc != 0 || modLfoV != 0)) {
        vibPitch = modPitch;
        lfoDlyTc = (int32_t)iZone.delayModLfoTc + (int32_t)pZone.delayModLfoTc;
        lfoFreqC = (int32_t)iZone.freqModLfoCents + (int32_t)pZone.freqModLfoCents;
      } else if (vibPitch == 0 && lfoFreqC == 0) {
        lfoFreqC = -535; // Default ~6.0 Hz natural vibrato rate when live CC1 Mod Wheel is raised
      }

      float susLin = (susCb >= 960) ? 0.0f : powf(10.0f, -(float)susCb * (1.0f / 200.0f));

      uint8_t atk127  = sf2TimecentsToAttack127(atkTc);
      uint8_t hold127 = sf2TimecentsToHold127(holdTc);
      uint8_t dec127  = sf2TimecentsToDecRel127(decTc);
      uint8_t sus127  = (uint8_t)constrain((int)(susLin * 127.0f + 0.5f), 0, 127);
      uint8_t rel127  = sf2TimecentsToDecRel127(relTc);
      uint8_t flt127  = sf2FilterCentsTo127(fcCent);
      uint8_t res127  = sf2FilterQTo127(qCb);
      uint8_t dlyNib  = (uint8_t)((sf2TimecentsToDelayNib(dlyVolTc) << 4) | sf2TimecentsToDelayNib(lfoDlyTc));
      uint8_t kScaleN = (uint8_t)((sf2EncodeKeyScaleNib(kHold) << 4) | sf2EncodeKeyScaleNib(kDec));
      int8_t modSemi  = (int8_t)constrain((modFc >= 0 ? (modFc + 50L) : (modFc - 50L)) / 100L, -120L, 120L);
      uint8_t mAtkDec = (uint8_t)((sf2TimecentsToModNib(modAtkTc) << 4) | sf2TimecentsToModNib(modDecTc));
      uint8_t mSusNib = (uint8_t)constrain((int)(((1000L - modSusTen) * 15L + 500L) / 1000L), 0, 15);
      uint8_t mSusRel = (uint8_t)((mSusNib << 4) | sf2TimecentsToModNib(modRelTc));
      uint8_t lfoFNib = sf2LfoFreqToNib(lfoFreqC);
      int8_t lfoPDiv2 = (int8_t)constrain((vibPitch >= 0 ? (vibPitch + 1L) : (vibPitch - 1L)) / 2L, -125L, 125L);

      // Check if this zone pairs as a linked stereo partner with an already-emitted zone
      bool pairedStereo = false;
      for (uint16_t prevK = 0; prevK < meta.zoneCount; prevK++) {
        uint8_t prevPoolId = sf2InstZoneIds[instIdx][prevK];
        Sf2Zone &pzExisting = sf2ZonePool[prevPoolId];
        if ((pzExisting.flags & SF2_FLAG_STEREO) != 0) continue;
        if (pzExisting.keyLo != keyLo || pzExisting.keyHi != keyHi ||
            pzExisting.velLo != velLo || pzExisting.velHi != velHi ||
            pzExisting.rootKey != rootKey || pzExisting.coarseTune != coarseClamped ||
            pzExisting.fineTuneCents != fineClamped ||
            (pzExisting.flags & SF2_FLAG_MODE_MASK) != sMode) {
          continue;
        }
        if (pzExisting.totalFrames != totalF || pzExisting.loopStart != relLoopStart || pzExisting.loopEnd != relLoopEnd) {
          continue;
        }
        const bool explicitLink = (zoneSampleLinks[prevK] == smpId && smpLink == zoneSampleIds[prevK]) &&
                                  ((zoneSampleTypes[prevK] == 2u && smpTypeClean == 4u) ||
                                   (zoneSampleTypes[prevK] == 4u && smpTypeClean == 2u));
        const bool panOpposite = (pzExisting.pan <= -50 && pan100 >= 50) ||
                                 (pzExisting.pan >= 50 && pan100 <= -50);
        if (explicitLink || panOpposite) {
          const bool curIsLeft = (smpTypeClean == 2u) || (smpTypeClean != 4u && pan100 < pzExisting.pan);
          if (curIsLeft) {
            pzExisting.rightStart = pzExisting.sampleStart;
            pzExisting.sampleStart = (uint32_t)dwStart;
          } else {
            pzExisting.rightStart = (uint32_t)dwStart;
          }
          pzExisting.pan = (int8_t)constrain(((int)pzExisting.pan + (int)pan100) / 2, -100, 100);
          pzExisting.flags |= SF2_FLAG_STEREO;
          pairedStereo = true;
          break;
        }
      }
      if (pairedStereo) continue;

      if (meta.zoneCount >= (uint16_t)SF2_MAX_ZONES_PER_INST) continue;
      int poolSlot = allocZonePoolSlot();
      if (poolSlot < 0) continue;

      uint8_t zoneModGrp = 0u;
      if (iLocalModCnt > 0) {
        Sf2RawMod iEffMods[32];
        int iEffCnt = mergeSf2ModLists(iGlobalMods, iGlobalModCnt, iLocalMods, iLocalModCnt, iEffMods, 32);
        sf2NoteImodDefaultOverrides(meta, iEffMods, iEffCnt);
        if (isSingleInstPreset && !hasIGlobal && ibagCnt == 1u && !anySubsequentPmod) {
          sf2EmitModListToSlot(instIdx, meta, iEffMods, iEffCnt, false, 0u);
          zoneModGrp = 0u;
        } else if (pEffModCnt > 0 || iEffCnt > 0) {
          zoneModGrp = sf2EmitOrReuseModGroup(instIdx, meta, pEffMods, pEffModCnt,
                                              iEffMods, iEffCnt, nextModGroup);
        }
      } else {
        if (!layerGroupBuilt) {
          layerGroupBuilt = true;
          if (pEffModCnt > 0 || iGlobalModCnt > 0) {
            layerModGroup = sf2EmitOrReuseModGroup(instIdx, meta, pEffMods, pEffModCnt,
                                                   iGlobalMods, iGlobalModCnt, nextModGroup);
          } else {
            layerModGroup = 0u;
          }
        }
        zoneModGrp = layerModGroup;
      }
      sf2ZoneModGroup[poolSlot] = zoneModGrp;
      Sf2ZoneAux &za = sf2ZoneAux[poolSlot];
      za.modEnvToPitchCents = (int16_t)modEnvP;
      za.modLfoToFltSemi    = (int8_t)constrain((modLfoFc >= 0 ? (modLfoFc + 50L) : (modLfoFc - 50L)) / 100L, -120L, 120L);
      za.modLfoToVolCbDiv2  = (int8_t)constrain((modLfoV >= 0 ? (modLfoV + 1L) : (modLfoV - 1L)) / 2L, -120L, 120L);

      uint8_t zFlags = (uint8_t)(sMode | (lfoFNib << SF2_FLAG_LFO_SHIFT));
      uint16_t rateField = (uint16_t)min(sRate, 65535u);
      if (sRate > 65535u) {
        rateField = (uint16_t)min((sRate + 2u) >> 2, 65535u);
        zFlags |= SF2_FLAG_RATE_X4;
      }

      Sf2Zone &z = sf2ZonePool[poolSlot];
      z.sampleStart        = (uint32_t)dwStart;
      z.totalFrames        = totalF;
      z.loopStart          = relLoopStart;
      z.loopEnd            = relLoopEnd;
      z.rightStart         = 0;
      z.origRateHz         = rateField;
      z.keyLo              = keyLo;
      z.keyHi              = keyHi;
      z.velLo              = velLo;
      z.velHi              = velHi;
      z.rootKey            = rootKey;
      z.coarseTune         = coarseClamped;
      z.fineTuneCents      = fineClamped;
      z.scaleTuning        = scaleTune;
      z.pan                = pan100;
      z.gainQ7             = gainQ7;
      z.flags              = zFlags;
      z.exclClass          = iZone.exclClass;
      z.preRollSlot        = 0xFFu;
      z.atkParam127        = atk127;
      z.decParam127        = dec127;
      z.susParam127        = sus127;
      z.relParam127        = rel127;
      z.fltCutoff127       = flt127;
      z.holdParam127       = hold127;
      z.dlyVolAndLfoNib    = dlyNib;
      z.keyHoldDecNib      = kScaleN;
      z.fltRes127          = res127;
      z.modEnvToFltSemi    = modSemi;
      z.modAtkDecNib       = mAtkDec;
      z.modSusRelNib       = mSusRel;
      z.lfoPitchCentsDiv2  = lfoPDiv2;

      uint16_t idx = meta.zoneCount++;
      sf2InstZoneIds[instIdx][idx] = (uint8_t)poolSlot;
      zoneSampleIds[idx]   = smpId;
      zoneSampleLinks[idx] = smpLink;
      zoneSampleTypes[idx] = smpTypeClean;
    }
  }

  // Stable-sort zones by (keyLo, keyHi, velLo) and track maxKeySpan for O(log N) Note-On lookup
  uint8_t maxSpan = 0;
  for (uint16_t i = 0; i < meta.zoneCount; i++) {
    uint8_t curId = sf2InstZoneIds[instIdx][i];
    const Sf2Zone &cz = sf2ZonePool[curId];
    uint8_t span = cz.keyHi - cz.keyLo;
    if (span > maxSpan) maxSpan = span;

    uint16_t j = i;
    while (j > 0) {
      const Sf2Zone &pz = sf2ZonePool[sf2InstZoneIds[instIdx][j - 1u]];
      bool comesBefore = (cz.keyLo < pz.keyLo) ||
                         (cz.keyLo == pz.keyLo && (cz.keyHi < pz.keyHi ||
                                                   (cz.keyHi == pz.keyHi && cz.velLo < pz.velLo)));
      if (!comesBefore) break;
      sf2InstZoneIds[instIdx][j] = sf2InstZoneIds[instIdx][j - 1u];
      j--;
    }
    sf2InstZoneIds[instIdx][j] = curId;
  }
  meta.maxKeySpan = maxSpan;
  sf2ResetModControllers(instIdx);

  storageKeepTracksStreaming();
  populateZonePreRolls(f, instIdx);
  return true;
}

// ============================================================================
// Slot File Assignment, Preset Stepping, Copy/Paste & Song Folder Remap
// ============================================================================
FLASHMEM static uint8_t sf2CollectModifiedCcPairs(int instIdx, uint8_t outPairs[64]) {
  if (instIdx < 0 || instIdx >= NUM_INSTRUMENTS || !sf2Slots[instIdx].valid) return 0u;
  uint8_t count = 0u;
  for (int cc = 0; cc < 128 && count < 32u; cc++) {
    if ((sf2UsedCcMask[instIdx][cc >> 5] & (1u << (cc & 31))) == 0u) continue;
    uint8_t cur = sf2CcVal[instIdx][cc];
    if (cur != sf2DefaultCcValue(instIdx, (uint8_t)cc)) {
      outPairs[count * 2u]      = (uint8_t)cc;
      outPairs[count * 2u + 1u] = cur;
      count++;
    }
  }
  return count;
}

FLASHMEM static void sf2ApplySavedCcPairs(int instIdx, const uint8_t *pairs, uint8_t pairCount) {
  if (instIdx < 0 || instIdx >= NUM_INSTRUMENTS || !pairs || pairCount == 0u) return;
  uint8_t n = min(pairCount, (uint8_t)32u);
  uint32_t irq = saveIrq();
  for (uint8_t k = 0; k < n; k++) {
    uint8_t cc  = pairs[k * 2u] & 0x7Fu;
    uint8_t val = pairs[k * 2u + 1u] & 0x7Fu;
    sf2CcVal[instIdx][cc] = val;
  }
  sf2RecomputeLiveMods(instIdx);
  restoreIrq(irq);
}

struct DiskClipSf2 {
  char dir[SAMPLER_DIR_MAX];
  char file[SAMPLER_FILE_MAX];
  uint16_t bank;
  uint16_t preset;
  uint8_t ccCount;
  uint8_t reserved;
  uint8_t ccPairs[64];
};

FLASHMEM static void spillSf2ClipboardIfSlot(int instIdx) {
  if (clipboardSf2SrcSlot != instIdx || instIdx < 0 || instIdx >= NUM_INSTRUMENTS) return;
  DiskClipSf2 tmp{};
  memcpy(tmp.dir, sf2SlotDir[instIdx], sizeof(tmp.dir));
  memcpy(tmp.file, sf2SlotFile[instIdx], sizeof(tmp.file));
  tmp.bank = sf2Slots[instIdx].curBank;
  tmp.preset = sf2Slots[instIdx].curPresetNum;
  tmp.ccCount = min(clipboardSf2CcCount, (uint8_t)32u);
  memcpy(tmp.ccPairs, clipboardSf2CcPairs, sizeof(tmp.ccPairs));

  FsFile f = SD.sdfs.open(CLIP_SF2_PATH, O_RDWR | O_CREAT | O_TRUNC);
  if (f.isOpen()) {
    bool ok = (f.write(&tmp, sizeof(tmp)) == (int)sizeof(tmp));
    f.close();
    clipboardSf2SrcSlot = ok ? -2 : -1;
  } else {
    clipboardSf2SrcSlot = -1;
  }
}

FLASHMEM bool sf2AssignFile(int instIdx, const char *fullOrRelPath, int initialBank, int initialPreset) {
  if (instIdx < 0 || instIdx >= NUM_INSTRUMENTS) return false;
  spillSf2ClipboardIfSlot(instIdx);
  fmSynthSilenceInst(instIdx);
  releaseSlotZonesAndStreams(instIdx, true);

  Sf2SlotMeta &meta = sf2Slots[instIdx];
  meta = Sf2SlotMeta{};
  sf2SlotDir[instIdx][0] = '\0';
  sf2SlotFile[instIdx][0] = '\0';

  if (!fullOrRelPath || !fullOrRelPath[0]) {
    return true;
  }

  const char *p = fullOrRelPath;
  if (strncasecmp(p, "/Capstan/SOUNDFONTS/", SOUNDFONTS_DIR_PREFIX_LEN) == 0 &&
      p[SOUNDFONTS_DIR_PREFIX_LEN] != '\0') {
    p += SOUNDFONTS_DIR_PREFIX_LEN;
  }

  const char *lastSlash = strrchr(p, '/');
  if (!lastSlash) {
    snprintf(sf2SlotFile[instIdx], SAMPLER_FILE_MAX, "%s", p);
  } else {
    if (lastSlash == p) {
      snprintf(sf2SlotDir[instIdx], SAMPLER_DIR_MAX, "/");
    } else if (p[0] == '/') {
      size_t dLen = min((size_t)(lastSlash - p), (size_t)(SAMPLER_DIR_MAX - 1));
      memcpy(sf2SlotDir[instIdx], p, dLen);
      sf2SlotDir[instIdx][dLen] = '\0';
    } else {
      size_t subLen = (size_t)(lastSlash - p);
      snprintf(sf2SlotDir[instIdx], SAMPLER_DIR_MAX, "%s/%.*s", SOUNDFONTS_DIR, (int)subLen, p);
    }
    snprintf(sf2SlotFile[instIdx], SAMPLER_FILE_MAX, "%s", lastSlash + 1);
  }

  char fullPath[SAMPLER_PATH_MAX];
  resolveSf2FullPath(instIdx, fullPath, sizeof(fullPath));
  FsFile f = SD.sdfs.open(fullPath, O_RDONLY);
  if (!f.isOpen()) {
    sf2SlotDir[instIdx][0] = '\0';
    sf2SlotFile[instIdx][0] = '\0';
    return false;
  }

  if (!parseSf2HydraIndex(f, meta.hydra)) {
    f.close();
    sf2SlotDir[instIdx][0] = '\0';
    sf2SlotFile[instIdx][0] = '\0';
    return false;
  }

  uint16_t tag = nextSf2FileTag++;
  if (tag == 0) tag = nextSf2FileTag++;
  meta.fileTag = tag;
  meta.valid = true;

  bool sawSf3 = false;
  bool ok = loadSf2PresetForSlot(instIdx, f, initialBank, initialPreset, -1, &sawSf3);
  f.close();
  if (!ok || (meta.zoneCount == 0 && sawSf3)) {
    releaseSlotZonesAndStreams(instIdx, true);
    meta = Sf2SlotMeta{};
    sf2SlotDir[instIdx][0] = '\0';
    sf2SlotFile[instIdx][0] = '\0';
    return false;
  }
  samplerPrewarmSf2File(fullPath, meta.fileTag);
  return true;
}

FLASHMEM void sf2UnloadFile(int instIdx) {
  if (instIdx < 0 || instIdx >= NUM_INSTRUMENTS) return;
  spillSf2ClipboardIfSlot(instIdx);
  fmSynthSilenceInst(instIdx);
  releaseSlotZonesAndStreams(instIdx, true);
  sf2Slots[instIdx] = Sf2SlotMeta{};
  sf2SlotDir[instIdx][0] = '\0';
  sf2SlotFile[instIdx][0] = '\0';
  fmSynthPatch(instIdx).op[1].ratioX100 = 0;
}

FLASHMEM bool sf2SelectPresetByIndex(int instIdx, int presetListIdx) {
  if (instIdx < 0 || instIdx >= NUM_INSTRUMENTS || !sf2Slots[instIdx].valid) return false;
  int total = (int)sf2Slots[instIdx].totalPresets;
  if (total <= 0) return false;
  int targetOrder = constrain(presetListIdx, 0, total - 1);

  char fullPath[SAMPLER_PATH_MAX];
  resolveSf2FullPath(instIdx, fullPath, sizeof(fullPath));
  FsFile f = SD.sdfs.open(fullPath, O_RDONLY);
  if (!f.isOpen()) return false;

  fmSynthSilenceInst(instIdx);
  bool ok = loadSf2PresetForSlot(instIdx, f, -1, -1, targetOrder);
  f.close();
  if (ok) {
    samplerPrewarmSf2File(fullPath, sf2Slots[instIdx].fileTag);
  }
  return ok;
}

FLASHMEM void sf2RequestCcPreset(int instIdx, uint8_t ccVal127) {
  if (instIdx < 0 || instIdx >= NUM_INSTRUMENTS) return;
  pendingCcPreset[instIdx] = (int8_t)(ccVal127 & 0x7Fu);
}

FLASHMEM bool sf2StepPreset(int instIdx, int delta) {
  if (instIdx < 0 || instIdx >= NUM_INSTRUMENTS || !sf2Slots[instIdx].valid) return false;
  int total = (int)sf2Slots[instIdx].totalPresets;
  if (total <= 1 || delta == 0) return false;
  int nextIdx = ((int)sf2Slots[instIdx].curPresetOrderIdx + delta) % total;
  if (nextIdx < 0) nextIdx += total;
  return sf2SelectPresetByIndex(instIdx, nextIdx);
}

static bool sf2PreviewModified = false;
static uint16_t sf2PreviewOrigBank = 0;
static uint16_t sf2PreviewOrigPreset = 0;
static bool sf2PreviewCursorReady = false;

FLASHMEM void sf2Init() {
  sf2ResetAll();
}

FLASHMEM void sf2ResetAll() {
  sf2PreviewModified = false;
  sf2PreviewCursorReady = false;
  closeSf2Browser();
  samplerInvalidateSf2Tag(0);
  for (size_t w = 0; w < sizeof(freeZoneBits) / sizeof(freeZoneBits[0]); w++) {
    freeZoneBits[w] = 0xFFFFFFFFu;
  }
  for (size_t w = 0; w < sizeof(freeModBits) / sizeof(freeModBits[0]); w++) {
    freeModBits[w] = 0xFFFFFFFFu;
  }
  freePreRollBits = (SF2_PREROLL_SLOTS >= 64) ? ~0ULL : ((1ULL << SF2_PREROLL_SLOTS) - 1ULL);
  memset(sf2PreRoll, 0, sizeof(sf2PreRoll));
  for (int s = 0; s < SF2_PREROLL_SLOTS; s++) {
    sf2PreRollInfo[s] = Sf2PreRollMeta{};
  }
  memset(sf2ZonePool, 0, sizeof(sf2ZonePool));
  memset(sf2ZoneAux, 0, sizeof(sf2ZoneAux));
  memset(sf2InstZoneIds, 0xFF, sizeof(sf2InstZoneIds));
  memset(sf2ZoneModGroup, 0, sizeof(sf2ZoneModGroup));
  memset(sf2ModPool, 0, sizeof(sf2ModPool));
  memset(sf2InstModIds, 0xFF, sizeof(sf2InstModIds));
  memset(sf2UsedCcMask, 0, sizeof(sf2UsedCcMask));
  memset(sf2SlotDir, 0, sizeof(sf2SlotDir));
  memset(sf2SlotFile, 0, sizeof(sf2SlotFile));
  for (int v = 0; v < NUM_FM_VOICES; v++) {
    sf2Voices[v] = Sf2VoiceRuntime{};
  }
  for (int i = 0; i < NUM_INSTRUMENTS; i++) {
    sf2Slots[i] = Sf2SlotMeta{};
    pendingCcPreset[i] = -1;
    sf2ResetModControllers(i);
  }
  clipboardSf2SrcSlot = -1;
  clipboardSf2CcCount = 0;
}

FLASHMEM void sf2ClearSlot(int instIdx) {
  if (instIdx < 0 || instIdx >= NUM_INSTRUMENTS) return;
  spillSf2ClipboardIfSlot(instIdx);
  releaseSlotZonesAndStreams(instIdx, true);
  sf2Slots[instIdx] = Sf2SlotMeta{};
  pendingCcPreset[instIdx] = -1;
  sf2ResetModControllers(instIdx);
  sf2SlotDir[instIdx][0] = '\0';
  sf2SlotFile[instIdx][0] = '\0';
}

FLASHMEM void sf2CopySlot(int instIdx) {
  if (instIdx < 0 || instIdx >= NUM_INSTRUMENTS) return;
  if ((InstType)fmSynthPatch(instIdx).type != InstType::SoundFont || !sf2HasFile(instIdx)) {
    clipboardSf2SrcSlot = -1;
    clipboardSf2CcCount = 0;
    return;
  }
  clipboardSf2SrcSlot = (int8_t)instIdx;
  clipboardSf2Bank = sf2Slots[instIdx].curBank;
  clipboardSf2Preset = sf2Slots[instIdx].curPresetNum;
  clipboardSf2CcCount = sf2CollectModifiedCcPairs(instIdx, clipboardSf2CcPairs);
}

FLASHMEM void sf2PasteSlot(int instIdx) {
  if (instIdx < 0 || instIdx >= NUM_INSTRUMENTS) return;
  if ((InstType)fmSynthPatch(instIdx).type != InstType::SoundFont) {
    sf2ClearSlot(instIdx);
    clipboardSf2SrcSlot = -1;
    clipboardSf2CcCount = 0;
    return;
  }

  if (clipboardSf2SrcSlot >= 0 && clipboardSf2SrcSlot < NUM_INSTRUMENTS) {
    if (clipboardSf2SrcSlot != instIdx) {
      char fullPath[SAMPLER_PATH_MAX];
      resolveSf2FullPath(clipboardSf2SrcSlot, fullPath, sizeof(fullPath));
      if (sf2AssignFile(instIdx, fullPath, (int)clipboardSf2Bank, (int)clipboardSf2Preset)) {
        sf2ApplySavedCcPairs(instIdx, clipboardSf2CcPairs, clipboardSf2CcCount);
      }
    }
  } else if (clipboardSf2SrcSlot == -2) {
    DiskClipSf2 tmp{};
    FsFile f = SD.sdfs.open(CLIP_SF2_PATH, O_RDONLY);
    if (f.isOpen()) {
      if (f.read(&tmp, sizeof(tmp)) == (int)sizeof(tmp)) {
        tmp.dir[SAMPLER_DIR_MAX - 1] = '\0';
        tmp.file[SAMPLER_FILE_MAX - 1] = '\0';
        char fullPath[SAMPLER_PATH_MAX];
        if (tmp.file[0] == '/') {
          snprintf(fullPath, sizeof(fullPath), "%s", tmp.file);
        } else {
          const char *d = tmp.dir[0] ? tmp.dir : SOUNDFONTS_DIR;
          snprintf(fullPath, sizeof(fullPath), (strcmp(d, "/") == 0) ? "/%s" : "%s/%s", d, tmp.file);
        }
        f.close();
        SD.sdfs.remove(CLIP_SF2_PATH);
        if (sf2AssignFile(instIdx, fullPath, (int)tmp.bank, (int)tmp.preset)) {
          sf2ApplySavedCcPairs(instIdx, tmp.ccPairs, tmp.ccCount);
        }
        clipboardSf2SrcSlot = -1;
        clipboardSf2CcCount = 0;
        return;
      }
      f.close();
      SD.sdfs.remove(CLIP_SF2_PATH);
    }
    sf2ClearSlot(instIdx);
  }
  clipboardSf2SrcSlot = -1;
  clipboardSf2CcCount = 0;
}

FLASHMEM void sf2SilenceInst(int instIdx) {
  if (instIdx < 0 || instIdx >= NUM_INSTRUMENTS) return;
  for (int v = 0; v < NUM_FM_VOICES; v++) {
    const FmVoice *fv = fmSynthVoiceAt(v);
    if (fv && fv->instIdx == (uint8_t)instIdx) {
      sf2Voices[v].wantStream = false;
    }
  }
  samplerSilenceInst(instIdx);
}

FLASHMEM void sf2StopAllStreams() {
  for (int v = 0; v < NUM_FM_VOICES; v++) {
    sf2Voices[v].wantStream = false;
  }
}

FLASHMEM void sf2RemapSongFolder(const char *oldSongName, const char *newSongName) {
  if (!oldSongName || !oldSongName[0] || !newSongName || !newSongName[0]) return;
  if (strcasecmp(oldSongName, newSongName) == 0) return;
  char oldPrefix[64], newPrefix[64];
  snprintf(oldPrefix, sizeof(oldPrefix), "%s/%s/", SONGS_DIR, oldSongName);
  snprintf(newPrefix, sizeof(newPrefix), "%s/%s/", SONGS_DIR, newSongName);
  size_t oldLen = strlen(oldPrefix);
  size_t newLen = strlen(newPrefix);
  for (int i = 0; i < NUM_INSTRUMENTS; i++) {
    if (strncasecmp(sf2SlotDir[i], oldPrefix, oldLen) == 0) {
      char tail[SAMPLER_DIR_MAX];
      snprintf(tail, sizeof(tail), "%s", sf2SlotDir[i] + oldLen);
      size_t maxTail = (newLen < (size_t)(SAMPLER_DIR_MAX - 1)) ? ((size_t)(SAMPLER_DIR_MAX - 1) - newLen) : 0;
      snprintf(sf2SlotDir[i], SAMPLER_DIR_MAX, "%.*s%.*s", (int)newLen, newPrefix, (int)maxTail, tail);
      if (sf2Slots[i].fileTag != 0) {
        samplerInvalidateSf2Tag(sf2Slots[i].fileTag);
        uint16_t tag = nextSf2FileTag++;
        if (tag == 0) tag = nextSf2FileTag++;
        sf2Slots[i].fileTag = tag;
      }
    }
  }
}

// ============================================================================
// Real-Time Note-On / Note-Off, Background Stream Attach & Audio ISR Renderer
// ============================================================================
static inline uint8_t effectiveZoneLoopMode(const Sf2Zone &z, const FmPatch &p) {
  if (z.loopEnd <= z.loopStart) return 0u;
  switch ((Sf2LoopOverride)constrain((int)p.algorithm, 0, NUM_SF2_LOOP_MODES - 1)) {
    case Sf2LoopOverride::FromFile: default: return z.flags & SF2_FLAG_MODE_MASK;
    case Sf2LoopOverride::Off:               return 0u;
    case Sf2LoopOverride::Cont:              return 1u;
    case Sf2LoopOverride::UntilRel:          return 3u;
  }
}

FLASHMEM int sf2MatchNoteZones(int instIdx, uint8_t note, uint8_t velocity, uint8_t outZoneIds[2]) {
  if (instIdx < 0 || instIdx >= NUM_INSTRUMENTS || !sf2Slots[instIdx].valid) return 0;
  const Sf2SlotMeta &meta = sf2Slots[instIdx];
  const uint16_t zCnt = meta.zoneCount;
  if (zCnt == 0) return 0;

  // Binary search sorted sf2InstZoneIds by keyLo using maxKeySpan lower bound
  const uint8_t minKeyLo = (note > meta.maxKeySpan) ? (uint8_t)(note - meta.maxKeySpan) : 0u;
  uint16_t lo = 0;
  uint16_t hi = zCnt;
  while (lo < hi) {
    uint16_t mid = (uint16_t)((lo + hi) >> 1);
    uint8_t midId = sf2InstZoneIds[instIdx][mid];
    if (midId < SF2_MAX_ZONES && sf2ZonePool[midId].keyLo < minKeyLo) {
      lo = mid + 1u;
    } else {
      hi = mid;
    }
  }

  int matched = 0;
  for (uint16_t k = lo; k < zCnt && matched < 2; k++) {
    uint8_t zPoolIdx = sf2InstZoneIds[instIdx][k];
    if (zPoolIdx >= SF2_MAX_ZONES) continue;
    const Sf2Zone &z = sf2ZonePool[zPoolIdx];
    if (z.keyLo > note) break;
    if (note <= z.keyHi && velocity >= z.velLo && velocity <= z.velHi) {
      outZoneIds[matched++] = zPoolIdx;
    }
  }
  return matched;
}

FLASHMEM bool sf2PrepareZoneVoiceOn(int instIdx, uint8_t zonePoolIdx, bool allowSdIo,
                                    uint8_t &outEvStreamTag, uint16_t &outEvStreamGen) {
  if (instIdx < 0 || instIdx >= NUM_INSTRUMENTS || zonePoolIdx >= SF2_MAX_ZONES) return false;
  const Sf2SlotMeta &meta = sf2Slots[instIdx];
  if (!meta.valid) return false;
  const Sf2Zone &z = sf2ZonePool[zonePoolIdx];
  const uint32_t totalF = z.totalFrames;
  if (totalF == 0) return false;

  // Fast Path (< 1 us, 0 SD I/O — works directly in 250 us MIDI ISR & 2.9 ms Audio ISR!):
  // Zone has its initial 640 frames cached in uncached RAM1 sf2PreRoll.
  if (z.preRollSlot < SF2_PREROLL_SLOTS) {
    outEvStreamTag = zonePoolIdx;
    outEvStreamGen = 0u; // 0 = Pre-Roll Immediate Mode
    return true;
  }

  // Fallback for 33rd+ uncached zone in large chromatic / drum SoundFonts: defer to loop() when in ISR
  if (!allowSdIo) {
    return false;
  }

  char fullPath[SAMPLER_PATH_MAX];
  resolveSf2FullPath(instIdx, fullPath, sizeof(fullPath));
  const FmPatch &p = fmSynthPatch(instIdx);
  const bool isStereo = (z.flags & SF2_FLAG_STEREO) != 0;

  SharedSf2StreamReq req{};
  req.instIdx        = instIdx;
  req.voiceIdx       = -1; // pendingVoice until ISR activates
  req.sf2Path        = fullPath;
  req.sf2FileTag     = meta.fileTag;
  req.kind           = isStereo ? SharedStreamKind::Sf2StereoLink : SharedStreamKind::Sf2Mono16;
  req.sf2LoopMode    = effectiveZoneLoopMode(z, p);
  req.dataOffsetL    = meta.hydra.smplOffset + z.sampleStart * 2u;
  req.dataOffsetR    = isStereo ? (meta.hydra.smplOffset + z.rightStart * 2u) : 0u;
  req.totalFrames    = totalF;
  req.startFrame     = 0u;
  req.loopStartFrame = z.loopStart;
  req.loopEndFrame   = z.loopEnd;

  uint32_t sIdx = 0, sGen = 0;
  if (!samplerOpenSf2Stream(req, sIdx, sGen, 1)) {
    return false;
  }
  outEvStreamTag = zonePoolIdx;
  outEvStreamGen = (uint16_t)(((sIdx & 0x07u) << 13) | (sGen & 0x1FFFu));
  return true;
}

FLASHMEM void sf2InitVoiceInIsr(int voiceIdx, FmVoice &v, const FmPatch &p,
                                uint8_t evStreamTag, uint16_t evStreamGen,
                                bool wasActiveSf2) {
  if (voiceIdx < 0 || voiceIdx >= NUM_FM_VOICES || evStreamTag >= SF2_MAX_ZONES) {
    v.active = false;
    return;
  }

  Sf2VoiceRuntime &sv = sf2Voices[voiceIdx];
  // Immediately release any previous stream owned by this voice slot before re-initializing
  if (v.opPhase[0] < SHARED_NUM_STREAMS) {
    SamplerStream *oldSt = samplerStreamSlot(v.opPhase[0]);
    if (oldSt && oldSt->generation == v.opPhase[1]) {
      oldSt->active = false;
      oldSt->pendingVoice = false;
      oldSt->loopLockedInRing = false;
    }
  }
  sv.wantStream = false;

  const Sf2Zone &z = sf2ZonePool[evStreamTag];
  const uint32_t totalF = z.totalFrames;
  if (totalF == 0) {
    v.active = false;
    return;
  }

  // Enforce SF2 Exclusive Class (e.g. closed hi-hat choking open hi-hat on the same instrument)
  if (z.exclClass != 0) {
    for (int i = 0; i < NUM_FM_VOICES; i++) {
      if (i == voiceIdx) continue;
      const FmVoice *other = fmSynthVoiceAt(i);
      if (other && other->active && other->instIdx == v.instIdx &&
          other->age != v.age && sf2Voices[i].exclClass == z.exclClass) {
        FmVoice &ov = const_cast<FmVoice &>(*other);
        ov.gate = false;
        ov.sustained = false;
        ov.ampStage = EnvStage::Release;
        ov.fltStage = EnvStage::Release;
        sf2Voices[i].envDelayRem = 0u;
        sf2Voices[i].envHoldRem = 0u;
        sf2Voices[i].ampRelMul = 0.992f; // ~8 ms smooth choke fade
        sf2Voices[i].choked = true;
      }
    }
  }

  if (wasActiveSf2 && (sv.prevOutL != 0 || sv.prevOutR != 0)) {
    sv.declickL = sv.prevOutL;
    sv.declickR = sv.prevOutR;
    sv.declickRem = 32;
  } else {
    sv.declickL = 0;
    sv.declickR = 0;
    sv.declickRem = 0;
  }
  sv.choked = false;
  sv.preRollHold = false;
  sv.prevOutL = 0;
  sv.prevOutR = 0;
  sv.voiceAge = v.age;
  sv.zonePoolIdx = evStreamTag;
  sv.exclClass = z.exclClass;
  sv.modGroup = sf2ZoneModGroup[evStreamTag];
  sv.totalFrames = totalF;
  sv.preRollPos = 0;
  sv.fracFp = 0;
  sv.prevL = 0;
  sv.prevR = 0;
  sv.lastL = 0;
  sv.lastR = 0;
  sv.zonePan = z.pan;

  // Base step at neutral patch tune (patch TUNE, Modulators, Vibrato LFO, & PitchBend are applied per-block)
  float keySemis = ((float)((int)v.note - (int)z.rootKey)) * ((float)z.scaleTuning * 0.01f) +
                   (float)z.coarseTune + ((float)z.fineTuneCents * 0.01f);
  float rateRatio = ((float)zoneSampleRateHz(z) * (1.0f / (float)SAMPLE_RATE_HZ)) * sf2PitchRatioFromSemis(keySemis);
  sv.baseStepFp = (uint32_t)constrain((int64_t)(rateRatio * 65536.0f + 0.5f), (int64_t)16, (int64_t)(16 * 65536));

  const uint8_t defOvrMask = (v.instIdx < NUM_INSTRUMENTS) ? sf2Slots[v.instIdx].defOverrideMask : 0u;
  sf2ApplyVoiceMods(v, sv, z, p, defOvrMask, true);

  const uint8_t effLoop = effectiveZoneLoopMode(z, p);

  if (evStreamGen == 0u && z.preRollSlot < SF2_PREROLL_SLOTS) {
    sv.inPreRoll = true;
    sv.streamReady = false;
    v.opPhase[0] = 0xFFu;
    v.opPhase[1] = 0u;
    const bool contLoopInsidePre = (effLoop == 1u) && (z.loopEnd > z.loopStart) && (z.loopEnd <= SF2_PREROLL_LEN);
    sv.wantStream = (totalF > SF2_PREROLL_LEN) && !contLoopInsidePre;
    int16_t s0L = sf2PreRoll[z.preRollSlot][0];
    int16_t s0R = ((z.flags & SF2_FLAG_STEREO) != 0) ? sf2PreRoll[z.preRollSlot + 1][0] : s0L;
    sv.prevL = s0L;
    sv.prevR = s0R;
  } else {
    uint32_t sIdx = (evStreamGen >> 13) & 0x07u;
    uint32_t sGen = evStreamGen & 0x1FFFu;
    sv.inPreRoll = false;
    sv.wantStream = false;
    sv.streamReady = true;
    v.opPhase[0] = sIdx;
    v.opPhase[1] = sGen;
    samplerActivateStream(sIdx, sGen, voiceIdx);
    SamplerStream *st = samplerStreamSlot(sIdx);
    if (st && st->generation == sGen && st->head > st->tail) {
      if ((z.flags & SF2_FLAG_STEREO) != 0) {
        uint32_t p0 = st->ring[st->tail & (SHARED_STEREO_RING_FRAMES - 1u)];
        sv.prevL = (int16_t)(p0 & 0xFFFFu);
        sv.prevR = (int16_t)(p0 >> 16);
      } else {
        const int16_t *mRing = reinterpret_cast<const int16_t *>(st->ring);
        sv.prevL = sv.prevR = mRing[st->tail & (SHARED_MONO_RING_FRAMES - 1u)];
      }
    }
  }
}

FLASHMEM bool sf2VoiceMatchesZone(int voiceIdx, uint8_t zonePoolIdx) {
  if (voiceIdx < 0 || voiceIdx >= NUM_FM_VOICES) return false;
  return sf2Voices[voiceIdx].zonePoolIdx == zonePoolIdx;
}

FLASHMEM bool sf2PlayToEndOnRelease(int instIdx, int voiceIdx, const FmPatch &p) {
  (void)instIdx;
  if (voiceIdx < 0 || voiceIdx >= NUM_FM_VOICES) return false;
  if ((Sf2EnvMode)p.op[0].wave != Sf2EnvMode::Sf2Trim) return false;
  uint8_t zId = sf2Voices[voiceIdx].zonePoolIdx;
  if (zId >= SF2_MAX_ZONES) return false;
  const Sf2Zone &z = sf2ZonePool[zId];
  return (effectiveZoneLoopMode(z, p) == 0u) && (z.scaleTuning == 0u); // Unpitched drum hits ring naturally
}

FLASHMEM void sf2ServiceStreams(bool fromCheckpoint) {
  if (!fromCheckpoint) {
    for (int inst = 0; inst < NUM_INSTRUMENTS; inst++) {
      int8_t ccVal = pendingCcPreset[inst];
      if (ccVal >= 0) {
        if (isSf2BrowserOpen() && sf2BrowserTargetSlot() == inst) {
          continue; // Defer until modal closes so sharedBrowserPool/chunk stay coherent
        }
        pendingCcPreset[inst] = -1;
        if ((InstType)fmSynthPatch(inst).type == InstType::SoundFont &&
            sf2Slots[inst].valid && sf2Slots[inst].totalPresets > 1) {
          int tot = (int)sf2Slots[inst].totalPresets;
          int wantIdx = ((int)ccVal * (tot - 1) + 63) / 127;
          if (wantIdx != (int)sf2Slots[inst].curPresetOrderIdx) {
            if (sf2SelectPresetByIndex(inst, wantIdx)) {
              markSongDirty();
              uiForceRedraw();
            }
          }
        }
      }
    }
  }

  for (int vIdx = 0; vIdx < NUM_FM_VOICES; vIdx++) {
    if (!sf2Voices[vIdx].wantStream) continue;

    uint32_t irq = saveIrq();
    const FmVoice *fv = fmSynthVoiceAt(vIdx);
    if (!fv || !fv->active || (InstType)fmSynthPatch(fv->instIdx).type != InstType::SoundFont) {
      sf2Voices[vIdx].wantStream = false;
      restoreIrq(irq);
      continue;
    }
    const int instIdx = (int)fv->instIdx;
    const uint32_t expectAge = fv->age;
    const uint8_t zPoolIdx = sf2Voices[vIdx].zonePoolIdx;
    restoreIrq(irq);

    if (zPoolIdx >= SF2_MAX_ZONES || !sf2Slots[instIdx].valid) {
      sf2Voices[vIdx].wantStream = false;
      continue;
    }

    const Sf2SlotMeta &meta = sf2Slots[instIdx];
    const Sf2Zone &z = sf2ZonePool[zPoolIdx];
    const FmPatch &p = fmSynthPatch(instIdx);
    const uint32_t totalF = z.totalFrames;
    if (totalF <= SF2_PREROLL_LEN) {
      sf2Voices[vIdx].wantStream = false;
      continue;
    }

    char fullPath[SAMPLER_PATH_MAX];
    resolveSf2FullPath(instIdx, fullPath, sizeof(fullPath));
    const bool isStereo = (z.flags & SF2_FLAG_STEREO) != 0;

    SharedSf2StreamReq req{};
    req.instIdx        = instIdx;
    req.voiceIdx       = vIdx;
    req.sf2Path        = fullPath;
    req.sf2FileTag     = meta.fileTag;
    req.kind           = isStereo ? SharedStreamKind::Sf2StereoLink : SharedStreamKind::Sf2Mono16;
    uint8_t streamLoopMode = effectiveZoneLoopMode(z, p);
    if (streamLoopMode == 3u && z.loopEnd <= SF2_PREROLL_LEN) {
      streamLoopMode = 0u;
    }
    req.sf2LoopMode    = streamLoopMode;
    req.dataOffsetL    = meta.hydra.smplOffset + z.sampleStart * 2u;
    req.dataOffsetR    = isStereo ? (meta.hydra.smplOffset + z.rightStart * 2u) : 0u;
    req.totalFrames    = totalF;
    req.startFrame     = SF2_PREROLL_LEN;
    req.loopStartFrame = z.loopStart;
    req.loopEndFrame   = z.loopEnd;

    uint32_t sIdx = 0, sGen = 0;
    // Fast 1-pass (512-frame) attach so bursts of rapid notes all connect within < 1.5 ms total
    if (!samplerOpenSf2Stream(req, sIdx, sGen, 1)) {
      sf2Voices[vIdx].wantStream = false;
      continue;
    }

    irq = saveIrq();
    FmVoice *liveV = const_cast<FmVoice *>(fmSynthVoiceAt(vIdx));
    if (liveV && liveV->active && liveV->age == expectAge && sf2Voices[vIdx].wantStream) {
      liveV->opPhase[0] = sIdx;
      liveV->opPhase[1] = sGen;
      sf2Voices[vIdx].streamReady = true;
      sf2Voices[vIdx].wantStream = false;
    } else {
      SamplerStream *st = samplerStreamSlot(sIdx);
      if (st && st->generation == sGen) {
        st->active = false;
        st->pendingVoice = false;
      }
    }
    restoreIrq(irq);
  }
}

__attribute__((noinline)) static float evalSf2Interp(float ym1, float y0, float y1, float y2, float frac, bool useHq) {
  if (!useHq) {
    return y0 + frac * (y1 - y0);
  }
  float c1 = 0.5f * (y1 - ym1);
  float c2 = ym1 - 2.5f * y0 + 2.0f * y1 - 0.5f * y2;
  float c3 = 0.5f * (y2 - ym1) + 1.5f * (y0 - y1);
  return ((c3 * frac + c2) * frac + c1) * frac + y0;
}

__attribute__((noinline)) static float stepSf2SvfChan(float raw, float qGain, float curAlpha, float curEffFbG,
                                                      float &lp1, float &lp2) {
  float x = raw * qGain + curEffFbG * (lp1 - lp2);
  if (x > 1.8f || x < -1.8f) {
    float ax = fabsf(x);
    float u = ax - 1.8f;
    float y = 1.8f + (0.7f * u) / (0.7f + u);
    x = (x >= 0.0f) ? y : -y;
  }
  lp1 += curAlpha * (x - lp1);
  lp2 += curAlpha * (lp1 - lp2);
  return lp2;
}

__attribute__((noinline)) static void retireSf2VoiceInIsr(FmVoice &v, Sf2VoiceRuntime &sv) {
  v.active = false;
  v.ampStage = EnvStage::Idle;
  if (v.opPhase[0] < SHARED_NUM_STREAMS) {
    SamplerStream *st = samplerStreamSlot(v.opPhase[0]);
    if (st && st->generation == v.opPhase[1]) {
      st->active = false;
    }
  }
  sv.wantStream = false;
  sv.preRollHold = false;
  sv.choked = false;
  sv.prevOutL = 0;
  sv.prevOutR = 0;
}

struct Sf2BlockPrep {
  uint32_t stepFp;
  float panGainL;
  float panGainR;
  float ampBaseScale;
  float qGain;
  int startIdx;
  int aaMaxIdx;
  uint8_t effLoop;
  bool isStereo;
  bool useAdsrOverride;
  bool useHq;
  bool userFltShaped;
};

__attribute__((noinline)) static bool sf2PrepareVoiceBlock(
    FmVoice &v, Sf2VoiceRuntime &sv, const Sf2Zone &z,
    const FmPatch &p, const FmInstRuntime &rt,
    const FmInstEnvRates &er,
    int32_t *outL, int32_t *outR, int numFrames,
    Sf2BlockPrep &out) {
  out.isStereo = (z.flags & SF2_FLAG_STEREO) != 0;
  out.effLoop = effectiveZoneLoopMode(z, p);
  out.useAdsrOverride = ((Sf2EnvMode)p.op[0].wave == Sf2EnvMode::CapstanAdsr);
  out.useHq = ((Sf2InterpMode)p.op[1].wave == Sf2InterpMode::HermiteHq);
  out.userFltShaped = (p.flt.amt < 127u || p.flt.attack > 0u || p.flt.sustain < 127u || p.flt.release < 100u);

  const int instSlot = constrain((int)v.instIdx, 0, NUM_INSTRUMENTS - 1);
  const uint8_t defOvrMask = sf2Slots[instSlot].defOverrideMask;
  const float modWheelBoost = ((defOvrMask & SF2_DEF_OVR_MODWHEEL_VIB) == 0u)
                                  ? ((float)sf2CcVal[instSlot][1] * (1.0f / 127.0f))
                                  : 0.0f;

  // Block-rate Vibrato / Mod LFO + Mod Wheel (CC1 -> +50 cents default SF2.01 vibrato unless overridden)
  const float totalAuthVib = (float)sv.lfoPitchCents + (float)sv.liveVibPitchCents;
  const bool wantDelayedLfo = (totalAuthVib != 0.0f) || (sv.lfoToFltCents != 0) || (sv.lfoToVolCb != 0);
  float vibCents = 0.0f;
  float lfoTremMul = 1.0f;
  sv.curLfoFltCents = 0;
  if (wantDelayedLfo || modWheelBoost > 0.001f) {
    if (wantDelayedLfo) {
      if (sv.lfoDelayBlocks > 0u) {
        sv.lfoDelayBlocks--;
      } else if (sv.lfoEnv < 1.0f) {
        sv.lfoEnv = min(1.0f, sv.lfoEnv + 0.08f);
      }
    }
    uint32_t lfoIdx = sv.lfoPhase >> (32u - INST_LUT_BITS);
    sv.lfoPhase += sv.lfoStepPerBlock;
    float lfoSin = instSinLutAt(lfoIdx);
    float dlySin = lfoSin * sv.lfoEnv;
    vibCents = totalAuthVib * dlySin + modWheelBoost * 50.0f * lfoSin;
    if (sv.lfoToFltCents != 0) {
      sv.curLfoFltCents = (int16_t)((float)sv.lfoToFltCents * dlySin);
    }
    if (sv.lfoToVolCb != 0) {
      lfoTremMul = sf2FastDbToLin(-(float)sv.lfoToVolCb * 0.1f * dlySin);
    }
  }

  // Combine baseStepFp with patch TUNE (-24.00..+24.00 st), Pitch Modulators, ModEnv->Pitch, Vibrato LFO, & Pitch Bend
  int tuneX100 = constrain((int)p.op[0].ratioX100, (int)SF2_TUNE_MIN, (int)SF2_TUNE_MAX) - (int)SF2_TUNE_ZERO;
  float modEnvPitch = (!out.useAdsrOverride && sv.modPitchCents != 0)
                          ? ((float)sv.modPitchCents * sv.modEnv)
                          : 0.0f;
  float totalTuneCents = (float)tuneX100 + sv.livePitchCents + vibCents + modEnvPitch;
  float pitchMul = ((defOvrMask & SF2_DEF_OVR_PITCHWHEEL) == 0u) ? rt.pitchBendRatio : 1.0f;
  if (totalTuneCents != 0.0f) {
    pitchMul *= sf2FastExp2(constrain(totalTuneCents * (1.0f / 1200.0f), -10.0f, 4.0f));
  }
  const uint32_t stepFp = (uint32_t)constrain((int64_t)((float)sv.baseStepFp * pitchMul + 0.5f),
                                              (int64_t)1, (int64_t)(16 * 65536));
  out.stepFp = stepFp;

  // Dynamic pitch-dependent anti-aliasing & low-rate sample imaging ceiling (integer __builtin_clz)
  int aaMaxIdx = 127;
  int lz = __builtin_clz(stepFp);
  int log2StepX256 = ((15 - lz) << 8) + (int)(((stepFp << lz) >> 23) & 0xFFu);
  if (log2StepX256 > 8) {
    aaMaxIdx = constrain(126 - ((log2StepX256 * 10) >> 8), 48, 127);
  } else if (log2StepX256 < -32) {
    aaMaxIdx = constrain(128 + ((log2StepX256 * 13) >> 8), 32, 127);
  }
  out.aaMaxIdx = aaMaxIdx;

  // Constant-power sin/cos pan for mono zones; cosine stereo balance for stereo linked zones
  int totalPan = constrain((int)sv.zonePan + (int)sv.livePanOff + (int)(int8_t)p.op[1].reserved[2], -100, 100);
  float panGainL = 1.0f;
  float panGainR = 1.0f;
  if (totalPan != 0) {
    if (!out.isStereo) {
      uint32_t angIdx = (uint32_t)(((totalPan + 100) * 256 + 100) / 200);
      panGainL = 1.41421356f * instSinLutAt(256u - angIdx);
      panGainR = 1.41421356f * instSinLutAt(angIdx);
    } else if (totalPan > 0) {
      panGainL = instSinLutAt(256u - (uint32_t)((totalPan * 256 + 50) / 100));
    } else {
      panGainR = instSinLutAt(256u - (uint32_t)(((-totalPan) * 256 + 50) / 100));
    }
  }
  out.panGainL = panGainL;
  out.panGainR = panGainR;

  constexpr float SF2_VOICE_BASE_GAIN = 0.30f;
  out.ampBaseScale = er.ampAmt * (sv.zoneGain * sv.liveGainMul * lfoTremMul * SF2_VOICE_BASE_GAIN) * 32768.0f;
  out.qGain = out.useAdsrOverride ? 1.0f : sv.fltQGain;

  int startIdx = (v.startSampleOffset < (uint8_t)numFrames) ? (int)v.startSampleOffset : 0;
  v.startSampleOffset = 0;
  out.startIdx = startIdx;

  // Continue any stolen-voice 32-sample declick ramp from sample 0 across pre-offset samples
  for (int k = 0; k < startIdx && sv.declickRem > 0; k++) {
    int32_t w = (int32_t)sv.declickRem;
    outL[k] += ((int32_t)sv.declickL * w) >> 5;
    outR[k] += ((int32_t)sv.declickR * w) >> 5;
    sv.declickRem--;
  }

  if (out.ampBaseScale <= 1.5f && sv.declickRem == 0) {
    retireSf2VoiceInIsr(v, sv);
    return false;
  }
  return true;
}

__attribute__((noinline)) static void sf2UpdateVoiceFilter16(
    FmVoice &v, Sf2VoiceRuntime &sv, const FmInstEnvRates &er,
    bool useAdsrOverride, bool userFltShaped, int aaMaxIdx,
    float &curAlpha, float &curEffFbG) {
  int fltIdx = 127;
  float baseFbG = sv.fltFbG;
  if (useAdsrOverride) {
    float fltMod = constrain(er.fltAmt * v.fltEnv, 0.0f, 1.0f);
    if (fltMod < 0.995f) {
      fltIdx = (int)(30.0f + fltMod * 97.0f);
      if (baseFbG < 0.20f) baseFbG = 0.20f;
    }
  } else {
    if ((sv.modFltCents != 0.0f || sv.modPitchCents != 0) && sv.modStage != EnvStage::Idle) {
      if (v.ampStage == EnvStage::Release) {
        sv.modStage = EnvStage::Release;
      }
      instAdvanceEnvelope(sv.modStage, sv.modEnv,
                          sv.modAtkInc16, sv.modDecMul16, sv.modSusLvl, sv.modRelMul16);
    }
    float fcCents = sv.baseFltCents + (float)sv.liveFltCents + (float)sv.curLfoFltCents + sv.modFltCents * sv.modEnv;
    if (userFltShaped) {
      float userFltNorm = constrain(er.fltAmt * v.fltEnv, 0.0f, 1.0f);
      fcCents += (userFltNorm - 1.0f) * 9600.0f;
    }
    if (fcCents < 13460.0f || baseFbG > 0.0f) {
      fltIdx = constrain((int)((fcCents - 1500.0f) * (127.0f / 12000.0f) + 0.5f), 0, 127);
      if (baseFbG > 0.0f && fltIdx > 125) fltIdx = 125;
    }
  }
  if (fltIdx > aaMaxIdx) {
    fltIdx = aaMaxIdx;
  }
  curAlpha = sf2FilterAlphaLut[fltIdx];
  curEffFbG = baseFbG * (1.0f - curAlpha);
}

// Hot Audio ISR Voice Renderer (ITCM)
static void sf2RenderVoice(FmVoice &v, const FmPatch &p, const FmInstRuntime &rt,
                           int32_t *outL, int32_t *outR, int numFrames) {
  const int voiceIdx = (int)(&v - fmSynthVoiceAt(0));
  if ((unsigned)voiceIdx >= (unsigned)NUM_FM_VOICES) {
    v.active = false;
    return;
  }

  Sf2VoiceRuntime &sv = sf2Voices[voiceIdx];
  if (sv.zonePoolIdx >= SF2_MAX_ZONES) {
    v.active = false;
    v.ampStage = EnvStage::Idle;
    return;
  }
  const Sf2Zone &z = sf2ZonePool[sv.zonePoolIdx];
  const FmInstEnvRates &er = instCachedEnvRates(v.instIdx);
  Sf2BlockPrep bp;
  if (!sf2PrepareVoiceBlock(v, sv, z, p, rt, er, outL, outR, numFrames, bp)) {
    return;
  }

  const bool isStereo = bp.isStereo;
  const uint8_t effLoop = bp.effLoop;
  const bool useAdsrOverride = bp.useAdsrOverride;
  const bool useHq = bp.useHq;
  const bool userFltShaped = bp.userFltShaped;
  const uint32_t stepFp = bp.stepFp;
  const float panGainL = bp.panGainL;
  const float panGainR = bp.panGainR;
  const float ampBaseScale = bp.ampBaseScale;
  const float qGain = bp.qGain;
  const int aaMaxIdx = bp.aaMaxIdx;

  float curAlpha = 1.0f;
  float curEffFbG = 0.0f;
  uint8_t fltSub = 0u;

  for (int i = bp.startIdx; i < numFrames; i++) {
    const bool held = v.gate || v.sustained;

    if (useAdsrOverride) {
      if (sv.choked) {
        if (userFltShaped) {
          instAdvanceEnvelope(v.fltStage, v.fltEnv, er.fltAtkInc, er.fltDecMul, er.fltSusLvl, er.fltRelMul);
        }
        instAdvanceEnvelope(v.ampStage, v.ampEnv, er.ampAtkInc, er.ampDecMul, er.ampSusLvl, 0.992f);
        if (v.ampStage == EnvStage::Idle) {
          retireSf2VoiceInIsr(v, sv);
          sv.declickRem = 0;
          break;
        }
      } else if (!instAdvanceVoiceEnvelopes(v)) {
        retireSf2VoiceInIsr(v, sv);
        sv.declickRem = 0;
        break;
      }
    } else {
      // 1. SF2 delayVolEnv stage: hold silent before Attack begins (O(1) fast-skip once declick finishes)
      if (sv.envDelayRem > 0u) {
        if (v.ampStage == EnvStage::Release && !held && sv.declickRem == 0u) {
          retireSf2VoiceInIsr(v, sv);
          break;
        }
        if (sv.declickRem > 0u) {
          int32_t w = (int32_t)sv.declickRem;
          outL[i] += ((int32_t)sv.declickL * w) >> 5;
          outR[i] += ((int32_t)sv.declickR * w) >> 5;
          sv.declickRem--;
          sv.envDelayRem--;
          continue;
        }
        uint32_t skip = min((uint32_t)(numFrames - i), sv.envDelayRem);
        sv.envDelayRem -= skip;
        i += (int)skip - 1;
        continue;
      }

      // 2. Advance user FLT envelope when shaped & SF2 Hold + DAHDSR Volume Envelope
      if (userFltShaped) {
        instAdvanceEnvelope(v.fltStage, v.fltEnv, er.fltAtkInc, er.fltDecMul, er.fltSusLvl, er.fltRelMul);
      }
      if (v.ampStage == EnvStage::Decay && sv.envHoldRem > 0u) {
        sv.envHoldRem--;
        v.ampEnv = 1.0f;
      } else {
        instAdvanceEnvelope(v.ampStage, v.ampEnv, sv.ampAtkInc, sv.ampDecMul, sv.ampSusLvl, sv.ampRelMul);
      }
      if (v.ampStage == EnvStage::Idle ||
          (v.ampStage == EnvStage::Sustain && v.ampEnv <= 0.0005f && !v.gate)) {
        retireSf2VoiceInIsr(v, sv);
        sv.declickRem = 0;
        break;
      }
    }

    // 3. 16-sample control-rate ModEnv + Resonant SVF + Anti-Aliasing cutoff update (~2.76 kHz)
    if ((fltSub++ & 15u) == 0u) {
      sf2UpdateVoiceFilter16(v, sv, er, useAdsrOverride, userFltShaped, aaMaxIdx, curAlpha, curEffFbG);
    }

    // 4. Sample fetch (RAM1 Pre-Roll or Ring-Locked SDIO Stream) into unified 4-point window
    const float frac = (float)(sv.fracFp & 0xFFFFu) * (1.0f / 65536.0f);
    int16_t ym1L = sv.prevL, y0L = 0, y1L = 0, y2L = 0;
    int16_t ym1R = sv.prevR, y0R = 0, y1R = 0, y2R = 0;
    bool holdLast = false;
    bool hitEofAfterSample = false;

    if (sv.inPreRoll && sv.preRollHold) {
      if (sv.streamReady && v.opPhase[0] < SHARED_NUM_STREAMS) {
        SamplerStream *st = samplerStreamSlot(v.opPhase[0]);
        if (st && st->active && st->generation == v.opPhase[1]) {
          sv.prevL = sf2PreRoll[z.preRollSlot][SF2_PREROLL_LEN - 1u];
          if (isStereo) sv.prevR = sf2PreRoll[z.preRollSlot + 1][SF2_PREROLL_LEN - 1u];
          sv.inPreRoll = false;
          sv.preRollHold = false;
        } else if (!sv.wantStream) {
          retireSf2VoiceInIsr(v, sv);
          break;
        }
      } else if (!sv.wantStream) {
        retireSf2VoiceInIsr(v, sv);
        break;
      }
    }

    if (sv.inPreRoll) {
      if (sv.preRollHold) {
        holdLast = true;
      } else {
        const uint32_t pos = sv.preRollPos;
        const int16_t *prL = sf2PreRoll[z.preRollSlot];
        const bool loopInPre = (effLoop != 0u) && (held || effLoop == 1u) &&
                               (z.loopEnd > z.loopStart) && (z.loopEnd <= SF2_PREROLL_LEN);
        uint32_t idx1 = pos + 1u;
        uint32_t idx2 = pos + 2u;
        if (loopInPre && idx2 >= z.loopEnd) {
          const uint32_t loopLen = z.loopEnd - z.loopStart;
          if (idx1 >= z.loopEnd) {
            idx1 = z.loopStart;
            idx2 = (loopLen > 1u) ? (z.loopStart + 1u) : z.loopStart;
          } else {
            idx2 = z.loopStart;
          }
        }
        y0L = prL[pos];
        y1L = prL[idx1];
        y2L = prL[idx2];
        const int16_t *prR = prL;
        if (isStereo) {
          prR = sf2PreRoll[z.preRollSlot + 1];
          y0R = prR[pos];
          y1R = prR[idx1];
          y2R = prR[idx2];
        }

        sv.fracFp += stepFp;
        uint32_t adv = sv.fracFp >> 16;
        sv.fracFp &= 0xFFFFu;

        if (adv > 0u) {
          uint32_t nextPos = pos + adv;
          if (loopInPre) {
            uint32_t loopLen = z.loopEnd - z.loopStart;
            if (nextPos >= z.loopEnd) {
              nextPos = z.loopStart + (nextPos - z.loopStart) % loopLen;
            }
            uint32_t prevIdx = (nextPos > z.loopStart) ? (nextPos - 1u) : (z.loopEnd - 1u);
            sv.prevL = prL[prevIdx];
            if (isStereo) sv.prevR = prR[prevIdx];
            sv.preRollPos = nextPos;
          } else if (sv.totalFrames <= SF2_PREROLL_LEN) {
            if (nextPos >= sv.totalFrames) {
              hitEofAfterSample = true;
            } else {
              sv.prevL = prL[nextPos - 1u];
              if (isStereo) sv.prevR = prR[nextPos - 1u];
              sv.preRollPos = nextPos;
            }
          } else if (nextPos < SF2_PREROLL_LEN) {
            sv.prevL = prL[nextPos - 1u];
            if (isStereo) sv.prevR = prR[nextPos - 1u];
            sv.preRollPos = nextPos;
          } else if (sv.streamReady && v.opPhase[0] < SHARED_NUM_STREAMS) {
            SamplerStream *st = samplerStreamSlot(v.opPhase[0]);
            if (st && st->active && st->generation == v.opPhase[1]) {
              const uint32_t over = nextPos - SF2_PREROLL_LEN;
              const uint32_t ringMask = isStereo ? (SHARED_STEREO_RING_FRAMES - 1u) : (SHARED_MONO_RING_FRAMES - 1u);
              const uint32_t oldTail = st->tail;
              const uint32_t availSt = st->head - oldTail;
              const uint32_t stepOver = min(over, availSt);
              if (stepOver > 0u) {
                uint32_t prevIdx = oldTail + stepOver - 1u;
                if (isStereo) {
                  uint32_t pm = st->ring[prevIdx & ringMask];
                  sv.prevL = (int16_t)(pm & 0xFFFFu);
                  sv.prevR = (int16_t)(pm >> 16);
                } else {
                  const int16_t *mRing = reinterpret_cast<const int16_t *>(st->ring);
                  sv.prevL = mRing[prevIdx & ringMask];
                  sv.prevR = sv.prevL;
                }
              } else {
                sv.prevL = prL[SF2_PREROLL_LEN - 1u];
                if (isStereo) sv.prevR = prR[SF2_PREROLL_LEN - 1u];
              }
              sv.inPreRoll = false;
              sv.preRollHold = false;
              st->tail = oldTail + stepOver;
            } else if (!sv.wantStream) {
              hitEofAfterSample = true;
            } else {
              sv.preRollPos = SF2_PREROLL_LEN - 1u;
              sv.fracFp = 0u;
              sv.preRollHold = true;
            }
          } else if (!sv.wantStream) {
            hitEofAfterSample = true;
          } else {
            sv.preRollPos = SF2_PREROLL_LEN - 1u;
            sv.fracFp = 0u;
            sv.preRollHold = true;
          }
        }
      }
    } else {
      SamplerStream *st = samplerStreamSlot(v.opPhase[0]);
      if (!st || !st->active || st->generation != v.opPhase[1]) {
        retireSf2VoiceInIsr(v, sv);
        break;
      }

      if (st->loopLockedInRing && effLoop == 3u && !held) {
        st->loopLockedInRing = false;
      }

      const uint32_t ringMask = isStereo ? (SHARED_STEREO_RING_FRAMES - 1u) : (SHARED_MONO_RING_FRAMES - 1u);
      uint32_t t = st->tail;
      uint32_t h = st->head;
      const bool locked = st->loopLockedInRing && (st->ringLoopLen > 0u);
      uint32_t avail = locked ? (uint32_t)st->ringLoopLen : (h - t);

      if (avail == 0u) {
        if (st->eofReached) {
          retireSf2VoiceInIsr(v, sv);
          break;
        }
        holdLast = true;
      } else {
        uint32_t idx1 = locked ? (t + 1u) : ((avail >= 2u) ? (t + 1u) : t);
        uint32_t idx2 = locked ? (t + 2u) : ((avail >= 3u) ? (t + 2u) : idx1);
        if (isStereo) {
          uint32_t p0 = st->ring[t & ringMask];
          uint32_t p1 = st->ring[idx1 & ringMask];
          uint32_t p2 = st->ring[idx2 & ringMask];
          y0L = (int16_t)(p0 & 0xFFFFu); y0R = (int16_t)(p0 >> 16);
          y1L = (int16_t)(p1 & 0xFFFFu); y1R = (int16_t)(p1 >> 16);
          y2L = (int16_t)(p2 & 0xFFFFu); y2R = (int16_t)(p2 >> 16);
        } else {
          const int16_t *mRing = reinterpret_cast<const int16_t *>(st->ring);
          y0L = mRing[t & ringMask];
          y1L = mRing[idx1 & ringMask];
          y2L = mRing[idx2 & ringMask];
        }

        sv.fracFp += stepFp;
        uint32_t adv = sv.fracFp >> 16;
        sv.fracFp &= 0xFFFFu;

        if (adv > 0u) {
          uint32_t nextT;
          uint32_t prevT;
          if (locked) {
            uint32_t ls = st->ringLoopStart;
            uint32_t ll = st->ringLoopLen;
            nextT = t + adv;
            while (nextT >= ls + ll) {
              nextT -= ll;
            }
            prevT = (nextT > ls) ? (nextT - 1u) : (ls + ll - 1u);
          } else {
            if (adv > avail) adv = avail;
            nextT = t + adv;
            prevT = nextT - 1u;
          }
          if (isStereo) {
            uint32_t pm = st->ring[prevT & ringMask];
            sv.prevL = (int16_t)(pm & 0xFFFFu);
            sv.prevR = (int16_t)(pm >> 16);
          } else {
            const int16_t *mRing = reinterpret_cast<const int16_t *>(st->ring);
            sv.prevL = mRing[prevT & ringMask];
          }
          st->tail = nextT;
        }
      }
    }

    // 5. Unified Interpolation + Resonant 2-Pole SVF (Mono Pre-Pan Fast Path vs Stereo)
    float iL = holdLast ? (float)sv.lastL
                        : evalSf2Interp((float)ym1L, (float)y0L, (float)y1L, (float)y2L, frac, useHq);
    sv.lastL = (int16_t)constrain((int32_t)iL, -32768, 32767);
    float rawL = iL * (1.0f / 32768.0f);

    const float envAmpScale = ampBaseScale * v.ampEnv;
    int32_t sOutL = 0;
    int32_t sOutR = 0;

    if (!isStereo) {
      float filtM = rawL;
      if (curAlpha < 0.999f) {
        filtM = stepSf2SvfChan(rawL, qGain, curAlpha, curEffFbG, v.fltLp1, v.fltLp2);
      } else {
        v.fltLp1 = rawL;
        v.fltLp2 = rawL;
      }
      float outM = filtM * envAmpScale;
      sOutL = (int32_t)(outM * panGainL);
      sOutR = (int32_t)(outM * panGainR);
    } else {
      float iR = holdLast ? (float)sv.lastR
                          : evalSf2Interp((float)ym1R, (float)y0R, (float)y1R, (float)y2R, frac, useHq);
      sv.lastR = (int16_t)constrain((int32_t)iR, -32768, 32767);
      float rawR = iR * (1.0f / 32768.0f);

      float filtL = rawL;
      float filtR = rawR;
      if (curAlpha < 0.999f) {
        filtL = stepSf2SvfChan(rawL, qGain, curAlpha, curEffFbG, v.fltLp1, v.fltLp2);
        filtR = stepSf2SvfChan(rawR, qGain, curAlpha, curEffFbG, v.opPrev[0], v.opPrev[1]);
      } else {
        v.fltLp1 = rawL;
        v.fltLp2 = rawL;
        v.opPrev[0] = rawR;
        v.opPrev[1] = rawR;
      }
      sOutL = (int32_t)(filtL * envAmpScale * panGainL);
      sOutR = (int32_t)(filtR * envAmpScale * panGainR);
    }

    if (sv.declickRem > 0) {
      int32_t w = (int32_t)sv.declickRem;
      sOutL += ((int32_t)sv.declickL * w) >> 5;
      sOutR += ((int32_t)sv.declickR * w) >> 5;
      sv.declickRem--;
    }
    sv.prevOutL = (int16_t)constrain(sOutL, -32768, 32767);
    sv.prevOutR = (int16_t)constrain(sOutR, -32768, 32767);
    outL[i] += sOutL;
    outR[i] += sOutR;

    if (hitEofAfterSample) {
      retireSf2VoiceInIsr(v, sv);
      break;
    }
  }
}

// ============================================================================
// SD Card Persistence ("SF21" in INST.DAT & "SF1P" in .FMI via RAM1 Staging)
// ============================================================================
struct DiskSf2SongHeader {
  char magic[4];    // "SF21"
  uint16_t version; // 1
  uint16_t slotMask;
};

struct DiskSf2SlotRecord {
  uint8_t slotIdx;
  uint8_t reserved;
  uint16_t bank;
  uint16_t preset;
  char dir[SAMPLER_DIR_MAX];
  char file[SAMPLER_FILE_MAX];
};

struct DiskSf2SingleHeader {
  char magic[4];    // "SF1P"
  uint16_t version; // 1
  uint16_t bank;
  uint16_t preset;
  uint16_t reserved;
  char dir[SAMPLER_DIR_MAX];
  char file[SAMPLER_FILE_MAX];
};

FLASHMEM bool sf2SaveSongData(FsFile &f) {
  if (!f.isOpen()) return false;
  uint16_t mask = 0;
  for (int i = 0; i < NUM_INSTRUMENTS; i++) {
    if ((InstType)fmSynthPatch(i).type == InstType::SoundFont && sf2SlotFile[i][0] != '\0') {
      mask |= (uint16_t)(1u << i);
    }
  }
  if (mask == 0) return true;

  DiskSf2SongHeader hdr{};
  memcpy(hdr.magic, "SF21", 4);
  hdr.version = 1;
  hdr.slotMask = mask;
  if (f.write(&hdr, sizeof(hdr)) != (int)sizeof(hdr)) return false;

  for (int i = 0; i < NUM_INSTRUMENTS; i++) {
    if ((mask & (1u << i)) == 0) continue;
    alignas(4) uint8_t pairs[64] = {};
    uint8_t pairCount = sf2CollectModifiedCcPairs(i, pairs);
    DiskSf2SlotRecord rec{};
    rec.slotIdx  = (uint8_t)i;
    rec.reserved = pairCount;
    rec.bank     = sf2Slots[i].curBank;
    rec.preset   = sf2Slots[i].curPresetNum;
    memcpy(rec.dir,  sf2SlotDir[i],  sizeof(rec.dir));
    memcpy(rec.file, sf2SlotFile[i], sizeof(rec.file));
    if (f.write(&rec, sizeof(rec)) != (int)sizeof(rec)) return false;
    if (pairCount > 0u) {
      size_t pairBytes = (size_t)pairCount * 2u;
      if (f.write(pairs, pairBytes) != pairBytes) return false;
    }
  }
  return true;
}

FLASHMEM bool sf2LoadSongData(FsFile &f) {
  for (int i = 0; i < NUM_INSTRUMENTS; i++) {
    sf2ClearSlot(i);
  }
  if (!f.isOpen()) return false;

  uint64_t startPos = f.curPosition();
  DiskSf2SongHeader hdr{};
  int rd = f.read(&hdr, sizeof(hdr));
  if (rd != (int)sizeof(hdr)) {
    return true;
  }
  if (memcmp(hdr.magic, "SF21", 4) != 0 || hdr.version != 1) {
    f.seekSet(startPos);
    return true;
  }

  for (int i = 0; i < NUM_INSTRUMENTS; i++) {
    if ((hdr.slotMask & (1u << i)) == 0) continue;
    DiskSf2SlotRecord rec{};
    if (f.read(&rec, sizeof(rec)) != (int)sizeof(rec)) break;
    alignas(4) uint8_t pairs[64] = {};
    uint8_t savedPairs = rec.reserved;
    uint8_t rdPairs = min(savedPairs, (uint8_t)32u);
    if (rdPairs > 0u) {
      size_t rdBytes = (size_t)rdPairs * 2u;
      if (f.read(pairs, rdBytes) != (int)rdBytes) break;
    }
    if (savedPairs > 32u) {
      uint64_t skipPos = f.curPosition() + (uint64_t)(savedPairs - 32u) * 2ULL;
      if (!f.seekSet(skipPos)) break;
    }
    rec.dir[SAMPLER_DIR_MAX - 1] = '\0';
    rec.file[SAMPLER_FILE_MAX - 1] = '\0';
    int dst = (int)rec.slotIdx;
    if (dst >= 0 && dst < NUM_INSTRUMENTS &&
        (InstType)fmSynthPatch(dst).type == InstType::SoundFont &&
        rec.file[0] != '\0') {
      char fullPath[SAMPLER_PATH_MAX];
      if (rec.file[0] == '/') {
        snprintf(fullPath, sizeof(fullPath), "%s", rec.file);
      } else {
        const char *d = rec.dir[0] ? rec.dir : SOUNDFONTS_DIR;
        snprintf(fullPath, sizeof(fullPath), (strcmp(d, "/") == 0) ? "/%s" : "%s/%s", d, rec.file);
      }
      if (sf2AssignFile(dst, fullPath, (int)rec.bank, (int)rec.preset) && rdPairs > 0u) {
        sf2ApplySavedCcPairs(dst, pairs, rdPairs);
      }
    }
  }
  return true;
}

FLASHMEM bool sf2SaveSingleInstData(FsFile &f, int instIdx) {
  if (!f.isOpen() || instIdx < 0 || instIdx >= NUM_INSTRUMENTS) return false;
  if ((InstType)fmSynthPatch(instIdx).type != InstType::SoundFont) return true;
  if (sf2SlotFile[instIdx][0] == '\0') return true;

  alignas(4) uint8_t pairs[64] = {};
  uint8_t pairCount = sf2CollectModifiedCcPairs(instIdx, pairs);
  DiskSf2SingleHeader rec{};
  memcpy(rec.magic, "SF1P", 4);
  rec.version  = 1;
  rec.bank     = sf2Slots[instIdx].curBank;
  rec.preset   = sf2Slots[instIdx].curPresetNum;
  rec.reserved = (uint16_t)pairCount;
  memcpy(rec.dir,  sf2SlotDir[instIdx],  sizeof(rec.dir));
  memcpy(rec.file, sf2SlotFile[instIdx], sizeof(rec.file));
  if (f.write(&rec, sizeof(rec)) != (int)sizeof(rec)) return false;
  if (pairCount > 0u) {
    size_t pairBytes = (size_t)pairCount * 2u;
    if (f.write(pairs, pairBytes) != pairBytes) return false;
  }
  return true;
}

FLASHMEM bool sf2LoadSingleInstData(FsFile &f, int instIdx) {
  if (instIdx < 0 || instIdx >= NUM_INSTRUMENTS) return false;
  sf2ClearSlot(instIdx);
  if (!f.isOpen()) return false;
  if ((InstType)fmSynthPatch(instIdx).type != InstType::SoundFont) return true;

  uint64_t startPos = f.curPosition();
  DiskSf2SingleHeader rec{};
  int rd = f.read(&rec, sizeof(rec));
  if (rd != (int)sizeof(rec) || memcmp(rec.magic, "SF1P", 4) != 0 || rec.version != 1) {
    if (rd > 0) f.seekSet(startPos);
    return true;
  }
  alignas(4) uint8_t pairs[64] = {};
  uint16_t savedPairs = rec.reserved;
  uint8_t rdPairs = (uint8_t)min(savedPairs, (uint16_t)32u);
  if (rdPairs > 0u) {
    size_t rdBytes = (size_t)rdPairs * 2u;
    if (f.read(pairs, rdBytes) != (int)rdBytes) rdPairs = 0u;
  }
  if (savedPairs > 32u) {
    uint64_t skipPos = min((uint64_t)f.fileSize(), f.curPosition() + (uint64_t)(savedPairs - 32u) * 2ULL);
    f.seekSet(skipPos);
  }
  rec.dir[SAMPLER_DIR_MAX - 1] = '\0';
  rec.file[SAMPLER_FILE_MAX - 1] = '\0';
  if (rec.file[0] != '\0') {
    char fullPath[SAMPLER_PATH_MAX];
    if (rec.file[0] == '/') {
      snprintf(fullPath, sizeof(fullPath), "%s", rec.file);
    } else {
      const char *d = rec.dir[0] ? rec.dir : SOUNDFONTS_DIR;
      snprintf(fullPath, sizeof(fullPath), (strcmp(d, "/") == 0) ? "/%s" : "%s/%s", d, rec.file);
    }
    if (sf2AssignFile(instIdx, fullPath, (int)rec.bank, (int)rec.preset) && rdPairs > 0u) {
      sf2ApplySavedCcPairs(instIdx, pairs, rdPairs);
    }
  }
  return true;
}

// ============================================================================
// Unified .sf2 File Browser & Preset Browser Modal (32-Entry Sliding Window)
// ============================================================================
struct Sf2PoolEntry {
  char name[43];
  uint8_t isDir;
  uint16_t bank;
  uint16_t preset;
  uint16_t orderIdx;
  uint16_t phdrIdx;
};
static_assert(sizeof(Sf2PoolEntry) <= 64, "Sf2PoolEntry must fit in 64-byte sharedBrowserPool slot");

static constexpr int MAX_SF2_BROWSER_WINDOW = 32;
static Sf2PoolEntry *const sf2BrowserWin = reinterpret_cast<Sf2PoolEntry *>(sharedBrowserPool);
static Sf2BrowserMode sf2BrMode = Sf2BrowserMode::Closed;
static int sf2BrInstIdx = 0;
static int sf2BrTotalCount = 0;
static int sf2BrWinStart = 0;
static int sf2BrWinCount = 0;
static int sf2BrCursor = 0;
static int sf2BrScroll = 0;
static char sf2BrDir[SAMPLER_DIR_MAX] = "/Capstan/SOUNDFONTS";

FLASHMEM static bool hasSf2Extension(const char *name) {
  if (!name) return false;
  size_t len = strlen(name);
  if (len < 5) return false;
  return strcasecmp(name + len - 4, ".sf2") == 0;
}

FLASHMEM static bool acceptSf2BrowserFile(const char *name, uint64_t) {
  return hasSf2Extension(name);
}

FLASHMEM static bool sf2PresetEntryLess(const Sf2PoolEntry &a, const Sf2PoolEntry &b) {
  return phdrItemLess(a.bank, a.preset, a.phdrIdx, b.bank, b.preset, b.phdrIdx);
}

FLASHMEM static void scanSf2FileWindow(const Sf2PoolEntry *pivot, int pivotIdx, int wantStart,
                                       const char *matchFile = nullptr, int *outMatchIdx = nullptr,
                                       Sf2PoolEntry *outMatchEntry = nullptr) {
  if (!SD.sdfs.exists(ROOT_DIR)) SD.sdfs.mkdir(ROOT_DIR);
  if (!SD.sdfs.exists(SOUNDFONTS_DIR)) SD.sdfs.mkdir(SOUNDFONTS_DIR);
  if (!SD.sdfs.exists(sf2BrDir)) {
    snprintf(sf2BrDir, sizeof(sf2BrDir), "%s", SOUNDFONTS_DIR);
  }
  pagedScanDirWindow<Sf2PoolEntry>(
      sf2BrDir, sf2BrowserWin, MAX_SF2_BROWSER_WINDOW,
      sf2BrTotalCount, sf2BrWinStart, sf2BrWinCount,
      acceptSf2BrowserFile, pivot, pivotIdx, wantStart,
      matchFile, outMatchIdx, outMatchEntry);
}

FLASHMEM static void scanSf2PresetWindow(const Sf2PoolEntry *pivot, int pivotIdx, int wantStart,
                                         int matchBank = -1, int matchPreset = -1,
                                         int *outMatchIdx = nullptr, Sf2PoolEntry *outMatchEntry = nullptr) {
  if (outMatchIdx) *outMatchIdx = -1;
  sf2BrTotalCount = 0;
  sf2BrWinStart = 0;
  sf2BrWinCount = 0;

  if (sf2BrInstIdx < 0 || sf2BrInstIdx >= NUM_INSTRUMENTS || !sf2Slots[sf2BrInstIdx].valid) return;
  const Sf2HydraIndex &hydra = sf2Slots[sf2BrInstIdx].hydra;
  if (hydra.phdrCount == 0) return;

  char fullPath[SAMPLER_PATH_MAX];
  resolveSf2FullPath(sf2BrInstIdx, fullPath, sizeof(fullPath));
  FsFile f = SD.sdfs.open(fullPath, O_RDONLY);
  if (!f.isOpen()) return;

  Sf2PoolEntry pivotCopy{};
  const bool usePivot = (pivot != nullptr);
  int pOff = 0, leftCap = 0, rightCap = MAX_SF2_BROWSER_WINDOW;
  int leftCount = 0, rightCount = 0, totalCount = 0;
  if (usePivot) {
    pivotCopy = *pivot;
    pOff = constrain(pivotIdx - wantStart, 0, MAX_SF2_BROWSER_WINDOW - 1);
    leftCap = pOff;
    rightCap = (MAX_SF2_BROWSER_WINDOW - 1) - pOff;
  }

  const bool hasMatch = (matchBank >= 0 && matchPreset >= 0);
  int matchRank = 0;
  bool matchFound = false;
  uint16_t matchedPhdrIdx = 0xFFFFu;

  uint8_t *buf = reinterpret_cast<uint8_t *>(storageSharedChunkBuf());
  static constexpr uint16_t BATCH = 128;

  for (uint16_t base = 0; base < hydra.phdrCount; base += BATCH) {
    uint16_t cnt = min((uint16_t)(hydra.phdrCount - base), BATCH);
    size_t rdBytes = (size_t)(cnt + 1u) * 38u;
    if (!f.seekSet((uint64_t)hydra.phdrOffset + (uint64_t)base * 38ULL)) break;
    if (f.read(buf, rdBytes) != (int)rdBytes) break;

    for (uint16_t i = 0; i < cnt; i++) {
      const uint8_t *rec = buf + i * 38u;
      uint16_t pbag0 = rdU16Le(rec + 24);
      uint16_t pbag1 = rdU16Le(rec + 38u + 24);
      if (pbag1 <= pbag0 || pbag1 > hydra.pbagCount) continue;

      Sf2PoolEntry item{};
      char pName[21];
      sanitizeSf2Label(rec, pName);
      snprintf(item.name, sizeof(item.name), "%s", pName);
      item.isDir   = 0;
      item.preset  = rdU16Le(rec + 20);
      item.bank    = rdU16Le(rec + 22);
      item.phdrIdx = base + i;

      totalCount++;
      if (hasMatch) {
        if (!matchFound && item.bank == (uint16_t)matchBank && item.preset == (uint16_t)matchPreset) {
          matchFound = true;
          matchedPhdrIdx = item.phdrIdx;
          if (outMatchEntry) *outMatchEntry = item;
        }
      }
      if (!usePivot) {
        pagedInsertSmallest(sf2BrowserWin, rightCount, rightCap, item, sf2PresetEntryLess);
      } else if (sf2PresetEntryLess(item, pivotCopy)) {
        pagedInsertLargest(sf2BrowserWin, leftCount, leftCap, item, sf2PresetEntryLess);
      } else if (sf2PresetEntryLess(pivotCopy, item)) {
        pagedInsertSmallest(&sf2BrowserWin[pOff + 1], rightCount, rightCap, item, sf2PresetEntryLess);
      }
    }
    keepStreaming();
  }

  if (matchFound && outMatchIdx) {
    // Count exact sorted rank of matchedPhdrIdx
    for (uint16_t base = 0; base < hydra.phdrCount; base += BATCH) {
      uint16_t cnt = min((uint16_t)(hydra.phdrCount - base), BATCH);
      size_t rdBytes = (size_t)(cnt + 1u) * 38u;
      if (!f.seekSet((uint64_t)hydra.phdrOffset + (uint64_t)base * 38ULL)) break;
      if (f.read(buf, rdBytes) != (int)rdBytes) break;
      for (uint16_t i = 0; i < cnt; i++) {
        uint16_t gIdx = base + i;
        if (gIdx == matchedPhdrIdx) continue;
        const uint8_t *rec = buf + i * 38u;
        uint16_t pbag0 = rdU16Le(rec + 24);
        uint16_t pbag1 = rdU16Le(rec + 38u + 24);
        if (pbag1 <= pbag0 || pbag1 > hydra.pbagCount) continue;
        if (phdrItemLess(rdU16Le(rec + 22), rdU16Le(rec + 20), gIdx,
                         (uint16_t)matchBank, (uint16_t)matchPreset, matchedPhdrIdx)) {
          matchRank++;
        }
      }
    }
    *outMatchIdx = matchRank;
  }

  f.close();

  pagedFinalizeWindow(sf2BrowserWin, usePivot, pivotCopy, pivotIdx, pOff, leftCount, rightCount,
                      totalCount, sf2BrTotalCount, sf2BrWinStart, sf2BrWinCount);
}

FLASHMEM static void rescanSf2BrowserWindow(const Sf2PoolEntry *pivot, int pivotIdx, int wantStart) {
  if (sf2BrMode == Sf2BrowserMode::PresetList) {
    scanSf2PresetWindow(pivot, pivotIdx, wantStart);
  } else {
    scanSf2FileWindow(pivot, pivotIdx, wantStart);
  }
}

FLASHMEM static void syncSf2BrowserScroll() {
  pagedSyncWindowScroll(sf2BrTotalCount, SF2_BROWSER_VISIBLE_ROWS, MAX_SF2_BROWSER_WINDOW,
                        sf2BrCursor, sf2BrScroll, sf2BrWinStart, sf2BrWinCount,
                        sf2BrowserWin, rescanSf2BrowserWindow);
}

FLASHMEM bool isSf2BrowserOpen() {
  return sf2BrMode != Sf2BrowserMode::Closed;
}

FLASHMEM Sf2BrowserMode sf2BrowserMode() {
  return sf2BrMode;
}

FLASHMEM int sf2BrowserTargetSlot() {
  return sf2BrInstIdx;
}

FLASHMEM const char *sf2BrowserCurrentDir() {
  return sf2BrDir;
}

FLASHMEM int sf2BrowserEntryCount() {
  return sf2BrTotalCount;
}

FLASHMEM int sf2BrowserCursor() {
  return sf2BrCursor;
}

FLASHMEM int sf2BrowserScroll() {
  return sf2BrScroll;
}

FLASHMEM bool sf2BrowserRowAt(int idx, Sf2BrowserRowInfo &outRow) {
  outRow = Sf2BrowserRowInfo{};
  if (idx < sf2BrWinStart || idx >= sf2BrWinStart + sf2BrWinCount) return false;
  const Sf2PoolEntry &ent = sf2BrowserWin[idx - sf2BrWinStart];
  outRow.valid = true;
  outRow.isDir = (ent.isDir != 0);
  if (sf2BrMode == Sf2BrowserMode::PresetList) {
    snprintf(outRow.label, sizeof(outRow.label), "%03u:%03u %.34s",
             (unsigned)min(ent.bank, (uint16_t)999u),
             (unsigned)min(ent.preset, (uint16_t)999u),
             ent.name);
    snprintf(outRow.rightBadge, sizeof(outRow.rightBadge), "LOAD");
  } else {
    snprintf(outRow.label, sizeof(outRow.label), "%s", ent.name);
    snprintf(outRow.rightBadge, sizeof(outRow.rightBadge), ent.isDir ? "OPEN" : "LOAD");
  }
  return true;
}

FLASHMEM void openSf2FileBrowser(int instIdx) {
  closeSf2Browser();
  closeSamplerBrowser();
  closeInstBrowser();
  closeSysPresetsModal();
  closeTypePresetModal();
  sf2BrInstIdx = constrain(instIdx, 0, NUM_INSTRUMENTS - 1);
  sf2BrMode = Sf2BrowserMode::FileSelect;
  instPreviewSetSf2PresetModal(false);

  if (!SD.sdfs.exists(ROOT_DIR)) SD.sdfs.mkdir(ROOT_DIR);
  if (!SD.sdfs.exists(SOUNDFONTS_DIR)) SD.sdfs.mkdir(SOUNDFONTS_DIR);

  const char *selectedFile = nullptr;
  char fullAssigned[SAMPLER_PATH_MAX] = {};
  if (sf2HasFile(sf2BrInstIdx)) {
    resolveSf2FullPath(sf2BrInstIdx, fullAssigned, sizeof(fullAssigned));
    char *lastSlash = strrchr(fullAssigned, '/');
    if (lastSlash) {
      selectedFile = lastSlash + 1;
      if (lastSlash == fullAssigned) {
        snprintf(sf2BrDir, sizeof(sf2BrDir), "/");
      } else {
        *lastSlash = '\0';
        if (SD.sdfs.exists(fullAssigned)) {
          snprintf(sf2BrDir, sizeof(sf2BrDir), "%s", fullAssigned);
        }
      }
    }
  } else if (sf2BrDir[0] == '\0' || !SD.sdfs.exists(sf2BrDir)) {
    snprintf(sf2BrDir, sizeof(sf2BrDir), "%s", SOUNDFONTS_DIR);
  }

  int matchedIdx = -1;
  Sf2PoolEntry matchedEntry{};
  sf2BrCursor = 0;
  sf2BrScroll = 0;
  scanSf2FileWindow(nullptr, 0, 0, selectedFile, &matchedIdx, &matchedEntry);

  if (matchedIdx >= 0) {
    sf2BrCursor = matchedIdx;
    if (matchedIdx >= sf2BrWinCount) {
      int maxScroll = max(0, sf2BrTotalCount - SF2_BROWSER_VISIBLE_ROWS);
      sf2BrScroll = constrain(sf2BrCursor - SF2_BROWSER_VISIBLE_ROWS + 1, 0, maxScroll);
      int maxWinStart = max(0, sf2BrTotalCount - MAX_SF2_BROWSER_WINDOW);
      int wantStart = constrain(sf2BrScroll - 12, 0, maxWinStart);
      scanSf2FileWindow(&matchedEntry, matchedIdx, wantStart);
    }
  } else if (sf2BrTotalCount > 1 && sf2BrWinCount > 0 && strcmp(sf2BrowserWin[0].name, "..") == 0) {
    sf2BrCursor = 1;
  }
  syncSf2BrowserScroll();
}

FLASHMEM bool openSf2PresetBrowser(int instIdx) {
  instIdx = constrain(instIdx, 0, NUM_INSTRUMENTS - 1);
  if (!sf2HasFile(instIdx)) return false;

  closeSamplerBrowser();
  closeInstBrowser();
  closeSysPresetsModal();
  closeTypePresetModal();
  sf2BrInstIdx = instIdx;
  sf2BrMode = Sf2BrowserMode::PresetList;

  int matchedIdx = -1;
  Sf2PoolEntry matchedEntry{};
  sf2BrCursor = 0;
  sf2BrScroll = 0;
  scanSf2PresetWindow(nullptr, 0, 0,
                      (int)sf2Slots[instIdx].curBank,
                      (int)sf2Slots[instIdx].curPresetNum,
                      &matchedIdx, &matchedEntry);
  if (sf2BrTotalCount <= 0) {
    sf2BrMode = Sf2BrowserMode::Closed;
    return false;
  }

  if (matchedIdx >= 0) {
    sf2BrCursor = matchedIdx;
    if (matchedIdx >= sf2BrWinCount) {
      int maxScroll = max(0, sf2BrTotalCount - SF2_BROWSER_VISIBLE_ROWS);
      sf2BrScroll = constrain(sf2BrCursor - SF2_BROWSER_VISIBLE_ROWS + 1, 0, maxScroll);
      int maxWinStart = max(0, sf2BrTotalCount - MAX_SF2_BROWSER_WINDOW);
      int wantStart = constrain(sf2BrScroll - 12, 0, maxWinStart);
      scanSf2PresetWindow(&matchedEntry, matchedIdx, wantStart);
    }
  }
  syncSf2BrowserScroll();
  sf2PreviewOrigBank = sf2Slots[instIdx].curBank;
  sf2PreviewOrigPreset = sf2Slots[instIdx].curPresetNum;
  sf2PreviewModified = false;
  sf2PreviewCursorReady = (matchedIdx >= 0);
  instPreviewSetSf2PresetModal(true);
  return true;
}

FLASHMEM bool sf2PreviewGetReadySlot(int &outSlot) {
  if (sf2BrMode != Sf2BrowserMode::PresetList || !sf2PreviewCursorReady ||
      sf2BrInstIdx < 0 || sf2BrInstIdx >= NUM_INSTRUMENTS) {
    return false;
  }
  outSlot = sf2BrInstIdx;
  return true;
}

FLASHMEM bool sf2PreviewEnsureLoaded() {
  if (sf2BrMode != Sf2BrowserMode::PresetList || sf2BrTotalCount <= 0) return false;
  if (sf2BrInstIdx < 0 || sf2BrInstIdx >= NUM_INSTRUMENTS || !sf2Slots[sf2BrInstIdx].valid) return false;
  const int idx = constrain(sf2BrCursor, 0, sf2BrTotalCount - 1);
  if (idx < sf2BrWinStart || idx >= sf2BrWinStart + sf2BrWinCount) return false;
  const Sf2PoolEntry &ent = sf2BrowserWin[idx - sf2BrWinStart];

  if (sf2Slots[sf2BrInstIdx].curBank == ent.bank &&
      sf2Slots[sf2BrInstIdx].curPresetNum == ent.preset) {
    sf2PreviewCursorReady = true;
    return true;
  }

  char fullPath[SAMPLER_PATH_MAX];
  resolveSf2FullPath(sf2BrInstIdx, fullPath, sizeof(fullPath));
  FsFile f = SD.sdfs.open(fullPath, O_RDONLY);
  if (!f.isOpen()) {
    sf2PreviewCursorReady = false;
    return false;
  }
  fmSynthSilenceInst(sf2BrInstIdx);
  bool ok = loadSf2PresetForSlot(sf2BrInstIdx, f, (int)ent.bank, (int)ent.preset, -1);
  f.close();
  if (ok) {
    samplerPrewarmSf2File(fullPath, sf2Slots[sf2BrInstIdx].fileTag);
    sf2PreviewModified = true;
    sf2PreviewCursorReady = true;
    return true;
  }
  sf2PreviewCursorReady = false;
  return false;
}

FLASHMEM void closeSf2Browser() {
  if (sf2BrMode == Sf2BrowserMode::PresetList) {
    instPreviewClearPlayHold();
    fmSynthSilenceInst(sf2BrInstIdx);
    if (sf2PreviewModified && sf2BrInstIdx >= 0 && sf2BrInstIdx < NUM_INSTRUMENTS &&
        sf2Slots[sf2BrInstIdx].valid &&
        (sf2Slots[sf2BrInstIdx].curBank != sf2PreviewOrigBank ||
         sf2Slots[sf2BrInstIdx].curPresetNum != sf2PreviewOrigPreset)) {
      char fullPath[SAMPLER_PATH_MAX];
      resolveSf2FullPath(sf2BrInstIdx, fullPath, sizeof(fullPath));
      FsFile f = SD.sdfs.open(fullPath, O_RDONLY);
      if (f.isOpen()) {
        if (loadSf2PresetForSlot(sf2BrInstIdx, f, (int)sf2PreviewOrigBank, (int)sf2PreviewOrigPreset, -1)) {
          samplerPrewarmSf2File(fullPath, sf2Slots[sf2BrInstIdx].fileTag);
        }
        f.close();
      }
    }
  }
  sf2PreviewModified = false;
  sf2PreviewCursorReady = false;
  sf2BrMode = Sf2BrowserMode::Closed;
  instPreviewSetSf2PresetModal(false);
}

FLASHMEM void sf2BrowserMove(int dy, int pageDelta) {
  if (sf2BrMode == Sf2BrowserMode::Closed || sf2BrTotalCount <= 0) return;
  int step = dy + pageDelta * SF2_BROWSER_VISIBLE_ROWS;
  if (step == 0) return;
  if (sf2BrMode == Sf2BrowserMode::PresetList) {
    instPreviewClearPlayHold();
    fmSynthSilenceInst(sf2BrInstIdx);
    sf2PreviewCursorReady = false;
  }
  sf2BrCursor = constrain(sf2BrCursor + step, 0, sf2BrTotalCount - 1);
  syncSf2BrowserScroll();
}

FLASHMEM bool sf2BrowserConfirm() {
  if (sf2BrMode == Sf2BrowserMode::Closed || sf2BrTotalCount <= 0) return false;
  const int idx = constrain(sf2BrCursor, 0, sf2BrTotalCount - 1);
  if (idx < sf2BrWinStart || idx >= sf2BrWinStart + sf2BrWinCount) return false;
  const Sf2PoolEntry ent = sf2BrowserWin[idx - sf2BrWinStart];

  if (sf2BrMode == Sf2BrowserMode::PresetList) {
    instPreviewClearPlayHold();
    if (sf2Slots[sf2BrInstIdx].valid &&
        sf2Slots[sf2BrInstIdx].curBank == ent.bank &&
        sf2Slots[sf2BrInstIdx].curPresetNum == ent.preset) {
      fmSynthSilenceInst(sf2BrInstIdx);
      sf2PreviewModified = false;
      sf2PreviewCursorReady = false;
      sf2BrMode = Sf2BrowserMode::Closed;
      instPreviewSetSf2PresetModal(false);
      return true;
    }
    char fullPath[SAMPLER_PATH_MAX];
    resolveSf2FullPath(sf2BrInstIdx, fullPath, sizeof(fullPath));
    FsFile f = SD.sdfs.open(fullPath, O_RDONLY);
    if (!f.isOpen()) return false;
    fmSynthSilenceInst(sf2BrInstIdx);
    bool ok = loadSf2PresetForSlot(sf2BrInstIdx, f, (int)ent.bank, (int)ent.preset, -1);
    f.close();
    if (ok) {
      samplerPrewarmSf2File(fullPath, sf2Slots[sf2BrInstIdx].fileTag);
      sf2PreviewModified = false;
      sf2PreviewCursorReady = false;
      sf2BrMode = Sf2BrowserMode::Closed;
      instPreviewSetSf2PresetModal(false);
    }
    return ok;
  }

  // FileSelect mode
  if (ent.isDir) {
    char entName[sizeof(ent.name)];
    memcpy(entName, ent.name, sizeof(entName));
    entName[sizeof(entName) - 1] = '\0';
    if (!pagedStepBrowserDir(sf2BrDir, sizeof(sf2BrDir), entName)) return false;
    sf2BrCursor = 0;
    sf2BrScroll = 0;
    scanSf2FileWindow(nullptr, 0, 0);
    if (sf2BrTotalCount > 1 && sf2BrWinCount > 0 && strcmp(sf2BrowserWin[0].name, "..") == 0) {
      sf2BrCursor = 1;
    }
    syncSf2BrowserScroll();
    return false;
  }

  char fullPath[SAMPLER_PATH_MAX];
  if (strcmp(sf2BrDir, "/") == 0) {
    snprintf(fullPath, sizeof(fullPath), "/%.42s", ent.name);
  } else {
    snprintf(fullPath, sizeof(fullPath), "%.95s/%.42s", sf2BrDir, ent.name);
  }
  if (!sf2AssignFile(sf2BrInstIdx, fullPath)) {
    return false;
  }
  sf2BrMode = Sf2BrowserMode::Closed;
  return true;
}

// ============================================================================
// InstEngineOps UI Formatting, Parameter Editing & Validation Callbacks
// ============================================================================
FLASHMEM static const FmPatch &sf2DefaultPatch() {
  return SF2_DEFAULT_PATCH;
}

FLASHMEM static void sf2ValidatePatch(FmPatch &p) {
  p.type              = (uint8_t)InstType::SoundFont;
  p.algorithm         = (uint8_t)constrain((int)p.algorithm, 0, NUM_SF2_LOOP_MODES - 1);
  p.op[0].ratioX100   = (uint16_t)constrain((int)p.op[0].ratioX100, (int)SF2_TUNE_MIN, (int)SF2_TUNE_MAX);
  p.op[0].wave        = (uint8_t)constrain((int)p.op[0].wave, 0, NUM_SF2_ENV_MODES - 1);
  p.op[1].wave        = (uint8_t)constrain((int)p.op[1].wave, 0, NUM_SF2_INTERP_MODES - 1);
  p.op[0].level       = (uint8_t)constrain((int)p.op[0].level, 0, 100);
  int pan             = constrain((int)(int8_t)p.op[1].reserved[2], -100, 100);
  p.op[1].reserved[2] = (uint8_t)(int8_t)pan;
}

// Section 2 (opIdx == 2, Row 3): 6-Cell Tone & Playback Row [TUNE | LOOP | ENV | INTP | VEL | PAN]
FLASHMEM static void sf2FormatMainCell(const FmPatch &p, int opIdx, int col,
                                       const char *&outLabel, char *outVal, size_t valSz,
                                       int &outBarVal, int &outBarMax) {
  (void)opIdx;
  switch (col) {
    case 0: {
      outLabel = "TUNE";
      int signedX100 = constrain((int)p.op[0].ratioX100, (int)SF2_TUNE_MIN, (int)SF2_TUNE_MAX) - (int)SF2_TUNE_ZERO;
      int absX100 = abs(signedX100);
      const char *signStr = (signedX100 < 0) ? "-" : (signedX100 > 0 ? "+" : "");
      snprintf(outVal, valSz, "%s%d.%02d", signStr, absX100 / 100, absX100 % 100);
      outBarVal = constrain((int)p.op[0].ratioX100, (int)SF2_TUNE_MIN, (int)SF2_TUNE_MAX);
      outBarMax = SF2_TUNE_MAX;
      break;
    }
    case 1: {
      outLabel = "LOOP";
      uint8_t m = (uint8_t)constrain((int)p.algorithm, 0, NUM_SF2_LOOP_MODES - 1);
      snprintf(outVal, valSz, "%s", sf2LoopModeName(m));
      outBarVal = (int)m + 1;
      outBarMax = NUM_SF2_LOOP_MODES;
      break;
    }
    case 2: {
      outLabel = "ENV";
      uint8_t m = (uint8_t)constrain((int)p.op[0].wave, 0, NUM_SF2_ENV_MODES - 1);
      snprintf(outVal, valSz, "%s", sf2EnvModeName(m));
      outBarVal = (int)m + 1;
      outBarMax = NUM_SF2_ENV_MODES;
      break;
    }
    case 3: {
      outLabel = "INTP";
      uint8_t m = (uint8_t)constrain((int)p.op[1].wave, 0, NUM_SF2_INTERP_MODES - 1);
      snprintf(outVal, valSz, "%s", sf2InterpModeName(m));
      outBarVal = (int)m + 1;
      outBarMax = NUM_SF2_INTERP_MODES;
      break;
    }
    case 4: {
      outLabel = "VEL";
      uint8_t v = (uint8_t)constrain((int)p.op[0].level, 0, 100);
      snprintf(outVal, valSz, "%u", (unsigned)v);
      outBarVal = v;
      outBarMax = 100;
      break;
    }
    default: {
      outLabel = "PAN";
      int pan = constrain((int)(int8_t)p.op[1].reserved[2], -100, 100);
      if (pan > 0) snprintf(outVal, valSz, "+%d", pan);
      else         snprintf(outVal, valSz, "%d", pan);
      outBarVal = pan + 100;
      outBarMax = 200;
      break;
    }
  }
}

FLASHMEM static void sf2AdjustMainCell(FmPatch &p, int opIdx, int col, int dx, int dy, int delta, int step) {
  (void)opIdx;
  switch (col) {
    case 0: {
      int tStep = (dy != 0) ? (dy * 100) : dx;
      p.op[0].ratioX100 = (uint16_t)constrain((int)p.op[0].ratioX100 + tStep, (int)SF2_TUNE_MIN, (int)SF2_TUNE_MAX);
      break;
    }
    case 1:
      p.algorithm = (uint8_t)(((int)p.algorithm + delta + NUM_SF2_LOOP_MODES) % NUM_SF2_LOOP_MODES);
      break;
    case 2:
      p.op[0].wave = (uint8_t)(((int)p.op[0].wave + delta + NUM_SF2_ENV_MODES) % NUM_SF2_ENV_MODES);
      break;
    case 3:
      p.op[1].wave = (uint8_t)(((int)p.op[1].wave + delta + NUM_SF2_INTERP_MODES) % NUM_SF2_INTERP_MODES);
      break;
    case 4:
      p.op[0].level = (uint8_t)constrain((int)p.op[0].level + step, 0, 100);
      break;
    default: {
      int pan = constrain((int)(int8_t)p.op[1].reserved[2] + step, -100, 100);
      p.op[1].reserved[2] = (uint8_t)(int8_t)pan;
      break;
    }
  }
}

FLASHMEM static void sf2ResetMainCell(FmPatch &p, const FmPatch &defPatch, int opIdx, int col) {
  (void)opIdx;
  switch (col) {
    case 0: p.op[0].ratioX100   = defPatch.op[0].ratioX100;   break;
    case 1: p.algorithm         = defPatch.algorithm;         break;
    case 2: p.op[0].wave        = defPatch.op[0].wave;        break;
    case 3: p.op[1].wave        = defPatch.op[1].wave;        break;
    case 4: p.op[0].level       = defPatch.op[0].level;       break;
    default: p.op[1].reserved[2] = defPatch.op[1].reserved[2]; break;
  }
}

PROGMEM static const InstEngineOps SF2_ENGINE_OPS = {
  InstType::SoundFont,
  "SOUNDFONT",
  "SF2",
  "SF",
  "SOUNDFONT FILE",
  "BANK & PRESET",
  "TONE & PLAYBACK",
  sf2DefaultPatch,
  sf2ValidatePatch,
  sf2FormatMainCell,
  sf2AdjustMainCell,
  sf2ResetMainCell,
  sf2RenderVoice
};

const InstEngineOps &instSoundFontEngine() {
  return SF2_ENGINE_OPS;
}

} // namespace looper
