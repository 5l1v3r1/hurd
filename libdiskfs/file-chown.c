/* libdiskfs implementetation of fs.defs: file_chown
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

/* Implement file_chown as described in <hurd/fs.defs>. */
kern_return_t
diskfs_S_file_chown (struct protid *cred,
		     uid_t uid,
		     gid_t gid)
{
  CHANGE_NODE_FIELD (cred,
		   ({
		     err = diskfs_isowner (np, cred);
		     if (err
			 || ((!diskfs_isuid (uid, cred)
			      || !diskfs_groupmember (gid, cred))
			     && !diskfs_isuid (0, cred)))
		       err = EPERM;
		     else
		       {
			 np->dn_stat.st_uid = uid;
			 np->dn_stat.st_gid = gid;
			 np->dn_set_ctime = 1;
		       }
		   }));
}
