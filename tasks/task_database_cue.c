/*  RetroArch - A frontend for libretro.
 *  Copyright (C) 2011-2017 - Daniel De Matteis
 *  Copyright (C) 2014-2017 - Jean-André Santoni
 *  Copyright (C) 2016-2019 - Brad Parker
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

#include <errno.h>
#include <ctype.h>
#include <string.h>

#include <retro_miscellaneous.h>
#include <compat/strcasestr.h>
#include <compat/strl.h>
#include <file/file_path.h>
#include <retro_endianness.h>
#include <streams/file_stream.h>
#include <streams/interface_stream.h>
#include <string/stdstring.h>

#ifdef HAVE_CONFIG_H
#include "../config.h"
#endif

#include "../database_info.h"

#include "tasks_internal.h"

#include "../list_special.h"
#include "../msg_hash.h"
#include "../verbosity.h"

#define MAGIC_LEN       17
#define MAX_TOKEN_LEN   255

#ifdef MSB_FIRST
#define MODETEST_VAL    0x00ffffff
#else
#define MODETEST_VAL    0xffffff00
#endif

struct magic_entry
{
   int32_t offset;
   const char *system_name;
   const char *magic;
   int length_magic;
};

static struct magic_entry MAGIC_NUMBERS[] = {
   { 0x008008,     "psp",        "\x50\x53\x50\x20\x47\x41\x4d\x45",                                      8},	
   { 0x008008,     "ps1",        "\x50\x4c\x41\x59\x53\x54\x41\x54\x49\x4f\x4e",                          11},	
   { 0x00001c,     "gc",         "\xc2\x33\x9f\x3d",                                                      4},	
   { 0,            "scd",        "\x53\x45\x47\x41\x44\x49\x53\x43\x53\x59\x53\x54\x45\x4d",              14},
   { 0,            "sat",        "\x53\x45\x47\x41\x20\x53\x45\x47\x41\x53\x41\x54\x55\x52\x4e",          15},
   { 0,            "dc",         "\x53\x45\x47\x41\x20\x53\x45\x47\x41\x4b\x41\x54\x41\x4e\x41",          15},
   /** [WIP] The following systems still need a dectct serial function and if not detected will be
             captured by detect_serial_ascii_game function.   
   { 0x000018,   "wii",        "\x5d\x1c\x9e\xa3",                                                      4},
   { 0x800008,   "cdi",        "\x43\x44\x2d\x52\x54\x4f\x53",                                          7},
   { 0x000820,   "pcecd",      "\x50\x43\x20\x45\x6e\x67\x69\x6e\x65\x20\x43\x44\x2d\x52\x4f\x4d",      16}, **/
   { 0,           NULL,        NULL,                                                                    0}
};

static int64_t get_token(intfstream_t *fd, char *token, uint64_t max_len)
{
   char *c       = token;
   int64_t len   = 0;
   int in_string = 0;

   for (;;)
   {
      int64_t rv = (int64_t)intfstream_read(fd, c, 1);
      if (rv == 0)
         return 0;

      if (rv < 1)
      {
         switch (errno)
         {
            case EINTR:
            case EAGAIN:
               continue;
            default:
               return -errno;
         }
      }

      switch (*c)
      {
         case ' ':
         case '\t':
         case '\r':
         case '\n':
            if (c == token)
               continue;

            if (!in_string)
            {
               *c = '\0';
               return len;
            }
            break;
         case '\"':
            if (c == token)
            {
               in_string = 1;
               continue;
            }

            *c = '\0';
            return len;
      }

      len++;
      c++;
      if (len == (int64_t)max_len)
      {
         *c = '\0';
         return len;
      }
   }
}

