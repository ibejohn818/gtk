/*
 * gtkimcontextime.c
 * Copyright (C) 2003 Takuro Ashie
 * Copyright (C) 2003-2004 Kazuki IWAMOTO
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library. If not, see <http://www.gnu.org/licenses/>.
 *
 */

/*
 *  Please see the following site for the detail of Windows IME API.
 *  http://msdn.microsoft.com/library/default.asp?url=/library/en-us/appendix/hh/appendix/imeimes2_35ph.asp
 */

#ifdef GTK_DISABLE_DEPRECATED
#undef GTK_DISABLE_DEPRECATED
#endif

#include "gtkimcontextime.h"
#include "gtkimmoduleprivate.h"
#include "gtkroot.h"

#include "imm-extra.h"

#include "gdk/gdkkeysyms.h"
#include "gdk/win32/gdkwin32.h"
#include "gtk/gtkimmodule.h"
#include "gtk/gtkstylecontextprivate.h"
#include "gtk/gtkcssstyleprivate.h"

/* avoid warning */
#ifdef STRICT
# undef STRICT
# include <pango/pangowin32.h>
# ifndef STRICT
#   define STRICT 1
# endif
#else /* STRICT */
#   include <pango/pangowin32.h>
#endif /* STRICT */

/* Determines what happens when focus is lost while preedit is in process. */
typedef enum {
  /* Preedit is committed. */
  GTK_WIN32_IME_FOCUS_BEHAVIOR_COMMIT,
  /* Preedit is discarded. */
  GTK_WIN32_IME_FOCUS_BEHAVIOR_DISCARD,
  /* Preedit follows the cursor (that means it will appear in the widget
   * that receives the focus) */
  GTK_WIN32_IME_FOCUS_BEHAVIOR_FOLLOW,
} GtkWin32IMEFocusBehavior;

#define IS_DEAD_KEY(k) \
    ((k) >= GDK_KEY_dead_grave && (k) <= (GDK_KEY_dead_dasia+1))

struct _GtkIMContextIMEPrivate
{
  /* When pretend_empty_preedit is set to TRUE,
   * gtk_im_context_ime_get_preedit_string() will return an empty string
   * instead of the actual content of ImmGetCompositionStringW().
   *
   * This is necessary because GtkEntry expects the preedit buffer to be
   * cleared before commit() is called, otherwise it leads to an assertion
   * failure in Pango. However, since we emit the commit() signal while
   * handling the WM_IME_COMPOSITION message, the IME buffer will be non-empty,
   * so we temporarily set this flag while emmiting the appropriate signals.
   *
   * See also:
   *   https://bugzilla.gnome.org/show_bug.cgi?id=787142
   *   https://gitlab.gnome.org/GNOME/gtk/commit/c255ba68fc2c918dd84da48a472e7973d3c00b03
   */
  gboolean pretend_empty_preedit;
  guint32 dead_key_keyval;
  GtkWin32IMEFocusBehavior focus_behavior;
};


/* GObject class methods */
static void gtk_im_context_ime_dispose    (GObject              *obj);
static void gtk_im_context_ime_finalize   (GObject              *obj);

static void gtk_im_context_ime_set_property (GObject      *object,
                                             guint         prop_id,
                                             const GValue *value,
                                             GParamSpec   *pspec);
static void gtk_im_context_ime_get_property (GObject      *object,
                                             guint         prop_id,
                                             GValue       *value,
                                             GParamSpec   *pspec);

/* GtkIMContext's virtual functions */
static void gtk_im_context_ime_set_client_widget   (GtkIMContext *context,
                                                    GtkWidget    *widget);
static gboolean gtk_im_context_ime_filter_keypress (GtkIMContext   *context,
                                                    GdkEvent       *event);
static void gtk_im_context_ime_reset               (GtkIMContext   *context);
static void gtk_im_context_ime_get_preedit_string  (GtkIMContext   *context,
                                                    char          **str,
                                                    PangoAttrList **attrs,
                                                    int            *cursor_pos);
static void gtk_im_context_ime_focus_in            (GtkIMContext   *context);
static void gtk_im_context_ime_focus_out           (GtkIMContext   *context);
static void gtk_im_context_ime_set_cursor_location (GtkIMContext   *context,
                                                    GdkRectangle   *area);
static void gtk_im_context_ime_set_use_preedit     (GtkIMContext   *context,
                                                    gboolean        use_preedit);

/* GtkIMContextIME's private functions */
static void gtk_im_context_ime_set_preedit_font (GtkIMContext    *context);

