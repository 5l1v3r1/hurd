/* console.c -- A console server.
   Copyright (C) 1997, 1999, 2002 Free Software Foundation, Inc.
   Written by Miles Bader and Marcus Brinkmann.

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
#include <stddef.h>
#include <unistd.h>

#include <argp.h>
#include <error.h>
#include <string.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <cthreads.h>
#include <mcheck.h>

#include <version.h>

#include "console.h"
#include "display.h"
#include "input.h"

const char *argp_program_version = STANDARD_HURD_VERSION (console);

char *netfs_server_name = "console";
char *netfs_server_version = HURD_VERSION;
int netfs_maxsymlinks = 16;	/* Arbitrary.  */


struct vcons
{
  /* Protected by cons->lock.  */
  vcons_t next;
  vcons_t prev;
  /* We acquire one reference per netnode.  */
  int refcnt;

  /* The following members remain constant over the lifetime of the
     object and accesses don't need to be locked.  */
  int id;
  char *name;
  cons_t cons;
  display_t display;
  input_t input;

  struct mutex lock;
  /* Nodes in the filesystem referring to this virtual console.  */
  struct node *dir_node;
  struct node *cons_node;
  struct node *disp_node;
  struct node *inpt_node;
};

struct cons
{
  /* The lock protects the console, all virtual consoles contained in
     it and the reference counters.  It also locks the
     configuration.  */
  struct mutex lock;
  vcons_t vcons_list;
  /* The encoding.  */
  const char *encoding;

  struct node *node;
  mach_port_t underlying;
  /* A template for the stat information of all nodes.  */
  struct stat stat_template;
} mycons;
cons_t cons = &mycons;

/* Lookup the virtual console with number ID in the console CONS,
   acquire a reference for it, and return it in R_VCONS.  If CREATE is
   true, the virtual console will be created if it doesn't exist yet.
   If CREATE is true, and ID 0, the first free virtual console id is
   used.  */
error_t
vcons_lookup (cons_t cons, int id, int create, vcons_t *r_vcons)
{
  error_t err;
  vcons_t previous_vcons = 0;
  vcons_t vcons;

  if (!id && !create)
    return EINVAL;

  mutex_lock (&cons->lock);
  if (id)
    {
      if (cons->vcons_list && cons->vcons_list->id <= id)
        {
          previous_vcons = cons->vcons_list;
          while (previous_vcons->next && previous_vcons->next->id <= id)
            previous_vcons = previous_vcons->next;
          if (previous_vcons->id == id)
            {
              previous_vcons->refcnt++;
	      mutex_unlock (&cons->lock);
              *r_vcons = previous_vcons;
              return 0;
            }
	}
      else if (!create)
	{
	  mutex_unlock (&cons->lock);
	  return ESRCH;
	}
    }
  else
    {
      id = 1;
      if (cons->vcons_list && cons->vcons_list->id == 1)
        {
          previous_vcons = cons->vcons_list;
          while (previous_vcons && previous_vcons->id == id)
            {
              id++;
              previous_vcons = previous_vcons->next;
            }
	}
    }

  vcons = calloc (1, sizeof (struct vcons));
  if (!vcons)
    {
      mutex_unlock (&vcons->cons->lock);
      return ENOMEM;
    }
  vcons->cons = cons;
  vcons->refcnt = 1;
  vcons->id = id;
  asprintf (&vcons->name, "%i", id);
  /* XXX Error checking.  */

  mutex_init (&vcons->lock);
  err = display_create (&vcons->display, cons->encoding);
  if (err)
    {
      free (vcons->name);
      free (vcons);
      mutex_unlock (&vcons->cons->lock);
      return err;
    }

  err = input_create (&vcons->input, cons->encoding);
  if (err)
    {
      display_destroy (vcons->display);
      free (vcons->name);
      free (vcons);
      mutex_unlock (&vcons->cons->lock);
      return err;
    }
  
  /* Insert the virtual console into the doubly linked list.  */
  if (previous_vcons)
    {
      vcons->prev = previous_vcons;
      if (previous_vcons->next)
        {
          previous_vcons->next->prev = vcons;
          vcons->next =  previous_vcons->next;
        }
      previous_vcons->next = vcons;
    }
  else
    {
      if (cons->vcons_list)
	{
	  cons->vcons_list->prev = vcons;
	  vcons->next = cons->vcons_list;
	}
      cons->vcons_list = vcons;
    }
  mutex_unlock (&cons->lock);
  *r_vcons = vcons;
  return 0;
}

