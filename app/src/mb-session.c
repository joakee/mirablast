/* mirablast - session controller
 *
 * Copyright (C) 2026 James Oakey
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The display management logic is ported from the miracast-extend script and
 * the reconnect machinery from the nd-window patch, both of which were tested
 * at length against real hardware. Where this file makes a choice that looks
 * odd (XDamage left on, xrandr --current everywhere, the reconnect settle and
 * budget rules), there is a hard-won reason; see the comments at each site.
 */

#include "mb-session.h"

#include <gio/gio.h>
#include <gst/gst.h>
#include <stdio.h>

#include "nd-dummy-provider.h"
#include "nd-meta-sink.h"
#include "nd-nm-device-registry.h"
#include "nd-pulseaudio.h"

#define STARTUP_TICK_MS      250
#define STARTUP_MAX_TICKS    180  /* 45s: evdi_open alone takes ~8-12s */
#define STARTUP_PROBE_AFTER  60   /* ticks before the probing fallback kicks in */
#define GUARD_TICKS          32   /* 8s panel-protection watch */
#define GEOM_WATCH_MS        1000
#define RECONNECT_SETTLE_S   6    /* sink reports DISCONNECTED before P2P is gone */
#define RECONNECT_TRIES      3
#define RECONNECT_DEADLINE_S 60   /* rediscovery wait */
#define RECONNECT_RATE_S     60   /* suppress repeats for the same region */

struct _MbSession
{
  GObject             parent_instance;

  MbSettings         *settings;
  GCancellable       *cancellable;

  NdMetaProvider     *meta_provider;
  NdNMDeviceRegistry *nm_registry;
  NdDummyProvider    *dummy_provider;
  NdPulseaudio       *pulse;

  /* virtual display */
  GSubprocess        *helper;
  gchar              *output;      /* e.g. DVI-I-1-1 */
  gchar              *primary;     /* resolved at start */
  guint               startup_id;
  guint               startup_ticks;
  gint                guard_ticks; /* -1 = not in guard phase */
  guint               geom_id;
  gint                rx, ry;
  guint               rw, rh;

  /* streaming */
  NdSink             *stream_sink;   /* the running (underlying) sink */
  NdSink             *connect_sink;  /* what was picked from the list */
  NdSink             *pending_sink;  /* picked before the display was up */
  GstElement         *capture_src;   /* weak */
  guint               exp_w, exp_h;  /* capture size fixed at connect */
  gchar              *warned_region; /* last region we warned about */

  /* reconnect (ported guards: settle, retries, rediscovery deadline,
   * rate limit, stuck-timer retirement, budget-clear-on-streaming) */
  guint               reconnect_id;
  guint               reconnect_wait;
  guint               reconnect_tries;
  guint               reconnect_deadline;
  gint64              reconnect_last;
  gchar              *reconnect_last_region;

  MbSessionState      state;
  gchar              *error_message;
};

G_DEFINE_TYPE (MbSession, mb_session, G_TYPE_OBJECT)

enum {
  STATE_CHANGED,
  N_SIGNALS
};
static guint signals[N_SIGNALS];

static void mb_session_do_connect (MbSession *self, NdSink *sink);
static void connect_pending (MbSession *self);

/* ------------------------------------------------------------------ */
/* helpers                                                             */

static void
set_state (MbSession *self, MbSessionState state, const gchar *error_message)
{
  if (self->state == state && !error_message)
    return;

  self->state = state;
  g_free (self->error_message);
  self->error_message = g_strdup (error_message);

  /* A queued pick only survives while the display is on its way up. */
  if (state == MB_SESSION_STATE_ERROR)
    g_clear_object (&self->pending_sink);

  {
    static const gchar *names[] = { "idle", "display-starting", "display-ready",
                                    "connecting", "streaming", "error" };
    g_message ("MbSession: state -> %s%s%s", names[state],
               error_message ? ": " : "", error_message ? error_message : "");
  }
  g_signal_emit (self, signals[STATE_CHANGED], 0);
}

static gchar *
run_argv (const gchar *const *argv)
{
  g_autoptr(GError) error = NULL;
  gchar *out = NULL;
  gint status = 0;

  if (!g_spawn_sync (NULL, (gchar **) argv, NULL,
                     G_SPAWN_STDERR_TO_DEV_NULL | G_SPAWN_SEARCH_PATH,
                     NULL, NULL, &out, NULL, &status, &error))
    {
      g_warning ("MbSession: spawning %s failed: %s", argv[0], error->message);
      return NULL;
    }
  return out;
}

/* xrandr --current reads the server's cached view in ~5ms. A plain query
 * re-probes every output over DDC, blocks X for ~0.8s, and polling it froze
 * every display on the machine once a second. Nothing here probes except the
 * delayed startup fallback. */
static gchar *
xrandr_current (void)
{
  const gchar *argv[] = { "xrandr", "--query", "--current", NULL };

  return run_argv (argv);
}