static int detect_ps1_game_sub(intfstream_t *fp,
      char *game_id, int sub_channel_mixed)
{
   uint8_t* tmp;
   uint8_t* boot_file;
   int skip, frame_size, cd_sector;
   uint8_t buffer[2048 * 2];
   int is_mode1 = 0;

   buffer[0]    = '\0';

   if (intfstream_seek(fp, 0, SEEK_END) == -1)
      return 0;

   if (!sub_channel_mixed)
   {
      if (!(intfstream_tell(fp) & 0x7FF))
      {
         unsigned int mode_test = 0;

         if (intfstream_seek(fp, 0, SEEK_SET) == -1)
            return 0;

         intfstream_read(fp, &mode_test, 4);
         if (mode_test != MODETEST_VAL)
            is_mode1 = 1;
      }
   }

   skip       = is_mode1? 0: 24;
   frame_size = sub_channel_mixed? 2448: is_mode1? 2048: 2352;

   if (intfstream_seek(fp, 156 + skip + 16 * frame_size, SEEK_SET) == -1)
      return 0;

   intfstream_read(fp, buffer, 6);

   cd_sector = buffer[2] | (buffer[3] << 8) | (buffer[4] << 16);

   if (intfstream_seek(fp, skip + cd_sector * frame_size, SEEK_SET) == -1)
      return 0;
   intfstream_read(fp, buffer, 2048 * 2);

   tmp = buffer;
   while (tmp < (buffer + 2048 * 2))
   {
      if (!*tmp)
         return 0;

      if (!strncasecmp((const char*)(tmp + 33), "SYSTEM.CNF;1", 12))
         break;

      tmp += *tmp;
   }

   if (tmp >= (buffer + 2048 * 2))
      return 0;

   cd_sector = tmp[2] | (tmp[3] << 8) | (tmp[4] << 16);
   if (intfstream_seek(fp, skip + cd_sector * frame_size, SEEK_SET) == -1)
      return 0;

   intfstream_read(fp, buffer, 256);
   buffer[256] = '\0';

   tmp = buffer;
   while(*tmp && strncasecmp((const char*)tmp, "boot", 4))
      tmp++;

   if (!*tmp)
      return 0;

   boot_file = tmp;
   while(*tmp && *tmp != '\n')
   {
      if ((*tmp == '\\') || (*tmp == ':'))
         boot_file = tmp + 1;

      tmp++;
   }

   tmp = boot_file;
   *game_id++ = toupper(*tmp++);
   *game_id++ = toupper(*tmp++);
   *game_id++ = toupper(*tmp++);
   *game_id++ = toupper(*tmp++);
   *game_id++ = '-';

   if (!isalnum(*tmp))
      tmp++;

   while(isalnum(*tmp))
   {
      *game_id++ = *tmp++;
      if (*tmp == '.')
         tmp++;
   }

   *game_id = 0;

   return 1;
}

int detect_ps1_game(intfstream_t *fd, char *game_id)
{
   if (detect_ps1_game_sub(fd, game_id, 0))
      return 1;

   return detect_ps1_game_sub(fd, game_id, 1);
}

int detect_psp_game(intfstream_t *fd, char *game_id)
{
   unsigned pos;
   bool rv   = false;

   for (pos = 0; pos < 100000; pos++)
   {
      intfstream_seek(fd, pos, SEEK_SET);

      if (intfstream_read(fd, game_id, 5) > 0)
      {
         game_id[5] = '\0';

         if (
               (string_is_equal(game_id, "ULES-"))
               || (string_is_equal(game_id, "ULUS-"))
               || (string_is_equal(game_id, "ULJS-"))

               || (string_is_equal(game_id, "ULEM-"))
               || (string_is_equal(game_id, "ULUM-"))
               || (string_is_equal(game_id, "ULJM-"))

               || (string_is_equal(game_id, "UCES-"))
               || (string_is_equal(game_id, "UCUS-"))
               || (string_is_equal(game_id, "UCJS-"))
               || (string_is_equal(game_id, "UCAS-"))
               || (string_is_equal(game_id, "UCKS-"))

               || (string_is_equal(game_id, "ULKS-"))
               || (string_is_equal(game_id, "ULAS-"))

               || (string_is_equal(game_id, "NPEH-"))
               || (string_is_equal(game_id, "NPUH-"))
               || (string_is_equal(game_id, "NPJH-"))
               || (string_is_equal(game_id, "NPHH-"))

               || (string_is_equal(game_id, "NPEG-"))
               || (string_is_equal(game_id, "NPUG-"))
               || (string_is_equal(game_id, "NPJG-"))
               || (string_is_equal(game_id, "NPHG-"))

               || (string_is_equal(game_id, "NPEZ-"))
               || (string_is_equal(game_id, "NPUZ-"))
               || (string_is_equal(game_id, "NPJZ-"))
               )
               {
                  intfstream_seek(fd, pos, SEEK_SET);
                  if (intfstream_read(fd, game_id, 10) > 0)
                  {
#if 0
                     game_id[4] = '-';
                     game_id[8] = game_id[9];
                     game_id[9] = game_id[10];
#endif
                     game_id[10] = '\0';
                     rv = true;
                  }
                  break;
               }
      }
      else
         break;
   }

   return rv;
}

