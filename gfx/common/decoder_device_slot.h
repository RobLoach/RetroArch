/* Copyright (C) 2026 The RetroArch team.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef RETROARCH_DECODER_DEVICE_SLOT_H
#define RETROARCH_DECODER_DEVICE_SLOT_H
#include <stdint.h>
#include <stdlib.h>
#include <boolean.h>
#include <retro_atomic.h>
#include <retro_inline.h>
#include <queues/retro_retire.h>

#ifdef RETRO_RETIRE_LOCK_FREE
#define DECODER_DEVICE_SLOT 1

/* The renderer's device as decoder setup sees it. The renderer's thread
 * replaces it; any thread may take a retained copy. A replaced device
 * is released once no thread is between loading it and retaining it,
 * by whichever thread is last out (retro_retire.h). */
typedef struct decoder_device_node
{
   mpsc_stack_node_t link;
   void (*release)(void *);
   void *device;
   uintptr_t generation;
   bool hdr;
} decoder_device_node_t;

typedef struct decoder_device_slot
{
   void *(*retain)(void *);
   void (*release)(void *);
   retro_atomic_ptr_t current;
   retro_retire_t     retire;
} decoder_device_slot_t;

static INLINE void decoder_device_slot_init(decoder_device_slot_t *s,
      void *(*retain)(void *), void (*release)(void *))
{
   retro_retire_init(&s->retire);
   s->retain  = retain;
   s->release = release;
   retro_atomic_ptr_init(&s->current, NULL);
}

static INLINE void decoder_device_node_free(mpsc_stack_node_t *link)
{
   decoder_device_node_t *node = (decoder_device_node_t *)link;
#ifndef DECODER_DEVICE_SLOT_TEST_RELEASE_EARLY
   node->release(node->device);
#endif
   free(node);
}

/* Renderer's thread. A NULL @device withdraws. */
static INLINE bool decoder_device_slot_set(decoder_device_slot_t *s,
      void *device, uintptr_t generation, bool hdr)
{
   decoder_device_node_t *node = NULL;
   if (device)
   {
      if (!(node = (decoder_device_node_t *)malloc(sizeof(*node))))
         return false;
      if (!(node->device = s->retain(device)))
      {
         free(node);
         return false;
      }
      node->link.next  = NULL;
      node->release    = s->release;
      node->generation = generation;
      node->hdr        = hdr;
   }
   node = (decoder_device_node_t *)
         retro_atomic_exchange_ptr(&s->current, node);
#ifdef DECODER_DEVICE_SLOT_TEST_RELEASE_EARLY
   if (node)
      node->release(node->device);
#endif
   retro_retire(&s->retire, node ? &node->link : NULL,
         decoder_device_node_free);
   return true;
}

/* Any thread: the current device retained, or NULL. */
static INLINE void *decoder_device_slot_acquire(decoder_device_slot_t *s,
      uintptr_t *generation, bool *hdr)
{
   void *device = NULL;
   const decoder_device_node_t *node;
   retro_retire_enter(&s->retire);
   if ((node = (const decoder_device_node_t *)
            retro_atomic_load_acquire_ptr(&s->current)))
   {
#ifdef DECODER_DEVICE_SLOT_TEST_LOADED
      DECODER_DEVICE_SLOT_TEST_LOADED(s);
#endif
      device = s->retain(node->device);
   }
   if (device)
   {
      if (generation)
         *generation = node->generation;
      if (hdr)
         *hdr = node->hdr;
   }
   retro_retire_leave(&s->retire, decoder_device_node_free);
   return device;
}
#endif
#endif
