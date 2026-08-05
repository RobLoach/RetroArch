/*  RetroArch - A frontend for libretro.
 *  Copyright (C) 2010-2014 - Hans-Kristian Arntzen
 *  Copyright (C) 2011-2017 - Daniel De Matteis
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

/* SDL3 OpenGL context driver. Lets the gl/gl1/glcore video drivers -
 * and with them every GL hardware-render libretro core - run on an
 * SDL3 window. The video_sdl3 driver itself stays software-frame-only
 * (SDL_Renderer cannot hand a live GL context to a core); HW-render
 * cores go through video_driver = gl/glcore + this context driver. */

#ifdef HAVE_CONFIG_H
#include "../../config.h"
#endif

#ifdef HAVE_X11
#include <X11/Xlib.h>
#endif

#include <string/stdstring.h>

#include "../../configuration.h"
#include "../../gfx/video_defines.h"
#include "../../gfx/video_driver.h"
#include "../../verbosity.h"

#include <SDL3/SDL.h>

#include "../common/sdl3_common.h"

typedef struct gfx_ctx_sdl3_data
{
   SDL_Window    *win;
   SDL_GLContext  ctx;
   SDL_GLContext  shared_ctx;

   int  width;  /* in pixels */
   int  height;

   bool full;
   bool core_hw_context_enable;
   bool adaptive_vsync;
} gfx_ctx_sdl3_data_t;

/* bind_api runs before init (see video_context_driver_init), so there
 * is no driver instance to store these in yet - same static pattern as
 * sdl_gl_ctx.c / x_ctx.c. The SDL_GL attributes themselves are applied
 * in set_video_mode, after the video subsystem is initialized. */
static enum gfx_ctx_api sdl3_gl_api   = GFX_CTX_OPENGL_API;
static unsigned         sdl3_gl_major = 0;
static unsigned         sdl3_gl_minor = 0;

/* Cached-context support (hw_render.cache_context, used by cores like
 * citra): across a video reinit the GL context - and with it the
 * core's GL state - must survive. destroy stashes the handles here and
 * the next set_video_mode re-adopts them, acking the reuse (same
 * pattern as wgl_ctx's static win32_hrc). While a context is stashed,
 * one SDL video-subsystem reference is deliberately kept: letting the
 * refcount hit zero would tear the context down with the subsystem. */
static SDL_GLContext    sdl3_gl_cached_ctx        = NULL;
static SDL_GLContext    sdl3_gl_cached_shared_ctx = NULL;
static bool             sdl3_gl_kept_video_alive  = false;

static void sdl3_ctx_destroy_resources(gfx_ctx_sdl3_data_t *sdl)
{
   video_driver_state_t *video_st = video_state_get_ptr();

   if (!sdl)
      return;

   if (sdl->ctx && (video_st->flags & VIDEO_FLAG_CACHE_CONTEXT))
   {
      /* hw_render.cache_context reinit: keep the context alive for
       * the next set_video_mode instead of destroying it. */
      SDL_GL_MakeCurrent(sdl->win, NULL);
      sdl3_gl_cached_ctx        = sdl->ctx;
      sdl3_gl_cached_shared_ctx = sdl->shared_ctx;
   }
   else
   {
      if (sdl->ctx)
         SDL_GL_DestroyContext(sdl->ctx);

      if (sdl->shared_ctx)
         SDL_GL_DestroyContext(sdl->shared_ctx);
   }

   sdl->ctx        = NULL;
   sdl->shared_ctx = NULL;

   if (sdl->win)
   {
      SDL_StopTextInput(sdl->win);
      SDL_DestroyWindow(sdl->win);
   }
   sdl->win        = NULL;
}

static void sdl3_ctx_destroy(void *data)
{
   gfx_ctx_sdl3_data_t *sdl = (gfx_ctx_sdl3_data_t*)data;

   if (!sdl)
      return;

   sdl3_ctx_destroy_resources(sdl);

   if (sdl3_gl_cached_ctx)
   {
      /* A cached context survives in the statics - hold on to our
       * video-subsystem reference so it stays valid until the next
       * init adopts it. */
      sdl3_gl_kept_video_alive = true;
   }
   else
   {
      /* SDL3 refcounts subsystems; this balances the
       * SDL_InitSubSystem in sdl3_ctx_init. */
      SDL_QuitSubSystem(SDL_INIT_VIDEO);
   }
   free(sdl);
}