int detect_gc_game(intfstream_t *fd, char *game_id)
{
   char region_id;
   char * prefix = "DL-DOL-", pre_game_id[50], raw_game_id[50];
   int x = 7;
   int y = 11;
   
   intfstream_seek(fd, 0, SEEK_SET);

   if (intfstream_read(fd, raw_game_id, 4) > 0)
   {
      raw_game_id[4] = '\0';
   }
   else
   {
      return false;
   }

   /** convert raw gamecube serial to redump serial.
   not enough is known about the disc data to properly
   convert every raw serial to redump serial.  it will
   only fail with the following excpetions:  the disc
   is a multi disc game, and the subregions of europe
   P-UKV, P-AUS, X-UKV, X-EUU will not match redump.**/
   
   /** insert prefix **/
   strncpy(pre_game_id, prefix, x);
   pre_game_id[x] = '\0';
   strcat(pre_game_id, raw_game_id);

   /** NYI: This perl code checks filename looking for multi disc to add to the serial **/
   /** if ($element =~ m/\(Disc 1/i or $element =~ m/\(Disk 1/i) {
      $game_id = $game_id . "-0";
   } elsif ($element =~ m/\(Disc 2/i or $element =~ m/\(Disk 2/i) {
      $game_id = $game_id . "-1";
   } **/

   /** check region then insert region suffix **/
   region_id = pre_game_id[10];

   strncpy(game_id, pre_game_id, y);
   game_id[y] = '\0';
   
   switch (region_id)
   {
      case 'E':
         strcat(game_id + y, "-USA");  
         return true;
      case 'J':
         strcat(game_id + y, "-JPN");  
         return true;
      case 'P': /** NYI: P can also be P-UKV, P-AUS **/
         strcat(game_id + y, "-EUR");  
         return true;
      case 'X': /** NYI: X can also be X-UKV, X-EUU **/
         strcat(game_id + y, "-EUR");  
         return true;
      case 'Y':
         strcat(game_id + y, "-FAH");  
         return true;
      case 'D':
         strcat(game_id + y, "-NOE");  
         return true;
      case 'S':
         strcat(game_id + y, "-ESP"); 
         return true;
      case 'F':
         strcat(game_id + y, "-FRA");  
         return true;
      case 'I':
         strcat(game_id + y, "-ITA");  
         return true;
      case 'H':
         strcat(game_id + y, "-HOL");  
         return true;
   }

   return false;
}

void remove_spaces (char* str_trimmed, const char* str_untrimmed)
{
   while (*str_untrimmed != '\0')
   {
      if(!isspace(*str_untrimmed))
      {
         *str_trimmed = *str_untrimmed;
         str_trimmed++;
      }
      str_untrimmed++;
   }
   *str_trimmed = '\0';
}

int index_last_occurance(char str[], char t)
{
   const char * ret = strrchr(str, t);
   if (ret) return ret-str;
   else return -1;
}

int detect_scd_game(intfstream_t *fd, char *game_id)
{
   char hyphen = '-';
   char pre_game_id[50];
   char raw_game_id[50];
   char check_prefix_t_hyp[10];
   char check_suffix_50[10];
   char check_prefix_g_hyp[10];
   char check_prefix_mk_hyp[10];
   int length;
   int lengthref;
   int index;
   char lgame_id[10];
   char * rgame_id = "-50";
   
   if (intfstream_seek(fd, 0x0183, SEEK_SET) >= 0)
   {

      if (intfstream_read(fd, raw_game_id, 11) > 0)
      {
         raw_game_id[11] = '\0';
      }
      else
      {
         return false;
      }
   }
   else
   {
      return false;
   }
   /** convert raw Sega - Mega-CD - Sega CD serial to redump serial. **/
   
   /** process raw serial to a pre serial without spaces **/
   remove_spaces(pre_game_id, raw_game_id);  /** rule: remove all spaces from the raw serial globally **/ 

   /** disect this pre serial into parts **/
   length = strlen(pre_game_id);
   lengthref = length - 2;
   strncpy(check_prefix_t_hyp, pre_game_id, 2);
   check_prefix_t_hyp[2] = '\0';
   strncpy(check_prefix_g_hyp, pre_game_id, 2);
   check_prefix_g_hyp[2] = '\0';
   strncpy(check_prefix_mk_hyp, pre_game_id, 3);
   check_prefix_mk_hyp[3] = '\0';
   strncpy(check_suffix_50, &pre_game_id[lengthref], length - 2 + 1);
   check_suffix_50[2] = '\0';
   
   /** redump serials are built differently for each prefix **/
   if (!strcmp(check_prefix_t_hyp, "T-") || !strcmp(check_prefix_g_hyp, "G-"))
   {
      index = index_last_occurance(pre_game_id, hyphen);
      if (index == -1)
         return false;
      strncpy(game_id, pre_game_id, index);  
      return true;
   }
   else if (strcmp(check_prefix_mk_hyp, "MK-") == 0)
   {
      if (strcmp(check_suffix_50, "50") == 0)
      {
         strncpy(lgame_id, &pre_game_id[3], 4);
         lgame_id[4] = '\0';
         strcat(game_id, lgame_id);
         strcat(game_id, rgame_id);
         return true;
      }
      else
      {
         strncpy(game_id, &pre_game_id[3], 4);
         return true;
      }
   }

   return false;
}

