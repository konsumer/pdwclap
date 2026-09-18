// wclap~ — load CLAP plugins compiled to wasm32 (WCLAP) straight into Pd.
//
//   [wclap~ /path/to/thing.wclap.wasm]
//   [wclap~ /path/to/thing.wclap.wasm com.example.plugin-id]
//
// Inlets:  0 = control, 1 = signal L in, 2 = signal R in
// Outlets: 0 = signal L out, 1 = signal R out, 2 = control (query replies)
//
// Message API (left inlet):
//   note <key> [velocity] [channel]   note on; velocity 0 is a note off
//                                     (same shape [notein] emits, so wiring
//                                     [notein] straight into it just works)
//   allnotesoff                       note off for every key
//   <param> <value>                   set a param by name
//   list <name> <value>               set a param by name, the form a patch can
//                                     build from a slider ([list prepend <name>])
//   param <name> [value]              set (2 args) or query (1 arg) by name
//   params                            report every param on the control outlet
//   texts <name>                      report the display text of every step of
//                                     a stepped param — a preset picker's labels
//   text <name> <value>               plugin's display text for that value
//   load <path> [plugin-id]           swap the loaded plugin
//
// Control outlet (rightmost), besides the replies above:
//   paraminfo <index> <name> <min> <max> <default> <value> <stepped> <enum>
//   paramsdone <count>
//   param <name> <value>              a param the plugin changed itself
//   paramtext <name> <value> <string> one step's display text (from `texts`)
//   textsdone <name> <count>
//   text <name> <string>              display text for one value
//   note <key> <velocity> <channel>   a note the plugin played itself (an
//                                     arpeggiator, a MIDI-out effect, ...) —
//                                     what [notein] emits and [noteout] takes
//   allnotesoff                       the plugin released every note at once
//   noteend <key> <channel>           that voice finished (no Pd equivalent)
//   midi <byte> [<byte> <byte>]       raw MIDI from the plugin, for [midiout]
//
// Param changes and notes are queued and delivered at the start of the next
// DSP block, so with DSP off they simply wait until it's turned back on.

#include <math.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#if defined(_WIN32)
#include <io.h>
#else
#include <unistd.h>
#endif

#include "clap/clap.h"
#include "m_pd.h"
#include "wclap_host.h"

#define WCLAP_TILDE_MAXPATH 1024
#define WCLAP_TILDE_TICK_MS 20.0
typedef struct _wclap_tilde {
  t_object x_obj;
  WclapHost* host;
  // The patch this object lives in, for resolving relative plugin paths (see
  // wclap_tilde_resolve_path).
  t_canvas* x_canvas;
  // Guards x->host against the DSP thread: `load` and `free` swap/close it
  // while perform() may be mid-block.
  pthread_mutex_t lock;
  t_clock* clock;
  t_outlet* ctl_out;
  bool warned_output_dropped;
  bool warned_nothing_loaded;
  // One bit per param, set the first time a value for it had to be clamped.
  uint8_t warned_clamp[WCLAP_MAX_PARAMS / 8];

#if PD_FLOATSIZE != 32
  // Pd builds with 64-bit samples need a conversion hop: WCLAP audio is f32.
  float *conv_in_l, *conv_in_r, *conv_out_l, *conv_out_r;
  int conv_size;
#endif
} t_wclap_tilde;

static t_class* wclap_tilde_class;

// --- helpers ----------------------------------------------------------------

// sys_expandpath (~ for the home directory) is a Pd symbol that isn't in
// m_pd.h; [readsf~] and friends rely on the same one.
extern void sys_expandpath(const char* from, char* to, int bufsize);

static bool wclap_tilde_path_exists(const char* path) {
#if defined(_WIN32)
  struct _stat info;
  return _stat(path, &info) == 0;
#else
  struct stat info;
  return stat(path, &info) == 0;
#endif
}