static gboolean
parse_geom (const gchar *listing, const gchar *name,
            guint *w, guint *h, gint *x, gint *y)
{
  g_autofree gchar *pattern = NULL;
  g_autoptr(GRegex) re = NULL;
  g_autoptr(GMatchInfo) mi = NULL;
  g_autofree gchar *escaped = NULL;

  if (!listing || !name)
    return FALSE;

  escaped = g_regex_escape_string (name, -1);
  pattern = g_strdup_printf ("^%s connected(?: primary)? (\\d+)x(\\d+)\\+(\\d+)\\+(\\d+)",
                             escaped);
  re = g_regex_new (pattern, G_REGEX_MULTILINE, 0, NULL);
  if (!re || !g_regex_match (re, listing, 0, &mi))
    return FALSE;

  if (w) *w = (guint) g_ascii_strtoull (g_match_info_fetch (mi, 1), NULL, 10);
  if (h) *h = (guint) g_ascii_strtoull (g_match_info_fetch (mi, 2), NULL, 10);
  if (x) *x = (gint) g_ascii_strtoll (g_match_info_fetch (mi, 3), NULL, 10);
  if (y) *y = (gint) g_ascii_strtoll (g_match_info_fetch (mi, 4), NULL, 10);
  return TRUE;
}

static gchar *
detect_primary (const gchar *listing)
{
  g_autoptr(GRegex) re_primary = NULL;
  g_autoptr(GRegex) re_any = NULL;
  g_autoptr(GMatchInfo) mi = NULL;

  re_primary = g_regex_new ("^(\\S+) connected primary", G_REGEX_MULTILINE, 0, NULL);
  if (g_regex_match (re_primary, listing, 0, &mi))
    return g_match_info_fetch (mi, 1);
  g_clear_pointer (&mi, g_match_info_unref);

  re_any = g_regex_new ("^(\\S+) connected", G_REGEX_MULTILINE, 0, NULL);
  if (g_regex_match (re_any, listing, 0, &mi))
    return g_match_info_fetch (mi, 1);

  return NULL;
}

/* Name of the X output backed by the evdi card: find the evdi DRM connector
 * in sysfs, then match it against the outputs X knows about (X appends an
 * index, e.g. DVI-I-1 becomes DVI-I-1-1). */
static gchar *
find_evdi_output (const gchar *listing)
{
  g_autoptr(GDir) dir = NULL;
  const gchar *entry;

  dir = g_dir_open ("/sys/class/drm", 0, NULL);
  if (!dir)
    return NULL;

  while ((entry = g_dir_read_name (dir)))
    {
      g_autofree gchar *driver_link = NULL;
      g_autofree gchar *driver = NULL;
      g_autofree gchar *card = NULL;
      g_autofree gchar *pattern = NULL;
      g_autoptr(GRegex) re = NULL;
      g_autoptr(GMatchInfo) mi = NULL;
      const gchar *dash;

      dash = strchr (entry, '-');
      if (!dash || !g_str_has_prefix (entry, "card"))
        continue;

      card = g_strndup (entry, dash - entry);
      driver_link = g_strdup_printf ("/sys/class/drm/%s/device/driver", card);
      driver = g_file_read_link (driver_link, NULL);
      if (!driver || !g_str_has_suffix (driver, "/evdi"))
        continue;

      {
        g_autofree gchar *conn = g_regex_escape_string (dash + 1, -1);
        pattern = g_strdup_printf ("^(%s(?:-\\d+)?) (?:dis)?connected", conn);
      }
      re = g_regex_new (pattern, G_REGEX_MULTILINE, 0, NULL);
      if (re && g_regex_match (re, listing, 0, &mi))
        return g_match_info_fetch (mi, 1);
    }
  return NULL;
}

static gchar *
resolve_helper (MbSession *self)
{
  const gchar *configured = mb_settings_get_helper (self->settings);
  g_autofree gchar *exe_dir = NULL;
  const gchar *candidates[3];
  gchar *path;

  if (configured && *configured)
    {
      if (g_file_test (configured, G_FILE_TEST_IS_EXECUTABLE))
        return g_strdup (configured);
      g_warning ("MbSession: configured helper %s is not executable", configured);
    }

  /* Next to the repo layout (app/build/../.. -> bin/), then ~/.local/bin,
   * then PATH. */
  {
    g_autofree gchar *self_path = g_file_read_link ("/proc/self/exe", NULL);
    if (self_path)
      exe_dir = g_path_get_dirname (self_path);
  }

  if (exe_dir)
    {
      g_autofree gchar *repo_helper =
        g_build_filename (exe_dir, "..", "..", "bin", "evdi-virtual-display", NULL);
      if (g_file_test (repo_helper, G_FILE_TEST_IS_EXECUTABLE))
        return g_steal_pointer (&repo_helper);
    }

  candidates[0] = "evdi-virtual-display";
  candidates[1] = NULL;

  {
    g_autofree gchar *local =
      g_build_filename (g_get_home_dir (), ".local", "bin", "evdi-virtual-display", NULL);
    if (g_file_test (local, G_FILE_TEST_IS_EXECUTABLE))
      return g_steal_pointer (&local);
  }

  path = g_find_program_in_path (candidates[0]);
  return path;
}

/* ------------------------------------------------------------------ */
/* virtual display lifecycle                                           */