static GdkWin32MessageFilterReturn
gtk_im_context_ime_message_filter               (GdkWin32Display *display,
                                                 MSG             *msg,
                                                 int             *ret_valp,
                                                 gpointer         data);
static void get_window_position                 (GdkSurface       *win,
                                                 int             *x,
                                                 int             *y);

G_DEFINE_TYPE_WITH_CODE (GtkIMContextIME, gtk_im_context_ime, GTK_TYPE_IM_CONTEXT,
			 gtk_im_module_ensure_extension_point ();
                         g_io_extension_point_implement (GTK_IM_MODULE_EXTENSION_POINT_NAME,
                                                         g_define_type_id,
                                                         "ime",
                                                         0))

static void
gtk_im_context_ime_class_init (GtkIMContextIMEClass *class)
{
  GtkIMContextClass *im_context_class = GTK_IM_CONTEXT_CLASS (class);
  GObjectClass *gobject_class = G_OBJECT_CLASS (class);

  gobject_class->finalize     = gtk_im_context_ime_finalize;
  gobject_class->dispose      = gtk_im_context_ime_dispose;
  gobject_class->set_property = gtk_im_context_ime_set_property;
  gobject_class->get_property = gtk_im_context_ime_get_property;

  im_context_class->set_client_widget   = gtk_im_context_ime_set_client_widget;
  im_context_class->filter_keypress     = gtk_im_context_ime_filter_keypress;
  im_context_class->reset               = gtk_im_context_ime_reset;
  im_context_class->get_preedit_string  = gtk_im_context_ime_get_preedit_string;
  im_context_class->focus_in            = gtk_im_context_ime_focus_in;
  im_context_class->focus_out           = gtk_im_context_ime_focus_out;
  im_context_class->set_cursor_location = gtk_im_context_ime_set_cursor_location;
  im_context_class->set_use_preedit     = gtk_im_context_ime_set_use_preedit;
}

static void
gtk_im_context_ime_init (GtkIMContextIME *context_ime)
{
  context_ime->client_surface         = NULL;
  context_ime->use_preedit            = TRUE;
  context_ime->preediting             = FALSE;
  context_ime->opened                 = FALSE;
  context_ime->focus                  = FALSE;
  context_ime->cursor_location.x      = 0;
  context_ime->cursor_location.y      = 0;
  context_ime->cursor_location.width  = 0;
  context_ime->cursor_location.height = 0;
  context_ime->commit_string          = NULL;

  context_ime->priv = g_malloc0 (sizeof (GtkIMContextIMEPrivate));
  context_ime->priv->focus_behavior = GTK_WIN32_IME_FOCUS_BEHAVIOR_COMMIT;
}


static void
gtk_im_context_ime_dispose (GObject *obj)
{
  GtkIMContext *context = GTK_IM_CONTEXT (obj);
  GtkIMContextIME *context_ime = GTK_IM_CONTEXT_IME (obj);

  if (context_ime->client_surface)
    gtk_im_context_ime_set_client_widget (context, NULL);

  G_OBJECT_CLASS (gtk_im_context_ime_parent_class)->dispose (obj);
}


static void
gtk_im_context_ime_finalize (GObject *obj)
{
  GtkIMContextIME *context_ime = GTK_IM_CONTEXT_IME (obj);

  g_free (context_ime->priv);
  context_ime->priv = NULL;

  G_OBJECT_CLASS (gtk_im_context_ime_parent_class)->finalize (obj);
}


static void
gtk_im_context_ime_set_property (GObject      *object,
                                 guint         prop_id,
                                 const GValue *value,
                                 GParamSpec   *pspec)
{
  GtkIMContextIME *context_ime = GTK_IM_CONTEXT_IME (object);

  g_return_if_fail (GTK_IS_IM_CONTEXT_IME (context_ime));

  switch (prop_id)
    {
    default:
      break;
    }
}


static void
gtk_im_context_ime_get_property (GObject    *object,
                                 guint       prop_id,
                                 GValue     *value,
                                 GParamSpec *pspec)
{
  GtkIMContextIME *context_ime = GTK_IM_CONTEXT_IME (object);

  g_return_if_fail (GTK_IS_IM_CONTEXT_IME (context_ime));

  switch (prop_id)
    {
    default:
      break;
    }
}


GtkIMContext *
gtk_im_context_ime_new (void)
{
  return g_object_new (GTK_TYPE_IM_CONTEXT_IME, NULL);
}