// Absolute (or ~) as written; otherwise looked for next to the patch first —
// what a patch author writes — and then along Pd's own search path, so -path
// and [declare -path] work the way they do for [readsf~]/[soundfiler]. If
// neither turns anything up the path is passed through as written and
// wclap_open reports it.
//
// The patch directory comes from the canvas the object was created in: at
// message time there is no "current canvas" to ask (canvas_getcurrent() is
// only set while a patch is being built), which is why this used to resolve
// relative names against whatever directory Pd was started in.
static void wclap_tilde_resolve_path(t_wclap_tilde* x, t_symbol* s, char* out, size_t out_size) {
  const char* name = s->s_name;
  if (name[0] == '\0') {
    snprintf(out, out_size, "%s", name);
    return;
  }

  if (sys_isabsolutepath(name) || name[0] == '~') {
    sys_expandpath(name, out, (int)out_size);
    return;
  }

  if (x->x_canvas) {
    char beside[MAXPDSTRING];
    canvas_makefilename(x->x_canvas, name, beside, sizeof(beside));
    if (wclap_tilde_path_exists(beside)) {
      snprintf(out, out_size, "%s", beside);
      return;
    }

    char dir[MAXPDSTRING];
    char* filename = NULL;
    int fd = canvas_open(x->x_canvas, name, "", dir, &filename, MAXPDSTRING, 1);
    if (fd >= 0) {
#if defined(_WIN32)
      _close(fd);
#else
      close(fd);
#endif
      snprintf(out, out_size, "%s/%s", dir, filename);
      return;
    }
  }

  snprintf(out, out_size, "%s", name);
}

static void wclap_tilde_open_host(t_wclap_tilde* x, const char* path, const char* plugin_id) {
  char err[512];
  err[0] = '\0';
  int block_size = sys_getblksize();
  WclapHost* host = wclap_host_open(path, plugin_id, sys_getsr(), (uint32_t)(block_size > 0 ? block_size : 64), err,
                                    sizeof(err));
  if (!host) {
    pd_error(x, "wclap~: %s", err[0] ? err : "cannot load plugin");
    return;
  }

  pthread_mutex_lock(&x->lock);
  WclapHost* previous = x->host;
  x->host = host;
  pthread_mutex_unlock(&x->lock);
  wclap_host_close(previous);
}

// True the first time this param's value had to be clamped (see set_param).
static bool wclap_tilde_warned_clamp(t_wclap_tilde* x, uint32_t index) {
  if (index >= WCLAP_MAX_PARAMS)
    return true;  // no room to remember: don't warn at all
  uint8_t bit = (uint8_t)(1u << (index % 8));
  uint8_t* byte = &x->warned_clamp[index / 8];
  if (*byte & bit)
    return true;
  *byte |= bit;
  return false;
}

static void wclap_tilde_output_param(t_wclap_tilde* x, const char* name, double value) {
  t_atom out[2];
  SETSYMBOL(&out[0], gensym(name));
  SETFLOAT(&out[1], (t_float)value);
  outlet_anything(x->ctl_out, gensym("param"), 2, out);
}

// A param can be named either by name or by its index (what `params` reports
// first), which matters when a patch routes that output back into the object
// instead of naming params by hand.
static int wclap_tilde_find_param(WclapHost* host, const char* name) {
  int index = wclap_host_param_find(host, name);
  if (index >= 0)
    return index;
  char* end = NULL;
  long numeric = strtol(name, &end, 10);
  if (end && *end == '\0' && numeric >= 0 && (uint32_t)numeric < wclap_host_param_count(host))
    return (int)numeric;
  return -1;
}

// Param names arrive as symbols, but [param 5 0.5( should work too, so a
// float atom reads as its numeric spelling and matches a param index.
static void wclap_tilde_atom_name(const t_atom* a, char* out, size_t out_size) {
  if (a->a_type == A_SYMBOL)
    snprintf(out, out_size, "%s", a->a_w.w_symbol->s_name);
  else
    snprintf(out, out_size, "%g", (double)a->a_w.w_float);
}