/* Acquire an additional reference to the virtual console VCONS.  */
void
vcons_ref (vcons_t vcons)
{
  cons_t cons = vcons->cons;

  mutex_lock (&cons->lock);
  vcons->refcnt++;
  mutex_unlock (&cons->lock);
}

/* Release a reference to the virtual console VCONS.  If this was the
   last reference the virtual console is destroyed.  */
void
vcons_release (vcons_t vcons)
{
  cons_t cons = vcons->cons;

  mutex_lock (&cons->lock);
  if (!--vcons->refcnt)
    {
      /* As we keep a reference for all input focus groups pointing to
         the virtual console, and a reference for the active console,
         we know that without references, this virtual console is
         neither active nor used by any input group.  */

      if (vcons->prev)
        vcons->prev->next = vcons->next;
      else
	cons->vcons_list = vcons->next;
      if (vcons->next)
        vcons->next->prev = vcons->prev;

      /* XXX Destroy the state.  */
      display_destroy (vcons->display);
      input_destroy (vcons->input);
      free (vcons->name);
      free (vcons);
    }
  mutex_unlock (&cons->lock);
}

struct netnode
{
  /* The root node points to the console object.  */
  cons_t cons;

  /* All other nodes point to the virtual console object.  */
  vcons_t vcons;
};

typedef enum
  {
    VCONS_NODE_DIR = 0,
    VCONS_NODE_CONSOLE,
    VCONS_NODE_DISPLAY,
    VCONS_NODE_INPUT
  } vcons_node_type;

/* Make a new virtual node.  Always consumes the ports.  */
static error_t
new_node (struct node **np, vcons_t vcons, vcons_node_type type)
{
  struct netnode *nn = calloc (1, sizeof *nn);
  if (nn == 0)
    return ENOMEM;

  nn->vcons = vcons;
  *np = netfs_make_node (nn);
  if (*np == 0)
    {
      free (nn);
      return ENOMEM;
    }
  (*np)->nn_stat = cons->stat_template;
  (*np)->nn_translated = 0;

  switch (type)
    {
    case VCONS_NODE_DIR:
      (*np)->nn_stat.st_ino = vcons->id << 2;
      (*np)->nn_stat.st_mode |= S_IFDIR;
      (*np)->nn_stat.st_size = 0;
      break;
    case VCONS_NODE_CONSOLE:
      (*np)->nn_stat.st_ino = (vcons->id << 2) + 1;
      (*np)->nn_stat.st_mode |= S_IFCHR;	/* Don't set nn_translated! */
      (*np)->nn_stat.st_mode &= ~(S_IXUSR | S_IXGRP | S_IXOTH);
      (*np)->nn_stat.st_size = 0;
      break;
    case VCONS_NODE_DISPLAY:
      (*np)->nn_stat.st_ino = (vcons->id << 2) + 2;
      (*np)->nn_stat.st_mode |= S_IFREG;
      (*np)->nn_stat.st_mode &= ~(S_IXUSR | S_IXGRP | S_IXOTH);
      (*np)->nn_stat.st_size = 2000 * sizeof (wchar_t); /* XXX */
      break;
    case VCONS_NODE_INPUT:
      (*np)->nn_stat.st_ino = (vcons->id << 2) + 3;
      (*np)->nn_stat.st_mode |= S_IFIFO;
      (*np)->nn_stat.st_mode &= ~(S_IXUSR | S_IXGRP | S_IXOTH);
      (*np)->nn_stat.st_size = 0;
      break;
    }

  /* If the underlying node isn't a directory, propagate read permission to
     execute permission since we need that for lookups.  */
  if (! S_ISDIR (cons->stat_template.st_mode)
      && S_ISDIR ((*np)->nn_stat.st_mode))
    {
      if (cons->stat_template.st_mode & S_IRUSR)
	(*np)->nn_stat.st_mode |= S_IXUSR;
      if (cons->stat_template.st_mode & S_IRGRP)
	(*np)->nn_stat.st_mode |= S_IXGRP;
      if (cons->stat_template.st_mode & S_IROTH)
	(*np)->nn_stat.st_mode |= S_IXOTH;
    }

  fshelp_touch (&(*np)->nn_stat, TOUCH_ATIME|TOUCH_MTIME|TOUCH_CTIME,
                console_maptime);

  return 0;
}


