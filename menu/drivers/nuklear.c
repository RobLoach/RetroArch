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
 * bridged to RetroArch's gfx_display abstraction layer via
 * Nuklear's vertex buffer output (nk_convert / nk_draw_foreach). */

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
#include "../../gfx/video_driver.h"
#include "../../configuration.h"

/* ── Nuklear ───────────────────────────────────────────────────────── */
#define NK_INCLUDE_FIXED_TYPES
#define NK_INCLUDE_STANDARD_IO
#define NK_INCLUDE_STANDARD_VARARGS
#define NK_INCLUDE_DEFAULT_ALLOCATOR
#define NK_INCLUDE_VERTEX_BUFFER_OUTPUT  /* nk_convert / nk_draw_foreach  */
#define NK_INCLUDE_FONT_BAKING           /* nk_font_atlas API              */
#define NK_INCLUDE_DEFAULT_FONT          /* bake ProggyClean into atlas    */
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

/* ── Vertex layout ─────────────────────────────────────────────────── */

/* Interleaved vertex produced by nk_convert. */
struct nuklear_vertex
{
   float    pos[2];
   float    uv[2];
   nk_byte  col[4]; /* R G B A */
};

/* Vertex layout descriptor passed to nk_convert_config. */
static const struct nk_draw_vertex_layout_element nk_vertex_layout[] = {
   {NK_VERTEX_POSITION, NK_FORMAT_FLOAT,    NK_OFFSETOF(struct nuklear_vertex, pos)},
   {NK_VERTEX_TEXCOORD, NK_FORMAT_FLOAT,    NK_OFFSETOF(struct nuklear_vertex, uv)},
   {NK_VERTEX_COLOR,    NK_FORMAT_R8G8B8A8, NK_OFFSETOF(struct nuklear_vertex, col)},
   {NK_VERTEX_LAYOUT_END}
};

/* ── Constants ─────────────────────────────────────────────────────── */

/* Initial capacity for per-frame scratch arrays (in vertex count). */
#define NUKLEAR_SCRATCH_INITIAL 1024

/* ── Private state ─────────────────────────────────────────────────── */

typedef struct
{
   struct nk_context              ctx;          /* Nuklear immediate-mode context */
   nk_console                    *console;      /* nuklear_console root widget     */

   /* Font atlas (Nuklear's built-in ProggyClean bitmap font). */
   struct nk_font_atlas           atlas;
   uintptr_t                      atlas_texture;
   struct nk_draw_null_texture    null_tex;

   /* Per-frame vertex/index/command buffers used by nk_convert. */
   struct nk_buffer               vbuf;         /* vertex buffer   */
   struct nk_buffer               ibuf;         /* index buffer    */
   struct nk_buffer               cbuf;         /* command buffer  */

   /* Scratch arrays for de-interleaved / de-indexed triangle data. */
   float                         *scratch_pos;   /* 2 floats / vertex */
   float                         *scratch_uv;    /* 2 floats / vertex */
   float                         *scratch_color; /* 4 floats / vertex */
   size_t                         scratch_cap;   /* current capacity  */

   /* Per-entry widget pointers for selection sync. */
   nk_console                   **entry_widgets;
   size_t                         entry_count;

   /* Persistent strings / per-type values owned by nuklear_t.
    * nuklear_console stores raw label/value pointers — keep them alive
    * for the entire lifetime of the widget tree. */
   char                          *menu_title;      /* title widget label           */
   char                         **labels;          /* entry label copies           */
   nk_bool                       *checked_states;  /* BOOL checkbox values         */
   int                           *int_values;      /* INT / UINT property values   */
   float                         *float_values;    /* FLOAT property values        */
   size_t                         label_count;     /* == entry_count               */

   bool                           atlas_initialized; /* nk_font_atlas_init called */
   bool                           menu_dirty;
   unsigned                       last_width;
   unsigned                       last_height;
} nuklear_t;

/* ── Scratch buffer helpers ────────────────────────────────────────── */

