/* Inode management routines

   Copyright (C) 1994, 1995, 1996 Free Software Foundation, Inc.

   Converted for ext2fs by Miles Bader <miles@gnu.ai.mit.edu>

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

#include "ext2fs.h"
#include <string.h>
#include <unistd.h>
#include <stdio.h>
#include <sys/stat.h>

/* these flags aren't actually defined by a header file yet, so temporarily
   disable them if necessary.  */
#ifndef UF_APPEND
#define UF_APPEND 0
#endif
#ifndef UF_NODUMP
#define UF_NODUMP 0
#endif
#ifndef UF_IMMUTABLE
#define UF_IMMUTABLE 0
#endif

#define	INOHSZ	512
#if	((INOHSZ&(INOHSZ-1)) == 0)
#define	INOHASH(ino)	((ino)&(INOHSZ-1))
#else
#define	INOHASH(ino)	(((unsigned)(ino))%INOHSZ)
#endif

static struct node *nodehash[INOHSZ];

static error_t read_node (struct node *np);

spin_lock_t generation_lock = SPIN_LOCK_INITIALIZER;

/* Initialize the inode hash table. */
void
inode_init ()
{
  int n;
  for (n = 0; n < INOHSZ; n++)
    nodehash[n] = 0;
}

/* Fetch inode INUM, set *NPP to the node structure; 
   gain one user reference and lock the node.  */
error_t 
diskfs_cached_lookup (int inum, struct node **npp)
{
  error_t err;
  struct node *np;
  struct disknode *dn;

  spin_lock (&diskfs_node_refcnt_lock);
  for (np = nodehash[INOHASH(inum)]; np; np = np->dn->hnext)
    if (np->cache_id == inum)
      {
	np->references++;
	spin_unlock (&diskfs_node_refcnt_lock);
	mutex_lock (&np->lock);
	*npp = np;
	return 0;
      }
  
  /* Format specific data for the new node.  */
  dn = malloc (sizeof (struct disknode));
  if (! dn)
    {
      spin_unlock (&diskfs_node_refcnt_lock);
      return ENOMEM;
    }
  dn->dirents = 0;
  dn->dir_idx = 0;
  dn->pager = 0;
  rwlock_init (&dn->alloc_lock);
  pokel_init (&dn->indir_pokel, diskfs_disk_pager, disk_image);

  /* Create the new node.  */
  np = diskfs_make_node (dn);
  np->cache_id = inum;

  mutex_lock (&np->lock);

  /* Put NP in NODEHASH.  */
  dn->hnext = nodehash[INOHASH(inum)];
  if (dn->hnext)
    dn->hnext->dn->hprevp = &dn->hnext;
  dn->hprevp = &nodehash[INOHASH(inum)];
  nodehash[INOHASH(inum)] = np;

  spin_unlock (&diskfs_node_refcnt_lock);
  
  /* Get the contents of NP off disk.  */
  err = read_node (np);
  
  if (!diskfs_check_readonly () && !np->dn_stat.st_gen)
    {
      spin_lock (&generation_lock);
      if (++next_generation < diskfs_mtime->seconds)
	next_generation = diskfs_mtime->seconds;
      np->dn_stat.st_gen = next_generation;
      spin_unlock (&generation_lock);
      np->dn_set_ctime = 1;
    }
  
  if (err)
    return err;
  else
    {
      *npp = np;
      return 0;
    }
}

/* Lookup node INUM (which must have a reference already) and return it
   without allocating any new references. */
struct node *
ifind (ino_t inum)
{
  struct node *np;
  
  spin_lock (&diskfs_node_refcnt_lock);
  for (np = nodehash[INOHASH(inum)]; np; np = np->dn->hnext)
    {
      if (np->cache_id != inum)
	continue;
      
      assert (np->references);
      spin_unlock (&diskfs_node_refcnt_lock);
      return np;
    }
  assert (0);
}

