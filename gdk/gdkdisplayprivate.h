/* GDK - The GIMP Drawing Kit
 * Copyright (C) 2010 Red Hat, Inc.
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
 */

#ifndef __GDK_DISPLAY_PRIVATE_H__
#define __GDK_DISPLAY_PRIVATE_H__

#include "gdkdisplay.h"
#include "gdksurface.h"
#include "gdkcursor.h"
#include "gdkmonitor.h"
#include "gdkdebug.h"
#include "gdksurfaceprivate.h"
#include "gdkkeysprivate.h"
#include "gdkdeviceprivate.h"

#ifdef GDK_RENDERING_VULKAN
#include <vulkan/vulkan.h>
#endif

G_BEGIN_DECLS

#define GDK_DISPLAY_CLASS(klass)      (G_TYPE_CHECK_CLASS_CAST ((klass), GDK_TYPE_DISPLAY, GdkDisplayClass))
#define GDK_IS_DISPLAY_CLASS(klass)   (G_TYPE_CHECK_CLASS_TYPE ((klass), GDK_TYPE_DISPLAY))
#define GDK_DISPLAY_GET_CLASS(obj)    (G_TYPE_INSTANCE_GET_CLASS ((obj), GDK_TYPE_DISPLAY, GdkDisplayClass))


typedef struct _GdkDisplayClass GdkDisplayClass;

/* Tracks information about the device grab on this display */
typedef struct
{
  GdkSurface *surface;
  gulong serial_start;
  gulong serial_end; /* exclusive, i.e. not active on serial_end */
  guint event_mask;
  guint32 time;

  guint activated : 1;
  guint implicit_ungrab : 1;
  guint owner_events : 1;
  guint implicit : 1;
} GdkDeviceGrabInfo;

/* Tracks information about which surface and position the pointer last was in.
 * This is useful when we need to synthesize events later.
 * Note that we track toplevel_under_pointer using enter/leave events,
 * so in the case of a grab, either with owner_events==FALSE or with the
 * pointer in no clients surface the x/y coordinates may actually be outside
 * the surface.
 */
typedef struct
{
  GdkSurface *surface_under_pointer;   /* surface that last got a normal enter event */
  double toplevel_x, toplevel_y;
  guint32 state;
  guint32 button;
  GdkDevice *last_physical_device;
} GdkPointerSurfaceInfo;

struct _GdkDisplay
{
  GObject parent_instance;

  GQueue queued_events;

  guint event_pause_count;       /* How many times events are blocked */

  guint closed             : 1;  /* Whether this display has been closed */

  GHashTable *device_grabs;

  GdkClipboard *clipboard;
  GdkClipboard *primary_clipboard;

  GHashTable *pointers_info;  /* GdkPointerSurfaceInfo for each device */
  guint32 last_event_time;    /* Last reported event time from server */

  guint double_click_time;  /* Maximum time between clicks in msecs */
  guint double_click_distance;   /* Maximum distance between clicks in pixels */

  GList *seats;

#ifdef GDK_RENDERING_VULKAN
  VkInstance vk_instance;
  VkDebugReportCallbackEXT vk_debug_callback;
  VkPhysicalDevice vk_physical_device;
  VkDevice vk_device;
  VkQueue vk_queue;
  uint32_t vk_queue_family_index;

  guint vulkan_refcount;
#endif /* GDK_RENDERING_VULKAN */

  /* egl info */
  guint have_egl_buffer_age : 1;
  guint have_egl_swap_buffers_with_damage : 1;
  guint have_egl_no_config_context : 1;
  guint have_egl_pixel_format_float : 1;
  guint have_egl_win32_libangle : 1;
};

struct _GdkDisplayClass
{
  GObjectClass parent_class;

  GType cairo_context_type;   /* type for GdkCairoContext, must be set */
  GType vk_context_type;      /* type for GdkVulkanContext, must be set if vk_extension_name != NULL */
  const char *vk_extension_name; /* Name of required windowing vulkan extension or %NULL (default) if Vulkan isn't supported */

  const char *              (*get_name)           (GdkDisplay *display);
  void                       (*beep)               (GdkDisplay *display);
  void                       (*sync)               (GdkDisplay *display);
  void                       (*flush)              (GdkDisplay *display);
  gboolean                   (*has_pending)        (GdkDisplay *display);
  void                       (*queue_events)       (GdkDisplay *display);
  void                       (*make_default)       (GdkDisplay *display);

  GdkAppLaunchContext *      (*get_app_launch_context) (GdkDisplay *display);

  gulong                     (*get_next_serial) (GdkDisplay *display);

  void                       (*notify_startup_complete) (GdkDisplay  *display,
                                                         const char *startup_id);
  const char *              (*get_startup_notification_id) (GdkDisplay  *display);

  GdkSurface *               (*create_surface) (GdkDisplay     *display,
                                                GdkSurfaceType  surface_type,
                                                GdkSurface     *parent,
                                                int             x,
                                                int             y,
                                                int             width,
                                                int             height);

  GdkKeymap *                (*get_keymap)         (GdkDisplay    *display);

