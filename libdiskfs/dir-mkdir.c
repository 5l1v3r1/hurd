/* libdiskfs implementation of fs.defs: dir_mkdir
   Copyright (C) 1992, 1993, 1994 Free Software Foundation

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

#include "priv.h"
#include "fs_S.h"

/* Implement dir_mkdir as found in <hurd/fs.defs>. */
error_t
diskfs_S_dir_mkdir (struct protid *dircred,
		    char *name,
		    mode_t mode)
{
  struct node *dnp;
  struct node *np = 0;
  struct dirstat *ds = alloca (diskfs_dirstat_size);
  int error;

  if (!dircred)
    return EOPNOTSUPP;
  
  dnp = dircred->po->np;
  if (diskfs_readonly)
    return EROFS;

  mutex_lock (&dnp->lock);

  error = diskfs_lookup (dnp, name, CREATE, 0, ds, dircred);

  if (error == EAGAIN)
    error = EEXIST;
  if (!error)
    error =  EEXIST;

  if (error != ENOENT)
    {
      diskfs_drop_dirstat (dnp, ds);
      mutex_unlock (&dnp->lock);
      return error;
    }

  mode &= ~(S_ISPARE | S_IFMT);
  mode |= S_IFDIR;

  error = diskfs_create_node (dnp, name, mode, &np, dircred, ds);

  if (!error)
    diskfs_nput (np);

  mutex_unlock (&dnp->lock);
  return error;
}
