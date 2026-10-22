/**
 * Ghost Recon 2 port - the options overlay (Dear ImGui), drawn by the window
 * thread over the game's picture. Delete or Home opens and closes it; while
 * it is open the game gets no input.
 */
#ifndef GR2_PORT_MENU_H
#define GR2_PORT_MENU_H

#include <stdint.h>
#include <SDL.h>
#include <volk.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    SDL_Window      *win;
    VkInstance       inst;
    VkPhysicalDevice phys;
    VkDevice         dev;
    uint32_t         qfam;
    VkQueue          queue;
    VkFormat         format;       /* the swapchain's */
    uint32_t         image_count;
    /* Window-thread actions the menu asks for. */
    void (*set_window_mode)(int mode);   /* PS_* */
    void (*set_vsync)(int on);
} PortMenuInit;

int  pmenu_init(const PortMenuInit *init);

/* Returns 1 when the event was the menu's (the game must not see it). */
int  pmenu_event(const SDL_Event *ev);

int  pmenu_is_open(void);

/* Record the overlay into cmd, rendering onto view (COLOR_ATTACHMENT_OPTIMAL)
 * of the given size. Does nothing while closed. */
void pmenu_draw(VkCommandBuffer cmd, VkImageView view, uint32_t w, uint32_t h);

/* The swapchain was recreated with this many images. */
void pmenu_set_image_count(uint32_t n);

#ifdef __cplusplus
}
#endif

#endif
