/* Map the disk image and handle faults accessing it.
   Copyright (C) 1996 Free Software Foundation, Inc.
   Written by Roland McGrath.

   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License as
   published by the Free Software Foundation; either version 2, or (at
   your option) any later version.

   This program is distributed in the hope that it will be useful, but
   WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA. */

#ifndef _HURD_DISKFS_PAGER_H
#define _HURD_DISKFS_PAGER_H 1
#include <hurd/pager.h>
#include <hurd/ports.h>
#include <setjmp.h>
#include <cthreads.h>
#include <errno.h>
#include <assert.h>
#include <stdlib.h>


/* Set up the three variables below and prepare a signal preempter
   so that the `diskfs_catch_exception' macro below works.
   INFO and MAY_CACHE are passed to `pager_create'.  */
extern void disk_pager_setup (struct user_pager_info *info, int may_cache);

extern struct port_bucket *pager_bucket; /* Ports bucket used by pagers.  */
extern struct pager *disk_pager; /* Pager backing to the disk.  */
extern void *disk_image;	/* Region mapping entire disk from it.  */

/* Return zero now.  Return a second time with a nonzero error_t
   if this thread faults accessing `disk_image' before calling
   `diskfs_end_catch_exception' (below).  */
#define diskfs_catch_exception()					      \
({									      \
    jmp_buf *env = alloca (sizeof (jmp_buf));				      \
    error_t err;							      \
    assert (cthread_data (cthread_self ()) == 0);			      \
    err = setjmp (*env);						      \
    if (err == 0)							      \
      cthread_set_data (cthread_self (), env);				      \
    err;								      \
})

/* No longer handle faults on `disk_image' in this thread.
   Any unexpected fault hereafter will crash the program.  */
#define diskfs_end_catch_exception()					      \
({									      \
    assert (cthread_data (cthread_self ()) != 0);			      \
    cthread_set_data (cthread_self (), 0);				      \
})


#endif	/* hurd/diskfs-pager.h */
