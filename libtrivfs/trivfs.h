/* 
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

#ifndef __TRIVFS_H__
#define __TRIVFS_H__

#include <errno.h>
#include <cthreads.h>		/* for mutexes &c */
#include <sys/types.h>		/* for uid_t &c */
#include <mach/mach.h>
#include <hurd/ports.h>

struct trivfs_protid
{
  struct port_info pi;
  uid_t *uids, *gids;
  int nuids, ngids;
  int isroot;
  /* REALNODE will be null if this protid wasn't fully created (currently
     only in the case where trivfs_protid_create_hook returns an error).  */
  mach_port_t realnode;		/* restricted permissions */
  void *hook;			/* for user use */
  struct trivfs_peropen *po;
};

/* These can be used as `intran' and `destructor' functions for
   a MiG port type, to have the stubs called with the protid pointer.  */
struct trivfs_protid *trivfs_begin_using_protid (mach_port_t);
void trivfs_end_using_protid (struct trivfs_protid *);

struct trivfs_peropen
{
  void *hook;			/* for user use */
  int openmodes;
  int refcnt;
  struct trivfs_control *cntl;
};

struct trivfs_control
{
  struct port_info pi;
  struct mutex lock;
  struct port_class *protid_class;
  struct port_bucket *protid_bucket;
  mach_port_t underlying;
  void *hook;			/* for user use */
};

/* These can be used as `intran' and `destructor' functions for
   a MiG port type, to have the stubs called with the control pointer.  */
struct trivfs_control *trivfs_begin_using_control (mach_port_t);
void trivfs_end_using_control (struct trivfs_control *);


/* The user must define these variables. */
extern int trivfs_fstype;
extern int trivfs_fsid;

/* Set these if trivfs should allow read, write, 
   or execute of file.    */
extern int trivfs_support_read;
extern int trivfs_support_write;
extern int trivfs_support_exec;

/* Set this some combination of O_READ, O_WRITE, and O_EXEC;
   trivfs will only allow opens of the specified modes.
   (trivfs_support_* is not used to validate opens, only actual
   operations.)  */
extern int trivfs_allow_open;

extern struct port_class *trivfs_protid_portclasses[];
extern int trivfs_protid_nportclasses;
extern struct port_class *trivfs_cntl_portclasses[];
extern int trivfs_cntl_nportclasses;

/* The user must define this function.  This should modify a struct 
   stat (as returned from the underlying node) for presentation to
   callers of io_stat.  It is permissable for this function to do
   nothing.  */
void trivfs_modify_stat (struct trivfs_protid *cred, struct stat *);

/* If this variable is set, it is called every time an open happens.
   UIDS, GIDS, and FLAGS are from the open; CNTL identifies the
   node being opened.  This call need not check permissions on the underlying
   node.  This call can block as necessary, unless O_NONBLOCK is set
   in FLAGS.  Any desired error can be returned, which will be reflected
   to the user and prevent the open from succeeding.  */
error_t (*trivfs_check_open_hook) (struct trivfs_control *cntl,
				   uid_t *uids, u_int nuids,
				   gid_t *gids, u_int ngids,
				   int flags);

/* If this variable is set, it is called every time a new protid
   structure is created and initialized. */
error_t (*trivfs_protid_create_hook) (struct trivfs_protid *);

/* If this variable is set, it is called every time a new peropen
   structure is created and initialized. */
error_t (*trivfs_peropen_create_hook) (struct trivfs_peropen *);

/* If this variable is set, it is called every time a protid structure
   is about to be destroyed. */
void (*trivfs_protid_destroy_hook) (struct trivfs_protid *);

/* If this variable is set, it is called every time a peropen structure
   is about to be destroyed. */
void (*trivfs_peropen_destroy_hook) (struct trivfs_peropen *);

/* Creates a control port for this filesystem and sends it to BOOTSTRAP with
   fsys_startup.  CONTROL_TYPE is the ports library type for the control
   port, and PROTID_TYPE is the type for ports representing opens of this
   node.  If CONTROL isn't NULL, the trivfs control port is return in it.  If
   any error occurs sending fsys_startup, it is returned, otherwise 0.  */
error_t trivfs_startup(mach_port_t bootstrap,
		       struct port_class *control_class,
		       struct port_bucket *control_bucket,
		       struct port_class *protid_class,
		       struct port_bucket *protid_bucket,
		       struct trivfs_control **control);

/* Call this to create a new control port and return a receive right
   for it; exactly one send right must be created from the returned
   receive right.  UNDERLYING is the underlying port, such as fsys_startup
   returns as the realnode.  PROTIDTYPE is the ports type to be used
   for ports that refer to this underlying node.  CNTLTYPE is the ports type
   to be used for the control port for this node. */
mach_port_t trivfs_handle_port (mach_port_t underlying, 
				struct port_class *control_class,
				struct port_bucket *control_bucket,
				struct port_class *protid_class,
				struct port_bucket *protid_bucket);

/* Install these as libports cleanroutines for trivfs_protid_class
   and trivfs_cntl_class respectively. */
void trivfs_clean_protid (void *);
void trivfs_clean_cntl (void *);

/* This demultiplees messages for trivfs ports. */
int trivfs_demuxer (mach_msg_header_t *, mach_msg_header_t *);

/* Return a new protid pointing to a new peropen in CRED, with REALNODE as
   the underlying node reference, with the given identity, and open flags in
   FLAGS.  CNTL is the trivfs control object.  */
error_t trivfs_open (struct trivfs_control *fsys,
		     uid_t *uids, unsigned num_uids,
		     gid_t *gids, unsigned num_gids,
		     unsigned flags,
		     mach_port_t realnode,
		     struct trivfs_protid **cred);

/* Return a duplicate of CRED in DUP, sharing the same peropen and hook.  A
   non-null hook may be used to detect that this is a duplicate by
   trivfs_peropen_create_hook.  */
error_t trivfs_protid_dup (struct trivfs_protid *cred,
			   struct trivfs_protid **dup);

/* The user must define this function.  Someone wants the filesystem
   CNTL to go away.  FLAGS are from the set FSYS_GOAWAY_*. */
error_t trivfs_goaway (struct trivfs_control *cntl, int flags);

/* Call this to set atime for the node to the current time.  */
error_t trivfs_set_atime (struct trivfs_control *cntl);

/* Call this to set mtime for the node to the current time. */
error_t trivfs_set_mtime (struct trivfs_control *cntl);

#endif /* __TRIVFS_H__ */