/* Node management.  */

/* Node NP has no more references; free all its associated
   storage.  */
void
netfs_node_norefs (struct node *np)
{
  vcons_t vcons = np->nn->vcons;

  /* The root node does never go away.  */
  assert (!np->nn->cons && np->nn->vcons);

  /* Avoid deadlock.  */
  spin_unlock (&netfs_node_refcnt_lock);

  /* Find the back reference to ourself in the virtual console
     structure, and delete it.  */
  mutex_lock (&vcons->lock);
  spin_lock (&netfs_node_refcnt_lock);
  if (np->references)
    {
      /* Someone else got a reference while we were attempting to go
	 away.  This can happen in netfs_attempt_lookup.  In this
	 case, just unlock the node and do nothing else.  */
      mutex_unlock (&vcons->lock);
      mutex_unlock (&np->lock);
      return;
    }
  if (np == vcons->dir_node)
    vcons->dir_node = 0;
  else if (np == vcons->cons_node)
    vcons->cons_node = 0;
  else if (np == vcons->disp_node)
    vcons->disp_node = 0;
  else
    {
      assert (np == vcons->inpt_node);
      vcons->inpt_node = 0;
    }
  mutex_unlock (&vcons->lock);

  /* Release our reference.  */
  vcons_release (vcons);

  free (np->nn);
  free (np);
}

/* Attempt to create a file named NAME in DIR for USER with MODE.  Set
   *NODE to the new node upon return.  On any error, clear *NODE.
   *NODE should be locked on success; no matter what, unlock DIR
   before returning.  */
error_t
netfs_attempt_create_file (struct iouser *user, struct node *dir,
			   char *name, mode_t mode, struct node **np)
{
  /* We create virtual consoles dynamically on the fly, so there is no
     need for an explicit create operation.  */
  *np = 0;
  mutex_unlock (&dir->lock);
  return EOPNOTSUPP;
}

/* Node NODE is being opened by USER, with FLAGS.  NEWNODE is nonzero
   if we just created this node.  Return an error if we should not
   permit the open to complete because of a permission
   restriction.  */
error_t
netfs_check_open_permissions (struct iouser *user, struct node *node,
			      int flags, int newnode)
{
  error_t err = 0;
  if (flags & O_READ)
    err = fshelp_access (&node->nn_stat, S_IREAD, user);
  if (!err && (flags & O_WRITE))
    err = fshelp_access (&node->nn_stat, S_IWRITE, user);
  if (!err && (flags & O_EXEC))
    err = fshelp_access (&node->nn_stat, S_IEXEC, user);
  return err;
}

/* This should attempt a utimes call for the user specified by CRED on node
   NODE, to change the atime to ATIME and the mtime to MTIME. */
error_t
netfs_attempt_utimes (struct iouser *cred, struct node *node,
		      struct timespec *atime, struct timespec *mtime)
{
  error_t err = fshelp_isowner (&node->nn_stat, cred);
  int flags = TOUCH_CTIME;
  
  if (! err)
    {
      if (mtime)
	{
	  node->nn_stat.st_mtime = mtime->tv_sec;
	  node->nn_stat.st_mtime_usec = mtime->tv_nsec / 1000;
	}
      else
	flags |= TOUCH_MTIME;
      
      if (atime)
	{
	  node->nn_stat.st_atime = atime->tv_sec;
	  node->nn_stat.st_atime_usec = atime->tv_nsec / 1000;
	}
      else
	flags |= TOUCH_ATIME;
      
      fshelp_touch (&node->nn_stat, flags, console_maptime);
    }
  return err;
}

/* Return the valid access types (bitwise OR of O_READ, O_WRITE, and O_EXEC)
   in *TYPES for file NODE and user CRED.  */
error_t
netfs_report_access (struct iouser *cred, struct node *node, int *types)
{
  *types = 0;
  if (fshelp_access (&node->nn_stat, S_IREAD, cred) == 0)
    *types |= O_READ;
  if (fshelp_access (&node->nn_stat, S_IWRITE, cred) == 0)
    *types |= O_WRITE;
  if (fshelp_access (&node->nn_stat, S_IEXEC, cred) == 0)
    *types |= O_EXEC;
  return 0;
}

