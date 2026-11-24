#!/usr/bin/env python3
"""
verify_firmware.py — Automated architectural, memory, DMA-safety, and storage
invariant verifier for the M8 Capstan firmware.

Run after compiling the firmware:
    python tools/recorder_sim/verify_firmware.py
"""

import argparse
import re
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
SRC_DIR = ROOT / "src"
SKETCH_NAME = "Capstan.ino"
TARGET_ELFS = {
    "model01": ("Model:01", ROOT / "build" / "model01" / f"{SKETCH_NAME}.elf"),
    "model02": ("Model:02", ROOT / "build" / "model02" / f"{SKETCH_NAME}.elf"),
}


class VerificationFailure(Exception):
    pass


def check(condition: bool, msg: str):
    if not condition:
        raise VerificationFailure(msg)
    print(f"  [PASS] {msg}")


def verify_dma_and_storage_source_invariants():
    print("=== 1. Source Invariant Checks (DMA Safety & Storage State Machine) ===")
    storage_cpp = (SRC_DIR / "storage.cpp").read_text(encoding="utf-8")
    midi_seq_cpp = (SRC_DIR / "midi_seq.cpp").read_text(encoding="utf-8")
    ino_src = (ROOT / SKETCH_NAME).read_text(encoding="utf-8")

    # 1a. Never pass eventPool, inputRing, or tr.ring directly to FsFile read/write
    direct_dma_pattern = re.compile(r"\.\s*(?:read|write)\s*\(\s*(?:eventPool|inputRing|tr\.ring|tracks\[)")
    check(
        not direct_dma_pattern.search(storage_cpp),
        "storage.cpp never passes DMAMEM buffers (inputRing/tr.ring) directly to FsFile::read/write",
    )
    check(
        not direct_dma_pattern.search(midi_seq_cpp),
        "midi_seq.cpp never passes DMAMEM eventPool directly to FsFile::read/write",
    )

    # 1b. loadSong() and loadSongByName() must stop transport without auto-saving before reading disk
    load_idx = storage_cpp.find("FLASHMEM bool loadSong(")
    load_name_idx = storage_cpp.find("FLASHMEM bool loadSongByName(")
    check(load_idx != -1 and load_name_idx != -1, "loadSong() and loadSongByName() exist in storage.cpp")
    load_body = storage_cpp[load_idx : load_idx + 1200]
    load_name_body = storage_cpp[load_name_idx : load_name_idx + 1200]
    check(
        "storageStopTransport()" in load_body
        and "storageStopTransport()" in load_name_body
        and "saveCurrentSong()" not in load_body
        and "saveCurrentSong()" not in load_name_body,
        "loadSong() and loadSongByName() call storageStopTransport() without auto-saving",
    )

    # 1b-2. Manual-save-only invariants: storageStopTransport() and serviceStorage() must never auto-save
    stop_idx = storage_cpp.find("void storageStopTransport()")
    stop_end = storage_cpp.find("void keepStreaming()", stop_idx)
    stop_body = storage_cpp[stop_idx:stop_end]
    svc_idx = storage_cpp.find("void serviceStorage()")
    svc_end = storage_cpp.find("void flushHeaders()", svc_idx)
    svc_body = storage_cpp[svc_idx:svc_end]
    check(
        "saveCurrentSong()" not in stop_body and "saveCurrentSong()" not in svc_body,
        "storageStopTransport() and serviceStorage() enforce manual-save-only (no background auto-save)",
    )

    # 1c. deleteSongByName() must not call saveCurrentSong()
    del_idx = storage_cpp.find("FLASHMEM bool deleteSongByName(")
    del_end = storage_cpp.find("FLASHMEM bool deleteSongAt(", del_idx)
    check(del_idx != -1 and del_end != -1, "deleteSongByName() exists in storage.cpp")
    del_body = storage_cpp[del_idx:del_end]
    check(
        "saveCurrentSong()" not in del_body and "removeSongFolderFiles(clean)" in del_body,
        "deleteSongByName() removes song files and scans folders without re-saving",
    )

    # 1d. System & Song saves must commit SD flash cache via syncSdCard()
    save_sys_idx = storage_cpp.find("FLASHMEM bool saveSystemSettings()")
    save_sys_end = storage_cpp.find("FLASHMEM static bool loadSystemSettingsFromDisk()", save_sys_idx)
    save_sys_body = storage_cpp[save_sys_idx:save_sys_end]
    check(
        "syncSdCard()" in save_sys_body,
        "saveSystemSettings() commits SD card flash cache via syncSdCard()",
    )

    # 1e. loop() in .ino must not mark the song dirty on every pass
    loop_idx = ino_src.find("void loop()")
    loop_body = ino_src[loop_idx:]
    check(
        "saveSessionWhenIdle();" not in loop_body,
        f"loop() in {SKETCH_NAME} does not call saveSessionWhenIdle() on every iteration",
    )

    # 1f. 5-minute inactivity screen blanking invariants
    config_h = (SRC_DIR / "config.h").read_text(encoding="utf-8")
    ui_cpp = (SRC_DIR / "ui.cpp").read_text(encoding="utf-8")
    input_cpp = (SRC_DIR / "input.cpp").read_text(encoding="utf-8")
    check(
        "SCREEN_BLANK_TIMEOUT_MS = 5UL * 60UL * 1000UL" in config_h,
        "SCREEN_BLANK_TIMEOUT_MS is set to 5 minutes (300,000 ms) in config.h",
    )
    check(
        "board.setBacklight(0);" in ui_cpp and "board.tft().fillScreen(BG);" in ui_cpp,
        "uiRefresh() blanks display and turns off backlight after SCREEN_BLANK_TIMEOUT_MS",
    )
    check(
        "swallowWakeKeys" in input_cpp and "isScreenBlanked()" in input_cpp,
        "pollKeys() wakes screen on button press and consumes the wake gesture until release",
    )
    check(
        "noteUserActivity()" in midi_seq_cpp,
        "midiSeqService() wakes screen / resets inactivity timer on incoming MIDI events",
    )

    # 1g. Punch-in / Punch-out WAV file padding & windowEntered race-safety invariants
    drain_idx = storage_cpp.find("static void drainRecordingBuffer(bool flushAll) {")
    check(drain_idx != -1, "drainRecordingBuffer() definition exists in storage.cpp")
    drain_body = storage_cpp[drain_idx : drain_idx + 2400]
    check(
        "padTrackFileToBytePos(" in drain_body and "recording.windowEntered" in drain_body,
        "drainRecordingBuffer() latches windowEntered and pads WAV file before seeking/writing punch-in audio",
    )

    # 1h. Modular Multi-Engine Instruments & Per-Page UI/Input (page_*, inst_*, instruments) invariants
    instruments_cpp = (SRC_DIR / "instruments.cpp").read_text(encoding="utf-8")
    inst_fm2op_h = (SRC_DIR / "inst_fm2op.h").read_text(encoding="utf-8")
    inst_fm2op_cpp = (SRC_DIR / "inst_fm2op.cpp").read_text(encoding="utf-8")
    inst_pd_h = (SRC_DIR / "inst_phasedist.h").read_text(encoding="utf-8")
    inst_pd_cpp = (SRC_DIR / "inst_phasedist.cpp").read_text(encoding="utf-8")
    engine_cpp = (SRC_DIR / "engine.cpp").read_text(encoding="utf-8")
    master_fx_cpp = (SRC_DIR / "master_fx.cpp").read_text(encoding="utf-8")
    page_files = [
        "page_audio.cpp",
        "page_midi.cpp",
        "page_song_settings.cpp",
        "page_main.cpp",
        "page_system_settings.cpp",
        "page_instruments.cpp",
        "page_instrument_type.cpp",
        "page_instrument_edit.cpp",
        "page_master.cpp",
    ]
    all_pages_cpp = "\n".join((SRC_DIR / pf).read_text(encoding="utf-8") for pf in page_files)
    check(
        "fmSynthRenderBlock(" in engine_cpp,
        "LooperEngine::update() renders internal synthesizer audio into recordable input & master bus",
    )
    check(
        "Page::Instruments" in all_pages_cpp
        and "Page::InstrumentType" in all_pages_cpp
        and "Page::InstrumentEdit" in all_pages_cpp
        and "Page::Master" in all_pages_cpp
        and "pageOpsFor(" in ui_cpp
        and "pageOpsFor(" in input_cpp,
        "Modular page_*.cpp modules and PageOps dispatcher implement UI rendering & input across all 9 pages",
    )
    check(
        "processMasterFx(mixL, mixR);" in engine_cpp
        and "masterFxOpsFor(" in master_fx_cpp
        and "setMasterChorus(" in master_fx_cpp
        and "setMasterDelay(" in master_fx_cpp
        and "setMasterDelayFeedback(" in master_fx_cpp
        and "setMasterReverb(" in master_fx_cpp
        and "setMasterTape(" in master_fx_cpp
        and "setMasterComp(" in master_fx_cpp,
        "master_fx.cpp and engine.cpp implement modular Master FX chain (Chorus -> Delay -> Reverb -> Tape -> Comp) across tracks + live monitor",
    )
    check(
        all(p in inst_fm2op_cpp for p in ('"OPERATOR A"', '"OPERATOR B"', "FM_DEFAULT_PATCH"))
        and "FM_FACTORY_PRESETS" not in inst_fm2op_cpp
        and "FM_PRESET_NAMES" not in inst_fm2op_cpp
        and "CARRIER" not in inst_fm2op_cpp
        and "MODULATOR" not in inst_fm2op_cpp
        and "NUM_FM_ALGOS = 3" in inst_fm2op_h
        and "FmAlgo::Parallel" in inst_fm2op_cpp
        and "FmAlgo::Fm" in inst_fm2op_cpp
        and "FmAlgo::FmAB" in inst_fm2op_cpp
        and "FM_RATIO_MAX = 2400" in inst_fm2op_h
        and "instFm2OpEngine()" in inst_fm2op_cpp,
        "inst_fm2op module defines Operator A & B, 3 FM algorithms ([A][B], [B]>[A], [A]>[B]), default patch (no hardcoded presets), and 0.00..24.00 ratio control",
    )
    check(
        all(p in inst_pd_cpp for p in ('"LINE 1"', '"LINE 2"', '"LINE 2 > LINE 1"', "PD_DEFAULT_PATCH"))
        and "PD_FACTORY_PRESETS" not in inst_pd_cpp
        and "PD_PRESET_NAMES" not in inst_pd_cpp
        and "TIMBRE" not in inst_pd_cpp
        and "AMPLIFIER" not in inst_pd_cpp
        and "NUM_PD_WAVES = 8" in inst_pd_h
        and '"PHASE DIST"' in inst_pd_cpp
        and "instPhaseDistEngine()" in inst_pd_cpp,
        "inst_phasedist module defines 8 waveforms, Line 1 / Line 2 / Line 2 > Line 1 sections, default patch (no hardcoded presets), and Phase Dist engine descriptor",
    )
    page_inst_cpp = (SRC_DIR / "page_instruments.cpp").read_text(encoding="utf-8")
    page_inst_type_cpp = (SRC_DIR / "page_instrument_type.cpp").read_text(encoding="utf-8")
    page_inst_edit_cpp = (SRC_DIR / "page_instrument_edit.cpp").read_text(encoding="utf-8")
    check(
        'SONGS_DIR = "/Capstan/SONGS"' in config_h
        and 'INST_PRESETS_DIR = "/Capstan/INSTRUMENT_PRESETS"' in config_h
        and "fmSynthSaveInstrumentToSong(" in instruments_cpp
        and "fmSynthSaveInstrumentPreset(" in instruments_cpp
        and "fmSynthLoadInstrumentFile(" in instruments_cpp
        and "openInstBrowser(" in instruments_cpp
        and "openSysPresetsModal(" in instruments_cpp
        and "openTypePresetModal(" in instruments_cpp
        and all(act in page_inst_cpp for act in ("INST_ROW_ACT_COPY", "INST_ROW_ACT_PASTE", "INST_ROW_ACT_NEW", "INST_ROW_ACT_CLR"))
        and "INST_ROW_ACT_LOAD" not in page_inst_cpp
        and all(lbl in page_inst_type_cpp for lbl in ('"Load song preset..."', '"Load global preset..."'))
        and '"SAVE NAME"' in ui_cpp
        and all(lbl in page_inst_edit_cpp for lbl in ('"LOAD"', '"SAVE TO..."', '"SONG"', '"PRESET"'))
        and "PRESET:" not in page_inst_edit_cpp,
        "Instrument files save to <Song>/INSTRUMENTS and /Capstan/INSTRUMENT_PRESETS with COPY/PASTE/NEW/CLR rows, Type page preset loaders, LOAD/SAVE TO... UI, SAVE NAME keyboard, and SD file browser",
    )
    pages_h = (SRC_DIR / "pages.h").read_text(encoding="utf-8")
    ui_h = (SRC_DIR / "ui.h").read_text(encoding="utf-8")
    ui_font_cpp = (SRC_DIR / "ui_font.cpp").read_text(encoding="utf-8")
    check(
        "CELL_BUF_MAX = 15000" in pages_h
        and "CELL_BUF_MAX = 7400" in pages_h
        and "SX(" in ui_h
        and "SY(" in ui_h,
        "pages.h & ui.h configure CELL_BUF_MAX (7400 on Model:01, 15000 on Model:02) and SX/SY scaling helpers",
    )
    check(
        "fmSynthSaveSong(" in storage_cpp and "fmSynthLoadSong(" in storage_cpp,
        "storage.cpp persists song instrument instances in INST.DAT during explicit save/load",
    )
    check(
        "sinLut" not in instruments_cpp[instruments_cpp.find("fmSynthSaveSong") :],
        "instruments.cpp stages INST.DAT and .FMI reads/writes via RAM1 stack buffers (DMA_SDIO safe)",
    )

    # 1h-2. System Settings vs Song Settings separation & Master Balance removal invariants
    storage_h = (SRC_DIR / "storage.h").read_text(encoding="utf-8")
    page_sys_cpp = (SRC_DIR / "page_system_settings.cpp").read_text(encoding="utf-8")
    ui_splash_cpp = (SRC_DIR / "ui_splash.cpp").read_text(encoding="utf-8")
    usb_drive_cpp = (SRC_DIR / "usb_drive.cpp").read_text(encoding="utf-8")
    page_song_cpp = (SRC_DIR / "page_song_settings.cpp").read_text(encoding="utf-8")
    page_master_cpp = (SRC_DIR / "page_master.cpp").read_text(encoding="utf-8")
    check(
        "masterBalance" not in storage_h
        and "masterBalance" not in storage_cpp
        and "setMasterBalance" not in engine_cpp
        and "MASTER_BALANCE_MAX" not in config_h,
        "Master Balance is completely removed from config.h, engine.*, storage.*, and UI pages",
    )
    check(
        all(lbl in page_song_cpp for lbl in (
            '"RECORD LINE/USB"',
            '"MONITOR LINE/USB"',
            '"LINE IN GAIN"',
            '"REC TRIG THRESH"',
            '"MIDI CLOCK OUT"',
            '"MIDI CLOCK SOURCE"',
            '"MIDI THRU"',
            '"SONG INSTRUMENTS"',
            '"MANAGE"',
            '"ARM"',
        ))
        and "if (row == PSET_REC_SRC) return 2;" in page_song_cpp
        and "MIN_THRESH_DB = -80" in config_h
        and "DEFAULT_THRESH_DB = -60" in config_h
        and all(removed not in page_song_cpp for removed in (
            '"REC LINE/USB/INSTR"',
            '"AUDIO INPUT SOURCE"',
            '"LINE INPUT MONITOR"',
            '"USB INPUT MONITOR"',
            '"DEFAULT AUDIO THRESH"',
            '"LIVE MIDI THRU"',
            '"SONG TEMPO"',
            '"METRONOME"',
            '"RECORD COUNT-IN"',
            '"PUNCH WINDOW"',
            '"PUNCH-IN POINT"',
            '"PUNCH-OUT POINT"',
        )),
        "page_song_settings.cpp exposes the song settings (RECORD LINE/USB, MONITOR LINE/USB with ON/ARM/OFF, REC TRIG THRESH -80dB, MIDI CLOCK OUT/SOURCE/THRU, SONG INSTRUMENTS, EXPORT MIDI TRACKS, EXPORT MASTER OUT)",
    )
    check(
        '"SCREEN BRIGHTNESS"' in page_sys_cpp
        and '"METRONOME VOLUME"' in page_sys_cpp
        and '"HEADPHONE GAIN"' not in page_sys_cpp
        and '"INSTRUMENT PRESETS"' in page_sys_cpp
        and '"PROJECTS"' in page_sys_cpp
        and '"MIDI MAPPINGS"' in page_sys_cpp
        and '"MANAGE"' in page_sys_cpp
        and '"USB CHARGE WHILE ON"' in page_sys_cpp
        and '"USB STORAGE DRIVE"' in page_sys_cpp
        and page_sys_cpp.index('"USB CHARGE WHILE ON"') < page_sys_cpp.index('"USB STORAGE DRIVE"')
        and '"Delete preset?"' in page_sys_cpp
        and '"Delete project?"' in page_sys_cpp
        and '"Remove song?"' in page_sys_cpp
        and 'PROJECTS_DIR = "/Capstan/PROJECTS"' in config_h
        and 'return "SYS | v" CAPSTAN_VERSION;' in page_sys_cpp
        and 'SPLASH_VER_TEXT[] = "v" CAPSTAN_VERSION;' in ui_splash_cpp
        and "'87" not in ui_splash_cpp
        and re.search(r'#define\s+CAPSTAN_VERSION\s+"\d+\.\d+"', config_h) is not None
        and "max(SX(62), textWidth(title) + SX(8))" in ui_cpp
        and "SSET_BRIGHTNESS    = 0" in (SRC_DIR / "app.h").read_text(encoding="utf-8")
        and "SSET_METRO_VOL     = 1" in (SRC_DIR / "app.h").read_text(encoding="utf-8")
        and "SSET_INST_PRESETS  = 2" in (SRC_DIR / "app.h").read_text(encoding="utf-8")
        and "SSET_PROJECTS      = 3" in (SRC_DIR / "app.h").read_text(encoding="utf-8")
        and "SSET_MIDI_MAP      = 4" in (SRC_DIR / "app.h").read_text(encoding="utf-8")
        and "SSET_USB_CHARGE_ON = 5" in (SRC_DIR / "app.h").read_text(encoding="utf-8")
        and "SSET_USB_DRIVE     = 6" in (SRC_DIR / "app.h").read_text(encoding="utf-8")
        and "SSET_COUNT         = 7" in (SRC_DIR / "app.h").read_text(encoding="utf-8")
        and "SSET_HP_GAIN" not in (SRC_DIR / "app.h").read_text(encoding="utf-8")
        and "usbChargeWhileOn = true" in storage_h
        and "usbChargeOffEnc" in storage_cpp
        and "ps.masterVolume" in page_master_cpp
        and "systemSettings" not in page_master_cpp
        and "MIN_BRIGHTNESS = 5" in config_h
        and "MAX_BRIGHTNESS = 100" in config_h
        and "DEFAULT_BRIGHTNESS = 50" in config_h
        and "DEFAULT_METRO_VOLUME = VOLUME_UNITY" in config_h
        and "brightnessToPwm(" in config_h
        and "setMetronomeVolume(" in engine_cpp
        and "VOLUME_MAX = 100, VOLUME_UNITY = 80" in config_h
        and "PAN_MAX = 100" in config_h
        and "MASTER_VOLUME_MAX = 100, MASTER_VOLUME_UNITY = 80" in config_h
        and "MASTER_VOLUME_SPK_DEFAULT = 40" in config_h
        and "MASTER_VOLUME_HP_DEFAULT = 50" in config_h
        and '"USB STORAGE MODE"' in usb_drive_cpp
        and '"PRESS ANY KEY TO EXIT"' in usb_drive_cpp
        and "drawUsbOverlayHeader(" in usb_drive_cpp
        and "drawUsbOverlayStatus(" in usb_drive_cpp
        and "drawUsbOverlayHint(" in usb_drive_cpp
        and "t.fillScreen(" not in usb_drive_cpp
        and "recLineIn = true" in storage_h
        and "recUsbIn = true" in storage_h
        and "lineMonitor = MonitorMode::On" in storage_h
        and "usbMonitor = MonitorMode::On" in storage_h
        and "usbInputMonitor = true" in storage_h
        and "midiClockOut = MidiClockOut::Off" in storage_h,
        "System Settings displays 'SYS | vX.Y' header with Brightness (5..100, default 50), Metronome Volume (0..100, default 80), Instrument Presets, Projects, MIDI Mappings, and USB Storage Drive (no Headphone Gain row)",
    )

    # 1i. USB Audio (2-ch Stereo Out / 2-ch Stereo In UAC2), Dynamic USB Mass Storage, & USB MIDI invariants
    patch_py = (ROOT / "tools" / "teensy_msc" / "patch.py").read_text(encoding="utf-8")
    usb_audio_cpp = (ROOT / "tools" / "teensy_msc" / "usb_audio_l7.cpp").read_text(encoding="utf-8")
    usb_msc_c = (ROOT / "tools" / "teensy_msc" / "usb_msc.c").read_text(encoding="utf-8")
    engine_h = (SRC_DIR / "engine.h").read_text(encoding="utf-8")
    midi_usb_cpp = (SRC_DIR / "midi_usb.cpp").read_text(encoding="utf-8")
    usb_names_c = (SRC_DIR / "usb_names.c").read_text(encoding="utf-8")
    usb_drive_h = (SRC_DIR / "usb_drive.h").read_text(encoding="utf-8")
    midi_in_cpp = (SRC_DIR / "midi_in.cpp").read_text(encoding="utf-8")
    midi_map_cpp = (SRC_DIR / "midi_map.cpp").read_text(encoding="utf-8")
    check(
        "PRODUCT_ID\t\t0x048A" in patch_py
        and "MSC_STORAGE_PID\t0x04D5" in patch_py
        and "BCD_DEVICE\t\t0x0381" in patch_py
        and "BCD_DEVICE\t\t0x0379" in patch_py
        and "compute_patch_stamp()" in patch_py
        and "L7_DELAY" not in patch_py
        and "NUM_INTERFACE\t\t6" in patch_py
        and "MIDI_INTERFACE        2" in patch_py
        and "AUDIO_INTERFACE\t3" in patch_py
        and "AUDIO_TX_CHANNELS_480\t2" in patch_py
        and "AUDIO_TX_CHANNELS_12\t2" in patch_py
        and "AUDIO_RX_CHANNELS\t2" in patch_py
        and "AUDIO_TX_ENDPOINT     7" in patch_py
        and "AUDIO_RX_ENDPOINT     6" in patch_py
        and "AUDIO_SYNC_ENDPOINT\t6" in patch_py
        and 'CHANNEL_NAMES = ["Master L", "Master R"]' in patch_py
        and 'tx_config = "0x03,0,0,0"' in patch_py
        and 'fb_size = "4" if hs else "3"' in patch_py
        and "250,                                    // bMaxPower (500 mA)" in patch_py
        and "usb_self_powered_no_charge" in patch_py
        and "usb_set_self_powered_no_charge(" in usb_msc_c
        and "usbReenumerateChargeDescriptor(" in midi_usb_cpp
        and "usb_configuration = 0;" in patch_py
        and "USB1_USBCMD &= ~USB_USBCMD_ATDTW;" in patch_py
        and "USB1_ENDPTPRIME = epmask;" in patch_py
        and "USB_USBMODE_SDIS" in patch_py
        and "USB_USBSTS_UEI" in patch_py
        and "USBPHY_CTRL_ENUTMILEVEL2" in patch_py
        and "endpoint->next = (uint32_t)first_active;" in patch_py
        and "'C', 'A', 'P', 'S', 'T', 'A', 'N', '-', '0', SERIAL_MODEL_DIGIT, '-'" in usb_names_c
        and "HW_OCOTP_MAC0" in usb_names_c
        and "startup_middle_hook(" in usb_names_c
        and "ISO_N = 8" in usb_audio_cpp
        and "SYNC_N = 4" in usb_audio_cpp
        and "USB_AUDIO_IRQ_PRIORITY = 64" in usb_audio_cpp
        and "DMAMEM static TxIsoPool" not in usb_audio_cpp
        and "DMAMEM static RxIsoPool" not in usb_audio_cpp
        and "ARM_DWT_CYCCNT" in usb_audio_cpp
        and "SOF_MIN_STEP_UFRAMES = 64u" in usb_audio_cpp
        and "sample_usb_sof_clock(" in usb_audio_cpp
        and "USB1_FRINDEX" in usb_audio_cpp
        and "usb_host_rate" in usb_audio_cpp
        and "setup_rx_stream(false)" in usb_audio_cpp
        and "setup_tx_stream(false)" in usb_audio_cpp
        and "RATE_KI_BLOCK" in usb_audio_cpp
        and "rx_integ *= 0.995f" in usb_audio_cpp
        and "lerp_stereo_frame(" in usb_audio_cpp
        and "USB_OUT_CHANNELS = 2;" in engine_h
        and "usbTrackChannel" not in engine_h
        and "usbTrackChannel" not in engine_cpp
        and "usbFrame[0] = (int16_t)constrain(postFxL" in engine_cpp
        and "usbFrame[1] = (int16_t)constrain(postFxR" in engine_cpp,
        "tools/teensy_msc & usb_names.c configure Normal Mode (PID 0x048A, CAPSTAN-01/02-<HWID>), self-healing stamp, SDIS/UEI/first_active hub safety, and 8 ms CYCCNT clock checkpoints",
    )
    check(
        "midiOutHasUsbPending(" in midi_usb_cpp
        and "usb_midi_available()" in midi_usb_cpp
        and "USB_MIDI_RX_BUDGET_PER_TICK" in midi_usb_cpp
        and "if (!usbMIDI.read()) continue;" in midi_usb_cpp
        and "DMAMEM static uint32_t usbMidiTxBuf" not in midi_usb_cpp
        and "UsbEndpointQueueHead" not in midi_usb_cpp
        and "usb_midi_reset_epoch" in midi_usb_cpp
        and "usbShutdownDetach()" in midi_usb_cpp
        and "midiOutTakeUsbPackets(" in midi_usb_cpp
        and "midiOutTakeUsbTicks(" in midi_usb_cpp
        and "USB_MIDI_TX_EP_MASK" in midi_usb_cpp
        and "NVIC_DISABLE_IRQ(IRQ_USB1)" in midi_usb_cpp
        and "usb_transmit(MIDI_TX_ENDPOINT" in midi_usb_cpp
        and "usbMidiEndpointActive()" in midi_usb_cpp
        and "!usb_msc_is_active()" in midi_usb_cpp
        and "usbMIDI.send" not in midi_usb_cpp
        and "usb_msc_set_mode(" in usb_drive_cpp
        and "storageSharedChunkBuf()" in usb_drive_cpp
        and "usbDriveBegin" not in usb_drive_cpp
        and "usbDriveBegin" not in usb_drive_h
        and "pollTimer" not in usb_drive_cpp
        and "usb_msc_clear_halt(" in usb_msc_c
        and "csw_pending_after_unjam" in usb_msc_c
        and "cbw_invalid_wedge" in usb_msc_c
        and "case 0x5A:" in usb_msc_c
        and "L7_IRQ_SAFE" in patch_py
        and "L7_MIDI_SAFE" in patch_py
        and "L7_SERIAL_FLASH" in patch_py
        and "usb_midi_reset(" in patch_py
        and "requestContinuousCcUiRedraw(" in midi_map_cpp
        and "availableForWrite() <= 0" in engine_cpp
        and "midiActiveClockOutPorts()" in engine_cpp
        and "FLASHMEM uint8_t midiActiveClockOutPorts()" not in engine_cpp,
        "midi_usb.cpp, usb_midi.c, usb_serial.c, usb_drive.cpp, and usb_msc.c isolate dynamic MSC mode (PID 0x04D5), harden error/reset paths, detach cleanly on power-off, and coalesce CC UI redraws",
    )
    check(
        "saveIrq()" in midi_seq_cpp
        and "thruPortMask" in midi_seq_cpp
        and "clockSourceAllows(" in midi_in_cpp
        and "noInterrupts()" not in midi_in_cpp
        and "tempoLocked" in midi_in_cpp
        and "candidateBpm" in midi_in_cpp
        and "midiInResetClockState()" in midi_in_cpp
        and "midiInResetClockState()" in page_song_cpp,
        "midi_seq.cpp and midi_in.cpp protect ISR input ring writes, suppress USB Thru echo, and stabilize external MIDI clock with multi-beat window + hysteresis",
    )

    # 1j. Model:02 (Teensy MicroMod + ILI9488 full 480x320 UI + 9x13 high-DPI font) invariants
    ili_cpp = (SRC_DIR / "ili9488_spi.cpp").read_text(encoding="utf-8")
    m8_board_cpp = (SRC_DIR / "M8Board.cpp").read_text(encoding="utf-8")
    check(
        "_w  = _pw;" in ili_cpp
        and "_h  = _ph;" in ili_cpp
        and "SCREEN_W = 480" in ui_h
        and "SCREEN_H = 320" in ui_h
        and "FONT_ROWS = 13" in ui_h
        and "FONT_GAP = 3" in ui_h
        and "clearPanel(0)" in m8_board_cpp
        and "LPUART_CTRL_RE" in m8_board_cpp
        and "teensyMM.menu.usb.serialmsc" in patch_py,
        "Model:02 ILI9488Spi uses full 480x320 viewport, 9x13 high-DPI font (1.5x W, 1.33x H), and teensyMM serialmsc",
    )
    power_off_idx = m8_board_cpp.find("void Board::powerOff()")
    power_off_body = m8_board_cpp[power_off_idx:] if power_off_idx != -1 else ""
    check(
        power_off_idx != -1
        and "bqWrite(0x00, _bqDefaultReg00)" in power_off_body
        and "bqWrite(0x02, 0x00)" in power_off_body
        and "bqWrite(0x03, _cgPaused ? 0x06 : 0x16)" in power_off_body
        and "setUsbChargeWhileOn(" in m8_board_cpp
        and "digitalWrite(40, LOW)" in power_off_body
        and "digitalWrite(28, LOW)" in power_off_body
        and "digitalWrite(6, HIGH)" in power_off_body
        and "digitalWrite(6, LOW)" in power_off_body
        and "while (powerButtonHeld())" in power_off_body
        and "INPUT_DISABLE" in power_off_body
        and "midiEnd();" in ino_src
        and "midiUsbEnd();" in ino_src,
        "Board::powerOff() & handlePowerButton() unlatch Model:02 power (pin 28 LOW + pin 6 pulse), disable MIDI/OTG, and wait for button release",
    )

    # Verify Model:02 9x13 glyph table row counts and 1.5x width proportionality against Model:01 3x5 glyphs
    m02_part, m01_part = ui_font_cpp.split("#else", 1)
    m01_part = m01_part.split("#endif", 1)[0]
    row_str_re = re.compile(r'"([.#]+)"')
    m02_blocks = m02_part.split("{'")[1:]
    m01_blocks = m01_part.split("{'")[1:]
    m02_chars = [b[0] for b in m02_blocks]
    m01_chars = [b[0] for b in m01_blocks]
    check(
        len(m02_blocks) == 85
        and len(m01_blocks) == 85
        and len(set(m02_chars)) == 85
        and m02_chars == m01_chars,
        "ui_font.cpp defines all 85 unique glyphs for both Model:02 (9x13) and Model:01 (3x5) with no duplicate char keys",
    )
    for b02, b01 in zip(m02_blocks, m01_blocks):
        r02 = row_str_re.findall(b02)
        r01 = row_str_re.findall(b01)
        ch = b02[0]
        if not (len(r02) == 13 and len(set(len(r) for r in r02)) == 1 and len(r02[0]) == len(r01[0]) * 3):
            raise VerificationFailure(
                f"Glyph '{ch}' in Model:02 invalid: rows={len(r02)}, widths={set(len(r) for r in r02)}, expected_w={len(r01[0]) * 3}"
            )
    check(True, "All 85 Model:02 glyphs are 13px tall with uniform row widths and exact 1.5x width proportionality")

    # 1k. Timeline zoom, millisecond/subdivision locators, header/punch editing, and transport bar invariants
    app_h = (SRC_DIR / "app.h").read_text(encoding="utf-8")
    app_cpp = (SRC_DIR / "app.cpp").read_text(encoding="utf-8")
    check(
        all(z in config_h for z in ("ZOOM_4_BARS", "ZOOM_16_BARS", "ZOOM_32_BARS", "ZOOM_FULL", "WAVE_PEAK_FPS = 8"))
        and "cycleTimelineZoom()" in app_cpp
        and "drawTimelineTicksInWindow(" in pages_h,
        "4-level timeline zoom (4/16/32/Full bars), 8 fps peak bins, and timeline ticks are configured",
    )
    check(
        "HEADER_ROW = -1" in app_h
        and "HEADER_CELL_COUNT = 8" in app_h
        and "PUNCH_INFO_CELL_COUNT = 6" in app_h
        and "timeGranularityDeltaMs(" in input_cpp
        and "handleHeaderCellOptAdjust(" in input_cpp
        and "appStepTapeMs(" in app_cpp
        and "appStepTapeBar32nd(" in app_cpp
        and "appStepTapeBy32nds(" in app_cpp
        and '"RETURN TO ZERO"' not in input_cpp
        and '"RETURN TO ZERO"' not in all_pages_cpp,
        "Millisecond (MM:SS.mmm) & bar/beat/8/16/32 locator editing and Rewind/FF step helpers are present",
    )
    check(
        "TR_CELL_REC   = 0" in app_h
        and "TR_CELL_LOOP  = 1" in app_h
        and "TR_CELL_COUNT = 6" in app_h
        and "toggleLastArmedTrack()" in app_cpp
        and "if (armedRec || armedDub)" in app_cpp[app_cpp.find("void toggleLastArmedTrack()") :]
        and "isPunchOrLoopWindowActive()" in app_cpp
        and "ArmMode::Overdub" in app_cpp[app_cpp.find("void toggleLastArmedTrack()") :]
        and 's.drawText(leftX, TY(btnH), "|", punchFg);' in ui_cpp
        and "drawLoopArrowIcon(" in ui_cpp
        and "uint16_t lBg = loopOn ? pageAccent : (sel ? DARK : PANEL);" in ui_cpp
        and "const int sq = SY(10);" in ui_cpp
        and 's.drawText(inLblX, TY(stripH), "IN", DIM);' in ui_cpp
        and 's.drawText(outLblX, TY(stripH), "OUT", DIM);' in ui_cpp
        and "takeOutputPeaks(" in ui_cpp
        and "mt.muted = false;" in midi_seq_cpp[midi_seq_cpp.find("void midiSeqClearTrack(") : midi_seq_cpp.find("void midiSeqClearAll(")],
        "Transport bar implements Record/Punch button, Loop button, vertical IN & OUT level meters, and track clear unmute",
    )
    check(
        "NUM_STEREO_AUDIO_TRACKS = 4" in config_h
        and "NUM_MONO_AUDIO_TRACKS = 4" in config_h
        and "NUM_AUDIO_TRACKS = NUM_STEREO_AUDIO_TRACKS + NUM_MONO_AUDIO_TRACKS" in config_h
        and "NUM_MIDI_TRACKS = 8" in config_h
        and "VISIBLE_TRACK_ROWS = 4" in app_h
        and "TRACK_ROWS = 8" in app_h
        and "stepAudioTrackArm(" in app_cpp
        and "audioTrackArmTag(" in app_cpp
        and "syncAudioScroll()" in app_cpp
        and "syncMidiScroll()" in app_cpp,
        "8 Audio tracks (4 Stereo + 4 Mono with L/R/LR input routing) + 8 MIDI tracks with 4-row scrollable viewport are configured",
    )

    # 1l. Modular Track Edit Mode & SD-backed Clipboard invariants
    track_edit_h = (SRC_DIR / "track_edit.h").read_text(encoding="utf-8")
    track_edit_cpp = (SRC_DIR / "track_edit.cpp").read_text(encoding="utf-8")
    check(
        "TrackEditOpDesc" in track_edit_h
        and "TE_OP_QUANTIZE" in track_edit_h
        and all(op in track_edit_cpp for op in (
            '"SET PUNCH/LOOP REGION"',
            '"QUANTIZE"',
            '"4th"',
            '"8th"',
            '"16th"',
            '"32nd"',
            '"COPY"',
            '"CUT"',
            '"PASTE"',
            '"DUPL"',
            '"CLR"',
            '"LOAD AUDIO CLIP"',
        ))
        and "QUANT_REVEAL_MS = 1500" in track_edit_cpp
        and "twoBarsAfterFrame(" in track_edit_cpp
        and "twoBarsBeforeFrame(" in track_edit_cpp
        and "storageCopyAudioRegionToClip(" in storage_cpp
        and "storagePasteAudioClipToTrack(" in storage_cpp
        and "storageLoadWavFileToTrack(" in storage_cpp
        and "storageDeleteAudioRegion(" in storage_cpp
        and "midiSeqCopyRegionToClip(" in midi_seq_cpp
        and "midiSeqPasteClipToTrack(" in midi_seq_cpp
        and "midiSeqDeleteRegion(" in midi_seq_cpp
        and "midiSeqQuantizeRegion(" in midi_seq_cpp
        and "drawTrackEditOverlay(" in ui_cpp
        and "trackEditEnter(" in input_cpp
        and "trackEditToggleEndpoint(" in input_cpp,
        "Modular Track Edit Mode (SET PUNCH REGION, MIDI QUANTIZE 4th/8th/16th/32nd, COPY, CUT, PASTE, DUPL, DEL) with +/-2 bar auto-adjust and SD temp clipboard is configured",
    )

    # 1m. Per-Track Audio Recording Source ("SRC" split above "TRG": SRC, LIN, USB, M01-M08, A01-A08, PRE, PST + auto-mute) invariants
    engine_h = (SRC_DIR / "engine.h").read_text(encoding="utf-8")
    page_audio_cpp = (SRC_DIR / "page_audio.cpp").read_text(encoding="utf-8")
    check(
        "enum class AudioRecSrc : uint8_t" in engine_h
        and "isAutoMuteRecSrc(" in engine_h
        and "Resample = 2" not in engine_h
        and "AUD_CELL_SRC  = 1" in app_h
        and "AUD_CELL_TRIG = 2" in app_h
        and "AUD_CELL_COUNT = 6" in app_h
        and all(lbl in app_cpp for lbl in ('"SRC"', '"LIN"', '"USB"', '"M01"', '"M08"', '"A01"', '"A08"', '"PRE"', '"PST"'))
        and "stepAudioRecSrc(" in app_cpp
        and "drawTrackSrcBadgeCell(" in ui_cpp
        and "drawTrackSrcBadgeCell(" in page_audio_cpp
        and "drawTrackTrigBadgeCell(" in page_audio_cpp
        and '"RES"' not in ui_cpp
        and "drawResampleModal" not in ui_cpp
        and "openResampleModal" not in app_cpp
        and "TrigMode::Resample" not in page_audio_cpp
        and "preFxL" in engine_cpp
        and "postFxL" in engine_cpp
        and "midiCapL" in engine_cpp
        and "srcAudioL" in engine_cpp
        and "recording.autoMute" in engine_cpp
        and "recording.autoMute" in storage_cpp,
        "Per-track Audio Recording Source ('SRC' split above 'TRG': SRC/LIN/USB/M01..M08/A01..A08/PRE/PST with isolated MIDI/Audio paths & auto-mute) is configured",
    )

    # 1n. Master Page 8-Track Strips (Audio V/BAL/PAN/M/S & MIDI IN/OUT/M/S + double-tap OPT toggle + bottom FX/VOL)
    midi_seq_h = (SRC_DIR / "midi_seq.h").read_text(encoding="utf-8")
    check(
        "MASTER_ROW_TRK_PARAM1" in app_h
        and "MASTER_ROW_TRK_PARAM2" in app_h
        and "MASTER_ROW_TRK_MS" in app_h
        and "MASTER_ROW_FX" in app_h
        and "toggleMasterTrackMode(" in app_cpp
        and "toggleMasterTrackMode()" in input_cpp
        and "drawMasterTrackBlock(" in page_master_cpp
        and "handleAudioTrackSoloPress(" in page_master_cpp
        and "handleMidiTrackSoloPress(" in page_master_cpp
        and "anyAudioSolo" in engine_cpp
        and "anyMidiSolo" in midi_seq_cpp
        and "solo" in engine_h
        and "solo" in midi_seq_h,
        "Master page renders 8 Audio/MIDI track blocks (01..08, V/BAL/PAN or IN/OUT, M/S with Solo/Multi-Solo) above FX & VOL",
    )

    # 1o. Transport Play button speed (-5.00x..+5.00x in 0.25x steps) & reverse playback
    check(
        "MIN_SPEED_QUARTER = -20" in config_h
        and "MAX_SPEED_QUARTER = 20" in config_h
        and "DEFAULT_SPEED_QUARTER = 4" in config_h
        and "REVERSE_1X_SPEED_QUARTER = -4" in config_h
        and "speedQuarter" in engine_h
        and "storageSetPlaybackSpeedQuarter(" in storage_cpp
        and "appStepPlaybackSpeed(" in app_cpp
        and "appEnsureValidRecordingSpeed(" in app_cpp
        and "handleTransportCellOptAdjust(" in input_cpp,
        "Transport Play button supports -5.00x..+5.00x playback speed (0.25x steps), reverse playback, and OPT+EDIT speed reset",
    )

    # 1p. Unified Scrollable Instrument Editor (Specific controls first, scrolling down to AMP, FLT, FX)
    instruments_h = (SRC_DIR / "instruments.h").read_text(encoding="utf-8")
    page_inst_edit_cpp = (SRC_DIR / "page_instrument_edit.cpp").read_text(encoding="utf-8")
    check(
        "InstEnvParams" in instruments_h
        and "InstFxParams" in instruments_h
        and "INST_ENV_MAX = 127" in instruments_h
        and "VOICES_PER_INSTRUMENT = 8" in config_h
        and "NUM_FM_VOICES = 32" in config_h
        and "toggleInstEditView" not in input_cpp
        and "isInstGeneralView" not in page_inst_edit_cpp
        and "syncInstEditScroll()" in page_inst_edit_cpp
        and "instGenBaseRow()" in page_inst_edit_cpp
        and "INST_FX_COUNT  = 7" in instruments_h
        and all(lbl in page_inst_edit_cpp for lbl in ('"AMT"', '"ATK"', '"DEC"', '"SUS"', '"REL"', '"DRV"', '"CHOR"', '"DLAY"', '"D.FB"', '"RVRB"', '"TAPE"', '"COMP"'))
        and "instGetTape(" in instruments_cpp
        and "instSetTape(" in instruments_cpp
        and "instGetComp(" in instruments_cpp
        and "instSetComp(" in instruments_cpp
        and '"IFX1"' in instruments_cpp
        and '"IF1X"' in instruments_cpp
        and '"FX TAPE"' in midi_map_cpp
        and '"FX COMP"' in midi_map_cpp
        and "instNeedsSharedAuxSend(" in master_fx_cpp
        and "processInstDriveSample(" in master_fx_cpp
        and "processInstFxSample(" in master_fx_cpp,
        "Instrument Editor merges Specific & General controls into one scrollable page (no OPT double-tap toggle) with 8-voice polyphony/inst (32 global), AMP/FLT, and 7-slot FX (DRV->CHOR->DLAY->RVRB->TAPE->COMP)",
    )

    # 1q. Sampler Instrument (4-Octave C3..B6 Keyboard, PRESS: STOP/ONCE/REP/REPTHRU, SD WAV Browser & Streaming)
    inst_sampler_h = (SRC_DIR / "inst_sampler.h").read_text(encoding="utf-8")
    inst_sampler_cpp = (SRC_DIR / "inst_sampler.cpp").read_text(encoding="utf-8")
    check(
        "NUM_INST_TYPES = 5" in instruments_h
        and "Sampler = 3" in instruments_h
        and "SAMPLER_NUM_KEYS = 48" in inst_sampler_h
        and "SAMPLER_BASE_NOTE = 36" in inst_sampler_h
        and all(pm in inst_sampler_cpp for pm in ('"STOP"', '"ONCE"', '"REP"', '"REPTHRU"'))
        and all(kn in inst_sampler_cpp for kn in ('"C3"', '"B3"', '"C4"', '"B4"', '"C5"', '"B5"', '"C6"', '"B6"'))
        and "instSamplerEngine()" in inst_sampler_cpp
        and "drawSamplerKeyboard(" in page_inst_edit_cpp
        and "drawSamplerPressAndKeyStatus(" in page_inst_edit_cpp
        and "drawSamplerBrowserModal(" in page_inst_edit_cpp
        and "openSamplerBrowser(" in page_inst_edit_cpp
        and "isSamplerBrowserOpen()" in input_cpp
        and "samplerServiceStreams()" in storage_cpp
        and ("uint8_t rawBuf[512];" in inst_sampler_cpp or "storageSharedChunkBuf()" in inst_sampler_cpp),
        "Sampler instrument defines 48-key (C3..B6) scrollable horizontal keyboard, PRESS toggle (STOP/ONCE/REP/REPTHRU), SD WAV browser, and RAM1-staged streaming",
    )

    # 1r. Noise Texture Drone Instrument (DRONE OSC + NOISE BED + CRACKLE & MOD)
    inst_nd_h = (SRC_DIR / "inst_noisedrone.h").read_text(encoding="utf-8")
    inst_nd_cpp = (SRC_DIR / "inst_noisedrone.cpp").read_text(encoding="utf-8")
    check(
        "NoiseDrone = 4" in instruments_h
        and "NUM_ND_WAVES = 6" in inst_nd_h
        and "NUM_ND_NOISE_TYPES = 6" in inst_nd_h
        and "NUM_ND_TRIG_MODES = 3" in inst_nd_h
        and "NUM_ND_CRACKLE_TYPES = 4" in inst_nd_h
        and all(sec in inst_nd_cpp for sec in ('"NOISE DRONE"', '"NTD"', '"DRONE OSC"', '"NOISE BED"', '"CRACKLE & MOD"', 'outLabel = "PAN";', "ND_DEFAULT_PATCH", "NdWave::Grind", "{127, 17, 60, 127, 90}", "{0, 0, 0, 0, 0}"))
        and "instNoiseDroneEngine()" in inst_nd_cpp
        and '"NOISE TEXTURE DRONE"' in page_inst_type_cpp,
        "Noise Texture Drone instrument defines 3 sections (DRONE OSC, NOISE BED, CRACKLE & MOD), PAN control, Gate/Latch/Auto triggers, and default patch",
    )

    # 1s. Standard MIDI Transport/MMC SysEx + Custom Global MIDI CC Mappings & MIDI Learn
    midi_map_h = (SRC_DIR / "midi_map.h").read_text(encoding="utf-8")
    midi_map_cpp = (SRC_DIR / "midi_map.cpp").read_text(encoding="utf-8")
    check(
        "MidiTransportIn" in engine_h
        and '"MIDI TRANSPORT IN"' in page_song_cpp
        and "midiInSysEx(" in midi_in_cpp
        and "transportSourceAllows(" in midi_in_cpp
        and "EVT_MMC_PLAY" in midi_in_cpp
        and "EVT_MMC_REC_STROBE" in midi_in_cpp
        and "MIDI_TARGET_COUNT" in midi_map_h
        and "MIDI_CAT_NAVIGATION" in midi_map_h
        and "MIDI_TGT_NAV_EDIT" in midi_map_h
        and "MIDI_TGT_NAV_OPT" in midi_map_h
        and "MIDI_TGT_NAV_SHIFT" in midi_map_h
        and "MIDI_TGT_SET_LOOP_START" in midi_map_h
        and "MIDI_TGT_SET_LOOP_END" in midi_map_h
        and '"CAT: NAVIGATION"' in midi_map_cpp
        and '"OPT"' in midi_map_cpp
        and '"SHIFT"' in midi_map_cpp
        and '"SET LOOP START"' in midi_map_cpp
        and '"SET LOOP END"' in midi_map_cpp
        and "midiMapCategoryRowTarget(" in page_sys_cpp
        and "inputSetMidiModifierHeld(" in midi_map_cpp
        and "inputTriggerNav(" in midi_map_cpp
        and "inputTriggerMidiOptEdit(" in midi_map_cpp
        and "midiMapHandleCc(" in midi_map_cpp
        and "midiMapService(" in midi_map_cpp
        and "midiMapStartLearn(" in midi_map_cpp
        and "learnIgnoreUntilMs" in midi_map_cpp
        and "toggleCursorOrLastArmedTrack(" in midi_map_cpp
        and "drawSysMidiMapModal(" in page_sys_cpp
        and '"CLR CAT"' in page_sys_cpp
        and '"CLR ALL"' in page_sys_cpp
        and "globalMidiMap" in storage_cpp
        and "MIDIMAP.DAT" not in storage_cpp,
        "Standard MIDI Transport/MMC SysEx (MIDI TRANSPORT IN) and Custom Global MIDI CC Mappings (SYSTEM.DAT + NAVIGATION + CLR CAT/ALL + 1s Learn delay) are configured",
    )

    # 1t. 3-Row Track OPT+EDIT Overlay & Waveform Cell OPT+LEFT/RIGHT / OPT+UP/DOWN Locator Shortcuts
    check(
        all(lbl in ui_cpp for lbl in (
            '"RETURN TO ZERO"',
            '"SEEK TO IN"',
            '"SEEK TO OUT"',
            '"PUNCH ON"',
            '"PUNCH OFF"',
            '"LOOP ON"',
            '"LOOP OFF"',
            '"SET IN"',
            '"SET OUT"',
            '"ALL"',
            '"NOTES"',
            '"CC/PB"',
        ))
        and "isClearTrackModalRevealing()" in app_cpp
        and "clearTrackModalTick()" in app_cpp
        and "audioRestoreLastTrackCursor()" in app_cpp
        and "midiRestoreLastTrackCursor()" in app_cpp
        and "inputOptHeaderHighlightCell()" in input_cpp
        and "canOptScrubTimeline()" in input_cpp,
        "Track OPT+EDIT 3-row overlay (CLEAR/SEEK/PUNCH-LOOP) and global OPT+LEFT/RIGHT (ms) & OPT+UP/DOWN (beat) shortcuts are configured",
    )

    # 1u. Per-Instrument MIDI CC Mapping Subview (SHIFT+RIGHT from Instrument Edit, IMAP/IM1P persistence, external & routed CC control)
    check(
        "INST_MIDI_MAP_TOTAL_SLOTS" in midi_map_h
        and "instMidiMapApplyRoutedCc(" in midi_map_cpp
        and "instMidiMapApplyRoutedCc(" in instruments_cpp
        and "instMidiMapSaveSongData(" in midi_map_cpp
        and "instMidiMapLoadSongData(" in midi_map_cpp
        and "instMidiMapSaveSingleInstData(" in midi_map_cpp
        and "instMidiMapLoadSingleInstData(" in midi_map_cpp
        and '"IMAP"' in midi_map_cpp
        and '"IM1P"' in midi_map_cpp
        and "drawInstMidiMapView(" in page_inst_edit_cpp
        and "openInstMidiMap()" in page_inst_edit_cpp
        and "closeInstMidiMap()" in page_inst_edit_cpp
        and '"I%02d MIDI MAP: %s"' in page_inst_edit_cpp,
        "Per-Instrument MIDI CC Mapping page (SHIFT+RIGHT from Instrument Edit, IMAP/IM1P persistence, and external + routed track CC control) is configured",
    )

    # 1v. Audio Track Clear & WAV Truncation Safety (prevents cleared audio from resurrecting on record/load clip)
    clear_idx = storage_cpp.find("FLASHMEM void clearAudioTrack(int t)")
    clear_end = storage_cpp.find("AUDIO_CLIP_PATH", clear_idx)
    ensure_idx = storage_cpp.find("FLASHMEM static void ensureTrackWavOpen(int recTrk)")
    ensure_end = storage_cpp.find("FLASHMEM static void armRecordingStateForTrack(", ensure_idx)
    check(clear_idx != -1 and clear_end != -1 and ensure_idx != -1 and ensure_end != -1, "clearAudioTrack() and ensureTrackWavOpen() exist in storage.cpp")
    clear_body = storage_cpp[clear_idx:clear_end]
    ensure_body = storage_cpp[ensure_idx:ensure_end]
    check(
        "!songSaved" not in clear_body
        and "SD.sdfs.remove(wPath)" in clear_body
        and "O_TRUNC" in ensure_body
        and "tr.file.truncate()" in ensure_body,
        "clearAudioTrack() unconditionally removes/truncates track WAV on SD and ensureTrackWavOpen() truncates stale trailing bytes",
    )

    # 1w. Audio Gain Staging, Hybrid Headphone Volume, Dual HP/Speaker Master Vol Memory, Lookahead Limiting, Q14 Delay/Allpass Headroom, & Polyphonic Instrument Headroom invariants
    inst_fm2op_cpp = (SRC_DIR / "inst_fm2op.cpp").read_text(encoding="utf-8")
    inst_pd_cpp = (SRC_DIR / "inst_phasedist.cpp").read_text(encoding="utf-8")
    inst_nd_cpp = (SRC_DIR / "inst_noisedrone.cpp").read_text(encoding="utf-8")
    inst_smp_cpp = (SRC_DIR / "inst_sampler.cpp").read_text(encoding="utf-8")
    check(
        "HeadphoneGain" not in config_h
        and "HeadphoneGain" not in storage_h
        and "MASTER_ATTEN_STEP_DB = 0.3f" in config_h
        and "MASTER_HP_MIN_DB = -51.5f" in config_h
        and "MASTER_HP_MAX_ANA_DB = 6.0f" in config_h
        and "MASTER_HP_MAX_TOTAL_DB = 14.0f" in config_h
        and "masterHeadphoneStepDb(" in config_h
        and "MIN_LINE_IN_GAIN_TENTHS = -120" in config_h
        and "MAX_LINE_IN_GAIN_TENTHS = 120" in config_h
        and "DEFAULT_LINE_IN_GAIN_TENTHS = 0" in config_h
        and "masterVolHpEnc" in storage_h
        and "masterVolSpkEnc" in storage_h
        and "reservedHpGain" in storage_cpp
        and "appDefaultMasterVolume(" in app_cpp
        and "appSetMasterVolume(" in app_cpp
        and "recomputeHybridMasterGain(" in engine_cpp
        and "SGTL_HP_UNITY_VOL_INT = 104" in engine_cpp
        and "SGTL_HP_MAX_VOL_INT   = 116" in engine_cpp
        and "outputMeterScale" in engine_cpp
        and "(float)nextStep / 129.0f" in app_cpp
        and "softClipToInt16(" in engine_cpp
        and "PostFxLimiter" in engine_cpp
        and "LookaheadMonitorLimiter" in master_fx_cpp
        and "rawPostFxL" in engine_cpp
        and ") * masterGain;" in engine_cpp
        and "appServiceCodecHardware(" in app_cpp
        and "readDelayTapQ14(" in master_fx_cpp
        and "softLimitQ14(" in master_fx_cpp
        and "softLimitVoiceSvf(" in instruments_cpp
        and "instBusLimiter" in instruments_cpp
        and "cyclesToPhase32(" in inst_fm2op_cpp
        and "4500.0f" in inst_fm2op_cpp
        and "4500.0f" in inst_pd_cpp
        and "4500.0f" in inst_nd_cpp
        and "SAMPLER_VOICE_BASE_GAIN = 0.25f" in inst_smp_cpp
        and "declickRem" in inst_smp_cpp
        and "NUM_SAMPLER_STREAMS = 8" in inst_smp_cpp
        and '"OUT: OUTPUT LEVEL"' in page_inst_edit_cpp
        and '"OUT LEVEL"' in midi_map_cpp,
        "Unified Option A SGTL5000 headphone volume (-51.5..0..+6/+14 dB, default 50 HP / 40 Speaker), 1:1 0.5 dB PGA slewing, dual HP/Speaker memory, Q14/Allpass & polyphonic synth/sampler headroom are configured",
    )

    # 1x. Song Settings Chromatic Tuner (LINE IN, USB IN, I01-I08 pre-Drive, A01-A08, per-Song A4 REF, LIVE/MUTE)
    tuner_h = (SRC_DIR / "tuner.h").read_text(encoding="utf-8")
    tuner_cpp = (SRC_DIR / "tuner.cpp").read_text(encoding="utf-8")
    inst_tuner_idx = instruments_cpp.find("tunerPushStereo32(")
    inst_drive_idx = instruments_cpp.find("applyInstDriveBlock(inst,", inst_tuner_idx if inst_tuner_idx != -1 else 0)
    check(
        re.search(r"PSET_DEF_THRESH\s*=\s*2\b", app_h) is not None
        and re.search(r"PSET_TUNER\s*=\s*5\b", app_h) is not None
        and '"CHROMATIC TUNER"' in page_song_cpp
        and 'songTunerActive ? "TUNER" : "SONG"' in page_song_cpp
        and '"SOURCE: %s"' in page_song_cpp
        and '"A4 REF: %u.%u HZ"' in page_song_cpp
        and '"LIVE"' in page_song_cpp
        and '"MUTE"' in page_song_cpp
        and '"IN LVL"' in page_song_cpp
        and "TUNER_SELECTABLE_SOURCES = 18" in tuner_h
        and "MIN_TUNER_REF_TENTHS     = 4000" in tuner_h
        and "MAX_TUNER_REF_TENTHS     = 4800" in tuner_h
        and "DEFAULT_TUNER_REF_TENTHS = 4400" in tuner_h
        and "tunerMatchRefToCurrentPitch(" in tuner_cpp
        and "fmSynthReleaseDirectVoices(" in tuner_cpp
        and "liveAuditionInstIdx(" in midi_seq_cpp
        and "tunerRefTenthsEnc" in storage_cpp
        and inst_tuner_idx != -1
        and inst_drive_idx != -1
        and inst_tuner_idx < inst_drive_idx,
        "Song Settings Chromatic Tuner (PSET_TUNER below REC TRIG THRESH, 'TUNER' header, LINE IN/USB IN/I01-I08 pre-Drive + external MIDI trigger/A01-A08, per-Song A4 REF, pitch-match, and LIVE/MUTE) is configured",
    )

    # 1y. SoundFont 2.x (.sf2) Internal Instrument (Hybrid RAM1 Pre-Roll Cache + Shared Ring-Locked SDIO Streamer)
    inst_sf2_h = (SRC_DIR / "inst_sf2.h").read_text(encoding="utf-8")
    inst_sf2_cpp = (SRC_DIR / "inst_sf2.cpp").read_text(encoding="utf-8")
    sf2_dma_pattern = re.compile(
        r"\.\s*(?:read|write)\s*\(\s*(?:sf2ZonePool|sf2SlotFile|sf2SlotDir|sf2Voices|sf2InstZoneIds|sf2ModPool|sf2InstModIds|sf2ZoneModGroup)"
    )
    check(
        not sf2_dma_pattern.search(inst_sf2_cpp),
        "inst_sf2.cpp never passes DMAMEM buffers (sf2ZonePool/sf2SlotFile/sf2Voices) directly to FsFile::read/write",
    )
    check(
        'SOUNDFONTS_DIR = "/Capstan/SOUNDFONTS"' in config_h
        and "SoundFont = 5" in instruments_h
        and "instSoundFontEngine()" in inst_sf2_cpp
        and "instSoundFontEngine()" in instruments_cpp
        and all(
            tok in inst_sf2_cpp
            for tok in (
                '"SOUNDFONT"',
                '"SF2"',
                '"SOUNDFONT FILE"',
                '"BANK & PRESET"',
                '"TONE & PLAYBACK"',
                'outLabel = "TUNE";',
                'outLabel = "LOOP";',
                'outLabel = "ENV";',
                'outLabel = "INTP";',
                'outLabel = "VEL";',
                'outLabel = "PAN";',
                '"HQ"',
                '"LIN"',
                '"ADSR"',
                '"SF21"',
                '"SF1P"',
                "storageSharedChunkBuf()",
                "storageKeepTracksStreaming()",
                "sf2PreRoll",
                "sf2PreRollInfo",
                "sf2FilterAlphaLut",
                "SF2_MAX_ZONES_PER_INST   = 254",
                "sizeof(Sf2Zone) == 48",
                "sizeof(Sf2ZoneAux) == 4",
                '"pmod"',
                '"imod"',
                "sf2EvalModSource(",
                "sf2ApplyVoiceMods(",
                "sf2RecomputeLiveMods(",
                "sf2RefreshLivePatchParams(",
                "sf2EmitOrReuseModGroup(",
                "sf2PresetModRowCount(",
                "sf2InstModRowCount(",
            )
        )
        and "Aftertouch" not in instruments_h
        and "Aftertouch" not in inst_sf2_h
        and "SF2_MOD_COLS_PER_ROW = 4" in inst_sf2_h
        and "SF2_NONCC_AFTERTOUCH" not in inst_sf2_cpp
        and "INST_EDIT_CACHE_SLOTS = 1 + INST_EDIT_MAX_VIS_BODY_ROWS" in page_inst_edit_cpp
        and "drawSf2ModParamCell(" in page_inst_edit_cpp
        and '"PRESET MODULATORS"' in page_inst_edit_cpp
        and '"INSTRUMENT MODULATORS"' in page_inst_edit_cpp
        and "loopLockedInRing" in inst_sampler_cpp
        and "packStereoSpan(" in inst_sampler_cpp
        and "samplerOpenSf2Stream(" in inst_sampler_cpp
        and "nextSharedStreamGen()" in inst_sampler_cpp
        and "reapOrphanedActiveStreams()" in inst_sampler_cpp
        and "samplerPrewarmSf2File(" in inst_sampler_cpp
        and "drawSf2BrowserModal(" in page_inst_edit_cpp
        and "sf2BrowserMode()" in ui_cpp
        and "const int wL = SX(214);" in page_inst_edit_cpp
        and "isSf2BrowserOpen()" in input_cpp
        and "fmSynthAudition(" not in page_inst_edit_cpp
        and "fmSynthAudition(" not in input_cpp[input_cpp.find("confirmSf2BrowserSelection()") : input_cpp.find("confirmTypePresetSelection()")]
        and "triggerSamplerKeyAuditionHold(" in input_cpp
        and "sf2ServiceStreams()" in storage_cpp
        and "sf2ServiceStreams(true)" in storage_cpp
        and "uiClearRegionStreaming(" in ui_cpp
        and "keepStreaming(true)" in ui_cpp
        and "keepStreaming()" in (SRC_DIR / "page_main.cpp").read_text(encoding="utf-8")
        and '"SF2 PRESET"' in midi_map_cpp
        and '"INTERP"' in midi_map_cpp,
        "SoundFont 2.x (.sf2) instrument defines /Capstan/SOUNDFONTS browser, preset selector, TUNE/LOOP/ENV(SF2|ADSR)/INTP(HQ|LIN)/VEL/PAN controls, in-ring loop lock, MIDI CC map, and SF21/SF1P persistence",
    )

    # 1z. Per-Audio-Track FX Chain (Drive -> Chorus -> Delay -> Reverb -> Tape -> Comp) & OPT Double-Tap Cycling
    master_fx_h = (SRC_DIR / "master_fx.h").read_text(encoding="utf-8")
    check(
        "AudioTrackFxSlot" in master_fx_h
        and "AudioTrackFxParams" in master_fx_h
        and "prepareAudioTrackFxBlock(" in engine_cpp
        and "processAudioTrackInsertSample(" in engine_cpp
        and "processAudioTrackAuxSample(" in engine_cpp
        and "AUD_PMODE_COUNT    = 5" in app_h
        and "cycleAudioParamMode()" in input_cpp
        and all(lbl in page_audio_cpp for lbl in ('"DRV"', '"CHR"', '"DLY"', '"DFB"', '"RVB"', '"TAP"', '"COMP"'))
        and "overview[2] == 0xF1" in storage_cpp
        and "anyAudioTrackDelayOrReverbActive()" in storage_cpp
        and "MIDI_TGT_AUD_FX_BASE" in midi_map_h
        and "VERB_AP2_L_LEN = 2076" in master_fx_cpp
        and "VERB_AP2_R_LEN = 1912" in master_fx_cpp
        and "else if (dInt > bufLen - 2) dInt = bufLen - 2;" in master_fx_cpp,
        "Per-Audio-Track FX chain (Drive -> Chorus -> Delay -> Reverb -> Tape -> Comp), Audio page OPT double-tap 5-mode cycling, SONG.DAT persistence, and MIDI CC mappings are configured",
    )

    # 1aa. Audio Input FX Chain (Line In + USB In: Drive -> Chorus -> Delay -> Reverb -> Tape -> Comp -> Out Vol, Pre-Monitor & Pre-Rec)
    eng_tuner_idx = engine_cpp.find("pushTunerInputBlock(tSrc,")
    eng_infx_idx = engine_cpp.find("processAudioInputFxBlock(inFxCtx,")
    eng_rec_idx = engine_cpp.find("int16_t recSampleL[AUDIO_BLOCK_SAMPLES];", eng_infx_idx if eng_infx_idx != -1 else 0)
    check(
        re.search(r"PSET_LINE_IN_GAIN\s*=\s*0\b", app_h) is not None
        and re.search(r"PSET_AUDIO_INPUT_FX\s*=\s*1\b", app_h) is not None
        and re.search(r"PSET_DEF_THRESH\s*=\s*2\b", app_h) is not None
        and '"AUDIO INPUT FX"' in page_song_cpp
        and '"OUT VOL"' in page_song_cpp
        and "drawSongInputFxView(" in page_song_cpp
        and "isSongInputFxOpen()" in ui_cpp
        and "isSongInputFxOpen()" in input_cpp
        and "updateAudioInputFxTargets(" in engine_cpp
        and "getAudioInputVolume(" in engine_cpp
        and "setAudioInputVolume(" in engine_cpp
        and "setAudioInputOutGain(" in master_fx_cpp
        and eng_tuner_idx != -1
        and eng_infx_idx != -1
        and eng_rec_idx != -1
        and eng_tuner_idx < eng_infx_idx < eng_rec_idx
        and "dp.audio[0].overview[10] == 0xF2" in storage_cpp
        and "dp.audio[0].overview[18]" in storage_cpp
        and "MIDI_TGT_IN_FX_BASE" in midi_map_h
        and '"IN OUT VOL"' in midi_map_cpp,
        "Audio Input FX chain (Drive -> Chorus -> Delay -> Reverb -> Tape -> Comp -> Out Vol [80 unity], pre-monitor & pre-record, post-Tuner), Song Settings top rows (LINE IN GAIN, AUDIO INPUT FX, REC TRIG THRESH), SONG.DAT persistence, and MIDI CC mappings are configured",
    )

    # 1ab. SONG.DAT v3 compact schema & legacy v2 read compatibility
    check(
        "struct DiskAudioTrackV3" in storage_cpp
        and "struct DiskSongV3" in storage_cpp
        and "DISK_SONG_VERSION = 3" in storage_cpp
        and "sizeof(DiskAudioTrackV3) == 20" in storage_cpp
        and "sizeof(DiskSongV3) == 232" in storage_cpp
        and "ver == 2 || ver == DISK_SONG_VERSION" in storage_cpp
        and "DiskSongV3 *sp = readDiskSongFromFolder(" in storage_cpp
        and "DiskSongV3 *spPtr = readDiskSongFromFolder(" in storage_cpp,
        "storage.cpp implements SONG.DAT v3 (232-byte single-sector schema with typed track/input FX and recSrc) while preserving full v2 read compatibility",
    )