/* The last reference to a node has gone away; drop
   it from the hash table and clean all state in the dn structure. */
void      
diskfs_node_norefs (struct node *np)
{
  *np->dn->hprevp = np->dn->hnext;
  if (np->dn->hnext)
    np->dn->hnext->dn->hprevp = np->dn->hprevp;

  if (np->dn->dirents)
    free (np->dn->dirents);
  assert (!np->dn->pager);

  /* Move any pending writes of indirect blocks.  */
  pokel_inherit (&global_pokel, &np->dn->indir_pokel);
  pokel_finalize (&np->dn->indir_pokel);

  free (np->dn);
  free (np);
}

/* The last hard reference to a node has gone away; arrange to have
   all the weak references dropped that can be. */
void
diskfs_try_dropping_softrefs (struct node *np)
{
  drop_pager_softrefs (np);
}

/* The last hard reference to a node has gone away. */
void
diskfs_lost_hardrefs (struct node *np)
{
}

/* A new hard reference to a node has been created; it's now OK to
   have unused weak references. */
void
diskfs_new_hardrefs (struct node *np)
{
  allow_pager_softrefs (np);
}

/* Read stat information out of the ext2_inode. */
static error_t
read_node (struct node *np)
{
  error_t err;
  static int fsid, fsidset;
  struct stat *st = &np->dn_stat;
  struct disknode *dn = np->dn;
  struct ext2_inode *di = dino (np->cache_id);
  struct ext2_inode_info *info = &dn->info;
  
  err = diskfs_catch_exception ();
  if (err)
    return err;

  if (!fsidset)
    {
      fsid = getpid ();
      fsidset = 1;
    }

  st->st_fstype = FSTYPE_EXT2FS;
  st->st_fsid = fsid;
  st->st_ino = np->cache_id;
  st->st_blksize = vm_page_size * 2;

  st->st_nlink = di->i_links_count;
  st->st_size = di->i_size;
  st->st_gen = di->i_version;

  st->st_atime = di->i_atime;
  st->st_mtime = di->i_mtime;
  st->st_ctime = di->i_ctime;

#ifdef XXX
  st->st_atime_usec = di->i_atime.ts_nsec / 1000;
  st->st_mtime_usec = di->i_mtime.ts_nsec / 1000;
  st->st_ctime_usec = di->i_ctime.ts_nsec / 1000;
#endif

  st->st_blocks = di->i_blocks;

  st->st_flags = 0;
  if (di->i_flags & EXT2_APPEND_FL)
    st->st_flags |= UF_APPEND;
  if (di->i_flags & EXT2_NODUMP_FL)
    st->st_flags |= UF_NODUMP;
  if (di->i_flags & EXT2_IMMUTABLE_FL)
    st->st_flags |= UF_IMMUTABLE;

  if (sblock->s_creator_os == EXT2_OS_HURD)
    {
      st->st_mode = di->i_mode | (di->i_mode_high << 16);
      st->st_mode &= ~S_ITRANS;
      if (di->i_translator)
	st->st_mode |= S_IPTRANS;

      st->st_uid = di->i_uid | (di->i_uid_high << 16);
      st->st_gid = di->i_gid | (di->i_gid_high << 16);

      st->st_author = di->i_author;
      if (st->st_author == -1)
	st->st_author = st->st_uid;
    }
  else
    {
      st->st_mode = di->i_mode & ~S_ITRANS;
      st->st_uid = di->i_uid;
      st->st_gid = di->i_gid;
      st->st_author = st->st_uid;
    }

  /* Setup the ext2fs auxiliary inode info.  */
  info->i_dtime = di->i_dtime;
  info->i_flags = di->i_flags;
  info->i_faddr = di->i_faddr;
  info->i_frag_no = di->i_frag;
  info->i_frag_size = di->i_fsize;
  info->i_osync = 0;
  info->i_file_acl = di->i_file_acl;
  info->i_dir_acl = di->i_dir_acl;
  info->i_version = di->i_version;
  info->i_block_group = inode_group_num (np->cache_id);
  info->i_next_alloc_block = 0;
  info->i_next_alloc_goal = 0;
  info->i_prealloc_count = 0;

  /* Set to a conservative value.  */
  dn->last_page_partially_writable = 0;

  if (S_ISCHR(st->st_mode) || S_ISBLK(st->st_mode))
    st->st_rdev = di->i_block[0];
  else
    {
      int block;
      for (block = 0; block < EXT2_N_BLOCKS; block++)
	info->i_data[block] = di->i_block[block];
      st->st_rdev = 0;
    }

  diskfs_end_catch_exception ();
  
  if (S_ISREG (st->st_mode) || S_ISDIR (st->st_mode)
      || (S_ISLNK (st->st_mode) && st->st_blocks))
    {
      unsigned offset;

      np->allocsize = np->dn_stat.st_size;

      /* Round up to a block multiple.  */
      offset = np->allocsize & ((1 << log2_block_size) - 1);
      if (offset > 0)
	np->allocsize += block_size - offset;
    }
  else
    /* Allocsize should be zero for anything except directories, files, and
       long symlinks.  These are the only things allowed to have any blocks
       allocated as well, although st_size may be zero for any type (cases
       where st_blocks=0 and st_size>0 include fast symlinks, and, under
       linux, some devices).  */
    {
      np->allocsize = 0;
      assert (st->st_blocks == 0);
    }

  return 0;
}

