/****************************************************************************
 * mm/kasan/kasan.c
 *
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.  The
 * ASF licenses this file to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance with the
 * License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  See the
 * License for the specific language governing permissions and limitations
 * under the License.
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/spinlock.h>

#include <assert.h>
#include <debug.h>
#include <execinfo.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "kasan.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define KASAN_BYTES_PER_WORD (sizeof(uintptr_t))
#define KASAN_BITS_PER_WORD  (KASAN_BYTES_PER_WORD * 8)

#define KASAN_FIRST_WORD_MASK(start) \
  (UINTPTR_MAX << ((start) & (KASAN_BITS_PER_WORD - 1)))
#define KASAN_LAST_WORD_MASK(end) \
  (UINTPTR_MAX >> (-(end) & (KASAN_BITS_PER_WORD - 1)))

#define KASAN_SHADOW_SCALE (sizeof(uintptr_t))

#define KASAN_SHADOW_SIZE(size) \
  (KASAN_BYTES_PER_WORD * ((size) / KASAN_SHADOW_SCALE / KASAN_BITS_PER_WORD))
#define KASAN_REGION_SIZE(size) \
  (sizeof(struct kasan_region_s) + KASAN_SHADOW_SIZE(size))

#define KASAN_INIT_VALUE            0xDEADCAFE

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct kasan_region_s
{
  FAR struct kasan_region_s *next;
  uintptr_t                  begin;
  uintptr_t                  end;
  uintptr_t                  shadow[1];
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static bool kasan_is_poisoned(FAR const void *addr, size_t size);

/****************************************************************************
 * Private Data
 ****************************************************************************/

static spinlock_t g_lock;
static FAR struct kasan_region_s *g_region;
static uint32_t g_region_init;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static inline FAR uintptr_t *kasan_find_mem(uintptr_t addr, size_t size,
                                            unsigned int *bit)
{
  FAR struct kasan_region_s *region;

  if (size == 0)
    {
      return NULL;
    }

  for (region = g_region; region != NULL; region = region->next)
    {
      if (addr >= region->begin && addr < region->end)
        {
          DEBUGASSERT(addr + size <= region->end);
          addr -= region->begin;
          addr /= KASAN_SHADOW_SCALE;
          *bit  = addr % KASAN_BITS_PER_WORD;
          return &region->shadow[addr / KASAN_BITS_PER_WORD];
        }
    }

  return NULL;
}

static FAR uintptr_t *kasan_mem_to_shadow(FAR const void *ptr, size_t size,
                                          unsigned int *bit)
{
  uintptr_t addr = (uintptr_t)ptr;
  FAR uintptr_t *ret;
  size_t mul;
  size_t mod;
  size_t i;

  if (g_region_init != KASAN_INIT_VALUE)
    {
      return NULL;
    }

  if (size > KASAN_SHADOW_SCALE)
    {
      mul = size / KASAN_SHADOW_SCALE;
      for (i = 0; i < mul; i++)
        {
          ret = kasan_find_mem(addr + i * KASAN_SHADOW_SCALE,
                               KASAN_SHADOW_SCALE, bit);
          if (ret == NULL)
            {
              return ret;
            }
        }

      mod = size % KASAN_SHADOW_SCALE;
      addr += mul * KASAN_SHADOW_SCALE;
      size = mod;
    }

  return kasan_find_mem(addr, size, bit);
}

static void kasan_show_memory(FAR const uint8_t *addr, size_t size,
                              size_t dumpsize)
{
  FAR const uint8_t *start = (FAR const uint8_t *)
                             (((uintptr_t)addr) & ~0xf) - dumpsize;
  FAR const uint8_t *end = start + 2 * dumpsize;
  FAR const uint8_t *p = start;
  char buffer[256];

  _alert("Shadow bytes around the buggy address:\n");
  for (p = start; p < end; p += 16)
    {
      int ret = sprintf(buffer, "  %p: ", p);
      int i;

      for (i = 0; i < 16; i++)
        {
          if (kasan_is_poisoned(p + i, 1))
            {
              if (p + i == addr)
                {
                  ret += sprintf(buffer + ret,
                                 "\b[\033[31m%02x\033[0m ", p[i]);
                }
              else if (p + i == addr + size - 1)
                {
                  ret += sprintf(buffer + ret, "\033[31m%02x\033[0m]", p[i]);
                }
              else
                {
                  ret += sprintf(buffer + ret, "\033[31m%02x\033[0m ", p[i]);
                }
            }
          else
            {
              ret += sprintf(buffer + ret, "\033[37m%02x\033[0m ", p[i]);
            }
        }

      _alert("%s\n", buffer);
    }
}

static void kasan_report(FAR const void *addr, size_t size,
                         bool is_write,
                         FAR void *return_address)
{
  static int recursion;
  irqstate_t flags;

  flags = enter_critical_section();

  if (++recursion == 1)
    {
      _alert("kasan detected a %s access error, address at %p,"
             "size is %zu, return address: %p\n",
             is_write ? "write" : "read",
             addr, size, return_address);

      kasan_show_memory(addr, size, 80);
#ifndef CONFIG_MM_KASAN_DISABLE_PANIC
      PANIC();
#else
      dump_stack();
#endif
    }

  --recursion;
  leave_critical_section(flags);
}

