/*  RetroArch - A frontend for libretro.
 *  Copyright (C) 2010-2014 - Hans-Kristian Arntzen
 *  Copyright (C) 2011-2017 - Daniel De Matteis
 *  Copyright (C) 2011-2017 - Higor Euripedes
 *  Copyright (C)      2026 - Rob Loach
 *
 *  RetroArch is free software: you can redistribute it and/or modify it under the terms
 *  of the GNU General Public License as published by the Free Software Found-
 *  ation, either version 3 of the License, or (at your option) any later version.
 *
 *  RetroArch is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 *  without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 *  PURPOSE.  See the GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along with RetroArch.
 *  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdlib.h>
#include <string.h>
#include <math.h>

#include <retro_inline.h>
#include <string/stdstring.h>

#ifdef HAVE_CONFIG_H
#include "../../config.h"
#endif

#ifdef HAVE_MENU
#include "../../menu/menu_driver.h"
#endif

#include <SDL3/SDL.h>

#include "../common/sdl3_common.h"
#include "../font_driver.h"

#include "../../configuration.h"
#include "../../retroarch.h"
#include "../../verbosity.h"

/*
 * FORWARD DECLARATIONS
 */

static void sdl3_gfx_free(void *data);

static INLINE void sdl3_tex_zero(sdl3_tex_t *t)
{
   if (t->tex)
      SDL_DestroyTexture(t->tex);

   t->tex   = NULL;
   t->w     = t->h = 0;
   t->pitch = 0;
}

static void sdl3_init_font(sdl3_video_t *vid, const char *font_path,
      unsigned font_size)
{
   int r, g, b;
   unsigned i, count;
   uint32_t *rgba                 = NULL;
   const struct font_atlas *atlas = NULL;
   settings_t           *settings = config_get_ptr();
   bool video_font_enable         = settings->bools.video_font_enable;
   float msg_color_r              = settings->floats.video_msg_color_r;
   float msg_color_g              = settings->floats.video_msg_color_g;
   float msg_color_b              = settings->floats.video_msg_color_b;

   if (!video_font_enable)
      return;

   if (!font_renderer_create_default(
            &vid->font_driver, &vid->font_data,
            *font_path ? font_path : NULL, font_size))
   {
      RARCH_WARN("[SDL3] Could not initialize fonts.\n");
      return;
   }

   r           = msg_color_r * 255;
   g           = msg_color_g * 255;
   b           = msg_color_b * 255;

   r           = (r < 0) ? 0 : (r > 255 ? 255 : r);
   g           = (g < 0) ? 0 : (g > 255 ? 255 : g);
   b           = (b < 0) ? 0 : (b > 255 ? 255 : b);

   vid->font_r = r;
   vid->font_g = g;
   vid->font_b = b;

   atlas       = vid->font_driver->get_atlas(vid->font_data);

   /* SDL3 reworked the surface/palette API, so instead of wrapping the
    * 8-bit alpha atlas in a paletted surface (as the SDL2 driver does)
    * we expand it directly into an ARGB8888 buffer: white RGB with the
    * atlas coverage in the alpha channel.  The glyph colour is then
    * applied at render time via SDL_SetTextureColorMod. */
   count       = atlas->width * atlas->height;
   rgba        = (uint32_t*)malloc(count * sizeof(uint32_t));
   if (!rgba)
      return;

   for (i = 0; i < count; i++)
      rgba[i] = ((uint32_t)atlas->buffer[i] << 24) | 0x00ffffffu;

   vid->font.tex = SDL_CreateTexture(vid->renderer,
         SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC,
         atlas->width, atlas->height);

   if (vid->font.tex)
   {
      SDL_UpdateTexture(vid->font.tex, NULL, rgba,
            atlas->width * sizeof(uint32_t));
      vid->font.w      = atlas->width;
      vid->font.h      = atlas->height;
      vid->font.active = true;

      SDL_SetTextureBlendMode(vid->font.tex, SDL_BLENDMODE_BLEND);
   }
   else
      RARCH_WARN("[SDL3] Failed to initialize font texture: %s\n", SDL_GetError());

   free(rgba);
}