/* Make sure that NP->nn_stat is filled with the most current
   information.  CRED identifies the user responsible for the
   operation. NP is locked.  */
error_t
netfs_validate_stat (struct node *np, struct iouser *cred)
{
  /* We are always uptodate.  */
  return 0;
}

/* This should sync the file NODE completely to disk, for the user
   CRED.  If WAIT is set, return only after sync is completely
   finished.  */
error_t
netfs_attempt_sync (struct iouser *cred, struct node *np, int wait)
{
  return 0;
}


/* Directory management.  */

/* Lookup NAME in DIR for USER; set *NODE to the found name upon
   return.  If the name was not found, then return ENOENT.  On any
   error, clear *NODE.  (*NODE, if found, should be locked, this call
   should unlock DIR no matter what.) */
error_t
netfs_attempt_lookup (struct iouser *user, struct node *dir,
		      char *name, struct node **node)
{
  error_t err;

  *node = 0;
  err = fshelp_access (&dir->nn_stat, S_IEXEC, user);
  if (err)
    goto out;

  if (strcmp (name, ".") == 0)
    {
      /* Current directory -- just add an additional reference to DIR
	 and return it.  */
      netfs_nref (dir);
      *node = dir;
      goto out;
    }

  if (strcmp (name, "..") == 0)
    {
      /* Parent directory -- if this is the root directory, return
	 EAGAIN.  Otherwise return the root node, because we know
	 that our hierarchy is only one level deep.  */

      if (dir->nn->cons)
	err = EAGAIN;
      else
	{
	  netfs_nref (netfs_root_node);
	  *node = netfs_root_node;
	}
      goto out;
    }

  if (dir->nn->cons)
    {
      /* This is the root directory.  Look up the desired virtual
	 console, creating it on the fly if necessary.  */
      vcons_t vcons;
      int release_vcons = 0;
      char *tail = NULL;
      int id;
      errno = 0;
      id = strtol (name, &tail, 0);
      if ((tail && *tail) || errno)
	{
	  err = ENOENT;
	  goto out;
	}
      err = vcons_lookup (dir->nn->cons, id, 1, &vcons);
      if (err == ESRCH || err == EINVAL)
	err = ENOENT;
      if (err)
	goto out;

      mutex_lock (&vcons->lock);
      if (vcons->dir_node)
	{
	  /* We already have a directory node for this virtual
	     console.  Use that, acquire a reference for it, and drop
	     our extra reference to the virtual console.  */
	  *node = vcons->dir_node;
	  netfs_nref (*node);
	  release_vcons = 1;
	}
      else
	{
	  /* Create a new directory node, connsuming the reference to
	     the virtual console.  */
	  err = new_node (node, vcons, VCONS_NODE_DIR);
	  if (!err)
	    vcons->dir_node = *node;
	  else
	    release_vcons = 1;
	}
      mutex_unlock (&vcons->lock);
      if (release_vcons)
	vcons_release (vcons);
    }
  else
    {
      /* This is a virtual console directory node.  */
      vcons_t vcons = dir->nn->vcons;
      int ref_vcons = 0;
      assert (dir == vcons->dir_node);

      if (!strcmp (name, "console"))
	{
	  mutex_lock (&vcons->lock);
	  if (vcons->cons_node)
	    {
	      *node = vcons->cons_node;
	      netfs_nref (*node);
	    }
	  else
	    {
	      err = new_node (node, vcons, VCONS_NODE_CONSOLE);
	      if (!err)
		{
		  vcons->cons_node = *node;
		  ref_vcons = 1;
		}
	    }
	  mutex_unlock (&vcons->lock);
	}
      else if (!strcmp (name, "display"))
	{
	  mutex_lock (&vcons->lock);
	  if (vcons->disp_node)
	    {
	      *node = vcons->disp_node;
	      netfs_nref (*node);
	    }
	  else
	    {
	      err = new_node (node, vcons, VCONS_NODE_DISPLAY);
	      if (!err)
		{
		  vcons->disp_node = *node;
		  ref_vcons = 1;
		}
	    }
	  mutex_unlock (&vcons->lock);
	}
      else if (!strcmp (name, "input"))
	{
	  mutex_lock (&vcons->lock);
	  if (vcons->inpt_node)
	    {
	      *node = vcons->inpt_node;
	      netfs_nref (*node);
	    }
	  else
	    {
	      err = new_node (node, vcons, VCONS_NODE_INPUT);
	      if (!err)
		{
		  vcons->inpt_node = *node;
		  ref_vcons = 1;
		}
	    }
	  mutex_unlock (&vcons->lock);
	}
      else
	err = ENOENT;

      if (ref_vcons)
	vcons_ref (vcons);
    }
  
  if (!err)
    fshelp_touch (&dir->nn_stat, TOUCH_ATIME, console_maptime);

 out:
  mutex_unlock (&dir->lock);
  if (err)
    *node = 0;
  else
    mutex_lock (&(*node)->lock);

  return err;
}

