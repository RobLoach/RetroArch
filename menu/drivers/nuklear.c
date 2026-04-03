/*  RetroArch - A frontend for libretro.
 *  Copyright (C) 2026 - RetroArch Team
 *
 *  RetroArch is free software: you can redistribute it and/or modify it under the terms
 *  of the GNU General Public License as published by the Free Software Foundation,
 *  either version 3 of the License, or (at your option) any later version.
 *
 *  RetroArch is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 *  without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 *  PURPOSE.  See the GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along with RetroArch.
 *  If not, see <http://www.gnu.org/licenses/>.
 */

/* Nuklear menu driver for RetroArch.
 * Uses nuklear_console for menu layout and widget rendering,
 * bridged to RetroArch's gfx_display abstraction layer. */

#ifdef HAVE_CONFIG_H
#include "../../config.h"
#endif

#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#include <lists/file_list.h>
#include <string/stdstring.h>
#include <file/file_path.h>
#include <compat/strl.h>

#include "../menu_driver.h"
#include "../menu_entries.h"
#include "../../gfx/gfx_display.h"
#include "../../gfx/font_driver.h"
#include "../../gfx/video_driver.h"
#include "../../configuration.h"
#include "../../file_path_special.h"

/* ── Nuklear ───────────────────────────────────────────────────────── */
#define NK_INCLUDE_FIXED_TYPES
#define NK_INCLUDE_STANDARD_IO
#define NK_INCLUDE_STANDARD_VARARGS
#define NK_INCLUDE_DEFAULT_ALLOCATOR
#define NK_IMPLEMENTATION
#include "../../deps/nuklear/nuklear.h"

/* nuklear_gamepad: use the "none" backend so RetroArch handles all input */
#define NK_GAMEPAD_NONE
#define NK_GAMEPAD_IMPLEMENTATION
#include "../../deps/nuklear_gamepad/nuklear_gamepad.h"

/* Override CVECTOR_H before nuklear_console.h picks it up.
 * Requires -I$(DEPS_DIR)/c-vector in CFLAGS (added by Makefile.common). */
#define CVECTOR_H "cvector.h"

/* nuklear_console */
#define NK_CONSOLE_IMPLEMENTATION
#include "../../deps/nuklear_console/nuklear_console.h"

/* ── Constants ─────────────────────────────────────────────────────── */

#define NUKLEAR_FONT_SIZE 14

/* ── Private state ─────────────────────────────────────────────────── */

typedef struct
{
   struct nk_context    ctx;            /* Nuklear immediate-mode context   */
   nk_console          *console;        /* nuklear_console root widget       */
   struct nk_user_font  user_font;      /* Nuklear font-width shim           */
   font_data_impl_t     font;           /* RetroArch font handle             */
   nk_console         **entry_widgets;  /* one pointer per visible entry     */
   size_t               entry_count;    /* length of entry_widgets           */
   bool                 menu_dirty;     /* widget tree needs rebuild         */
   unsigned             last_width;
   unsigned             last_height;
} nuklear_t;

/* ── Font-width shim ───────────────────────────────────────────────── */

static float nuklear_font_width(nk_handle handle,
      float height, const char *text, int len)
{
   font_data_impl_t *f = (font_data_impl_t*)handle.ptr;
   (void)height;
   if (!f || !f->font || !text || len <= 0)
      return 0.0f;
   return (float)font_driver_get_message_width(f->font,
         text, (size_t)len, 1.0f);
}

/* ── Color helpers ─────────────────────────────────────────────────── */

/* Convert nk_color to a float[16] (4 vertices × RGBA) for draw_quad. */
static void nk_color_to_float_arr(struct nk_color c, float out[16])
{
   float r = c.r / 255.0f;
   float g = c.g / 255.0f;
   float b = c.b / 255.0f;
   float a = c.a / 255.0f;
   int   i;
   for (i = 0; i < 4; i++)
   {
      out[i * 4 + 0] = r;
      out[i * 4 + 1] = g;
      out[i * 4 + 2] = b;
      out[i * 4 + 3] = a;
   }
}