def verify_wav_and_binary_headers():
    print("\n=== 2. Binary Format & WAV Sector-Alignment Checks ===")
    config_h = (SRC_DIR / "config.h").read_text(encoding="utf-8")
    m = re.search(r"WAV_DATA_OFFSET\s*=\s*(\d+)", config_h)
    check(m is not None and int(m.group(1)) == 512, "WAV_DATA_OFFSET is 512 bytes (1 SD sector)")

    # Simulate 512-byte WAV header construction from storage.cpp
    stereo_frames = 44100 * 10
    data_bytes = stereo_frames * 4
    riff_size = (512 - 8) + data_bytes
    junk_size = 512 - 44 - 8
    hdr = bytearray(512)
    hdr[0:4] = b"RIFF"
    struct.pack_into("<I", hdr, 4, riff_size)
    hdr[8:12] = b"WAVE"
    hdr[12:16] = b"fmt "
    struct.pack_into("<IHHIIHH", hdr, 16, 16, 1, 2, 44100, 44100 * 4, 4, 16)
    hdr[36:40] = b"JUNK"
    struct.pack_into("<I", hdr, 40, junk_size)
    hdr[504:508] = b"data"
    struct.pack_into("<I", hdr, 508, data_bytes)
    check(
        len(hdr) == 512 and hdr[0:4] == b"RIFF" and hdr[504:508] == b"data",
        "512-byte sector-aligned RIFF WAVE header layout is valid",
    )

    # Verify SONG.DAT v3 single-sector binary layout (72 B header + 8 x 20 B track descriptors = 232 B)
    song_v3_hdr = struct.pack(
        "<4sHH40sIBBH7sB8s",
        b"M8PR",
        3,
        232,
        b"\x00" * 40,
        0,
        0,
        0,
        0,
        b"\x00" * 7,
        80,
        b"\x00" * 8,
    )
    song_v3_trk = struct.pack("<bbBBbBBBB7sI", 80, 0, 0, 0, -60, 0, 0, 0, 0, b"\x00" * 7, 44100)
    check(
        len(song_v3_hdr) == 72
        and len(song_v3_trk) == 20
        and (len(song_v3_hdr) + 8 * len(song_v3_trk)) == 232
        and song_v3_hdr[:4] == b"M8PR",
        "SONG.DAT v3 packed binary layout (72B header + 8x20B track descriptors = 232B single-sector record) is valid",
    )

    # Verify SoundFont 2.x RIFF 'sfbk' / 'pdta' Hydra packed struct sizes match SF2.01 specification
    phdr_rec = struct.pack("<20sHHHIII", b"Grand Piano\x00" + b"\x00" * 8, 0, 0, 0, 0, 0, 0)
    bag_rec = struct.pack("<HH", 0, 0)
    mod_rec = struct.pack("<HHhHH", 0, 0, 0, 0, 0)
    gen_rec = struct.pack("<Hh", 58, 60)
    inst_rec = struct.pack("<20sH", b"PianoInst\x00" + b"\x00" * 10, 0)
    shdr_rec = struct.pack("<20sIIIIIBbHH", b"Piano_C4\x00" + b"\x00" * 11, 0, 64, 8, 56, 44100, 60, 0, 0, 1)
    check(
        len(phdr_rec) == 38
        and len(bag_rec) == 4
        and len(mod_rec) == 10
        and len(gen_rec) == 4
        and len(inst_rec) == 22
        and len(shdr_rec) == 46,
        "SoundFont 2.x RIFF 'sfbk' Hydra record layouts (phdr=38B, bag=4B, mod=10B, gen=4B, inst=22B, shdr=46B) match SF2.01 spec",
    )