static void *sdl3_ctx_init(void *video_driver)
{
   gfx_ctx_sdl3_data_t *sdl = (gfx_ctx_sdl3_data_t*)
      calloc(1, sizeof(gfx_ctx_sdl3_data_t));

   if (!sdl)
      return NULL;

#ifdef HAVE_X11
   XInitThreads();
#endif

   if (sdl3_gl_kept_video_alive)
   {
      /* Adopt the subsystem reference the previous instance kept to
       * protect its cached GL context. */
      sdl3_gl_kept_video_alive = false;
   }
   else if (!SDL_InitSubSystem(SDL_INIT_VIDEO))
   {
      RARCH_WARN("[SDL3 GL] Failed to initialize SDL video subsystem: %s.\n",
            SDL_GetError());
      free(sdl);
      return NULL;
   }

   RARCH_LOG("[SDL3 GL] SDL %d.%d.%d gfx context driver initialized.\n",
         SDL_MAJOR_VERSION, SDL_MINOR_VERSION, SDL_MICRO_VERSION);

   return sdl;
}

static enum gfx_ctx_api sdl3_ctx_get_api(void *data) { return sdl3_gl_api; }

static bool sdl3_ctx_bind_api(void *data,
      enum gfx_ctx_api api, unsigned major,
      unsigned minor)
{
   if (api != GFX_CTX_OPENGL_API && api != GFX_CTX_OPENGL_ES_API)
      return false;

   sdl3_gl_api   = api;
   sdl3_gl_major = major;
   sdl3_gl_minor = minor;

   return true;
}

static void sdl3_ctx_swap_interval(void *data, int interval)
{
   /* Adaptive vsync (-1) and multi-frame intervals aren't supported
    * everywhere; fall back to a plain 1-frame interval rather than
    * silently keeping the previous setting. */
   if (!SDL_GL_SetSwapInterval(interval) && interval != 0)
      SDL_GL_SetSwapInterval(1);
}

static bool sdl3_ctx_set_video_mode(void *data,
      unsigned width, unsigned height,
      bool fullscreen)
{
   gfx_ctx_sdl3_data_t *sdl     = (gfx_ctx_sdl3_data_t*)data;
   settings_t *settings         = config_get_ptr();
   bool windowed_fullscreen     = settings->bools.video_windowed_fullscreen;
   unsigned video_monitor_index = settings->uints.video_monitor_index;
   unsigned version             = sdl3_gl_major * 1000 + sdl3_gl_minor;

   if (!sdl)
      return false;

   if (sdl->win)
   {
      SDL_SetWindowSize(sdl->win, width, height);
      SDL_SetWindowFullscreen(sdl->win, fullscreen);
   }
   else
   {
      SDL_WindowFlags flags = SDL_WINDOW_OPENGL
            | SDL_WINDOW_RESIZABLE
            | SDL_WINDOW_HIGH_PIXEL_DENSITY;

      if (fullscreen)
         flags |= SDL_WINDOW_FULLSCREEN;

      /* GL attributes must be set before the window/context are
       * created. Profile selection mirrors x_ctx: an explicit GL 3.1+
       * request (glcore, RETRO_HW_CONTEXT_OPENGL_CORE cores) gets a
       * core profile; anything else gets compatibility, which is also
       * SDL's pre-3.2 default. */
      if (sdl3_gl_api == GFX_CTX_OPENGL_ES_API)
         SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,
               SDL_GL_CONTEXT_PROFILE_ES);
      else if (sdl->core_hw_context_enable || version >= 3001)
         SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,
               SDL_GL_CONTEXT_PROFILE_CORE);
      else
         SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,
               SDL_GL_CONTEXT_PROFILE_COMPATIBILITY);

      if (sdl3_gl_major > 0)
      {
         SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, sdl3_gl_major);
         SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, sdl3_gl_minor);
      }

      sdl->win = SDL_CreateWindow("RetroArch", width, height, flags);
      if (!sdl->win)
         goto error;

      if (video_monitor_index)
      {
         int count               = 0;
         SDL_DisplayID *displays = SDL_GetDisplays(&count);
         if (displays && (int)video_monitor_index <= count)
         {
            SDL_DisplayID id     = displays[video_monitor_index - 1];
            SDL_SetWindowPosition(sdl->win,
                  SDL_WINDOWPOS_CENTERED_DISPLAY(id),
                  SDL_WINDOWPOS_CENTERED_DISPLAY(id));
         }
         SDL_free(displays);
      }

      /* SDL3 only emits SDL_EVENT_TEXT_INPUT for windows that opted
       * in; the SDL3 input driver forwards those events for menu text
       * entry and core keyboard callbacks. */
      SDL_StartTextInput(sdl->win);
   }

   if (fullscreen)
   {
      /* SDL3 fullscreen windows are borderless (desktop) by default;
       * an exclusive video mode has to be requested explicitly. */
      if (!windowed_fullscreen)
      {
         SDL_DisplayMode mode;
         if (SDL_GetClosestFullscreenDisplayMode(
                  SDL_GetDisplayForWindow(sdl->win),
                  width, height, 0.0f, false, &mode))
            SDL_SetWindowFullscreenMode(sdl->win, &mode);
      }
      SDL_HideCursor();
   }

   sdl3_set_handles(sdl->win);

   /* Re-adopt a context stashed across a video reinit
    * (hw_render.cache_context). */
   if (!sdl->ctx && sdl3_gl_cached_ctx)
   {
      sdl->ctx                  = sdl3_gl_cached_ctx;
      sdl->shared_ctx           = sdl3_gl_cached_shared_ctx;
      sdl3_gl_cached_ctx        = NULL;
      sdl3_gl_cached_shared_ctx = NULL;
   }

   if (sdl->ctx)
   {
      /* Bind the surviving context to the new window. */
      if (!SDL_GL_MakeCurrent(sdl->win, sdl->ctx))
         goto error;
      video_driver_cache_context_ack_set();
      RARCH_LOG("[SDL3 GL] Using cached GL context.\n");
   }
   else
   {
      if (!(sdl->ctx = SDL_GL_CreateContext(sdl->win)))
         goto error;

      /* Probe adaptive vsync support once, with a live context. */
      sdl->adaptive_vsync = SDL_GL_SetSwapInterval(-1);
      SDL_GL_SetSwapInterval(0);
   }

   sdl->full = fullscreen;
   SDL_GetWindowSizeInPixels(sdl->win, &sdl->width, &sdl->height);

   return true;