static bool nuklear_ensure_scratch(nuklear_t *nk, size_t needed)
{
   float *pos, *uv, *col;

   if (needed <= nk->scratch_cap)
      return true;

   pos = (float*)realloc(nk->scratch_pos,   needed * 2 * sizeof(float));
   uv  = (float*)realloc(nk->scratch_uv,    needed * 2 * sizeof(float));
   col = (float*)realloc(nk->scratch_color, needed * 4 * sizeof(float));

   if (!pos || !uv || !col)
   {
      free(pos);
      free(uv);
      free(col);
      return false;
   }

   nk->scratch_pos   = pos;
   nk->scratch_uv    = uv;
   nk->scratch_color = col;
   nk->scratch_cap   = needed;
   return true;
}

static void nuklear_free_scratch(nuklear_t *nk)
{
   free(nk->scratch_pos);
   free(nk->scratch_uv);
   free(nk->scratch_color);
   nk->scratch_pos   = NULL;
   nk->scratch_uv    = NULL;
   nk->scratch_color = NULL;
   nk->scratch_cap   = 0;
}

/* ── Entry-widget list and persistent-string helpers ───────────────── */

/* Free the persistent label copies and checkbox-state array.
 * Must be called before rebuilding or on driver free. */
static void nuklear_clear_labels(nuklear_t *nk)
{
   size_t i;
   for (i = 0; i < nk->label_count; i++)
      free(nk->labels[i]);
   free(nk->labels);
   free(nk->checked_states);
   free(nk->int_values);
   free(nk->float_values);
   free(nk->menu_title);
   nk->labels         = NULL;
   nk->checked_states = NULL;
   nk->int_values     = NULL;
   nk->float_values   = NULL;
   nk->menu_title     = NULL;
   nk->label_count    = 0;
}