static void sdl3_render_msg(sdl3_video_t *vid, const char *msg)
{
   int delta_x, delta_y, x, y;
   unsigned width, height;
   settings_t *settings;
   float msg_pos_x, msg_pos_y;

   /* Legacy bitmap OSD font path - the yellow-text OSD output and the
    * set_osd_msg fallback. */
   if (!msg || !*msg || !vid->font_data || !vid->font.tex)
      return;

   delta_x   = 0;
   delta_y   = 0;
   width     = vid->vp.width;
   height    = vid->vp.height;
   settings  = config_get_ptr();
   msg_pos_x = settings->floats.video_msg_pos_x;
   msg_pos_y = settings->floats.video_msg_pos_y;
   x         = (int)(msg_pos_x * width);
   y         = (int)((1.0f - msg_pos_y) * height);

   SDL_SetTextureColorMod(vid->font.tex,
         vid->font_r, vid->font_g, vid->font_b);

   for (; *msg; msg++)
   {
      SDL_FRect src_rect, dst_rect;
      const struct font_glyph *gly =
         vid->font_driver->get_glyph(vid->font_data, (uint8_t)*msg);

      if (!gly)
         gly = vid->font_driver->get_glyph(vid->font_data, '?');

      if (!gly)
         continue;

      src_rect.x = (float)gly->atlas_offset_x;
      src_rect.y = (float)gly->atlas_offset_y;
      src_rect.w = (float)gly->width;
      src_rect.h = (float)gly->height;

      dst_rect.x = (float)(x + delta_x + gly->draw_offset_x);
      dst_rect.y = (float)(y + delta_y + gly->draw_offset_y);
      dst_rect.w = (float)gly->width;
      dst_rect.h = (float)gly->height;

      SDL_RenderTexture(vid->renderer, vid->font.tex, &src_rect, &dst_rect);

      delta_x += gly->advance_x;
      delta_y -= gly->advance_y;
   }
}

static void sdl3_init_renderer(sdl3_video_t *vid)
{
   /* SDL3 dropped the renderer flags argument; vsync is configured
    * separately via SDL_SetRenderVSync. */
   vid->renderer = SDL_CreateRenderer(vid->window, NULL);

   if (!vid->renderer)
   {
      RARCH_ERR("[SDL3] Failed to initialize renderer: %s.\n", SDL_GetError());
      return;
   }

   SDL_SetRenderVSync(vid->renderer, vid->video.vsync ? 1 : 0);
   SDL_SetRenderDrawColor(vid->renderer, 0, 0, 0, 255);
}

static void sdl3_refresh_renderer(sdl3_video_t *vid)
{
   SDL_Rect r;

   SDL_RenderClear(vid->renderer);

   r.x = vid->vp.x;
   r.y = vid->vp.y;
   r.w = (int)vid->vp.width;
   r.h = (int)vid->vp.height;

   SDL_SetRenderViewport(vid->renderer, &r);
}

static void sdl3_refresh_viewport(sdl3_video_t *vid)
{
   int win_w, win_h;

   SDL_GetWindowSize(vid->window, &win_w, &win_h);

   vid->vp.full_width  = win_w;
   vid->vp.full_height = win_h;
   video_driver_update_viewport(&vid->vp, false, vid->video.force_aspect, true);

   /* Tell the rest of the engine about our actual window dimensions
    * (see the equivalent sdl2_gfx note). */
   video_driver_set_output_size(win_w, win_h);

   vid->flags &= ~SDL3_FLAG_SHOULD_RESIZE;

   sdl3_refresh_renderer(vid);
}

static void sdl3_refresh_input_size(sdl3_video_t *vid, bool menu, bool rgb32,
      unsigned width, unsigned height, unsigned pitch)
{
   sdl3_tex_t *target = menu ? &vid->menu : &vid->frame;

   if (   !target->tex
       || target->w     != width
       || target->h     != height
       || target->rgb32 != rgb32
       || target->pitch != pitch)
   {
      SDL_PixelFormat format;

      sdl3_tex_zero(target);

      if (menu)
         format = rgb32 ? SDL_PIXELFORMAT_ARGB8888 : SDL_PIXELFORMAT_RGBA4444;
      else /* this assumes the frontend will convert 0RGB1555 to RGB565 */
         format = rgb32 ? SDL_PIXELFORMAT_ARGB8888 : SDL_PIXELFORMAT_RGB565;

      target->tex = SDL_CreateTexture(vid->renderer, format,
            SDL_TEXTUREACCESS_STREAMING, width, height);

      if (!target->tex)
      {
         RARCH_ERR("[SDL3] Failed to create %s texture: %s.\n",
               menu ? "menu" : "main", SDL_GetError());
         return;
      }

      SDL_SetTextureScaleMode(target->tex,
            (menu || !vid->video.smooth)
            ? SDL_SCALEMODE_NEAREST : SDL_SCALEMODE_LINEAR);

      if (menu)
         SDL_SetTextureBlendMode(target->tex, SDL_BLENDMODE_BLEND);

      target->w     = width;
      target->h     = height;
      target->pitch = pitch;
      target->rgb32 = rgb32;

      /* If target is menu, do not override 'active' state (this should
       * only be set by sdl3_poke_texture_enable()). */
      if (!menu)
         target->active = true;
   }
}

