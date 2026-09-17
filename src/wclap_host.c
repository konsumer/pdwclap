// Host core: WCLAP loading via wclap-bridge, CLAP host callbacks, event
// queues, and the process loop. Deliberately free of Pd — the Pd object in
// wclap_tilde.c is a thin shell over this, and tools/wclap_harness.c drives
// the same code offline so the host can be tested without ever starting Pd.

#include "wclap_host.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "clap/clap.h"
#include "wclap-bridge.h"

// --- library table ----------------------------------------------------------
// One wclap_open per file, shared by every object loading out of it. Only
// touched from the main thread (object creation/destruction), so no lock.

typedef struct {
  char path[1024];
  void* wclap;
  int ref_count;
} LibEntry;

#define WCLAP_MAX_LIBS 64
static LibEntry s_libs[WCLAP_MAX_LIBS];
static int s_lib_count = 0;
static bool s_bridge_initialized = false;

static void set_err(char* err, uint32_t capacity, const char* fmt, ...) {
  va_list args;
  if (!err || !capacity)
    return;
  va_start(args, fmt);
  vsnprintf(err, capacity, fmt, args);
  va_end(args);
}

static LibEntry* lib_acquire(const char* path, char* err, uint32_t err_capacity) {
  if (!s_bridge_initialized) {
    wclap_global_init(0);
    s_bridge_initialized = true;
  }
  for (int i = 0; i < s_lib_count; i++) {
    if (strcmp(s_libs[i].path, path) == 0) {
      s_libs[i].ref_count++;
      return &s_libs[i];
    }
  }
  // Reuse a slot freed by lib_release rather than compacting the array: live
  // hosts hold raw LibEntry* into it.
  LibEntry* slot = NULL;
  for (int i = 0; i < s_lib_count; i++) {
    if (!s_libs[i].wclap) {
      slot = &s_libs[i];
      break;
    }
  }
  if (!slot) {
    if (s_lib_count >= WCLAP_MAX_LIBS) {
      set_err(err, err_capacity, "too many WCLAP files loaded (%d)", WCLAP_MAX_LIBS);
      return NULL;
    }
    slot = &s_libs[s_lib_count++];
  }

  void* wclap = wclap_open(path);
  if (!wclap) {
    set_err(err, err_capacity, "cannot open '%s'", path);
    return NULL;
  }
  char bridge_err[512];
  if (wclap_get_error(wclap, bridge_err, sizeof(bridge_err))) {
    set_err(err, err_capacity, "%s", bridge_err);
    wclap_close(wclap);
    return NULL;
  }

  memset(slot->path, 0, sizeof(slot->path));
  strncpy(slot->path, path, sizeof(slot->path) - 1);
  slot->wclap = wclap;
  slot->ref_count = 1;
  return slot;
}

static void lib_release(LibEntry* entry) {
  if (!entry || --entry->ref_count > 0)
    return;
  wclap_close(entry->wclap);
  entry->wclap = NULL;
  entry->path[0] = '\0';
  entry->ref_count = 0;
}

// --- event queues -----------------------------------------------------------
// Two SPSC rings of WclapEvent:
//   * `ring`        Pd's main thread produces (notes/param changes), the audio
//                   thread consumes them inside process().
//   * `output_ring` the audio thread produces (what the plugin emitted during
//                   process()), the main thread drains it when it polls.
// One producer and one consumer each, so plain release/acquire ordering is
// enough — no lock on either side.

#define WCLAP_EVENT_RING 1024
#define WCLAP_OUTPUT_RING 1024

typedef struct {
  WclapEvent events[WCLAP_EVENT_RING];
  atomic_uint head;  // next slot to write (producer)
  atomic_uint tail;  // next slot to read (consumer)
} WclapEventRing;

typedef struct {
  WclapEvent events[WCLAP_OUTPUT_RING];
  atomic_uint head;
  atomic_uint tail;
} WclapOutputRing;

static bool ring_push(WclapEventRing* ring, const WclapEvent* event) {
  unsigned head = atomic_load_explicit(&ring->head, memory_order_relaxed);
  unsigned next = (head + 1) % WCLAP_EVENT_RING;
  if (next == atomic_load_explicit(&ring->tail, memory_order_acquire))
    return false;  // full: 128/block drain keeps up with anything Pd can send
  ring->events[head] = *event;
  atomic_store_explicit(&ring->head, next, memory_order_release);
  return true;
}