static void wclap_tilde_set_param(t_wclap_tilde* x, const char* name, double value) {
  bool found = false;
  bool loaded = false;
  bool clamped = false;
  const WclapParamInfo* param = NULL;

  pthread_mutex_lock(&x->lock);
  if (x->host) {
    loaded = true;
    int index = wclap_tilde_find_param(x->host, name);
    if (index >= 0) {
      param = wclap_host_param_info(x->host, (uint32_t)index);
      clamped = !wclap_host_set_param(x->host, (uint32_t)index, value);
      found = true;
    }
  }
  pthread_mutex_unlock(&x->lock);

  if (loaded && !found) {
    pd_error(x, "wclap~: no param named '%s'", name);
    return;
  }

  // A slider wired to the wrong param shows up here and nowhere else: the
  // value is out of range for this one (say, a 0..255 slider against dexed's
  // Cutoff, which is 0..1), so it gets clamped and nothing audible happens.
  // Once per param, or a sweeping slider would spam the console.
  if (clamped && param && !wclap_tilde_warned_clamp(x, param->index)) {
    pd_error(x, "wclap~: '%s' takes %g..%g — %g clamped (wrong param for this control?)", param->name,
             param->min_value, param->max_value, value);
  }
}

static void wclap_tilde_output_value(t_wclap_tilde* x, const char* name, double value) {
  wclap_tilde_output_param(x, name, value);
}

// --- messages ---------------------------------------------------------------

static void wclap_tilde_note(t_wclap_tilde* x, t_symbol* s, int argc, t_atom* argv) {
  (void)s;
  if (argc < 1) {
    pd_error(x, "wclap~: note needs a key");
    return;
  }
  int key = (int)atom_getfloat(&argv[0]);
  double velocity = argc > 1 ? atom_getfloat(&argv[1]) / 127.0 : 1.0;  // [notein] sends 0..127
  int channel = argc > 2 ? (int)atom_getfloat(&argv[2]) : 0;
  if (velocity < 0.0)
    velocity = 0.0;
  if (velocity > 1.0)
    velocity = 1.0;

  pthread_mutex_lock(&x->lock);
  if (x->host) {
    if (velocity > 0.0)
      wclap_host_note_on(x->host, key, velocity, channel);
    else
      wclap_host_note_off(x->host, key, channel);
  }
  pthread_mutex_unlock(&x->lock);
}

static void wclap_tilde_allnotesoff(t_wclap_tilde* x) {
  pthread_mutex_lock(&x->lock);
  if (x->host)
    wclap_host_all_notes_off(x->host);
  pthread_mutex_unlock(&x->lock);
}

// [param foo 0.5( sets, [param foo( asks the plugin what foo is now.
static void wclap_tilde_param(t_wclap_tilde* x, t_symbol* s, int argc, t_atom* argv) {
  (void)s;
  if (argc < 1) {
    pd_error(x, "wclap~: param needs a name");
    return;
  }
  char name[256];
  wclap_tilde_atom_name(&argv[0], name, sizeof(name));

  if (argc > 1) {
    wclap_tilde_set_param(x, name, atom_getfloat(&argv[1]));
    return;
  }

  bool loaded = false;
  bool found = false;
  double value = 0.0;

  pthread_mutex_lock(&x->lock);
  if (x->host) {
    loaded = true;
    int index = wclap_tilde_find_param(x->host, name);
    if (index >= 0) {
      value = wclap_host_param_value(x->host, (uint32_t)index);
      found = true;
    }
  }
  pthread_mutex_unlock(&x->lock);

  if (loaded && !found) {
    pd_error(x, "wclap~: no param named '%s'", name);
    return;
  }
  if (found)
    wclap_tilde_output_value(x, name, value);
}

// Any selector that isn't a built-in message is treated as a param name, so
// patches can say [cutoff 1000( instead of [param cutoff 1000(.
static void wclap_tilde_anything(t_wclap_tilde* x, t_symbol* s, int argc, t_atom* argv) {
  if (argc < 1) {
    pd_error(x, "wclap~: '%s' needs a value", s->s_name);
    return;
  }
  wclap_tilde_set_param(x, s->s_name, atom_getfloat(&argv[0]));
}

// A list message names the param in its first atom: `list <name> <value>` sets
// and `list <name>` queries. That is how a patch drives a param from a slider
// — a top-level patch can't put $1 in a message box (it is expanded at load
// against the patch's own arguments, which there are none of), so building the
// `param <name> <value>` message has to go through [list prepend] instead.
static void wclap_tilde_list(t_wclap_tilde* x, t_symbol* s, int argc, t_atom* argv) {
  (void)s;
  if (argc < 1) {
    pd_error(x, "wclap~: list needs a param name");
    return;
  }
  char name[256];
  wclap_tilde_atom_name(&argv[0], name, sizeof(name));
  if (argc < 2) {
    wclap_tilde_param(x, s, 1, argv);  // same as `param <name>`
    return;
  }
  wclap_tilde_set_param(x, name, atom_getfloat(&argv[1]));
}