void left_and_right_trim_spaces(char *s)
{
   int  i,j;

   for(i=0;s[i]==' '||s[i]=='\t';i++);
	
   for(j=0;s[i];i++)
   {
      s[j++]=s[i];
   }
   s[j]='\0';
   for(i=0;s[i]!='\0';i++)
   {
      if(s[i]!=' '&& s[i]!='\t')
         j=i;
   }
   s[j+1]='\0';
}

int detect_sat_game(intfstream_t *fd, char *game_id)
{
   char hyphen = '-';
   char raw_game_id[50];
   char raw_region_id[10];
   char region_id;
   char check_prefix_t_hyp[10];
   char check_prefix_mk_hyp[10];
   int length;
   char lgame_id[10];
   char rgame_id[10];
   char * game_id50 = "-50";
   
   if (intfstream_seek(fd, 0x0020, SEEK_SET) >= 0)
   {

      if (intfstream_read(fd, raw_game_id, 9) > 0)
      {
         raw_game_id[9] = '\0';
      }
      else
      {
         return false;
      }
   }
   else
   {
      return false;
   }
   
   if (intfstream_seek(fd, 0x0040, SEEK_SET) >= 0)
   {

      if (intfstream_read(fd, raw_region_id, 1) > 0)
      {
         raw_game_id[1] = '\0';
      }
   }
   else
   {
      return false;
   }
   
   region_id = raw_region_id[0];
   
   left_and_right_trim_spaces(raw_game_id);

   /** disect this raw serial into parts **/
   strncpy(check_prefix_t_hyp, raw_game_id, 2);
   check_prefix_t_hyp[2] = '\0';
   strncpy(check_prefix_mk_hyp, raw_game_id, 3);
   check_prefix_mk_hyp[3] = '\0';
   length = strlen(raw_game_id);
   raw_game_id[length] = '\0';
         
   /** redump serials are built differently for each region **/
   switch (region_id)
   {
      case 'U':
         if (strcmp(check_prefix_mk_hyp, "MK-") == 0)
         {
            strncpy(game_id, &raw_game_id[3], length - 3);
            game_id[length - 3] = '\0';
            return true;
         }
         else
         {
            strncpy(game_id, &raw_game_id[0], length);
            game_id[length] = '\0';
            return true;
         }
      case 'E':
         strncpy(lgame_id, &raw_game_id[0], 2);
         lgame_id[2] = '\0';
         strncpy(rgame_id, &raw_game_id[2], length - 1);
         rgame_id[length - 1] = '\0';
         strcat(game_id, lgame_id);
         strcat(game_id, rgame_id);
         strcat(game_id, game_id50);
         return true;
      case 'J':
         strncpy(game_id, &raw_game_id[0], length);
         game_id[length] = '\0';
         return true;
   }
   
   return false;
}

int count_occurances_single_character(char *str, char t)
{
   int ctr = 0;
   int index = -1;
   int i;
    
   for (i = 0; str[i] != '\0'; ++i) {
      if (t == str[i])
         ++ctr;
   }

   index = ctr;
   return index;
}

void replace_space_with_single_character(char *str, char t)
{
   int new_char;
   int ctr=0; 
   char *dest = str;
	
   while (str[ctr])
   {
      new_char=str[ctr];
      if (isspace(new_char)) 
      new_char=t;
      *dest++ = new_char;
      ctr++;
   }

   *dest = '\0';
}

void replace_multi_space_with_single_space(char *str)
{
   char *dest = str;

   while (*str != '\0')
   {
      while (*str == ' ' && *(str + 1) == ' ')
      str++;

      *dest++ = *str++;
   }
  
   *dest = '\0';
}

