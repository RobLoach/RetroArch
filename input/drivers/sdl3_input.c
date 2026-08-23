/*  RetroArch - A frontend for libretro.
 *  Copyright (C) 2010-2014 - Hans-Kristian Arntzen
 *  Copyright (C) 2011-2017 - Daniel De Matteis
 *  Copyright (C) 2014-2015 - Higor Euripedes
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

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <boolean.h>
#include <string/stdstring.h>
#include <encodings/utf.h>
#include <libretro.h>

#include <SDL3/SDL.h>

#include "../input_keymaps.h"

#include "../../configuration.h"
#include "../../retroarch.h"
#include "../../verbosity.h"

#include "../../gfx/common/sdl3_common.h"

/* OVERLAY_MAX_TOUCH */
#define SDL3_MAX_TOUCH 16

#define SDL3_MAX_MICE 8

/* Mouse state, so that input_mouse_index can address individual mice.
 * Index 0 is the merged system mouse (SDL_GetMouseState and friends),
 * while indices 1..N map to per-device state accumulated from SDL
 * events. Only relative motion, buttons and wheel are per-device; the
 * absolute cursor position exists solely for the merged system mouse.
 * An id of 0 marks a vacant slot: slots are kept in place when a
 * mouse unplugs, so the input_mouse_index mapping of the remaining
 * mice never shifts mid-session. */
typedef struct sdl3_mouse
{
   SDL_MouseID id;
   /* This frame's relative motion, kept in SDL's native floats and
    * truncated to whole pixels only at the libretro boundary. */
   float x;
   float y;
   /* Accumulated motion still to be folded into x/y; after each fold
    * it carries the sub-pixel part the boundary truncation drops. */
   float rel_x;
   float rel_y;
   /* Button and wheel state, indexed by RETRO_DEVICE_ID_MOUSE_*. */
   bool buttons[RETRO_DEVICE_ID_MOUSE_BUTTON_5 + 1];
} sdl3_mouse_t;

typedef struct sdl3_input
{
   /* The keyboard state, provided by SDL_GetKeyboardState(). */
   const bool *kb_state;
   int kb_num_keys;
   SDL_Scancode key_scancode_lut[RETROK_LAST];
   /* The merged system mouse (input_mouse_index 0). */
   sdl3_mouse_t mouse;
   /* Absolute position stays fractional; it's truncated at the API
    * boundary, where nothing accumulates. */
   float mouse_abs_x;
   float mouse_abs_y;

   /* Per-device slots (input_mouse_index 1..N). */
   int num_mice;
   sdl3_mouse_t mice[SDL3_MAX_MICE];

   /* Number of connected touch devices. Saves having to query them
    * every frame. */
   int num_touch_devices;
   unsigned touch_recheck;

   /* Number of active fingers across all touch devices. */
   int num_touches;
   struct
   {
      float x;
      float y;
   } touches[SDL3_MAX_TOUCH];
} sdl3_input_t;

/* Rebuilt on SDL_EVENT_KEYMAP_CHANGED (e.g. system layout switch). */
static void sdl3_build_scancode_lut(sdl3_input_t *sdl)
{
   int i;
   for (i = 0; i < RETROK_LAST; i++)
      sdl->key_scancode_lut[i] = rarch_keysym_lut[i]
            ? SDL_GetScancodeFromKey((SDL_Keycode)rarch_keysym_lut[i], NULL)
            : SDL_SCANCODE_UNKNOWN;
}

static sdl3_mouse_t *sdl3_get_mouse(sdl3_input_t *sdl, SDL_MouseID id)
{
   int i;
   /* 0 is what SDL puts in an event's which field for motion it
    * synthesizes itself (e.g. warps); it must not match the 0 that
    * marks a vacant slot. */
   if (id == 0)
      return NULL;
   for (i = 0; i < sdl->num_mice; i++)
   {
      if (sdl->mice[i].id == id)
         return &sdl->mice[i];
   }
   return NULL;
}

/* Resolves a port's Mouse Index setting onto the mouse it addresses,
 * or NULL when it points past the known devices. Index 0 is the
 * merged system mouse; 1..N are the individual slots. */
static const sdl3_mouse_t *sdl3_get_port_mouse(sdl3_input_t *sdl, unsigned port)
{
   unsigned mouse_index = config_get_ptr()->uints.input_mouse_index[port];
   if (mouse_index == 0)
      return &sdl->mouse;
   if (mouse_index <= (unsigned)sdl->num_mice)
      return &sdl->mice[mouse_index - 1];
   return NULL;
}

/* Folds accumulated motion into this frame's x/y, carrying over the
 * sub-pixel part that the int16_t boundary conversion drops. */
