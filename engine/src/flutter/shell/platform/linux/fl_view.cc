// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/shell/platform/linux/public/flutter_linux/fl_view.h"

#ifdef GDK_WINDOWING_WAYLAND
#include <gdk/wayland/gdkwayland.h>
#endif

#include <cstring>

#include "flutter/common/constants.h"
#include "flutter/shell/platform/linux/fl_compositor_opengl.h"
#include "flutter/shell/platform/linux/fl_compositor_software.h"
#include "flutter/shell/platform/linux/fl_engine_private.h"
#include "flutter/shell/platform/linux/fl_key_event.h"
#include "flutter/shell/platform/linux/fl_opengl_manager.h"
#include "flutter/shell/platform/linux/fl_plugin_registrar_private.h"
#include "flutter/shell/platform/linux/fl_pointer_manager.h"
#include "flutter/shell/platform/linux/fl_scrolling_manager.h"
#include "flutter/shell/platform/linux/fl_touch_manager.h"
#include "flutter/shell/platform/linux/fl_view_private.h"
#include "flutter/shell/platform/linux/fl_view_accessible.h"
#include "flutter/shell/platform/linux/fl_window_state_monitor.h"
#include "flutter/shell/platform/linux/public/flutter_linux/fl_engine.h"
#include "flutter/shell/platform/linux/public/flutter_linux/fl_plugin_registry.h"

struct _FlView {
  GtkBox parent_instance;

  // Event box the render area goes inside.
  GtkWidget* event_box;

  // The widget rendering the Flutter view.
  GtkDrawingArea* render_area;

  // Rendering context when using OpenGL.
  GdkGLContext* render_context;

  // Engine this view is showing.
  FlEngine* engine;

  // Combines layers into frame.
  FlCompositor* compositor;

  // Signal subscription for engine restart signal.
  guint on_pre_engine_restart_cb_id;

  // Signal subscription for updating semantics signal.
  guint update_semantics_cb_id;

  // ID for this view.
  FlutterViewId view_id;

  // Background color.
  GdkRGBA* background_color;

  // TRUE if have got the first frame to render.
  gboolean have_first_frame;

  // Monitor to track window state.
  FlWindowStateMonitor* window_state_monitor;

  // Manages scrolling events.
  FlScrollingManager* scrolling_manager;

  // Manages pointer events.
  FlPointerManager* pointer_manager;

  // Manages touch events.
  FlTouchManager* touch_manager;

  // Accessible tree from Flutter, exposed as an AtkPlug.
  FlViewAccessible* view_accessible;

  // Signal subscripton for cursor changes.
  guint cursor_changed_cb_id;

  // TRUE if the view size should be controlled by Flutter.
  gboolean sized_to_content;

  GCancellable* cancellable;
};

enum { SIGNAL_FIRST_FRAME, LAST_SIGNAL };

static guint fl_view_signals[LAST_SIGNAL];

static void fl_renderable_iface_init(FlRenderableInterface* iface);

static void fl_view_plugin_registry_iface_init(
    FlPluginRegistryInterface* iface);

G_DEFINE_TYPE_WITH_CODE(
    FlView,
    fl_view,
    GTK_TYPE_BOX,
    G_IMPLEMENT_INTERFACE(fl_renderable_get_type(), fl_renderable_iface_init)
        G_IMPLEMENT_INTERFACE(fl_plugin_registry_get_type(),
                              fl_view_plugin_registry_iface_init))

static gboolean handle_key_event(FlView* self, FlKeyEvent* event);