/* Convert nk_color to the RRGGBBAA uint32 expected by draw_text. */
static uint32_t nk_color_to_u32(struct nk_color c)
{
   return ((uint32_t)c.r << 24)
        | ((uint32_t)c.g << 16)
        | ((uint32_t)c.b <<  8)
        |  (uint32_t)c.a;
}

/* ── Entry-widget list helpers ─────────────────────────────────────── */

static void nuklear_clear_entry_widgets(nuklear_t *nk)
{
   if (nk->entry_widgets)
      free(nk->entry_widgets);
   nk->entry_widgets = NULL;
   nk->entry_count   = 0;
}

static bool nuklear_append_widget(nuklear_t *nk, nk_console *w)
{
   nk_console **tmp = (nk_console**)realloc(nk->entry_widgets,
         (nk->entry_count + 1) * sizeof(nk_console*));
   if (!tmp)
      return false;
   nk->entry_widgets                    = tmp;
   nk->entry_widgets[nk->entry_count++] = w;
   return true;
}

/* ── Widget-tree construction ──────────────────────────────────────── */

static void nuklear_build_menu(nuklear_t *nk)
{
   char               title[NAME_MAX_LENGTH];
   struct menu_state *menu_st;
   menu_list_t       *menu_list;
   file_list_t       *sel_list;
   size_t             entries_end;
   size_t             i;

   /* Release previous tree */
   if (nk->console)
   {
      nk_console_free(nk->console);
      nk->console = NULL;
   }
   nuklear_clear_entry_widgets(nk);

   nk->console = nk_console_init(&nk->ctx);
   if (!nk->console)
      return;

   /* Non-selectable title label */
   menu_entries_get_title(title, sizeof(title));
   nk_console_label(nk->console, title);

   /* Build one widget per visible menu entry */
   menu_st     = menu_state_get_ptr();
   menu_list   = menu_st->entries.list;
   sel_list    = menu_list
         ? MENU_LIST_GET_SELECTION(menu_list, 0) : NULL;
   entries_end = sel_list ? sel_list->size : 0;

   for (i = 0; i < entries_end; i++)
   {
      menu_entry_t entry;
      nk_console   *w;

      MENU_ENTRY_INITIALIZE(entry);
      menu_entry_get(&entry, 0, (unsigned)i, NULL, true);

      if (entry.type == MENU_ENTRY_BOOL)
      {
         /* value string is "ON" or "OFF" */
         nk_bool checked = (entry.value[0] == 'O' && entry.value[1] == 'N');
         w = nk_console_checkbox(nk->console, entry.rich_label, &checked);
      }
      else
      {
         w = nk_console_button(nk->console, entry.rich_label);
      }

      nuklear_append_widget(nk, w);
   }

   nk->menu_dirty = false;
}

/* Sync nuklear_console's active widget to RetroArch's selection_ptr. */
static void nuklear_sync_selection(nuklear_t *nk)
{
   struct menu_state *menu_st = menu_state_get_ptr();
   size_t             sel     = menu_st->selection_ptr;
   if (nk->entry_widgets && sel < nk->entry_count)
      nk_console_set_active_widget(nk->entry_widgets[sel]);
}

/* ── Driver lifecycle ──────────────────────────────────────────────── */

static void *nuklear_init(void **userdata, bool video_is_threaded)
{
   menu_handle_t *menu;
   nuklear_t     *nk;

   (void)video_is_threaded;

   menu = (menu_handle_t*)calloc(1, sizeof(*menu));
   if (!menu)
      return NULL;

   nk = (nuklear_t*)calloc(1, sizeof(*nk));
   if (!nk)
   {
      free(menu);
      return NULL;
   }

   if (!nk_init_default(&nk->ctx, NULL))
   {
      free(nk);
      free(menu);
      return NULL;
   }

   nk->menu_dirty = true;
   *userdata       = nk;
   return menu;
}

static void nuklear_free(void *data)
{
   nuklear_t *nk = (nuklear_t*)data;
   if (!nk)
      return;

   if (nk->console)
   {
      nk_console_free(nk->console);
      nk->console = NULL;
   }
   nuklear_clear_entry_widgets(nk);

   if (nk->font.font)
   {
      font_driver_free(nk->font.font);
      nk->font.font = NULL;
   }

   nk_free(&nk->ctx);
   free(nk);
}