static void sdl3_mouse_fold_motion(sdl3_mouse_t *mouse)
{
   /* Converting a float outside int16_t's range is undefined behaviour
    * rather than a wrap, so clamp before the cast. Only reachable if
    * the frontend stops polling for a long stretch while the mouse
    * keeps moving. */
   mouse->rel_x = SDL_clamp(mouse->rel_x, -32767.0f, 32767.0f);
   mouse->rel_y = SDL_clamp(mouse->rel_y, -32767.0f, 32767.0f);

   mouse->x = mouse->rel_x;
   mouse->y = mouse->rel_y;

   /* Keep only the sub-pixel part; the whole pixels are what the
    * boundary cast will report. */
   mouse->rel_x -= SDL_truncf(mouse->x);
   mouse->rel_y -= SDL_truncf(mouse->y);
}

/* Wheel state is edge-style: it reflects this frame's events only. */
static void sdl3_mouse_clear_wheel(sdl3_mouse_t *mouse)
{
   mouse->buttons[RETRO_DEVICE_ID_MOUSE_WHEELUP]         = false;
   mouse->buttons[RETRO_DEVICE_ID_MOUSE_WHEELDOWN]       = false;
   mouse->buttons[RETRO_DEVICE_ID_MOUSE_HORIZ_WHEELUP]   = false;
   mouse->buttons[RETRO_DEVICE_ID_MOUSE_HORIZ_WHEELDOWN] = false;
}

static void sdl3_mouse_wheel(sdl3_mouse_t *mouse, float wx, float wy)
{
   mouse->buttons[RETRO_DEVICE_ID_MOUSE_WHEELUP]         |= wy > 0;
   mouse->buttons[RETRO_DEVICE_ID_MOUSE_WHEELDOWN]       |= wy < 0;
   mouse->buttons[RETRO_DEVICE_ID_MOUSE_HORIZ_WHEELUP]   |= wx > 0;
   mouse->buttons[RETRO_DEVICE_ID_MOUSE_HORIZ_WHEELDOWN] |= wx < 0;
}

/* Maps an SDL mouse button onto its RETRO_DEVICE_ID_MOUSE_* index,
 * or -1 for buttons libretro has no id for. */
static int sdl3_translate_mouse_button(Uint8 button)
{
   switch (button)
   {
      case SDL_BUTTON_LEFT:
         return RETRO_DEVICE_ID_MOUSE_LEFT;
      case SDL_BUTTON_RIGHT:
         return RETRO_DEVICE_ID_MOUSE_RIGHT;
      case SDL_BUTTON_MIDDLE:
         return RETRO_DEVICE_ID_MOUSE_MIDDLE;
      case SDL_BUTTON_X1:
         return RETRO_DEVICE_ID_MOUSE_BUTTON_4;
      case SDL_BUTTON_X2:
         return RETRO_DEVICE_ID_MOUSE_BUTTON_5;
   }
   return -1;
}

static void sdl3_mouse_added(sdl3_input_t *sdl, SDL_MouseID id)
{
   int slot;
   const char *name;

   /* Events with a 0 instance id belong to the merged system mouse,
    * and SDL_TOUCH_MOUSEID marks touch-synthesized events; neither is
    * an addressable device. */
   if (id == 0 || id == SDL_TOUCH_MOUSEID)
      return;
   /* The list is seeded from SDL_GetMice() at init, so the ADDED
    * events SDL delivers for already-connected mice must not
    * duplicate entries. */
   if (sdl3_get_mouse(sdl, id))
      return;

   /* Reuse the first vacant slot instead of growing the list. */
   for (slot = 0; slot < sdl->num_mice; slot++)
   {
      if (sdl->mice[slot].id == 0)
         break;
   }
   if (slot >= SDL3_MAX_MICE)
   {
      RARCH_WARN("[SDL3] Ignoring mouse %u; only %d mice are supported.\n",
            (unsigned)id, SDL3_MAX_MICE);
      return;
   }

   memset(&sdl->mice[slot], 0, sizeof(sdl->mice[0]));
   sdl->mice[slot].id = id;
   if (slot == sdl->num_mice)
      sdl->num_mice++;

   /* Shown by the Mouse Index setting; slot 0 lives at menu index 1,
    * as menu index 0 is the merged system mouse. */
   if (!(name = SDL_GetMouseNameForID(id)))
      name = "Mouse";
   input_config_set_mouse_display_name((unsigned)slot + 1, name);
   RARCH_LOG("[SDL3] Mouse #%d: \"%s\".\n", slot + 1, name);
}

