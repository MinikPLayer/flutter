# Plan: 100% End-to-End GPU Acceleration in GTK4

This document outlines the architectural plan to transition the Flutter Linux shell from the current hybrid/Cairo-bound rendering model to a fully GPU-accelerated (zero-copy) rendering pipeline using GTK4's Scene Graph (GSK) and `GdkGLTexture`.

---

## 1. Architectural Overview

In the current port, the Flutter engine renders to the GPU via Skia, but presenting the final frame to the GTK window is bottlenecked by `cairo_t*` drawing. By migrating away from Cairo drawing area widgets to retained-mode GTK4 snapshot nodes, we can pass GL textures directly to the GTK4 renderer (`GskRenderer`).

### Legacy vs. Accelerated Pipeline Comparison

#### Legacy/Current Port (CPU Copy Bottleneck)
```
[Flutter/Skia] -> [GL Texture] -> [Compositor FBO (GPU)]
                                            |
                                  (gdk_cairo_draw_from_gl)
                                            v
[CPU Readback (glReadPixels)] -> [Cairo Image Surface (CPU)] -> [GskRenderer (GPU)]
```

#### Accelerated Port (Zero-Copy GPU Path)
```
[Flutter/Skia] -> [GL Texture] -> [Compositor FBO (GPU)]
                                            |
                                   (gdk_gl_texture_new)
                                            v
                                [GdkGLTexture Object (GPU)]
                                            |
                                  (GskTextureScaleNode)
                                            v
                                    [GskRenderer (GPU)]
```

---

## 2. Refactoring Steps

### Step 1: Subclass `GtkWidget` for `FlView`
In GTK4, custom drawing is performed by overriding the `snapshot` virtual method of a `GtkWidget` rather than listening to a `draw` signal of a `GtkDrawingArea`.
1. Modify `FlView` definition in `fl_view.cc` to subclass `GtkWidget` directly (or inherit from a container widget like `GtkBox`).
2. Remove `self->render_area` (`GtkDrawingArea`) and `self->event_box`.
3. Register the class with a custom `snapshot` implementation in `fl_view_class_init`:
   ```c
   GtkWidgetClass* widget_class = GTK_WIDGET_CLASS(klass);
   widget_class->snapshot = fl_view_snapshot;
   ```

---

### Step 2: Refactor `FlCompositor` Interface
Change the compositor interface from immediate-mode Cairo painting to a texture-retrieval model.
1. Update `fl_compositor.h` to replace the `render` method:
   ```diff
   - gboolean fl_compositor_render(FlCompositor* self, cairo_t* cr, GdkSurface* window, gboolean wait_for_frame);
   + GdkTexture* fl_compositor_get_texture(FlCompositor* self, gboolean wait_for_frame);
   ```

---

### Step 3: Implement `GdkGLTexture` in `FlCompositorOpenGL`
Instead of copying pixels or passing texture IDs to Cairo, wrap the OpenGL compositor's texture directly into a GTK4 `GdkTexture`.
1. Modify `fl_compositor_opengl.cc` to implement `fl_compositor_opengl_get_texture`:
   ```c
   static GdkTexture* fl_compositor_opengl_get_texture(FlCompositor* compositor, gboolean wait_for_frame) {
     FlCompositorOpenGL* self = FL_COMPOSITOR_OPENGL(compositor);
     g_mutex_lock(&self->frame_mutex);

     // [Wait for frame logic remains the same] ...

     // Create the GdkTexture wrapping the OpenGL texture ID
     GdkGLContext* context = gdk_gl_context_get_current();
     GdkTexture* texture = gdk_gl_texture_new(
         context,
         fl_framebuffer_get_texture_id(self->framebuffer),
         width,
         height,
         [](gpointer data) {
           // Release callback when GTK is done with the texture frame
         },
         self
     );

     g_mutex_unlock(&self->frame_mutex);
     return texture;
   }
   ```

---

### Step 4: Implement `snapshot` in `FlView`
Within the `snapshot` function, retrieve the frame texture from the compositor and append it directly as a render node to the GTK4 snapshot tree.
1. Implement the snapshot callback:
   ```c
   static void fl_view_snapshot(GtkWidget* widget, GtkSnapshot* snapshot) {
     FlView* self = FL_VIEW(widget);

     gboolean wait_for_frame = !self->sized_to_content;
     g_autoptr(GdkTexture) texture = fl_compositor_get_texture(self->compositor, wait_for_frame);

     if (texture == NULL) {
       return;
     }

     int width = gtk_widget_get_width(widget);
     int height = gtk_widget_get_height(widget);

     // Append a texture node directly to the retained-mode scene graph
     gsk_snapshot_append_texture(
         snapshot,
         texture,
         &GRAPHENE_RECT_INIT(0, 0, width, height)
     );
   }
   ```

---

### Step 5: Clean Up Rendering Fixes & Workarounds
Because end-to-end GPU rendering keeps the textures completely within OpenGL and GTK's native shaders, color formats are handled natively without formatting mismatches:
* **Remove Shaders Color-Swizzling**: Revert the fragment shader color-swizzling workaround. Standard RGBA textures will be read and displayed correctly by GSK's fragment shaders, which resolves the color channel mapping mismatch automatically.
* **Remove Viewport and Cairo Matrix Operations**: Remove Cairo scaling/translation matrices and manual `glViewport` overrides from the compositor, as GSK handles coordinates, viewport bounds, and coordinate scaling automatically during scene layout.

---

## 3. Milestones & Implementation Tasks

- [ ] **Milestone 1: View Subclassing**
  - Refactor `FlView` class declaration in `fl_view.cc` to inherit from `GtkWidget`.
  - Wire custom event controllers (`GtkEventController`) directly to `FlView`.
- [ ] **Milestone 2: Compositor API Update**
  - Update `FlCompositor` abstraction layer to return `GdkTexture*`.
  - Re-implement `FlCompositorSoftware` to generate a `GdkTexture` from raw pixel buffers via `gdk_memory_texture_new`.
- [ ] **Milestone 3: GdkGLTexture Integration**
  - Implement `gdk_gl_texture_new` frame packaging in `fl_compositor_opengl.cc`.
  - Set up synchronization locks between the Flutter rasterization thread and GTK main thread during texture destruction.
- [ ] **Milestone 4: Snapshot Wiring & Testing**
  - Implement `fl_view_snapshot` to construct GSK texture render nodes.
  - Test and verify execution under both X11 and Wayland display servers.
