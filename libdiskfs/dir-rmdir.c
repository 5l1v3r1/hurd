/* libdsikfs implementation of fs.defs: dir_rmdir
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

/* Implement dir_rmdir as described in <hurd/fs.defs>. */
error_t
diskfs_S_dir_rmdir (struct protid *dircred,
		    char *name)
{
  struct node *dnp;
  struct node *np = 0;
  struct dirstat *ds = alloca (diskfs_dirstat_size);
  error_t error;

  if (!dircred)
    return EOPNOTSUPP;
  
  dnp = dircred->po->np;
  if (readonly)
    return EROFS;

  mutex_lock (&dnp->lock);

  error = diskfs_lookup (dnp, name, REMOVE, &np, ds, dircred);
  if (error == EAGAIN)
    error = ENOTEMPTY;
  if (error)
    {
      mutex_unlock (&dnp->lock);
      diskfs_drop_dirstat (ds);
      return error;
    }

  /* Attempt to rmdir(".") */
  if (dnp == np)
    {
      diskfs_nrele (np);
      diskfs_drop_dirstat (ds);
      mutex_unlock (&dnp->lock);
      return EINVAL;
    }

  /* Verify the directory is empty (and valid).  (Rmdir ".." won't be
     valid since ".." will contain a reference to the current directory and
     thus be non-empty). */
  if (!diskfs_dirempty (np, dircred))
    {
      diskfs_nput (np);
      diskfs_drop_dirstat (ds);
      mutex_unlock (&dnp->lock);
      return ENOTEMPTY;
    }

  error = diskfs_dirremove (dnp, ds);

  if (!error)
    {
      np->dn_stat.st_nlink--;
      np->dn_set_ctime = 1;
    }
  
  if (!error)
    diskfs_clear_directory (np, dnp, dircred);

  diskfs_nput (np);
  mutex_unlock (&dnp->lock);
  return 0;
}