static void
gtk_im_context_ime_set_client_widget (GtkIMContext *context,
                                      GtkWidget    *widget)
{
  GtkIMContextIME *context_ime;
  GdkSurface *client_surface = NULL;

  g_return_if_fail (GTK_IS_IM_CONTEXT_IME (context));
  context_ime = GTK_IM_CONTEXT_IME (context);

  if (widget)
    client_surface = gtk_native_get_surface (gtk_widget_get_native (widget));

  if (client_surface != NULL)
    {
      HWND hwnd = gdk_win32_surface_get_impl_hwnd (client_surface);
      HIMC himc = ImmGetContext (hwnd);
      if (himc)
        {
          context_ime->opened = ImmGetOpenStatus (himc);
          ImmReleaseContext (hwnd, himc);
        }
      else
        {
          context_ime->opened = FALSE;
        }
    }
  else if (context_ime->focus)
    {
      gtk_im_context_ime_focus_out (context);
    }

  context_ime->client_surface = client_surface;
}

static gunichar
_gtk_im_context_ime_dead_key_unichar (guint    keyval,
                                      gboolean spacing)
{
  switch (keyval)
    {
#define CASE(keysym, unicode, spacing_unicode) \
      case GDK_KEY_dead_##keysym: return (spacing) ? spacing_unicode : unicode;

      CASE (grave, 0x0300, 0x0060);
      CASE (acute, 0x0301, 0x00b4);
      CASE (circumflex, 0x0302, 0x005e);
      CASE (tilde, 0x0303, 0x007e);	/* Also used with perispomeni, 0x342. */
      CASE (macron, 0x0304, 0x00af);
      CASE (breve, 0x0306, 0x02d8);
      CASE (abovedot, 0x0307, 0x02d9);
      CASE (diaeresis, 0x0308, 0x00a8);
      CASE (hook, 0x0309, 0);
      CASE (abovering, 0x030A, 0x02da);
      CASE (doubleacute, 0x030B, 0x2dd);
      CASE (caron, 0x030C, 0x02c7);
      CASE (abovecomma, 0x0313, 0);         /* Equivalent to psili */
      CASE (abovereversedcomma, 0x0314, 0); /* Equivalent to dasia */
      CASE (horn, 0x031B, 0);	/* Legacy use for psili, 0x313 (or 0x343). */
      CASE (belowdot, 0x0323, 0);
      CASE (cedilla, 0x0327, 0x00b8);
      CASE (ogonek, 0x0328, 0);	/* Legacy use for dasia, 0x314.*/
      CASE (iota, 0x0345, 0);

#undef CASE
    default:
      return 0;
    }
}

static void
_gtk_im_context_ime_commit_unichar (GtkIMContextIME *context_ime,
                                    gunichar         c)
{
  char utf8[10];
  int len;

  if (context_ime->priv->dead_key_keyval != 0)
    {
      gunichar combining;

      combining =
        _gtk_im_context_ime_dead_key_unichar (context_ime->priv->dead_key_keyval,
                                              FALSE);
      g_unichar_compose (c, combining, &c);
    }

  len = g_unichar_to_utf8 (c, utf8);
  utf8[len] = 0;

  g_signal_emit_by_name (context_ime, "commit", utf8);
  context_ime->priv->dead_key_keyval = 0;
}

