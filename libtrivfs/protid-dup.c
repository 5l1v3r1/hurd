/* Duplicate a protid

   Copyright (C) 1993, 1994, 1995 Free Software Foundation, Inc.

   This file is part of the GNU Hurd.

   The GNU Hurd is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2, or (at your option)
   any later version.

   The GNU Hurd is distributed in the hope that it will be useful, 
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with the GNU Hurd; see the file COPYING.  If not, write to
   the Free Software Foundation, 675 Mass Ave, Cambridge, MA 02139, USA.  */

#include <string.h>		/* For bcopy() */

#include "priv.h"

/* Return a duplicate of CRED in DUP, sharing the same peropen and hook.  A
   non-null hook may be used to detect that this is a duplicate by
   trivfs_peropen_create_hook.  */
error_t
trivfs_protid_dup (struct trivfs_protid *cred, struct trivfs_protid **dup)
{
  error_t err = 0;
  struct trivfs_protid *new = 
    ports_allocate_port (cred->po->cntl->protid_bucket,
			 sizeof (struct trivfs_protid), 
			 cred->po->cntl->protid_class);

  mutex_lock (&cred->po->cntl->lock);
  new->po = cred->po;
  new->po->refcnt++;
  mutex_unlock (&cred->po->cntl->lock);

  new->isroot = cred->isroot;

  new->uids = malloc (cred->nuids * sizeof (uid_t));
  bcopy (cred->uids, new->uids, cred->nuids * sizeof (uid_t));
  new->nuids = cred->nuids;

  new->gids = malloc (cred->ngids * sizeof (gid_t));
  bcopy (cred->gids, new->gids, cred->ngids * sizeof (uid_t));
  new->ngids = cred->ngids;

  new->realnode = cred->realnode;
  mach_port_mod_refs (mach_task_self (), new->realnode,
		      MACH_PORT_RIGHT_SEND, 1);

  new->hook = cred->hook;

  if (trivfs_protid_create_hook)
    err = (*trivfs_protid_create_hook) (new);
  if (err)
    {
      mach_port_deallocate (mach_task_self (), new->realnode);
      /* Signal that the user destroy hook shouldn't be called on NEW.  */
      new->realnode = MACH_PORT_NULL;
      ports_port_deref (new);
    }
  else
    *dup = new;

  return err;
}