static void
helper_exited_cb (GObject *source, GAsyncResult *res, gpointer user_data)
{
  MbSession *self = MB_SESSION (user_data);

  g_subprocess_wait_finish (G_SUBPROCESS (source), res, NULL);

  /* Only relevant if it died while we still wanted it. */
  if (self->helper == G_SUBPROCESS (source) &&
      self->state == MB_SESSION_STATE_DISPLAY_STARTING)
    {
      g_warning ("MbSession: evdi-virtual-display exited early");
      g_clear_object (&self->helper);
      g_clear_handle_id (&self->startup_id, g_source_remove);
      set_state (self, MB_SESSION_STATE_ERROR,
                 "evdi-virtual-display exited early; see the log in XDG_RUNTIME_DIR. "
                 "Starting again too soon after a stop can do this; wait ~15s.");
    }

  g_object_unref (self);
}

static void position_output (MbSession *self);
static gboolean geom_watch_cb (gpointer user_data);

static void
helper_stop_sync (GSubprocess *helper)
{
  /* A helper parked in drm_read() will not act on SIGTERM; escalate rather
   * than leave it holding the evdi device, which blocks the next start.
   * The escalation is unconditional after a short grace because process
   * aliveness cannot be checked here without a main loop iteration (the
   * quit path has none left); force_exit on an already-dead child is
   * harmless. */
  g_subprocess_send_signal (helper, SIGTERM);
  g_usleep (1500 * 1000);
  g_subprocess_force_exit (helper);
}

static gboolean
startup_tick_cb (gpointer user_data)
{
  MbSession *self = MB_SESSION (user_data);
  g_autofree gchar *listing = NULL;
  g_autofree gchar *out = NULL;

  self->startup_ticks++;

  /* Guard phase: xfsettingsd applies its display profile a second or two
   * after outputs change and has been seen switching the primary panel off,
   * leaving the virtual display as the only monitor. Watch for a while and
   * undo it; never leave the user staring at a blank laptop. */
  if (self->guard_ticks >= 0)
    {
      listing = xrandr_current ();
      if (listing && !parse_geom (listing, self->primary, NULL, NULL, NULL, NULL))
        {
          guint w, h;
          g_autofree gchar *mode = NULL;
          g_autofree gchar *pos = NULL;

          g_warning ("MbSession: %s was switched off by a display profile; re-enabling",
                     self->primary);
          mb_settings_get_resolution (self->settings, &w, &h);
          mode = g_strdup_printf ("%ux%u", w, h);
          {
            guint pw = 0, ph = 0;
            gint px = 0, py = 0;
            parse_geom (listing, self->output, &pw, &ph, &px, &py);
            pos = g_strdup_printf ("%ux0", pw ? px : 1920);
          }
          {
            const gchar *argv[] = { "xrandr",
                                    "--output", self->primary, "--auto", "--primary", "--pos", "0x0",
                                    "--output", self->output, "--mode", mode, "--pos", pos,
                                    NULL };
            g_free (run_argv (argv));
          }
        }

      if (--self->guard_ticks < 0)
        {
          /* Guard over: latch the geometry and go ready. */
          g_autofree gchar *fresh = xrandr_current ();

          if (!fresh ||
              !parse_geom (fresh, self->output, &self->rw, &self->rh, &self->rx, &self->ry))
            {
              set_state (self, MB_SESSION_STATE_ERROR,
                         "virtual display did not report geometry");
              self->startup_id = 0;
              return G_SOURCE_REMOVE;
            }

          g_message ("MbSession: virtual display %s %ux%u at +%d+%d",
                     self->output, self->rw, self->rh, self->rx, self->ry);
          set_state (self, MB_SESSION_STATE_DISPLAY_READY, NULL);
          if (self->geom_id == 0)
            self->geom_id = g_timeout_add (GEOM_WATCH_MS, geom_watch_cb, self);
          self->startup_id = 0;
          connect_pending (self);
          return G_SOURCE_REMOVE;
        }
      return G_SOURCE_CONTINUE;
    }

  if (!self->helper)
    {
      self->startup_id = 0;
      return G_SOURCE_REMOVE;
    }

  if (self->startup_ticks > STARTUP_MAX_TICKS)
    {
      set_state (self, MB_SESSION_STATE_ERROR, "evdi output never came up");
      mb_session_display_stop (self);
      return G_SOURCE_REMOVE;
    }

  /* evdi's hotplug reaches X on its own (~8s, measured); a probing query is
   * only insurance for a server that never gets the event, held back so it
   * never fires in the normal case (each probe blocks X ~0.8s). */
  if (self->startup_ticks > STARTUP_PROBE_AFTER && self->startup_ticks % 20 == 0)
    {
      const gchar *argv[] = { "xrandr", "--query", NULL };
      g_autoptr(GSubprocess) probe =
        g_subprocess_newv (argv, G_SUBPROCESS_FLAGS_STDOUT_SILENCE |
                           G_SUBPROCESS_FLAGS_STDERR_SILENCE, NULL);
      (void) probe;
    }

  listing = xrandr_current ();
  if (!listing)
    return G_SOURCE_CONTINUE;

  out = find_evdi_output (listing);
  if (!out)
    return G_SOURCE_CONTINUE;

  {
    g_autofree gchar *pattern = NULL;
    g_autofree gchar *escaped = g_regex_escape_string (out, -1);
    g_autoptr(GRegex) re = NULL;

    pattern = g_strdup_printf ("^%s connected", escaped);
    re = g_regex_new (pattern, G_REGEX_MULTILINE, 0, NULL);
    if (!re || !g_regex_match (re, listing, 0, NULL))
      return G_SOURCE_CONTINUE;
  }

  g_free (self->output);
  self->output = g_steal_pointer (&out);
  position_output (self);
  self->guard_ticks = GUARD_TICKS;
  return G_SOURCE_CONTINUE;
}

