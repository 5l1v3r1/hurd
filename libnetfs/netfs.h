/* 

   Copyright (C) 1994, 1995, 1996 Free Software Foundation

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

#ifndef _HURD_NETFS_H_
#define _HURD_NETFS_H_

#include <hurd/ports.h>
#include <hurd/fshelp.h>
#include <hurd/ioserver.h>
#include <assert.h>
#include <argp.h>

/* This library supports client-side network file system
   implementations.  It is analogous to the diskfs library provided for 
   disk-based filesystems.  */

struct protid
{
  struct port_info pi;
  
  /* User identification */
  struct netcred *credential;
  
  /* Object this refers to */
  struct peropen *po;
  
  /* Shared memory I/O information. */
  memory_object_t shared_object;
  struct shared_io *mapped;
};

/* One of these is created for each open */
struct peropen
{
  off_t filepointer;
  int lock_status;
  int refcnt;
  int openstat;
  mach_port_t dotdotport;
  struct node *np;
};

/* A unique one of these exists for each node currently in use. */
struct node
{
  struct node *next, **prevp;
  
  /* Protocol specific stuff. */
  struct netnode *nn;

  struct stat nn_stat;

  int istranslated;
  
  struct mutex lock;
  
  int references;
  
  mach_port_t sockaddr;
  
  int owner;
  
  struct transbox transbox;

  struct lock_box userlock;

  struct conch conch;

  struct dirmod *dirmod_reqs;
};

/* The user must define this function.  Make sure that NP->nn_stat is
   filled with current information.  CRED identifies the user
   responsible for the operation.  */
error_t netfs_validate_stat (struct node *NP, struct netcred *cred);

/* The user must define this function.  This should attempt a chmod
   call for the user specified by CRED on node NODE, to change the
   owner to UID and the group to GID. */
error_t netfs_attempt_chown (struct netcred *cred, struct node *np,
			     uid_t uid, uid_t gid);

/* The user must define this function.  This should attempt a chauthor
   call for the user specified by CRED on node NODE, to change the
   author to AUTHOR. */
error_t netfs_attempt_chauthor (struct netcred *cred, struct node *np,
				uid_t author);

/* The user must define this function.  This should attempt a chmod
   call for the user specified by CRED on node NODE, to change the
   mode to MODE. */
error_t netfs_attempt_chmod (struct netcred *cred, struct node *np,
			     mode_t mode);

/* The user must define this function.  This should attempt a chflags
   call for the user specified by CRED on node NODE, to change the
   flags to FLAGS. */
error_t netfs_attempt_chflags (struct netcred *cred, struct node *np,
			       int flags);

/* The user must define this function.  This should attempt a utimes
   call for the user specified by CRED on node NODE, to change the
   atime to ATIME and the mtime to MTIME. */
error_t netfs_attempt_utimes (struct netcred *cred, struct node *np,
			      struct timespec *atime, struct timespec *mtime);

/* The user must define this function.  This should attempt to set the
   size of the file NODE (for user CRED) to SIZE bytes long. */
error_t netfs_attempt_set_size (struct netcred *cred, struct node *np,
				off_t size);

/* The user must define this function.  This should attempt to fetch
   filesystem status information for the remote filesystem, for the
   user CRED. */
error_t netfs_attempt_statfs (struct netcred *cred, struct node *np,
			      struct fsys_statfsbuf *st);

/* The user must define this function.  This should sync the file NP
   completely to disk, for the user CRED.  If WAIT is set, return
   only after sync is completely finished.  */
error_t netfs_attempt_sync (struct netcred *cred, struct node *np,
			    int wait);

/* The user must define this function.  This should sync the entire
   remote filesystem.  If WAIT is set, return only after
   sync is completely finished.  */
error_t netfs_attempt_syncfs (struct netcred *cred, int wait);