static void nuklear_clear_entry_widgets(nuklear_t *nk)
{
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

   /* Free previous widget tree and all heap-owned strings/states. */
   if (nk->console)
   {
      nk_console_free(nk->console);
      nk->console = NULL;
   }
   nuklear_clear_entry_widgets(nk);
   nuklear_clear_labels(nk);

   /* Determine entry count before allocating. */
   menu_st     = menu_state_get_ptr();
   menu_list   = menu_st->entries.list;
   sel_list    = menu_list
         ? MENU_LIST_GET_SELECTION(menu_list, 0) : NULL;
   entries_end = sel_list ? sel_list->size : 0;

   /* Allocate persistent label / state arrays.
    * nuklear_console stores raw label/value pointers — these must remain
    * valid for the entire lifetime of the widget tree. */
   if (entries_end > 0)
   {
      nk->labels         = (char**)calloc(entries_end, sizeof(char*));
      nk->checked_states = (nk_bool*)calloc(entries_end, sizeof(nk_bool));
      nk->int_values     = (int*)calloc(entries_end, sizeof(int));
      nk->float_values   = (float*)calloc(entries_end, sizeof(float));
      if (!nk->labels || !nk->checked_states
            || !nk->int_values || !nk->float_values)
      {
         nuklear_clear_labels(nk);
         return;
      }
   }
   nk->label_count = entries_end;

   /* Persistent copy of the title string. */
   menu_entries_get_title(title, sizeof(title));
   nk->menu_title = strdup(title);

   nk->console = nk_console_init(&nk->ctx);
   if (!nk->console)
   {
      nuklear_clear_labels(nk);
      return;
   }

   /* Non-selectable title label — uses our persistent copy. */
   nk_console_label(nk->console, nk->menu_title ? nk->menu_title : "");

   for (i = 0; i < entries_end; i++)
   {
      menu_entry_t entry;
      nk_console   *w;

      MENU_ENTRY_INITIALIZE(entry);
      entry.flags = MENU_ENTRY_FLAG_RICH_LABEL_ENABLED
                  | MENU_ENTRY_FLAG_VALUE_ENABLED
                  | MENU_ENTRY_FLAG_LABEL_ENABLED
                  ;
      menu_entry_get(&entry, 0, (unsigned)i, NULL, true);

      /* Copy label into heap-owned string so the widget pointer stays valid.
       * Fall back to entry.label when rich_label wasn't populated by
       * cbs->action_label (e.g. callback is NULL for some entry types). */
      nk->labels[i] = strdup(
            !string_is_empty(entry.rich_label)
            ? entry.rich_label : entry.label);

      switch (entry.type)
      {
         case MENU_ENTRY_BOOL:
            /* checkbox: stores a persistent nk_bool* that it writes on toggle */
            nk->checked_states[i] =
                  (entry.value[0] == 'O' && entry.value[1] == 'N')
                  ? nk_true : nk_false;
            w = nk_console_checkbox(nk->console,
                  nk->labels[i] ? nk->labels[i] : "",
                  &nk->checked_states[i]);
            break;

         case MENU_ENTRY_INT:
         case MENU_ENTRY_UINT:
         case MENU_ENTRY_SIZE:
            /* property_int: stores a persistent int* and renders label+value */
            nk->int_values[i] = atoi(entry.value);
            w = nk_console_property_int(nk->console,
                  nk->labels[i] ? nk->labels[i] : "",
                  INT_MIN, &nk->int_values[i], INT_MAX, 1, 1.0f);
            break;

         case MENU_ENTRY_FLOAT:
            /* property_float: stores a persistent float* and renders label+value */
            nk->float_values[i] = (float)atof(entry.value);
            w = nk_console_property_float(nk->console,
                  nk->labels[i] ? nk->labels[i] : "",
                  -1e9f, &nk->float_values[i], 1e9f, 0.1f, 0.01f);
            break;

         case MENU_ENTRY_ENUM:
         case MENU_ENTRY_HEX:
         case MENU_ENTRY_BIND:
         case MENU_ENTRY_STRING:
         case MENU_ENTRY_PATH:
         case MENU_ENTRY_DIR:
         case MENU_ENTRY_ACTION:
         default:
            w = nk_console_button(nk->console,
                  nk->labels[i] ? nk->labels[i] : "");
            break;
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

   nk_buffer_init_default(&nk->vbuf);
   nk_buffer_init_default(&nk->ibuf);
   nk_buffer_init_default(&nk->cbuf);

   /* Pre-allocate scratch arrays. */
   nk->scratch_pos   = (float*)malloc(NUKLEAR_SCRATCH_INITIAL * 2 * sizeof(float));
   nk->scratch_uv    = (float*)malloc(NUKLEAR_SCRATCH_INITIAL * 2 * sizeof(float));
   nk->scratch_color = (float*)malloc(NUKLEAR_SCRATCH_INITIAL * 4 * sizeof(float));
   if (!nk->scratch_pos || !nk->scratch_uv || !nk->scratch_color)
   {
      nuklear_free_scratch(nk);
      nk_buffer_free(&nk->vbuf);
      nk_buffer_free(&nk->ibuf);
      nk_buffer_free(&nk->cbuf);
      nk_free(&nk->ctx);
      free(nk);
      free(menu);
      return NULL;
   }
   nk->scratch_cap = NUKLEAR_SCRATCH_INITIAL;

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
   nuklear_clear_labels(nk);

   if (nk->atlas_texture)
   {
      video_driver_texture_unload(&nk->atlas_texture);
      nk->atlas_texture = 0;
   }
   if (nk->atlas_initialized)
   {
      nk_font_atlas_clear(&nk->atlas);
      nk->atlas_initialized = false;
   }

   nk_buffer_free(&nk->vbuf);
   nk_buffer_free(&nk->ibuf);
   nk_buffer_free(&nk->cbuf);

   nuklear_free_scratch(nk);

   nk_free(&nk->ctx);
   free(nk);
}

static void nuklear_context_reset(void *data, bool video_is_threaded)
{
   nuklear_t      *nk      = (nuklear_t*)data;
   const void     *img;
   int             aw, ah;
   struct texture_image ti;
   unsigned        width, height;

   (void)video_is_threaded;

   if (!nk)
      return;

   /* Release previous atlas if any. */
   if (nk->atlas_texture)
   {
      video_driver_texture_unload(&nk->atlas_texture);
      nk->atlas_texture = 0;
   }
   if (nk->atlas_initialized)
   {
      nk_font_atlas_clear(&nk->atlas);
      nk->atlas_initialized = false;
   }

   /* Bake Nuklear's built-in ProggyClean font into an RGBA32 atlas. */
   nk_font_atlas_init_default(&nk->atlas);
   nk->atlas_initialized = true;
   nk_font_atlas_begin(&nk->atlas);
   img = nk_font_atlas_bake(&nk->atlas, &aw, &ah, NK_FONT_ATLAS_RGBA32);

   /* Upload atlas pixels to the GPU. */
   memset(&ti, 0, sizeof(ti));
   ti.width          = (unsigned)aw;
   ti.height         = (unsigned)ah;
   ti.pixels         = (uint32_t*)img;
   ti.supports_rgba  = true;
   video_driver_texture_load(&ti, TEXTURE_FILTER_NEAREST, &nk->atlas_texture);

   /* Finalise atlas: frees pixel data, sets up null_tex white region. */
   nk_font_atlas_end(&nk->atlas,
         nk_handle_ptr((void*)nk->atlas_texture),
         &nk->null_tex);

   /* Install the baked font into the Nuklear context. */
   if (nk->atlas.default_font)
      nk_style_set_font(&nk->ctx, &nk->atlas.default_font->handle);

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
   if (nk->atlas_texture)
   {
      video_driver_texture_unload(&nk->atlas_texture);
      nk->atlas_texture = 0;
   }
   if (nk->atlas_initialized)
   {
      nk_font_atlas_clear(&nk->atlas);
      nk->atlas_initialized = false;
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

/* ── Frame rendering ───────────────────────────────────────────────── */

static void nuklear_frame(void *data, video_frame_info_t *video_info)
{
   nuklear_t                        *nk;
   gfx_display_t                    *p_disp;
   gfx_display_ctx_driver_t         *dispctx;
   const struct nk_draw_command     *cmd;
   const struct nuklear_vertex      *vertices;
   const nk_draw_index              *indices;
   void                             *userdata;
   unsigned                          video_width;
   unsigned                          video_height;
   char                              title[NAME_MAX_LENGTH];
   struct nk_convert_config          cfg;
   nk_uint                           elem_offset;

   nk = (nuklear_t*)data;
   if (!nk || !nk->atlas_texture)
      return;

   p_disp       = disp_get_ptr();
   dispctx      = p_disp->dispctx;
   if (!dispctx || !dispctx->draw)
      return;

   userdata     = video_info->userdata;
   video_width  = video_info->width;
   video_height = video_info->height;

   if (video_width == 0 || video_height == 0)
      return;

   /* Rebuild widget tree if stale */
   if (nk->menu_dirty)
      nuklear_build_menu(nk);

   if (!nk->console)
      return;

   nuklear_sync_selection(nk);

   /* Feed empty input block (RetroArch owns all navigation). */
   nk_input_begin(&nk->ctx);
   nk_input_end(&nk->ctx);

   /* Pump the nuklear_console widget tree into Nuklear's command queue. */
   menu_entries_get_title(title, sizeof(title));
   nk_console_render_window(nk->console, title,
         nk_rect(0.0f, 0.0f, (float)video_width, (float)video_height), 0);

   /* Convert Nuklear commands → vertex / index / draw-command buffers. */
   memset(&cfg, 0, sizeof(cfg));
   cfg.vertex_layout        = nk_vertex_layout;
   cfg.vertex_size          = sizeof(struct nuklear_vertex);
   cfg.vertex_alignment     = NK_ALIGNOF(struct nuklear_vertex);
   cfg.global_alpha         = 1.0f;
   cfg.shape_AA             = NK_ANTI_ALIASING_ON;
   cfg.line_AA              = NK_ANTI_ALIASING_ON;
   cfg.circle_segment_count = 22;
   cfg.curve_segment_count  = 22;
   cfg.arc_segment_count    = 22;
   cfg.tex_null             = nk->null_tex;

   nk_buffer_clear(&nk->vbuf);
   nk_buffer_clear(&nk->ibuf);
   nk_buffer_clear(&nk->cbuf);
   nk_convert(&nk->ctx, &nk->cbuf, &nk->vbuf, &nk->ibuf, &cfg);

   vertices    = (const struct nuklear_vertex*)nk->vbuf.memory.ptr;
   indices     = (const nk_draw_index*)nk->ibuf.memory.ptr;
   elem_offset = 0;

   /* Walk each draw command and submit a triangle batch. */
   nk_draw_foreach(cmd, &nk->ctx, &nk->cbuf)
   {
      nk_uint                   i;
      gfx_display_ctx_draw_t    draw;
      struct video_coords        coords;
      float                     inv_w = 1.0f / (float)video_width;
      float                     inv_h = 1.0f / (float)video_height;

      if (cmd->elem_count == 0)
      {
         elem_offset += cmd->elem_count;
         continue;
      }

      /* Expand scratch buffers if needed. */
      if (!nuklear_ensure_scratch(nk, cmd->elem_count))
      {
         elem_offset += cmd->elem_count;
         continue;
      }

      /* De-index and de-interleave into separate float arrays.
       *
       * Coordinate normalisation for gfx_display:
       *   x_norm = nk_x / video_width          (left=0 .. right=1)
       *   y_norm = 1.0f - nk_y / video_height  (top→1, bottom→0)
       *
       * The GL2 display driver uses mvp_no_rot = ortho(0,1,0,1,-1,1)
       * with a full-screen viewport, so [0,1]² maps correctly to NDC.
       * The Y flip converts Nuklear's top-left origin to GL's bottom-left.
       */
      for (i = 0; i < cmd->elem_count; i++)
      {
         const struct nuklear_vertex *v =
               &vertices[indices[elem_offset + i]];
         size_t j = (size_t)i;

         nk->scratch_pos[j * 2 + 0] = v->pos[0] * inv_w;
         nk->scratch_pos[j * 2 + 1] = 1.0f - v->pos[1] * inv_h;

         nk->scratch_uv[j * 2 + 0] = v->uv[0];
         nk->scratch_uv[j * 2 + 1] = v->uv[1];

         nk->scratch_color[j * 4 + 0] = (float)v->col[0] * (1.0f / 255.0f);
         nk->scratch_color[j * 4 + 1] = (float)v->col[1] * (1.0f / 255.0f);
         nk->scratch_color[j * 4 + 2] = (float)v->col[2] * (1.0f / 255.0f);
         nk->scratch_color[j * 4 + 3] = (float)v->col[3] * (1.0f / 255.0f);
      }

      /* Scissor: Nuklear gives top-left pixel coords; gfx_display_scissor_begin
       * accepts the same convention and handles the GL Y-flip internally. */
      gfx_display_scissor_begin(p_disp, userdata, video_width, video_height,
            (int)cmd->clip_rect.x,    (int)cmd->clip_rect.y,
            (unsigned)cmd->clip_rect.w, (unsigned)cmd->clip_rect.h);

      /* Build the draw call. */
      memset(&coords, 0, sizeof(coords));
      coords.vertices      = (unsigned)cmd->elem_count;
      coords.vertex        = nk->scratch_pos;
      coords.tex_coord     = nk->scratch_uv;
      coords.lut_tex_coord = nk->scratch_uv; /* reuse UVs; stock shader ignores LUT */
      coords.color         = nk->scratch_color;

      memset(&draw, 0, sizeof(draw));
      draw.x           = 0;
      draw.y           = 0;
      draw.width       = video_width;
      draw.height      = video_height;
      draw.coords      = &coords;
      draw.texture     = (uintptr_t)cmd->texture.ptr;
      draw.prim_type   = GFX_DISPLAY_PRIM_TRIANGLES;
      draw.matrix_data = dispctx->get_default_mvp
            ? dispctx->get_default_mvp(userdata) : NULL;

      if (dispctx->blend_begin)
         dispctx->blend_begin(userdata);
      dispctx->draw(&draw, userdata, video_width, video_height);
      if (dispctx->blend_end)
         dispctx->blend_end(userdata);

      elem_offset += cmd->elem_count;
   }

   /* Disable scissor after all draw commands. */
   if (dispctx->scissor_end)
      dispctx->scissor_end(userdata, video_width, video_height);

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