// Snapshots the table before emitting: outlet_anything can run patch code
// that reloads the plugin, which would free the very table being read.
static void wclap_tilde_params(t_wclap_tilde* x) {
  uint32_t count = 0;
  typedef struct {
    char name[256];
    double min, max, default_value, value;
    bool stepped;
    bool enumeration;  // every step has a name (CLAP_PARAM_IS_ENUM)
  } Row;
  Row* rows = NULL;

  pthread_mutex_lock(&x->lock);
  if (x->host) {
    count = wclap_host_param_count(x->host);
    rows = (Row*)malloc((count ? count : 1) * sizeof(*rows));
    if (rows) {
      uint32_t kept = 0;
      for (uint32_t i = 0; i < count; i++) {
        const WclapParamInfo* param = wclap_host_param_info(x->host, i);
        if (!param)
          continue;
        Row* row = &rows[kept++];
        snprintf(row->name, sizeof(row->name), "%s", param->name);
        row->min = param->min_value;
        row->max = param->max_value;
        row->default_value = param->default_value;
        row->value = wclap_host_param_value(x->host, i);
        row->stepped = (param->flags & CLAP_PARAM_IS_STEPPED) != 0;
        row->enumeration = (param->flags & CLAP_PARAM_IS_ENUM) != 0;
      }
      count = kept;
    } else {
      count = 0;
    }
  }
  pthread_mutex_unlock(&x->lock);

  for (uint32_t i = 0; i < count; i++) {
    t_atom out[8];
    SETFLOAT(&out[0], (t_float)i);
    SETSYMBOL(&out[1], gensym(rows[i].name));
    SETFLOAT(&out[2], (t_float)rows[i].min);
    SETFLOAT(&out[3], (t_float)rows[i].max);
    SETFLOAT(&out[4], (t_float)rows[i].default_value);
    SETFLOAT(&out[5], (t_float)rows[i].value);
    SETFLOAT(&out[6], rows[i].stepped ? 1.0 : 0.0);
    SETFLOAT(&out[7], rows[i].enumeration ? 1.0 : 0.0);
    outlet_anything(x->ctl_out, gensym("paraminfo"), 8, out);
  }
  free(rows);

  t_atom out[1];
  SETFLOAT(&out[0], (t_float)count);
  outlet_anything(x->ctl_out, gensym("paramsdone"), 1, out);
}

// One step's display text per step of a stepped param: this is where a synth's
// preset names live ("PolySynth1", "Dexed_01"), the same strings poketrack
// shows in place of the raw number.
//
// Every label is a call into the wasm plugin under the lock the DSP thread
// shares, and juno1's Patch has 256 of them, so they go out in batches with
// the lock released in between — one label is one short lock hold, not a
// 256-call stall in the middle of a block.
#define WCLAP_TILDE_LABEL_BATCH 32
#define WCLAP_TILDE_MAX_LABELS 1024