/* Return EINVAL if this is not a hurd filesystem and any bits are set in L
   except the low 16 bits, else 0.  */
static inline error_t
check_high_bits (struct node *np, long l)
{
  if (sblock->s_creator_os == EXT2_OS_HURD)
    return 0;
  else
    return ((l & ~0xFFFF) == 0) ? 0 : EINVAL;
}

/* Return 0 if NP's owner can be changed to UID; otherwise return an error
   code. */
error_t
diskfs_validate_owner_change (struct node *np, uid_t uid)
{
  return check_high_bits (np, uid);
}

/* Return 0 if NP's group can be changed to GID; otherwise return an error
   code. */
error_t
diskfs_validate_group_change (struct node *np, gid_t gid)
{
  return check_high_bits (np, gid);
}

/* Return 0 if NP's mode can be changed to MODE; otherwise return an error
   code.  It must always be possible to clear the mode; diskfs will not ask
   for permission before doing so.  */
error_t
diskfs_validate_mode_change (struct node *np, mode_t mode)
{
  return check_high_bits (np, mode);
}

/* Return 0 if NP's author can be changed to AUTHOR; otherwise return an
   error code. */
error_t
diskfs_validate_author_change (struct node *np, uid_t author)
{
  if (sblock->s_creator_os == EXT2_OS_HURD)
    return 0;
  else
    /* For non-hurd filesystems, the auther & owner are the same.  */
    return (author == np->dn_stat.st_uid) ? 0 : EINVAL;
}

/* The user may define this function.  Return 0 if NP's flags can be
   changed to FLAGS; otherwise return an error code.  It must always
   be possible to clear the flags.   */
error_t
diskfs_validate_flags_change (struct node *np, int flags)
{
  if (flags & (UF_NODUMP | UF_IMMUTABLE | UF_APPEND))
    return 0;
  else
    return EINVAL;
}

/* Writes everything from NP's inode to the disk image, and returns a pointer
   to it, or NULL if nothing need be done.  */
