/* fakeroot -- a translator for faking actions that aren't really permitted
   Copyright (C) 2002 Free Software Foundation, Inc.

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
   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA */

#include <hurd/netfs.h>
#include <stdio.h>
#include <stdlib.h>
#include <argp.h>
#include <error.h>
#include <string.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <cthreads.h>
#include <hurd/ihash.h>
#include <hurd/paths.h>

#include <version.h>

const char *argp_program_version = STANDARD_HURD_VERSION (fakeroot);

int netfs_maxsymlinks = 16;	/* arbitrary */

struct netnode
{
  void **idport_locp;		/* easy removal pointer in idport ihash */
  mach_port_t idport;		/* port from io_identity */
  int openmodes;		/* O_READ | O_WRITE | O_EXEC */
  file_t file;			/* port on real file */

  unsigned int faked;
};

#define FAKE_UID	(1 << 0)
#define FAKE_GID	(1 << 1)
#define FAKE_AUTHOR	(1 << 2)
#define FAKE_MODE	(1 << 3)
#define FAKE_REFERENCE	(1 << 4) /* got node_norefs with st_nlink > 0 */

struct mutex idport_ihash_lock = MUTEX_INITIALIZER;
struct ihash idport_ihash;


static error_t
new_node (file_t file, mach_port_t idport, int openmodes, struct node **np)
{
  error_t err;
  struct netnode *nn = calloc (1, sizeof *nn);
  if (nn == 0)
    {
      mach_port_deallocate (mach_task_self (), file);
      if (idport != MACH_PORT_NULL)
	mach_port_deallocate (mach_task_self (), idport);
      return ENOMEM;
    }
  nn->file = file;
  nn->openmodes = openmodes;
  if (idport != MACH_PORT_NULL)
    nn->idport = idport;
  else
    {
      int fileno;
      mach_port_t fsidport;
      err = io_identity (file, &nn->idport, &fsidport, &fileno);
      if (err)
	{
	  mach_port_deallocate (mach_task_self (), file);
	  free (nn);
	  return err;
	}
    }
  *np = netfs_make_node (nn);
  if (*np == 0)
    err = ENOMEM;
  else
    {
      mutex_lock (&idport_ihash_lock);
      err = ihash_add (&idport_ihash, nn->idport, *np, &nn->idport_locp);
      mutex_unlock (&idport_ihash_lock);
    }
  if (err)
    {
      mach_port_deallocate (mach_task_self (), nn->idport);
      mach_port_deallocate (mach_task_self (), file);
      free (nn);
    }
  return err;
}

/* Node NP has no more references; free all its associated storage. */
void
netfs_node_norefs (struct node *np)
{
  if (np->nn->faked != 0
      && netfs_validate_stat (np, 0) == 0 && np->nn_stat.st_nlink > 0)
    {
      /* The real node still exists and we have faked some attributes.
	 We must keep our node alive in core to retain those values.
	 XXX
	 For now, we will leak the node if it gets deleted later.
	 That will keep the underlying file alive with st_nlink=0
	 until this fakeroot filesystem dies.  One easy solution
	 would be to scan nodes with references=1 for st_nlink=0
	 at some convenient time, periodically or in syncfs.  */
      if ((np->nn->faked & FAKE_REFERENCE) == 0)
	{
	  np->nn->faked |= FAKE_REFERENCE;
	  ++np->references;
	}
      mutex_unlock (&np->lock);
      return;
    }

  spin_unlock (&netfs_node_refcnt_lock); /* Avoid deadlock.  */
  mutex_lock (&idport_ihash_lock);
  spin_lock (&netfs_node_refcnt_lock);
  /* Previous holder of this lock might have just got a reference.  */
  if (np->references > 0)
    {
      mutex_unlock (&idport_ihash_lock);
      return;
    }
  ihash_locp_remove (&idport_ihash, np->nn->idport_locp);
  mutex_unlock (&idport_ihash_lock);
  mach_port_deallocate (mach_task_self (), np->nn->file);
  mach_port_deallocate (mach_task_self (), np->nn->idport);
  free (np->nn);
  free (np);
}

/* Make sure that NP->nn_stat is filled with the most current information.
   CRED identifies the user responsible for the operation. NP is locked.  */