int detect_dc_game(intfstream_t *fd, char *game_id)
{
   char hyphen = '-';
   char hyphen_str[] = "-";
   int total_hyphens;
   int total_hyphens_recalc;
   char pre_game_id[50];
   char raw_game_id[50];
   char check_prefix_t_hyp[10];
   char check_prefix_t[10];
   char check_prefix_hdr_hyp[10];
   char check_prefix_mk_hyp[10];
   int length;
   int length_recalc;
   int index;
   size_t size_t_var;
   char lgame_id[20];
   char rgame_id[20];

   if (intfstream_seek(fd, 0x0040, SEEK_SET) >= 0)
   {

      if (intfstream_read(fd, raw_game_id, 10) > 0)
      {
         raw_game_id[10] = '\0';
      }
      else
      {
         return false;
      }
   }
   else
   {
      return false;
   }
   
   left_and_right_trim_spaces(raw_game_id);
   replace_multi_space_with_single_space(raw_game_id);
   replace_space_with_single_character(raw_game_id, hyphen);
   length = strlen(raw_game_id);
   raw_game_id[length] = '\0';
   total_hyphens = count_occurances_single_character(raw_game_id, hyphen);
   
   /** disect this raw serial into parts **/
   strncpy(check_prefix_t_hyp, raw_game_id, 2);
   check_prefix_t_hyp[2] = '\0';
   strncpy(check_prefix_t, raw_game_id, 1);
   check_prefix_t[1] = '\0';
   strncpy(check_prefix_hdr_hyp, raw_game_id, 4);
   check_prefix_hdr_hyp[4] = '\0';
   strncpy(check_prefix_mk_hyp, raw_game_id, 3);
   check_prefix_mk_hyp[3] = '\0';

   /** redump serials are built differently for each prefix **/
   if (!strcmp(check_prefix_t_hyp, "T-"))
   {
      if (total_hyphens >= 2)
      {
         index = index_last_occurance(raw_game_id, hyphen);
         if (index < 0)
         {
            return false;
         }
         else
         {
            size_t_var = (size_t)index;
         }
         strncpy(lgame_id, &raw_game_id[0], size_t_var);
         lgame_id[index] = '\0';
         strncpy(rgame_id, &raw_game_id[index + 1], length - 1);
         rgame_id[length - 1] = '\0';
         strcat(game_id, lgame_id);
         strcat(game_id, hyphen_str);
         strcat(game_id, rgame_id);
         return true;
      }
      else if (total_hyphens == 1)
      {
         if (length <= 7)
         {
            strncpy(game_id, raw_game_id, 7);
            game_id[7] = '\0';
            return true;
         } 
         else if (length >= 8)
         {
            strncpy(lgame_id, raw_game_id, 7);
            lgame_id[7] = '\0'; 
            strncpy(rgame_id, &raw_game_id[length - 2], length - 1);
            rgame_id[length - 1] = '\0';
            strcat(game_id, lgame_id);
            strcat(game_id, hyphen_str);
            strcat(game_id, rgame_id);
            return true;
         }
      }
   } else if (!strcmp(check_prefix_t, "T"))
   {
      strncpy(lgame_id, raw_game_id, 1);
      lgame_id[1] = '\0';  			  
      strncpy(rgame_id, &raw_game_id[1], length - 1);
      rgame_id[length - 1] = '\0';		  
      strcat(pre_game_id, lgame_id);
      strcat(pre_game_id, hyphen_str);
      strcat(pre_game_id, rgame_id);
      
      total_hyphens_recalc = count_occurances_single_character(pre_game_id, hyphen);
      
      if (total_hyphens_recalc >= 2)
      {
         index = index_last_occurance(pre_game_id, hyphen);
         if (index < 0)
         {
            return false;
         }
         else
         {
            size_t_var = (size_t)index;
         }
         strncpy(lgame_id, pre_game_id, index);
         lgame_id[index] = '\0';
         length_recalc = strlen(pre_game_id);
         strncpy(rgame_id, &pre_game_id[length_recalc - 2], length_recalc - 1);
         rgame_id[length_recalc - 1] = '\0';
         strcat(game_id, lgame_id);
         strcat(game_id, hyphen_str);
         strcat(game_id, rgame_id);
         return true;
      }
      else if (total_hyphens_recalc == 1)
      {
         length_recalc = strlen(pre_game_id) - 1;
         if (length_recalc <= 8)
         {
            strncpy(game_id, pre_game_id, 9);
            game_id[9] = '\0';
            return true;
         }
         else if (length_recalc >= 9)
         {
            strncpy(lgame_id, pre_game_id, 7);
            lgame_id[7] = '\0';
            strncpy(rgame_id, &pre_game_id[length_recalc - 2], length_recalc - 1);
            rgame_id[length_recalc - 1] = '\0';
            strcat(game_id, lgame_id);
            strcat(game_id, hyphen_str);
            strcat(game_id, rgame_id);
            return true;
         }
      }
   }
   else if (!strcmp(check_prefix_hdr_hyp, "HDR-"))
   {
      if (total_hyphens >= 2)
      {
         index = index_last_occurance(raw_game_id, hyphen);
         if (index < 0)
         {
            return false;
         }
         else
         {
            size_t_var = (size_t)index;
         }
         strncpy(lgame_id, raw_game_id, index - 1);
         lgame_id[index - 1] = '\0';
         strncpy(rgame_id, &raw_game_id[length - 4], length - 3);
         rgame_id[length - 3] = '\0';
         strcat(game_id, lgame_id);
         strcat(game_id, hyphen_str);
         strcat(game_id, rgame_id);
         return true;
      }
      else
      {
         strcpy(game_id, raw_game_id);
         return true;
      }
   }
   else if (!strcmp(check_prefix_mk_hyp, "MK-"))
   {
      if (length <= 8)
      {
         strncpy(game_id, raw_game_id, 8);
         game_id[8] = '\0';
         return true;
      }
      else if (length >= 9)
      {
         strncpy(lgame_id, raw_game_id, 8);
         lgame_id[8] = '\0';
         strncpy(rgame_id, &raw_game_id[length - 2], length - 1);
         rgame_id[length - 1] = '\0';				  
         strcat(game_id, lgame_id);
         strcat(game_id, hyphen_str);
         strcat(game_id, rgame_id); 				  
      }
   }
   
   return false;
}