static void sdl3_mouse_removed(sdl3_input_t *sdl, SDL_MouseID id)
{
   int slot;
   sdl3_mouse_t *mouse = sdl3_get_mouse(sdl, id);

   if (!mouse)
      return;

   slot = (int)(mouse - sdl->mice);
   RARCH_LOG("[SDL3] Mouse #%d removed.\n", slot + 1);

   /* Vacate the slot in place (id 0), leaving the other mice on
    * their indices; the slot reads back as no input until a new
    * mouse fills it. */
   memset(mouse, 0, sizeof(*mouse));
   input_config_set_mouse_display_name((unsigned)slot + 1, "N/A");

   while (sdl->num_mice > 0 && sdl->mice[sdl->num_mice - 1].id == 0)
      sdl->num_mice--;
}

static void *sdl3_input_init(const char *joypad_driver)
{
   sdl3_input_t *sdl = (sdl3_input_t*)calloc(1, sizeof(*sdl));
   if (!sdl)
      return NULL;

   /* Set up the SDL event queue. */
   if (!SDL_InitSubSystem(SDL_INIT_EVENTS))
   {
      free(sdl);
      return NULL;
   }

   input_keymaps_init_keyboard_lut(rarch_key_map_sdl3);

   sdl->kb_state = SDL_GetKeyboardState(&sdl->kb_num_keys);
   sdl3_build_scancode_lut(sdl);

   /* Prime the touch probe so a present touchscreen works from the
    * first frame (see sdl3_poll_touch). */
   {
      SDL_TouchID *devices = SDL_GetTouchDevices(&sdl->num_touch_devices);
      SDL_free(devices);
   }

   /* Seed the per-device mouse list from the mice already connected;
    * SDL_EVENT_MOUSE_ADDED/REMOVED keep it current from here on. */
   {
      int i;
      int num_mice      = 0;
      SDL_MouseID *mice = SDL_GetMice(&num_mice);

      /* Menu index 0 is the merged system mouse; the device slots
       * follow at 1..N. The names are global state a previous input
       * driver may have filled, so reset them all first. */
      input_config_set_mouse_display_name(0, "System mouse");
      for (i = 1; i <= SDL3_MAX_MICE; i++)
         input_config_set_mouse_display_name((unsigned)i, "N/A");

      if (mice)
      {
         for (i = 0; i < num_mice; i++)
            sdl3_mouse_added(sdl, mice[i]);
         SDL_free(mice);
      }
   }

   return sdl;
}

static bool sdl3_key_pressed(sdl3_input_t *sdl, int key)
{
   /* The keyboard state array is refreshed by SDL while pumping
    * window events - it stays empty until a focused SDL3 window
    * exists (i.e. the SDL3 video driver is running). */
   SDL_Scancode sym = sdl->key_scancode_lut[key];

   if ((int)sym >= sdl->kb_num_keys)
      return false;

   return sdl->kb_state[sym];
}

/* Resolves a retro_keybind mouse-button bind (bind->mbutton) against
 * the port's mouse state; used by the lightgun bind checks below. */
static bool sdl3_mouse_button_pressed(
      sdl3_input_t *sdl, unsigned port, unsigned key)
{
   const sdl3_mouse_t *mouse = sdl3_get_port_mouse(sdl, port);
   /* The X/Y slots (0 and 1) are never set, so they read back false. */
   if (mouse && key <= RETRO_DEVICE_ID_MOUSE_BUTTON_5)
      return mouse->buttons[key];
   return false;
}

