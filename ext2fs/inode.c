/* Inode management routines

   Copyright (C) 1994, 1995 Free Software Foundation, Inc.

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

#define	INOHSZ	512
#if	((INOHSZ&(INOHSZ-1)) == 0)
#define	INOHASH(ino)	((ino)&(INOHSZ-1))
#else
#define	INOHASH(ino)	(((unsigned)(ino))%INOHSZ)
#endif

static struct node *nodehash[INOHSZ];

static error_t read_disknode (struct node *np);

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
iget (ino_t inum, struct node **npp)
{
  error_t err;
  struct node *np;
  struct disknode *dn;

  spin_lock (&diskfs_node_refcnt_lock);
  for (np = nodehash[INOHASH(inum)]; np; np = np->dn->hnext)
    if (np->dn->number == inum)
      {
	np->references++;
	spin_unlock (&diskfs_node_refcnt_lock);
	mutex_lock (&np->lock);
	*npp = np;
	return 0;
      }
  
  dn = malloc (sizeof (struct disknode));
  
  dn->number = inum;
  dn->dirents = 0;
  
  rwlock_init (&dn->alloc_lock);
  pokel_init (&dn->indir_pokel, disk_pager, disk_image);
  dn->pager = 0;
  
  np = diskfs_make_node (dn);
  mutex_lock (&np->lock);
  dn->hnext = nodehash[INOHASH(inum)];
  if (dn->hnext)
    dn->hnext->dn->hprevp = &dn->hnext;
  dn->hprevp = &nodehash[INOHASH(inum)];
  nodehash[INOHASH(inum)] = np;
  spin_unlock (&diskfs_node_refcnt_lock);
  
  err = read_disknode (np);
  
  if (!diskfs_readonly && !np->dn_stat.st_gen)
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
      if (np->dn->number != inum)
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
read_disknode (struct node *np)
{
  error_t err;
  unsigned offset;
  static int fsid, fsidset;
  struct stat *st = &np->dn_stat;
  struct disknode *dn = np->dn;
  struct ext2_inode *di = dino (dn->number);
  struct ext2_inode_info *info = &dn->info;
  
  err = diskfs_catch_exception ();
  if (err)
    return err;

  np->istranslated = sblock->s_creator_os == EXT2_OS_HURD && di->i_translator;

  if (!fsidset)
    {
      fsid = getpid ();
      fsidset = 1;
    }

  st->st_fstype = FSTYPE_EXT2FS;
  st->st_fsid = fsid;
  st->st_ino = dn->number;
  st->st_blksize = vm_page_size * 2;

  st->st_mode = di->i_mode | (di->i_mode_high << 16);
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
  st->st_flags = di->i_flags;
  
  st->st_uid = di->i_uid | (di->i_uid_high << 16);
  st->st_gid = di->i_gid | (di->i_gid_high << 16);
  st->st_author = di->i_author;
  if (st->st_author == -1)
    st->st_author = st->st_uid;

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
  info->i_block_group = inode_group_num(dn->number);
  info->i_next_alloc_block = 0;
  info->i_next_alloc_goal = 0;
  info->i_prealloc_count = 0;

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

  /* Set these to conservative values.  */
  dn->last_page_partially_writable = 0;
  dn->last_block_allocated = 1;
  
  np->allocsize = np->dn_stat.st_size;
  offset = np->allocsize & ((1 << log2_block_size) - 1);
  if (offset > 0)
    np->allocsize += block_size - offset;

  return 0;
}

/* Writes everything from NP's inode to the disk image, and returns a pointer
   to it, or NULL if nothing need be done.  */
static struct ext2_inode *
write_node (struct node *np)
{
  error_t err;
  struct stat *st = &np->dn_stat;
  struct ext2_inode *di = dino (np->dn->number);

  if (np->dn->info.i_prealloc_count)
    ext2_discard_prealloc (np);
  
  assert (!np->dn_set_ctime && !np->dn_set_atime && !np->dn_set_mtime);
  if (np->dn_stat_dirty)
    {
      assert (!diskfs_readonly);

      ext2_debug ("writing inode %d to disk", np->dn->number);

      err = diskfs_catch_exception ();
      if (err)
	return NULL;
  
      di->i_version = st->st_gen;

      /* We happen to know that the stat mode bits are the same
	 as the ext2fs mode bits. */
      /* XXX? */

      di->i_mode = st->st_mode & 0xffff;
      di->i_mode_high = (st->st_mode >> 16) & 0xffff;
      
      di->i_uid = st->st_uid & 0xFFFF;
      di->i_gid = st->st_gid & 0xFFFF;
      di->i_uid_high = st->st_uid >> 16;
      di->i_gid_high = st->st_gid >> 16;

      di->i_author = st->st_author;

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
      di->i_flags = st->st_flags;
      if (! np->istranslated)
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
  read_disknode (node);

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
    sync_global_ptr (di, wait);
}

/* Set *ST with appropriate values to reflect the current state of the
   filesystem.  */
error_t
diskfs_set_statfs (struct fsys_statfsbuf *st)
{
  st->fsys_stb_type = FSTYPE_EXT2FS;
  st->fsys_stb_iosize = block_size;
  st->fsys_stb_bsize = frag_size;
  st->fsys_stb_blocks = sblock->s_blocks_count;
  st->fsys_stb_bfree = sblock->s_free_blocks_count;
  st->fsys_stb_bavail = st->fsys_stb_bfree - sblock->s_r_blocks_count;
  st->fsys_stb_files = sblock->s_inodes_count;
  st->fsys_stb_ffree = sblock->s_free_inodes_count;
  st->fsys_stb_fsid = getpid ();
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
  
  di = dino (np->dn->number);
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
      np->istranslated = 0;
      np->dn_set_ctime = 1;
    }
  
  if (namelen)
    {
      buf[0] = namelen & 0xFF;
      buf[1] = (namelen >> 8) & 0xFF;
      bcopy (name, buf + 2, namelen);

      bcopy (buf, bptr (blkno), block_size);
      record_global_poke (bptr (blkno));

      np->istranslated = 1;
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

  blkno = (dino (np->dn->number))->i_translator;
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

/* Called when all hard ports have gone away. */
void
diskfs_shutdown_soft_ports ()
{
  /* Should initiate termination of internally held pager ports
     (the only things that should be soft) XXX */
}