static gboolean
gtk_im_context_ime_filter_keypress (GtkIMContext *context,
                                    GdkEvent     *event)
{
  GtkIMContextIME *context_ime;
  gboolean retval = FALSE;
  guint32 c;
  GdkModifierType state, consumed_modifiers, no_text_input_mask;
  guint keyval;

  g_return_val_if_fail (GTK_IS_IM_CONTEXT_IME (context), FALSE);
  g_return_val_if_fail (event, FALSE);

  if (gdk_event_get_event_type ((GdkEvent *) event) == GDK_KEY_RELEASE)
    return FALSE;

  no_text_input_mask = GDK_ALT_MASK|GDK_CONTROL_MASK;

  state = gdk_event_get_modifier_state ((GdkEvent *) event);
  consumed_modifiers = gdk_key_event_get_consumed_modifiers (event);

  if (state & no_text_input_mask & ~consumed_modifiers)
    return FALSE;

  context_ime = GTK_IM_CONTEXT_IME (context);

  if (!context_ime->focus)
    return FALSE;

  if (!GDK_IS_SURFACE (context_ime->client_surface))
    return FALSE;

  keyval = gdk_key_event_get_keyval ((GdkEvent *) event);

  if (keyval == GDK_KEY_space &&
      context_ime->priv->dead_key_keyval != 0)
    {
      c = _gtk_im_context_ime_dead_key_unichar (context_ime->priv->dead_key_keyval, TRUE);
      context_ime->priv->dead_key_keyval = 0;
      _gtk_im_context_ime_commit_unichar (context_ime, c);
      return TRUE;
    }

  c = gdk_keyval_to_unicode (keyval);

  if (c && !g_unichar_iscntrl(c))
    {
      _gtk_im_context_ime_commit_unichar (context_ime, c);
      retval = TRUE;
    }
  else if (IS_DEAD_KEY (keyval))
    {
      gunichar dead_key;

      dead_key = _gtk_im_context_ime_dead_key_unichar (keyval, FALSE);

      /* Emulate double input of dead keys */
      if (dead_key && keyval == context_ime->priv->dead_key_keyval)
        {
          c = _gtk_im_context_ime_dead_key_unichar (context_ime->priv->dead_key_keyval, TRUE);
          context_ime->priv->dead_key_keyval = 0;
          _gtk_im_context_ime_commit_unichar (context_ime, c);
          _gtk_im_context_ime_commit_unichar (context_ime, c);
        }
      else
        context_ime->priv->dead_key_keyval = keyval;
    }

  return retval;
}


static void
gtk_im_context_ime_reset (GtkIMContext *context)
{
  GtkIMContextIME *context_ime = GTK_IM_CONTEXT_IME (context);
  HWND hwnd;
  HIMC himc;

  if (!context_ime->client_surface)
    return;

  hwnd = gdk_win32_surface_get_impl_hwnd (context_ime->client_surface);
  himc = ImmGetContext (hwnd);
  if (!himc)
    return;

  ImmNotifyIME (himc, NI_COMPOSITIONSTR, CPS_CANCEL, 0);

  if (context_ime->preediting)
    {
      context_ime->preediting = FALSE;
      g_signal_emit_by_name (context, "preedit-changed");
    }

  ImmReleaseContext (hwnd, himc);
}


static char *
get_utf8_preedit_string (GtkIMContextIME *context_ime,
                         int  kind,
                         int *pos_ret)
{
  gunichar2 *utf16str = NULL;
  glong size;
  char *utf8str = NULL;
  HWND hwnd;
  HIMC himc;
  int pos = 0;
  GError *error = NULL;

  if (pos_ret)
    *pos_ret = 0;

  if (!context_ime->client_surface)
    return g_strdup ("");
  hwnd = gdk_win32_surface_get_impl_hwnd (context_ime->client_surface);
  himc = ImmGetContext (hwnd);
  if (!himc)
    return g_strdup ("");

  size = ImmGetCompositionStringW (himc, kind, NULL, 0);

  if (size > 0)
    {
      utf16str = g_malloc (size);

      ImmGetCompositionStringW (himc, kind, utf16str, size);
      utf8str = g_utf16_to_utf8 (utf16str, size / sizeof (gunichar2),
                                 NULL, NULL, &error);
      if (error)
        {
          g_warning ("%s", error->message);
          g_error_free (error);

        }
    }

  if (pos_ret)
    {
      pos = ImmGetCompositionStringW (himc, GCS_CURSORPOS, NULL, 0);
      if (pos < 0 || size < pos)
        {
          g_warning ("ImmGetCompositionString: "
                     "Invalid cursor position!");
          pos = 0;
        }
    }

  if (!utf8str)
    {
      utf8str = g_strdup ("");
      pos = 0;
    }

  if (pos_ret)
    *pos_ret = pos;

  ImmReleaseContext (hwnd, himc);
  g_free (utf16str);

  return utf8str;
}