static int16_t sdl3_input_state(
      void *data,
      const input_device_driver_t *joypad,
      const input_device_driver_t *sec_joypad,
      rarch_joypad_info_t *joypad_info,
      const retro_keybind_set *binds,
      bool keyboard_mapping_blocked,
      unsigned port,
      unsigned device,
      unsigned idx,
      unsigned id)
{
   int16_t ret = 0;
   sdl3_input_t *sdl = (sdl3_input_t*)data;

   switch (device)
   {
      case RETRO_DEVICE_JOYPAD:
         if (id == RETRO_DEVICE_ID_JOYPAD_MASK)
         {
            unsigned i;

            if (!keyboard_mapping_blocked)
            {
               for (i = 0; i < RARCH_FIRST_CUSTOM_BIND; i++)
               {
                  if (binds[port][i].valid)
                  {
                     if ((binds[port][i].key && binds[port][i].key < RETROK_LAST)
                           && sdl3_key_pressed(sdl, binds[port][i].key))
                        ret |= (1 << i);
                  }
               }
            }

            return ret;
         }

         if (id < RARCH_BIND_LIST_END)
         {
            if (binds[port][id].valid)
            {
               if ((binds[port][id].key && binds[port][id].key < RETROK_LAST)
                     && sdl3_key_pressed(sdl, binds[port][id].key)
                     && (id == RARCH_GAME_FOCUS_TOGGLE || !keyboard_mapping_blocked)
                  )
                  return 1;
            }
         }
         break;
      case RETRO_DEVICE_ANALOG:
         {
            int id_minus_key = 0;
            int id_plus_key = 0;
            unsigned id_minus = 0;
            unsigned id_plus = 0;
            bool id_plus_valid = false;
            bool id_minus_valid = false;

            input_conv_analog_id_to_bind_id(idx, id, id_minus, id_plus);

            id_minus_valid = binds[port][id_minus].valid;
            id_plus_valid = binds[port][id_plus].valid;
            id_minus_key = binds[port][id_minus].key;
            id_plus_key = binds[port][id_plus].key;

            if (id_plus_valid && id_plus_key && id_plus_key < RETROK_LAST)
            {
               if (sdl3_key_pressed(sdl, id_plus_key))
                  ret = 0x7fff;
            }
            if (id_minus_valid && id_minus_key && id_minus_key < RETROK_LAST)
            {
               if (sdl3_key_pressed(sdl, id_minus_key))
                  ret += -0x7fff;
            }
         }
         return ret;
      case RETRO_DEVICE_MOUSE:
      case RARCH_DEVICE_MOUSE_SCREEN:
         {
            const sdl3_mouse_t *mouse = sdl3_get_port_mouse(sdl, port);

            if (!mouse)
               break;

            switch (id)
            {
               case RETRO_DEVICE_ID_MOUSE_X:
                  /* Only relative motion is per-device; the screen
                   * cursor is a single system-wide position. */
                  if (device == RARCH_DEVICE_MOUSE_SCREEN)
                     return (int16_t)sdl->mouse_abs_x;
                  return (int16_t)mouse->x;
               case RETRO_DEVICE_ID_MOUSE_Y:
                  if (device == RARCH_DEVICE_MOUSE_SCREEN)
                     return (int16_t)sdl->mouse_abs_y;
                  return (int16_t)mouse->y;
               default:
                  if (id <= RETRO_DEVICE_ID_MOUSE_BUTTON_5)
                     return mouse->buttons[id];
                  break;
            }
         }
         break;
      case RETRO_DEVICE_POINTER:
      case RARCH_DEVICE_POINTER_SCREEN:
         {
            video_viewport_t vp = {0};
            bool screen = device == RARCH_DEVICE_POINTER_SCREEN;
            int16_t res_x = 0;
            int16_t res_y = 0;
            int16_t res_screen_x = 0;
            int16_t res_screen_y = 0;
            int abs_x = 0;
            int abs_y = 0;
            int16_t pressed = 0;

            if (id == RETRO_DEVICE_ID_POINTER_COUNT)
               return sdl->num_touches ? sdl->num_touches
                     : (sdl->mouse.buttons[RETRO_DEVICE_ID_MOUSE_LEFT] ? 1 : 0);

            if (!video_driver_get_viewport_info(&vp))
               break;

            /* Touch contacts take precedence; the mouse doubles as
             * pointer 0 when no fingers are down (touch/pointer
             * overlay support - input_poll_overlay walks pointer
             * indices until PRESSED reads 0). */
            if (sdl->num_touches > 0)
            {
               if ((int)idx >= sdl->num_touches)
                  return 0;
               abs_x = (int)(sdl->touches[idx].x * (float)vp.full_width);
               abs_y = (int)(sdl->touches[idx].y * (float)vp.full_height);
               pressed = 1;
            }
            else
            {
               if (idx != 0)
                  return 0;
               abs_x = (int)sdl->mouse_abs_x;
               abs_y = (int)sdl->mouse_abs_y;
               pressed = sdl->mouse.buttons[RETRO_DEVICE_ID_MOUSE_LEFT];
            }

            if (video_driver_translate_coord_viewport(
                        &vp, abs_x, abs_y,
                        &res_x, &res_y, &res_screen_x, &res_screen_y,
                        true))
            {
               if (screen)
               {
                  res_x = res_screen_x;
                  res_y = res_screen_y;
               }

               switch (id)
               {
                  case RETRO_DEVICE_ID_POINTER_X:
                     return res_x;
                  case RETRO_DEVICE_ID_POINTER_Y:
                     return res_y;
                  case RETRO_DEVICE_ID_POINTER_PRESSED:
                     return pressed;
                  case RETRO_DEVICE_ID_POINTER_IS_OFFSCREEN:
                     return input_driver_pointer_is_offscreen(res_x, res_y);
               }
            }
         }
         break;
      case RETRO_DEVICE_KEYBOARD:
         return (id && id < RETROK_LAST) && sdl3_key_pressed(sdl, id);
      case RETRO_DEVICE_LIGHTGUN:
         switch (id)
         {
            /* Aiming */
            case RETRO_DEVICE_ID_LIGHTGUN_SCREEN_X:
            case RETRO_DEVICE_ID_LIGHTGUN_SCREEN_Y:
            case RETRO_DEVICE_ID_LIGHTGUN_IS_OFFSCREEN:
               {
                  video_viewport_t vp  = {0};
                  int16_t res_x        = 0;
                  int16_t res_y        = 0;
                  int16_t res_screen_x = 0;
                  int16_t res_screen_y = 0;

                  if (video_driver_translate_coord_viewport_wrap(
                              &vp, (int)sdl->mouse_abs_x, (int)sdl->mouse_abs_y,
                              &res_x, &res_y, &res_screen_x, &res_screen_y))
                  {
                     switch (id)
                     {
                        case RETRO_DEVICE_ID_LIGHTGUN_SCREEN_X:
                           return res_x;
                        case RETRO_DEVICE_ID_LIGHTGUN_SCREEN_Y:
                           return res_y;
                        case RETRO_DEVICE_ID_LIGHTGUN_IS_OFFSCREEN:
                           return input_driver_pointer_is_offscreen(res_x, res_y);
                        default:
                           break;
                     }
                  }
               }
               break;
            /* Buttons */
            case RETRO_DEVICE_ID_LIGHTGUN_TRIGGER:
            case RETRO_DEVICE_ID_LIGHTGUN_RELOAD:
            case RETRO_DEVICE_ID_LIGHTGUN_AUX_A:
            case RETRO_DEVICE_ID_LIGHTGUN_AUX_B:
            case RETRO_DEVICE_ID_LIGHTGUN_AUX_C:
            case RETRO_DEVICE_ID_LIGHTGUN_START:
            case RETRO_DEVICE_ID_LIGHTGUN_SELECT:
            case RETRO_DEVICE_ID_LIGHTGUN_DPAD_UP:
            case RETRO_DEVICE_ID_LIGHTGUN_DPAD_DOWN:
            case RETRO_DEVICE_ID_LIGHTGUN_DPAD_LEFT:
            case RETRO_DEVICE_ID_LIGHTGUN_DPAD_RIGHT:
            case RETRO_DEVICE_ID_LIGHTGUN_PAUSE: /* deprecated */
               {
                  unsigned new_id                 = input_driver_lightgun_id_convert(id);
                  const uint64_t bind_joykey      = input_config_binds[port][new_id].joykey;
                  const uint64_t bind_joyaxis     = input_config_binds[port][new_id].joyaxis;
                  const uint64_t autobind_joykey  = input_autoconf_binds[port][new_id].joykey;
                  const uint64_t autobind_joyaxis = input_autoconf_binds[port][new_id].joyaxis;
                  uint16_t joyport                = joypad_info->joy_idx;
                  float axis_threshold            = joypad_info->axis_threshold;
                  const uint64_t joykey           = (bind_joykey != NO_BTN)
                        ? bind_joykey  : autobind_joykey;
                  const uint32_t joyaxis          = (bind_joyaxis != AXIS_NONE)
                        ? bind_joyaxis : autobind_joyaxis;

                  if (binds[port][new_id].valid)
                  {
                     if ((uint16_t)joykey != NO_BTN && joypad->button(
                              joyport, (uint16_t)joykey))
                        return 1;
                     if (joyaxis != AXIS_NONE &&
                           ((float)abs(joypad->axis(joyport, joyaxis))
                            / 0x8000) > axis_threshold)
                        return 1;
                     else if ((binds[port][new_id].key && binds[port][new_id].key < RETROK_LAST)
                           && !keyboard_mapping_blocked
                           && sdl3_key_pressed(sdl, binds[port][new_id].key)
                        )
                        return 1;
                     else if (sdl3_mouse_button_pressed(sdl, port,
                           binds[port][new_id].mbutton))
                        return 1;
                  }
               }
               break;
            /* Deprecated relative aiming */
            case RETRO_DEVICE_ID_LIGHTGUN_X:
            case RETRO_DEVICE_ID_LIGHTGUN_Y:
               {
                  const sdl3_mouse_t *mouse = sdl3_get_port_mouse(sdl, port);
                  if (!mouse)
                     break;
                  if (id == RETRO_DEVICE_ID_LIGHTGUN_X)
                     return (int16_t)mouse->x;
                  return (int16_t)mouse->y;
               }
         }
         break;
   }

   return 0;
}