static void wclap_tilde_texts(t_wclap_tilde* x, t_symbol* s, int argc, t_atom* argv) {
  (void)s;
  if (argc < 1) {
    pd_error(x, "wclap~: texts needs a param name");
    return;
  }
  char name[256];
  wclap_tilde_atom_name(&argv[0], name, sizeof(name));

  if (!x->host) {
    if (!x->warned_nothing_loaded) {
      x->warned_nothing_loaded = true;
      pd_error(x, "wclap~: no plugin loaded");
    }
    return;
  }

  double value = 0.0;
  double last = -1.0;
  bool found = false;
  bool stepped = false;

  pthread_mutex_lock(&x->lock);
  if (x->host) {
    int index = wclap_tilde_find_param(x->host, name);
    if (index >= 0) {
      const WclapParamInfo* param = wclap_host_param_info(x->host, (uint32_t)index);
      found = true;
      // A stepped param's values are whole numbers in [min, max]; anything
      // else has no finite set of labels (its text is just a formatted number).
      stepped = (param->flags & CLAP_PARAM_IS_STEPPED) != 0;
      if (stepped) {
        // ceil() of a value just under zero gives -0.0, which Pd would print
        // as "-0"; fabs() clears the sign where a plain 0.0 assignment can't.
        value = fabs(ceil(param->min_value - 0.5));
        last = floor(param->max_value + 0.5);
        if (last - value > WCLAP_TILDE_MAX_LABELS - 1)
          last = value + WCLAP_TILDE_MAX_LABELS - 1;
      }
    }
  }
  pthread_mutex_unlock(&x->lock);

  if (!found) {
    pd_error(x, "wclap~: no param named '%s'", name);
    return;
  }

  typedef struct {
    double value;
    char text[256];
  } Label;
  Label batch[WCLAP_TILDE_LABEL_BATCH];
  uint32_t emitted = 0;

  while (stepped && value <= last) {
    uint32_t count = 0;
    pthread_mutex_lock(&x->lock);
    WclapHost* host = x->host;  // re-read: emitting can run a reentrant [load(
    int index = host ? wclap_tilde_find_param(host, name) : -1;
    if (index >= 0) {
      for (count = 0; count < WCLAP_TILDE_LABEL_BATCH && value <= last; count++, value += 1.0) {
        batch[count].value = value;
        batch[count].text[0] = '\0';
        wclap_host_param_text(host, (uint32_t)index, value, batch[count].text, sizeof(batch[count].text));
      }
    }
    pthread_mutex_unlock(&x->lock);

    if (index < 0)
      break;
    for (uint32_t i = 0; i < count; i++) {
      if (!batch[i].text[0])
        continue;  // this step has no text to show
      t_atom out[3];
      SETSYMBOL(&out[0], gensym(name));
      SETFLOAT(&out[1], (t_float)batch[i].value);
      SETSYMBOL(&out[2], gensym(batch[i].text));
      outlet_anything(x->ctl_out, gensym("paramtext"), 3, out);
      emitted++;
    }
  }

  t_atom out[2];
  SETSYMBOL(&out[0], gensym(name));
  SETFLOAT(&out[1], (t_float)emitted);
  outlet_anything(x->ctl_out, gensym("textsdone"), 2, out);
}

static void wclap_tilde_text(t_wclap_tilde* x, t_symbol* s, int argc, t_atom* argv) {
  (void)s;
  if (argc < 2) {
    pd_error(x, "wclap~: text needs a param name and a value");
    return;
  }
  char name[256];
  wclap_tilde_atom_name(&argv[0], name, sizeof(name));
  char text[256];
  text[0] = '\0';
  bool loaded = false;
  bool ok = false;

  pthread_mutex_lock(&x->lock);
  if (x->host) {
    loaded = true;
    int index = wclap_tilde_find_param(x->host, name);
    ok = index >= 0 && wclap_host_param_text(x->host, (uint32_t)index, atom_getfloat(&argv[1]), text, sizeof(text));
  }
  pthread_mutex_unlock(&x->lock);

  if (!loaded)
    return;
  if (!ok) {
    pd_error(x, "wclap~: '%s' has no display text for that value", name);
    return;
  }

  t_atom out[2];
  SETSYMBOL(&out[0], gensym(name));
  SETSYMBOL(&out[1], gensym(text));
  outlet_anything(x->ctl_out, gensym("text"), 2, out);
}

static void wclap_tilde_load(t_wclap_tilde* x, t_symbol* s, int argc, t_atom* argv) {
  (void)s;
  if (argc < 1) {
    pd_error(x, "wclap~: load needs a path");
    return;
  }
  char path[WCLAP_TILDE_MAXPATH];
  wclap_tilde_resolve_path(x, atom_getsymbol(&argv[0]), path, sizeof(path));
  const char* plugin_id = argc > 1 ? atom_getsymbol(&argv[1])->s_name : "";
  wclap_tilde_open_host(x, path, plugin_id);
}

// --- dsp --------------------------------------------------------------------