/* Returned directory entries are aligned to blocks this many bytes long.
   Must be a power of two.  */
#define DIRENT_ALIGN 4
#define DIRENT_NAME_OFFS offsetof (struct dirent, d_name)
/* Length is structure before the name + the name + '\0', all
   padded to a four-byte alignment.  */
#define DIRENT_LEN(name_len)                                                  \
  ((DIRENT_NAME_OFFS + (name_len) + 1 + (DIRENT_ALIGN - 1))                   \
   & ~(DIRENT_ALIGN - 1))

/* Implement the netfs_get_dirents callback as described in
   <hurd/netfs.h>. */
error_t
netfs_get_dirents (struct iouser *cred, struct node *dir,
		   int first_entry, int num_entries, char **data,
		   mach_msg_type_number_t *data_len,
		   vm_size_t max_data_len, int *data_entries)
{
  error_t err;
  int count = 0;
  size_t size = 0;		/* Total size of our return block.  */
  struct vcons *first_vcons, *vcons;

  /* Add the length of a directory entry for NAME to SIZE and return true,
     unless it would overflow MAX_DATA_LEN or NUM_ENTRIES, in which case
     return false.  */
  int bump_size (const char *name)
    {
      if (num_entries == -1 || count < num_entries)
	{
	  size_t new_size = size + DIRENT_LEN (strlen (name));
	  if (max_data_len > 0 && new_size > max_data_len)
	    return 0;
	  size = new_size;
	  count++;
	  return 1;
	}
      else
	return 0;
    }

  if (!dir->nn->cons && !(dir == dir->nn->vcons->dir_node))
    return ENOTDIR;

  if (dir->nn->cons)
    {
      mutex_lock (&dir->nn->cons->lock);

      /* Find the first entry.  */
      for (first_vcons = dir->nn->cons->vcons_list, count = 2;
	   first_vcons && first_entry > count;
	   first_vcons = first_vcons->next)
	count++;

      count = 0;
    }
      
  /* Make space for the `.' and `..' entries.  */
  if (first_entry == 0)
    bump_size (".");
  if (first_entry <= 1)
    bump_size ("..");

  if (dir->nn->cons)
    {
      /* See how much space we need for the result.  */
      for (vcons = first_vcons; vcons; vcons = vcons->next)
	if (!bump_size (vcons->name))
	  break;
    }
  else
    {
      if (first_entry <= 2)
	bump_size ("console");
      if (first_entry <= 3)
	bump_size ("display");
      if (first_entry <= 4)
	bump_size ("input");
    }

  /* Allocate it.  */
  *data = mmap (0, size, PROT_READ|PROT_WRITE, MAP_ANON, 0, 0);
  err = ((void *) *data == (void *) -1) ? errno : 0;

  if (! err)
    /* Copy out the result.  */
    {
      char *p = *data;

      int add_dir_entry (const char *name, ino_t fileno, int type)
	{
	  if (num_entries == -1 || count < num_entries)
	    {
	      struct dirent hdr;
	      size_t name_len = strlen (name);
	      size_t sz = DIRENT_LEN (name_len);

	      if (sz > size)
		return 0;
	      else
		size -= sz;

	      hdr.d_fileno = fileno;
	      hdr.d_reclen = sz;
	      hdr.d_type = type;
	      hdr.d_namlen = name_len;

	      memcpy (p, &hdr, DIRENT_NAME_OFFS);
	      strcpy (p + DIRENT_NAME_OFFS, name);
	      p += sz;

	      count++;

	      return 1;
	    }
	  else
	    return 0;
	}

      *data_len = size;
      *data_entries = count;

      count = 0;

      if (dir->nn->cons)
	{
	  /* Add `.' and `..' entries.  */
	  if (first_entry == 0)
	    add_dir_entry (".", 2, DT_DIR);
	  if (first_entry <= 1)
	    add_dir_entry ("..", 2, DT_DIR);

	  /* Fill in the real directory entries.  */
	  for (vcons = first_vcons; vcons; vcons = vcons->next)
	    if (!add_dir_entry (vcons->name,
				vcons->id << 2, DT_DIR))
	      break;
	  mutex_unlock (&dir->nn->cons->lock);
	}
      else
	{
	  /* Add `.' and `..' entries.  */
	  if (first_entry == 0)
	    add_dir_entry (".", dir->nn_stat.st_ino, DT_DIR);
	  if (first_entry <= 1)
	    add_dir_entry ("..", 2, DT_DIR);

	  if (first_entry <= 2)
	    add_dir_entry ("console", (dir->nn->vcons->id << 2) + 1, DT_REG);
	  if (first_entry <= 3)
	    add_dir_entry ("display", (dir->nn->vcons->id << 2) + 2, DT_REG);
	  if (first_entry <= 4)
	    add_dir_entry ("input", (dir->nn->vcons->id << 3) + 2, DT_FIFO);
	}	  
    }
      
  fshelp_touch (&dir->nn_stat, TOUCH_ATIME, console_maptime);
  return err;
}