static void sdl3_input_free(void *data)
{
   sdl3_input_t *sdl = (sdl3_input_t*)data;

   if (!sdl)
      return;

   /* Drop only the events this driver owns, matching what
    * sdl3_input_poll consumes. Flushing SDL_EVENT_FIRST..LAST would
    * also take the video driver's pending SDL_EVENT_QUIT and the
    * joypad driver's hotplug events with it - harmless at shutdown,
    * but input drivers are also torn down and recreated on a runtime
    * driver switch, where that can swallow a window close the user
    * already clicked. */
   SDL_FlushEvents(SDL_EVENT_KEY_DOWN,         SDL_EVENT_MOUSE_REMOVED);
   SDL_FlushEvents(SDL_EVENT_FINGER_DOWN,      SDL_EVENT_FINGER_CANCELED);
   SDL_FlushEvents(SDL_EVENT_PEN_PROXIMITY_IN, SDL_EVENT_PEN_AXIS);

   SDL_QuitSubSystem(SDL_INIT_EVENTS);
   free(sdl);
}

static bool sdl3_set_sensor_state(void *data, unsigned port,
      enum retro_sensor_action action, unsigned rate)
{
   /* Sensors are not exposed through the SDL3 keyboard/mouse driver.
    * Gamepad gyro/accel are handled by the SDL3 joypad driver. */
   switch (action)
   {
      case RETRO_SENSOR_ILLUMINANCE_DISABLE:
      case RETRO_SENSOR_GYROSCOPE_DISABLE:
      case RETRO_SENSOR_ACCELEROMETER_DISABLE:
         /* Disabling an unsupported sensor shouldn't fail. */
         return true;
      default:
         break;
   }