static bool ring_pop(WclapEventRing* ring, WclapEvent* out) {
  unsigned tail = atomic_load_explicit(&ring->tail, memory_order_relaxed);
  if (tail == atomic_load_explicit(&ring->head, memory_order_acquire))
    return false;
  *out = ring->events[tail];
  atomic_store_explicit(&ring->tail, (tail + 1) % WCLAP_EVENT_RING, memory_order_release);
  return true;
}

static bool output_ring_push(WclapOutputRing* ring, const WclapEvent* event) {
  unsigned head = atomic_load_explicit(&ring->head, memory_order_relaxed);
  unsigned next = (head + 1) % WCLAP_OUTPUT_RING;
  if (next == atomic_load_explicit(&ring->tail, memory_order_acquire))
    return false;
  ring->events[head] = *event;
  atomic_store_explicit(&ring->head, next, memory_order_release);
  return true;
}

static bool output_ring_pop(WclapOutputRing* ring, WclapEvent* out) {
  unsigned tail = atomic_load_explicit(&ring->tail, memory_order_relaxed);
  if (tail == atomic_load_explicit(&ring->head, memory_order_acquire))
    return false;
  *out = ring->events[tail];
  atomic_store_explicit(&ring->tail, (tail + 1) % WCLAP_OUTPUT_RING, memory_order_release);
  return true;
}

// --- host ------------------------------------------------------------------

struct WclapHost {
  clap_host_t host;  // host.host_data = the WclapHost
  LibEntry* lib;
  const clap_plugin_t* plugin;
  const clap_plugin_params_t* params;

  char name[128];
  char id[256];
  double sample_rate;
  uint32_t max_frames;
  // CLAP counts audio ports and channels separately: a port declares its own
  // channel_count, and process() takes one buffer per port. Pd only ever
  // hands us a stereo pair, so the per-port tables below say how a plugin's
  // ports map onto it; anything past those two channels gets silence in and
  // scratch out.
  uint32_t input_port_count;
  uint32_t output_port_count;
  uint32_t input_channel_count;
  uint32_t output_channel_count;
  uint32_t input_port_channels[WCLAP_MAX_PORTS];
  uint32_t output_port_channels[WCLAP_MAX_PORTS];
  bool is_instrument;
  bool activated;

  WclapParamInfo* param_table;
  uint32_t param_count;

  // Notes we've sent and not yet released, so `allnotesoff` can send an exact
  // note off for each instead of a wildcard — CLAP allows a key/channel of -1
  // to mean "all notes", but plenty of plugins (as-clap's included) only match
  // whole tuples, which would leave those notes stuck.
  bool active_notes[16][128];

  WclapEventRing ring;
  WclapOutputRing output_ring;
  atomic_uint output_dropped;  // events with no Pd equivalent, or past the ring's capacity
  atomic_bool rescan_pending;
  atomic_bool main_thread_pending;

  // Audio-thread event storage for the block being processed. Param events
  // first, then notes (see input_events_get).
  clap_event_param_value_t param_events[WCLAP_MAX_BLOCK_EVENTS];
  clap_event_note_t note_events[WCLAP_MAX_BLOCK_EVENTS];
  uint32_t param_event_count;
  uint32_t note_event_count;

  // silence stands in for input channels the caller had no buffer for;
  // scratch is where the plugin's own output channels are written before
  // being copied out to the caller (see wclap_host_process).
  float* silence;
  float* scratch[WCLAP_MAX_CHANNELS];

  // Serializes process() against CLAP's [main-thread]-only calls, which the
  // Pd layer makes while audio may be running (CLAP marks them main-thread
  // precisely because they aren't safe against a concurrent process()).
  pthread_mutex_t lock;
};

// --- host callbacks ---------------------------------------------------------

// CLAP_EXT_PARAMS host side. We hold no param cache beyond the immutable
// table, so the plugin loading a preset only has to flag a rescan; the Pd
// main-thread clock picks it up (wclap_host_main_thread_work).
static void host_params_rescan(const clap_host_t* host, clap_param_rescan_flags flags) {
  (void)flags;
  WclapHost* self = (WclapHost*)host->host_data;
  if (self)
    atomic_store(&self->rescan_pending, true);
}

static void host_params_clear(const clap_host_t* host, clap_id id, clap_param_clear_flags flags) {
  (void)host;
  (void)id;
  (void)flags;
}

static void host_params_request_flush(const clap_host_t* host) {
  (void)host;
  // Nothing to do: queued changes already land in the next process().
}

static const clap_host_params_t s_host_params = {
    host_params_rescan,
    host_params_clear,
    host_params_request_flush,
};