static PangoAttrList *
get_pango_attr_list (GtkIMContextIME *context_ime, const char *utf8str)
{
  PangoAttrList *attrs = pango_attr_list_new ();
  HWND hwnd;
  HIMC himc;
  guint8 *buf = NULL;

  if (!context_ime->client_surface)
    return attrs;
  hwnd = gdk_win32_surface_get_impl_hwnd (context_ime->client_surface);
  himc = ImmGetContext (hwnd);
  if (!himc)
    return attrs;

  if (context_ime->preediting)
    {
      const char *schr = utf8str, *echr;
      guint16 f_red, f_green, f_blue, b_red, b_green, b_blue;
      glong len, spos = 0, epos, sidx = 0, eidx;
      PangoAttribute *attr;

      /*
       *  get attributes list of IME.
       */
      len = ImmGetCompositionStringW (himc, GCS_COMPATTR, NULL, 0);
      buf = g_malloc (len);
      ImmGetCompositionStringW (himc, GCS_COMPATTR, buf, len);

      /*
       *  schr and echr are pointer in utf8str.
       */
      for (echr = g_utf8_next_char (utf8str); *schr != '\0';
           echr = g_utf8_next_char (echr))
        {
          /*
           *  spos and epos are buf(attributes list of IME) position by
           *  bytes.
           *  Using the wide-char API, this value is same with UTF-8 offset.
           */
	  epos = g_utf8_pointer_to_offset (utf8str, echr);

          /*
           *  sidx and eidx are positions in utf8str by bytes.
           */
          eidx = echr - utf8str;

          /*
           *  convert attributes list to PangoAttriute.
           */
          if (*echr == '\0' || buf[spos] != buf[epos])
            {
              switch (buf[spos])
                {
                case ATTR_TARGET_CONVERTED:
                  attr = pango_attr_underline_new (PANGO_UNDERLINE_DOUBLE);
                  attr->start_index = sidx;
                  attr->end_index = eidx;
                  pango_attr_list_change (attrs, attr);
                  f_red = f_green = f_blue = 0;
                  b_red = b_green = b_blue = 0xffff;
                  break;
                case ATTR_TARGET_NOTCONVERTED:
                  f_red = f_green = f_blue = 0xffff;
                  b_red = b_green = b_blue = 0;
                  break;
                case ATTR_INPUT_ERROR:
                  f_red = f_green = f_blue = 0;
                  b_red = b_green = b_blue = 0x7fff;
                  break;
                default:        /* ATTR_INPUT,ATTR_CONVERTED,ATTR_FIXEDCONVERTED */
                  attr = pango_attr_underline_new (PANGO_UNDERLINE_SINGLE);
                  attr->start_index = sidx;
                  attr->end_index = eidx;
                  pango_attr_list_change (attrs, attr);
                  f_red = f_green = f_blue = 0;
                  b_red = b_green = b_blue = 0xffff;
                }
              attr = pango_attr_foreground_new (f_red, f_green, f_blue);
              attr->start_index = sidx;
              attr->end_index = eidx;
              pango_attr_list_change (attrs, attr);
              attr = pango_attr_background_new (b_red, b_green, b_blue);
              attr->start_index = sidx;
              attr->end_index = eidx;
              pango_attr_list_change (attrs, attr);

              schr = echr;
              spos = epos;
              sidx = eidx;
            }
        }
    }

  ImmReleaseContext (hwnd, himc);
  g_free (buf);

  return attrs;
}


static void
gtk_im_context_ime_get_preedit_string (GtkIMContext   *context,
                                       char          **str,
                                       PangoAttrList **attrs,
                                       int            *cursor_pos)
{
  char *utf8str = NULL;
  int pos = 0;
  GtkIMContextIME *context_ime;

  context_ime = GTK_IM_CONTEXT_IME (context);

  if (!context_ime->focus || context_ime->priv->pretend_empty_preedit)
    utf8str = g_strdup ("");
  else
    utf8str = get_utf8_preedit_string (context_ime, GCS_COMPSTR, &pos);

  if (attrs)
    *attrs = get_pango_attr_list (context_ime, utf8str);

  if (str)
    *str = utf8str;
  else
    utf8str = NULL;

  if (cursor_pos)
    *cursor_pos = pos;
}