def parse_elf32_symbols(elf_bytes: bytes) -> tuple[dict, dict]:
    if elf_bytes[:4] != b"\x7fELF":
        return {}, {}
    e_shoff, _, _, _, _, e_shentsize, e_shnum, _ = struct.unpack_from("<IIHHHHHH", elf_bytes, 32)
    sections = []
    for i in range(e_shnum):
        off = e_shoff + i * e_shentsize
        sh_name, sh_type, sh_flags, sh_addr, sh_offset, sh_size, sh_link, _, _, sh_entsize = (
            struct.unpack_from("<IIIIIIIIII", elf_bytes, off)
        )
        sections.append((sh_type, sh_offset, sh_size, sh_link, sh_entsize))

    symbols = {}
    sizes = {}
    for sh_type, sh_offset, sh_size, sh_link, sh_entsize in sections:
        if sh_type == 2 and sh_entsize == 16 and 0 <= sh_link < len(sections):  # SHT_SYMTAB
            _, str_off, str_size, _, _ = sections[sh_link]
            strtab = elf_bytes[str_off : str_off + str_size]
            count = sh_size // 16
            for j in range(count):
                st_name, st_value, st_size, st_info, st_other, st_shndx = struct.unpack_from(
                    "<IIIBBH", elf_bytes, sh_offset + j * 16
                )
                if st_name < len(strtab):
                    end = strtab.find(b"\x00", st_name)
                    if end != -1:
                        sym_name = strtab[st_name:end].decode("ascii", errors="ignore")
                        if sym_name:
                            symbols[sym_name] = st_value
                            sizes[sym_name] = st_size
    return symbols, sizes