static bool kasan_is_poisoned(FAR const void *addr, size_t size)
{
  FAR uintptr_t *p;
  unsigned int bit;

  p = kasan_mem_to_shadow(addr, size, &bit);
  return p && ((*p >> bit) & 1);
}

static void kasan_set_poison(FAR const void *addr, size_t size,
                             bool poisoned)
{
  FAR uintptr_t *p;
  unsigned int bit;
  unsigned int nbit;
  uintptr_t mask;
  int flags;

  if (size == 0)
    {
      return;
    }

  flags = spin_lock_irqsave(&g_lock);

  p = kasan_find_mem((uintptr_t)addr, size, &bit);
  DEBUGASSERT(p != NULL);

  nbit = KASAN_BITS_PER_WORD - bit % KASAN_BITS_PER_WORD;
  mask = KASAN_FIRST_WORD_MASK(bit);

  size /= KASAN_SHADOW_SCALE;
  while (size >= nbit)
    {
      if (poisoned)
        {
          *p++ |= mask;
        }
      else
        {
          *p++ &= ~mask;
        }

      bit  += nbit;
      size -= nbit;

      nbit = KASAN_BITS_PER_WORD;
      mask = UINTPTR_MAX;
    }

  if (size)
    {
      mask &= KASAN_LAST_WORD_MASK(bit + size);
      if (poisoned)
        {
          *p |= mask;
        }
      else
        {
          *p &= ~mask;
        }
    }

  spin_unlock_irqrestore(&g_lock, flags);
}

static inline void kasan_check_report(FAR const void *addr, size_t size,
                                      bool is_write,
                                      FAR void *return_address)
{
  if (kasan_is_poisoned(addr, size))
    {
      kasan_report(addr, size, is_write, return_address);
    }
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/* Exported functions called from other mm module */

void kasan_poison(FAR const void *addr, size_t size)
{
  kasan_set_poison(addr, size, true);
}

void kasan_unpoison(FAR const void *addr, size_t size)
{
  kasan_set_poison(addr, size, false);
}

void kasan_register(FAR void *addr, FAR size_t *size)
{
  FAR struct kasan_region_s *region;

  region = (FAR struct kasan_region_s *)
    ((FAR char *)addr + *size - KASAN_REGION_SIZE(*size));

  region->begin = (uintptr_t)addr;
  region->end   = region->begin + *size;
  region->next  = g_region;
  g_region      = region;
  g_region_init = KASAN_INIT_VALUE;

  kasan_poison(addr, *size);
  *size -= KASAN_REGION_SIZE(*size);
}

void kasan_init_early(void)
{
  g_region_init = 0;
}

/* Exported functions called from the compiler generated code */

void __sanitizer_annotate_contiguous_container(FAR const void *beg,
                                               FAR const void *end,
                                               FAR const void *old_mid,
                                               FAR const void *new_mid)
{
  /* Shut up compiler complaints */
}

void __asan_before_dynamic_init(FAR const void *module_name)
{
  /* Shut up compiler complaints */
}

void __asan_after_dynamic_init(void)
{
  /* Shut up compiler complaints */
}

void __asan_handle_no_return(void)
{
  /* Shut up compiler complaints */
}

void __asan_report_load_n_noabort(FAR void *addr, size_t size)
{
  kasan_report(addr, size, false, return_address(0));
}

void __asan_report_store_n_noabort(FAR void *addr, size_t size)
{
  kasan_report(addr, size, true, return_address(0));
}

void __asan_loadN_noabort(FAR void *addr, size_t size)
{
  kasan_check_report(addr, size, false, return_address(0));
}

void __asan_storeN_noabort(FAR void * addr, size_t size)
{
  kasan_check_report(addr, size, true, return_address(0));
}

void __asan_loadN(FAR void *addr, size_t size)
{
  kasan_check_report(addr, size, false, return_address(0));
}

void __asan_storeN(FAR void *addr, size_t size)
{
  kasan_check_report(addr, size, true, return_address(0));
}

#define DEFINE_ASAN_LOAD_STORE(size) \
  void __asan_report_load##size##_noabort(FAR void *addr) \
  { \
    kasan_report(addr, size, false, return_address(0)); \
  } \
  void __asan_report_store##size##_noabort(FAR void *addr) \
  { \
    kasan_report(addr, size, true, return_address(0)); \
  } \
  void __asan_load##size##_noabort(FAR void *addr) \
  { \
    kasan_check_report(addr, size, false, return_address(0)); \
  } \
  void __asan_store##size##_noabort(FAR void *addr) \
  { \
    kasan_check_report(addr, size, true, return_address(0)); \
  } \
  void __asan_load##size(FAR void *addr) \
  { \
    kasan_check_report(addr, size, false, return_address(0)); \
  } \
  void __asan_store##size(FAR void *addr) \
  { \
    kasan_check_report(addr, size, true, return_address(0)); \
  }

DEFINE_ASAN_LOAD_STORE(1)
DEFINE_ASAN_LOAD_STORE(2)
DEFINE_ASAN_LOAD_STORE(4)
DEFINE_ASAN_LOAD_STORE(8)
DEFINE_ASAN_LOAD_STORE(16)
