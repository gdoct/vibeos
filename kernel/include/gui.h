#ifndef VIBEOS_GUI_H
#define VIBEOS_GUI_H

#ifdef __cplusplus
extern "C" {
#endif

/* Start the GUI (graphics/graphics.md): brings up the framebuffer compositor +
   window manager in a worker task, with a mouse pointer driven by the USB mouse.
   No-op if there is no framebuffer. Call after sched_init + usb_init. */
void gui_init(void);

#ifdef __cplusplus
}
#endif

#endif