error:
   RARCH_WARN("[SDL3 GL] Failed to set video mode: %s.\n", SDL_GetError());
   return false;
}

static void sdl3_ctx_get_video_size(void *data,
      unsigned *width, unsigned *height)
{
   gfx_ctx_sdl3_data_t *sdl = (gfx_ctx_sdl3_data_t*)data;

   if (!sdl)
      return;

   if (sdl->win)
   {
      SDL_GetWindowSizeInPixels(sdl->win, &sdl->width, &sdl->height);
      *width  = sdl->width;
      *height = sdl->height;
   }
   else
   {
      /* No window yet - report the desktop mode so the frontend can
       * size fullscreen output. */
      const SDL_DisplayMode *mode =
            SDL_GetCurrentDisplayMode(SDL_GetPrimaryDisplay());
      if (mode)
      {
         *width  = mode->w;
         *height = mode->h;
      }
   }
}

static float sdl3_ctx_get_refresh_rate(void *data)
{
   gfx_ctx_sdl3_data_t *sdl    = (gfx_ctx_sdl3_data_t*)data;
   const SDL_DisplayMode *mode;

   if (!sdl || !sdl->win)
      return 0.0f;

   mode = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(sdl->win));
   return mode ? mode->refresh_rate : 0.0f;
}

static void sdl3_ctx_update_title(void *data)
{
   char title[128];
   gfx_ctx_sdl3_data_t *sdl = (gfx_ctx_sdl3_data_t*)data;

   title[0] = '\0';

   video_driver_get_window_title(title, sizeof(title));

   if (sdl && sdl->win && title[0])
      SDL_SetWindowTitle(sdl->win, title);
}

static void sdl3_ctx_check_window(void *data, bool *quit,
      bool *resize, unsigned *width, unsigned *height)
{
   gfx_ctx_sdl3_data_t *sdl = (gfx_ctx_sdl3_data_t*)data;

   sdl3_pump_window_events(quit, resize);

   if (*resize && sdl && sdl->win)
   {
      SDL_GetWindowSizeInPixels(sdl->win, &sdl->width, &sdl->height);
      *width  = sdl->width;
      *height = sdl->height;
   }
}

static bool sdl3_ctx_has_focus(void *data)
{
   gfx_ctx_sdl3_data_t *sdl = (gfx_ctx_sdl3_data_t*)data;
   SDL_WindowFlags flags    = (SDL_WINDOW_INPUT_FOCUS | SDL_WINDOW_MOUSE_FOCUS);
   if (!sdl || !sdl->win)
      return false;
   return (SDL_GetWindowFlags(sdl->win) & flags) == flags;
}

static bool sdl3_ctx_suppress_screensaver(void *data, bool enable)
{
   return enable ? SDL_DisableScreenSaver() : SDL_EnableScreenSaver();
}

static void sdl3_ctx_swap_buffers(void *data)
{
   gfx_ctx_sdl3_data_t *sdl = (gfx_ctx_sdl3_data_t*)data;
   if (sdl && sdl->win)
      SDL_GL_SwapWindow(sdl->win);
}