error_t
netfs_validate_stat (struct node *np, struct iouser *cred)
{
  struct stat st;
  error_t err = io_stat (np->nn->file, &st);
  if (err)
    return err;

  if (np->nn->faked & FAKE_UID)
    st.st_uid = np->nn_stat.st_uid;
  if (np->nn->faked & FAKE_GID)
    st.st_gid = np->nn_stat.st_gid;
  if (np->nn->faked & FAKE_AUTHOR)
    st.st_author = np->nn_stat.st_author;
  if (np->nn->faked & FAKE_MODE)
    st.st_mode = np->nn_stat.st_mode;

  np->nn_stat = st;
  return 0;
}

error_t
netfs_attempt_chown (struct iouser *cred, struct node *np,
		     uid_t uid, uid_t gid)
{
  if (uid != -1)
    {
      np->nn->faked |= FAKE_UID;
      np->nn_stat.st_uid = uid;
    }
  if (gid != -1)
    {
      np->nn->faked |= FAKE_GID;
      np->nn_stat.st_gid = gid;
    }
  return 0;
}

error_t
netfs_attempt_chauthor (struct iouser *cred, struct node *np, uid_t author)
{
  np->nn->faked |= FAKE_AUTHOR;
  np->nn_stat.st_author = author;
  return 0;
}

/* Return the mode that the real underlying file should have if the
   fake mode is being set to MODE.  We always give ourselves read and
   write permission so that we can open the file as root would be able
   to.  We give ourselves execute permission iff any execute bit is
   set in the fake mode.  */
static inline mode_t
real_from_fake_mode (mode_t mode)
{
  return mode | S_IREAD | S_IWRITE | (((mode << 3) | (mode << 6)) & S_IEXEC);
}

/* This should attempt a chmod call for the user specified by CRED on
   locked node NODE, to change the mode to MODE.  Unlike the normal Unix
   and Hurd meaning of chmod, this function is also used to attempt to
   change files into other types.  If such a transition is attempted which
   is impossible, then return EOPNOTSUPP.  */
error_t
netfs_attempt_chmod (struct iouser *cred, struct node *np, mode_t mode)
{
  if ((mode & S_IFMT) != (np->nn_stat.st_mode & S_IFMT))
    return EOPNOTSUPP;
  if (((mode | (mode << 3) | (mode << 6))
       ^ (np->nn_stat.st_mode | (np->nn_stat.st_mode << 3)
	  | (np->nn_stat.st_mode << 6)))
      & S_IEXEC)
    {
      /* We are changing the executable bit, so this is not all fake.  We
	 don't bother with error checking since the fake mode change should
	 always succeed--worst case a later open will get EACCES.  */
      (void) file_chmod (np->nn->file, real_from_fake_mode (mode));
    }
  np->nn->faked |= FAKE_MODE;
  np->nn_stat.st_mode = mode;
  return 0;
}

/* The user must define this function.  Attempt to turn locked node NP
   (user CRED) into a symlink with target NAME.  */
error_t
netfs_attempt_mksymlink (struct iouser *cred, struct node *np, char *name)
{
  char trans[sizeof _HURD_SYMLINK + np->nn_stat.st_size + 1];
  memcpy (trans, _HURD_SYMLINK, sizeof _HURD_SYMLINK);
  memcpy (&trans[sizeof _HURD_SYMLINK], name, np->nn_stat.st_size + 1);
  return file_set_translator (np->nn->file,
			      FS_TRANS_EXCL|FS_TRANS_SET,
			      FS_TRANS_EXCL|FS_TRANS_SET, 0,
			      trans, sizeof trans,
			      MACH_PORT_NULL, MACH_MSG_TYPE_COPY_SEND);
}

error_t
netfs_attempt_mkdev (struct iouser *cred, struct node *np,
		     mode_t type, dev_t indexes)
{
  char *trans = 0;
  int translen = asprintf (&trans, "%s%c%d%c%d",
			   S_ISCHR (type) ? _HURD_CHRDEV : _HURD_BLKDEV,
			   '\0', major (indexes), '\0', minor (indexes));
  if (trans == 0)
    return ENOMEM;
  else
    {
      error_t err = file_set_translator (np->nn->file,
					 FS_TRANS_EXCL|FS_TRANS_SET,
					 FS_TRANS_EXCL|FS_TRANS_SET, 0,
					 trans, translen + 1,
					 MACH_PORT_NULL,
					 MACH_MSG_TYPE_COPY_SEND);
      free (trans);
      return err;
    }
}

