// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/shell/platform/linux/fl_window_state_monitor.h"

#include <gtk/gtk.h>

#include "flutter/shell/platform/linux/public/flutter_linux/fl_string_codec.h"

struct _FlWindowStateMonitor {
  GObject parent_instance;

  // Messenger to communicate with engine.
  FlBinaryMessenger* messenger;

  // Window being monitored.
  GtkWindow* window;
};

G_DEFINE_TYPE(FlWindowStateMonitor, fl_window_state_monitor, G_TYPE_OBJECT);

static void fl_window_state_monitor_dispose(GObject* object) {
  FlWindowStateMonitor* self = FL_WINDOW_STATE_MONITOR(object);

  g_clear_object(&self->messenger);

  G_OBJECT_CLASS(fl_window_state_monitor_parent_class)->dispose(object);
}

static void fl_window_state_monitor_class_init(
    FlWindowStateMonitorClass* klass) {
  G_OBJECT_CLASS(klass)->dispose = fl_window_state_monitor_dispose;
}

static void fl_window_state_monitor_init(FlWindowStateMonitor* self) {}

FlWindowStateMonitor* fl_window_state_monitor_new(FlBinaryMessenger* messenger,
                                                  GtkWindow* window) {
  FlWindowStateMonitor* self = FL_WINDOW_STATE_MONITOR(
      g_object_new(fl_window_state_monitor_get_type(), nullptr));
  self->messenger = FL_BINARY_MESSENGER(g_object_ref(messenger));
  self->window = window;

  return self;
}