static void sdl3_ctx_input_driver(void *data,
      const char *name,
      input_driver_t **input, void **input_data)
{
   /* The frontend selects the input driver separately (sdl3 by
    * default on SDL3 builds). */
   *input      = NULL;
   *input_data = NULL;
}

static gfx_ctx_proc_t sdl3_ctx_get_proc_address(const char *name)
{
   return (gfx_ctx_proc_t)SDL_GL_GetProcAddress(name);
}

static void sdl3_ctx_show_mouse(void *data, bool state)
{
   if (state)
      SDL_ShowCursor();
   else
      SDL_HideCursor();
}

static uint32_t sdl3_ctx_get_flags(void *data)
{
   uint32_t flags           = 0;
   gfx_ctx_sdl3_data_t *sdl = (gfx_ctx_sdl3_data_t*)data;
   unsigned version         = sdl3_gl_major * 1000 + sdl3_gl_minor;

   if (sdl && sdl->adaptive_vsync)
      BIT32_SET(flags, GFX_CTX_FLAGS_ADAPTIVE_VSYNC);

   if ((sdl && sdl->core_hw_context_enable) || version >= 3001)
      BIT32_SET(flags, GFX_CTX_FLAGS_GL_CORE_CONTEXT);

   if (string_is_equal(video_driver_get_ident(), "glcore"))
   {
#if defined(HAVE_SLANG) && defined(HAVE_SPIRV_CROSS)
      BIT32_SET(flags, GFX_CTX_FLAGS_SHADERS_SLANG);
#endif
   }
   else
      BIT32_SET(flags, GFX_CTX_FLAGS_SHADERS_GLSL);

   return flags;
}

static void sdl3_ctx_set_flags(void *data, uint32_t flags)
{
   gfx_ctx_sdl3_data_t *sdl = (gfx_ctx_sdl3_data_t*)data;
   if (!sdl)
      return;
   if (BIT32_GET(flags, GFX_CTX_FLAGS_GL_CORE_CONTEXT))
      sdl->core_hw_context_enable = true;
}

/* HW-render cores draw on their own GL context, sharing objects with
 * the frontend's - this is what makes context_reset-based cores work. */
static void sdl3_ctx_bind_hw_render(void *data, bool enable)
{
   gfx_ctx_sdl3_data_t *sdl = (gfx_ctx_sdl3_data_t*)data;
   if (!sdl || !sdl->win || !sdl->ctx)
      return;

   if (enable)
   {
      if (!sdl->shared_ctx)
      {
         SDL_GL_SetAttribute(SDL_GL_SHARE_WITH_CURRENT_CONTEXT, 1);
         SDL_GL_MakeCurrent(sdl->win, sdl->ctx);
         if (!(sdl->shared_ctx = SDL_GL_CreateContext(sdl->win)))
         {
            RARCH_ERR("[SDL3 GL] Failed to create shared GL context: %s.\n",
                  SDL_GetError());
            return;
         }
      }
      SDL_GL_MakeCurrent(sdl->win, sdl->shared_ctx);
   }
   else
      SDL_GL_MakeCurrent(sdl->win, sdl->ctx);
}

const gfx_ctx_driver_t gfx_ctx_sdl3_gl =
{
   sdl3_ctx_init,
   sdl3_ctx_destroy,
   sdl3_ctx_get_api,
   sdl3_ctx_bind_api,
   sdl3_ctx_swap_interval,
   sdl3_ctx_set_video_mode,
   sdl3_ctx_get_video_size,
   sdl3_ctx_get_refresh_rate,
   NULL, /* get_video_output_size */
   NULL, /* get_video_output_prev */
   NULL, /* get_video_output_next */
   NULL, /* get_metrics */
   NULL, /* translate_aspect */
   sdl3_ctx_update_title,
   sdl3_ctx_check_window,
   NULL, /* set_resize */
   sdl3_ctx_has_focus,
   sdl3_ctx_suppress_screensaver,
   true, /* has_windowed */
   sdl3_ctx_swap_buffers,
   sdl3_ctx_input_driver,
   sdl3_ctx_get_proc_address,
   NULL, /* image_buffer_init */
   NULL, /* image_buffer_write */
   sdl3_ctx_show_mouse,
   "gl_sdl3",
   sdl3_ctx_get_flags,
   sdl3_ctx_set_flags,
   sdl3_ctx_bind_hw_render,
   NULL, /* get_context_data */
   NULL, /* make_current */
   NULL, /* create_surface */
   NULL  /* destroy_surface */
};
