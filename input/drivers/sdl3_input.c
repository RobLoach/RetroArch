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

#include <boolean.h>
#include <string/stdstring.h>
#include <encodings/utf.h>
#include <libretro.h>

#include <SDL3/SDL.h>

#include "../input_keymaps.h"

#include "../../configuration.h"
#include "../../retroarch.h"

#include "../../gfx/common/sdl3_common.h"

/* OVERLAY_MAX_TOUCH */
#define SDL3_MAX_TOUCH 16

typedef struct sdl3_input
{
   /* The keyboard state, provided by SDL_GetKeyboardState(). */
   const bool  *kb_state;
   int          kb_num_keys;
   SDL_Scancode key_scancode_lut[RETROK_LAST];
   float mouse_x;
   float mouse_y;
   float mouse_abs_x;
   float mouse_abs_y;
   bool mouse_l;
   bool mouse_r;
   bool mouse_m;
   bool mouse_b4;
   bool mouse_b5;
   bool mouse_wu;
   bool mouse_wd;
   bool mouse_wl;
   bool mouse_wr;

   /* Number of connected touch devices. Saves having to query them
    * every frame. */
   int      num_touch_devices;
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
            ? SDL_GetScancodeFromKey(
                  (SDL_Keycode)rarch_keysym_lut[i], NULL)
            : SDL_SCANCODE_UNKNOWN;
}

static void *sdl3_input_init(const char *joypad_driver)
{
   sdl3_input_t     *sdl = (sdl3_input_t*)calloc(1, sizeof(*sdl));
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
   int16_t       ret = 0;
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
                     if (     (binds[port][i].key && binds[port][i].key < RETROK_LAST)
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
               if (     (binds[port][id].key && binds[port][id].key < RETROK_LAST)
                     && sdl3_key_pressed(sdl, binds[port][id].key)
                     && (id == RARCH_GAME_FOCUS_TOGGLE || !keyboard_mapping_blocked)
                  )
                  return 1;
            }
         }
         break;
      case RETRO_DEVICE_ANALOG:
         {
            int id_minus_key      = 0;
            int id_plus_key       = 0;
            unsigned id_minus     = 0;
            unsigned id_plus      = 0;
            bool id_plus_valid    = false;
            bool id_minus_valid   = false;

            input_conv_analog_id_to_bind_id(idx, id, id_minus, id_plus);

            id_minus_valid        = binds[port][id_minus].valid;
            id_plus_valid         = binds[port][id_plus].valid;
            id_minus_key          = binds[port][id_minus].key;
            id_plus_key           = binds[port][id_plus].key;

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
         if (config_get_ptr()->uints.input_mouse_index[ port ] == 0)
         {
            switch (id)
            {
               case RETRO_DEVICE_ID_MOUSE_LEFT:
                  return sdl->mouse_l;
               case RETRO_DEVICE_ID_MOUSE_RIGHT:
                  return sdl->mouse_r;
               case RETRO_DEVICE_ID_MOUSE_WHEELUP:
                  return sdl->mouse_wu;
               case RETRO_DEVICE_ID_MOUSE_WHEELDOWN:
                  return sdl->mouse_wd;
               case RETRO_DEVICE_ID_MOUSE_HORIZ_WHEELUP:
                  return sdl->mouse_wr;
               case RETRO_DEVICE_ID_MOUSE_HORIZ_WHEELDOWN:
                  return sdl->mouse_wl;
               case RETRO_DEVICE_ID_MOUSE_X:
                  if (device == RARCH_DEVICE_MOUSE_SCREEN)
                     return (int16_t)sdl->mouse_abs_x;
                  return (int16_t)sdl->mouse_x;
               case RETRO_DEVICE_ID_MOUSE_Y:
                  if (device == RARCH_DEVICE_MOUSE_SCREEN)
                     return (int16_t)sdl->mouse_abs_y;
                  return (int16_t)sdl->mouse_y;
               case RETRO_DEVICE_ID_MOUSE_MIDDLE:
                  return sdl->mouse_m;
               case RETRO_DEVICE_ID_MOUSE_BUTTON_4:
                  return sdl->mouse_b4;
               case RETRO_DEVICE_ID_MOUSE_BUTTON_5:
                  return sdl->mouse_b5;
            }
         }
         break;
      case RETRO_DEVICE_POINTER:
      case RARCH_DEVICE_POINTER_SCREEN:
         {
            video_viewport_t vp         = {0};
            bool screen                 = device ==
               RARCH_DEVICE_POINTER_SCREEN;
            int16_t res_x               = 0;
            int16_t res_y               = 0;
            int16_t res_screen_x        = 0;
            int16_t res_screen_y        = 0;
            int abs_x                   = 0;
            int abs_y                   = 0;
            int16_t pressed             = 0;

            if (id == RETRO_DEVICE_ID_POINTER_COUNT)
               return sdl->num_touches
                     ? sdl->num_touches : (sdl->mouse_l ? 1 : 0);

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
               abs_x   = (int)(sdl->touches[idx].x * (float)vp.full_width);
               abs_y   = (int)(sdl->touches[idx].y * (float)vp.full_height);
               pressed = 1;
            }
            else
            {
               if (idx != 0)
                  return 0;
               abs_x   = (int)sdl->mouse_abs_x;
               abs_y   = (int)sdl->mouse_abs_y;
               pressed = sdl->mouse_l;
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
      /* TODO: update button binds to match other input drivers */
      case RETRO_DEVICE_LIGHTGUN:
      {
         video_viewport_t vp         = {0};
         int16_t res_x               = 0;
         int16_t res_y               = 0;
         int16_t res_screen_x        = 0;
         int16_t res_screen_y        = 0;

         /* Buttons and relative aiming don't depend on viewport
          * translation - handle them first so they keep working even
          * when no viewport info is available. */
         switch (id)
         {
            case RETRO_DEVICE_ID_LIGHTGUN_X:
               return (int16_t)sdl->mouse_x;
            case RETRO_DEVICE_ID_LIGHTGUN_Y:
               return (int16_t)sdl->mouse_y;
            case RETRO_DEVICE_ID_LIGHTGUN_TRIGGER:
               return sdl->mouse_l;
            case RETRO_DEVICE_ID_LIGHTGUN_RELOAD:
               return sdl->mouse_m;
            case RETRO_DEVICE_ID_LIGHTGUN_START:
               return sdl->mouse_r;
            case RETRO_DEVICE_ID_LIGHTGUN_SELECT:
               return sdl->mouse_l && sdl->mouse_r;
            default:
               break;
         }

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
            }
         }
         break;
      }
   }

   return 0;
}