error_t
netfs_attempt_chflags (struct iouser *cred, struct node *np, int flags)
{
  return file_chflags (np->nn->file, flags);
}

error_t
netfs_attempt_utimes (struct iouser *cred, struct node *np,
		      struct timespec *atime, struct timespec *mtime)
{
  struct timeval a, m;
  if (atime)
    {
      TIMESPEC_TO_TIMEVAL (&a, atime);
    }
  else
    a.tv_sec = a.tv_usec = -1;
  if (mtime)
    {
      TIMESPEC_TO_TIMEVAL (&m, mtime);
    }
  else
    m.tv_sec = m.tv_usec = -1;

  return file_utimes (np->nn->file,
		      *(time_value_t *) &a, *(time_value_t *) &m);
}

error_t
netfs_attempt_set_size (struct iouser *cred, struct node *np, off_t size)
{
  return file_set_size (np->nn->file, size);
}

error_t
netfs_attempt_statfs (struct iouser *cred, struct node *np, struct statfs *st)
{
  return file_statfs (np->nn->file, st);
}

error_t
netfs_attempt_sync (struct iouser *cred, struct node *np, int wait)
{
  return file_sync (np->nn->file, wait, 0);
}

error_t
netfs_attempt_syncfs (struct iouser *cred, int wait)
{
  return 0;
}

/* Lookup NAME in DIR (which is locked) for USER; set *NP to the found name
   upon return.  If the name was not found, then return ENOENT.  On any
   error, clear *NP.  (*NP, if found, should be locked and a reference to
   it generated.  This call should unlock DIR no matter what.)  */
error_t
netfs_attempt_lookup (struct iouser *user, struct node *dir,
		      char *name, struct node **np)
{
  error_t err;
  int flags;
  const file_t dirfile = dir->nn->file;
  file_t file;

  /* We must unlock the directory before making RPCs to the underlying
     filesystem in case they somehow wind up trying to refer back to one of
     our nodes.  The DIRFILE port will not change or die as long as DIR
     lives, and our caller holds a reference keeping it alive.  */
  mutex_unlock (&dir->lock);

  flags = O_RDWR|O_EXEC;
  file = file_name_lookup_under (dirfile, name, flags | O_NOLINK, 0);
  if (file == MACH_PORT_NULL && (errno == EACCES || errno == EOPNOTSUPP
				 || errno == EROFS || errno == EISDIR))
    {
      flags = O_RDWR;
      file = file_name_lookup_under (dirfile, name, flags | O_NOLINK, 0);
    }
  if (file == MACH_PORT_NULL && (errno == EACCES || errno == EOPNOTSUPP
				 || errno == EROFS || errno == EISDIR))
    {
      flags = O_READ|O_EXEC;
      file = file_name_lookup_under (dirfile, name, flags | O_NOLINK, 0);
    }
  if (file == MACH_PORT_NULL && (errno == EACCES || errno == EOPNOTSUPP
				 || errno == EISDIR))
    {
      flags = O_READ;
      file = file_name_lookup_under (dirfile, name, flags | O_NOLINK, 0);
    }
  if (file == MACH_PORT_NULL && (errno == EACCES || errno == EOPNOTSUPP
				 || errno == EISDIR))
    {
      flags = 0;
      file = file_name_lookup_under (dirfile, name, flags | O_NOLINK, 0);
    }
  *np = 0;
  if (file == MACH_PORT_NULL)
    err = errno;
  else
    {
      mach_port_t idport, fsidport;
      int fileno;
      err = io_identity (file, &idport, &fsidport, &fileno);
      if (err)
	mach_port_deallocate (mach_task_self (), file);
      else
	{
	  mach_port_deallocate (mach_task_self (), fsidport);
	  if (fsidport == netfs_fsys_identity)
	    {
	      /* Talking to ourselves!  We just looked up one of our own nodes.
		 Find the node and return it.  */
	      struct protid *cred = ports_lookup_port (netfs_port_bucket, file,
						       netfs_protid_class);
	      mach_port_deallocate (mach_task_self (), idport);
	      mach_port_deallocate (mach_task_self (), file);
	      if (cred == 0)
		return EGRATUITOUS;
	      *np = cred->po->np;
	      netfs_nref (*np);
	      ports_port_deref (cred);
	    }
	  else
	    {
	      mutex_lock (&idport_ihash_lock);
	      *np = ihash_find (&idport_ihash, idport);
	      if (*np != 0)
		{
		  /* We already know about this node.  */
		  mach_port_deallocate (mach_task_self (), idport);
		  mach_port_deallocate (mach_task_self (), file);
		  netfs_nref (*np);
		  mutex_unlock (&idport_ihash_lock);
		}
	      else
		{
		  mutex_unlock (&idport_ihash_lock);
		  err = new_node (file, idport, flags, np);
		}
	    }
	}
    }

  if (*np)
    mutex_lock (&(*np)->lock);
  return err;
}