static void nuklear_context_reset(void *data, bool video_is_threaded)
{
   nuklear_t      *nk       = (nuklear_t*)data;
   gfx_display_t  *p_disp   = disp_get_ptr();
   settings_t     *settings  = config_get_ptr();
   char            fontpath[PATH_MAX_LENGTH];
   unsigned        width, height;
   const char     *dir_assets;

   if (!nk || !settings)
      return;

   dir_assets = settings->paths.directory_assets;

   /* Use the same font as Material UI (guaranteed when HW menus are built) */
   fill_pathname_join_special(fontpath, dir_assets, "glui", sizeof(fontpath));
   fill_pathname_join_special(fontpath, fontpath,
         FILE_PATH_TTF_FONT, sizeof(fontpath));

   /* (Re)load font */
   if (nk->font.font)
   {
      font_driver_free(nk->font.font);
      nk->font.font = NULL;
   }
   nk->font.font = gfx_display_font_file(p_disp, fontpath,
         NUKLEAR_FONT_SIZE, video_is_threaded);

   /* Install font width shim so Nuklear can measure text for layout */
   if (nk->font.font)
   {
      nk->user_font.userdata = nk_handle_ptr(&nk->font);
      nk->user_font.height   = (float)NUKLEAR_FONT_SIZE;
      nk->user_font.width    = nuklear_font_width;
      nk_style_set_font(&nk->ctx, &nk->user_font);
   }

   /* White texture used for filled-rect draws */
   gfx_display_deinit_white_texture();
   gfx_display_init_white_texture();

   video_driver_get_size(&width, &height);
   nk->last_width  = width;
   nk->last_height = height;

   nk->menu_dirty  = true;
}

static void nuklear_context_destroy(void *data)
{
   nuklear_t *nk = (nuklear_t*)data;
   if (!nk)
      return;
   if (nk->font.font)
   {
      font_driver_free(nk->font.font);
      nk->font.font = NULL;
   }
   gfx_display_deinit_white_texture();
}

static void nuklear_render(void *data,
      unsigned width, unsigned height, bool is_idle)
{
   nuklear_t *nk = (nuklear_t*)data;
   (void)is_idle;
   if (!nk)
      return;
   if (width != nk->last_width || height != nk->last_height)
   {
      nk->last_width  = width;
      nk->last_height = height;
      nk->menu_dirty  = true;
   }
}

static void nuklear_frame(void *data, video_frame_info_t *video_info)
{
   const struct nk_command *cmd;
   nuklear_t               *nk;
   gfx_display_t           *p_disp;
   char                     title[NAME_MAX_LENGTH];
   void                    *userdata;
   unsigned                 video_width;
   unsigned                 video_height;

   nk = (nuklear_t*)data;
   if (!nk || !nk->font.font)
      return;

   p_disp       = disp_get_ptr();
   userdata     = video_info->userdata;
   video_width  = video_info->width;
   video_height = video_info->height;

   /* Rebuild widget tree if stale */
   if (nk->menu_dirty)
      nuklear_build_menu(nk);

   if (!nk->console)
      return;

   /* Synchronise nuklear_console's active widget with RetroArch's selection */
   nuklear_sync_selection(nk);

   /* Feed empty input block (RetroArch handles navigation) */
   nk_input_begin(&nk->ctx);
   nk_input_end(&nk->ctx);

   /* Render the nuklear_console widget tree into Nuklear's command buffer */
   menu_entries_get_title(title, sizeof(title));
   nk_console_render_window(nk->console, title,
         nk_rect(0.0f, 0.0f, (float)video_width, (float)video_height), 0);

   /* Translate Nuklear draw commands → gfx_display calls.
    * gfx_display uses the same top-left coordinate origin as Nuklear,
    * so no Y-axis flip is required here. */
   nk_foreach(cmd, &nk->ctx)
   {
      switch (cmd->type)
      {
         case NK_COMMAND_RECT_FILLED:
         {
            const struct nk_command_rect_filled *r =
                  (const struct nk_command_rect_filled*)cmd;
            float color[16];
            nk_color_to_float_arr(r->color, color);
            gfx_display_draw_quad(p_disp, userdata,
                  video_width, video_height,
                  (int)r->x, (int)r->y,
                  (unsigned)r->w, (unsigned)r->h,
                  video_width, video_height,
                  color, NULL);
            break;
         }

         case NK_COMMAND_TEXT:
         {
            const struct nk_command_text *t =
                  (const struct nk_command_text*)cmd;
            uint32_t color = nk_color_to_u32(t->foreground);
            /* gfx_display_draw_text skips draws where alpha == 0 */
            if ((color & 0xFF) == 0)
               color |= 0xFF;
            font_bind(&nk->font);
            gfx_display_draw_text(nk->font.font, t->string,
                  (float)t->x, (float)t->y,
                  (int)video_width, (int)video_height,
                  color, TEXT_ALIGN_LEFT, 1.0f,
                  false, 0.0f, true);
            font_flush(video_width, video_height, &nk->font);
            break;
         }

         default:
            break;
      }
   }

   nk_clear(&nk->ctx);
}

