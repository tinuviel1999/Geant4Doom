// SPDX-License-Identifier: GPL-2.0-or-later
// Doom's music the way a Sound Blaster played it: the WAD's MIDI songs (D_*.mid) sequenced onto
// an emulated OPL3 FM chip (Nuked-OPL3), with the WAD's own instrument bank (GENMIDI), driven
// the way DMX (Doom's sound library) drove the chip. The driver logic (voice allocation,
// volume curve, pitch table, double voices) follows Chocolate Doom's i_oplmusic.c, which
// reproduces DMX; this is a fresh implementation of that behaviour, not a copy.
//
// Thread use: Load/Stop from the GUI thread, Render from the audio thread; Render is the only
// one that touches the chip, and a mutex guards the song.
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <vector>

extern "C" {
#include "opl3.h"
}

class DoomMusic
{
 public:
  explicit DoomMusic(double rate) : fRate(rate) {}

  // The instrument bank: the GENMIDI lump ("#OPL_II#", 128 melodic + 47 percussion instruments)
  bool LoadInstruments(const std::filesystem::path& file)
  {
    const auto bytes = ReadFile(file);
    if (bytes.size() < 8 + kInstruments * 36 || std::memcmp(bytes.data(), "#OPL_II#", 8)) return false;
    for (int i = 0; i < kInstruments; ++i) {
      const std::uint8_t* p = bytes.data() + 8 + 36 * i;
      Instrument& in = fInstruments[i];
      in.flags = p[0] | p[1] << 8;
      in.fineTuning = p[2];
      in.fixedNote = p[3];
      for (int v = 0; v < 2; ++v) {
        const std::uint8_t* q = p + 4 + 16 * v;
        auto op = [](const std::uint8_t* o) { return Operator{o[0], o[1], o[2], o[3], o[4], o[5]}; };
        in.voices[v].modulator = op(q);
        in.voices[v].feedback = q[6];
        in.voices[v].carrier = op(q + 7);
        in.voices[v].noteOffset = static_cast<std::int16_t>(q[14] | q[15] << 8);
      }
    }
    fHaveInstruments = true;
    return true;
  }

  // Start a MIDI file, looping; false if it can't be read (the old song keeps playing)
  bool Load(const std::filesystem::path& file)
  {
    auto song = std::make_unique<Song>();
    if (!fHaveInstruments || !ParseMidi(ReadFile(file), *song)) return false;
    std::lock_guard<std::mutex> lock(fMutex);
    fSong = std::move(song);
    fRestart = true;
    return true;
  }

  void Stop()
  {
    std::lock_guard<std::mutex> lock(fMutex);
    fSong.reset();
    fRestart = true;
  }

  // Add `frames` stereo frames of music, times gain, to interleaved floats
  void Render(float* out, std::size_t frames, float gain)
  {
    std::lock_guard<std::mutex> lock(fMutex);
    if (fRestart) {
      fRestart = false;
      Reset();
      if (!fSong) return;
    }
    if (!fSong) return;
    for (std::size_t f = 0; f < frames; ++f) {
      Sequence();
      std::int16_t s[2];
      OPL3_GenerateResampled(&fChip, s);
      out[2 * f] += s[0] / 32768.f * gain;
      out[2 * f + 1] += s[1] / 32768.f * gain;
    }
  }

 private:
  static constexpr int kInstruments = 175, kPercussion = 128;  // percussion: keys 35..81
  static constexpr int kVoices = 18;                            // OPL3: two banks of nine