static t_int* wclap_tilde_perform(t_int* w) {
  t_wclap_tilde* x = (t_wclap_tilde*)(w[1]);
  t_sample* in_l = (t_sample*)(w[2]);
  t_sample* in_r = (t_sample*)(w[3]);
  t_sample* out_l = (t_sample*)(w[4]);
  t_sample* out_r = (t_sample*)(w[5]);
  int n = (int)(w[6]);

  pthread_mutex_lock(&x->lock);
  WclapHost* host = x->host;
  if (host) {
#if PD_FLOATSIZE == 32
    wclap_host_process(host, (const float*)in_l, (const float*)in_r, (float*)out_l, (float*)out_r, (uint32_t)n);
#else
    int frames = n < x->conv_size ? n : x->conv_size;
    for (int i = 0; i < frames; i++) {
      x->conv_in_l[i] = (float)in_l[i];
      x->conv_in_r[i] = (float)in_r[i];
    }
    wclap_host_process(host, x->conv_in_l, x->conv_in_r, x->conv_out_l, x->conv_out_r, (uint32_t)frames);
    for (int i = 0; i < frames; i++) {
      out_l[i] = (t_sample)x->conv_out_l[i];
      out_r[i] = (t_sample)x->conv_out_r[i];
    }
    for (int i = frames; i < n; i++) {
      out_l[i] = 0;
      out_r[i] = 0;
    }
#endif
  } else {
    memset(out_l, 0, (size_t)n * sizeof(*out_l));
    memset(out_r, 0, (size_t)n * sizeof(*out_r));
  }
  pthread_mutex_unlock(&x->lock);
  return (w + 7);
}

static void wclap_tilde_dsp(t_wclap_tilde* x, t_signal** sp) {
  int n = (int)sp[0]->s_n;
  if (n <= 0)
    return;

  pthread_mutex_lock(&x->lock);
#if PD_FLOATSIZE != 32
  if (x->conv_size < n) {
    freebytes(x->conv_in_l, (size_t)x->conv_size * sizeof(float));
    freebytes(x->conv_in_r, (size_t)x->conv_size * sizeof(float));
    freebytes(x->conv_out_l, (size_t)x->conv_size * sizeof(float));
    freebytes(x->conv_out_r, (size_t)x->conv_size * sizeof(float));
    x->conv_in_l = (float*)getbytes((size_t)n * sizeof(float));
    x->conv_in_r = (float*)getbytes((size_t)n * sizeof(float));
    x->conv_out_l = (float*)getbytes((size_t)n * sizeof(float));
    x->conv_out_r = (float*)getbytes((size_t)n * sizeof(float));
    x->conv_size = n;
  }
#endif
  // Pd only re-runs dsp() when the graph or the audio settings change, which
  // is exactly when the sample rate or block size can differ from what the
  // plugin was activated with. No-op when neither changed.
  if (x->host)
    wclap_host_set_audio_config(x->host, sys_getsr(), (uint32_t)n);
  pthread_mutex_unlock(&x->lock);

  dsp_add(wclap_tilde_perform, 6, x, sp[0]->s_vec, sp[1]->s_vec, sp[2]->s_vec, sp[3]->s_vec, n);
}

// The plugin's own events reach us on the DSP thread, and Pd's messaging isn't
// thread-safe, so they wait in a queue the clock drains here. Emitted after the
// lock is dropped: outlet_anything runs patch code, which may reload the
// plugin out from under us.
#define WCLAP_TILDE_MAX_OUT_EVENTS 32

// CLAP hands over three MIDI bytes; how many of them are real depends on the
// status byte, and a consumer like [midiout] can't tell a trailing zero from a
// data byte.
static int wclap_tilde_midi_length(uint8_t status) {
  switch (status & 0xf0) {
    case 0x80:  // note off
    case 0x90:  // note on
    case 0xa0:  // poly aftertouch
    case 0xb0:  // control change
    case 0xe0:  // pitch bend
      return 3;
    case 0xc0:  // program change
    case 0xd0:  // channel aftertouch
      return 2;
    default:
      break;
  }
  if (status == 0xf1 || status == 0xf3)  // MTC quarter frame, song select
    return 2;
  if (status == 0xf2)  // song position
    return 3;
  return 1;  // system/realtime bytes stand alone
}

