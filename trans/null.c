/* A translator for providing endless empty space and immediate eof.

   Copyright (C) 1995 Free Software Foundation, Inc.

   Written by Miles Bader <miles@gnu.ai.mit.edu>

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

#include <hurd.h>
#include <hurd/ports.h>
#include <hurd/trivfs.h>
#include <hurd/fsys.h>

#include <stdio.h>
#include <unistd.h>
#include <error.h>
#include <string.h>
#include <fcntl.h>
#include <limits.h>

/* ---------------------------------------------------------------- */

struct port_class *control_class;
struct port_class *node_class;
struct port_bucket *port_bucket;

struct port_class *trivfs_protid_portclasses[1];
struct port_class *trivfs_cntl_portclasses[1];
int trivfs_protid_nportclasses = 1;
int trivfs_cntl_nportclasses = 1;

/* If true, then reading from this device will yield an endless stream of
   zeros instead of immediate EOF.  This also makes it mappable.  */
static int provide_zeros = 0;

void
main (int argc, char **argv)
{
  error_t err;
  mach_port_t bootstrap;
  
  control_class = ports_create_class (trivfs_clean_cntl, 0);
  node_class = ports_create_class (trivfs_clean_protid, 0);
  port_bucket = ports_create_bucket ();
  trivfs_protid_portclasses[0] = node_class;
  trivfs_cntl_portclasses[0] = control_class;

  if (argc == 2 &&
      (strcmp(argv[1], "-z") == 0 || strcmp(argv[1], "--zero") == 0))
    provide_zeros = 1;
  else if (argc != 1)
    {
      fprintf(stderr, "Usage: %s [-z|--zero]", program_invocation_name);
      exit(1);
    }

  task_get_bootstrap_port (mach_task_self (), &bootstrap);
  if (bootstrap == MACH_PORT_NULL)
    error(1, 0, "must be started as a translator");

  /* Reply to our parent */
  err = trivfs_startup(bootstrap, control_class, port_bucket,
		       node_class, port_bucket, NULL);
  if (err)
    error(3, err, "Contacting parent");

  /* Launch. */
  ports_manage_port_operations_one_thread (port_bucket, trivfs_demuxer, 0);

  exit(0);
}

/* ---------------------------------------------------------------- */
/* Trivfs hooks  */

int trivfs_fstype = FSTYPE_DEV;
int trivfs_fsid = 0;

int trivfs_support_read = 1;
int trivfs_support_write = 1;
int trivfs_support_exec = 0;

int trivfs_allow_open = O_READ | O_WRITE;

void
trivfs_modify_stat (struct stat *st)
{
  st->st_blksize = vm_page_size * 256; /* Make transfers LARRRRRGE */

  st->st_size = 0;
  st->st_blocks = 0;

  st->st_mode &= ~S_IFMT;
  st->st_mode |= S_IFCHR;
}

error_t
trivfs_goaway (int flags, mach_port_t realnode, 
	       struct port_class *control_class, 
	       struct port_class *protid_class)
{
  exit(0);
}

/* ---------------------------------------------------------------- */

/* Return objects mapping the data underlying this memory object.  If
   the object can be read then memobjrd will be provided; if the
   object can be written then memobjwr will be provided.  For objects
   where read data and write data are the same, these objects will be
   equal, otherwise they will be disjoint.  Servers are permitted to
   implement io_map but not io_map_cntl.  Some objects do not provide
   mapping; they will set none of the ports and return an error.  Such
   objects can still be accessed by io_read and io_write.  */
kern_return_t
trivfs_S_io_map(struct trivfs_protid *cred,
		memory_object_t *rdobj,
		mach_msg_type_name_t *rdtype,
		memory_object_t *wrobj,
		mach_msg_type_name_t *wrtype)
{
  return EINVAL;		/* XXX should work! */
}

/* ---------------------------------------------------------------- */

/* Read data from an IO object.  If offset if -1, read from the object
   maintained file pointer.  If the object is not seekable, offset is
   ignored.  The amount desired to be read is in AMT.  */
kern_return_t
trivfs_S_io_read(struct trivfs_protid *cred,
		 mach_port_t reply, mach_msg_type_name_t replytype,
		 vm_address_t *data,
		 mach_msg_type_number_t *datalen,
		 off_t offs,
		 mach_msg_type_number_t amt)
{
  error_t err = 0;

  if (!cred)
    err = EOPNOTSUPP;
  else if (!(cred->po->openmodes & O_READ))
    err = EBADF;
  else if (provide_zeros)
    {
      if (amt > *datalen)
	err = vm_allocate(mach_task_self(), data, amt, 1);
      else
	bzero((void *)*data, amt);
      *datalen = amt;
    }
  else
    *datalen = 0;

  return 0;
}

/* ---------------------------------------------------------------- */

/* Tell how much data can be read from the object without blocking for
   a "long time" (this should be the same meaning of "long time" used
   by the nonblocking flag.  */