// Redraw the view from the GTK thread.
static gboolean redraw_cb(gpointer user_data) {
  FlView* self = FL_VIEW(user_data);

  if (!self->have_first_frame) {
    self->have_first_frame = TRUE;
    g_signal_emit(self, fl_view_signals[SIGNAL_FIRST_FRAME], 0);
  }

  // If Flutter is controlling the window size, then resize the view if
  // necessary.
  GtkAllocation allocation;
  gtk_widget_get_allocation(GTK_WIDGET(self->render_area), &allocation);
  gint scale_factor =
      gtk_widget_get_scale_factor(GTK_WIDGET(self->render_area));
  size_t width = allocation.width * scale_factor;
  size_t height = allocation.height * scale_factor;
  size_t frame_width, frame_height;
  fl_compositor_get_frame_size(self->compositor, &frame_width, &frame_height);
  gboolean frame_size_matches = width == frame_width && height == frame_height;
  if (self->sized_to_content && !frame_size_matches) {
    gtk_widget_set_size_request(GTK_WIDGET(self->render_area),
                                frame_width / scale_factor,
                                frame_height / scale_factor);
    GtkRoot* root = gtk_widget_get_root(GTK_WIDGET(self->render_area));
    if (GTK_IS_WINDOW(root)) {
      // Resize to smallest size, so that the window will shrink to fit the new
      // size of the render area.
      gtk_window_set_default_size(GTK_WINDOW(root), 1, 1);
    }
    return G_SOURCE_REMOVE;
  }

  gtk_widget_queue_draw(GTK_WIDGET(self->render_area));

  return G_SOURCE_REMOVE;
}

// Signal handler for GtkWidget::delete-event
static gboolean window_delete_event_cb(FlView* self) {
  fl_engine_request_app_exit(self->engine);
  // Stop the event from propagating.
  return TRUE;
}

static void init_scrolling(FlView* self) {
  g_clear_object(&self->scrolling_manager);
  self->scrolling_manager =
      fl_scrolling_manager_new(self->engine, self->view_id);
}

static void init_touch(FlView* self) {
  g_clear_object(&self->touch_manager);
  self->touch_manager = fl_touch_manager_new(self->engine, self->view_id);
}

static FlutterPointerDeviceKind get_pointer_device_kind(GdkEvent* event) {
  if (event == nullptr) {
    return kFlutterPointerDeviceKindMouse;
  }
  GdkDevice* device = gdk_event_get_device(event);
  if (device == nullptr) {
    return kFlutterPointerDeviceKindMouse;
  }

  GdkInputSource source = gdk_device_get_source(device);
  switch (source) {
    case GDK_SOURCE_PEN:
    case GDK_SOURCE_TABLET_PAD:
      return kFlutterPointerDeviceKindStylus;
    case GDK_SOURCE_TOUCHSCREEN:
      return kFlutterPointerDeviceKindTouch;
    case GDK_SOURCE_TOUCHPAD:  // trackpad device type is reserved for gestures
    case GDK_SOURCE_TRACKPOINT:
    case GDK_SOURCE_KEYBOARD:
    case GDK_SOURCE_MOUSE:
      return kFlutterPointerDeviceKindMouse;
  }
  return kFlutterPointerDeviceKindMouse;
}

// Called when the mouse cursor changes.
static void cursor_changed_cb(FlView* self) {
  FlMouseCursorHandler* handler =
      fl_engine_get_mouse_cursor_handler(self->engine);
  const gchar* cursor_name = fl_mouse_cursor_handler_get_cursor_name(handler);
  g_autoptr(GdkCursor) cursor =
      gdk_cursor_new_from_name(cursor_name, nullptr);
  gtk_widget_set_cursor(GTK_WIDGET(self), cursor);
}

// Set the mouse cursor.
static void setup_cursor(FlView* self) {
  FlMouseCursorHandler* handler =
      fl_engine_get_mouse_cursor_handler(self->engine);

  self->cursor_changed_cb_id = g_signal_connect_swapped(
      handler, "cursor-changed", G_CALLBACK(cursor_changed_cb), self);
  cursor_changed_cb(self);
}

// Updates the engine with the current window metrics.
static void handle_geometry_changed(FlView* self) {
  // No updates required when size controlled by Flutter.
  if (self->sized_to_content) {
    return;
  }

  GtkAllocation allocation;
  gtk_widget_get_allocation(GTK_WIDGET(self), &allocation);
  gint scale_factor = gtk_widget_get_scale_factor(GTK_WIDGET(self));

  FlutterEngineDisplayId display_id = 0;
  size_t width = allocation.width, height = allocation.height;
  size_t min_width = width, min_height = height;
  size_t max_width = width, max_height = height;
  fl_engine_send_window_metrics_event(
      self->engine, display_id, self->view_id, min_width * scale_factor,
      min_height * scale_factor, max_width * scale_factor,
      max_height * scale_factor, scale_factor);
}

