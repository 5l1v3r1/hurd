/* Get run-time file system options

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

#include <errno.h>

#include "priv.h"

error_t
diskfs_S_file_get_fs_options (struct protid *cred,
			      char **data, unsigned *data_len)
{
  error_t err;
  char *argz;

  if (!cred)
    return EOPNOTSUPP;

  rwlock_reader_lock (&diskfs_fsys_lock);
  err = diskfs_get_options (&argz, data_len);
  rwlock_reader_unlock (&diskfs_fsys_lock);

  if (!err)
    /* Move ARGZ from a malloced buffer into a vm_alloced one.  */
    {
      err = vm_allocate (mach_task_self (), (vm_address_t *)data, data_len, 1);
      if (!err)
	bcopy (argz, *data, *data_len);
      free (argz);
    }

  return err;
}