static void
gtk_im_context_ime_focus_in (GtkIMContext *context)
{
  GtkIMContextIME *context_ime = GTK_IM_CONTEXT_IME (context);
  GdkSurface *toplevel;
  HWND hwnd;
  HIMC himc;

  if (!GDK_IS_SURFACE (context_ime->client_surface))
    return;

  /* switch current context */
  context_ime->focus = TRUE;

  toplevel = context_ime->client_surface;
  if (!GDK_IS_SURFACE (toplevel))
    {
      g_warning ("gtk_im_context_ime_focus_in(): "
                 "cannot find toplevel window.");
      return;
    }

  hwnd = gdk_win32_surface_get_impl_hwnd (toplevel);
  himc = ImmGetContext (hwnd);
  if (!himc)
    return;

  gdk_win32_display_add_filter (GDK_WIN32_DISPLAY (gdk_surface_get_display (toplevel)),
                                gtk_im_context_ime_message_filter, context_ime);

  /* restore preedit context */
  context_ime->opened = ImmGetOpenStatus (himc);

  switch (context_ime->priv->focus_behavior)
    {
      case GTK_WIN32_IME_FOCUS_BEHAVIOR_COMMIT:
      case GTK_WIN32_IME_FOCUS_BEHAVIOR_DISCARD:
        gtk_im_context_ime_reset (context);
        break;

      case GTK_WIN32_IME_FOCUS_BEHAVIOR_FOLLOW:
        {
          gchar *utf8str = get_utf8_preedit_string (context_ime, GCS_COMPSTR, NULL);
          if (utf8str != NULL && strlen(utf8str) > 0)
            {
              context_ime->preediting = TRUE;
              gtk_im_context_ime_set_cursor_location (context, NULL);
              g_signal_emit_by_name (context, "preedit-start");
              g_signal_emit_by_name (context, "preedit-changed");
            }
          g_free (utf8str);
        }
        break;
      default:
        g_assert_not_reached ();
        break;
    }

  /* clean */
  ImmReleaseContext (hwnd, himc);
}


static void
gtk_im_context_ime_focus_out (GtkIMContext *context)
{
  GtkIMContextIME *context_ime = GTK_IM_CONTEXT_IME (context);
  gboolean was_preediting;

  if (!GDK_IS_SURFACE (context_ime->client_surface))
    return;

  /* switch current context */
  was_preediting = context_ime->preediting;
  context_ime->opened = FALSE;
  context_ime->preediting = FALSE;
  context_ime->focus = FALSE;

  switch (context_ime->priv->focus_behavior)
    {
      case GTK_WIN32_IME_FOCUS_BEHAVIOR_COMMIT:
        if (was_preediting)
          {
            gchar *utf8str = get_utf8_preedit_string (context_ime, GCS_COMPSTR, NULL);

            context_ime->priv->pretend_empty_preedit = TRUE;
            g_signal_emit_by_name (context, "preedit-changed");
            g_signal_emit_by_name (context, "preedit-end");
			g_signal_emit_by_name (context, "commit", utf8str);
            g_signal_emit_by_name (context, "preedit-start");
            g_signal_emit_by_name (context, "preedit-changed");
            context_ime->priv->pretend_empty_preedit = FALSE;
            g_free (utf8str);
          }
        G_GNUC_FALLTHROUGH;
      case GTK_WIN32_IME_FOCUS_BEHAVIOR_DISCARD:
        gtk_im_context_ime_reset (context);

        /* Callbacks triggered by im_context_ime_reset() could set the focus back to our
           context. In that case, we want to exit here. */

        if (context_ime->focus)
          return;

        break;

      case GTK_WIN32_IME_FOCUS_BEHAVIOR_FOLLOW:
        break;
    }

  /* remove event filter */
  if (GDK_IS_SURFACE (context_ime->client_surface))
    {
      gdk_win32_display_remove_filter (GDK_WIN32_DISPLAY (gdk_surface_get_display (context_ime->client_surface)),
                                       gtk_im_context_ime_message_filter,
                                       context_ime);
    }

  if (was_preediting)
    {
      g_warning ("gtk_im_context_ime_focus_out(): "
                 "cannot find toplevel window.");
      g_signal_emit_by_name (context, "preedit-changed");
      g_signal_emit_by_name (context, "preedit-end");
    }
}


static void
gtk_im_context_ime_set_cursor_location (GtkIMContext *context,
                                        GdkRectangle *area)
{
  int wx = 0, wy = 0;
  GtkIMContextIME *context_ime;
  COMPOSITIONFORM cf;
  HWND hwnd;
  HIMC himc;

  g_return_if_fail (GTK_IS_IM_CONTEXT_IME (context));

  context_ime = GTK_IM_CONTEXT_IME (context);
  if (area)
    context_ime->cursor_location = *area;

  if (!context_ime->client_surface)
    return;

  hwnd = gdk_win32_surface_get_impl_hwnd (context_ime->client_surface);
  himc = ImmGetContext (hwnd);
  if (!himc)
    return;

  get_window_position (context_ime->client_surface, &wx, &wy);
  cf.dwStyle = CFS_POINT;
  cf.ptCurrentPos.x = wx + context_ime->cursor_location.x;
  cf.ptCurrentPos.y = wy + context_ime->cursor_location.y;
  ImmSetCompositionWindow (himc, &cf);

  ImmReleaseContext (hwnd, himc);
}


