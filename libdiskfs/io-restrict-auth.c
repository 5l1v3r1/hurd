/* 
   Copyright (C) 1994,95,96,2001 Free Software Foundation

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
#include "io_S.h"

/* Tell if the array LIST (of size N) contains a member equal to QUERY. */
static inline int
listmember (int *list, int query, int n)
{
  int i;
  for (i = 0; i < n; i++)
    if (list[i] == query)
      return 1;
  return 0;
}

/* Implement io_restrict_auth as described in <hurd/io.defs>. */
kern_return_t
diskfs_S_io_restrict_auth (struct protid *cred,
			   mach_port_t *newport,
			   mach_msg_type_name_t *newportpoly,
			   uid_t *uids,
			   u_int nuids,
			   gid_t *gids,
			   u_int ngids)
{
  error_t err = 0;
  struct idvec *uvec, *gvec;
  struct iouser *user;
  struct protid *newpi;
  int i;
  
  if (!cred)
    return EOPNOTSUPP;

  if (idvec_contains (cred->user->uids, 0))
    /* CRED has root access, and so may use any ids.  */
    {
      err = iohelp_create_complex_iouser (&user, uids, nuids, gids, ngids);
      if (err)
        return err;
    }
  else
    {
      uvec = make_idvec ();
      if (! uvec)
        return ENOMEM;

      gvec = make_idvec ();
      if (! gvec)
        {
	  idvec_free (uvec);
	  return ENOMEM;
	}
  
      /* Otherwise, use any of the requested ids that CRED already has.  */
      for (i = 0; i < cred->user->uids->num; i++)
	if (listmember (uids, cred->user->uids->ids[i], nuids))
	  {
	    err = idvec_add (uvec, cred->user->uids->ids[i]);
	    if (err)
	      goto out;
	  }
      for (i = 0; i < cred->user->gids->num; i++)
	if (listmember (gids, cred->user->gids->ids[i], ngids))
	  {
	    idvec_add (gvec, cred->user->gids->ids[i]);
	    if (err)
	      goto out;
	  }

      err = iohelp_create_iouser (&user, uvec, gvec);

      if (err)
        {
        out:
	  idvec_free (uvec);
	  idvec_free (gvec);
	  return err;
	}
    }

  mutex_lock (&cred->po->np->lock);
  err = diskfs_create_protid (cred->po, user, &newpi);
  if (! err)
    {
      *newport = ports_get_right (newpi);
      *newportpoly = MACH_MSG_TYPE_MAKE_SEND;
      ports_port_deref (newpi);
    }
  mutex_unlock (&cred->po->np->lock);

  iohelp_free_iouser (user);
  return err;
}