error_t
netfs_attempt_mkdir (struct iouser *user, struct node *dir,
		     char *name, mode_t mode)
{
  return dir_mkdir (dir->nn->file, name, mode | S_IRWXU);
}


/* XXX
   Removing a node should mark the netnode so that it is GC'd when
   it has no hard refs.
 */

error_t
netfs_attempt_unlink (struct iouser *user, struct node *dir, char *name)
{
  return dir_unlink (dir->nn->file, name);
}

error_t
netfs_attempt_rename (struct iouser *user, struct node *fromdir,
		      char *fromname, struct node *todir,
		      char *toname, int excl)
{
  return dir_rename (fromdir->nn->file, fromname,
		     todir->nn->file, toname, excl);
}

error_t
netfs_attempt_rmdir (struct iouser *user,
		     struct node *dir, char *name)
{
  return dir_rmdir (dir->nn->file, name);
}

error_t
netfs_attempt_link (struct iouser *user, struct node *dir,
		    struct node *file, char *name, int excl)
{
  return dir_link (dir->nn->file, file->nn->file, name, excl);
}

error_t
netfs_attempt_mkfile (struct iouser *user, struct node *dir,
		      mode_t mode, struct node **np)
{
  file_t newfile;
  error_t err = dir_mkfile (dir->nn->file, O_RDWR|O_EXEC,
			    real_from_fake_mode (mode), &newfile);
  if (err == 0)
    err = new_node (newfile, MACH_PORT_NULL, O_RDWR|O_EXEC, np);
  mutex_unlock (&dir->lock);
  return err;
}

error_t
netfs_attempt_create_file (struct iouser *user, struct node *dir,
			   char *name, mode_t mode, struct node **np)
{
  file_t newfile = file_name_lookup_under (dir->nn->file, name,
					   O_CREAT|O_RDWR|O_EXEC,
					   real_from_fake_mode (mode));
  mutex_unlock (&dir->lock);
  if (newfile == MACH_PORT_NULL)
    return errno;
  return new_node (newfile, MACH_PORT_NULL, O_RDWR|O_EXEC, np);
}

error_t
netfs_attempt_readlink (struct iouser *user, struct node *np, char *buf)
{
  char transbuf[sizeof _HURD_SYMLINK + np->nn_stat.st_size + 1];
  char *trans = transbuf;
  size_t translen = sizeof transbuf;
  error_t err = file_get_translator (np->nn->file, &trans, &translen);
  if (err == 0)
    {
      if (translen < sizeof _HURD_SYMLINK
	  || memcmp (trans, _HURD_SYMLINK, sizeof _HURD_SYMLINK) != 0)
	err = EINVAL;
      else
	{
	  assert (translen <= sizeof _HURD_SYMLINK + np->nn_stat.st_size + 1);
	  memcpy (buf, &trans[sizeof _HURD_SYMLINK],
		  translen - sizeof _HURD_SYMLINK);
	}
      if (trans != transbuf)
	munmap (trans, translen);
    }
  return err;
}

error_t
netfs_check_open_permissions (struct iouser *user, struct node *np,
			      int flags, int newnode)
{
  return (flags & (O_READ|O_WRITE|O_EXEC) & ~np->nn->openmodes) ? EACCES : 0;
}

error_t
netfs_attempt_read (struct iouser *cred, struct node *np,
		    off_t offset, size_t *len, void *data)
{
  char *buf = data;
  error_t err = io_read (np->nn->file, &buf, len, offset, *len);
  if (err == 0 && buf != data)
    {
      memcpy (data, buf, *len);
      munmap (buf, *len);
    }
  return err;
}

error_t
netfs_attempt_write (struct iouser *cred, struct node *np,
		     off_t offset, size_t *len, void *data)
{
  return io_write (np->nn->file, data, *len, offset, len);
}