/* This should sync the entire remote filesystem.  If WAIT is set, return
   only after sync is completely finished.  */
error_t
netfs_attempt_syncfs (struct iouser *cred, int wait)
{
  return 0;
}

/* This should attempt a chmod call for the user specified by CRED on node
   NODE, to change the owner to UID and the group to GID. */
error_t
netfs_attempt_chown (struct iouser *cred, struct node *node,
		     uid_t uid, uid_t gid)
{
  cons_t cons = node->nn->cons;
  vcons_t vcons;
  error_t err;

  if (!cons)
    return EOPNOTSUPP;

  err = file_chown (cons->underlying, uid, gid);
  if (err)
    return err;

  /* Change NODE's owner.  */
  node->nn_stat.st_uid = uid;
  node->nn_stat.st_gid = gid;

  mutex_lock (&cons->lock);
  cons->stat_template.st_uid = uid;
  cons->stat_template.st_gid = gid;

  /* Change the owner of each leaf node.  */
  for (vcons = cons->vcons_list; vcons; vcons = vcons->next)
    {
      if (vcons->dir_node)
	{
	  vcons->dir_node->nn_stat.st_uid = uid;
	  vcons->dir_node->nn_stat.st_gid = gid;
	}
      if (vcons->cons_node)
	{
	  vcons->cons_node->nn_stat.st_uid = uid;
	  vcons->cons_node->nn_stat.st_gid = gid;
	}
      if (vcons->disp_node)
	{
	  vcons->disp_node->nn_stat.st_uid = uid;
	  vcons->disp_node->nn_stat.st_gid = gid;
	}
      if (vcons->inpt_node)
	{
	  vcons->inpt_node->nn_stat.st_uid = uid;
	  vcons->inpt_node->nn_stat.st_gid = gid;
	}
    }
  mutex_unlock (&cons->lock);
  fshelp_touch (&node->nn_stat, TOUCH_CTIME, console_maptime);
  return err;
}

/* This should attempt a chauthor call for the user specified by CRED on node
   NODE, to change the author to AUTHOR. */
error_t
netfs_attempt_chauthor (struct iouser *cred, struct node *node, uid_t author)
{
  cons_t cons = node->nn->cons;
  vcons_t vcons;
  error_t err;

  if (!cons)
    return EOPNOTSUPP;

  err = file_chauthor (cons->underlying, author);
  if (err)
    return err;

  /* Change NODE's author.  */
  node->nn_stat.st_author = author;

  mutex_lock (&cons->lock);
  cons->stat_template.st_author = author;

  /* Change the author of each leaf node.  */
  for (vcons = cons->vcons_list; vcons; vcons = vcons->next)
    {
      if (vcons->dir_node)
	vcons->dir_node->nn_stat.st_author = author;
      if (vcons->cons_node)
	vcons->cons_node->nn_stat.st_author = author;
      if (vcons->disp_node)
	vcons->disp_node->nn_stat.st_author = author;
      if (vcons->inpt_node)
	vcons->inpt_node->nn_stat.st_author = author;
    }
  mutex_unlock (&cons->lock);
  fshelp_touch (&node->nn_stat, TOUCH_CTIME, console_maptime);
  return err;
}