static void view_added_cb(GObject* object,
                          GAsyncResult* result,
                          gpointer user_data) {
  g_autoptr(GError) error = nullptr;
  if (!fl_engine_add_view_finish(FL_ENGINE(object), result, &error)) {
    if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
      return;
    }

    g_warning("Failed to add view: %s", error->message);
    // FIXME: Show on the GLArea
    return;
  }
}

/*
// Called when the engine updates accessibility.
static void update_semantics_cb(FlView* self,
                                const FlutterSemanticsUpdate2* update) {
  // No-op for GTK4 PoC
}
*/

// Invoked by the engine right before the engine is restarted.
//
// This method should reset states to be as if the engine had just been started,
// which usually indicates the user has requested a hot restart (Shift-R in the
// Flutter CLI.)
static void on_pre_engine_restart_cb(FlView* self) {
  init_scrolling(self);
  init_touch(self);
}

// Implements FlRenderable::present_layers
static void fl_view_present_layers(FlRenderable* renderable,
                                   const FlutterLayer** layers,
                                   size_t layers_count) {
  FlView* self = FL_VIEW(renderable);

  fl_compositor_present_layers(self->compositor, layers, layers_count);

  // Perform the redraw in the GTK thead.
  g_idle_add(redraw_cb, self);
}

// Implements FlPluginRegistry::get_registrar_for_plugin.
static FlPluginRegistrar* fl_view_get_registrar_for_plugin(
    FlPluginRegistry* registry,
    const gchar* name) {
  FlView* self = FL_VIEW(registry);

  return fl_plugin_registrar_new(self,
                                 fl_engine_get_binary_messenger(self->engine),
                                 fl_engine_get_texture_registrar(self->engine));
}

static void fl_renderable_iface_init(FlRenderableInterface* iface) {
  iface->present_layers = fl_view_present_layers;
}

static void fl_view_plugin_registry_iface_init(
    FlPluginRegistryInterface* iface) {
  iface->get_registrar_for_plugin = fl_view_get_registrar_for_plugin;
}

static void sync_modifier_if_needed(FlView* self, GdkEvent* event) {
  if (event == nullptr) {
    return;
  }
  guint event_time = gdk_event_get_time(event);
  GdkModifierType event_state = gdk_event_get_modifier_state(event);
  fl_keyboard_manager_sync_modifier_if_needed(
      fl_engine_get_keyboard_manager(self->engine), event_state, event_time);
}

static void set_scrolling_position(FlView* self, gdouble x, gdouble y) {
  gint scale_factor = gtk_widget_get_scale_factor(GTK_WIDGET(self));
  fl_scrolling_manager_set_last_mouse_position(
      self->scrolling_manager, x * scale_factor, y * scale_factor);
}

static void button_pressed_cb(GtkGestureClick* gesture,
                              gint n_press,
                              gdouble x,
                              gdouble y,
                              gpointer user_data) {
  FlView* self = FL_VIEW(user_data);
  GdkEvent* event = gtk_event_controller_get_current_event(GTK_EVENT_CONTROLLER(gesture));
  guint button = gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(gesture));
  guint32 event_time = gdk_event_get_time(event);

  set_scrolling_position(self, x, y);
  sync_modifier_if_needed(self, event);

  gint scale_factor = gtk_widget_get_scale_factor(GTK_WIDGET(self));
  fl_pointer_manager_handle_button_press(
      self->pointer_manager, event_time,
      get_pointer_device_kind(event), x * scale_factor, y * scale_factor,
      button);
}