static void
position_output (MbSession *self)
{
  guint w, h;
  g_autofree gchar *mode = NULL;
  g_autofree gchar *result = NULL;

  mb_settings_get_resolution (self->settings, &w, &h);
  mode = g_strdup_printf ("%ux%u", w, h);

  {
    const gchar *argv[] = { "xrandr",
                            "--output", self->output, "--mode", mode,
                            "--right-of", self->primary, NULL };
    result = run_argv (argv);
  }
}

void
mb_session_display_start (MbSession *self)
{
  g_autofree gchar *helper_path = NULL;
  g_autofree gchar *listing = NULL;
  g_autofree gchar *log_path = NULL;
  g_autoptr(GError) error = NULL;
  g_autoptr(GSubprocessLauncher) launcher = NULL;
  guint w, h;

  if (self->state != MB_SESSION_STATE_IDLE && self->state != MB_SESSION_STATE_ERROR)
    return;

  /* An ERROR from a failed connect leaves the display itself perfectly
   * healthy; recover the state instead of double-spawning the helper (whose
   * lock the second instance would trip over anyway). */
  if (self->helper && self->output)
    {
      g_message ("MbSession: display %s already up, recovering from error state",
                 self->output);
      set_state (self, MB_SESSION_STATE_DISPLAY_READY, NULL);
      connect_pending (self);
      return;
    }

  if (!mb_settings_get_resolution (self->settings, &w, &h))
    {
      set_state (self, MB_SESSION_STATE_ERROR, "resolution must be WxH (64..4094)");
      return;
    }

  helper_path = resolve_helper (self);
  if (!helper_path)
    {
      set_state (self, MB_SESSION_STATE_ERROR,
                 "evdi-virtual-display helper not found; set its path in Settings");
      return;
    }

  listing = xrandr_current ();
  {
    const gchar *configured = mb_settings_get_primary (self->settings);

    g_free (self->primary);
    if (configured && *configured)
      self->primary = g_strdup (configured);
    else
      self->primary = listing ? detect_primary (listing) : NULL;
  }
  if (!self->primary)
    {
      set_state (self, MB_SESSION_STATE_ERROR, "no connected output found to extend from");
      return;
    }

  mb_settings_apply_environment (self->settings);

  log_path = g_build_filename (g_get_user_runtime_dir (), "mirablast-evdi.log", NULL);
  launcher = g_subprocess_launcher_new (G_SUBPROCESS_FLAGS_NONE);
  g_subprocess_launcher_set_stdout_file_path (launcher, log_path);
  g_subprocess_launcher_set_stderr_file_path (launcher, log_path);

  {
    g_autofree gchar *mode = g_strdup_printf ("%ux%u", w, h);
    g_autofree gchar *refresh = g_strdup_printf ("%u", mb_settings_get_refresh (self->settings));

    self->helper = g_subprocess_launcher_spawn (launcher, &error,
                                                helper_path, mode,
                                                "--refresh", refresh, NULL);
  }

  if (!self->helper)
    {
      set_state (self, MB_SESSION_STATE_ERROR, error->message);
      return;
    }

  g_subprocess_wait_async (self->helper, self->cancellable,
                           helper_exited_cb, g_object_ref (self));

  self->startup_ticks = 0;
  self->guard_ticks = -1;
  set_state (self, MB_SESSION_STATE_DISPLAY_STARTING, NULL);
  g_message ("MbSession: waiting for the evdi output (evdi_open takes ~8-12s)");
  self->startup_id = g_timeout_add (STARTUP_TICK_MS, startup_tick_cb, self);
}

void
mb_session_display_stop (MbSession *self)
{
  mb_session_disconnect (self);
  g_clear_object (&self->pending_sink);

  g_clear_handle_id (&self->startup_id, g_source_remove);
  g_clear_handle_id (&self->geom_id, g_source_remove);

  if (self->output && self->primary)
    {
      /* Enable the panel in the same call that drops the virtual output, so
       * the session is never left with no active monitor. */
      const gchar *argv[] = { "xrandr",
                              "--output", self->primary, "--auto", "--primary", "--pos", "0x0",
                              "--output", self->output, "--off", NULL };
      g_free (run_argv (argv));
    }

  if (self->helper)
    {
      helper_stop_sync (self->helper);
      g_clear_object (&self->helper);
    }

  g_clear_pointer (&self->output, g_free);
  set_state (self, MB_SESSION_STATE_IDLE, NULL);
}

/* ------------------------------------------------------------------ */
/* geometry watch + reconnect                                          */

static void mb_session_request_reconnect (MbSession *self);