  struct Operator
  {
    std::uint8_t tremolo, attack, sustain, waveform, scale, level;
  };
  struct InstrumentVoice
  {
    Operator modulator, carrier;
    std::uint8_t feedback;
    std::int16_t noteOffset;
  };
  struct Instrument
  {
    std::uint16_t flags;  // 1: fixed pitch, 4: two voices
    std::uint8_t fineTuning, fixedNote;
    InstrumentVoice voices[2];
  };
  struct Channel
  {
    const Instrument* instrument;
    int volume, pan, bend;
  };
  struct Voice
  {
    int index, op1, op2, bank;  // register numbers
    const Instrument* instrument = nullptr;
    int instrumentVoice = 0;
    int channel = -1, key = 0, note = 0, noteVolume = 0;
    unsigned freq = 0, carVolume = 0, modVolume = 0, pan = 0x30, priority = 0;
  };
  struct Event
  {
    std::uint32_t tick;
    std::uint8_t status, data1, data2;  // status 0xff: tempo
    std::uint32_t tempo;
  };
  struct Song
  {
    std::vector<Event> events;  // all tracks, merged in time order
    double division = 96;       // ticks per beat
  };

  // --- the MIDI file ---

  static std::vector<std::uint8_t> ReadFile(const std::filesystem::path& file)
  {
    std::ifstream in(file, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
  }

  static bool ParseMidi(const std::vector<std::uint8_t>& b, Song& song)
  {
    auto u32 = [&](std::size_t p) { return std::uint32_t(b[p]) << 24 | b[p + 1] << 16 | b[p + 2] << 8 | b[p + 3]; };
    if (b.size() < 14 || std::memcmp(b.data(), "MThd", 4)) return false;
    const int tracks = b[10] << 8 | b[11], division = b[12] << 8 | b[13];
    song.division = division & 0x8000 ? 96 : std::max(1, division);  // SMPTE timing: not in Doom's songs
    std::size_t p = 8 + u32(4);
    for (int t = 0; t < tracks && p + 8 <= b.size(); ++t) {
      const std::size_t start = p + 8, end = std::min<std::size_t>(b.size(), start + u32(p + 4));
      const bool isTrack = !std::memcmp(&b[p], "MTrk", 4);
      p = end;
      if (!isTrack) continue;
      std::uint32_t tick = 0;
      std::uint8_t running = 0;
      std::size_t q = start;
      auto vlq = [&]() {
        std::uint32_t v = 0;
        for (int i = 0; i < 4 && q < end; ++i) {
          const std::uint8_t c = b[q++];
          v = v << 7 | (c & 0x7f);
          if (!(c & 0x80)) break;
        }
        return v;
      };
      while (q < end) {
        tick += vlq();
        if (q >= end) break;
        std::uint8_t status = b[q];
        if (status & 0x80)
          ++q;
        else
          status = running;  // running status
        if (status == 0xff) {  // meta: only the tempo matters
          if (q >= end) break;
          const std::uint8_t type = b[q++];
          const std::uint32_t len = vlq();
          if (type == 0x51 && len == 3 && q + 3 <= end)
            song.events.push_back({tick, 0xff, 0, 0, std::uint32_t(b[q] << 16 | b[q + 1] << 8 | b[q + 2])});
          if (type == 0x2f) break;  // end of track
          q += len;
        }
        else if (status == 0xf0 || status == 0xf7)  // sysex
          q += vlq();
        else if (status & 0x80) {
          running = status;
          const int type = status & 0xf0, n = (type == 0xc0 || type == 0xd0) ? 1 : 2;
          if (q + n > end) break;
          const std::uint8_t d1 = b[q], d2 = n == 2 ? b[q + 1] : 0;
          q += n;
          if (type != 0xa0 && type != 0xd0) song.events.push_back({tick, status, d1, d2, 0});
        }
        else
          break;  // data byte without a status: corrupt
      }
    }
    std::stable_sort(song.events.begin(), song.events.end(),
                     [](const Event& a, const Event& b) { return a.tick < b.tick; });
    return !song.events.empty();
  }

  // --- sequencing: advance the song by one output sample ---

  void Sequence()
  {
    const auto& events = fSong->events;
    while (fNext < events.size() && events[fNext].tick <= fTick) Process(events[fNext++]);
    if (fNext >= events.size()) {  // the end: loop, as Doom does
      fTick = 0;
      fNext = 0;
      for (auto& c : fChannels) InitChannel(c);
      return;
    }
    fTick += fSong->division * 1e6 / (fTempo * fRate);
  }

  void Process(const Event& e)
  {
    if (e.status == 0xff) {
      if (e.tempo > 0) fTempo = e.tempo;
      return;
    }
    const int channel = e.status & 0x0f;
    switch (e.status & 0xf0) {
      case 0x80: KeyOff(channel, e.data1); break;
      case 0x90: KeyOn(channel, e.data1, e.data2); break;
      case 0xb0:
        if (e.data1 == 7)
          SetChannelVolume(channel, e.data2);
        else if (e.data1 == 10)
          SetChannelPan(channel, e.data2);
        else if (e.data1 == 0x7b)  // all notes off
          for (std::size_t i = 0; i < fAllocated.size(); ++i)
            if (fVoices[fAllocated[i]].channel == channel) ReleaseVoice(i--);
        break;
      case 0xc0: fChannels[channel].instrument = &fInstruments[e.data1 & 0x7f]; break;
      case 0xe0: {  // DMX uses only the MSB: 64 steps either way, 32 steps per semitone
        fChannels[channel].bend = e.data2 - 64;
        std::vector<int> bent, others;  // bent voices go to the end of the list (as DMX)
        for (int v : fAllocated) {
          if (fVoices[v].channel == channel) {
            UpdateFrequency(fVoices[v]);
            bent.push_back(v);
          }
          else
            others.push_back(v);
        }
        fAllocated = others;
        fAllocated.insert(fAllocated.end(), bent.begin(), bent.end());
        break;
      }
    }
  }

  // --- the DMX driver ---

  void Write(int reg, int value) { OPL3_WriteReg(&fChip, static_cast<std::uint16_t>(reg), static_cast<std::uint8_t>(value)); }

  void Reset()
  {
    OPL3_Reset(&fChip, static_cast<std::uint32_t>(fRate));
    for (int bank : {0, 0x100}) {
      for (int r = 0x40; r <= 0x55; ++r) Write(r | bank, 0x3f);  // operators silent
      for (int r = 0x60; r <= 0xf5; ++r) Write(r | bank, 0);
      for (int r = 0x01; r < 0x40; ++r) Write(r | bank, 0);
    }
    Write(0x04, 0x60);  // timers off
    Write(0x04, 0x80);
    Write(0x01, 0x20);   // waveform select on
    Write(0x08, 0x40);   // keyboard split
    Write(0x105, 0x01);  // OPL3 mode: 18 voices, stereo
    fFree.clear();
    fAllocated.clear();
    for (int i = 0; i < kVoices; ++i) {
      Voice& v = fVoices[i] = Voice{};
      v.index = i % 9;
      v.op1 = kOperators[0][i % 9];
      v.op2 = kOperators[1][i % 9];
      v.bank = (i / 9) << 8;
      fFree.push_back(i);
    }
    for (auto& c : fChannels) InitChannel(c);
    fTick = 0;
    fNext = 0;
    fTempo = 500000;  // 120 bpm until the song says otherwise
  }

  void InitChannel(Channel& c) { c = {&fInstruments[0], 100, 0x30, 0}; }

  void KeyOn(int channel, int key, int velocity)
  {
    if (velocity == 0) return KeyOff(channel, key);
    const Instrument* instrument;
    int note = key;
    if (channel == 9) {  // percussion: an instrument per key, played at note 60
      if (key < 35 || key > 81) return;
      instrument = &fInstruments[kPercussion + key - 35];
      note = 60;
    }
    else
      instrument = fChannels[channel].instrument;
    if (fFree.empty()) ReplaceVoice();
    VoiceOn(channel, *instrument, 0, note, key, velocity);
    if (instrument->flags & 4) VoiceOn(channel, *instrument, 1, note, key, velocity);
  }

  void KeyOff(int channel, int key)
  {
    for (std::size_t i = 0; i < fAllocated.size(); ++i) {
      const Voice& v = fVoices[fAllocated[i]];
      if (v.channel == channel && v.key == key) ReleaseVoice(i--);
    }
  }

  // All voices busy: drop one (a second voice of a double instrument if possible, otherwise
  // the highest channel), as DMX 1.9 does
  void ReplaceVoice()
  {
    std::size_t result = 0;
    for (std::size_t i = 0; i < fAllocated.size(); ++i) {
      const Voice& v = fVoices[fAllocated[i]];
      if (v.instrumentVoice != 0 || v.channel >= fVoices[fAllocated[result]].channel) result = i;
    }
    if (!fAllocated.empty()) ReleaseVoice(result);
  }

  void ReleaseVoice(std::size_t i)
  {
    const int index = fAllocated[i];
    Voice& v = fVoices[index];
    Write((0xb0 + v.index) | v.bank, v.freq >> 8);  // key off
    v.channel = -1;
    v.note = 0;
    fAllocated.erase(fAllocated.begin() + i);
    fFree.push_back(index);
  }

  void VoiceOn(int channel, const Instrument& instrument, int instrumentVoice, int note, int key, int velocity)
  {
    if (fFree.empty()) return;
    const int index = fFree.front();
    fFree.erase(fFree.begin());
    fAllocated.push_back(index);
    Voice& v = fVoices[index];
    v.channel = channel;
    v.key = key;
    v.note = (instrument.flags & 1) ? instrument.fixedNote : note;
    v.pan = fChannels[channel].pan;
    SetInstrument(v, instrument, instrumentVoice);
    SetVolume(v, velocity);
    v.freq = 0;
    UpdateFrequency(v);
  }

  void LoadOperator(int op, const Operator& data, bool maxLevel, unsigned& volume)
  {
    const unsigned level = data.scale | (maxLevel ? 0x3f : data.level);
    volume = level;
    Write(0x40 + op, level);
    Write(0x20 + op, data.tremolo);
    Write(0x60 + op, data.attack);
    Write(0x80 + op, data.sustain);
    Write(0xe0 + op, data.waveform);
  }

  void SetInstrument(Voice& v, const Instrument& instrument, int instrumentVoice)
  {
    if (v.instrument == &instrument && v.instrumentVoice == instrumentVoice) return;
    v.instrument = &instrument;
    v.instrumentVoice = instrumentVoice;
    const InstrumentVoice& data = instrument.voices[instrumentVoice];
    const bool modulating = !(data.feedback & 1);
    // the carrier silent until SetVolume; both, when the operators are additive
    LoadOperator(v.op2 | v.bank, data.carrier, true, v.carVolume);
    LoadOperator(v.op1 | v.bank, data.modulator, !modulating, v.modVolume);
    Write((0xc0 + v.index) | v.bank, data.feedback | v.pan);
    v.priority = 0x0f - (data.carrier.attack >> 4) + 0x0f - (data.carrier.sustain & 0x0f);
  }

  void SetVolume(Voice& v, int velocity)
  {
    v.noteVolume = velocity;
    const InstrumentVoice& data = v.instrument->voices[v.instrumentVoice];
    const unsigned midi = 2 * (kVolume[fChannels[v.channel].volume] + 1);
    const unsigned full = (kVolume[v.noteVolume] * midi) >> 9;
    const unsigned car = 0x3f - full;
    if (car == (v.carVolume & 0x3f)) return;
    v.carVolume = car | (v.carVolume & 0xc0);
    Write((0x40 + v.op2) | v.bank, v.carVolume);
    if ((data.feedback & 1) && data.modulator.level != 0x3f) {  // additive: the modulator is heard too
      unsigned mod = std::max<unsigned>(data.modulator.level, car) | (v.modVolume & 0xc0);
      if (mod != v.modVolume) {
        v.modVolume = mod;
        Write((0x40 + v.op1) | v.bank, mod | (data.modulator.scale & 0xc0));
      }
    }
  }

  void SetChannelVolume(int channel, int volume)
  {
    fChannels[channel].volume = std::min(volume, 127);
    for (auto& v : fVoices)
      if (v.channel == channel) SetVolume(v, v.noteVolume);
  }

  void SetChannelPan(int channel, int pan)
  {
    // DMX had the stereo sides swapped (bit 4 is the left output): corrected here
    pan = 144 - pan;
    const int reg = pan >= 96 ? 0x10 : pan <= 48 ? 0x20 : 0x30;
    if (fChannels[channel].pan == reg) return;
    fChannels[channel].pan = reg;
    for (auto& v : fVoices)
      if (v.channel == channel) {
        v.pan = reg;
        Write((0xc0 + v.index) | v.bank, v.instrument->voices[v.instrumentVoice].feedback | reg);
      }
  }

  // DMX's pitch table: 32 steps per semitone from two semitones below C0 (16.35 Hz, one octave
  // above MIDI note 0: the GENMIDI note offsets, mostly -12, make up for it). Notes from 7 on
  // repeat one octave of F-numbers with the block (octave) in bits 10-12. This formula gives
  // DMX's table values to within rounding.
  static unsigned FNumber(int index)
  {
    const double hz = 16.3516 * std::pow(2., (index - 64) / 384.);
    if (index < 284) return static_cast<unsigned>(std::lround(hz * 1048576. / 49716.));
    const int octave = std::min((index - 284) / 384, 7);
    const double base = 16.3516 * std::pow(2., (284 + (index - 284) % 384 - 64) / 384.);
    return static_cast<unsigned>(std::lround(base * 1048576. / 49716.)) | octave << 10;
  }

  void UpdateFrequency(Voice& v)
  {
    const Instrument& in = *v.instrument;
    int note = v.note;
    if (!(in.flags & 1)) note += in.voices[v.instrumentVoice].noteOffset;
    while (note < 0) note += 12;
    while (note > 95) note -= 12;
    int index = 64 + 32 * note + fChannels[v.channel].bend;
    if (v.instrumentVoice != 0) index += in.fineTuning / 2 - 64;  // the second voice, detuned
    const unsigned freq = FNumber(std::max(index, 0));
    if (freq == v.freq) return;
    Write((0xa0 + v.index) | v.bank, freq & 0xff);
    Write((0xb0 + v.index) | v.bank, (freq >> 8) | 0x20);  // key on
    v.freq = freq;
  }

  // MIDI volume -> OPL level (DMX's curve, as measured by the Chocolate Doom authors)
  static constexpr unsigned kVolume[128] = {
    0,   1,   3,   5,   6,   8,   10,  11,  13,  14,  16,  17,  19,  20,  22,  23,  25,  26,  27,  29,  30,  32,
    33,  34,  36,  37,  39,  41,  43,  45,  47,  49,  50,  52,  54,  55,  57,  59,  60,  61,  63,  64,  66,  67,
    68,  69,  71,  72,  73,  74,  75,  76,  77,  79,  80,  81,  82,  83,  84,  84,  85,  86,  87,  88,  89,  90,
    91,  92,  92,  93,  94,  95,  96,  96,  97,  98,  99,  99,  100, 101, 101, 102, 103, 103, 104, 105, 105, 106,
    107, 107, 108, 109, 109, 110, 110, 111, 112, 112, 113, 113, 114, 114, 115, 115, 116, 117, 117, 118, 118, 119,
    119, 120, 120, 121, 121, 122, 122, 123, 123, 123, 124, 124, 125, 125, 126, 126, 127, 127};
  static constexpr int kOperators[2][9] = {{0x00, 0x01, 0x02, 0x08, 0x09, 0x0a, 0x10, 0x11, 0x12},
                                           {0x03, 0x04, 0x05, 0x0b, 0x0c, 0x0d, 0x13, 0x14, 0x15}};

  double fRate;
  bool fHaveInstruments = false;
  std::array<Instrument, kInstruments> fInstruments{};
  std::mutex fMutex;  // fSong and fRestart (GUI thread) against Render (audio thread)
  std::unique_ptr<Song> fSong;
  bool fRestart = true;
  // only the audio thread touches these
  opl3_chip fChip{};
  std::array<Voice, kVoices> fVoices;
  std::vector<int> fFree, fAllocated;  // voice indices, in DMX's list order
  std::array<Channel, 16> fChannels{};
  std::size_t fNext = 0;
  double fTick = 0, fTempo = 500000;  // song position in ticks; microseconds per beat
};