static void *sdl3_gfx_init(const video_info_t *video,
      input_driver_t **input, void **input_data)
{
   int i;
   unsigned flags;
   sdl3_video_t *vid                 = NULL;
   SDL_InitFlags sdl_subsystem_flags = SDL_WasInit(0);
   settings_t *settings              = config_get_ptr();

   /* Initialise graphics subsystem, if required. */
   if (sdl_subsystem_flags == 0)
   {
      if (!SDL_Init(SDL_INIT_VIDEO))
         return NULL;
   }
   else if ((sdl_subsystem_flags & SDL_INIT_VIDEO) == 0)
   {
      if (!SDL_InitSubSystem(SDL_INIT_VIDEO))
         return NULL;
   }

   vid = (sdl3_video_t*)calloc(1, sizeof(*vid));
   if (!vid)
      return NULL;

   RARCH_LOG("[SDL3] Available renderers (change with $SDL_RENDER_DRIVER):\n");
   for (i = 0; i < SDL_GetNumRenderDrivers(); ++i)
   {
      const char *name = SDL_GetRenderDriver(i);
      if (name)
         RARCH_LOG("[SDL3] \t%s\n", name);
   }

   if (!video->fullscreen)
      RARCH_LOG("[SDL3] Creating window @ %ux%u.\n", video->width, video->height);

   if (video->fullscreen)
      flags = SDL_WINDOW_FULLSCREEN;
   else
      flags = SDL_WINDOW_RESIZABLE;

   /* SDL3 dropped the window position arguments from SDL_CreateWindow;
    * the window manager places it (or use SDL_SetWindowPosition). */
   vid->window = SDL_CreateWindow("RetroArch",
         (int)video->width, (int)video->height, flags);

   if (!vid->window)
   {
      RARCH_ERR("[SDL3] Failed to init SDL window: %s.\n", SDL_GetError());
      goto error;
   }

   /* A NULL fullscreen mode requests borderless desktop fullscreen. */
   if (video->fullscreen && settings->bools.video_windowed_fullscreen)
      SDL_SetWindowFullscreenMode(vid->window, NULL);

   vid->video        = *video;
   vid->video.smooth = video->smooth;
   vid->flags       |= SDL3_FLAG_SHOULD_RESIZE;

   sdl3_tex_zero(&vid->frame);
   sdl3_tex_zero(&vid->menu);

   if (video->fullscreen)
      SDL_HideCursor();

   sdl3_init_renderer(vid);
   sdl3_init_font(vid,
         settings->paths.path_font,
         settings->floats.video_font_size);

   sdl3_refresh_viewport(vid);

   /* Publish the window handle so the SDL3 input driver can grab the
    * mouse without depending on the sdl3_video_t layout. */
   video_driver_display_userdata_set((uintptr_t)vid->window);

   *input      = NULL;
   *input_data = NULL;

   return vid;

error:
   sdl3_gfx_free(vid);
   return NULL;
}

static void sdl3_check_window(sdl3_video_t *vid)
{
   SDL_Event event;

   SDL_PumpEvents();
   while (SDL_PeepEvents(&event, 1,
            SDL_GETEVENT, SDL_EVENT_QUIT, SDL_EVENT_WINDOW_LAST) > 0)
   {
      switch (event.type)
      {
         case SDL_EVENT_QUIT:
            vid->flags |= SDL3_FLAG_QUITTING;
            break;
         case SDL_EVENT_WINDOW_RESIZED:
         case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
            vid->flags |= SDL3_FLAG_SHOULD_RESIZE;
            break;
         default:
            break;
      }
   }
}