/* The user must define this function.  Lookup NAME in DIR for USER;
   set *NP to the found name upon return.  If the name was not found,
   then return ENOENT.  On any error, clear *NP.  (*NP, if found, should
   be locked, this call should unlock DIR no matter what.) */
error_t netfs_attempt_lookup (struct netcred *user, struct node *dir, 
			      char *name, struct node **np);

/* The user must define this function.  Delete NAME in DIR for USER. */
error_t netfs_attempt_unlink (struct netcred *user, struct node *dir,
			      char *name);

/* Note that in this one call, neither of the specific nodes are locked. */
error_t netfs_attempt_rename (struct netcred *user, struct node *fromdir,
			      char *fromname, struct node *todir, 
			      char *toname);

/* The user must define this function.  Attempt to create a new
   directory named NAME in DIR for USER with mode MODE.  */
error_t netfs_attempt_mkdir (struct netcred *user, struct node *dir,
			     char *name, mode_t mode);

/* The user must define this function.  Attempt to remove directory
   named NAME in DIR for USER. */
error_t netfs_attempt_rmdir (struct netcred *user, 
			     struct node *dir, char *name);


/* The user must define this function.  Create a link in DIR with name
   NAME to FILE for USER.  Note that neither DIR nor FILE are
   locked. */
error_t netfs_attempt_link (struct netcred *user, struct node *dir,
			    struct node *file, char *name);

/* The user must define this function.  Attempt to create an anonymous
   file related to DIR for USER with MODE.  Set *NP to the returned
   file upon success.  No matter what, unlock DIR. */
error_t netfs_attempt_mkfile (struct netcred *user, struct node *dir,
			      mode_t mode, struct node **np);

/* The user must define this function.  Attempt to create a file named
   NAME in DIR for USER with MODE.  Set *NP to the new node upon
   return.  On any error, clear *NP.  *NP should be locked on success;
   no matter what, unlock DIR before returning.  */
error_t netfs_attempt_create_file (struct netcred *user, struct node *dir,
				   char *name, mode_t mode, struct node **np);

/* The user must define this function.  Read the contents of NP (a symlink),
   for USER, into BUF. */
error_t netfs_attempt_readlink (struct netcred *user, struct node *np,
				char *buf);

/* The user must define this function.  Node NP is being opened by USER,
   with FLAGS.  NEWNODE is nonzero if we just created this node.  Return
   an error if we should not permit the open to complete because of a
   permission restriction. */
error_t netfs_check_open_permissions (struct netcred *user, struct node *np,
				      int flags, int newnode);

/* The user must define this function.  Read from the file NP for user
   CRED starting at OFFSET and continuing for up to *LEN bytes.  Put
   the data at DATA.  Set *LEN to the amount successfully read upon
   return.  */
error_t netfs_attempt_read (struct netcred *cred, struct node *np,
			    off_t offset, size_t *len, void *data);

/* The user must define this function.  Write to the file NP for user
   CRED starting at OFSET and continuing for up to *LEN bytes from
   DATA.  Set *LEN to the amount seccessfully written upon return. */
error_t netfs_attempt_write (struct netcred *cred, struct node *np,
			     off_t offset, size_t *len, void *data);

/* The user must define this function.  Return the valid access types
   (bitwise OR of O_READ, O_WRITE, and O_EXEC) in *TYPES for file NP
   and user CRED.  */
void netfs_report_access (struct netcred *cred, struct node *np,
			  int *types);

/* The user must define this function.  Malloc and fill two arrays with
   the uids and gids from the specified credential. */
void netfs_interpret_credential (struct netcred *cred, uid_t **uids,
				 int *nuids, uid_t **gids, int *ngids);

/* The user must define this function.  Return a (virtual or physical)
   copy of CRED. */
struct netcred *netfs_copy_credential (struct netcred *cred);

/* The user must define this function.  The specified credential is 
   not in use any more.  */
void netfs_drop_credential (struct netcred *cred);

/* The user must define this function.  Create a new credential
   from the specified UID and GID arrays. */