static gboolean
geom_watch_cb (gpointer user_data)
{
  MbSession *self = MB_SESSION (user_data);
  g_autofree gchar *listing = NULL;
  guint w, h;
  gint x, y;

  if (!self->output)
    return G_SOURCE_CONTINUE;

  listing = xrandr_current ();
  if (!listing ||
      !parse_geom (listing, self->output, &w, &h, &x, &y))
    return G_SOURCE_CONTINUE;   /* momentarily off; keep the last region */

  if (w != self->rw || h != self->rh || x != self->rx || y != self->ry)
    {
      g_message ("MbSession: virtual display moved to %ux%u+%d+%d", w, h, x, y);
      self->rw = w; self->rh = h; self->rx = x; self->ry = y;
    }

  if (!self->capture_src)
    return G_SOURCE_CONTINUE;

  {
    guint cur_sx = 0, cur_sy = 0, cur_ex = 0, cur_ey = 0;
    guint cur_w, cur_h;

    g_object_get (self->capture_src,
                  "startx", &cur_sx, "starty", &cur_sy,
                  "endx", &cur_ex, "endy", &cur_ey, NULL);
    cur_w = cur_ex - cur_sx + 1;
    cur_h = cur_ey - cur_sy + 1;

    /* The capture size was fixed when caps were negotiated; a region of a
     * different size (rotation) cannot be applied to a running stream. */
    if (self->rw != self->exp_w || self->rh != self->exp_h)
      {
        g_autofree gchar *region =
          g_strdup_printf ("%ux%u+%d+%d", self->rw, self->rh, self->rx, self->ry);

        if (g_strcmp0 (self->warned_region, region) != 0)
          {
            g_warning ("MbSession: cannot resize a running capture (%ux%u -> %ux%u); "
                       "reconnect to pick up the new size",
                       self->exp_w, self->exp_h, self->rw, self->rh);
            g_free (self->warned_region);
            self->warned_region = g_steal_pointer (&region);
          }
        return G_SOURCE_CONTINUE;
      }

    if (cur_w != self->exp_w || cur_h != self->exp_h)
      {
        /* ximagesrc silently resets its crop to the full screen whenever an
         * X screen resize leaves the old crop overshooting the root window
         * (gst_ximage_src_get_caps). Restore it -- and reconnect, because the
         * intervideo bridge downstream saw the caps change and does not
         * recover on its own. */
        g_message ("MbSession: capture was reset to %ux%u+%u+%u (X screen resize); "
                   "restoring %ux%u+%d+%d",
                   cur_w, cur_h, cur_sx, cur_sy,
                   self->rw, self->rh, self->rx, self->ry);
        g_object_set (self->capture_src,
                      "startx", (guint) self->rx,
                      "starty", (guint) self->ry,
                      "endx", (guint) (self->rx + self->rw - 1),
                      "endy", (guint) (self->ry + self->rh - 1), NULL);
        mb_session_request_reconnect (self);
        return G_SOURCE_CONTINUE;
      }

    if (cur_sx != (guint) self->rx || cur_sy != (guint) self->ry)
      {
        g_message ("MbSession: moving capture to +%d+%d", self->rx, self->ry);
        g_object_set (self->capture_src,
                      "startx", (guint) self->rx,
                      "starty", (guint) self->ry,
                      "endx", (guint) (self->rx + self->rw - 1),
                      "endy", (guint) (self->ry + self->rh - 1), NULL);
      }
  }

  return G_SOURCE_CONTINUE;
}

static gboolean
sink_is_connectable (NdSink *sink)
{
  /* A discovered sink is an NdMetaSink wrapping whichever provider currently
   * offers it; after a teardown it has no backing sink until the peer is
   * re-discovered, and starting a stream in that state asserts. */
  if (ND_IS_META_SINK (sink))
    return nd_meta_sink_get_sink (ND_META_SINK (sink)) != NULL;
  return TRUE;
}

static gboolean
reconnect_tick_cb (gpointer user_data)
{
  MbSession *self = MB_SESSION (user_data);

  if (self->stream_sink)
    {
      NdSinkState state;

      g_object_get (self->stream_sink, "state", &state, NULL);

      /* A failed attempt parks the sink in ERROR; drop it so it can reach
       * DISCONNECTED and be started again. */
      if (state == ND_SINK_STATE_ERROR)
        {
          nd_sink_stop_stream (self->stream_sink);
          return G_SOURCE_CONTINUE;
        }

      /* Streaming again: nothing left to reconnect. Retiring here matters --
       * this timer would otherwise spin for the rest of the session and its
       * non-zero id makes every later reconnect request bail out silently. */
      if (state == ND_SINK_STATE_STREAMING)
        {
          self->reconnect_id = 0;
          return G_SOURCE_REMOVE;
        }

      return G_SOURCE_CONTINUE;
    }

  /* The sink reports DISCONNECTED before the P2P group has finished going
   * away, and starting a new stream into that fails; let it settle. */
  if (self->reconnect_wait > 0)
    {
      self->reconnect_wait--;
      return G_SOURCE_CONTINUE;
    }

  if (!self->connect_sink)
    {
      self->reconnect_id = 0;
      return G_SOURCE_REMOVE;
    }

  if (!sink_is_connectable (self->connect_sink))
    {
      if (self->reconnect_deadline > 0)
        {
          self->reconnect_deadline--;
          return G_SOURCE_CONTINUE;
        }
      g_warning ("MbSession: sink was not re-discovered, giving up; "
                 "connect it again from the list");
      self->reconnect_id = 0;
      set_state (self, MB_SESSION_STATE_ERROR, "sink was not re-discovered");
      return G_SOURCE_REMOVE;
    }

  self->reconnect_id = 0;
  g_message ("MbSession: re-connecting after the capture was lost");
  mb_session_do_connect (self, self->connect_sink);
  return G_SOURCE_REMOVE;
}

