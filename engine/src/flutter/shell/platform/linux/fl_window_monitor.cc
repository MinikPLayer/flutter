// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <gtk/gtk.h>

#include "flutter/shell/platform/common/isolate_scope.h"
#include "flutter/shell/platform/linux/fl_window_monitor.h"

struct _FlWindowMonitor {
  GObject parent_instance;

  // Window being monitored.
  GtkWindow* window;

  // Isolate to call callbacks with.
  flutter::Isolate isolate;

  // Callbacks.
  void (*on_configure)(void);
  void (*on_state_changed)(void);
  void (*on_is_active_notify)(void);
  void (*on_title_notify)(void);
  void (*on_close)(void);
  void (*on_destroy)(void);
};

G_DEFINE_TYPE(FlWindowMonitor, fl_window_monitor, G_TYPE_OBJECT)

static void property_notify_cb(FlWindowMonitor* self) {
  flutter::IsolateScope scope(self->isolate);
  self->on_configure();
}

static void state_notify_cb(FlWindowMonitor* self) {
  flutter::IsolateScope scope(self->isolate);
  self->on_state_changed();
}

static void is_active_notify_cb(FlWindowMonitor* self) {
  flutter::IsolateScope scope(self->isolate);
  self->on_is_active_notify();
}

static void title_notify_cb(FlWindowMonitor* self) {
  flutter::IsolateScope scope(self->isolate);
  self->on_title_notify();
}

static gboolean delete_event_cb(FlWindowMonitor* self) {
  flutter::IsolateScope scope(self->isolate);
  self->on_close();

  // Stop default behaviour of destroying the window.
  return TRUE;
}

static void destroy_cb(FlWindowMonitor* self) {
  flutter::IsolateScope scope(self->isolate);
  self->on_destroy();
}

static void fl_window_monitor_dispose(GObject* object) {
  FlWindowMonitor* self = FL_WINDOW_MONITOR(object);

  // Disconnect all handlers using data. If we try and disconnect them
  // individually they generated warnings after the widget has been destroyed.
  g_signal_handlers_disconnect_by_data(self->window, self);
  g_clear_object(&self->window);

  G_OBJECT_CLASS(fl_window_monitor_parent_class)->dispose(object);
}

static void fl_window_monitor_class_init(FlWindowMonitorClass* klass) {
  G_OBJECT_CLASS(klass)->dispose = fl_window_monitor_dispose;
}

static void fl_window_monitor_init(FlWindowMonitor* self) {}

G_MODULE_EXPORT FlWindowMonitor* fl_window_monitor_new(
    GtkWindow* window,
    void (*on_configure)(void),
    void (*on_state_changed)(void),
    void (*on_is_active_notify)(void),
    void (*on_title_notify)(void),
    void (*on_close)(void),
    void (*on_destroy)(void)) {
  FlWindowMonitor* self =
      FL_WINDOW_MONITOR(g_object_new(fl_window_monitor_get_type(), nullptr));

  self->window = GTK_WINDOW(g_object_ref(window));
  self->isolate = flutter::Isolate::Current();
  self->on_configure = on_configure;
  self->on_state_changed = on_state_changed;
  self->on_is_active_notify = on_is_active_notify;
  self->on_title_notify = on_title_notify;
  self->on_close = on_close;
  self->on_destroy = on_destroy;
  g_signal_connect_swapped(window, "notify::default-width",
                           G_CALLBACK(property_notify_cb), self);
  g_signal_connect_swapped(window, "notify::default-height",
                           G_CALLBACK(property_notify_cb), self);
  g_signal_connect_swapped(window, "notify::suspended",
                           G_CALLBACK(state_notify_cb), self);
  g_signal_connect_swapped(window, "notify::is-active",
                           G_CALLBACK(is_active_notify_cb), self);
  g_signal_connect_swapped(window, "notify::title", G_CALLBACK(title_notify_cb),
                           self);
  g_signal_connect_swapped(window, "close-request", G_CALLBACK(delete_event_cb),
                           self);
  g_signal_connect_swapped(window, "destroy", G_CALLBACK(destroy_cb), self);

  return self;
}
