/* FS helper library definitions
   Copyright (C) 1994, 1995 Free Software Foundation

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

#ifndef _HURD_FSHELP_
#define _HURD_FSHELP_

/* This library implements various things that are generic to
   all or most implementors of the filesystem protocol.  It 
   presumes that you are using the iohelp library as well.  It
   is divided into separate facilities which may be used independently.  */

#include <errno.h>
#include <mach.h>
#include <hurd/hurd_types.h>
#include <cthreads.h>


/* Passive translator linkage */
/* These routines are self-contained and start passive translators,
   returning the control port.  They do not require multi threading
   or the ports library. */

/* Start a passive translator NAME with arguments ARGZ (length
   ARGZ_LEN).  Initialize the initports to PORTS (length PORTS_LEN),
   the initints to INTS (length INTS_LEN), and the file descriptor
   table to FDS (length FDS_LEN).  Return the control port in
   *CONTROL.  If the translator doesn't respond or die in TIMEOUT
   milliseconds (if TIMEOUT > 0), return an appropriate error.  If the
   translator dies before responding, return EDIED.  */
error_t
fshelp_start_translator_long (mach_port_t underlying,
			      mach_msg_type_name_t underlying_type,
			      char *name, char *argz, int argz_len,
			      mach_port_t *fds, 
			      mach_msg_type_name_t fds_type, int fds_len,
			      mach_port_t *ports, 
			      mach_msg_type_name_t ports_type, int ports_len,
			      int *ints, int ints_len,
			      int timeout, fsys_t *control);


/* Same as fshelp_start_translator_long, except the initports and ints
   are copied from our own state, fd[2] is copied from our own stderr,
   and the other fds are cleared.  */
error_t
fshelp_start_translator (mach_port_t underlying, 
			 mach_msg_type_name_t  underlying_type,
			 char *name, char *argz, int argz_len,
			 int timeout, fsys_t *control);


/* Active translator linkage */

/* These routines implement the linkage to active translators needed
   by any filesystem which supports them.  They require cthreads and
   use the passive translator routines above, but they don't require
   the ports library at all.  */

struct transbox 
{
  fsys_t active;
  struct mutex *lock;
  int flags;
  struct condition wakeup;
  void *cookie;
};
#define TRANSBOX_STARTING 1
#define TRANSBOX_WANTED 2

/* This interface is complex, because creating the ports and state
   necessary for start_translator_long is expensive.  The caller to
   fshelp_fetch_root should not need to create them on every call, since
   usually there will be an existing active translator. */

/* This routine is called by fshelp_fetch_root to fetch more
   information.  Return the owner and group of the underlying
   translated file in *UID and *GID; return an unauthenticated
   node for the file itself in *UNDERLYING, and point *ARGZ at
   the entire passive translator spec for the file (setting
   *ARGZ_LEN to the length.)   If there is no passive 
   translator, then return ENOENT.   COOKIE1 is the cookie passed
   in fshelp_transbox_init.  COOKIE2 is the cookie passed in the
   call to fshelp_fetch_root.  */
typedef error_t (*fshelp_callback_t) (void *cookie1, void *cookie2,
				      mach_port_t *underlying,
				      uid_t *uid, gid_t *gid, 
				      char **argz, int *argz_len);


/* Fetch the root from TRANSBOX.  DOTDOT is an unauthenticated port
   for the directory in which we are looking; UIDS (length UIDS_LEN)
   and GIDS (length GIDS_LEN) are the ids of the user responsible for
   the call.  FLAGS are as for dir_pathtrans (but O_CREAT and O_EXCL
   are not meaningful and are ignored).  The trasnbox lock (as
   set by fshelp_transbox_init) must be held before the call, and will
   be held upon return, but may be released during the operation of
   the call.  */
error_t
fshelp_fetch_root (struct transbox *transbox, void *cookie,
		   file_t dotdot, 
		   uid_t *uids, int uids_len,
		   uid_t *gids, int gids_len,
		   int flags, fshelp_callback_t callback,
		   retry_type *retry, char *retryname, mach_port_t *root);

void
fshelp_transbox_init (struct transbox *transbox,
		      struct mutex *lock,
		      void *cookie);

/* Return true iff there is an active translator on this box */
extern inline int
fshelp_translated (struct transbox *box)
{
  return (box->active != MACH_PORT_NULL);
}

/* Atomically replace the existing active translator port for this box
   with NEWACTIVE.  If EXCL is non-zero then don't frob an existing
   active; return EBUSY instead.  */
error_t fshelp_set_active (struct transbox *box,
			   fsys_t newactive, int excl);

/* Fetch the control port to make a request on it.  It's a bad idea
   to do fsys_getroot with the result; use fetch_root instead. */
error_t fshelp_fetch_control (struct transbox *box,
			      mach_port_t *control);

/* A transbox is being deallocated, clean associated state. */
void fshelp_drop_transbox (struct transbox *box);



/* Flock handling. */
struct lock_box
{
  int type;
  struct condition wait;
  int waiting;
  int shcount;
};

/* Call when a user makes a request to acquire an lock via file_lock.
   There should be one lock box per object and one int per open; these
   are passed as arguments BOX and USER respectively.  FLAGS are as
   per file_lock.  MUT is a mutex which will be held whenever this
   routine is called, to lock BOX->wait.  */ 
error_t fshelp_acquire_lock (struct lock_box *box, int *user, 
			     struct mutex *mut, int flags);

  
/* Initialize lock_box BOX.  (The user int passed to fshelp_acquire_lock
   should be initialized with LOCK_UN.).  */
void fshelp_lock_init (struct lock_box *box);


#endif