/* This should attempt a chmod call for the user specified by CRED on node
   NODE, to change the mode to MODE.  Unlike the normal Unix and Hurd meaning
   of chmod, this function is also used to attempt to change files into other
   types.  If such a transition is attempted which is impossible, then return
   EOPNOTSUPP.  */
error_t
netfs_attempt_chmod (struct iouser *cred, struct node *node, mode_t mode)
{
  error_t err;

  mode &= ~S_ITRANS;
  if ((mode & S_IFMT) == 0)
    mode |= (node->nn_stat.st_mode & S_IFMT);

  if (!node->nn->cons || ((mode & S_IFMT) != (node->nn_stat.st_mode & S_IFMT)))
    return EOPNOTSUPP;

  err = file_chmod (node->nn->cons->underlying, mode & ~S_IFMT);
  if (err)
    return err;

  node->nn_stat.st_mode = mode;
  fshelp_touch (&node->nn_stat, TOUCH_CTIME, console_maptime);
  return err;
}


/* The user must define this function.  Attempt to turn locked node NP
   (user CRED) into a symlink with target NAME.  */
error_t
netfs_attempt_mksymlink (struct iouser *cred, struct node *np, char *name)
{
  return EOPNOTSUPP;
}

error_t
netfs_attempt_mkdev (struct iouser *cred, struct node *np,
		     mode_t type, dev_t indexes)
{
  return EOPNOTSUPP;
}

error_t
netfs_attempt_chflags (struct iouser *cred, struct node *np, int flags)
{
  return EOPNOTSUPP;
}

error_t
netfs_attempt_set_size (struct iouser *cred, struct node *np, off_t size)
{
  vcons_t vcons = np->nn->vcons;

  if (!vcons || np == vcons->dir_node
      || np == vcons->disp_node)
    return EOPNOTSUPP;

  assert (np == vcons->cons_node || np == vcons->inpt_node);
  return 0;
}

error_t
netfs_attempt_statfs (struct iouser *cred, struct node *np, struct statfs *st)
{
  return EOPNOTSUPP;
}

error_t
netfs_attempt_mkdir (struct iouser *user, struct node *dir,
		     char *name, mode_t mode)
{
  return EOPNOTSUPP;
}

error_t
netfs_attempt_unlink (struct iouser *user, struct node *dir, char *name)
{
  return EOPNOTSUPP;
}

error_t
netfs_attempt_rename (struct iouser *user, struct node *fromdir,
		      char *fromname, struct node *todir,
		      char *toname, int excl)
{
  return EOPNOTSUPP;
}

error_t
netfs_attempt_rmdir (struct iouser *user,
		     struct node *dir, char *name)
{
  return EOPNOTSUPP;
}

error_t
netfs_attempt_link (struct iouser *user, struct node *dir,
		    struct node *file, char *name, int excl)
{
  return EOPNOTSUPP;
}

error_t
netfs_attempt_mkfile (struct iouser *user, struct node *dir,
		      mode_t mode, struct node **np)
{
  return EOPNOTSUPP;
}

error_t
netfs_attempt_readlink (struct iouser *user, struct node *np, char *buf)
{
  return EOPNOTSUPP;
}

error_t
netfs_attempt_read (struct iouser *cred, struct node *np,
		    off_t offset, size_t *len, void *data)
{
  error_t err = 0;
  vcons_t vcons = np->nn->vcons;
  if (!vcons || np == vcons->dir_node
      || np == vcons->inpt_node)
    return EOPNOTSUPP;

  mutex_unlock (&np->lock);
  if (np == vcons->cons_node)
    {
      ssize_t amt = input_dequeue (vcons->input,
				   /* cred->po->openstat & O_NONBLOCK */ 0,
				   data, *len);
      if (amt == -1)
	err = errno;
      else
	*len = amt;
    }
  else
    {
      /* Pass display content to caller.  */
      ssize_t amt;
      assert (np == vcons->disp_node);
      amt  = display_read (vcons->display,
			   /* cred->po->openstat & O_NONBLOCK */ 0,
			   offset, data, *len);
      if (amt == -1)
	err = errno;
      else
	*len = amt;
    }
  mutex_lock (&np->lock);
  return err;
}