  GdkGLContext *         (* init_gl)                   (GdkDisplay        *display,
                                                        GError           **error);
  /* Returns the distance from a perfect score EGL config.
   * GDK chooses the one with the *LOWEST* score */
  guint                  (* rate_egl_config)           (GdkDisplay        *display,
                                                        gpointer           egl_display,
                                                        gpointer           egl_config);

  GdkSeat *              (*get_default_seat)           (GdkDisplay        *display);

  GListModel *           (*get_monitors)               (GdkDisplay     *self);
  GdkMonitor *           (*get_monitor_at_surface)     (GdkDisplay     *display,
                                                        GdkSurface      *surface);
  gboolean               (*get_setting)                (GdkDisplay     *display,
                                                        const char     *name,
                                                        GValue         *value);
  void                   (*set_cursor_theme)           (GdkDisplay     *display,
                                                        const char     *name,
                                                        int             size);

  /* Signals */
  void                   (*opened)                     (GdkDisplay     *display);
  void                   (*closed)                     (GdkDisplay     *display,
                                                        gboolean        is_error);
};


typedef void (* GdkDisplayPointerInfoForeach) (GdkDisplay           *display,
                                               GdkDevice            *device,
                                               GdkPointerSurfaceInfo *device_info,
                                               gpointer              user_data);

void                _gdk_display_update_last_event    (GdkDisplay     *display,
                                                       GdkEvent       *event);
void                _gdk_display_device_grab_update   (GdkDisplay *display,
                                                       GdkDevice  *device,
                                                       gulong      current_serial);
GdkDeviceGrabInfo * _gdk_display_get_last_device_grab (GdkDisplay *display,
                                                       GdkDevice  *device);
GdkDeviceGrabInfo * _gdk_display_add_device_grab      (GdkDisplay       *display,
                                                       GdkDevice        *device,
                                                       GdkSurface        *surface,
                                                       gboolean          owner_events,
                                                       GdkEventMask      event_mask,
                                                       gulong            serial_start,
                                                       guint32           time,
                                                       gboolean          implicit);
GdkDeviceGrabInfo * _gdk_display_has_device_grab      (GdkDisplay       *display,
                                                       GdkDevice        *device,
                                                       gulong            serial);
gboolean            _gdk_display_end_device_grab      (GdkDisplay       *display,
                                                       GdkDevice        *device,
                                                       gulong            serial,
                                                       GdkSurface        *if_child,
                                                       gboolean          implicit);
GdkPointerSurfaceInfo * _gdk_display_get_pointer_info  (GdkDisplay       *display,
                                                       GdkDevice        *device);
void                _gdk_display_pointer_info_foreach (GdkDisplay       *display,
                                                       GdkDisplayPointerInfoForeach func,
                                                       gpointer          user_data);
gulong              _gdk_display_get_next_serial      (GdkDisplay       *display);
void                _gdk_display_pause_events         (GdkDisplay       *display);
void                _gdk_display_unpause_events       (GdkDisplay       *display);
GdkSurface *        gdk_display_create_surface        (GdkDisplay       *display,
                                                       GdkSurfaceType    surface_type,
                                                       GdkSurface       *parent,
                                                       int               x,
                                                       int               y,
                                                       int               width,
                                                       int               height);

GdkGLContext *      gdk_display_get_gl_context        (GdkDisplay       *display);

gboolean            gdk_display_init_egl              (GdkDisplay       *display,
                                                       int /*EGLenum*/   platform,
                                                       gpointer          native_display,
                                                       gboolean          allow_any,
                                                       GError          **error);
gpointer            gdk_display_get_egl_display       (GdkDisplay       *display);
gpointer            gdk_display_get_egl_config        (GdkDisplay       *display);
gpointer            gdk_display_get_egl_config_high_depth
                                                      (GdkDisplay       *display);

void                gdk_display_set_rgba              (GdkDisplay       *display,
                                                       gboolean          rgba);
void                gdk_display_set_composited        (GdkDisplay       *display,
                                                       gboolean          composited);
void                gdk_display_set_input_shapes      (GdkDisplay       *display,
                                                       gboolean          input_shapes);

void                gdk_display_add_seat              (GdkDisplay       *display,
                                                       GdkSeat          *seat);
void                gdk_display_remove_seat           (GdkDisplay       *display,
                                                       GdkSeat          *seat);
void                gdk_display_emit_opened           (GdkDisplay       *display);

void                gdk_display_setting_changed       (GdkDisplay       *display,
                                                       const char       *name);

GdkEvent *          gdk_display_get_event             (GdkDisplay       *display);

GdkKeymap *  gdk_display_get_keymap  (GdkDisplay *display);

void _gdk_display_set_surface_under_pointer (GdkDisplay *display,
                                             GdkDevice  *device,
                                             GdkSurface  *surface);

void _gdk_windowing_got_event                (GdkDisplay       *display,
                                              GList            *event_link,
                                              GdkEvent         *event,
                                              gulong            serial);

G_END_DECLS

#endif  /* __GDK_DISPLAY_PRIVATE_H__ */