static void button_released_cb(GtkGestureClick* gesture,
                               gint n_press,
                               gdouble x,
                               gdouble y,
                               gpointer user_data) {
  FlView* self = FL_VIEW(user_data);
  GdkEvent* event = gtk_event_controller_get_current_event(GTK_EVENT_CONTROLLER(gesture));
  guint button = gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(gesture));
  guint32 event_time = gdk_event_get_time(event);

  set_scrolling_position(self, x, y);
  sync_modifier_if_needed(self, event);

  gint scale_factor = gtk_widget_get_scale_factor(GTK_WIDGET(self));
  fl_pointer_manager_handle_button_release(
      self->pointer_manager, event_time,
      get_pointer_device_kind(event), x * scale_factor, y * scale_factor,
      button);
}

static gboolean scroll_cb(GtkEventControllerScroll* controller,
                          gdouble dx,
                          gdouble dy,
                          gpointer user_data) {
  FlView* self = FL_VIEW(user_data);
  GdkEvent* event = gtk_event_controller_get_current_event(GTK_EVENT_CONTROLLER(controller));
  fl_scrolling_manager_handle_scroll_event(
      self->scrolling_manager, event,
      gtk_widget_get_scale_factor(GTK_WIDGET(self)));
  return TRUE;
}

static void motion_cb(GtkEventControllerMotion* controller,
                      gdouble x,
                      gdouble y,
                      gpointer user_data) {
  FlView* self = FL_VIEW(user_data);
  GdkEvent* event = gtk_event_controller_get_current_event(GTK_EVENT_CONTROLLER(controller));
  sync_modifier_if_needed(self, event);

  gint scale_factor = gtk_widget_get_scale_factor(GTK_WIDGET(self));
  fl_pointer_manager_handle_motion(
      self->pointer_manager, gdk_event_get_time(event),
      get_pointer_device_kind(event), x * scale_factor, y * scale_factor);
}

static void enter_cb(GtkEventControllerMotion* controller,
                     gdouble x,
                     gdouble y,
                     gpointer user_data) {
  FlView* self = FL_VIEW(user_data);
  GdkEvent* event = gtk_event_controller_get_current_event(GTK_EVENT_CONTROLLER(controller));
  gint scale_factor = gtk_widget_get_scale_factor(GTK_WIDGET(self));
  fl_pointer_manager_handle_enter(
      self->pointer_manager, gdk_event_get_time(event),
      get_pointer_device_kind(event), x * scale_factor, y * scale_factor);
}

static void leave_cb(GtkEventControllerMotion* controller,
                     gpointer user_data) {
  FlView* self = FL_VIEW(user_data);
  GdkEvent* event = gtk_event_controller_get_current_event(GTK_EVENT_CONTROLLER(controller));
  gdouble x = 0.0, y = 0.0;
  gint scale_factor = gtk_widget_get_scale_factor(GTK_WIDGET(self));
  fl_pointer_manager_handle_leave(
      self->pointer_manager, gdk_event_get_time(event),
      get_pointer_device_kind(event), x * scale_factor, y * scale_factor);
}

static void focus_enter_cb(GtkEventControllerFocus* controller,
                           gpointer user_data) {
  FlView* self = FL_VIEW(user_data);
  fl_text_input_handler_set_widget(
      fl_engine_get_text_input_handler(self->engine), GTK_WIDGET(self));
}

static gboolean key_pressed_cb(GtkEventControllerKey* controller,
                               guint keyval,
                               guint keycode,
                               GdkModifierType state,
                               gpointer user_data) {
  FlView* self = FL_VIEW(user_data);
  GdkEvent* event = gtk_event_controller_get_current_event(GTK_EVENT_CONTROLLER(controller));
  guint32 event_time = gdk_event_get_time(event);
  guint8 group = gdk_key_event_get_layout(event);
  g_autoptr(FlKeyEvent) fl_event = fl_key_event_new(event_time, TRUE, keycode, keyval, state, group, event);
  return handle_key_event(self, fl_event);
}

static gboolean key_released_cb(GtkEventControllerKey* controller,
                                guint keyval,
                                guint keycode,
                                GdkModifierType state,
                                gpointer user_data) {
  FlView* self = FL_VIEW(user_data);
  GdkEvent* event = gtk_event_controller_get_current_event(GTK_EVENT_CONTROLLER(controller));
  guint32 event_time = gdk_event_get_time(event);
  guint8 group = gdk_key_event_get_layout(event);
  g_autoptr(FlKeyEvent) fl_event = fl_key_event_new(event_time, FALSE, keycode, keyval, state, group, event);
  return handle_key_event(self, fl_event);
}

