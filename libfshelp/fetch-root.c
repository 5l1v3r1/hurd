/* 
   Copyright (C) 1995 Free Software Foundation, Inc.
   Written by Michael I. Bushnell.

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

#include "trans.h"
#include <unistd.h>
#include <assert.h>
#include <string.h>
#include <hurd/fsys.h>

error_t
fshelp_fetch_root (struct transbox *box, void *cookie,
		   file_t dotdot,
		   uid_t *uids, int uids_len,
		   uid_t *gids, int gids_len,
		   int flags,
		   fshelp_fetch_root_callback1_t callback1,
		   fshelp_fetch_root_callback2_t callback2,
		   retry_type *retry, char *retryname, 
		   file_t *root)
{
  error_t err;
  mach_port_t control;
  
 start_over:

  if (box->active != MACH_PORT_NULL)
    assert ((box->flags & TRANSBOX_STARTING) == 0);
  else
    {
      uid_t uid, gid;
      char *argz;
      int argz_len;
      error_t err;
      mach_port_t ports[INIT_PORT_MAX];
      int ints[INIT_INT_MAX];
      mach_port_t fds[STDERR_FILENO + 1];
      auth_t ourauth, newauth;
      int uidarray[2], gidarray[2];
      
      mach_port_t
	reauth (mach_port_t port, mach_msg_type_name_t port_type)
	  {
	    mach_port_t rend, ret;
	    error_t err;

	    if (port == MACH_PORT_NULL)
	      return port;

	    rend = mach_reply_port ();
	    err = io_reauthenticate (port, rend, 
				     MACH_MSG_TYPE_MAKE_SEND);
	    assert_perror (err);

	    if (port_type == MACH_MSG_TYPE_MAKE_SEND)
	      mach_port_insert_right (mach_task_self (), port, port,port_type);

	    err = auth_user_authenticate (newauth, port, rend,
					  MACH_MSG_TYPE_MAKE_SEND,
					  &ret);
	    if (err)
	      ret = MACH_PORT_NULL;
	  
	    mach_port_destroy (mach_task_self (), rend);
	    if (!err && port_type != MACH_MSG_TYPE_COPY_SEND)
	      mach_port_deallocate (mach_task_self (), port);

	    return ret;
	  }
      error_t fetch_underlying (int flags, mach_port_t *underlying,
				mach_msg_type_name_t *underlying_type)
	{
	  error_t err =
	    (*callback2) (box->cookie, cookie, flags,
			  underlying, underlying_type);
	  if (!err)
	    {
	      *underlying = reauth (*underlying, *underlying_type);
	      *underlying_type = MACH_MSG_TYPE_MOVE_SEND;
	    }
	  return err;
	}
      
      if (box->flags & TRANSBOX_STARTING)
	{
	  box->flags |= TRANSBOX_WANTED;
	  condition_wait (&box->wakeup, box->lock);
	  goto start_over;
	}
      box->flags |= TRANSBOX_STARTING;
      mutex_unlock (box->lock);
	  
      err = (*callback1) (box->cookie, cookie, &uid, &gid, &argz, &argz_len);
      if (err)
	goto return_error;
      
      ourauth = getauth ();
      uidarray[0] = uidarray[1] = uid;
      gidarray[0] = gidarray[1] = gid;
      err = auth_makeauth (ourauth, 0, MACH_MSG_TYPE_MAKE_SEND, 0,
			   uidarray, 1, uidarray, 2,
			   gidarray, 1, gidarray, 2, &newauth);
      assert_perror (err);
      
      bzero (ports, INIT_PORT_MAX * sizeof (mach_port_t));
      bzero (fds, (STDERR_FILENO + 1) * sizeof (mach_port_t));
      bzero (ints, INIT_INT_MAX * sizeof (int));
      
      ports[INIT_PORT_CWDIR] = reauth (getcwdir (), MACH_MSG_TYPE_MOVE_SEND);
      ports[INIT_PORT_CRDIR] = reauth (getcrdir (), MACH_MSG_TYPE_MOVE_SEND);
      ports[INIT_PORT_AUTH] = newauth;
      
      fds[STDERR_FILENO] =
	reauth (getdport (STDERR_FILENO), MACH_MSG_TYPE_MOVE_SEND);
      
      err = fshelp_start_translator_long (fetch_underlying,
					  argz, argz, argz_len,
					  fds, MACH_MSG_TYPE_MOVE_SEND,
					  STDERR_FILENO + 1,
					  ports, MACH_MSG_TYPE_MOVE_SEND,
					  INIT_PORT_MAX, 
					  ints, INIT_INT_MAX,
					  0, &control);
      
      mutex_lock (box->lock);
      
      free (argz);

    return_error:
      
      box->flags &= ~TRANSBOX_STARTING;
      if (box->flags & TRANSBOX_WANTED)
	{
	  box->flags &= ~TRANSBOX_WANTED;
	  condition_broadcast (&box->wakeup);
	}

      if (err)
	return err;
      
      box->active = control;
    }
  
  control = box->active;
  mach_port_mod_refs (mach_task_self (), control, 
		      MACH_PORT_RIGHT_SEND, 1);
  mutex_unlock (box->lock);
  
  /* Cancellation point XXX */
  err = fsys_getroot (control, dotdot, MACH_MSG_TYPE_COPY_SEND,
		      uids, uids_len, gids, gids_len, flags,
		      retry, retryname, root);
  
  mutex_lock (box->lock);
  
  if ((err == MACH_SEND_INVALID_DEST || err == MIG_SERVER_DIED)
      && control == box->active)
    fshelp_set_active (box, MACH_PORT_NULL, 0);
  mach_port_deallocate (mach_task_self (), control);

  if (err == MACH_SEND_INVALID_DEST || err == MIG_SERVER_DIED)
    goto start_over;
  
  return err;
}

		      
  