static void
gtk_im_context_ime_set_use_preedit (GtkIMContext *context,
                                    gboolean      use_preedit)
{
  GtkIMContextIME *context_ime;

  g_return_if_fail (GTK_IS_IM_CONTEXT_IME (context));
  context_ime = GTK_IM_CONTEXT_IME (context);

  context_ime->use_preedit = use_preedit;
  if (context_ime->preediting)
    {
      HWND hwnd;
      HIMC himc;

      hwnd = gdk_win32_surface_get_impl_hwnd (context_ime->client_surface);
      himc = ImmGetContext (hwnd);
      if (!himc)
        return;

      /* FIXME: What to do? */

      ImmReleaseContext (hwnd, himc);
    }
}


static void
gtk_im_context_ime_set_preedit_font (GtkIMContext *context)
{
  GtkIMContextIME *context_ime;
  GtkWidget *widget = NULL;
  HWND hwnd;
  HIMC himc;
  HKL ime = GetKeyboardLayout (0);
  const char *lang;
  gunichar wc;
  PangoContext *pango_context;
  PangoFont *font;
  LOGFONT *logfont;
  PangoFontDescription *font_desc;

  g_return_if_fail (GTK_IS_IM_CONTEXT_IME (context));

  context_ime = GTK_IM_CONTEXT_IME (context);
  if (!context_ime->client_surface)
    return;

  widget = GTK_WIDGET (gtk_native_get_for_surface (context_ime->client_surface));
  if (!widget)
    return;

  hwnd = gdk_win32_surface_get_impl_hwnd (context_ime->client_surface);
  himc = ImmGetContext (hwnd);
  if (!himc)
    return;

  /* set font */
  pango_context = gtk_widget_get_pango_context (widget);
  if (!pango_context)
    goto ERROR_OUT;

  /* Try to make sure we use a font that actually can show the
   * language in question.
   */

  switch (PRIMARYLANGID (LOWORD (ime)))
    {
    case LANG_JAPANESE:
      lang = "ja"; break;
    case LANG_KOREAN:
      lang = "ko"; break;
    case LANG_CHINESE:
      switch (SUBLANGID (LOWORD (ime)))
	{
	case SUBLANG_CHINESE_TRADITIONAL:
	  lang = "zh_TW"; break;
	case SUBLANG_CHINESE_SIMPLIFIED:
	  lang = "zh_CN"; break;
	case SUBLANG_CHINESE_HONGKONG:
	  lang = "zh_HK"; break;
	case SUBLANG_CHINESE_SINGAPORE:
	  lang = "zh_SG"; break;
	case SUBLANG_CHINESE_MACAU:
	  lang = "zh_MO"; break;
	default:
	  lang = "zh"; break;
	}
      break;
    default:
      lang = ""; break;
    }

  font_desc = gtk_css_style_get_pango_font (gtk_style_context_lookup_style (gtk_widget_get_style_context (widget)));

  if (lang[0])
    {
      /* We know what language it is. Look for a character, any
       * character, that language needs.
       */
      PangoLanguage *pango_lang = pango_language_from_string (lang);
      PangoFontset *fontset =
        pango_context_load_fontset (pango_context,
				                            font_desc,
				                            pango_lang);
      gunichar *sample =
	g_utf8_to_ucs4 (pango_language_get_sample_string (pango_lang),
			-1, NULL, NULL, NULL);
      wc = 0x4E00;		/* In all CJK languages? */
      if (sample != NULL)
	{
	  int i;

	  for (i = 0; sample[i]; i++)
	    if (g_unichar_iswide (sample[i]))
	      {
		wc = sample[i];
		break;
	      }
	  g_free (sample);
	}
      font = pango_fontset_get_font (fontset, wc);
      g_object_unref (fontset);
    }
  else
    font = pango_context_load_font (pango_context, font_desc);

  if (!font)
    goto ERROR_OUT;

  logfont = pango_win32_font_logfont (font);
  if (logfont)
    ImmSetCompositionFont (himc, logfont);

  g_object_unref (font);

ERROR_OUT:
  /* clean */
  ImmReleaseContext (hwnd, himc);
}