static bool sdl3_gfx_frame(void *data, const void *frame, unsigned width,
      unsigned height, uint64_t frame_count,
      unsigned pitch, const char *msg, video_frame_info_t *video_info)
{
   char title[128];
   sdl3_video_t *vid  = (sdl3_video_t*)data;
#ifdef HAVE_MENU
   bool menu_is_alive = (video_info->menu_st_flags & MENU_ST_FLAG_ALIVE) ? true : false;
#endif

   if (vid->flags & SDL3_FLAG_SHOULD_RESIZE)
      sdl3_refresh_viewport(vid);

   if (frame)
   {
      SDL_RenderClear(vid->renderer);
      sdl3_refresh_input_size(vid, false, vid->video.rgb32, width, height, pitch);
      SDL_UpdateTexture(vid->frame.tex, NULL, frame, pitch);
   }

   if (vid->frame.tex)
      SDL_RenderTextureRotated(vid->renderer, vid->frame.tex,
            NULL, NULL, vid->rotation, NULL, SDL_FLIP_NONE);

#ifdef HAVE_MENU
   menu_driver_frame(menu_is_alive, video_info);
#endif

   if (vid->menu.active && vid->menu.tex)
      SDL_RenderTexture(vid->renderer, vid->menu.tex, NULL, NULL);

   if (msg)
      sdl3_render_msg(vid, msg);

   SDL_RenderPresent(vid->renderer);

   title[0] = '\0';

   video_driver_get_window_title(title, sizeof(title));

   if (title[0])
      SDL_SetWindowTitle(vid->window, title);

   return true;
}

static void sdl3_gfx_set_nonblock_state(void *data, bool toggle,
      bool adaptive_vsync_enabled, unsigned swap_interval)
{
   sdl3_video_t *vid = (sdl3_video_t*)data;
   vid->video.vsync  = !toggle;
   if (vid->renderer)
      SDL_SetRenderVSync(vid->renderer, vid->video.vsync ? 1 : 0);
   sdl3_refresh_renderer(vid);
}

static bool sdl3_gfx_alive(void *data)
{
   sdl3_video_t *vid = (sdl3_video_t*)data;
   sdl3_check_window(vid);
   if (vid->flags & SDL3_FLAG_QUITTING)
      return false;
   return true;
}

static bool sdl3_gfx_focus(void *data)
{
   sdl3_video_t *vid = (sdl3_video_t*)data;
   unsigned flags    = (SDL_WINDOW_INPUT_FOCUS | SDL_WINDOW_MOUSE_FOCUS);
   return (SDL_GetWindowFlags(vid->window) & flags) == flags;
}

static bool sdl3_gfx_suspend_screensaver(void *data, bool enable) { return false; }

/* TODO/FIXME - implement */
static bool sdl3_gfx_has_windowed(void *data) { return true; }

static void sdl3_gfx_free(void *data)
{
   sdl3_video_t *vid = (sdl3_video_t*)data;
   if (!vid)
      return;

   sdl3_tex_zero(&vid->frame);
   sdl3_tex_zero(&vid->menu);
   sdl3_tex_zero(&vid->font);

   if (vid->renderer)
      SDL_DestroyRenderer(vid->renderer);

   if (vid->window)
      SDL_DestroyWindow(vid->window);

   if (vid->font_data)
      vid->font_driver->free(vid->font_data);

   free(vid);
}

static void sdl3_gfx_set_rotation(void *data, unsigned rotation)
{
   sdl3_video_t *vid = (sdl3_video_t*)data;

   if (vid)
      vid->rotation = 270 * rotation;
}

static void sdl3_gfx_viewport_info(void *data, struct video_viewport *vp)
{
   sdl3_video_t *vid = (sdl3_video_t*)data;
   *vp = vid->vp;
}

static bool sdl3_gfx_read_viewport(void *data, uint8_t *buffer, bool is_idle)
{
   SDL_Surface *surf = NULL, *bgr24 = NULL;
   sdl3_video_t *vid = (sdl3_video_t*)data;

   if (!is_idle)
      video_driver_cached_frame();

   /* Unlike SDL2 (which reads the window surface) SDL3 exposes a
    * direct renderer readback that works with an accelerated
    * renderer. */
   surf = SDL_RenderReadPixels(vid->renderer, NULL);
   if (!surf)
   {
      RARCH_WARN("[SDL3] Failed to read viewport pixels: %s.\n", SDL_GetError());
      return false;
   }

   bgr24 = SDL_ConvertSurface(surf, SDL_PIXELFORMAT_BGR24);
   SDL_DestroySurface(surf);

   if (!bgr24)
   {
      RARCH_WARN("[SDL3] Failed to convert viewport data to BGR24: %s.\n", SDL_GetError());
      return false;
   }

   memcpy(buffer, bgr24->pixels, bgr24->h * bgr24->pitch);

   SDL_DestroySurface(bgr24);

   return true;
}

static void sdl3_poke_set_filtering(void *data, unsigned index, bool smooth, bool ctx_scaling)
{
   sdl3_video_t *vid = (sdl3_video_t*)data;
   vid->video.smooth = smooth;

   sdl3_tex_zero(&vid->frame);
}