static void gesture_rotation_begin_cb(FlView* self) {
  fl_scrolling_manager_handle_rotation_begin(self->scrolling_manager);
}

static void gesture_rotation_update_cb(FlView* self,
                                       gdouble rotation) {
  fl_scrolling_manager_handle_rotation_update(self->scrolling_manager,
                                              rotation);
}

static void gesture_rotation_end_cb(FlView* self) {
  fl_scrolling_manager_handle_rotation_end(self->scrolling_manager);
}

static void gesture_zoom_begin_cb(FlView* self) {
  fl_scrolling_manager_handle_zoom_begin(self->scrolling_manager);
}

static void gesture_zoom_update_cb(FlView* self, gdouble scale) {
  fl_scrolling_manager_handle_zoom_update(self->scrolling_manager, scale);
}

static void gesture_zoom_end_cb(FlView* self) {
  fl_scrolling_manager_handle_zoom_end(self->scrolling_manager);
}

static void setup_opengl(FlView* self) {
  g_autoptr(GError) error = nullptr;

  GtkNative* native = gtk_widget_get_native(GTK_WIDGET(self->render_area));
  GdkSurface* surface = gtk_native_get_surface(native);
  self->render_context = gdk_surface_create_gl_context(surface, &error);
  if (self->render_context == nullptr) {
    g_warning("Failed to create OpenGL context: %s", error->message);
    return;
  }

  if (!gdk_gl_context_realize(self->render_context, &error)) {
    g_warning("Failed to realize OpenGL context: %s", error->message);
    return;
  }

  // If using Wayland, then EGL is in use and we can access the frame
  // from the Flutter context using EGLImage. If not (i.e. X11 using GLX)
  // then we have to copy the texture via the CPU.
  gboolean shareable = FALSE;
#ifdef GDK_WINDOWING_WAYLAND
  shareable = GDK_IS_WAYLAND_DISPLAY(gtk_widget_get_display(GTK_WIDGET(self)));
#endif
  self->compositor = FL_COMPOSITOR(fl_compositor_opengl_new(
      fl_engine_get_task_runner(self->engine),
      fl_engine_get_opengl_manager(self->engine), shareable));
}

static void setup_software(FlView* self) {
  self->compositor = FL_COMPOSITOR(
      fl_compositor_software_new(fl_engine_get_task_runner(self->engine)));
}

static void realize_cb(FlView* self) {
  switch (fl_engine_get_renderer_type(self->engine)) {
    case kOpenGL:
      setup_opengl(self);
      break;
    case kSoftware:
      setup_software(self);
      break;
    default:
      break;
  }

  if (self->view_id != flutter::kFlutterImplicitViewId) {
    setup_cursor(self);
    return;
  }

  GtkWidget* toplevel_window = GTK_WIDGET(gtk_widget_get_root(GTK_WIDGET(self)));

  self->window_state_monitor =
      fl_window_state_monitor_new(fl_engine_get_binary_messenger(self->engine),
                                  GTK_WINDOW(toplevel_window));

  // Handle requests by the user to close the application.
  g_signal_connect_swapped(toplevel_window, "close-request",
                           G_CALLBACK(window_delete_event_cb), self);

  // Flutter engine will need to make the context current from raster thread
  // during initialization.
  fl_opengl_manager_clear_current(fl_engine_get_opengl_manager(self->engine));

  g_autoptr(GError) error = nullptr;
  if (!fl_engine_start(self->engine, &error)) {
    g_warning("Failed to start Flutter engine: %s", error->message);
    return;
  }

  setup_cursor(self);

  handle_geometry_changed(self);
}

static void size_allocate_cb(FlView* self, int width, int height) {
  handle_geometry_changed(self);
}