// CLAP_EXT_LOG — the only channel a plugin has for diagnostics. stderr, not
// Pd's console: plugins log from whichever thread they like.
static void host_log(const clap_host_t* host, clap_log_severity severity, const char* msg) {
  (void)host;
  static const char* labels[] = {"DEBUG", "INFO", "WARNING", "ERROR", "FATAL"};
  const char* label = (severity >= 0 && severity < 5) ? labels[severity] : "LOG";
  fprintf(stderr, "wclap~: [%s] %s\n", label, msg);
}

static const clap_host_log_t s_host_log = {host_log};

static void host_request_restart(const clap_host_t* host) {
  (void)host;
}

static void host_request_process(const clap_host_t* host) {
  (void)host;
}

static void host_request_callback(const clap_host_t* host) {
  WclapHost* self = (WclapHost*)host->host_data;
  if (self)
    atomic_store(&self->main_thread_pending, true);
}

static const void* host_get_extension(const clap_host_t* host, const char* extension_id) {
  (void)host;
  if (strcmp(extension_id, CLAP_EXT_PARAMS) == 0)
    return &s_host_params;
  if (strcmp(extension_id, CLAP_EXT_LOG) == 0)
    return &s_host_log;
  return NULL;
}

static const clap_host_t s_host_template = {
    CLAP_VERSION_INIT,
    NULL,  // host_data — set per host
    "pdwclap",
    "pdwclap",
    "",
    "0.1.0",
    host_get_extension,
    host_request_restart,
    host_request_process,
    host_request_callback,
};

// --- params -----------------------------------------------------------------

// Caller holds the lock.
static void rebuild_param_table(WclapHost* self) {
  uint32_t count = self->params ? self->params->count(self->plugin) : 0;
  if (count > WCLAP_MAX_PARAMS)
    count = WCLAP_MAX_PARAMS;

  WclapParamInfo* table = calloc(count ? count : 1, sizeof(*table));
  if (!table)
    return;

  uint32_t kept = 0;
  for (uint32_t i = 0; i < count; i++) {
    clap_param_info_t info;
    memset(&info, 0, sizeof(info));
    if (!self->params->get_info(self->plugin, i, &info))
      continue;
    WclapParamInfo* param = &table[kept];
    param->index = kept;
    param->id = (uint32_t)info.id;
    snprintf(param->name, sizeof(param->name), "%s", info.name);
    param->min_value = info.min_value;
    param->max_value = info.max_value;
    param->default_value = info.default_value;
    param->flags = (uint32_t)info.flags;
    kept++;
  }

  free(self->param_table);
  self->param_table = table;
  self->param_count = kept;
}

uint32_t wclap_host_param_count(const WclapHost* host) {
  return host ? host->param_count : 0;
}

const WclapParamInfo* wclap_host_param_info(const WclapHost* host, uint32_t index) {
  if (!host || index >= host->param_count)
    return NULL;
  return &host->param_table[index];
}

int32_t wclap_host_param_find(const WclapHost* host, const char* name) {
  if (!host || !name)
    return -1;
  for (uint32_t i = 0; i < host->param_count; i++) {
    if (strcmp(host->param_table[i].name, name) == 0)
      return (int32_t)i;
  }
  return -1;
}

const WclapParamInfo* wclap_host_param_by_id(const WclapHost* host, uint32_t id) {
  if (!host)
    return NULL;
  for (uint32_t i = 0; i < host->param_count; i++) {
    if (host->param_table[i].id == id)
      return &host->param_table[i];
  }
  return NULL;
}

static double clamp_param(const WclapParamInfo* param, double value) {
  // Written as negated comparisons so a NaN lands on min_value rather than
  // propagating into the plugin.
  if (!(value >= param->min_value))
    value = param->min_value;
  if (value > param->max_value)
    value = param->max_value;
  return value;
}

double wclap_host_param_value(WclapHost* host, uint32_t index) {
  const WclapParamInfo* param = wclap_host_param_info(host, index);
  if (!param)
    return 0.0;
  if (!host->params || !host->params->get_value)
    return param->default_value;

  double value = param->default_value;
  pthread_mutex_lock(&host->lock);
  bool ok = host->params->get_value(host->plugin, (clap_id)param->id, &value);
  pthread_mutex_unlock(&host->lock);
  return ok ? value : param->default_value;
}

bool wclap_host_param_text(WclapHost* host, uint32_t index, double value, char* out, uint32_t capacity) {
  const WclapParamInfo* param = wclap_host_param_info(host, index);
  if (!param || !host->params || !host->params->value_to_text || !out || !capacity)
    return false;

  pthread_mutex_lock(&host->lock);
  bool ok = host->params->value_to_text(host->plugin, (clap_id)param->id, value, out, capacity);
  pthread_mutex_unlock(&host->lock);
  return ok;
}