error_t
netfs_attempt_write (struct iouser *cred, struct node *np,
		     off_t offset, size_t *len, void *data)
{
  error_t err = 0;
  vcons_t vcons = np->nn->vcons;
  if (!vcons || np == vcons->dir_node
      || np == vcons->disp_node)
    return EOPNOTSUPP;

  mutex_unlock (&np->lock);
  if (np == vcons->cons_node)
    {
      /* The term server is writing to the console device.  Feed the
	 data into the screen matrix display.  */
      int amt = display_output (vcons->display,
				/* cred->po->openstat & O_NONBLOCK */ 0,
				data, *len);
      if (amt == -1)
	err = errno;
      else
	*len = amt;
    } 
  else
    {
      int amt;
      /* The input driver is writing to the input device.  Feed the
	 data into the input queue.  */
      assert (np == vcons->inpt_node);

      amt = input_enqueue (vcons->input,
			   /* cred->po->openstat & O_NONBLOCK */ 1,
			   data, *len);
      if (amt == -1)
	err = errno;
      else
	*len = amt;
    }
  mutex_lock (&np->lock);
  return err;
}


int
main (int argc, char **argv)
{
  error_t err;
  mach_port_t bootstrap;
  struct stat ul_stat;
  struct netnode root_nn = { cons: cons, vcons: 0 };

  mtrace();
  struct argp argp
    = { NULL, NULL, NULL,
	"A translator that provides virtual console displays." };

  /* Parse our command line arguments (all none of them).  */
  argp_parse (&argp, argc, argv, 0, 0, 0);

  task_get_bootstrap_port (mach_task_self (), &bootstrap);
  netfs_init ();

  /* Create the root node (some attributes initialized below).  */
  netfs_root_node = netfs_make_node (&root_nn);
  if (! netfs_root_node)
    error (5, ENOMEM, "Cannot create root node");

  err = maptime_map (0, 0, &console_maptime);
  if (err)
    error (6, err, "Cannot map time");

  mutex_init (&cons->lock);
  cons->encoding = "ISO-8859-1";
  cons->node = netfs_root_node;
  cons->underlying = netfs_startup (bootstrap, O_READ);
  if (cons->underlying == MACH_PORT_NULL)
    error (5, err, "Cannot get underlying node");

  err = io_stat (cons->underlying, &ul_stat);
  if (err)
    error (6, err, "Cannot stat underlying node");

  /* CONS.stat_template contains some fields that are inherited by all
     nodes we create.  */
  cons->stat_template.st_uid = ul_stat.st_uid;
  cons->stat_template.st_gid = ul_stat.st_gid;
  cons->stat_template.st_author = ul_stat.st_author;
  cons->stat_template.st_mode = (ul_stat.st_mode & ~S_IFMT & ~S_ITRANS);
  cons->stat_template.st_fsid = getpid ();
  cons->stat_template.st_nlink = 1;
  cons->stat_template.st_fstype = FSTYPE_MISC;

  /* Initialize the root node's stat information.  */
  netfs_root_node->nn_stat = cons->stat_template;
  netfs_root_node->nn_stat.st_ino = 2;
  netfs_root_node->nn_stat.st_mode |= S_IFDIR;
  netfs_root_node->nn_translated = 0;

  /* If the underlying node isn't a directory, propagate read permission to
     execute permission since we need that for lookups.  */
  if (! S_ISDIR (ul_stat.st_mode))
    {
      if (ul_stat.st_mode & S_IRUSR)
	netfs_root_node->nn_stat.st_mode |= S_IXUSR;
      if (ul_stat.st_mode & S_IRGRP)
	netfs_root_node->nn_stat.st_mode |= S_IXGRP;
      if (ul_stat.st_mode & S_IROTH)
	netfs_root_node->nn_stat.st_mode |= S_IXOTH;
    }
      
  fshelp_touch (&netfs_root_node->nn_stat, TOUCH_ATIME|TOUCH_MTIME|TOUCH_CTIME,
		console_maptime);

  netfs_server_loop ();		/* Never returns.  */

  /*NOTREACHED*/
  return 0;
}