static void
mb_session_request_reconnect (MbSession *self)
{
  g_autofree gchar *region = NULL;

  if (!mb_settings_get_auto_reconnect (self->settings))
    {
      g_message ("MbSession: auto-reconnect is disabled; the picture may stay broken");
      return;
    }

  if (self->reconnect_id != 0)
    {
      g_message ("MbSession: re-connect already in progress, skipping");
      return;
    }

  if (!self->connect_sink || !self->stream_sink)
    return;

  region = g_strdup_printf ("%ux%u+%d+%d", self->rw, self->rh, self->rx, self->ry);

  /* Never cycle the session in a tight loop -- but a genuinely new position
   * is a real move and deserves its reconnect even if the last one was
   * recent. Only repeats for the same region are suppressed. */
  if (self->reconnect_last != 0 &&
      g_get_monotonic_time () - self->reconnect_last < RECONNECT_RATE_S * G_TIME_SPAN_SECOND &&
      g_strcmp0 (region, self->reconnect_last_region) == 0)
    {
      g_warning ("MbSession: capture lost again within a minute for the same "
                 "region; leaving the session alone");
      return;
    }

  self->reconnect_last = g_get_monotonic_time ();
  g_free (self->reconnect_last_region);
  self->reconnect_last_region = g_steal_pointer (&region);

  g_message ("MbSession: capture lost to an X screen resize, re-connecting");
  self->reconnect_tries = RECONNECT_TRIES;
  self->reconnect_wait = RECONNECT_SETTLE_S;
  self->reconnect_deadline = RECONNECT_DEADLINE_S;
  nd_sink_stop_stream (self->stream_sink);
  self->reconnect_id = g_timeout_add_seconds (1, reconnect_tick_cb, self);
}

/* ------------------------------------------------------------------ */
/* streaming                                                           */

static GstElement *
sink_create_source_cb (MbSession *self, NdSink *sink)
{
  GstBin *bin;
  GstElement *src, *scale, *filter, *dst, *res;
  g_autoptr(GstCaps) caps = NULL;

  bin = GST_BIN (gst_bin_new ("mirablast source bin"));

  src = gst_element_factory_make ("ximagesrc", "mirablast X11 source");
  if (!src)
    g_error ("MbSession: ximagesrc missing, cannot capture");

  g_object_set (src,
                "startx", (guint) self->rx,
                "starty", (guint) self->ry,
                "endx", (guint) (self->rx + self->rw - 1),
                "endy", (guint) (self->ry + self->rh - 1),
                NULL);

  /* XDamage stays at its default (on): forcing it off made the encoded
   * stream flicker and smear the cursor on a real sink. The switch exists
   * for experiments only. */
  if (!mb_settings_get_use_damage (self->settings))
    {
      g_warning ("MbSession: XDamage disabled by settings; expect flicker");
      g_object_set (src, "use-damage", FALSE, NULL);
    }

  self->exp_w = self->rw;
  self->exp_h = self->rh;
  g_clear_pointer (&self->warned_region, g_free);

  /* Pin what crosses the intervideosink/intervideosrc bridge to the region
   * size. That pair negotiates caps once; letting an ximagesrc crop reset
   * change them wedges it -- the stream keeps flowing while the sink shows
   * black for the rest of the session. */
  scale = gst_element_factory_make ("videoscale", "mirablast region scale");
  filter = gst_element_factory_make ("capsfilter", "mirablast region filter");
  if (!scale || !filter)
    g_error ("MbSession: videoscale/capsfilter missing");

  caps = gst_caps_new_simple ("video/x-raw",
                              "width", G_TYPE_INT, (gint) self->rw,
                              "height", G_TYPE_INT, (gint) self->rh,
                              NULL);
  g_object_set (filter, "caps", caps, NULL);

  dst = gst_element_factory_make ("intervideosink", "mirablast inter sink");
  if (!dst)
    g_error ("MbSession: intervideosink missing");
  g_object_set (dst,
                "channel", "mirablast-inter-video",
                "max-lateness", (gint64) -1,
                "sync", FALSE,
                NULL);

  gst_bin_add_many (bin, src, scale, filter, dst, NULL);
  gst_element_link_many (src, scale, filter, dst, NULL);

  res = gst_element_factory_make ("intervideosrc", "mirablast screencast src");
  g_object_set (res,
                "do-timestamp", FALSE,
                "timeout", (guint64) G_MAXUINT64,
                "channel", "mirablast-inter-video",
                NULL);
  gst_bin_add (bin, res);

  gst_element_add_pad (GST_ELEMENT (bin),
                       gst_ghost_pad_new ("src",
                                          gst_element_get_static_pad (res, "src")));

  if (self->capture_src)
    g_object_remove_weak_pointer (G_OBJECT (self->capture_src),
                                  (gpointer *) &self->capture_src);
  self->capture_src = src;
  g_object_add_weak_pointer (G_OBJECT (src), (gpointer *) &self->capture_src);

  g_message ("MbSession: capturing %ux%u+%d+%d, bridge pinned to %ux%u",
             self->rw, self->rh, self->rx, self->ry, self->rw, self->rh);

  g_object_ref_sink (bin);
  return GST_ELEMENT (bin);
}