error_t
netfs_report_access (struct iouser *cred, struct node *np, int *types)
{
  *types = np->nn->openmodes
    & ((np->nn_stat.st_mode & (S_IXUSR|S_IXGRP|S_IXOTH)) ? ~0 : ~O_EXEC);
  return 0;
}

error_t
netfs_get_dirents (struct iouser *cred, struct node *dir,
		   int entry, int nentries, char **data,
		   mach_msg_type_number_t *datacnt,
		   vm_size_t bufsize, int *amt)
{
  return dir_readdir (dir->nn->file, data, datacnt,
		      entry, nentries, bufsize, amt);
}

error_t
netfs_file_get_storage_info (struct iouser *cred,
			     struct node *np,
			     mach_port_t **ports,
			     mach_msg_type_name_t *ports_type,
			     mach_msg_type_number_t *num_ports,
			     int **ints,
			     mach_msg_type_number_t *num_ints,
			     off_t **offsets,
			     mach_msg_type_number_t *num_offsets,
			     char **data,
			     mach_msg_type_number_t *data_len)
{
  *ports_type = MACH_MSG_TYPE_MOVE_SEND;
  return file_get_storage_info (np->nn->file,
				ports, num_ports,
				ints, num_ints,
				offsets, num_offsets,
				data, data_len);
}

kern_return_t
netfs_S_file_exec (struct protid *user,
                   task_t task,
                   int flags,
                   char *argv,
                   size_t argvlen,
                   char *envp,
                   size_t envplen,
                   mach_port_t *fds,
                   size_t fdslen,
                   mach_port_t *portarray,
                   size_t portarraylen,
                   int *intarray,
                   size_t intarraylen,
                   mach_port_t *deallocnames,
                   size_t deallocnameslen,
                   mach_port_t *destroynames,
                   size_t destroynameslen)
{
  error_t err;

  if (!user)
    return EOPNOTSUPP;

  mutex_lock (&user->po->np->lock);
  err = file_exec (user->po->np->nn->file, task, flags, argv, argvlen,
		   envp, envplen, fds, MACH_MSG_TYPE_MOVE_SEND, fdslen,
		   portarray, MACH_MSG_TYPE_MOVE_SEND, portarraylen,
		   intarray, intarraylen, deallocnames, deallocnameslen,
		   destroynames, destroynameslen);
  mutex_unlock (&user->po->np->lock);
  return err;
}

error_t
netfs_S_io_map (struct protid *user,
		mach_port_t *rdobj, mach_msg_type_name_t *rdobjtype,
		mach_port_t *wrobj, mach_msg_type_name_t *wrobjtype)
{
  error_t err;

  if (!user)
    return EOPNOTSUPP;
  *rdobjtype = *wrobjtype = MACH_MSG_TYPE_MOVE_SEND;

  mutex_lock (&user->po->np->lock);
  err = io_map (user->po->np->nn->file, rdobj, wrobj);
  mutex_unlock (&user->po->np->lock);
  return err;
}

error_t
netfs_S_io_map_cntl (struct protid *user,
                     mach_port_t *obj,
                     mach_msg_type_name_t *objtype)
{
  error_t err;

  if (!user)
    return EOPNOTSUPP;
  *objtype = MACH_MSG_TYPE_MOVE_SEND;

  mutex_lock (&user->po->np->lock);
  err = io_map_cntl (user->po->np->nn->file, obj);
  mutex_unlock (&user->po->np->lock);
  return err;
}

#define NETFS_S_SIMPLE(name)			\
error_t						\
netfs_S_##name (struct protid *user)		\
{						\
  error_t err;					\
						\
  if (!user)					\
    return EOPNOTSUPP;				\
						\
  mutex_lock (&user->po->np->lock);		\
  err = name (user->po->np->nn->file);		\
  mutex_unlock (&user->po->np->lock);		\
  return err;					\
}

NETFS_S_SIMPLE (io_get_conch)
NETFS_S_SIMPLE (io_release_conch)
NETFS_S_SIMPLE (io_eofnotify)
NETFS_S_SIMPLE (io_readnotify)
NETFS_S_SIMPLE (io_readsleep)
NETFS_S_SIMPLE (io_sigio)