// --- queued changes ---------------------------------------------------------

static void ring_push_simple(WclapHost* host, uint8_t kind, int32_t key, int32_t channel, double value) {
  WclapEvent event = {0};
  event.kind = kind;
  event.key = key;
  event.channel = channel;
  event.value = value;
  ring_push(&host->ring, &event);
}

bool wclap_host_set_param(WclapHost* host, uint32_t index, double value) {
  if (!host)
    return true;
  const WclapParamInfo* param = wclap_host_param_info(host, index);
  if (!param)
    return true;
  double clamped = clamp_param(param, value);
  WclapEvent event = {0};
  event.kind = WCLAP_EVENT_PARAM;
  event.index = (int32_t)index;
  event.value = clamped;
  ring_push(&host->ring, &event);
  return clamped == value;
}

static int clamp_channel(int32_t channel) {
  if (channel < 0 || channel > 15)
    return 0;
  return (int)channel;
}

void wclap_host_note_on(WclapHost* host, int32_t key, double velocity, int32_t channel) {
  if (!host)
    return;
  if (velocity < 0.0)
    velocity = 0.0;
  if (velocity > 1.0)
    velocity = 1.0;
  int ch = clamp_channel(channel);
  if (key >= 0 && key < 128)
    host->active_notes[ch][key] = true;
  ring_push_simple(host, WCLAP_EVENT_NOTE_ON, key, ch, velocity);
}

void wclap_host_note_off(WclapHost* host, int32_t key, int32_t channel) {
  if (!host)
    return;
  int ch = clamp_channel(channel);
  if (key >= 0 && key < 128)
    host->active_notes[ch][key] = false;
  ring_push_simple(host, WCLAP_EVENT_NOTE_OFF, key, ch, 0.0);
}

void wclap_host_all_notes_off(WclapHost* host) {
  if (!host)
    return;
  for (int channel = 0; channel < 16; channel++) {
    for (int key = 0; key < 128; key++) {
      if (!host->active_notes[channel][key])
        continue;
      host->active_notes[channel][key] = false;
      ring_push_simple(host, WCLAP_EVENT_NOTE_OFF, key, channel, 0.0);
    }
  }
}

// --- process ----------------------------------------------------------------

// Caller holds the lock.
static void drain_events(WclapHost* self) {
  WclapEvent event;
  uint32_t drained = 0;
  while (drained < WCLAP_MAX_BLOCK_EVENTS && ring_pop(&self->ring, &event)) {
    drained++;
    switch (event.kind) {
      case WCLAP_EVENT_NOTE_ON:
      case WCLAP_EVENT_NOTE_OFF: {
        clap_event_note_t* ev = &self->note_events[self->note_event_count++];
        memset(ev, 0, sizeof(*ev));
        ev->header.size = sizeof(*ev);
        ev->header.time = 0;  // Pd messages carry no sample offset
        ev->header.space_id = CLAP_CORE_EVENT_SPACE_ID;
        ev->header.type = event.kind == WCLAP_EVENT_NOTE_ON ? CLAP_EVENT_NOTE_ON : CLAP_EVENT_NOTE_OFF;
        ev->note_id = -1;
        ev->port_index = 0;
        ev->channel = (int16_t)event.channel;
        ev->key = (int16_t)event.key;
        ev->velocity = event.value;
        break;
      }
      case WCLAP_EVENT_PARAM: {
        const WclapParamInfo* param = wclap_host_param_info(self, (uint32_t)event.index);
        if (!param)
          break;
        clap_event_param_value_t* ev = &self->param_events[self->param_event_count++];
        memset(ev, 0, sizeof(*ev));
        ev->header.size = sizeof(*ev);
        ev->header.time = 0;
        ev->header.space_id = CLAP_CORE_EVENT_SPACE_ID;
        ev->header.type = CLAP_EVENT_PARAM_VALUE;
        ev->param_id = (clap_id)param->id;
        ev->cookie = NULL;
        ev->note_id = -1;
        ev->port_index = -1;
        ev->channel = -1;
        ev->key = -1;
        ev->value = event.value;
        break;
      }
    }
  }
}

static const clap_event_header_t* input_events_get(const clap_input_events_t* list, uint32_t index) {
  WclapHost* self = (WclapHost*)list->ctx;
  if (index < self->param_event_count)
    return &self->param_events[index].header;
  return &self->note_events[index - self->param_event_count].header;
}

