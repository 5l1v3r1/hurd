/*
   Copyright (C) 1993, 1994, 1995, 1996 Free Software Foundation

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

/* Written by Michael I. Bushnell.  */

#include "priv.h"
#include "fsys_S.h"
#include <hurd/fsys.h>
#include <fcntl.h>

/* Implement fsys_getroot as described in <hurd/fsys.defs>. */
kern_return_t
diskfs_S_fsys_getroot (fsys_t controlport,
		       mach_port_t reply,
		       mach_msg_type_name_t replytype,
		       mach_port_t dotdot,
		       uid_t *uids,
		       u_int nuids,
		       uid_t *gids,
		       u_int ngids,
		       int flags,
		       retry_type *retry,
		       char *retryname,
		       file_t *returned_port,
		       mach_msg_type_name_t *returned_port_poly)
{
  struct port_info *pt = ports_lookup_port (diskfs_port_bucket, controlport, 
					    diskfs_control_class);
  error_t error = 0;
  mode_t type;
  struct protid pseudocred;
  struct protid *newpi;
  
  if (!pt)
    return EOPNOTSUPP;

  flags &= O_HURD;

  rwlock_reader_lock (&diskfs_fsys_lock);
  mutex_lock (&diskfs_root_node->lock);
    
  /* This code is similar (but not the same as) the code in
     dir-pathtrans.c that does the same thing.  Perhaps a way should
     be found to share the logic.  */

  type = diskfs_root_node->dn_stat.st_mode & S_IFMT;

  if ((diskfs_root_node->istranslated
       || fshelp_translated (&diskfs_root_node->transbox))
      && !(flags & O_NOTRANS))
    {
      error = fshelp_fetch_root (&diskfs_root_node->transbox,
				 &dotdot, dotdot, uids, nuids,
				 gids, ngids, flags, 
				 _diskfs_translator_callback1,
				 _diskfs_translator_callback2,
				 retry, retryname, returned_port);
      if (error != ENOENT)
	{
	  mutex_unlock (&diskfs_root_node->lock);
	  rwlock_reader_unlock (&diskfs_fsys_lock);
	  if (!error)
	    *returned_port_poly = MACH_MSG_TYPE_MOVE_SEND;
	  return error;
	}
      
      /* ENOENT means the translator was removed in the interim. */
      error = 0;
    }
  
  if (type == S_IFLNK && !(flags & (O_NOLINK | O_NOTRANS)))
    {
      /* Handle symlink interpretation */
      char pathbuf[diskfs_root_node->dn_stat.st_size + 1];
      int amt;
      
      if (diskfs_read_symlink_hook)
	error = (*diskfs_read_symlink_hook) (diskfs_root_node, pathbuf);
      if (!diskfs_read_symlink_hook || error == EINVAL)
	error = diskfs_node_rdwr (diskfs_root_node, pathbuf, 0,
				  diskfs_root_node->dn_stat.st_size, 0,
				  0, &amt);
      pathbuf[amt] = '\0';

      mutex_unlock (&diskfs_root_node->lock);
      rwlock_reader_unlock (&diskfs_fsys_lock);
      if (error)
	return error;
      
      if (pathbuf[0] == '/')
	{
	  *retry = FS_RETRY_MAGICAL;
	  *returned_port = MACH_PORT_NULL;
	  *returned_port_poly = MACH_MSG_TYPE_COPY_SEND;
	  strcpy (retryname, pathbuf);
	  mach_port_deallocate (mach_task_self (), dotdot);
	  return 0;
	}
      else
	{
	  *retry = FS_RETRY_REAUTH;
	  *returned_port = dotdot;
	  *returned_port_poly = MACH_MSG_TYPE_MOVE_SEND;
	  strcpy (retryname, pathbuf);
	  return 0;
	}
    }

  if ((type == S_IFSOCK || type == S_IFBLK 
       || type == S_IFCHR || type == S_IFIFO)
      && (flags & (O_READ|O_WRITE|O_EXEC)))
    error = EOPNOTSUPP;
  
  /* diskfs_access requires a cred; so we give it one. */
  pseudocred.uids = uids;
  pseudocred.gids = gids;
  pseudocred.nuids = nuids;
  pseudocred.ngids = ngids;
      
  if (!error && (flags & O_READ))
    error = diskfs_access (diskfs_root_node, S_IREAD, &pseudocred);
  
  if (!error && (flags & O_EXEC))
    error = diskfs_access (diskfs_root_node, S_IEXEC, &pseudocred);
  
  if (!error && (flags & (O_WRITE)))
    {
      if (type == S_IFDIR)
	error = EISDIR;
      else if (diskfs_readonly)
	error = EROFS;
      else 
	error = diskfs_access (diskfs_root_node, S_IWRITE, &pseudocred);
    }

  if (error)
    {
      mutex_unlock (&diskfs_root_node->lock);
      rwlock_reader_unlock (&diskfs_fsys_lock);
      return error;
    }
  
  if ((flags & O_NOATIME)
      && (diskfs_isowner (diskfs_root_node, &pseudocred) == EPERM))
    flags &= ~O_NOATIME;

  flags &= ~OPENONLY_STATE_MODES;

  error = diskfs_create_protid (diskfs_make_peropen (diskfs_root_node,
						     flags, dotdot),
				uids, nuids, gids, ngids, &newpi);
  mach_port_deallocate (mach_task_self (), dotdot);
  if (! error)
    {
      *retry = FS_RETRY_NORMAL;
      *retryname = '\0';
      *returned_port = ports_get_right (newpi);
      *returned_port_poly = MACH_MSG_TYPE_MAKE_SEND;
      ports_port_deref (newpi);
    }
  
  mutex_unlock (&diskfs_root_node->lock);
  rwlock_reader_unlock (&diskfs_fsys_lock);

  ports_port_deref (pt);

  return error;
}