/* ── Menu-state callbacks ──────────────────────────────────────────── */

static void nuklear_populate_entries(void *data,
      const char *path, const char *label, unsigned k)
{
   nuklear_t *nk = (nuklear_t*)data;
   (void)path;
   (void)label;
   (void)k;
   if (nk)
      nk->menu_dirty = true;
}

static void nuklear_toggle(void *userdata, bool value)
{
   (void)userdata;
   (void)value;
}

static int nuklear_bind_init(menu_file_list_cbs_t *cbs,
      const char *path, const char *label,
      unsigned type, size_t idx)
{
   (void)cbs;
   (void)path;
   (void)label;
   (void)type;
   (void)idx;
   return 0;
}

static size_t nuklear_list_get_size(void *data, enum menu_list_type type)
{
   (void)data;
   if (type == MENU_LIST_PLAIN)
   {
      struct menu_state *menu_st = menu_state_get_ptr();
      menu_list_t *menu_list     = menu_st->entries.list;
      if (menu_list)
         return MENU_LIST_GET_STACK_SIZE(menu_list, 0);
   }
   return 0;
}

/* ── Driver registration ───────────────────────────────────────────── */

menu_ctx_driver_t menu_ctx_nuklear = {
   NULL,                        /* set_texture           */
   NULL,                        /* render_messagebox     */
   nuklear_render,
   nuklear_frame,
   nuklear_init,
   nuklear_free,
   nuklear_context_reset,
   nuklear_context_destroy,
   nuklear_populate_entries,
   nuklear_toggle,
   NULL,                        /* navigation_clear      */
   NULL,                        /* navigation_decrement  */
   NULL,                        /* navigation_increment  */
   NULL,                        /* navigation_set        */
   NULL,                        /* navigation_set_last   */
   NULL,                        /* navigation_descend_alphabet */
   NULL,                        /* navigation_ascend_alphabet  */
   NULL,                        /* lists_init            */
   NULL,                        /* list_insert           */
   NULL,                        /* list_prepend          */
   NULL,                        /* list_free             */
   NULL,                        /* list_clear            */
   NULL,                        /* list_cache            */
   NULL,                        /* list_push             */
   NULL,                        /* list_get_selection    */
   nuklear_list_get_size,
   NULL,                        /* list_get_entry        */
   NULL,                        /* list_set_selection    */
   nuklear_bind_init,
   NULL,                        /* load_image            */
   "nuklear",
   NULL,                        /* environ_cb            */
   NULL,                        /* update_thumbnail_path        */
   NULL,                        /* update_thumbnail_image       */
   NULL,                        /* refresh_thumbnail_image      */
   NULL,                        /* set_thumbnail_content        */
   NULL,                        /* osk_ptr_at_pos               */
   NULL,                        /* update_savestate_thumbnail_path  */
   NULL,                        /* update_savestate_thumbnail_image */
   NULL,                        /* pointer_down          */
   NULL,                        /* pointer_up            */
   NULL                         /* entry_action          */
};