static uint32_t input_events_size(const clap_input_events_t* list) {
  WclapHost* self = (WclapHost*)list->ctx;
  return self->param_event_count + self->note_event_count;
}

// The plugin's own events, on the audio thread inside process(). Notes, param
// values and raw MIDI get queued for the main thread to poll; anything else
// (note expression, param modulation, gestures, sysex, MIDI 2, transport) has
// no Pd equivalent and is only counted, so the Pd layer can say so once.
static bool output_events_try_push(const clap_output_events_t* list, const clap_event_header_t* event) {
  WclapHost* self = (WclapHost*)list->ctx;
  if (!self)
    return false;

  WclapEvent queued = {0};
  if (event->space_id != CLAP_CORE_EVENT_SPACE_ID) {
    atomic_fetch_add_explicit(&self->output_dropped, 1, memory_order_relaxed);
    return true;
  }

  switch (event->type) {
    case CLAP_EVENT_NOTE_ON:
      queued.kind = WCLAP_EVENT_NOTE_ON;
      break;
    case CLAP_EVENT_NOTE_OFF:
      queued.kind = WCLAP_EVENT_NOTE_OFF;
      break;
    case CLAP_EVENT_NOTE_CHOKE:
      queued.kind = WCLAP_EVENT_NOTE_CHOKE;
      break;
    case CLAP_EVENT_NOTE_END:
      queued.kind = WCLAP_EVENT_NOTE_END;
      break;
    case CLAP_EVENT_PARAM_VALUE:
      queued.kind = WCLAP_EVENT_PARAM;
      break;
    case CLAP_EVENT_MIDI:
      queued.kind = WCLAP_EVENT_MIDI;
      break;
    default:
      atomic_fetch_add_explicit(&self->output_dropped, 1, memory_order_relaxed);
      return true;
  }

  if (queued.kind <= WCLAP_EVENT_NOTE_END) {
    const clap_event_note_t* note = (const clap_event_note_t*)event;
    queued.key = note->key;
    queued.channel = note->channel;
    queued.value = note->velocity;
  } else if (queued.kind == WCLAP_EVENT_PARAM) {
    const clap_event_param_value_t* param = (const clap_event_param_value_t*)event;
    queued.param_id = (uint32_t)param->param_id;
    queued.value = param->value;
  } else {
    const clap_event_midi_t* midi = (const clap_event_midi_t*)event;
    memcpy(queued.midi, midi->data, sizeof(queued.midi));
  }

  if (!output_ring_push(&self->output_ring, &queued)) {
    atomic_fetch_add_explicit(&self->output_dropped, 1, memory_order_relaxed);
    return false;  // actually couldn't take it: the queue is full
  }
  return true;
}