/**
 * Check for an ASCII serial in the first few bits of the ISO (Wii).
 */
int detect_serial_ascii_game(intfstream_t *fd, char *game_id)
{
   unsigned pos;
   int numberOfAscii = 0;
   bool rv   = false;

   for (pos = 0; pos < 10000; pos++)
   {
      intfstream_seek(fd, pos, SEEK_SET);
      if (intfstream_read(fd, game_id, 15) > 0)
      {
         unsigned i;
         game_id[15] = '\0';
         numberOfAscii = 0;

         /* When scanning WBFS files, "WBFS" is discovered as the first serial. Ignore it. */
         if (string_is_equal(game_id, "WBFS")) {
            continue;
         }

         /* Loop through until we run out of ASCII characters. */
         for (i = 0; i < 15; i++)
         {
            /* Is the given character ASCII? A-Z, 0-9, - */
            if (game_id[i] == 45 || (game_id[i] >= 48 && game_id[i] <= 57) || (game_id[i] >= 65 && game_id[i] <= 90))
               numberOfAscii++;
            else
               break;
         }

         /* If the length of the text is between 3 and 9 characters, it could be a serial. */
         if (numberOfAscii > 3 && numberOfAscii < 9)
         {
            /* Cut the string off, and return it as a valid serial. */
            game_id[numberOfAscii] = '\0';
            rv = true;
            break;
         }
      }
   }

   return rv;
}

int detect_system(intfstream_t *fd, const char **system_name)
{
   int i;
   char magic[50];

   RARCH_LOG("%s\n", msg_hash_to_str(MSG_COMPARING_WITH_KNOWN_MAGIC_NUMBERS));
   for (i = 0; MAGIC_NUMBERS[i].system_name != NULL; i++)
   {
      if (intfstream_seek(fd, MAGIC_NUMBERS[i].offset, SEEK_SET) >= 0)
      {
         if (intfstream_read(fd, magic, MAGIC_NUMBERS[i].length_magic) > 0)
         {
            magic[MAGIC_NUMBERS[i].length_magic] = '\0';
            RARCH_LOG("=== Got here === Offset: %d\n", MAGIC_NUMBERS[i].offset);
            RARCH_LOG("=== Got here === Read magic: %s\n", magic);
            RARCH_LOG("=== Got here === Data magic: %s\n", MAGIC_NUMBERS[i].magic);
            if (memcmp(MAGIC_NUMBERS[i].magic, magic, MAGIC_NUMBERS[i].length_magic) == 0)
            {
               *system_name = MAGIC_NUMBERS[i].system_name;
               return true;
            }
         }
      }
   }
   
   return false;
}

static int64_t intfstream_get_file_size(const char *path)
{
   int64_t rv;
   intfstream_t *fd = intfstream_open_file(path,
         RETRO_VFS_FILE_ACCESS_READ, RETRO_VFS_FILE_ACCESS_HINT_NONE);
   if (!fd)
      return -1;
   rv = intfstream_get_size(fd);
   intfstream_close(fd);
   free(fd);
   return rv;
}

static bool update_cand(int64_t *cand_index, int64_t *last_index,
                        uint64_t *largest, char *last_file, uint64_t *offset,
                        uint64_t *size, char *track_path, uint64_t max_len)
{
   if (*cand_index != -1)
   {
      if ((uint64_t)(*last_index - *cand_index) > *largest)
      {
         *largest    = *last_index - *cand_index;
         strlcpy(track_path, last_file, (size_t)max_len);
         *offset     = *cand_index;
         *size       = *largest;
         *cand_index = -1;
         return true;
      }
      *cand_index = -1;
   }
   return false;
}