kern_return_t
trivfs_S_io_readable (struct trivfs_protid *cred,
		      mach_port_t reply, mach_msg_type_name_t replytype,
		      mach_msg_type_number_t *amount)
{
  if (!cred)
    return EOPNOTSUPP;
  else if (!(cred->po->openmodes & O_READ))
    return EINVAL;
  else if (provide_zeros)
    *amount = INT_MAX;
  else
    *amount = 0;
  return 0;
}

/* ---------------------------------------------------------------- */

/* Change current read/write offset */
kern_return_t
trivfs_S_io_seek (struct trivfs_protid *cred,
		  mach_port_t reply, mach_msg_type_name_t replytype,
		  off_t offset, int whence, off_t *new_offset)
{
  if (!cred)
    return EOPNOTSUPP;
  *new_offset = 0;
  return 0;
}

/* ---------------------------------------------------------------- */

/* SELECT_TYPE is the bitwise OR of SELECT_READ, SELECT_WRITE, and SELECT_URG.
   Block until one of the indicated types of i/o can be done "quickly", and
   return the types that are then available.  ID_TAG is returned as passed; it
   is just for the convenience of the user in matching up reply messages with
   specific requests sent.  */
kern_return_t
trivfs_S_io_select (struct trivfs_protid *cred,
		    mach_port_t reply, mach_msg_type_name_t replytype,
		    int *type, int *tag)
{
  if (!cred)
    return EOPNOTSUPP;
  else if (((*type & SELECT_READ) && !(cred->po->openmodes & O_READ))
	   || ((*type & SELECT_WRITE) && !(cred->po->openmodes & O_WRITE)))
    return EBADF;
  else
    *type &= ~SELECT_URG;
  return 0;
}

/* ---------------------------------------------------------------- */

/* Write data to an IO object.  If offset is -1, write at the object
   maintained file pointer.  If the object is not seekable, offset is
   ignored.  The amount successfully written is returned in amount.  A
   given user should not have more than one outstanding io_write on an
   object at a time; servers implement congestion control by delaying
   responses to io_write.  Servers may drop data (returning ENOBUFS)
   if they recevie more than one write when not prepared for it.  */
kern_return_t
trivfs_S_io_write (struct trivfs_protid *cred,
		   mach_port_t reply, mach_msg_type_name_t replytype,
		   vm_address_t data, mach_msg_type_number_t datalen,
		   off_t offs, mach_msg_type_number_t *amt)
{
  if (!cred)
    return EOPNOTSUPP;
  else if (!(cred->po->openmodes & O_WRITE))
    return EBADF;
  *amt = datalen;
  return 0;
}

/* ---------------------------------------------------------------- */

/* Truncate file.  */
kern_return_t
trivfs_S_file_truncate (struct trivfs_protid *cred, off_t size)
{
  return 0;
}

/* ---------------------------------------------------------------- */
/* These four routines modify the O_APPEND, O_ASYNC, O_FSYNC, and
   O_NONBLOCK bits for the IO object. In addition, io_get_openmodes
   will tell you which of O_READ, O_WRITE, and O_EXEC the object can
   be used for.  The O_ASYNC bit affects icky async I/O; good async 
   I/O is done through io_async which is orthogonal to these calls. */

kern_return_t
trivfs_S_io_get_openmodes (struct trivfs_protid *cred,
			   mach_port_t reply, mach_msg_type_name_t replytype,
			   int *bits)
{
  if (!cred)
    return EOPNOTSUPP;
  else
    {
      *bits = cred->po->openmodes;
      return 0;
    }
}

error_t
trivfs_S_io_set_all_openmodes(struct trivfs_protid *cred,
			      mach_port_t reply,
			      mach_msg_type_name_t replytype,
			      int mode)
{
  if (!cred)
    return EOPNOTSUPP;
  else
    return 0;
}

kern_return_t
trivfs_S_io_set_some_openmodes (struct trivfs_protid *cred,
				mach_port_t reply,
				mach_msg_type_name_t replytype,
				int bits)
{
  if (!cred)
    return EOPNOTSUPP;
  else
    return 0;
}

kern_return_t
trivfs_S_io_clear_some_openmodes (struct trivfs_protid *cred,
				  mach_port_t reply,
				  mach_msg_type_name_t replytype,
				  int bits)
{
  if (!cred)
    return EOPNOTSUPP;
  else
    return 0;
}

/* ---------------------------------------------------------------- */
/* Get/set the owner of the IO object.  For terminals, this affects
   controlling terminal behavior (see term_become_ctty).  For all
   objects this affects old-style async IO.  Negative values represent
   pgrps.  This has nothing to do with the owner of a file (as
   returned by io_stat, and as used for various permission checks by
   filesystems).  An owner of 0 indicates that there is no owner.  */

kern_return_t
trivfs_S_io_get_owner (struct trivfs_protid *cred,
		       mach_port_t reply,
		       mach_msg_type_name_t replytype,
		       pid_t *owner)
{
  if (!cred)
    return EOPNOTSUPP;
  *owner = 0;
  return 0;
}

kern_return_t
trivfs_S_io_mod_owner (struct trivfs_protid *cred,
		       mach_port_t reply, mach_msg_type_name_t replytype,
		       pid_t owner)
{
  if (!cred)
    return EOPNOTSUPP;
  else
    return EINVAL;
}