static void paint_background(FlView* self, cairo_t* cr) {
  // Don't bother drawing if fully transparent - the widget above this will
  // already be drawn by GTK.
  if (self->background_color->red == 0 && self->background_color->green == 0 &&
      self->background_color->blue == 0 && self->background_color->alpha == 0) {
    return;
  }

  gdk_cairo_set_source_rgba(cr, self->background_color);
  cairo_paint(cr);
}

static void draw_cb(GtkDrawingArea* drawing_area,
                    cairo_t* cr,
                    int width,
                    int height,
                    gpointer user_data) {
  FlView* self = FL_VIEW(user_data);
  paint_background(self, cr);

  if (self->render_context) {
    gdk_gl_context_make_current(self->render_context);
  }

  gboolean wait_for_frame = !self->sized_to_content;
  GtkNative* native = gtk_widget_get_native(GTK_WIDGET(self->render_area));
  GdkSurface* surface = gtk_native_get_surface(native);
  fl_compositor_render(self->compositor, cr, surface, wait_for_frame);

  if (self->render_context) {
    gdk_gl_context_clear_current();
  }
}

static void fl_view_notify(GObject* object, GParamSpec* pspec) {
  FlView* self = FL_VIEW(object);

  if (strcmp(pspec->name, "scale-factor") == 0) {
    handle_geometry_changed(self);
  }

  if (G_OBJECT_CLASS(fl_view_parent_class)->notify != nullptr) {
    G_OBJECT_CLASS(fl_view_parent_class)->notify(object, pspec);
  }
}

static void fl_view_dispose(GObject* object) {
  FlView* self = FL_VIEW(object);

  g_cancellable_cancel(self->cancellable);

  if (self->engine != nullptr) {
    FlMouseCursorHandler* handler =
        fl_engine_get_mouse_cursor_handler(self->engine);
    if (self->cursor_changed_cb_id != 0) {
      g_signal_handler_disconnect(handler, self->cursor_changed_cb_id);
      self->cursor_changed_cb_id = 0;
    }

    // Release the view ID from the engine.
    fl_engine_remove_view(self->engine, self->view_id, nullptr, nullptr,
                          nullptr);
  }

  if (self->on_pre_engine_restart_cb_id != 0) {
    g_signal_handler_disconnect(self->engine,
                                self->on_pre_engine_restart_cb_id);
    self->on_pre_engine_restart_cb_id = 0;
  }

  if (self->update_semantics_cb_id != 0) {
    g_signal_handler_disconnect(self->engine, self->update_semantics_cb_id);
    self->update_semantics_cb_id = 0;
  }

  g_clear_object(&self->render_context);
  g_clear_object(&self->engine);
  g_clear_object(&self->compositor);
  g_clear_pointer(&self->background_color, gdk_rgba_free);
  g_clear_object(&self->window_state_monitor);
  g_clear_object(&self->scrolling_manager);
  g_clear_object(&self->pointer_manager);
  g_clear_object(&self->touch_manager);
  g_clear_object(&self->view_accessible);
  g_clear_object(&self->cancellable);

  G_OBJECT_CLASS(fl_view_parent_class)->dispose(object);
}

// Implements GtkWidget::realize.
static void fl_view_realize(GtkWidget* widget) {
  FlView* self = FL_VIEW(widget);

  GTK_WIDGET_CLASS(fl_view_parent_class)->realize(widget);

  // Realize the child widgets.
  gtk_widget_realize(GTK_WIDGET(self->render_area));
}

static gboolean handle_key_event(FlView* self, FlKeyEvent* event) {
  fl_keyboard_manager_handle_event(
      fl_engine_get_keyboard_manager(self->engine), FL_KEY_EVENT(g_object_ref(event)), self->cancellable,
      [](GObject* object, GAsyncResult* result, gpointer user_data) {
        FlView* self = FL_VIEW(user_data);

        g_autoptr(FlKeyEvent) redispatch_event = nullptr;
        g_autoptr(GError) error = nullptr;
        if (!fl_keyboard_manager_handle_event_finish(
                FL_KEYBOARD_MANAGER(object), result, &redispatch_event,
                &error)) {
          if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
            return;
          }

          g_warning("Failed to handle key event: %s", error->message);
        }

        if (redispatch_event != nullptr) {
          if (!fl_text_input_handler_filter_keypress(
                  fl_engine_get_text_input_handler(self->engine),
                  redispatch_event)) {
            fl_keyboard_manager_add_redispatched_event(
                fl_engine_get_keyboard_manager(self->engine), redispatch_event);
          }
        }
      },
      self);

  return TRUE;
}