int cue_find_track(const char *cue_path, bool first,
      uint64_t *offset, uint64_t *size, char *track_path, uint64_t max_len)
{
   int rv;
   intfstream_info_t info;
   char *tmp_token            = (char*)malloc(MAX_TOKEN_LEN);
   char *last_file            = (char*)malloc(PATH_MAX_LENGTH + 1);
   intfstream_t *fd           = NULL;
   int64_t last_index         = -1;
   int64_t cand_index         = -1;
   int32_t cand_track         = -1;
   int32_t track              = 0;
   uint64_t largest             = 0;
   int64_t volatile file_size = -1;
   bool is_data               = false;
   char *cue_dir              = (char*)malloc(PATH_MAX_LENGTH);
   cue_dir[0]                 = '\0';

   fill_pathname_basedir(cue_dir, cue_path, PATH_MAX_LENGTH);

   info.type        = INTFSTREAM_FILE;
   fd               = (intfstream_t*)intfstream_init(&info);

   if (!fd)
      goto error;

   if (!intfstream_open(fd, cue_path,
            RETRO_VFS_FILE_ACCESS_READ, RETRO_VFS_FILE_ACCESS_HINT_NONE))
   {
      RARCH_LOG("Could not open CUE file '%s': %s\n", cue_path,
            strerror(errno));
      goto error;
   }

   RARCH_LOG("Parsing CUE file '%s'...\n", cue_path);

   tmp_token[0] = '\0';

   rv = -EINVAL;

   while (get_token(fd, tmp_token, MAX_TOKEN_LEN) > 0)
   {
      if (string_is_equal_noncase(tmp_token, "FILE"))
      {
         /* Set last index to last EOF */
         if (file_size != -1)
            last_index = file_size;

         /* We're changing files since the candidate, update it */
         if (update_cand(&cand_index, &last_index, &largest, last_file, offset,
                         size, track_path, max_len))
         {
            rv = 0;
            if (first)
               goto clean;
         }

         get_token(fd, tmp_token, MAX_TOKEN_LEN);
         fill_pathname_join(last_file, cue_dir, tmp_token, PATH_MAX_LENGTH);

         file_size = intfstream_get_file_size(last_file);

         get_token(fd, tmp_token, MAX_TOKEN_LEN);

      }
      else if (string_is_equal_noncase(tmp_token, "TRACK"))
      {
         get_token(fd, tmp_token, MAX_TOKEN_LEN);
         get_token(fd, tmp_token, MAX_TOKEN_LEN);
         is_data = !string_is_equal_noncase(tmp_token, "AUDIO");
         ++track;
      }
      else if (string_is_equal_noncase(tmp_token, "INDEX"))
      {
         int m, s, f;
         get_token(fd, tmp_token, MAX_TOKEN_LEN);
         get_token(fd, tmp_token, MAX_TOKEN_LEN);

         if (sscanf(tmp_token, "%02d:%02d:%02d", &m, &s, &f) < 3)
         {
            RARCH_LOG("Error parsing time stamp '%s'\n", tmp_token);
            goto error;
         }

         last_index = (size_t) (((m * 60 + s) * 75) + f) * 2352;

         /* If we've changed tracks since the candidate, update it */
         if (cand_track != -1 && track != cand_track &&
             update_cand(&cand_index, &last_index, &largest, last_file, offset,
                         size, track_path, max_len))
         {
            rv = 0;
            if (first)
               goto clean;
         }

         if (!is_data)
            continue;

         if (cand_index == -1)
         {
            cand_index = last_index;
            cand_track = track;
         }
      }
   }

   if (file_size != -1)
      last_index = file_size;

   if (update_cand(&cand_index, &last_index, &largest, last_file, offset,
                   size, track_path, max_len))
      rv = 0;

clean:
   free(cue_dir);
   free(tmp_token);
   free(last_file);
   intfstream_close(fd);
   free(fd);
   return rv;

error:
   free(cue_dir);
   free(tmp_token);
   free(last_file);
   if (fd)
   {
      intfstream_close(fd);
      free(fd);
   }
   return -errno;
}

bool cue_next_file(intfstream_t *fd,
      const char *cue_path, char *path, uint64_t max_len)
{
   bool rv                    = false;
   char *tmp_token            = (char*)malloc(MAX_TOKEN_LEN);
   char *cue_dir              = (char*)malloc(PATH_MAX_LENGTH);
   cue_dir[0]                 = '\0';

   fill_pathname_basedir(cue_dir, cue_path, PATH_MAX_LENGTH);

   tmp_token[0] = '\0';

   while (get_token(fd, tmp_token, MAX_TOKEN_LEN) > 0)
   {
      if (string_is_equal_noncase(tmp_token, "FILE"))
      {
         get_token(fd, tmp_token, MAX_TOKEN_LEN);
         fill_pathname_join(path, cue_dir, tmp_token, (size_t)max_len);
         rv = true;
         break;
      }
   }

   free(cue_dir);
   free(tmp_token);
   return rv;
}

