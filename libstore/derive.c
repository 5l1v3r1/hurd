/* Calculation of various derived store fields

   Copyright (C) 1995 Free Software Foundation, Inc.

   Written by Miles Bader <miles@gnu.ai.mit.edu>

   This file is part of the GNU Hurd.

   The GNU Hurd is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License as
   published by the Free Software Foundation; either version 2, or (at
   your option) any later version.

   The GNU Hurd is distributed in the hope that it will be useful, but
   WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA. */

#include "store.h"

/* Fills in the values of the various fields in STORE that are derivable from
   the set of runs & the block size.  */
void
_store_derive (struct store *store)
{
  off_t *runs = store->runs;
  unsigned runs_len = store->runs_len;
  size_t bsize = store->block_size;

  /* BLOCK & SIZE */
  store->blocks = 0;
  store->size = 0;

  while (runs_len > 0)
    {
      store->size += bsize * runs[1];
      if (runs[0] >= 0)
	store->blocks += runs[1];
      runs += 2;
      runs_len--;
    }

  /* LOG2_BLOCK_SIZE */
  store->log2_block_size = 0;
  while ((1 << store->log2_block_size) < bsize)
    store->log2_block_size++;
  if ((1 << store->log2_block_size) != bsize)
    store->log2_block_size = 0;

  /* LOG2_BLOCKS_PER_PAGE */
  store->log2_blocks_per_page = 0;
  while ((bsize << store->log2_blocks_per_page) < vm_page_size)
    store->log2_blocks_per_page++;
  if ((bsize << store->log2_blocks_per_page) != vm_page_size)
    store->log2_blocks_per_page = 0;
}