static void wclap_tilde_emit_output(t_wclap_tilde* x) {
  WclapEvent events[WCLAP_TILDE_MAX_OUT_EVENTS];
  t_symbol* param_names[WCLAP_TILDE_MAX_OUT_EVENTS];
  uint32_t count = 0;
  uint32_t dropped = 0;

  pthread_mutex_lock(&x->lock);
  if (x->host) {
    count = wclap_host_poll_output(x->host, events, WCLAP_TILDE_MAX_OUT_EVENTS);
    for (uint32_t i = 0; i < count; i++) {
      param_names[i] = NULL;
      if (events[i].kind != WCLAP_EVENT_PARAM || events[i].index < 0)
        continue;
      const WclapParamInfo* param = wclap_host_param_info(x->host, (uint32_t)events[i].index);
      if (param)
        param_names[i] = gensym(param->name);
    }
    dropped = wclap_host_output_dropped(x->host);
  }
  pthread_mutex_unlock(&x->lock);

  if (dropped && !x->warned_output_dropped) {
    // Once per object: a plugin can emit these every block.
    x->warned_output_dropped = true;
    pd_error(x,
             "wclap~: dropped %u plugin output event(s) — no Pd equivalent (note expression, modulation, sysex) or a full queue",
             dropped);
  }

  for (uint32_t i = 0; i < count; i++) {
    const WclapEvent* event = &events[i];
    t_atom out[3];
    switch (event->kind) {
      case WCLAP_EVENT_NOTE_ON:
      case WCLAP_EVENT_NOTE_OFF:
      case WCLAP_EVENT_NOTE_CHOKE: {
        // CLAP's choke means "steal this voice", which Pd has no expression
        // for, so it goes out as a note off — as does a note off for every
        // note (a wildcard key).
        bool on = event->kind == WCLAP_EVENT_NOTE_ON;
        if (!on && event->key < 0) {
          // A wildcard note off means "every note", which is a Pd message of
          // its own — and one this object understands on its left inlet.
          outlet_anything(x->ctl_out, gensym("allnotesoff"), 0, NULL);
          break;
        }
        if (on && event->key < 0)
          break;  // a note on has to name a key
        double velocity = 0.0;
        if (on) {
          // Pd velocities are whole numbers 1..127; 0 is how it spells note
          // off, so a note on never rounds down to it.
          velocity = (double)(int)(event->value * 127.0 + 0.5);
          if (velocity < 1.0)
            velocity = 1.0;
          if (velocity > 127.0)
            velocity = 127.0;
        }
        SETFLOAT(&out[0], (t_float)event->key);
        SETFLOAT(&out[1], (t_float)velocity);
        SETFLOAT(&out[2], (t_float)(event->channel < 0 ? 0 : event->channel));
        outlet_anything(x->ctl_out, gensym("note"), 3, out);
        break;
      }
      case WCLAP_EVENT_NOTE_END: {
        // Not a Pd concept (the voice just finished), but a patch holding a
        // note map may want to know.
        SETFLOAT(&out[0], (t_float)event->key);
        SETFLOAT(&out[1], (t_float)(event->channel < 0 ? 0 : event->channel));
        outlet_anything(x->ctl_out, gensym("noteend"), 2, out);
        break;
      }
      case WCLAP_EVENT_PARAM: {
        if (!param_names[i])
          break;  // an id that isn't in the table
        SETSYMBOL(&out[0], param_names[i]);
        SETFLOAT(&out[1], (t_float)event->value);
        outlet_anything(x->ctl_out, gensym("param"), 2, out);
        break;
      }
      case WCLAP_EVENT_MIDI: {
        int length = wclap_tilde_midi_length(event->midi[0]);
        for (int byte = 0; byte < length; byte++)
          SETFLOAT(&out[byte], (t_float)event->midi[byte]);
        outlet_anything(x->ctl_out, gensym("midi"), length, out);
        break;
      }
    }
  }
}

// --- lifecycle --------------------------------------------------------------

// Plugins are allowed to ask the host to call back later (a preset load, a
// background scan), and their own output events wait in a queue — both have to
// happen on the main thread. 20ms is the worst-case delay a note a plugin
// played takes to come out of the control outlet.
static void wclap_tilde_tick(t_wclap_tilde* x) {
  pthread_mutex_lock(&x->lock);
  if (x->host)
    wclap_host_main_thread_work(x->host);
  pthread_mutex_unlock(&x->lock);

  wclap_tilde_emit_output(x);
  clock_delay(x->clock, WCLAP_TILDE_TICK_MS);
}