int gdi_find_track(const char *gdi_path, bool first,
      char *track_path, uint64_t max_len)
{
   int rv;
   intfstream_info_t info;
   char *tmp_token   = (char*)malloc(MAX_TOKEN_LEN);
   intfstream_t *fd  = NULL;
   uint64_t largest  = 0;
   int size          = -1;
   int mode          = -1;
   int64_t file_size = -1;

   info.type         = INTFSTREAM_FILE;

   fd                = (intfstream_t*)intfstream_init(&info);

   if (!fd)
      goto error;

   if (!intfstream_open(fd, gdi_path,
            RETRO_VFS_FILE_ACCESS_READ, RETRO_VFS_FILE_ACCESS_HINT_NONE))
   {
      RARCH_LOG("Could not open GDI file '%s': %s\n", gdi_path,
            strerror(errno));
      goto error;
   }

   RARCH_LOG("Parsing GDI file '%s'...\n", gdi_path);

   tmp_token[0] = '\0';

   rv = -EINVAL;

   /* Skip track count */
   get_token(fd, tmp_token, MAX_TOKEN_LEN);

   /* Track number */
   while (get_token(fd, tmp_token, MAX_TOKEN_LEN) > 0)
   {
      /* Offset */
      if (get_token(fd, tmp_token, MAX_TOKEN_LEN) <= 0)
      {
         errno = EINVAL;
         goto error;
      }

      /* Mode */
      if (get_token(fd, tmp_token, MAX_TOKEN_LEN) <= 0)
      {
         errno = EINVAL;
         goto error;
      }
      mode = atoi(tmp_token);

      /* Sector size */
      if (get_token(fd, tmp_token, MAX_TOKEN_LEN) <= 0)
      {
         errno = EINVAL;
         goto error;
      }
      size = atoi(tmp_token);

      /* File name */
      if (get_token(fd, tmp_token, MAX_TOKEN_LEN) <= 0)
      {
         errno = EINVAL;
         goto error;
      }

      /* Check for data track */
      if (!(mode == 0 && size == 2352))
      {
         char *last_file   = (char*)malloc(PATH_MAX_LENGTH + 1);
         char *gdi_dir     = (char*)malloc(PATH_MAX_LENGTH);

         gdi_dir[0]        = '\0';

         fill_pathname_basedir(gdi_dir, gdi_path, PATH_MAX_LENGTH);

         fill_pathname_join(last_file,
               gdi_dir, tmp_token, PATH_MAX_LENGTH);
         file_size = intfstream_get_file_size(last_file);
         if (file_size < 0)
         {
            free(gdi_dir);
            free(last_file);
            goto error;
         }

         if ((uint64_t)file_size > largest)
         {
            strlcpy(track_path, last_file, (size_t)max_len);

            rv      = 0;
            largest = file_size;

            if (first)
            {
               free(gdi_dir);
               free(last_file);
               goto clean;
            }
         }
         free(gdi_dir);
         free(last_file);
      }

      /* Disc offset (not used?) */
      if (get_token(fd, tmp_token, MAX_TOKEN_LEN) <= 0)
      {
         errno = EINVAL;
         goto error;
      }
   }

clean:
   free(tmp_token);
   intfstream_close(fd);
   free(fd);
   return rv;

error:
   free(tmp_token);
   if (fd)
   {
      intfstream_close(fd);
      free(fd);
   }
   return -errno;
}

bool gdi_next_file(intfstream_t *fd, const char *gdi_path,
      char *path, uint64_t max_len)
{
   bool rv         = false;
   char *tmp_token = (char*)malloc(MAX_TOKEN_LEN);
   int64_t offset  = -1;

   tmp_token[0]    = '\0';

   /* Skip initial track count */
   offset = intfstream_tell(fd);
   if (offset == 0)
      get_token(fd, tmp_token, MAX_TOKEN_LEN);

   /* Track number */
   get_token(fd, tmp_token, MAX_TOKEN_LEN);

   /* Offset */
   get_token(fd, tmp_token, MAX_TOKEN_LEN);

   /* Mode */
   get_token(fd, tmp_token, MAX_TOKEN_LEN);

   /* Sector size */
   get_token(fd, tmp_token, MAX_TOKEN_LEN);

   /* File name */
   if (get_token(fd, tmp_token, MAX_TOKEN_LEN) > 0)
   {
      char *gdi_dir   = (char*)malloc(PATH_MAX_LENGTH);

      gdi_dir[0]      = '\0';

      fill_pathname_basedir(gdi_dir, gdi_path, PATH_MAX_LENGTH);

      fill_pathname_join(path, gdi_dir, tmp_token, (size_t)max_len);
      rv = true;

      /* Disc offset */
      get_token(fd, tmp_token, MAX_TOKEN_LEN);

      free(gdi_dir);
   }

   free(tmp_token);
   return rv;
}