   return false;
}

/* Gets the SDL_Window, if it exists. */
static SDL_Window *sdl3_input_window(void)
{
   gfx_ctx_ident_t ident_info;

   if (string_is_equal(video_driver_get_ident(), "sdl3"))
   {
      sdl3_video_t *video_ptr = (sdl3_video_t*)video_driver_get_ptr();
      return video_ptr != NULL ? video_ptr->window : NULL;
   }

   /* gl/gl1/glcore/vulkan running on the SDL3 context drivers. They
    * register their SDL_Window as the display userdata via sdl3_set_handles. */
   ident_info.ident = NULL;
   video_context_driver_get_ident(&ident_info);
   if (string_is_equal(ident_info.ident, "gl_sdl3") || string_is_equal(ident_info.ident, "vk_sdl3"))
      return (SDL_Window*)video_driver_display_userdata_get();

   return NULL;
}

static void sdl3_poll_mouse(sdl3_input_t *sdl)
{
   SDL_Window *win;
   float dx = 0.0f;
   float dy = 0.0f;
   SDL_MouseButtonFlags btn = SDL_GetMouseState(&sdl->mouse_abs_x, &sdl->mouse_abs_y);
   SDL_GetRelativeMouseState(&dx, &dy);

   sdl->mouse.rel_x += dx;
   sdl->mouse.rel_y += dy;
   sdl3_mouse_fold_motion(&sdl->mouse);

   /* SDL reports mouse coordinates in window coordinates (points),
    * while the video driver's viewport metrics are in output pixels. */
   if (!(win = sdl3_input_window()))
      win = SDL_GetMouseFocus();

   if (win)
   {
      float density = SDL_GetWindowPixelDensity(win);
      if (density > 0.0f && density != 1.0f)
      {
         sdl->mouse_abs_x *= density;
         sdl->mouse_abs_y *= density;
      }
   }

   sdl->mouse.buttons[RETRO_DEVICE_ID_MOUSE_LEFT]     = (SDL_BUTTON_MASK(SDL_BUTTON_LEFT) & btn) != 0;
   sdl->mouse.buttons[RETRO_DEVICE_ID_MOUSE_RIGHT]    = (SDL_BUTTON_MASK(SDL_BUTTON_RIGHT) & btn) != 0;
   sdl->mouse.buttons[RETRO_DEVICE_ID_MOUSE_MIDDLE]   = (SDL_BUTTON_MASK(SDL_BUTTON_MIDDLE) & btn) != 0;
   sdl->mouse.buttons[RETRO_DEVICE_ID_MOUSE_BUTTON_4] = (SDL_BUTTON_MASK(SDL_BUTTON_X1) & btn) != 0;
   sdl->mouse.buttons[RETRO_DEVICE_ID_MOUSE_BUTTON_5] = (SDL_BUTTON_MASK(SDL_BUTTON_X2) & btn) != 0;
}

/* Snapshot the active fingers across all touchscreens. Polling
 * SDL_GetTouchFingers avoids tracking finger-id lifetimes through
 * SDL_EVENT_FINGER_* events by hand. */
