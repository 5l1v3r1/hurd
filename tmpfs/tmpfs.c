/* Main program and global state for tmpfs.
   Copyright (C) 2000, 2001 Free Software Foundation, Inc.

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

#include <argp.h>
#include <argz.h>
#include <string.h>
#include <inttypes.h>
#include <error.h>

#include "tmpfs.h"
#include <limits.h>
#include <version.h>

char *diskfs_server_name = "tmpfs";
char *diskfs_server_version = HURD_VERSION;
char *diskfs_disk_name = "swap";

/* We ain't got to show you no stinkin' sync'ing.  */
int diskfs_default_sync_interval = 0;

/* We must supply some claimed limits, though we don't impose any new ones.  */
int diskfs_link_max = (1ULL << (sizeof (nlink_t) * CHAR_BIT)) - 1;
int diskfs_name_max = 255;	/* dirent d_namlen limit */
int diskfs_maxsymlinks = 8;

/* Yeah, baby, we do it all!  */
int diskfs_shortcut_symlink = 1;
int diskfs_shortcurt_chrdev = 1;
int diskfs_shortcurt_blkdev = 1;
int diskfs_shortcurt_fifo = 1;
int diskfs_shortcurt_ifsock = 1;

struct node *diskfs_root_node;
mach_port_t default_pager;

off_t tmpfs_page_limit, tmpfs_space_used;

error_t
diskfs_set_statfs (struct statfs *st)
{
  fsblkcnt_t pages;

  st->f_type = FSTYPE_MEMFS;
  st->f_fsid = getpid ();

  st->f_bsize = vm_page_size;
  st->f_blocks = tmpfs_page_limit;

  spin_lock (&diskfs_node_refcnt_lock);
  st->f_files = num_files;
  pages = round_page (tmpfs_space_used) / vm_page_size;
  spin_unlock (&diskfs_node_refcnt_lock);

  st->f_bfree = pages < tmpfs_page_limit ? tmpfs_page_limit - pages : 0;
  st->f_bavail = st->f_bfree;
  st->f_ffree = st->f_bavail / sizeof (struct disknode); /* Well, sort of.  */

  return 0;
}


error_t
diskfs_set_hypermetadata (int wait, int clean)
{
  /* All the state always just lives in core, so we have nothing to do.  */
  return 0;
}

void
diskfs_sync_everything (int wait)
{
}

error_t
diskfs_reload_global_state ()
{
  return 0;
}

int diskfs_synchronous = 0;


/* Parse a command line option.  */
static error_t
parse_opt (int key, char *arg, struct argp_state *state)
{
  /* We save our parsed values in this structure, hung off STATE->hook.
     Only after parsing all options successfully will we use these values.  */
  struct
  {
    off_t size;
  } *values = state->hook;

  switch (key)
    {
    case ARGP_KEY_INIT:
      state->child_inputs[0] = state->input;
      values = malloc (sizeof *values);
      if (values == 0)
	return ENOMEM;
      state->hook = values;
      bzero (values, sizeof *values);
      break;

    case ARGP_KEY_NO_ARGS:
      argp_error (state, "must supply maximum size");
      return EINVAL;

    case ARGP_KEY_ARGS:
      if (state->argv[state->next + 1] != 0)
	{
	  argp_error (state, "too many arguments");
	  return EINVAL;
	}
      else
	{
	  char *end = NULL;
	  intmax_t size = strtoimax (state->argv[state->next], &end, 0);
	  if (end == NULL || end == arg)
	    {
	      argp_error (state, "argument must be a number");
	      return EINVAL;
	    }
	  if (size < 0)
	    {
	      argp_error (state, "negative size not meaningful");
	      return EINVAL;
	    }
	  switch (*end)
	    {
	    case 'g':
	    case 'G':
	      size <<= 10;
	    case 'm':
	    case 'M':
	      size <<= 10;
	    case 'k':
	    case 'K':
	      size <<= 10;
	      break;
	    }
	  size = (off_t) size;
	  if (size < 0)
	    {
	      argp_error (state, "size too large");
	      return EINVAL;
	    }
	  values->size = size;
	}
      break;

    case ARGP_KEY_SUCCESS:
      /* All options parse successfully, so implement ours if possible.  */
      tmpfs_page_limit = values->size / vm_page_size;
      break;

    default:
      return ARGP_ERR_UNKNOWN;
    }
  return 0;
}

