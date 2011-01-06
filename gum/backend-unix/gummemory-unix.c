/*
 * Copyright (C) 2008 Ole Andr� Vadla Ravn�s <ole.andre.ravnas@tandberg.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

#include "gummemory.h"

#include "gummemory-priv.h"

#include <unistd.h>
#define __USE_GNU     1
#include <sys/mman.h>
#undef __USE_GNU
#define INSECURE      0
#define NO_MALLINFO   1
#define USE_LOCKS     1
#define USE_DL_PREFIX 1
#include "dlmalloc.c"

static gint gum_page_protection_to_unix (GumPageProtection page_prot);

void
_gum_memory_init (void)
{
}

void
_gum_memory_deinit (void)
{
}

guint
gum_query_page_size (void)
{
  return sysconf (_SC_PAGE_SIZE);
}

static GumPageProtection
gum_memory_get_protection (gpointer address,
                           guint len)
{
  GumPageProtection result = GUM_PAGE_NO_ACCESS;
  FILE * fp;
  gchar line[1024 + 1];

  fp = fopen ("/proc/self/maps", "r");
  g_assert (fp != NULL);

  while (fgets (line, sizeof (line), fp) != NULL)
  {
    gint n;
    gpointer start, end;
    gchar protection[16];

    n = sscanf (line, "%p-%p %s ", &start, &end, protection);
    g_assert_cmpint (n, ==, 3);

    if (start > address)
      break;
    else if (address >= start && address + len <= end)
    {
      if (protection[0] == 'r')
        result |= GUM_PAGE_READ;
      if (protection[1] == 'w')
        result |= GUM_PAGE_WRITE;
      if (protection[2] == 'x')
        result |= GUM_PAGE_EXECUTE;
      break;
    }
  }

  fclose (fp);

  return result;
}

gboolean
gum_memory_is_readable (gpointer address,
                        guint len)
{
  return (gum_memory_get_protection (address, len) & GUM_PAGE_READ) != 0;
}

static gboolean
gum_memory_is_writable (gpointer address,
                        guint len)
{
  return (gum_memory_get_protection (address, len) & GUM_PAGE_WRITE) != 0;
}

guint8 *
gum_memory_read (gpointer address,
                 guint len,
                 gint * n_bytes_read)
{
  guint8 * result = NULL;
  gint result_len = 0;

  if (gum_memory_is_readable (address, len))
  {
    result = g_memdup (address, len);
    result_len = len;
  }

  if (n_bytes_read != NULL)
    *n_bytes_read = result_len;

  return result;
}

gboolean
gum_memory_write (gpointer address,
                  guint8 * bytes,
                  guint len)
{
  gboolean result = FALSE;

  if (gum_memory_is_writable (address, len))
  {
    memcpy (address, bytes, len);
    result = TRUE;
  }

  return result;
}

void
gum_mprotect (gpointer address,
              guint size,
              GumPageProtection page_prot)
{
  gpointer aligned_address;
  gint unix_page_prot;
  gint result;

  g_assert (size != 0);

  aligned_address = GSIZE_TO_POINTER (
      GPOINTER_TO_SIZE (address) & ~(gum_query_page_size () - 1));
  unix_page_prot = gum_page_protection_to_unix (page_prot);

  result = mprotect (aligned_address, size, unix_page_prot);
  g_assert_cmpint (result, ==, 0);
}

gpointer
gum_malloc (gsize size)
{
  return dlmalloc (size);
}

gpointer
gum_malloc0 (gsize size)
{
  return dlcalloc (1, size);
}

gpointer
gum_realloc (gpointer mem,
             gsize size)
{
  return dlrealloc (mem, size);
}

gpointer
gum_memdup (gconstpointer mem,
            gsize byte_size)
{
  gpointer result;

  result = dlmalloc (byte_size);
  memcpy (result, mem, byte_size);

  return result;
}

void
gum_free (gpointer mem)
{
  dlfree (mem);
}

gpointer
gum_alloc_n_pages (guint n_pages,
                   GumPageProtection page_prot)
{
  guint8 * result = NULL;
  guint page_size, size, alloc_size;
  gint ret;

  /* sbrk() or mmap() would probably be better choices here */
  page_size = gum_query_page_size ();
  size = n_pages * page_size;
  alloc_size = page_size + size;
  ret = posix_memalign ((void **) &result, page_size, alloc_size);
  g_assert (ret == 0);

  *((guint *) result) = size;

  result += page_size;
  memset (result, 0, size);
  gum_mprotect (result, size, page_prot);

  return result;
}

gpointer
gum_alloc_n_pages_near (guint n_pages,
                        GumPageProtection page_prot,
                        GumAddressSpec * address_spec)
{
  gpointer result = NULL;
  gsize page_size, size;
  gint unix_page_prot;
  const gint flags = MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED;
  guint8 * low_address, * high_address;

  page_size = gum_query_page_size ();
  size = n_pages * page_size;
  unix_page_prot = gum_page_protection_to_unix (page_prot);

  low_address = (guint8 *)
      (GPOINTER_TO_SIZE (address_spec->near_address) & ~(page_size - 1));
  high_address = low_address;

  do
  {
    gsize cur_distance;

    low_address -= page_size;
    high_address += page_size;
    cur_distance = (gsize) high_address - (gsize) address_spec->near_address;
    if (cur_distance > address_spec->max_distance)
      break;

    result = mmap (low_address, size, unix_page_prot, flags, -1, 0);
    if (result == NULL)
      result = mmap (high_address, size, unix_page_prot, flags, -1, 0);
  }
  while (result == NULL);

  g_assert (result != NULL);

  return result;
}

void
gum_free_pages (gpointer mem)
{
  guint8 * start;
  guint page_size, size;

  page_size = gum_query_page_size ();
  start = (guint8 *) mem - page_size;
  size = *((guint *) start);

  gum_mprotect (mem, size, GUM_PAGE_READ | GUM_PAGE_WRITE);
  free (start);
}

static gint
gum_page_protection_to_unix (GumPageProtection page_prot)
{
  gint unix_page_prot = PROT_NONE;

  if ((page_prot & GUM_PAGE_READ) != 0)
    unix_page_prot |= PROT_READ;
  if ((page_prot & GUM_PAGE_WRITE) != 0)
    unix_page_prot |= PROT_WRITE;
  if ((page_prot & GUM_PAGE_EXECUTE) != 0)
    unix_page_prot |= PROT_EXEC;

  return unix_page_prot;
}