static void fl_view_class_init(FlViewClass* klass) {
  GObjectClass* object_class = G_OBJECT_CLASS(klass);
  object_class->notify = fl_view_notify;
  object_class->dispose = fl_view_dispose;

  GtkWidgetClass* widget_class = GTK_WIDGET_CLASS(klass);
  widget_class->realize = fl_view_realize;

  fl_view_signals[SIGNAL_FIRST_FRAME] =
      g_signal_new("first-frame", fl_view_get_type(), G_SIGNAL_RUN_LAST, 0,
                   NULL, NULL, NULL, G_TYPE_NONE, 0);
}

// Engine related construction.
static void setup_engine(FlView* self) {
  self->view_accessible = nullptr;

  self->pointer_manager = fl_pointer_manager_new(self->view_id, self->engine);

  init_scrolling(self);
  init_touch(self);

  self->on_pre_engine_restart_cb_id =
      g_signal_connect_swapped(self->engine, "on-pre-engine-restart",
                               G_CALLBACK(on_pre_engine_restart_cb), self);
}

static void fl_view_init(FlView* self) {
  self->cancellable = g_cancellable_new();

  gtk_widget_set_can_focus(GTK_WIDGET(self), TRUE);

  self->view_id = -1;

  GdkRGBA default_background = {
      .red = 0.0, .green = 0.0, .blue = 0.0, .alpha = 1.0};
  self->background_color = gdk_rgba_copy(&default_background);

  self->render_area = GTK_DRAWING_AREA(gtk_drawing_area_new());
  gtk_widget_set_hexpand(GTK_WIDGET(self->render_area), TRUE);
  gtk_widget_set_vexpand(GTK_WIDGET(self->render_area), TRUE);
  gtk_box_append(GTK_BOX(self), GTK_WIDGET(self->render_area));

  g_signal_connect_swapped(self->render_area, "realize", G_CALLBACK(realize_cb),
                           self);
  g_signal_connect_swapped(self->render_area, "resize",
                           G_CALLBACK(size_allocate_cb), self);
  gtk_drawing_area_set_draw_func(self->render_area, draw_cb, self, nullptr);

  // Set up event controllers
  GtkGesture* click = gtk_gesture_click_new();
  gtk_widget_add_controller(GTK_WIDGET(self->render_area), GTK_EVENT_CONTROLLER(click));
  g_signal_connect(click, "pressed", G_CALLBACK(button_pressed_cb), self);
  g_signal_connect(click, "released", G_CALLBACK(button_released_cb), self);

  GtkEventController* motion = gtk_event_controller_motion_new();
  gtk_widget_add_controller(GTK_WIDGET(self->render_area), motion);
  g_signal_connect(motion, "motion", G_CALLBACK(motion_cb), self);
  g_signal_connect(motion, "enter", G_CALLBACK(enter_cb), self);
  g_signal_connect(motion, "leave", G_CALLBACK(leave_cb), self);

  GtkEventController* scroll = gtk_event_controller_scroll_new(
      static_cast<GtkEventControllerScrollFlags>(
          GTK_EVENT_CONTROLLER_SCROLL_BOTH_AXES | GTK_EVENT_CONTROLLER_SCROLL_DISCRETE));
  gtk_widget_add_controller(GTK_WIDGET(self->render_area), scroll);
  g_signal_connect(scroll, "scroll", G_CALLBACK(scroll_cb), self);

  GtkEventController* focus = gtk_event_controller_focus_new();
  gtk_widget_add_controller(GTK_WIDGET(self), focus);
  g_signal_connect(focus, "enter", G_CALLBACK(focus_enter_cb), self);

  GtkEventController* key = gtk_event_controller_key_new();
  gtk_widget_add_controller(GTK_WIDGET(self), key);
  g_signal_connect(key, "key-pressed", G_CALLBACK(key_pressed_cb), self);
  g_signal_connect(key, "key-released", G_CALLBACK(key_released_cb), self);

  GtkGesture* zoom = gtk_gesture_zoom_new();
  gtk_widget_add_controller(GTK_WIDGET(self->render_area), GTK_EVENT_CONTROLLER(zoom));
  g_signal_connect_swapped(zoom, "begin", G_CALLBACK(gesture_zoom_begin_cb), self);
  g_signal_connect_swapped(zoom, "scale-changed", G_CALLBACK(gesture_zoom_update_cb), self);
  g_signal_connect_swapped(zoom, "end", G_CALLBACK(gesture_zoom_end_cb), self);

  GtkGesture* rotate = gtk_gesture_rotate_new();
  gtk_widget_add_controller(GTK_WIDGET(self->render_area), GTK_EVENT_CONTROLLER(rotate));
  g_signal_connect_swapped(rotate, "begin", G_CALLBACK(gesture_rotation_begin_cb), self);
  g_signal_connect_swapped(rotate, "angle-changed", G_CALLBACK(gesture_rotation_update_cb), self);
  g_signal_connect_swapped(rotate, "end", G_CALLBACK(gesture_rotation_end_cb), self);
}