static void* wclap_tilde_new(t_symbol* s, int argc, t_atom* argv) {
  (void)s;
  t_wclap_tilde* x = (t_wclap_tilde*)pd_new(wclap_tilde_class);
  // Valid here and now (Pd sets the current canvas while building a patch),
  // but not from a message method later on.
  x->x_canvas = canvas_getcurrent();

  // Control-only leftmost inlet, then the audio pair — same shape as [vst~].
  inlet_new(&x->x_obj, &x->x_obj.ob_pd, &s_signal, &s_signal);
  inlet_new(&x->x_obj, &x->x_obj.ob_pd, &s_signal, &s_signal);
  outlet_new(&x->x_obj, &s_signal);
  outlet_new(&x->x_obj, &s_signal);
  x->ctl_out = outlet_new(&x->x_obj, 0);

  pthread_mutex_init(&x->lock, NULL);
  x->host = NULL;
  x->clock = clock_new(x, (t_method)wclap_tilde_tick);

  if (argc > 0) {
    if (argc > 2)
      pd_error(x, "wclap~: extra arguments ignored (want: <path> [plugin-id])");
    char path[WCLAP_TILDE_MAXPATH];
    wclap_tilde_resolve_path(x, atom_getsymbol(&argv[0]), path, sizeof(path));
    const char* plugin_id = argc > 1 ? atom_getsymbol(&argv[1])->s_name : "";
    wclap_tilde_open_host(x, path, plugin_id);
  } else {
    post("wclap~: nothing loaded — send [load <path>( to pick a plugin");
  }

  clock_delay(x->clock, WCLAP_TILDE_TICK_MS);
  return x;
}

static void wclap_tilde_free(t_wclap_tilde* x) {
  clock_free(x->clock);

  pthread_mutex_lock(&x->lock);
  WclapHost* host = x->host;
  x->host = NULL;
  pthread_mutex_unlock(&x->lock);
  wclap_host_close(host);

#if PD_FLOATSIZE != 32
  freebytes(x->conv_in_l, (size_t)x->conv_size * sizeof(float));
  freebytes(x->conv_in_r, (size_t)x->conv_size * sizeof(float));
  freebytes(x->conv_out_l, (size_t)x->conv_size * sizeof(float));
  freebytes(x->conv_out_r, (size_t)x->conv_size * sizeof(float));
#endif
  pthread_mutex_destroy(&x->lock);
}

// Pd looks the setup function up by name once it has loaded the external,
// which the unices do for every non-static symbol but MSVC only does for
// what is explicitly marked for export.
#ifdef _MSC_VER
#define WCLAP_TILDE_SETUP __declspec(dllexport)
#else
#define WCLAP_TILDE_SETUP
#endif

WCLAP_TILDE_SETUP void wclap_tilde_setup(void) {
  wclap_tilde_class = class_new(gensym("wclap~"), (t_newmethod)wclap_tilde_new, (t_method)wclap_tilde_free,
                                sizeof(t_wclap_tilde), CLASS_DEFAULT, A_GIMME, 0);

  class_addmethod(wclap_tilde_class, (t_method)wclap_tilde_dsp, gensym("dsp"), A_CANT, 0);
  class_addmethod(wclap_tilde_class, (t_method)wclap_tilde_note, gensym("note"), A_GIMME, 0);
  class_addmethod(wclap_tilde_class, (t_method)wclap_tilde_allnotesoff, gensym("allnotesoff"), A_NULL, 0);
  class_addmethod(wclap_tilde_class, (t_method)wclap_tilde_param, gensym("param"), A_GIMME, 0);
  class_addlist(wclap_tilde_class, (t_method)wclap_tilde_list);
  class_addmethod(wclap_tilde_class, (t_method)wclap_tilde_params, gensym("params"), A_NULL, 0);
  class_addmethod(wclap_tilde_class, (t_method)wclap_tilde_text, gensym("text"), A_GIMME, 0);
  class_addmethod(wclap_tilde_class, (t_method)wclap_tilde_texts, gensym("texts"), A_GIMME, 0);
  class_addmethod(wclap_tilde_class, (t_method)wclap_tilde_load, gensym("load"), A_GIMME, 0);
  class_addanything(wclap_tilde_class, (t_method)wclap_tilde_anything);
}