void wclap_host_process(WclapHost* host, const float* in_left, const float* in_right,
                        float* out_left, float* out_right, uint32_t frames) {
  if (!host || !host->plugin || frames == 0)
    return;

  // The Pd layer re-activates on block-size change (dsp() runs before the
  // first process of a new size); this is just belt and braces so a caller
  // that forgets can't run the plugin past its activated frame count.
  if (frames > host->max_frames)
    frames = host->max_frames;
  if (frames == 0)
    return;

  const uint32_t input_channels = host->input_channel_count;
  const uint32_t output_channels = host->output_channel_count;
  float* in_channels[WCLAP_MAX_CHANNELS];
  float* out_channels[WCLAP_MAX_CHANNELS];

  // Inputs are read straight out of the caller's buffers but outputs land in
  // host-owned ones, then get copied over when the plugin is done: Pd will
  // hand a tilde object the same buffer as both an input and an output ("so
  // tilde objects must be able to compute in place"), so writing to the
  // caller's output vectors before the plugin has read its input would eat
  // the input — and zeroing them would silence it entirely.
  for (uint32_t i = 0; i < input_channels; i++) {
    const float* source = NULL;
    if (i == 0)
      source = in_left;
    else if (i == 1)
      source = in_right ? in_right : in_left;
    in_channels[i] = (float*)(source ? source : host->silence);
  }
  for (uint32_t i = 0; i < output_channels; i++) {
    memset(host->scratch[i], 0, frames * sizeof(float));
    out_channels[i] = host->scratch[i];
  }

  // One clap_audio_buffer_t per port, each with its own channel pointers.
  clap_audio_buffer_t audio_in[WCLAP_MAX_PORTS];
  clap_audio_buffer_t audio_out[WCLAP_MAX_PORTS];
  float* in_port_data[WCLAP_MAX_PORTS][WCLAP_MAX_CHANNELS];
  float* out_port_data[WCLAP_MAX_PORTS][WCLAP_MAX_CHANNELS];

  uint32_t channel = 0;
  for (uint32_t port = 0; port < host->input_port_count; port++) {
    for (uint32_t c = 0; c < host->input_port_channels[port]; c++)
      in_port_data[port][c] = in_channels[channel++];
    memset(&audio_in[port], 0, sizeof(audio_in[port]));
    audio_in[port].data32 = in_port_data[port];
    audio_in[port].channel_count = host->input_port_channels[port];
  }
  channel = 0;
  for (uint32_t port = 0; port < host->output_port_count; port++) {
    for (uint32_t c = 0; c < host->output_port_channels[port]; c++)
      out_port_data[port][c] = out_channels[channel++];
    memset(&audio_out[port], 0, sizeof(audio_out[port]));
    audio_out[port].data32 = out_port_data[port];
    audio_out[port].channel_count = host->output_port_channels[port];
  }

  clap_input_events_t in_events = {host, input_events_size, input_events_get};
  clap_output_events_t out_events = {host, output_events_try_push};

  clap_process_t process = {0};
  process.steady_time = -1;
  process.frames_count = frames;
  process.transport = NULL;
  process.audio_inputs = host->input_port_count ? audio_in : NULL;
  process.audio_inputs_count = host->input_port_count;
  process.audio_outputs = host->output_port_count ? audio_out : NULL;
  process.audio_outputs_count = host->output_port_count;
  process.in_events = &in_events;
  process.out_events = &out_events;

  pthread_mutex_lock(&host->lock);
  drain_events(host);
  host->plugin->process(host->plugin, &process);
  host->param_event_count = 0;
  host->note_event_count = 0;
  pthread_mutex_unlock(&host->lock);

  // Nothing in the caller's output vectors has been touched until now, so
  // aliasing between them and the inputs can't have hurt anything.
  if (out_left) {
    if (output_channels >= 1)
      memcpy(out_left, host->scratch[0], frames * sizeof(float));
    else
      memset(out_left, 0, frames * sizeof(float));
  }
  if (out_right) {
    // A mono plugin leaves the right channel silent; Pd patches expect the
    // signal at both outlets, so mirror rather than dropping it.
    if (output_channels >= 2)
      memcpy(out_right, host->scratch[1], frames * sizeof(float));
    else if (output_channels == 1)
      memcpy(out_right, host->scratch[0], frames * sizeof(float));
    else
      memset(out_right, 0, frames * sizeof(float));
  }
}

// --- lifecycle --------------------------------------------------------------

static bool allocate_buffers(WclapHost* self) {
  free(self->silence);
  self->silence = calloc(self->max_frames ? self->max_frames : 1, sizeof(float));
  for (uint32_t i = 0; i < WCLAP_MAX_CHANNELS; i++) {
    free(self->scratch[i]);
    self->scratch[i] = calloc(self->max_frames ? self->max_frames : 1, sizeof(float));
  }
  return self->silence != NULL;
}

static void free_buffers(WclapHost* self) {
  free(self->silence);
  self->silence = NULL;
  for (uint32_t i = 0; i < WCLAP_MAX_CHANNELS; i++) {
    free(self->scratch[i]);
    self->scratch[i] = NULL;
  }
}

// Caller holds the lock.
static void deactivate(WclapHost* self) {
  if (!self->activated)
    return;
  self->plugin->stop_processing(self->plugin);
  self->plugin->deactivate(self->plugin);
  self->activated = false;
}

// Caller holds the lock.
static bool activate(WclapHost* self) {
  if (!self->plugin->activate(self->plugin, self->sample_rate, 1, self->max_frames))
    return false;
  self->activated = true;
  if (!self->plugin->start_processing(self->plugin))
    fprintf(stderr, "wclap~: %s: start_processing() failed\n", self->name);
  return true;
}

bool wclap_host_set_audio_config(WclapHost* host, double sample_rate, uint32_t max_frames) {
  if (!host || !host->plugin)
    return false;
  if (max_frames == 0)
    max_frames = 1;
  if (sample_rate == host->sample_rate && max_frames == host->max_frames)
    return true;

  pthread_mutex_lock(&host->lock);
  deactivate(host);
  double previous_rate = host->sample_rate;
  uint32_t previous_frames = host->max_frames;
  host->sample_rate = sample_rate;
  host->max_frames = max_frames;
  bool ok = allocate_buffers(host) && activate(host);
  pthread_mutex_unlock(&host->lock);

  if (!ok) {
    fprintf(stderr, "wclap~: %s: cannot activate at %g Hz / %u frames\n", host->name, sample_rate, max_frames);
    pthread_mutex_lock(&host->lock);
    host->sample_rate = previous_rate;
    host->max_frames = previous_frames;
    ok = allocate_buffers(host) && activate(host);
    pthread_mutex_unlock(&host->lock);
  }
  return ok;
}