G_MODULE_EXPORT FlView* fl_view_new(FlDartProject* project) {
  g_autoptr(FlEngine) engine = fl_engine_new(project);
  FlView* self = FL_VIEW(g_object_new(fl_view_get_type(), nullptr));

  self->view_id = flutter::kFlutterImplicitViewId;
  self->engine = FL_ENGINE(g_object_ref(engine));

  setup_engine(self);

  fl_engine_set_implicit_view(engine, FL_RENDERABLE(self));

  return self;
}

G_MODULE_EXPORT FlView* fl_view_new_for_engine(FlEngine* engine) {
  FlView* self = FL_VIEW(g_object_new(fl_view_get_type(), nullptr));

  self->engine = FL_ENGINE(g_object_ref(engine));

  size_t min_width = 1, min_height = 1, max_width = 1, max_height = 1;
  gint scale_factor = gtk_widget_get_scale_factor(GTK_WIDGET(self));
  self->view_id = fl_engine_add_view(
      engine, FL_RENDERABLE(self), min_width, min_height, max_width, max_height,
      scale_factor, self->cancellable, view_added_cb, self);

  setup_engine(self);

  return self;
}

G_MODULE_EXPORT FlView* fl_view_new_sized_to_content(FlEngine* engine) {
  FlView* self = FL_VIEW(g_object_new(fl_view_get_type(), nullptr));

  self->engine = FL_ENGINE(g_object_ref(engine));

  self->sized_to_content = TRUE;
  size_t min_width = 1, min_height = 1, max_width = G_MAXSIZE,
         max_height = G_MAXSIZE;
  gint scale_factor = gtk_widget_get_scale_factor(GTK_WIDGET(self));
  self->view_id = fl_engine_add_view(
      engine, FL_RENDERABLE(self), min_width, min_height, max_width, max_height,
      scale_factor, self->cancellable, view_added_cb, self);

  setup_engine(self);

  return self;
}

G_MODULE_EXPORT FlEngine* fl_view_get_engine(FlView* self) {
  g_return_val_if_fail(FL_IS_VIEW(self), nullptr);
  return self->engine;
}

G_MODULE_EXPORT
int64_t fl_view_get_id(FlView* self) {
  g_return_val_if_fail(FL_IS_VIEW(self), -1);
  return self->view_id;
}

G_MODULE_EXPORT void fl_view_set_background_color(FlView* self,
                                                  const GdkRGBA* color) {
  g_return_if_fail(FL_IS_VIEW(self));
  gdk_rgba_free(self->background_color);
  self->background_color = gdk_rgba_copy(color);
}

FlViewAccessible* fl_view_get_accessible(FlView* self) {
  g_return_val_if_fail(FL_IS_VIEW(self), nullptr);
  return self->view_accessible;
}