static struct ext2_inode *
write_node (struct node *np)
{
  error_t err;
  struct stat *st = &np->dn_stat;
  struct ext2_inode *di = dino (np->cache_id);

  if (np->dn->info.i_prealloc_count)
    ext2_discard_prealloc (np);
  
  assert (!np->dn_set_ctime && !np->dn_set_atime && !np->dn_set_mtime);
  if (np->dn_stat_dirty)
    {
      assert (!diskfs_readonly);

      ext2_debug ("writing inode %d to disk", np->cache_id);

      err = diskfs_catch_exception ();
      if (err)
	return NULL;
  
      di->i_version = st->st_gen;

      /* We happen to know that the stat mode bits are the same
	 as the ext2fs mode bits. */
      /* XXX? */

      /* Only the low 16 bits of these fields are standard across all ext2
	 implementations.  */
      di->i_mode = st->st_mode & 0xFFFF & ~S_ITRANS;
      di->i_uid = st->st_uid & 0xFFFF;
      di->i_gid = st->st_gid & 0xFFFF;

      if (sblock->s_creator_os == EXT2_OS_HURD)
	/* If this is a hurd-compatible filesystem, write the high bits too. */
	{
	  di->i_mode_high = (st->st_mode >> 16) & 0xffff & ~S_ITRANS;
	  di->i_uid_high = st->st_uid >> 16;
	  di->i_gid_high = st->st_gid >> 16;
	  di->i_author = st->st_author;
	}
      else
	/* No hurd extensions should be turned on.  */
	{
	  assert ((st->st_uid & ~0xFFFF) == 0);
	  assert ((st->st_gid & ~0xFFFF) == 0);
	  assert ((st->st_mode & ~0xFFFF) == 0);
	  assert (st->st_author == st->st_uid);
	}

      di->i_links_count = st->st_nlink;
      di->i_size = st->st_size;

      di->i_atime = st->st_atime;
      di->i_mtime = st->st_mtime;
      di->i_ctime = st->st_ctime;
#ifdef XXX
      di->i_atime.ts_nsec = st->st_atime_usec * 1000;
      di->i_mtime.ts_nsec = st->st_mtime_usec * 1000;
      di->i_ctime.ts_nsec = st->st_ctime_usec * 1000;
#endif

      di->i_blocks = st->st_blocks;

      /* Convert generic flags in ST->st_flags to ext2-specific flags in DI
         (but don't mess with ext2 flags we don't know about).  */
      di->i_flags &= ~(EXT2_APPEND_FL | EXT2_NODUMP_FL | EXT2_IMMUTABLE_FL);
      if (st->st_flags & UF_APPEND)
	di->i_flags |= EXT2_APPEND_FL;
      if (st->st_flags & UF_NODUMP)
	di->i_flags |= EXT2_NODUMP_FL;
      if (st->st_flags & UF_IMMUTABLE)
	di->i_flags |= EXT2_IMMUTABLE_FL;

      if (!(st->st_mode & S_IPTRANS) && sblock->s_creator_os == EXT2_OS_HURD)
	di->i_translator = 0;

      /* Set dtime non-zero to indicate a deleted file.  */
      di->i_dtime = (st->st_mode ? 0 : di->i_mtime);

      if (S_ISCHR(st->st_mode) || S_ISBLK(st->st_mode))
	di->i_block[0] = st->st_rdev;
      else
	{
	  int block;
	  for (block = 0; block < EXT2_N_BLOCKS; block++)
	    di->i_block[block] = np->dn->info.i_data[block];
	}
  
      diskfs_end_catch_exception ();
      np->dn_stat_dirty = 0;

      return di;
    }
  else
    return NULL;
}

/* Reload all data specific to NODE from disk, without writing anything.
   Always called with DISKFS_READONLY true.  */
error_t
diskfs_node_reload (struct node *node)
{
  struct disknode *dn = node->dn;

  if (dn->dirents)
    {
      free (dn->dirents);
      dn->dirents = 0;
    }
  pokel_flush (&dn->indir_pokel);
  flush_node_pager (node);
  read_node (node);

  return 0;
}

/* For each active node, call FUN.  The node is to be locked around the call
   to FUN.  If FUN returns non-zero for any node, then immediately stop, and
   return that value.  */
