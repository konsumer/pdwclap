#pragma once

// Pd-free core of the wclap~ external: loads a WCLAP (a CLAP plugin compiled
// to wasm32) through wclap-bridge's Wasmtime embedding, which hands back a
// real clap_plugin_factory_t — so everything here is an ordinary CLAP host,
// just with the Pd object plumbing (src/wclap_tilde.c) kept out of it so it
// can also be driven from tools/wclap_harness.c without pd loaded.
//
// Threading contract, mirroring CLAP's own annotations:
//   [main-thread]  everything that isn't wclap_host_process()/wclap_host_*
//                  _note_*/wclap_host_set_param() — param queries, param table
//                  rebuilds, activate/deactivate, close.
//   [audio-thread] wclap_host_process(). Note and param *changes* are queued
//                  on the main thread and delivered as CLAP events at the
//                  start of the next process() call, so the two threads never
//                  touch the plugin at the same time.
// The one place they must meet is CLAP's [main-thread] param/value_to_text
// calls racing a running process(): those are serialized with a mutex that
// process() also takes (see WclapHost.lock).

#include <stdbool.h>
#include <stdint.h>

typedef struct WclapHost WclapHost;

// Matches a stepped/enum param's natural type; a plugin may declare more, but
// beyond this we'd be allocating tables for something no Pd patch will use.
#define WCLAP_MAX_PARAMS 2048

// Audio channels we're willing to shuffle; a plugin declaring more keeps its
// extra channels, but anything past this is fed silence.
#define WCLAP_MAX_CHANNELS 8

// CLAP plugins declare audio ports, each with its own channel count; the ones
// these WCLAP examples ship are mono/stereo, so a handful covers anything
// realistic.
#define WCLAP_MAX_PORTS 8

// Events (notes + param changes) drained per process() call. Pd can't produce
// anywhere near this many in one 64-sample block, so this only bounds the
// arrays below; anything past it waits for the next block.
#define WCLAP_MAX_BLOCK_EVENTS 128

// One event type for both directions: what Pd sends into the plugin, and what
// the plugin sends back out. Each direction only produces some of the kinds —
// WCLAP_EVENT_NOTE_CHOKE/NOTE_END/MIDI only ever come out of the plugin.
typedef enum {
  WCLAP_EVENT_NOTE_ON,
  WCLAP_EVENT_NOTE_OFF,
  WCLAP_EVENT_NOTE_CHOKE,  // out: steal (match) the voice, not a release
  WCLAP_EVENT_NOTE_END,    // out: that voice has finished
  WCLAP_EVENT_PARAM,       // both: in = set the param, out = the plugin changed/reported it
  WCLAP_EVENT_MIDI,        // out: 1-3 raw MIDI bytes in `midi`
} WclapEventKind;

typedef struct {
  uint8_t kind;       // WclapEventKind
  int32_t index;      // in: param table index; out: the index its clap_id resolved to, or -1
  uint32_t param_id;  // out (WCLAP_EVENT_PARAM): the plugin's own clap_id
  int32_t key;        // note events: 0..127
  int32_t channel;    // note events: 0..15
  double value;       // note velocity 0..1, or the param value
  uint8_t midi[3];    // WCLAP_EVENT_MIDI
} WclapEvent;

typedef struct {
  uint32_t index;  // index into the param table (what the Pd layer addresses)
  uint32_t id;     // clap_id, what the plugin gets in events
  char name[256];
  double min_value;
  double max_value;
  double default_value;
  uint32_t flags;  // clap_param_info_flags
} WclapParamInfo;

// Loads the plugin with this id from `path` (a .wasm file or a .wclap
// directory); a NULL/empty plugin_id picks the first plugin in the file.
// Returns NULL on failure with a message in err.
WclapHost* wclap_host_open(const char* path, const char* plugin_id, double sample_rate,
                           uint32_t max_frames, char* err, uint32_t err_capacity);
void wclap_host_close(WclapHost* host);

const char* wclap_host_name(const WclapHost* host);
const char* wclap_host_id(const WclapHost* host);
// Total audio channels the plugin declares, summed across its ports.
uint32_t wclap_host_input_count(const WclapHost* host);
uint32_t wclap_host_output_count(const WclapHost* host);
bool wclap_host_is_instrument(const WclapHost* host);

// Re-activates the plugin when the sample rate or block size changed (Pd
// re-runs dsp() on every audio-settings/graph change). Cheap no-op otherwise.
bool wclap_host_set_audio_config(WclapHost* host, double sample_rate, uint32_t max_frames);

uint32_t wclap_host_param_count(const WclapHost* host);
const WclapParamInfo* wclap_host_param_info(const WclapHost* host, uint32_t index);
// Index of the param with this exact name, or -1. Names aren't guaranteed
// unique by CLAP; the first match wins.
int32_t wclap_host_param_find(const WclapHost* host, const char* name);
// The param with this clap_id, or NULL — used to resolve ids in the plugin's
// own output events.
const WclapParamInfo* wclap_host_param_by_id(const WclapHost* host, uint32_t id);
// Current value as the plugin reports it, falling back to the default when it
// has no get_value or won't answer.
double wclap_host_param_value(WclapHost* host, uint32_t index);
// CLAP_EXT_PARAMS' value_to_text: "displayable" text for a value ("2.3 kHz").
bool wclap_host_param_text(WclapHost* host, uint32_t index, double value, char* out, uint32_t capacity);

// All of these are queued and take effect at the next wclap_host_process().
// Returns false if the value was outside the param's range and got clamped —
// a slider wired to the wrong param lands here (and then nothing audible
// happens), so callers can say so.
bool wclap_host_set_param(WclapHost* host, uint32_t index, double value);
void wclap_host_note_on(WclapHost* host, int32_t key, double velocity, int32_t channel);
void wclap_host_note_off(WclapHost* host, int32_t key, int32_t channel);
void wclap_host_all_notes_off(WclapHost* host);

// One block. in_left/in_right may be NULL when the caller has no audio in
// (instruments); out_left/out_right are `frames` floats the plugin writes.
// A plugin with fewer output channels than 2 gets its output mirrored so both
// Pd outlets carry it; extra plugin channels land in scratch and are dropped.
void wclap_host_process(WclapHost* host, const float* in_left, const float* in_right,
                        float* out_left, float* out_right, uint32_t frames);

// Call periodically from the main thread: delivers the plugin's deferred
// on_main_thread() work and re-reads the param table when the plugin asked
// for a rescan (a preset load changing param names/ranges, say).
void wclap_host_main_thread_work(WclapHost* host);

// [main-thread] Drains up to max_events of what the plugin emitted, oldest
// first. Param events come back with `index` resolved against the current
// param table (-1 if the plugin named a clap_id we don't know). Kinds with no
// Pd representation (note expression, param modulation, gestures, sysex,
// MIDI 2, transport) are never queued — they only show up in the dropped
// count below.
uint32_t wclap_host_poll_output(WclapHost* host, WclapEvent* events, uint32_t max_events);

// [main-thread] Output events the host couldn't pass on since this was last
// called — no Pd equivalent for them, or the output queue was full — reset to
// zero here.
uint32_t wclap_host_output_dropped(WclapHost* host);
