/* 
   Copyright (C) 1995 Free Software Foundation, Inc.
   Written by Michael I. Bushnell, p/BSG.

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
   Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111, USA. */

#include <fcntl.h>
#include <assert.h>
#include <string.h>
#include <stdio.h>
#include <hurd/paths.h>
#include "netfs.h"
#include "fs_S.h"
#include "callbacks.h"
#include "misc.h"

/* XXX - Temporary hack; this belongs in a header file, probably types.h. */
#define major(x) ((int)(((unsigned) (x) >> 8) & 0xff))
#define minor(x) ((int)((x) & 0xff))

error_t
netfs_S_dir_lookup (struct protid *diruser,
		    char *filename,
		    int flags,
		    mode_t mode,
		    retry_type *do_retry,
		    char *retry_name,
		    mach_port_t *retry_port,
		    mach_msg_type_name_t *retry_port_type)
{
  int create;			/* true if O_CREAT flag set */
  int excl;			/* true if O_EXCL flag set */
  int mustbedir = 0;		/* true if the result must be S_IFDIR */
  int lastcomp = 0;		/* true if we are at the last component */
  int newnode = 0;		/* true if this node is newly created */
  int nsymlinks = 0;
  struct node *dnp, *np;
  char *nextname;
  error_t error;
  struct protid *newpi;

  if (!diruser)
    return EOPNOTSUPP;
  
  create = (flags & O_CREAT);
  excl = (flags & O_EXCL);
  
  /* Skip leading slashes */
  while (*filename == '/')
    filename++;
  
  *retry_port_type = MACH_MSG_TYPE_MAKE_SEND;
  *do_retry = FS_RETRY_NORMAL;
  *retry_name = '\0';
  
  if (*filename == '\0')
    {
      mustbedir = 1;
      
      /* Set things up in the state expected by the code from gotit: on. */
      dnp = 0;
      np = diruser->po->np;
      mutex_lock (&np->lock);
      netfs_nref (np);
      goto gotit;
    }
  
  dnp = diruser->po->np;
  mutex_lock (&dnp->lock);
  np = 0;
  
  netfs_nref (dnp);		/* acquire a reference for later netfs_nput */
  
  do
    {
      assert (!lastcomp);
      
      /* Find the name of the next pathname component */
      nextname = index (filename, '/');
      
      if (nextname)
	{
	  *nextname++ = '\0';
	  while (*nextname == '/')
	    nextname++;
	  if (*nextname == '\0')
	    {
	      /* These are the rules for filenames ending in /. */
	      nextname = 0;
	      lastcomp = 1;
	      mustbedir = 1;
	      create = 0;
	    }
	  else
	    lastcomp = 0;
	}
      else 
	lastcomp = 1;
      
      np = 0;
	  
      /* Attempt a lookup on the next pathname component. */
      error = netfs_attempt_lookup (diruser->credential, dnp, nextname, &np);

      /* Implement O_EXCL flag here */
      if (lastcomp && create && excl && (!error || error == EAGAIN))
	error = EEXIST;
	  
      /* If we get an error, we're done */
      if (error == EAGAIN)
	{
	  /* This really means .. from root */
	  if (diruser->po->dotdotport != MACH_PORT_NULL)
	    {
	      *do_retry = FS_RETRY_REAUTH;
	      *retry_port = diruser->po->dotdotport;
	      *retry_port_type = MACH_MSG_TYPE_COPY_SEND;
	      if (!lastcomp)
		strcpy (retry_name, nextname);
	      error = 0;
	      goto out;
	    }
	  else
	    {
	      /* We are the global root; .. from our root is
		 just our root again. */
	      error = 0;
	      np = dnp;
	      netfs_nref (np);
	    }
	}
	  
      /* Create the new node if necessary */
      if (lastcomp && create && error == ENOENT)
	{
	  mode &= ~(S_IFMT | S_ISPARE | S_ISVTX);
	  mode |= S_IFREG;
	  error = netfs_attempt_create_file (diruser->credential, dnp, 
					     filename, mode, &np);
	  newnode = 1;
	}
	     
      /* If this is translated, start the translator (if necessary)
	 and return. */
      if ((((flags & O_NOTRANS) == 0) || !lastcomp)
	  && (np->istranslated
	      || S_ISFIFO (np->nn_stat.st_mode)
	      || S_ISCHR (np->nn_stat.st_mode)
	      || S_ISBLK (np->nn_stat.st_mode)
	      || fshelp_translated (&np->transbox)))
	{
	  mach_port_t dirport;
	  uid_t *uids, *gids;
	  int nuids, ngids;
	  
	  /* A callback function for short-circuited translators.
	     S_ISLNK and S_IFSOCK are handled elsewhere. */
	  error_t short_circuited_callback1 (void *cookie1, void *cookie2,
					     uid_t *uid, gid_t *gid,
					     char **argz, int *argz_len)
	    {
	      struct node *np = cookie1;
	      error_t err;

	      err = netfs_validate_stat (np, diruser);
	      if (err)
		return err;

	      switch (np->nn_stat.st_mode & S_IFMT)
		{
		case S_IFCHR:
		case S_IFBLK:
		  asprintf (argz, "%s%c%d%c%d",
			    (S_ISCHR (np->nn_stat.st_mode) 
			     ? _HURD_CHRDEV : _HURD_BLKDEV),
			    0, major (np->nn_stat.st_rdev),
			    0, minor (np->nn_stat.st_rdev));
		  *argz_len = strlen (*argz) + 1;
		  *argz_len += strlen (*argz + *argz_len) + 1;
		  *argz_len += strlen (*argz + *argz_len) + 1;
		  break;
		case S_IFIFO:
		  asprintf (argz, "%s", _HURD_FIFO);
		  *argz_len = strlen (*argz) + 1;
		  break;
		default:
		  return ENOENT;
		}

	      *uid = np->nn_stat.st_uid;
	      *gid = np->nn_stat.st_gid;
		  
	      return 0;
	    }
	      
	  /* Create an unauthenticated port for DNP, and then
	     unlock it. */
	  newpi = 
	    netfs_make_protid (netfs_make_peropen (dnp, 0,
						   diruser->po->dotdotport),
			       0);
	  dirport = ports_get_right (newpi);
	  mach_port_insert_right (mach_task_self (), dirport, dirport,
				  MACH_MSG_TYPE_MAKE_SEND);
	  ports_port_deref (newpi);
	  if (np != dnp)
	    mutex_unlock (&dnp->lock);
	  
	  netfs_interpret_credential (diruser->credential, &uids, &nuids,
				      &gids, &ngids);
	  error = fshelp_fetch_root (&np->transbox, &diruser->po->dotdotport,
				     dirport, uids, nuids, gids, ngids,
				     lastcomp ? flags : 0,
				     (np->istranslated
				      ? _netfs_translator_callback1
				      : short_circuited_callback1),
				     _netfs_translator_callback2,
				     do_retry, retry_name, retry_port);
	  free (uids);
	  free (gids);
	  if (error != ENOENT)
	    {
	      netfs_nrele (dnp);
	      netfs_nput (np);
	      *retry_port_type = MACH_MSG_TYPE_MOVE_SEND;
	      if (!lastcomp && !error)
		{
		  strcat (retry_name, "/");
		  strcat (retry_name, nextname);
		}
	      return error;
	    }
	  
	  /* ENOENT means there was a hiccup, and the translator vanished
	     while NP was unlocked inside fshelp_fetch_root.
	     Reacquire the locks and continue as normal. */
	  error = 0;
	  if (np != dnp)
	    {
	      if (!strcmp (filename, ".."))
		mutex_lock (&dnp->lock);
	      else
		{
		  mutex_unlock (&np->lock);
		  mutex_lock (&dnp->lock);
		  mutex_lock (&np->lock);
		}
	    }
	}
      
      netfs_validate_stat (np, diruser);
      
      if (S_ISLNK (np->nn_stat.st_mode)
	  && !(lastcomp && (flags & (O_NOLINK|O_NOTRANS))))
	{
	  size_t nextnamelen, newnamelen, linklen;
	  char *linkbuf;
	  
	  /* Handle symlink interpretation */
	  if (nsymlinks++ > netfs_maxsymlinks)
	    {
	      error = ELOOP;
	      goto out;
	    }

	  linklen = np->nn_stat.st_size;
	  
	  nextnamelen = nextname ? strlen (nextname) + 1 : 0;
	  newnamelen = nextnamelen + linklen + 1;
	  linkbuf = alloca (newnamelen);
	  
	  error = netfs_attempt_readlink (diruser->credential, np, linkbuf);
	  if (error)
	    goto out;
	  
	  if (nextname)
	    {
	      linkbuf[linklen] = '/';
	      bcopy (nextname, linkbuf + linklen + 1,
		     nextnamelen - 1);
	    }
	  linkbuf[nextnamelen + linklen] = '\0';
	  
	  if (linkbuf[0] == '/')
	    {
	      /* Punt to the caller */
	      *do_retry = FS_RETRY_MAGICAL;
	      *retry_port = MACH_PORT_NULL;
	      strcpy (retry_name, linkbuf);
	      goto out;
	    }
	  
	  filename = linkbuf;
	  if (lastcomp)
	    {
	      lastcomp = 0;

	      /* Symlinks to nonexistent files aren't allowed to cause
		 creation, so clear the flag here. */
	      create = 0;
	    }
	  netfs_nput (np);
	  np = 0;
	}
      else
	{
	  /* Normal nodes here */
	  filename = nextname;
	  if (np == dnp)
	    netfs_nrele (dnp);
	  else
	    netfs_nput (dnp);
	  
	  if (!lastcomp)
	    {
	      dnp = np;
	      np = 0;
	    }
	  else
	    dnp = 0;
	}
    }
  while (filename && *filename);

  /* At this point, NP is the node to return.  */
 gotit:

  if (mustbedir)
    {
      netfs_validate_stat (np, diruser);
      if (!S_ISDIR (np->nn_stat.st_mode))
	{
	  error = ENOTDIR;
	  goto out;
	}
    }      
  error = netfs_check_open_permissions (diruser->credential, np,
					flags, newnode);
  if (error)
    goto out;
  
  flags &= ~OPENONLY_STATE_MODES;
  
  newpi = netfs_make_protid (netfs_make_peropen (np, flags,
						 diruser->po->dotdotport),
			     diruser->credential);
  *retry_port = ports_get_right (newpi);
  ports_port_deref (newpi);
  
 out:
  if (np)
    {
      if (dnp == np)
	netfs_nrele (np);
      else
	netfs_nput (np);
    }
  if (dnp)
    netfs_nput (dnp);
  return error;
}

	    