def verify_elf_symbols(selected_targets: list[str], explicit_targets: bool):
    print("\n=== 3. Teensy 4.0 / MicroMod ELF Symbol & Memory Region Verification ===")
    src_inputs = [ROOT / SKETCH_NAME]
    src_inputs.extend(SRC_DIR.glob("*.h"))
    src_inputs.extend(SRC_DIR.glob("*.cpp"))
    src_inputs.extend(SRC_DIR.glob("*.c"))
    teensy_msc_dir = ROOT / "tools" / "teensy_msc"
    if teensy_msc_dir.exists():
        src_inputs.extend(teensy_msc_dir.glob("*.c*"))
    latest_src_mtime = max(p.stat().st_mtime for p in src_inputs if p.exists())

    found_any = False
    for key in selected_targets:
        label, elf_path = TARGET_ELFS[key]
        if not elf_path.exists():
            if explicit_targets:
                check(False, f"[{label}] Built ELF exists at {elf_path}")
            continue
        found_any = True
        elf_mtime = elf_path.stat().st_mtime
        check(
            elf_mtime >= latest_src_mtime - 1.0,
            f"[{label}] ELF binary is up-to-date with current source files",
        )
        syms, sizes = parse_elf32_symbols(elf_path.read_bytes())
        ram1_chunk_addr = None
        ram2_event_pool_addr = None
        ram2_rings_addr = None
        ram1_input_ring_addr = None
        ram1_tuner_ring_addr = None
        ram2_sin_lut_addr = None
        ram1_sf2_preroll_addr = None
        ram2_sf2_zone_pool_addr = None
        usb_tx_ring_addr = syms.get("_ZL7tx_ring")
        usb_tx_ring_size = sizes.get("_ZL7tx_ring")
        usb_midi_tx_buf_addr = syms.get("_ZL12usbMidiTxBuf")

        for name, addr in syms.items():
            if "looper" in name and "chunk" in name:
                ram1_chunk_addr = addr
            elif "looper" in name and "eventPool" in name:
                ram2_event_pool_addr = addr
            elif "looper" in name and "rings" in name:
                ram2_rings_addr = addr
            elif "looper" in name and "inputRing" in name:
                ram1_input_ring_addr = addr
            elif "looper" in name and "tunerRing" in name:
                ram1_tuner_ring_addr = addr
            elif "looper" in name and "sinLut" in name:
                ram2_sin_lut_addr = addr
            elif "sf2PreRoll" in name:
                ram1_sf2_preroll_addr = addr
            elif "sf2ZonePool" in name:
                ram2_sf2_zone_pool_addr = addr

        check(
            ram1_chunk_addr is not None and 0x20000000 <= ram1_chunk_addr < 0x20080000,
            f"[{label}] looper::chunk is in uncached RAM1 DTCM (0x{ram1_chunk_addr or 0:08X}) for DMA_SDIO safety",
        )
        check(
            ram1_input_ring_addr is not None and 0x20000000 <= ram1_input_ring_addr < 0x20080000,
            f"[{label}] looper::inputRing is in RAM1 DTCM (0x{ram1_input_ring_addr or 0:08X})",
        )
        check(
            ram1_tuner_ring_addr is not None and 0x20000000 <= ram1_tuner_ring_addr < 0x20080000,
            f"[{label}] looper::tunerRing (4 KB decimated tuner capture buffer) is in RAM1 DTCM (0x{ram1_tuner_ring_addr or 0:08X}), preserving RAM2 budget",
        )
        check(
            ram2_rings_addr is not None and 0x20200000 <= ram2_rings_addr < 0x20280000,
            f"[{label}] looper::rings (252 KB audio track rings) is in RAM2 DMAMEM (0x{ram2_rings_addr or 0:08X})",
        )
        check(
            ram2_event_pool_addr is not None and 0x20200000 <= ram2_event_pool_addr < 0x20280000,
            f"[{label}] looper::eventPool (128 KB MIDI events) is in RAM2 DMAMEM (0x{ram2_event_pool_addr or 0:08X})",
        )
        check(
            ram2_sin_lut_addr is not None and 0x20200000 <= ram2_sin_lut_addr < 0x20280000,
            f"[{label}] looper::sinLut (4 KB FM sine LUT) is in cached RAM2 DMAMEM (0x{ram2_sin_lut_addr or 0:08X})",
        )
        check(
            ram1_sf2_preroll_addr is not None and 0x20000000 <= ram1_sf2_preroll_addr < 0x20080000,
            f"[{label}] sf2PreRoll (41.1 KB zero-latency SF2 attack/micro-loop cache) is in uncached RAM1 DTCM (0x{ram1_sf2_preroll_addr or 0:08X})",
        )
        check(
            ram2_sf2_zone_pool_addr is not None and 0x20200000 <= ram2_sf2_zone_pool_addr < 0x20280000,
            f"[{label}] sf2ZonePool (12.2 KB SF2 Hydra zone descriptors, 254 zones) is in cached RAM2 DMAMEM (0x{ram2_sf2_zone_pool_addr or 0:08X})",
        )
        check(
            usb_tx_ring_addr is not None
            and 0x20200000 <= usb_tx_ring_addr < 0x20280000
            and usb_tx_ring_size == 768 * 2 * 2,
            f"[{label}] usb_audio_l7 tx_ring is 3,072 B (768 frames x 2 ch x 2 B, got {usb_tx_ring_size} B) in RAM2 DMAMEM (0x{usb_tx_ring_addr or 0:08X})",
        )
        check(
            usb_midi_tx_buf_addr is not None and 0x20000000 <= usb_midi_tx_buf_addr < 0x20080000,
            f"[{label}] midi_usb usbMidiTxBuf is in uncached RAM1 DTCM (0x{usb_midi_tx_buf_addr or 0:08X})",
        )
        ep0_setup_addr = syms.get("endpoint0_setup")
        ep0_complete_addr = syms.get("endpoint0_complete")
        etext_addr = syms.get("_etext")
        ebss_addr = syms.get("_ebss")
        estack_addr = syms.get("_estack")
        heap_start_addr = syms.get("_heap_start")
        heap_end_addr = syms.get("_heap_end")
        check(
            ep0_setup_addr is not None
            and ep0_setup_addr >= 0x60000000
            and ep0_complete_addr is not None
            and ep0_complete_addr >= 0x60000000,
            f"[{label}] Cold USB EP0 handlers (endpoint0_setup=0x{ep0_setup_addr or 0:08X}, endpoint0_complete=0x{ep0_complete_addr or 0:08X}) reside in FLASHMEM (not inlined into ITCM usb_isr)",
        )
        check(
            etext_addr is not None and etext_addr <= 131072,
            f"[{label}] ITCM code end (_etext=0x{etext_addr or 0:08X}, {etext_addr or 0} B) stays within 4 banks (<= 131,072 B, {131072 - (etext_addr or 0)} B free in bank 4)",
        )
        dtcm_free = (estack_addr - ebss_addr) if (estack_addr is not None and ebss_addr is not None) else -1
        check(
            ebss_addr is not None and estack_addr is not None and dtcm_free >= 64000,
            f"[{label}] DTCM free stack margin is >= 64,000 B ({dtcm_free} B free: _ebss=0x{ebss_addr or 0:08X}, _estack=0x{estack_addr or 0:08X})",
        )
        ram2_free = (heap_end_addr - heap_start_addr) if (heap_end_addr is not None and heap_start_addr is not None) else -1
        check(
            heap_start_addr is not None and heap_end_addr is not None and ram2_free >= 32000,
            f"[{label}] RAM2 (DMAMEM) free margin is >= 32,000 B ({ram2_free} B free)",
        )

    if not found_any:
        print("  [SKIP] No ELF binaries found in build/ (compile firmware first)")