error_t
netfs_S_io_prenotify (struct protid *user,
                      vm_offset_t start, vm_offset_t stop)
{
  error_t err;

  if (!user)
    return EOPNOTSUPP;

  mutex_lock (&user->po->np->lock);
  err = io_prenotify (user->po->np->nn->file, start, stop);
  mutex_unlock (&user->po->np->lock);
  return err;
}

error_t
netfs_S_io_postnotify (struct protid *user,
                       vm_offset_t start, vm_offset_t stop)
{
  error_t err;

  if (!user)
    return EOPNOTSUPP;

  mutex_lock (&user->po->np->lock);
  err = io_postnotify (user->po->np->nn->file, start, stop);
  mutex_unlock (&user->po->np->lock);
  return err;
}

/* This overrides the library's definition.  */
int
netfs_demuxer (mach_msg_header_t *inp,
	       mach_msg_header_t *outp)
{
  int netfs_fs_server (mach_msg_header_t *, mach_msg_header_t *);
  int netfs_io_server (mach_msg_header_t *, mach_msg_header_t *);
  int netfs_fsys_server (mach_msg_header_t *, mach_msg_header_t *);
  int netfs_ifsock_server (mach_msg_header_t *, mach_msg_header_t *);

  if (netfs_io_server (inp, outp)
      || netfs_fs_server (inp, outp)
      || ports_notify_server (inp, outp)
      || netfs_fsys_server (inp, outp)
      /* XXX we should intercept interrupt_operation and do
	 the ports_S_interrupt_operation work as well as
	 sending an interrupt_operation to the underlying file.
       */
      || ports_interrupt_server (inp, outp))
    return 1;
  else
    {
      /* We didn't recognize the message ID, so pass the message through
	 unchanged to the underlying file.  */
      struct protid *cred = ports_lookup_port (netfs_port_bucket,
					       inp->msgh_local_port,
					       netfs_protid_class);
      if (cred == 0)
	/* This must be an unknown message on our fsys control port.  */
	return 0;
      else
	{
	  error_t err;
	  assert (MACH_MSGH_BITS_LOCAL (inp->msgh_bits)
		  == MACH_MSG_TYPE_MOVE_SEND);
	  inp->msgh_bits = (inp->msgh_bits & MACH_MSGH_BITS_COMPLEX)
	    | MACH_MSGH_BITS (MACH_MSG_TYPE_COPY_SEND,
			      MACH_MSGH_BITS_REMOTE (inp->msgh_bits));
	  inp->msgh_local_port = inp->msgh_remote_port;	/* reply port */
	  inp->msgh_remote_port = cred->po->np->nn->file;
	  err = mach_msg (inp, MACH_SEND_MSG, inp->msgh_size, 0,
			  MACH_PORT_NULL, MACH_MSG_TIMEOUT_NONE,
			  MACH_PORT_NULL);
	  assert_perror (err);	/* XXX should synthesize reply */
	  ports_port_deref (cred);
	  return 1;
	}
    }
}


int
main (int argc, char **argv)
{
  error_t err;
  mach_port_t bootstrap;

  struct argp argp = { NULL, NULL, NULL, "\
A translator for faking privileged access to an underlying filesystem.\v\
This translator appears to give transparent access to the underlying \
directory node.  However, all accesses are made using the credentials \
of the translator regardless of the client and the translator fakes \
success for chown and chmod operations that only root could actually do, \
reporting the faked IDs and modes in later stat calls, and allows \
any user to open nodes regardless of permissions as is done for root." };

  /* Parse our command line arguments (all none of them).  */
  argp_parse (&argp, argc, argv, ARGP_IN_ORDER, 0, 0);

  task_get_bootstrap_port (mach_task_self (), &bootstrap);
  netfs_init ();

  /* Get our underlying node (we presume it's a directory) and use
     that to make the root node of the filesystem.  */
  err = new_node (netfs_startup (bootstrap, O_READ), MACH_PORT_NULL, O_READ,
		  &netfs_root_node);
  if (err)
    error (5, err, "Cannot create root node");

  err = netfs_validate_stat (netfs_root_node, 0);
  if (err)
    error (6, err, "Cannot stat underlying node");

  netfs_root_node->nn_stat.st_mode &= ~(S_IPTRANS | S_IATRANS);
  netfs_root_node->nn_stat.st_mode |= S_IROOT;
  netfs_root_node->nn->faked |= FAKE_MODE;

  netfs_server_loop ();		/* Never returns.  */

  /*NOTREACHED*/
  return 0;
}