struct netcred *netfs_make_credential (uid_t *uids, int nuids,
				       uid_t *gids, int ngids);

/* The user must define this function.  Node NP is all done; free
   all its associated storage. */
void netfs_node_norefs (struct node *np);

error_t netfs_get_dirents (struct netcred *, struct node *, int, int, char **,
			   mach_msg_type_number_t *, vm_size_t, int *);

/* The user may define this function, in which case it is called when the the
   filesystem receives a set-options request.  ARGC and ARGV are the
   arguments given, and STANDARD_ARGP is a pointer to a struct argp
   containing the info necessary to parse `standard' netfs runtime options.
   The user may chain this onto the end of his own argp structure and call
   argp_parse, or ignore it completely (or indeed, just call argp_parse on it
   -- which is the behavior of the default implementation of this function.
   EINVAL is returned if an unknown option is encountered.  */
error_t netfs_parse_runtime_options (int argc, char **argv,
				     const struct argp *standard_argp);

/* The user may define this function, in which case it is called when the the
   filesystem receives a get-options request.  ARGZ & ARGZ_LEN will contain
   information on `standard' netfs options; the user may extend them
   (probably by using argz_add), or ignore them, in which case case ARGZ
   should be freed, as it is malloced.  The default implementation simply
   leaves ARGZ & ARGZ_LEN unmodified and returns sucess (0).  */
error_t netfs_unparse_runtime_options (char **argz, size_t *argz_len);

/* Definitions provided by netfs. */
struct node *netfs_make_node (struct netnode *);

extern spin_lock_t netfs_node_refcnt_lock;

extern int netfs_maxsymlinks;

void netfs_init (void);
void netfs_server_loop (void);
struct protid *netfs_make_protid (struct peropen *, struct netcred *);
struct peropen *netfs_make_peropen (struct node *, int, mach_port_t);
void netfs_drop_node (struct node *);
void netfs_release_protid (void *);
void netfs_release_peropen (struct peropen *);
int netfs_demuxer (mach_msg_header_t *, mach_msg_header_t *);
error_t netfs_shutdown (int);

/* Parse and execute the runtime options in ARGC and ARGV.  EINVAL is
   returned if some option is unrecognized.  */
error_t netfs_set_options (int argc, char **argv);

/* Return an argz string describing the current options.  Fill *ARGZ
   with a pointer to newly malloced storage holding the list and *LEN
   to the length of that storage. */
error_t netfs_get_options (char **argz, unsigned *argz_len);

/* A pointer to an argp structure for the standard netfs command line
   arguments.  The user may call argp_parse on this to parse the command
   line, chain it onto the end of his own argp structure, or ignore it
   completely.  */
extern const struct argp *netfs_startup_argp;

struct port_class *netfs_protid_class;
struct port_class *netfs_control_class;
struct port_bucket *netfs_port_bucket;
struct node *netfs_root_node;

auth_t netfs_auth_server_port;

extern inline void
netfs_nref (struct node *np)
{
  spin_lock (&netfs_node_refcnt_lock);
  np->references++;
  spin_unlock (&netfs_node_refcnt_lock);
}
  
extern inline void
netfs_nrele (struct node *np)
{
  spin_lock (&netfs_node_refcnt_lock);
  assert (np->references);
  np->references--;
  if (np->references == 0)
    {
      mutex_lock (&np->lock);
      netfs_drop_node (np);
    }
  else
    spin_unlock (&netfs_node_refcnt_lock);
}

extern inline void
netfs_nput (struct node *np)
{
  spin_lock (&netfs_node_refcnt_lock);
  assert (np->references);
  np->references--;
  if (np->references == 0)
    netfs_drop_node (np);
  else
    {
      spin_unlock (&netfs_node_refcnt_lock);
      mutex_unlock (&np->lock);
    }
}



/* Mig gook. */
typedef struct protid *protid_t;


#endif /* _HURD_NETFS_H_ */
