/* Setuid reauthentication for exec

   Copyright (C) 1995 Free Software Foundation, Inc.

   Written by Miles Bader <miles@gnu.ai.mit.edu>,
     from the original by Michael I. Bushnell p/BSG  <mib@gnu.ai.mit.edu>

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

#include <hurd/io.h>
#include <hurd/process.h>
#include <hurd/auth.h>
#include <idvec.h>

#include "fshelp.h"

/* If SUID or SGID is true, adds UID and/or GID respectively to the
   authentication in PORTS[INIT_PORT_AUTH], and replaces it with the result.
   All the other ports in PORTS and FDS are then reauthenticated, using any
   privileges available through AUTH.  If GET_FILE_IDS is non-NULL, and the
   auth port in PORTS[INIT_PORT_AUTH] is bogus, it is called to get a list of
   uids and gids from the file to use as a replacement.  If SECURE is
   non-NULL, whether not the added ids are new is returned in it.  If either
   the uid or gid case fails, then the other may still be applied.  */
error_t
fshelp_exec_reauth (int suid, uid_t uid, int sgid, gid_t gid,
		    auth_t auth,
		    error_t
		      (*get_file_ids)(struct idvec *uids, struct idvec *gids),
		    mach_port_t *ports, mach_msg_type_number_t num_ports,
		    mach_port_t *fds, mach_msg_type_number_t num_fds,
		    int *secure)
{
  error_t err;
  int _secure = 0;

  if (suid || sgid)
    {
      int i;
      int already_root = 0;
      auth_t newauth;
      /* These variables describe the auth port that the user gave us. */
      struct idvec *eff_uids = make_idvec (), *avail_uids = make_idvec ();
      struct idvec *eff_gids = make_idvec (), *avail_gids = make_idvec ();

      void
	reauth (mach_port_t *port, int isproc)
	  {
	    mach_port_t newport, ref;
	    if (*port == MACH_PORT_NULL)
	      return;
	    ref = mach_reply_port ();
	    err = (isproc ? proc_reauthenticate : io_reauthenticate)
	      (*port, ref, MACH_MSG_TYPE_MAKE_SEND);
	    if (!err)
	      err = auth_user_authenticate (newauth, *port, ref,
					    MACH_MSG_TYPE_MAKE_SEND, &newport);
	    if (err)
	      {
		/* Could not reauthenticate.  Roland thinks we should not
		   give away the old port.  I disagree; it can't actually hurt
		   because the old id's are still available, so it's no
		   security problem. */

		/* Nothing Happens. */
	      }
	    else
	      {
		if (isproc)
		  mach_port_deallocate (mach_task_self (), newport);
		else
		  {
		    mach_port_deallocate (mach_task_self (), *port);
		    *port = newport;
		  }
	      }
	    mach_port_destroy (mach_task_self (), ref);
	  }

      if (!eff_uids || !avail_uids || !eff_gids || !avail_gids)
	goto abandon_suid;	/* Allocation error; probably toast, but... */

      /* STEP 0: Fetch the user's current id's. */
      err = idvec_merge_auth (eff_uids, avail_uids, eff_gids, avail_gids,
			      ports[INIT_PORT_AUTH]);
      if (err)
	goto abandon_suid;

      already_root =
	idvec_contains (eff_uids, 0) || idvec_contains (avail_uids, 0);

      /* If the user's auth port is fraudulent, then these values will be
	 wrong.  No matter; we will repeat these checks using secure id sets
	 later if the port turns out to be bogus.  */
      if (suid)
	err = idvec_setid (eff_uids, avail_uids, uid, &_secure);
      if (sgid && !err)
	err = idvec_setid (eff_uids, avail_uids, gid, &_secure);
      if (err)
	goto abandon_suid;

      /* STEP 3: Attempt to create this new auth handle. */
      err = auth_makeauth (auth, &ports[INIT_PORT_AUTH],
			   MACH_MSG_TYPE_COPY_SEND, 1, 
			   eff_uids->ids, eff_uids->num,
			   avail_uids->ids, avail_uids->num,
			   eff_gids->ids, eff_gids->num,
			   avail_gids->ids, avail_gids->num,
			   &newauth);
      if (err == EINVAL && get_file_ids)
	/* The user's auth port was bogus.  As we can't trust what the user
	   has told us about ids, we use the authentication on the file being
	   execed (which we know is good), as the effective ids, and assume
	   no aux ids.  */
	{
	  /* Get rid of all ids from the bogus auth port.  */
	  idvec_clear (eff_uids);
	  idvec_clear (avail_uids);
	  idvec_clear (eff_gids);
	  idvec_clear (avail_gids);

	  /* Now add some from a source we trust.  */
	  err = (*get_file_ids)(eff_uids, eff_gids);

	  already_root = idvec_contains (eff_uids, 0);
	  if (suid && !err)
	    err = idvec_setid (eff_uids, avail_uids, uid, &_secure);
	  if (sgid && !err)
	    err = idvec_setid (eff_uids, avail_uids, gid, &_secure);
	  if (err)
	    goto abandon_suid;

	  /* Trrrry again...  */
	  err = auth_makeauth (auth, 0, MACH_MSG_TYPE_COPY_SEND, 1, 
			       eff_uids->ids, eff_uids->num,
			       avail_uids->ids, avail_uids->num,
			       eff_gids->ids, eff_gids->num,
			       avail_gids->ids, avail_gids->num,
			       &newauth);
	}

      if (err)
	goto abandon_suid;

      if (already_root)
	_secure = 0;		/* executive privilege */
      
      /* STEP 4: Re-authenticate all the ports we are handing to the user
	 with this new port, and install the new auth port in ports. */
      for (i = 0; i < num_fds; ++i)
	reauth (&fds[i], 0);
      if (_secure)
	/* Not worth doing; the exec server will just do it again.  */
	ports[INIT_PORT_CRDIR] = MACH_PORT_NULL;
      else
	reauth (&ports[INIT_PORT_CRDIR], 0);
      reauth (&ports[INIT_PORT_PROC], 1);
      reauth (&ports[INIT_PORT_CWDIR], 0);
      mach_port_deallocate (mach_task_self (), ports[INIT_PORT_AUTH]);
      ports[INIT_PORT_AUTH] = newauth;

      if (eff_uids->num > 0)
	proc_setowner (ports[INIT_PORT_PROC], eff_uids->ids[0]);

    abandon_suid:
      if (eff_uids)
	idvec_free (eff_uids);
      if (avail_uids)
	idvec_free (avail_uids);
      if (eff_gids)
	idvec_free (eff_gids);
      if (avail_gids)
	idvec_free (avail_gids);
    }

  if (_secure && secure)
    *secure = _secure;

  return err;
}