def verify_public_release_cleanliness():
    print("\n=== 4. Public Release Cleanliness & Sanitization Checks ===")
    this_file = Path(__file__).resolve()
    scan_paths = [
        ROOT / "Capstan.ino",
        ROOT / "build_all.py",
        ROOT / "README.md",
        ROOT / "GEMINI.md",
    ]
    for sub in ("src", "docs", "tools", ".agents"):
        d = ROOT / sub
        if d.exists():
            for p in sorted(d.rglob("*")):
                if p.is_file() and p.resolve() != this_file and "__pycache__" not in p.parts:
                    scan_paths.append(p)

    forbidden = [
        ("brand reference", re.compile(r"dirty" + r"wave", re.I)),
        ("firmware image name", re.compile(r"M8_V6_", re.I)),
        ("restricted analysis terminology", re.compile(r"disas" + r"sembl|decom" + r"pil|reverse[- ]eng" + r"ineer", re.I)),
        ("third-party file reference", re.compile(r"ableton" + r"\.cpp", re.I)),
        ("local user path", re.compile(r"C:\\Users\\" + r"shawn", re.I)),
    ]

    violations = []
    for p in scan_paths:
        if not p.exists():
            continue
        text = p.read_text(encoding="utf-8", errors="ignore")
        for label, pat in forbidden:
            m = pat.search(text)
            if m:
                rel = p.relative_to(ROOT)
                violations.append(f"{rel}: matched {label} ('{m.group(0)}')")

    check(
        len(violations) == 0,
        "All source, tool, doc, and skill files are free of third-party brand, external binary, and local user path references"
        + ("" if not violations else f" — violations: {'; '.join(violations[:5])}"),
    )


def parse_args():
    parser = argparse.ArgumentParser(
        description="Automated architectural, memory, DMA-safety, and storage invariant verifier for Capstan."
    )
    parser.add_argument(
        "targets",
        nargs="*",
        help="Optional target(s) to verify: model01, model02 (default: both)",
    )
    return parser.parse_args()


def main():
    ns = parse_args()
    explicit_targets = len(ns.targets) > 0
    if explicit_targets:
        selected_targets = []
        for t in ns.targets:
            k = t.lower()
            if k not in TARGET_ELFS:
                print(
                    f"[FAIL] Unknown target '{t}'. Choose from: {', '.join(TARGET_ELFS.keys())}",
                    file=sys.stderr,
                )
                sys.exit(1)
            if k not in selected_targets:
                selected_targets.append(k)
    else:
        selected_targets = ["model01", "model02"]

    try:
        verify_dma_and_storage_source_invariants()
        verify_wav_and_binary_headers()
        verify_elf_symbols(selected_targets, explicit_targets)
        verify_public_release_cleanliness()
        print("\nAll Capstan firmware verification checks PASSED.")
    except VerificationFailure as e:
        print(f"\n[FAIL] {e}", file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main()