static GstElement *
sink_create_audio_source_cb (MbSession *self, NdSink *sink)
{
  GstElement *res;

  if (!self->pulse)
    return NULL;

  res = nd_pulseaudio_get_source (self->pulse);
  return g_object_ref_sink (res);
}

static void
sink_notify_state_cb (MbSession *self, GParamSpec *pspec, NdSink *sink)
{
  NdSinkState state;

  g_object_get (sink, "state", &state, NULL);
  g_message ("MbSession: sink state now 0x%x", state);

  switch (state)
    {
    case ND_SINK_STATE_ENSURE_FIREWALL:
    case ND_SINK_STATE_WAIT_P2P:
    case ND_SINK_STATE_WAIT_SOCKET:
    case ND_SINK_STATE_WAIT_STREAMING:
      set_state (self, MB_SESSION_STATE_CONNECTING, NULL);
      break;

    case ND_SINK_STATE_STREAMING:
      /* The retry budget has done its job. Clearing it here rather than at
       * connect time matters: a reconnect's own connect only fails after the
       * fact, and zeroing earlier left nothing to retry with. */
      self->reconnect_tries = 0;
      set_state (self, MB_SESSION_STATE_STREAMING, NULL);
      break;

    case ND_SINK_STATE_ERROR:
      if (self->reconnect_tries > 0 && self->reconnect_id == 0)
        {
          self->reconnect_tries--;
          self->reconnect_wait = RECONNECT_SETTLE_S;
          g_message ("MbSession: re-connect attempt failed, %u left",
                     self->reconnect_tries);
          nd_sink_stop_stream (self->stream_sink);
          self->reconnect_id = g_timeout_add_seconds (1, reconnect_tick_cb, self);
        }
      else if (self->reconnect_id == 0)
        {
          g_object_set (self->meta_provider, "discover", TRUE, NULL);
          set_state (self, MB_SESSION_STATE_ERROR, "streaming failed");
        }
      break;

    case ND_SINK_STATE_DISCONNECTED:
      g_signal_handlers_disconnect_by_data (sink, self);
      g_clear_object (&self->stream_sink);
      g_object_set (self->meta_provider, "discover", TRUE, NULL);

      if (self->reconnect_id != 0)
        set_state (self, MB_SESSION_STATE_CONNECTING, NULL);
      else if (self->state != MB_SESSION_STATE_ERROR)
        set_state (self,
                   self->output ? MB_SESSION_STATE_DISPLAY_READY
                                : MB_SESSION_STATE_IDLE, NULL);
      break;

    default:
      break;
    }
}

static void
mb_session_do_connect (MbSession *self, NdSink *sink)
{
  if (!sink_is_connectable (sink))
    {
      set_state (self, MB_SESSION_STATE_ERROR, "sink is not currently reachable");
      return;
    }

  mb_settings_apply_environment (self->settings);

  /* Stop discovery before negotiating: a P2P-FIND in flight keeps the radio
   * scanning and group formation times out (supplicant-timeout). gnd does
   * this at the same point. Discovery resumes on DISCONNECTED. */
  g_object_set (self->meta_provider, "discover", FALSE, NULL);

  self->stream_sink = nd_sink_start_stream (sink);
  if (!self->stream_sink)
    {
      set_state (self, MB_SESSION_STATE_ERROR, "could not start streaming");
      return;
    }

  g_signal_connect_object (self->stream_sink, "create-source",
                           (GCallback) sink_create_source_cb, self,
                           G_CONNECT_SWAPPED);
  g_signal_connect_object (self->stream_sink, "create-audio-source",
                           (GCallback) sink_create_audio_source_cb, self,
                           G_CONNECT_SWAPPED);
  g_signal_connect_object (self->stream_sink, "notify::state",
                           (GCallback) sink_notify_state_cb, self,
                           G_CONNECT_SWAPPED);

  set_state (self, MB_SESSION_STATE_CONNECTING, NULL);
  /* We might already be in an error state. */
  sink_notify_state_cb (self, NULL, self->stream_sink);
}

/* Picked from the tray before the display existed: the connect was deferred
 * to whichever code path brings the output up. */
static void
connect_pending (MbSession *self)
{
  g_autoptr(NdSink) sink = g_steal_pointer (&self->pending_sink);

  if (!sink || self->stream_sink || !self->output)
    return;

  g_message ("MbSession: display is ready, connecting to the queued sink");
  mb_session_connect_sink (self, sink);
}

void
mb_session_connect_sink (MbSession *self, NdSink *sink)
{
  if (self->stream_sink)
    {
      g_warning ("MbSession: already streaming; disconnect first");
      return;
    }

  /* The sink list is reachable from the tray whether or not the display is
   * running, so a pick doubles as "start the display, then cast to this".
   * Nothing else may be queued: the pick is dropped on error or on stop. */
  if (!self->output)
    {
      g_set_object (&self->pending_sink, sink);

      if (self->state == MB_SESSION_STATE_IDLE ||
          self->state == MB_SESSION_STATE_ERROR)
        mb_session_display_start (self);
      else
        g_message ("MbSession: display still starting, queueing the connect");
      return;
    }

  g_set_object (&self->connect_sink, sink);
  self->reconnect_last = 0;
  g_clear_pointer (&self->reconnect_last_region, g_free);
  mb_session_do_connect (self, sink);
}