static GdkWin32MessageFilterReturn
gtk_im_context_ime_message_filter (GdkWin32Display *display,
                                   MSG             *msg,
                                   int             *ret_valp,
                                   gpointer         data)
{
  GtkIMContext *context;
  GtkIMContextIME *context_ime;
  HWND hwnd;
  HIMC himc;
  GdkSurface *toplevel;
  GdkWin32MessageFilterReturn retval = GDK_WIN32_MESSAGE_FILTER_CONTINUE;

  g_return_val_if_fail (GTK_IS_IM_CONTEXT_IME (data), retval);

  context = GTK_IM_CONTEXT (data);
  context_ime = GTK_IM_CONTEXT_IME (data);
  if (!context_ime->focus)
    return retval;

  toplevel = context_ime->client_surface;
  if (gdk_win32_surface_get_impl_hwnd (toplevel) != msg->hwnd)
    return retval;

  hwnd = gdk_win32_surface_get_impl_hwnd (context_ime->client_surface);
  himc = ImmGetContext (hwnd);
  if (!himc)
    return retval;

  *ret_valp = 0;

  switch (msg->message)
    {
    case WM_IME_COMPOSITION:
      {
        int wx = 0, wy = 0;
        CANDIDATEFORM cf;

        get_window_position (context_ime->client_surface, &wx, &wy);
        /* FIXME! */
        {
          HWND impl_hwnd;
          POINT pt;
          RECT rc;

          impl_hwnd =
            gdk_win32_surface_get_impl_hwnd (context_ime->client_surface);
          GetWindowRect (impl_hwnd, &rc);
          pt.x = wx;
          pt.y = wy;
          ClientToScreen (impl_hwnd, &pt);
          wx = pt.x - rc.left;
          wy = pt.y - rc.top;
        }
        cf.dwIndex = 0;
        cf.dwStyle = CFS_CANDIDATEPOS;
        cf.ptCurrentPos.x = wx + context_ime->cursor_location.x;
        cf.ptCurrentPos.y = wy + context_ime->cursor_location.y
          + context_ime->cursor_location.height;
        ImmSetCandidateWindow (himc, &cf);

        if ((msg->lParam & GCS_COMPSTR))
          g_signal_emit_by_name (context, "preedit-changed");

        if (msg->lParam & GCS_RESULTSTR)
          {
            gchar *utf8str = get_utf8_preedit_string (context_ime, GCS_RESULTSTR, NULL);

            if (utf8str)
              {
                context_ime->priv->pretend_empty_preedit = TRUE;
                g_signal_emit_by_name (context, "preedit-changed");
                g_signal_emit_by_name (context, "preedit-end");

                g_signal_emit_by_name (context, "commit", utf8str);

                g_signal_emit_by_name (context, "preedit-start");
                g_signal_emit_by_name (context, "preedit-changed");
                context_ime->priv->pretend_empty_preedit = FALSE;

                retval = TRUE;
              }

            g_free (utf8str);
          }

        if (context_ime->use_preedit)
          retval = GDK_WIN32_MESSAGE_FILTER_REMOVE;
      }

      break;

    case WM_IME_STARTCOMPOSITION:
      context_ime->preediting = TRUE;
      gtk_im_context_ime_set_cursor_location (context, NULL);
      g_signal_emit_by_name (context, "preedit-start");
      if (context_ime->use_preedit)
        retval = GDK_WIN32_MESSAGE_FILTER_REMOVE;
      break;

    case WM_IME_ENDCOMPOSITION:
      context_ime->preediting = FALSE;
      g_signal_emit_by_name (context, "preedit-changed");
      g_signal_emit_by_name (context, "preedit-end");

      if (context_ime->use_preedit)
        retval = GDK_WIN32_MESSAGE_FILTER_REMOVE;
      break;

    case WM_IME_NOTIFY:
      switch (msg->wParam)
        {
        case IMN_SETOPENSTATUS:
          context_ime->opened = ImmGetOpenStatus (himc);
          gtk_im_context_ime_set_preedit_font (context);
          break;

        default:
          break;
        }
      break;
    default:
      break;
    }

  ImmReleaseContext (hwnd, himc);
  return retval;
}


/*
 * x and y must be initialized to 0.
 */
static void
get_window_position (GdkSurface *surface, int *x, int *y)
{
  GdkSurface *parent, *toplevel;

  g_return_if_fail (GDK_IS_SURFACE (surface));
  g_return_if_fail (x && y);

  if (GDK_IS_POPUP (surface))
    {
      parent = gdk_popup_get_parent (GDK_POPUP (surface));
      toplevel = surface;
    }
  else
    {
      parent = NULL;
      toplevel = surface;
    }

  if (parent && parent != toplevel)
    get_window_position (parent, x, y);
}