static void sdl3_poll_touch(sdl3_input_t *sdl)
{
   int i;
   int num_devices = 0;
   int num_direct = 0;
   SDL_TouchID *devices     = NULL;

   sdl->num_touches = 0;

   /* Skip the per-frame device query on touch-less setups; re-probe
    * every ~10s since SDL3 has no touch hotplug event. */
   if (sdl->num_touch_devices == 0)
   {
      if (++sdl->touch_recheck < 600)
         return;
      sdl->touch_recheck = 0;
   }

   devices = SDL_GetTouchDevices(&num_devices);
   if (!devices)
   {
      sdl->num_touch_devices = 0;
      return;
   }

   for (i = 0; i < num_devices; i++)
   {
      int j, num_fingers = 0;
      SDL_Finger **fingers;

      /* Only SDL_TOUCH_DEVICE_DIRECT is a touchscreen. The two indirect
       * types are trackpads, whose fingers are device or cursor-relative. */
      if (SDL_GetTouchDeviceType(devices[i]) != SDL_TOUCH_DEVICE_DIRECT)
         continue;

      num_direct++;

      if (sdl->num_touches >= SDL3_MAX_TOUCH)
         continue;

      if (!(fingers = SDL_GetTouchFingers(devices[i], &num_fingers)))
         continue;

      for (j = 0; j < num_fingers && sdl->num_touches < SDL3_MAX_TOUCH; j++)
      {
         sdl->touches[sdl->num_touches].x = fingers[j]->x;
         sdl->touches[sdl->num_touches].y = fingers[j]->y;
         sdl->num_touches++;
      }

      SDL_free(fingers);
   }

   SDL_free(devices);

   /* Count touchscreens only, so a machine whose sole touch device is
    * a trackpad still takes the cheap early-out above. */
   sdl->num_touch_devices = num_direct;
}

/* Translates an SDL_Keymod to a RETROKMOD. */
static uint16_t sdl3_translate_mod(SDL_Keymod smod)
{
   uint16_t mod = 0;

   if (smod & SDL_KMOD_SHIFT)
      mod |= RETROKMOD_SHIFT;
   if (smod & SDL_KMOD_CTRL)
      mod |= RETROKMOD_CTRL;
   if (smod & SDL_KMOD_ALT)
      mod |= RETROKMOD_ALT;
   if (smod & SDL_KMOD_GUI)
      mod |= RETROKMOD_META;
   if (smod & SDL_KMOD_NUM)
      mod |= RETROKMOD_NUMLOCK;
   if (smod & SDL_KMOD_CAPS)
      mod |= RETROKMOD_CAPSLOCK;
   if (smod & SDL_KMOD_SCROLL)
      mod |= RETROKMOD_SCROLLOCK;

   return mod;
}

/* Translates control/modifier keys into their ASCII character counterpart. */
static uint32_t sdl3_translate_control_key(unsigned code, uint16_t mod)
{
   switch (code)
   {
      case RETROK_BACKSPACE:
      case RETROK_TAB:
      case RETROK_RETURN:
      case RETROK_ESCAPE:
      case RETROK_DELETE:
      case RETROK_KP_ENTER:
         return input_keymaps_translate_rk_to_ascii((enum retro_key)code, (enum retro_mod)mod);
      default:
         break;
   }

   return 0;
}

/* Grabs text from the clipboard, and passes it as keyboard input. */
static void sdl3_paste_clipboard(void)
{
   char *text = SDL_GetClipboardText();
   const char *ptr = text;

   if (!text)
      return;

   while (*ptr)
   {
      uint32_t c = utf8_walk(&ptr);

      /* Skip newline and backspace characters, since those would
       * negatively affect the input. */
      if (c >= 0x20 && c != 0x7f)
         input_keyboard_event(true, RETROK_UNKNOWN, c, 0, RETRO_DEVICE_KEYBOARD);
   }

   SDL_free(text);
}