/* Override the standard diskfs routine so we can add our own output.  */
error_t
diskfs_append_args (char **argz, unsigned *argz_len)
{
  error_t err;

  /* Get the standard things.  */
  err = diskfs_append_std_options (argz, argz_len);

  if (!err)
    {
      off_t lim = tmpfs_page_limit * vm_page_size;
      char buf[100], sfx;
#define S(n, c) if ((lim & ((1 << n) - 1)) == 0) sfx = c, lim >>= n
      S (30, 'G'); else S (20, 'M'); else S (10, 'K'); else sfx = '\0';
#undef S
      snprintf (buf, sizeof buf, "%ld%c", lim, sfx);
      err = argz_add (argz, argz_len, buf);
    }

  return err;
}

/* Add our startup arguments to the standard diskfs set.  */
static const struct argp_child startup_children[] =
  {{&diskfs_startup_argp}, {0}};
static struct argp startup_argp = {0, parse_opt, "MAX-BYTES", "\
\v\
MAX-BYTES may be followed by k or K for kilobytes,\n\
m or M for megabytes, g or G for gigabytes.",
				   startup_children};

/* Similarly at runtime.  */
static const struct argp_child runtime_children[] =
  {{&diskfs_std_runtime_argp}, {0}};
static struct argp runtime_argp = {0, parse_opt, 0, 0, runtime_children};

struct argp *diskfs_runtime_argp = (struct argp *)&runtime_argp;



int
main (int argc, char **argv)
{
  error_t err;
  mach_port_t bootstrap, realnode, host_priv;
  struct stat st;

  err = argp_parse (&startup_argp, argc, argv, ARGP_IN_ORDER, NULL, NULL);
  assert_perror (err);

  task_get_bootstrap_port (mach_task_self (), &bootstrap);
  if (bootstrap == MACH_PORT_NULL)
    error (2, 0, "Must be started as a translator");

  /* Get our port to the default pager.  Without that,
     we have no place to put file contents.  */
  err = get_privileged_ports (&host_priv, NULL);
  if (err)
    error (0, err, "Cannot get host privileged port");
  else
    {
      err = vm_set_default_memory_manager (host_priv, &default_pager);
      mach_port_deallocate (mach_task_self (), host_priv);
      if (err)
	error (0, err, "Cannot get default pager port");
    }
  if (default_pager == MACH_PORT_NULL)
    error (0, 0, "files cannot have contents with no default pager port");

  /* Initialize the diskfs library.  Must come before any other diskfs call. */
  err = diskfs_init_diskfs ();
  if (err)
    error (4, err, "init");

  err = diskfs_alloc_node (0, S_IFDIR, &diskfs_root_node);
  if (err)
    error (4, err, "cannot create root directory");

  diskfs_spawn_first_thread ();

  /* Now that we are all set up to handle requests, and diskfs_root_node is
     set properly, it is safe to export our fsys control port to the
     outside world.  */
  realnode = diskfs_startup_diskfs (bootstrap, 0);

  /* Propagate permissions, owner, etc. from underlying node to
     the root directory of the new (empty) filesystem.  */
  err = io_stat (realnode, &st);
  mach_port_deallocate (mach_task_self (), realnode);
  if (err)
    {
      error (0, err, "cannot stat underlying node");
      diskfs_root_node->dn_stat.st_mode = S_IFDIR | 0777 | S_ISVTX;
      diskfs_root_node->dn_set_ctime = 1;
      diskfs_root_node->dn_set_mtime = 1;
      diskfs_root_node->dn_set_atime = 1;
    }
  else
    {
      diskfs_root_node->dn_stat.st_mode = S_IFDIR | (st.st_mode &~ S_IFMT);
      if (S_ISREG(st.st_mode) && (st.st_mode & 0111) == 0)
	/* There are no execute bits set, as by default on a plain file.
	   For the virtual directory, set execute bits where read bits are
	   set on the underlying plain file.  */
	diskfs_root_node->dn_stat.st_mode |= (st.st_mode & 0444) >> 2;
      diskfs_root_node->dn_stat.st_uid = st.st_uid;
      diskfs_root_node->dn_stat.st_author = st.st_author;
      diskfs_root_node->dn_stat.st_gid = st.st_gid;
      diskfs_root_node->dn_stat.st_atime = st.st_atime;
      diskfs_root_node->dn_stat.st_mtime = st.st_mtime;
      diskfs_root_node->dn_stat.st_ctime = st.st_ctime;
      diskfs_root_node->dn_stat.st_flags = st.st_flags;
    }
  diskfs_root_node->dn_stat.st_mode &= ~S_ITRANS;
  diskfs_root_node->dn_stat.st_mode |= S_IROOT;

  mutex_unlock (&diskfs_root_node->lock);

  /* and so we die, leaving others to do the real work.  */
  cthread_exit (0);
  /* NOTREACHED */
  return 0;
}