static void sdl3_poke_set_aspect_ratio(void *data, unsigned aspect_ratio_idx)
{
   sdl3_video_t *vid = (sdl3_video_t*)data;

   if (!vid)
      return;

   vid->video.force_aspect = true;
   vid->flags             |= SDL3_FLAG_SHOULD_RESIZE;
}

static void sdl3_poke_apply_state_changes(void *data)
{
   sdl3_video_t *vid = (sdl3_video_t*)data;
   vid->flags       |= SDL3_FLAG_SHOULD_RESIZE;
}

static void sdl3_poke_set_texture_frame(void *data,
      const void *frame, bool rgb32,
      unsigned width, unsigned height, float alpha)
{
   if (frame)
   {
      sdl3_video_t *vid = (sdl3_video_t*)data;

      sdl3_refresh_input_size(vid, true, rgb32, width, height,
            width * (rgb32 ? 4 : 2));

      SDL_UpdateTexture(vid->menu.tex, NULL, frame, (int)vid->menu.pitch);
   }
}

static void sdl3_poke_texture_enable(void *data,
      bool enable, bool full_screen)
{
   sdl3_video_t *vid = (sdl3_video_t*)data;

   if (!vid)
      return;

   vid->menu.active = enable;
}

static void sdl3_poke_set_osd_msg(void *data, const char *msg, size_t msg_len,
      const struct font_params *params, void *font)
{
   sdl3_video_t *vid = (sdl3_video_t*)data;
   sdl3_render_msg(vid, msg);
}

static void sdl3_show_mouse(void *data, bool state)
{
   if (state)
      SDL_ShowCursor();
   else
      SDL_HideCursor();
}

static void sdl3_grab_mouse_toggle(void *data)
{
   sdl3_video_t *vid = (sdl3_video_t*)data;
   SDL_SetWindowMouseGrab(vid->window, !SDL_GetWindowMouseGrab(vid->window));
}

static uint32_t sdl3_get_flags(void *data) { return 0; }

static video_poke_interface_t sdl3_video_poke_interface = {
   sdl3_get_flags,
   NULL, /* load_texture - menus/widgets need SDL_RenderGeometry */
   NULL, /* unload_texture */
   NULL, /* set_video_mode */
   NULL, /* get_refresh_rate */
   sdl3_poke_set_filtering,
   NULL, /* get_video_output_size */
   NULL, /* get_video_output_prev */
   NULL, /* get_video_output_next */
   NULL, /* get_current_framebuffer */
   NULL, /* get_proc_address */
   sdl3_poke_set_aspect_ratio,
   sdl3_poke_apply_state_changes,
   sdl3_poke_set_texture_frame,
   sdl3_poke_texture_enable,
   sdl3_poke_set_osd_msg,
   sdl3_show_mouse,
   sdl3_grab_mouse_toggle,
   NULL, /* get_current_shader */
   NULL, /* get_current_software_framebuffer */
   NULL, /* get_hw_render_interface */
   NULL, /* set_hdr_menu_nits */
   NULL, /* set_hdr_paper_white_nits */
   NULL, /* set_hdr_expand_gamut */
   NULL, /* set_hdr_scanlines */
   NULL  /* set_hdr_subpixel_layout */
};

static void sdl3_gfx_poke_interface(void *data, const video_poke_interface_t **iface)
{
   (void)data;
   *iface = &sdl3_video_poke_interface;
}

static bool sdl3_gfx_set_shader(void *data,
      enum rarch_shader_type type, const char *path)
{
   (void)data;
   (void)type;
   (void)path;

   return false;
}

video_driver_t video_sdl3 = {
   sdl3_gfx_init,
   sdl3_gfx_frame,
   sdl3_gfx_set_nonblock_state,
   sdl3_gfx_alive,
   sdl3_gfx_focus,
   sdl3_gfx_suspend_screensaver,
   sdl3_gfx_has_windowed,
   sdl3_gfx_set_shader,
   sdl3_gfx_free,
   "sdl3",
   NULL, /* set_viewport */
   sdl3_gfx_set_rotation,
   sdl3_gfx_viewport_info,
   sdl3_gfx_read_viewport,
   NULL, /* read_frame_raw */
#ifdef HAVE_OVERLAY
   NULL, /* overlay_interface */
#endif
   sdl3_gfx_poke_interface,
   NULL, /* wrap_type_to_enum */
   NULL, /* shader_load_begin */
   NULL, /* shader_load_step */
#ifdef HAVE_GFX_WIDGETS
   NULL, /* gfx_widgets_enabled - OSD fallback for now */
#endif
   NULL  /* invalidate_hw_render_cache */
};