static void sdl3_input_poll(void *data)
{
   SDL_Event event;
   int i;
   sdl3_input_t *sdl = (sdl3_input_t*)data;

   /* SDL only emits keyboard/mouse-wheel events for a window that owns
    * the input focus. Without an SDL3 video driver to create and pump
    * that window, this queue drains nothing and key/wheel state below
    * never updates. */
   SDL_PumpEvents();

   sdl3_poll_mouse(sdl);
   sdl3_poll_touch(sdl);

   sdl3_mouse_clear_wheel(&sdl->mouse);
   for (i = 0; i < sdl->num_mice; i++)
      sdl3_mouse_clear_wheel(&sdl->mice[i]);

   while (SDL_PeepEvents(&event, 1, SDL_GETEVENT, SDL_EVENT_KEY_DOWN, SDL_EVENT_MOUSE_REMOVED) > 0)
   {
      if (     event.type == SDL_EVENT_KEY_DOWN
            || event.type == SDL_EVENT_KEY_UP)
      {
         uint16_t mod  = sdl3_translate_mod(event.key.mod);
         unsigned code = input_keymaps_translate_keysym_to_rk(
               event.key.key);

         /* Allow pasting the clipboard. */
         if (     event.type == SDL_EVENT_KEY_DOWN
               && event.key.key == SDLK_V
               && (event.key.mod & SDL_KMOD_CTRL)
               && input_state_get_ptr()->keyboard_line.enabled)
         {
            sdl3_paste_clipboard();
            continue;
         }

         input_keyboard_event(event.type == SDL_EVENT_KEY_DOWN,
               code, sdl3_translate_control_key(code, mod), mod,
               RETRO_DEVICE_KEYBOARD);
      }
      else if (event.type == SDL_EVENT_TEXT_INPUT)
      {
         const char *text = event.text.text;
         uint16_t mod = sdl3_translate_mod(SDL_GetModState());

         while (text && *text)
            input_keyboard_event(true, RETROK_UNKNOWN,
                  utf8_walk(&text), mod, RETRO_DEVICE_KEYBOARD);
      }
      else if (event.type == SDL_EVENT_MOUSE_WHEEL)
      {
         sdl3_mouse_t *mouse = sdl3_get_mouse(sdl, event.wheel.which);
         float wx = event.wheel.x;
         float wy = event.wheel.y;

         /* FLIPPED = "natural" scrolling: SDL delivers inverted
          * signs and expects the caller to negate them. */
         if (event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED)
         {
            wx = -wx;
            wy = -wy;
         }
         sdl3_mouse_wheel(&sdl->mouse, wx, wy);
         if (mouse)
            sdl3_mouse_wheel(mouse, wx, wy);
      }
      else if (event.type == SDL_EVENT_MOUSE_MOTION)
      {
         /* Only feeds the per-device state; the merged mouse gets its
          * motion from SDL_GetRelativeMouseState in sdl3_poll_mouse. */
         sdl3_mouse_t *mouse = sdl3_get_mouse(sdl, event.motion.which);
         if (mouse)
         {
            mouse->rel_x += event.motion.xrel;
            mouse->rel_y += event.motion.yrel;
         }
      }
      else if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN
            || event.type == SDL_EVENT_MOUSE_BUTTON_UP)
      {
         sdl3_mouse_t *mouse = sdl3_get_mouse(sdl, event.button.which);
         int button = sdl3_translate_mouse_button(event.button.button);
         if (mouse && button >= 0)
            mouse->buttons[button] = event.type == SDL_EVENT_MOUSE_BUTTON_DOWN;
      }
      else if (event.type == SDL_EVENT_MOUSE_ADDED)
         sdl3_mouse_added(sdl, event.mdevice.which);
      else if (event.type == SDL_EVENT_MOUSE_REMOVED)
         sdl3_mouse_removed(sdl, event.mdevice.which);
      else if (event.type == SDL_EVENT_KEYMAP_CHANGED)
         sdl3_build_scancode_lut(sdl);
   }

   /* Fold this frame's per-device motion, mirroring the merged path
    * in sdl3_poll_mouse above. */
   for (i = 0; i < sdl->num_mice; i++)
      sdl3_mouse_fold_motion(&sdl->mice[i]);

   /* Neither range is consumed anywhere: sdl3_poll_touch reads finger
    * state by polling instead of by event, and pens aren't wired up at
    * all. Both fire at device rate for as long as there's contact, so
    * left in the queue they grow until SDL's queue fills and starts
    * refusing pushes - at which point the events that do matter (quit,
    * keys) get dropped along with them. */
   SDL_FlushEvents(SDL_EVENT_FINGER_DOWN,      SDL_EVENT_FINGER_CANCELED);
   SDL_FlushEvents(SDL_EVENT_PEN_PROXIMITY_IN, SDL_EVENT_PEN_AXIS);
}

static void sdl3_grab_mouse(void *data, bool state)
{
   SDL_Window *win = sdl3_input_window();

   if (win)
      SDL_SetWindowMouseGrab(win, state);
}

static uint64_t sdl3_get_capabilities(void *data)
{
   return (1 << RETRO_DEVICE_JOYPAD)
         | (1 << RETRO_DEVICE_MOUSE)
         | (1 << RETRO_DEVICE_KEYBOARD)
         | (1 << RETRO_DEVICE_LIGHTGUN)
         | (1 << RETRO_DEVICE_POINTER)
         | (1 << RETRO_DEVICE_ANALOG);
}

input_driver_t input_sdl3 = {
   sdl3_input_init,
   sdl3_input_poll,
   sdl3_input_state,
   sdl3_input_free,
   sdl3_set_sensor_state,
   NULL,                   /* get_sensor_input */
   sdl3_get_capabilities,
   "sdl3",
   sdl3_grab_mouse,
   NULL,                   /* grab_stdin */
   NULL                    /* keypress_vibrate */
};