// Fills the per-port channel table for one direction and returns the total
// channel count across ports.
static uint32_t scan_ports(const clap_plugin_t* plugin, const clap_plugin_audio_ports_t* ports, bool is_input,
                           uint32_t* port_channels, uint32_t* out_port_count) {
  uint32_t total = 0;
  uint32_t port_count = ports ? ports->count(plugin, is_input) : 0;
  if (port_count > WCLAP_MAX_PORTS)
    port_count = WCLAP_MAX_PORTS;
  for (uint32_t i = 0; i < port_count; i++) {
    clap_audio_port_info_t info;
    memset(&info, 0, sizeof(info));
    if (!ports->get(plugin, i, is_input, &info) || info.channel_count == 0)
      continue;
    uint32_t channels = info.channel_count;
    if (total + channels > WCLAP_MAX_CHANNELS)
      channels = WCLAP_MAX_CHANNELS - total;
    port_channels[*out_port_count] = channels;
    (*out_port_count)++;
    total += channels;
    if (total >= WCLAP_MAX_CHANNELS)
      break;
  }
  return total;
}

WclapHost* wclap_host_open(const char* path, const char* plugin_id, double sample_rate,
                           uint32_t max_frames, char* err, uint32_t err_capacity) {
  if (!path || !path[0]) {
    set_err(err, err_capacity, "no plugin path");
    return NULL;
  }
  if (sample_rate <= 0)
    sample_rate = 44100.0;
  if (max_frames == 0)
    max_frames = 64;

  LibEntry* lib = lib_acquire(path, err, err_capacity);
  if (!lib)
    return NULL;

  const clap_plugin_factory_t* factory =
      (const clap_plugin_factory_t*)wclap_get_factory(lib->wclap, CLAP_PLUGIN_FACTORY_ID);
  if (!factory) {
    set_err(err, err_capacity, "'%s' has no CLAP plugin factory", path);
    lib_release(lib);
    return NULL;
  }

  uint32_t factory_count = factory->get_plugin_count(factory);
  const clap_plugin_descriptor_t* descriptor = NULL;
  for (uint32_t i = 0; i < factory_count; i++) {
    const clap_plugin_descriptor_t* candidate = factory->get_plugin_descriptor(factory, i);
    if (!candidate)
      continue;
    if (!plugin_id || !plugin_id[0] || strcmp(candidate->id, plugin_id) == 0) {
      descriptor = candidate;
      break;
    }
  }
  if (!descriptor) {
    set_err(err, err_capacity, "plugin id '%s' not found in '%s'", plugin_id ? plugin_id : "", path);
    lib_release(lib);
    return NULL;
  }

  WclapHost* self = calloc(1, sizeof(*self));
  if (!self) {
    set_err(err, err_capacity, "out of memory");
    lib_release(lib);
    return NULL;
  }
  self->host = s_host_template;
  self->host.host_data = self;
  self->lib = lib;
  self->sample_rate = sample_rate;
  self->max_frames = max_frames;
  pthread_mutex_init(&self->lock, NULL);
  snprintf(self->name, sizeof(self->name), "%s", descriptor->name ? descriptor->name : "");
  snprintf(self->id, sizeof(self->id), "%s", descriptor->id ? descriptor->id : "");

  self->plugin = factory->create_plugin(factory, &self->host, descriptor->id);
  if (!self->plugin) {
    set_err(err, err_capacity, "plugin '%s' failed to instantiate", self->id);
    wclap_host_close(self);
    return NULL;
  }
  if (!self->plugin->init(self->plugin)) {
    set_err(err, err_capacity, "plugin '%s' init() failed", self->id);
    wclap_host_close(self);
    return NULL;
  }

  const clap_plugin_audio_ports_t* audio_ports =
      (const clap_plugin_audio_ports_t*)self->plugin->get_extension(self->plugin, CLAP_EXT_AUDIO_PORTS);

  // Some frameworks (as-clap among them) always expose CLAP_EXT_NOTE_PORTS,
  // even for effects that never use it, so the port count decides — and it
  // has to be asked before the audio port scan below, which leans on it.
  const clap_plugin_note_ports_t* note_ports =
      (const clap_plugin_note_ports_t*)self->plugin->get_extension(self->plugin, CLAP_EXT_NOTE_PORTS);
  self->is_instrument = note_ports && note_ports->count(self->plugin, true) > 0;

  if (audio_ports) {
    self->input_channel_count = scan_ports(self->plugin, audio_ports, true, self->input_port_channels,
                                           &self->input_port_count);
    self->output_channel_count = scan_ports(self->plugin, audio_ports, false, self->output_port_channels,
                                            &self->output_port_count);
  } else {
    // No CLAP_EXT_AUDIO_PORTS at all: CLAP requires it, but a plugin like that
    // still writes to process->audio_outputs (poketrack's pthread-synth is
    // one), so assume the conventional stereo pair rather than handing it
    // nothing to write into.
    if (!self->is_instrument) {
      self->input_port_channels[0] = 2;
      self->input_port_count = 1;
      self->input_channel_count = 2;
    }
    self->output_port_channels[0] = 2;
    self->output_port_count = 1;
    self->output_channel_count = 2;
  }

  self->params = (const clap_plugin_params_t*)self->plugin->get_extension(self->plugin, CLAP_EXT_PARAMS);

  if (!allocate_buffers(self)) {
    set_err(err, err_capacity, "out of memory");
    wclap_host_close(self);
    return NULL;
  }
  pthread_mutex_lock(&self->lock);
  rebuild_param_table(self);
  bool activated = activate(self);
  pthread_mutex_unlock(&self->lock);
  if (!activated) {
    set_err(err, err_capacity, "plugin '%s' activate() failed at %g Hz", self->id, sample_rate);
    wclap_host_close(self);
    return NULL;
  }

  return self;
}