error_t
diskfs_node_iterate (error_t (*fun)(struct node *))
{
  error_t err = 0;
  int n, num_nodes = 0;
  struct node *node, **node_list, **p;
  
  spin_lock (&diskfs_node_refcnt_lock);

  /* We must copy everything from the hash table into another data structure
     to avoid running into any problems with the hash-table being modified
     during processing (normally we delegate access to hash-table with
     diskfs_node_refcnt_lock, but we can't hold this while locking the
     individual node locks).  */

  for (n = 0; n < INOHSZ; n++)
    for (node = nodehash[n]; node; node = node->dn->hnext)
      num_nodes++;

  node_list = alloca (num_nodes * sizeof (struct node *));
  p = node_list;
  for (n = 0; n < INOHSZ; n++)
    for (node = nodehash[n]; node; node = node->dn->hnext)
      {
	*p++ = node;
	node->references++;
      }

  spin_unlock (&diskfs_node_refcnt_lock);

  p = node_list;
  while (num_nodes-- > 0)
    {
      node = *p++;
      if (!err)
	{
	  mutex_lock (&node->lock);
	  err = (*fun)(node);
	  mutex_unlock (&node->lock);
	}
      diskfs_nrele (node);
    }

  return err;
}

/* Write all active disknodes into the ext2_inode pager. */
void
write_all_disknodes ()
{
  error_t write_one_disknode (struct node *node)
    {
      struct ext2_inode *di;

      diskfs_set_node_times (node);

      /* Sync the indirect blocks here; they'll all be done before any
	 inodes.  Waiting for them shouldn't be too bad.  */
      pokel_sync (&node->dn->indir_pokel, 1);

      /* Update the inode image.  */
      di = write_node (node);
      if (di)
	record_global_poke (di);

      return 0;
    }

  diskfs_node_iterate (write_one_disknode);
}

/* Sync the info in NP->dn_stat and any associated format-specific
   information to disk.  If WAIT is true, then return only after the
   physicial media has been completely updated.  */
void
diskfs_write_disknode (struct node *np, int wait)
{
  struct ext2_inode *di = write_node (np);
  if (di)
    if (wait)
      sync_global_ptr (di, 1);
    else
      record_global_poke (di);
}

/* Set *ST with appropriate values to reflect the current state of the
   filesystem.  */
error_t
diskfs_set_statfs (struct statfs *st)
{
  st->f_type = FSTYPE_EXT2FS;
  st->f_bsize = block_size;
  st->f_blocks = sblock->s_blocks_count;
  st->f_bfree = sblock->s_free_blocks_count;
  st->f_bavail = st->f_bfree - sblock->s_r_blocks_count;
  st->f_files = sblock->s_inodes_count;
  st->f_ffree = sblock->s_free_inodes_count;
  st->f_fsid = getpid ();
  st->f_namelen = 0;
  return 0;
}

/* Implement the diskfs_set_translator callback from the diskfs
   library; see <hurd/diskfs.h> for the interface description. */
error_t
diskfs_set_translator (struct node *np, char *name, unsigned namelen,
		       struct protid *cred)
{
  daddr_t blkno;
  error_t err;
  char buf[block_size];
  struct ext2_inode *di;

  assert (!diskfs_readonly);

  if (sblock->s_creator_os != EXT2_OS_HURD)
    return EOPNOTSUPP;

  if (namelen + 2 > block_size)
    return ENAMETOOLONG;

  err = diskfs_catch_exception ();
  if (err)
    return err;
  
  di = dino (np->cache_id);
  blkno = di->i_translator;
  
