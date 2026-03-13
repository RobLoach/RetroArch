/*  RetroArch - A frontend for libretro.
 *  Copyright (C) 2011-2019 - Daniel De Matteis
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

#ifdef HAVE_CONFIG_H
#include "../../config.h"
#endif

#include <retro_miscellaneous.h>

#include "sdl3_common.h"
#include "../../retroarch.h"

#include <SDL3/SDL.h>

void sdl3_set_handles(void *data, enum rarch_display_type display_type)
{
   SDL_Window       *window = (SDL_Window*)data;
   SDL_PropertiesID  props  = SDL_GetWindowProperties(window);

   video_driver_display_userdata_set((uintptr_t)window);

   switch (display_type)
   {
      case RARCH_DISPLAY_WIN32:
#if defined(_WIN32)
         video_driver_display_type_set(RARCH_DISPLAY_WIN32);
         video_driver_display_set(0);
         video_driver_window_set((uintptr_t)SDL_GetPointerProperty(props,
               SDL_PROP_WINDOW_WIN32_HWND_POINTER, NULL));
#endif
         break;
      case RARCH_DISPLAY_X11:
#if defined(HAVE_X11)
         video_driver_display_type_set(RARCH_DISPLAY_X11);
         video_driver_display_set((uintptr_t)SDL_GetPointerProperty(props,
               SDL_PROP_WINDOW_X11_DISPLAY_POINTER, NULL));
         video_driver_window_set((uintptr_t)SDL_GetNumberProperty(props,
               SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0));
#endif
         break;
      case RARCH_DISPLAY_OSX:
#ifdef HAVE_COCOA
         video_driver_display_type_set(RARCH_DISPLAY_OSX);
         video_driver_display_set(0);
         video_driver_window_set((uintptr_t)SDL_GetPointerProperty(props,
               SDL_PROP_WINDOW_COCOA_WINDOW_POINTER, NULL));
#endif
         break;
      case RARCH_DISPLAY_WAYLAND:
#if defined(HAVE_WAYLAND)
         video_driver_display_type_set(RARCH_DISPLAY_WAYLAND);
         video_driver_display_set((uintptr_t)SDL_GetPointerProperty(props,
               SDL_PROP_WINDOW_WAYLAND_DISPLAY_POINTER, NULL));
         video_driver_window_set((uintptr_t)SDL_GetPointerProperty(props,
               SDL_PROP_WINDOW_WAYLAND_SURFACE_POINTER, NULL));
#endif
         break;
      default:
      case RARCH_DISPLAY_NONE:
         break;
   }
}