void wclap_host_close(WclapHost* host) {
  if (!host)
    return;
  pthread_mutex_lock(&host->lock);
  if (host->plugin) {
    deactivate(host);
    // Drain deferred main-thread work before destroy — safe now that audio is
    // stopped and we're on the main thread.
    for (int i = 0; i < 4 && atomic_load(&host->main_thread_pending); i++) {
      atomic_store(&host->main_thread_pending, false);
      if (host->plugin->on_main_thread)
        host->plugin->on_main_thread(host->plugin);
    }
    host->plugin->destroy(host->plugin);
    host->plugin = NULL;
  }
  pthread_mutex_unlock(&host->lock);
  pthread_mutex_destroy(&host->lock);
  free_buffers(host);
  free(host->param_table);
  lib_release(host->lib);
  free(host);
}

const char* wclap_host_name(const WclapHost* host) {
  return host ? host->name : "";
}

const char* wclap_host_id(const WclapHost* host) {
  return host ? host->id : "";
}

uint32_t wclap_host_input_count(const WclapHost* host) {
  return host ? host->input_channel_count : 0;
}

uint32_t wclap_host_output_count(const WclapHost* host) {
  return host ? host->output_channel_count : 0;
}

bool wclap_host_is_instrument(const WclapHost* host) {
  return host && host->is_instrument;
}

void wclap_host_main_thread_work(WclapHost* host) {
  if (!host)
    return;
  if (atomic_exchange(&host->rescan_pending, false)) {
    pthread_mutex_lock(&host->lock);
    rebuild_param_table(host);
    pthread_mutex_unlock(&host->lock);
  }
  if (atomic_exchange(&host->main_thread_pending, false)) {
    pthread_mutex_lock(&host->lock);
    if (host->plugin && host->plugin->on_main_thread)
      host->plugin->on_main_thread(host->plugin);
    pthread_mutex_unlock(&host->lock);
  }
}

uint32_t wclap_host_poll_output(WclapHost* host, WclapEvent* events, uint32_t max_events) {
  if (!host || !events)
    return 0;
  uint32_t count = 0;
  while (count < max_events && output_ring_pop(&host->output_ring, &events[count])) {
    if (events[count].kind == WCLAP_EVENT_PARAM) {
      // Resolved here rather than when the plugin pushed it: the table is the
      // main thread's, and it can be rebuilt between the two.
      const WclapParamInfo* param = wclap_host_param_by_id(host, events[count].param_id);
      events[count].index = param ? (int32_t)param->index : -1;
    }
    count++;
  }
  return count;
}

uint32_t wclap_host_output_dropped(WclapHost* host) {
  if (!host)
    return 0;
  return atomic_exchange(&host->output_dropped, 0);
}