  if (namelen && !blkno)
    {
      /* Allocate block for translator */
      blkno =
	ext2_new_block ((np->dn->info.i_block_group
			 * EXT2_BLOCKS_PER_GROUP (sblock))
			+ sblock->s_first_data_block,
			0, 0);
      if (blkno == 0)
	{
	  diskfs_end_catch_exception ();
	  return ENOSPC;
	}
      
      di->i_translator = blkno;
      record_global_poke (di);

      np->dn_stat.st_blocks += 1 << log2_stat_blocks_per_fs_block;
      np->dn_set_ctime = 1;
    }
  else if (!namelen && blkno)
    {
      /* Clear block for translator going away. */
      di->i_translator = 0;
      record_global_poke (di);
      ext2_free_blocks (blkno, 1);

      np->dn_stat.st_blocks -= 1 << log2_stat_blocks_per_fs_block;
      np->dn_stat.st_mode &= ~S_IPTRANS;
      np->dn_set_ctime = 1;
    }
  
  if (namelen)
    {
      buf[0] = namelen & 0xFF;
      buf[1] = (namelen >> 8) & 0xFF;
      bcopy (name, buf + 2, namelen);

      bcopy (buf, bptr (blkno), block_size);
      record_global_poke (bptr (blkno));

      np->dn_stat.st_mode |= S_IPTRANS;
      np->dn_set_ctime = 1;
    }
  
  diskfs_end_catch_exception ();
  return err;
}

/* Implement the diskfs_get_translator callback from the diskfs library.
   See <hurd/diskfs.h> for the interface description. */
error_t
diskfs_get_translator (struct node *np, char **namep, unsigned *namelen)
{
  error_t err;
  daddr_t blkno;
  unsigned datalen;
  void *transloc;

  assert (sblock->s_creator_os == EXT2_OS_HURD);

  err = diskfs_catch_exception ();
  if (err)
    return err;

  blkno = (dino (np->cache_id))->i_translator;
  assert (blkno);
  transloc = bptr (blkno);
  
  datalen =
    ((unsigned char *)transloc)[0] + (((unsigned char *)transloc)[1] << 8);
  *namep = malloc (datalen);
  bcopy (transloc + 2, *namep, datalen);

  diskfs_end_catch_exception ();

  *namelen = datalen;
  return 0;
}

/* The maximum size of a symlink store in the inode (including '\0').  */
#define MAX_INODE_SYMLINK \
  (EXT2_N_BLOCKS * sizeof (((struct ext2_inode *)0)->i_block[0]))

/* Write an in-inode symlink, or return EINVAL if we can't.  */
static error_t
write_symlink (struct node *node, char *target)
{
  size_t len = strlen (target) + 1;

  if (len > MAX_INODE_SYMLINK)
    return EINVAL;

  assert (node->dn_stat.st_blocks == 0);

  bcopy (target, node->dn->info.i_data, len);
  node->dn_stat.st_size = len - 1;
  node->dn_set_ctime = 1;
  node->dn_set_mtime = 1;

  return 0;
}

/* Read an in-inode symlink, or return EINVAL if we can't.  */
static error_t
read_symlink (struct node *node, char *target)
{
  if (node->dn_stat.st_blocks)
    return EINVAL;

  assert (node->dn_stat.st_size < MAX_INODE_SYMLINK);

  bcopy (node->dn->info.i_data, target, node->dn_stat.st_size);
  return 0;
}

/* If this function is nonzero (and diskfs_shortcut_symlink is set) it
   is called to set a symlink.  If it returns EINVAL or isn't set,
   then the normal method (writing the contents into the file data) is
   used.  If it returns any other error, it is returned to the user.  */
error_t (*diskfs_create_symlink_hook)(struct node *np, char *target) =
  write_symlink;

/* If this function is nonzero (and diskfs_shortcut_symlink is set) it
   is called to read the contents of a symlink.  If it returns EINVAL or
   isn't set, then the normal method (reading from the file data) is
   used.  If it returns any other error, it is returned to the user. */
error_t (*diskfs_read_symlink_hook)(struct node *np, char *target) =
  read_symlink;

/* Called when all hard ports have gone away. */
void
diskfs_shutdown_soft_ports ()
{
  /* Should initiate termination of internally held pager ports
     (the only things that should be soft) XXX */
}