static void sdl3_input_free(void *data)
{
   sdl3_input_t *sdl = (sdl3_input_t*)data;

   if (!sdl)
      return;

   /* Flush out all pending events. */
   SDL_FlushEvents(SDL_EVENT_FIRST, SDL_EVENT_LAST);

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

static void sdl3_poll_mouse(sdl3_input_t *sdl)
{
   /* SDL3 reports mouse coordinates as floats; keep them at full
    * precision here and only narrow at the libretro API boundary. */
   Uint32 btn = SDL_GetRelativeMouseState(&sdl->mouse_x, &sdl->mouse_y);

   /* Absolute position is relative to the focused SDL window's client
    * area. With no SDL3 window yet it stays at the origin, so the
    * pointer/lightgun viewport mapping below effectively reads (0,0). */
   SDL_GetMouseState(&sdl->mouse_abs_x, &sdl->mouse_abs_y);

   /* SDL reports mouse coordinates in window coordinates (points),
    * while the video driver's viewport metrics are in output pixels -
    * on HiDPI displays these differ by the window's pixel density.
    * Scale so the pointer/lightgun viewport mapping lines up. */
   {
      SDL_Window *win = SDL_GetMouseFocus();
      if (win)
      {
         float density = SDL_GetWindowPixelDensity(win);
         if (density > 0.0f && density != 1.0f)
         {
            sdl->mouse_abs_x *= density;
            sdl->mouse_abs_y *= density;
         }
      }
   }

   sdl->mouse_l  = (SDL_BUTTON_MASK(SDL_BUTTON_LEFT)   & btn) != 0;
   sdl->mouse_r  = (SDL_BUTTON_MASK(SDL_BUTTON_RIGHT)  & btn) != 0;
   sdl->mouse_m  = (SDL_BUTTON_MASK(SDL_BUTTON_MIDDLE) & btn) != 0;
   sdl->mouse_b4 = (SDL_BUTTON_MASK(SDL_BUTTON_X1)     & btn) != 0;
   sdl->mouse_b5 = (SDL_BUTTON_MASK(SDL_BUTTON_X2)     & btn) != 0;
}

/* Snapshot the active fingers across all touch devices. Polling
 * SDL_GetTouchFingers avoids tracking finger-id lifetimes through
 * SDL_EVENT_FINGER_* events by hand. */
static void sdl3_poll_touch(sdl3_input_t *sdl)
{
   int          i;
   SDL_TouchID *devices = NULL;

   sdl->num_touches = 0;

   /* Skip the per-frame device query on touch-less setups; re-probe
    * every ~10s since SDL3 has no touch hotplug event. */
   if (sdl->num_touch_devices == 0)
   {
      if (++sdl->touch_recheck < 600)
         return;
      sdl->touch_recheck = 0;
   }

   devices = SDL_GetTouchDevices(&sdl->num_touch_devices);

   if (!devices)
      return;

   for (i = 0; i < sdl->num_touch_devices && sdl->num_touches < SDL3_MAX_TOUCH; i++)
   {
      int          j, num_fingers = 0;
      SDL_Finger **fingers        = SDL_GetTouchFingers(devices[i],
            &num_fingers);

      if (!fingers)
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
}

static uint16_t sdl3_translate_mod(SDL_Keymod smod)
{
   uint16_t mod = 0;

   if (smod & SDL_KMOD_SHIFT)
      mod |= RETROKMOD_SHIFT;
   if (smod & SDL_KMOD_CTRL)
      mod |= RETROKMOD_CTRL;
   if (smod & SDL_KMOD_ALT)
      mod |= RETROKMOD_ALT;
   if (smod & SDL_KMOD_NUM)
      mod |= RETROKMOD_NUMLOCK;
   if (smod & SDL_KMOD_CAPS)
      mod |= RETROKMOD_CAPSLOCK;
   if (smod & SDL_KMOD_SCROLL)
      mod |= RETROKMOD_SCROLLOCK;

   return mod;
}

static void sdl3_input_poll(void *data)
{
   SDL_Event event;
   sdl3_input_t *sdl = (sdl3_input_t*)data;

   /* SDL only emits keyboard/mouse-wheel events for a window that owns
    * the input focus. Without an SDL3 video driver to create and pump
    * that window, this queue drains nothing and key/wheel state below
    * never updates. */
   SDL_PumpEvents();

   sdl3_poll_mouse(sdl);
   sdl3_poll_touch(sdl);

   /* Wheel state is edge-triggered: it should only report motion that
    * occurred this frame, so clear it before draining events - else a
    * single notch would keep scrolling forever. */
   sdl->mouse_wu = false;
   sdl->mouse_wd = false;
   sdl->mouse_wl = false;
   sdl->mouse_wr = false;

   while (SDL_PeepEvents(&event, 1,
            SDL_GETEVENT, SDL_EVENT_KEY_DOWN, SDL_EVENT_MOUSE_WHEEL) > 0)
   {
      if (     event.type == SDL_EVENT_KEY_DOWN
            || event.type == SDL_EVENT_KEY_UP)
      {
         uint16_t mod  = sdl3_translate_mod(event.key.mod);
         unsigned code = input_keymaps_translate_keysym_to_rk(
               event.key.key);

         /* Character 0: typed characters are delivered separately
          * through SDL_EVENT_TEXT_INPUT below (mirroring the win32
          * WM_KEYDOWN / WM_CHAR split), so don't also synthesize one
          * from the keycode or text entry would double up. */
         input_keyboard_event(event.type == SDL_EVENT_KEY_DOWN,
               code, 0, mod, RETRO_DEVICE_KEYBOARD);
      }
      else if (event.type == SDL_EVENT_TEXT_INPUT)
      {
         /* Proper text entry (shifted symbols, non-ASCII, IME) for
          * menu text fields and core keyboard callbacks. These
          * events only flow because the SDL3 video driver calls
          * SDL_StartTextInput on its window. */
         const char *text = event.text.text;
         uint16_t    mod  = sdl3_translate_mod(SDL_GetModState());

         while (text && *text)
            input_keyboard_event(true, RETROK_UNKNOWN,
                  utf8_walk(&text), mod, RETRO_DEVICE_KEYBOARD);
      }
      else if (event.type == SDL_EVENT_MOUSE_WHEEL)
      {
         /* OR-accumulate: wheel state was cleared before the drain,
          * so multiple notches in one frame all register. */
         sdl->mouse_wu |= event.wheel.y > 0;
         sdl->mouse_wd |= event.wheel.y < 0;
         sdl->mouse_wl |= event.wheel.x < 0;
         sdl->mouse_wr |= event.wheel.x > 0;
      }
      else if (event.type == SDL_EVENT_KEYMAP_CHANGED)
         sdl3_build_scancode_lut(sdl);
   }
}

static void sdl3_grab_mouse(void *data, bool state)
{
   sdl3_video_t *video_ptr = NULL;

   if (string_is_not_equal(video_driver_get_ident(), "sdl3"))
      return;

   video_ptr = (sdl3_video_t*)video_driver_get_ptr();

   if (video_ptr)
      SDL_SetWindowMouseGrab(video_ptr->window, state);
}

static uint64_t sdl3_get_capabilities(void *data)
{
   return
           (1 << RETRO_DEVICE_JOYPAD)
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