void
mb_session_disconnect (MbSession *self)
{
  g_clear_handle_id (&self->reconnect_id, g_source_remove);
  self->reconnect_tries = 0;

  if (self->stream_sink)
    nd_sink_stop_stream (self->stream_sink);
}

/* ------------------------------------------------------------------ */
/* boilerplate                                                         */

NdMetaProvider *
mb_session_get_provider (MbSession *self)
{
  return self->meta_provider;
}

MbSessionState
mb_session_get_state (MbSession *self)
{
  return self->state;
}

gchar *
mb_session_dup_status (MbSession *self)
{
  switch (self->state)
    {
    case MB_SESSION_STATE_IDLE:
      return g_strdup ("Idle");
    case MB_SESSION_STATE_DISPLAY_STARTING:
      if (self->pending_sink)
        {
          g_autofree gchar *name = NULL;

          g_object_get (self->pending_sink, "display-name", &name, NULL);
          return g_strdup_printf ("Starting virtual display, then casting to %s…",
                                  name ? name : "sink");
        }
      return g_strdup ("Starting virtual display…");
    case MB_SESSION_STATE_DISPLAY_READY:
      return g_strdup_printf ("Display %s ready — not casting",
                              self->output ? self->output : "?");
    case MB_SESSION_STATE_CONNECTING:
      return g_strdup ("Connecting…");
    case MB_SESSION_STATE_STREAMING:
      {
        g_autofree gchar *name = NULL;
        if (self->connect_sink)
          g_object_get (self->connect_sink, "display-name", &name, NULL);
        return g_strdup_printf ("Casting to %s", name ? name : "sink");
      }
    case MB_SESSION_STATE_ERROR:
      return g_strdup_printf ("Error: %s",
                              self->error_message ? self->error_message : "unknown");
    default:
      return g_strdup ("?");
    }
}

static void
pulse_init_cb (GObject *source, GAsyncResult *res, gpointer user_data)
{
  MbSession *self = MB_SESSION (user_data);
  g_autoptr(GError) error = NULL;

  if (!g_async_initable_init_finish (G_ASYNC_INITABLE (source), res, &error))
    {
      if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        g_warning ("MbSession: pulseaudio init failed (audio disabled): %s",
                   error->message);
      g_object_unref (source);
      g_object_unref (self);
      return;
    }

  self->pulse = ND_PULSEAUDIO (source);
  g_object_unref (self);
}

static void
mb_session_dispose (GObject *object)
{
  MbSession *self = MB_SESSION (object);

  g_cancellable_cancel (self->cancellable);
  mb_session_display_stop (self);

  if (self->capture_src)
    {
      g_object_remove_weak_pointer (G_OBJECT (self->capture_src),
                                    (gpointer *) &self->capture_src);
      self->capture_src = NULL;
    }

  g_clear_object (&self->connect_sink);
  g_clear_object (&self->pending_sink);
  g_clear_object (&self->nm_registry);
  g_clear_object (&self->dummy_provider);
  g_clear_object (&self->meta_provider);
  g_clear_object (&self->pulse);
  g_clear_object (&self->cancellable);
  g_clear_object (&self->settings);

  G_OBJECT_CLASS (mb_session_parent_class)->dispose (object);
}

static void
mb_session_finalize (GObject *object)
{
  MbSession *self = MB_SESSION (object);

  g_free (self->output);
  g_free (self->primary);
  g_free (self->error_message);
  g_free (self->warned_region);
  g_free (self->reconnect_last_region);

  G_OBJECT_CLASS (mb_session_parent_class)->finalize (object);
}

static void
mb_session_class_init (MbSessionClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = mb_session_dispose;
  object_class->finalize = mb_session_finalize;

  signals[STATE_CHANGED] = g_signal_new ("state-changed",
                                         MB_TYPE_SESSION,
                                         G_SIGNAL_RUN_LAST,
                                         0, NULL, NULL, NULL,
                                         G_TYPE_NONE, 0);
}

static void
mb_session_init (MbSession *self)
{
  self->state = MB_SESSION_STATE_IDLE;
  self->guard_ticks = -1;
  self->cancellable = g_cancellable_new ();
}

MbSession *
mb_session_new (MbSettings *settings, gboolean with_dummy)
{
  MbSession *self = g_object_new (MB_TYPE_SESSION, NULL);
  NdPulseaudio *pulse;

  self->settings = g_object_ref (settings);

  self->meta_provider = nd_meta_provider_new ();
  g_object_set (self->meta_provider, "discover", TRUE, NULL);
  self->nm_registry = nd_nm_device_registry_new (self->meta_provider);

  if (with_dummy)
    {
      self->dummy_provider = nd_dummy_provider_new ();
      nd_meta_provider_add_provider (self->meta_provider,
                                     ND_PROVIDER (self->dummy_provider));
    }

  pulse = nd_pulseaudio_new ("Mirablast", "mirablast");
  g_async_initable_init_async (G_ASYNC_INITABLE (pulse),
                               G_PRIORITY_LOW,
                               self->cancellable,
                               pulse_init_cb,
                               g_object_ref (self));

  return self;
}
