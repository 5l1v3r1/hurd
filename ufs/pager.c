/* Pager for ufs
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

#include "ufs.h"
#include <strings.h>
#include <stdio.h>
#include <unistd.h>

spin_lock_t node2pagelock = SPIN_LOCK_INITIALIZER;

#ifdef DONT_CACHE_MEMORY_OBJECTS
#define MAY_CACHE 0
#else
#define MAY_CACHE 1
#endif

struct port_bucket *pager_bucket;

/* Find the location on disk of page OFFSET in pager UPI.  Return the
   disk address (in disk block) in *ADDR.  If *NPLOCK is set on
   return, then release that mutex after I/O on the data has
   completed.  Set DISKSIZE to be the amount of valid data on disk.
   (If this is an unallocated block, then set *ADDR to zero.)  */
static error_t
find_address (struct user_pager_info *upi,
	      vm_address_t offset,
	      daddr_t *addr,
	      int *disksize,
	      struct rwlock **nplock)
{
  error_t err;
  
  assert (upi->type == DISK || upi->type == FILE_DATA);

  if (upi->type == DISK)
    {
      *disksize = __vm_page_size;
      *addr = offset / DEV_BSIZE;
      *nplock = 0;
      return 0;
    }
  else 
    {
      struct iblock_spec indirs[NIADDR + 1];
      struct node *np;
  
      np = upi->np;
      
      rwlock_reader_lock (&np->dn->allocptrlock);
      *nplock = &np->dn->allocptrlock;

      if (offset >= np->allocsize)
	{
	  rwlock_reader_unlock (&np->dn->allocptrlock);
	  return EIO;
	}
      
      if (offset + __vm_page_size > np->allocsize)
	*disksize = np->allocsize - offset;
      else
	*disksize = __vm_page_size;
      
      err = fetch_indir_spec (np, lblkno (sblock, offset), indirs);
      if (err)
	rwlock_reader_unlock (&np->dn->allocptrlock);
      else
	{
	  if (indirs[0].bno)
	    *addr = (fsbtodb (sblock, indirs[0].bno)
		     + blkoff (sblock, offset) / DEV_BSIZE);
	  else
	    *addr = 0;
	}
      
      return err;
    }
}


/* Implement the pager_read_page callback from the pager library.  See 
   <hurd/pager.h> for the interface description. */
error_t
pager_read_page (struct user_pager_info *pager,
		 vm_offset_t page,
		 vm_address_t *buf,
		 int *writelock)
{
  error_t err;
  struct rwlock *nplock;
  daddr_t addr;
  int disksize;
  
  err = find_address (pager, page, &addr, &disksize, &nplock);
  if (err)
    return err;
  
  if (addr)
    {
      err = dev_read_sync (addr, (void *)buf, disksize);
      if (!err && disksize != __vm_page_size)
	bzero ((void *)(*buf + disksize), __vm_page_size - disksize);
      *writelock = 0;
    }
  else
    {
#if 0
      printf ("Write-locked pagein Object %#x\tOffset %#x\n", pager, page);
      fflush (stdout);
#endif
      vm_allocate (mach_task_self (), buf, __vm_page_size, 1);
      *writelock = 1;
    }
      
  if (nplock)
    rwlock_reader_unlock (nplock);
  
  return err;
}

/* Implement the pager_write_page callback from the pager library.  See 
   <hurd/pager.h> for the interface description. */
error_t
pager_write_page (struct user_pager_info *pager,
		  vm_offset_t page,
		  vm_address_t buf)
{
  daddr_t addr;
  int disksize;
  struct rwlock *nplock;
  error_t err;
  
  err = find_address (pager, page, &addr, &disksize, &nplock);
  if (err)
    return err;
  
  if (addr)
    err = dev_write_sync (addr, buf, disksize);
  else
    {
      printf ("Attempt to write unallocated disk\n.");
      printf ("Object %p\tOffset %#x\n", pager, page);
      fflush (stdout);
      err = 0;			/* unallocated disk; 
				   error would be pointless */
    }
    
  if (nplock)
    rwlock_reader_unlock (nplock);
  
  return err;
}

/* Implement the pager_unlock_page callback from the pager library.  See 
   <hurd/pager.h> for the interface description. */
error_t
pager_unlock_page (struct user_pager_info *pager,
		   vm_offset_t address)
{
  struct node *np;
  error_t err;
  struct iblock_spec indirs[NIADDR + 1];
  daddr_t bno;
  struct disknode *dn;
  struct dinode *di;

  /* Zero an sblock->fs_bsize piece of disk starting at BNO, 
     synchronously.  We do this on newly allocated indirect
     blocks before setting the pointer to them to ensure that an
     indirect block absolutely never points to garbage. */
  void zero_disk_block (int bno)
    {
      bzero (indir_block (bno), sblock->fs_bsize);
      sync_disk_blocks (bno, sblock->fs_bsize, 1);
    };

  /* Problem--where to get cred values for allocation here? */

#if 0
  printf ("Unlock page request, Object %#x\tOffset %#x...", pager, address);
  fflush (stdout);
#endif

  if (pager->type == DISK)
    return 0;
  
  np = pager->np;
  dn = np->dn;
  di = dino (dn->number);

  rwlock_writer_lock (&dn->allocptrlock);
  
  /* If this is the last block, we don't let it get unlocked. */
  if (address + __vm_page_size
      > blkroundup (sblock, np->allocsize) - sblock->fs_bsize)
    {
      printf ("attempt to unlock at last block denied\n");
      fflush (stdout);
      rwlock_writer_unlock (&dn->allocptrlock);
      return EIO;
    }
    
  err = fetch_indir_spec (np, lblkno (sblock, address), indirs);
  if (err)
    {
      rwlock_writer_unlock (&dn->allocptrlock);
      return EIO;
    }

  err = diskfs_catch_exception ();
  if (err)
    {
      rwlock_writer_unlock (&dn->allocptrlock);
      return EIO;
    }

  /* See if we need a triple indirect block; fail if we do. */
  assert (indirs[0].offset == -1 
	  || indirs[1].offset == -1 
	  || indirs[2].offset == -1);
  
  /* Check to see if this block is allocated. */
  if (indirs[0].bno == 0)
    {
      if (indirs[0].offset == -1)
	{
	  err = ffs_alloc (np, lblkno (sblock, address),
			   ffs_blkpref (np, lblkno (sblock, address),
					lblkno (sblock, address), di->di_db),
			   sblock->fs_bsize, &bno, 0);
	  if (err)
	    goto out;
	  assert (lblkno (sblock, address) < NDADDR);
	  indirs[0].bno = di->di_db[lblkno (sblock, address)] = bno;
	  record_poke (di, sizeof (struct dinode));
	}
      else
	{
	  daddr_t *siblock;
	  
	  /* We need to set siblock to the single indirect block
	     array; see if the single indirect block is allocated. */
	  if (indirs[1].bno == 0)
	    {
	      if (indirs[1].offset == -1)
		{
		  err = ffs_alloc (np, lblkno (sblock, address),
				   ffs_blkpref (np, lblkno (sblock, address),
						INDIR_SINGLE, di->di_ib),
				   sblock->fs_bsize, &bno, 0);
		  if (err)
		    goto out;
		  zero_disk_block (bno);
		  indirs[1].bno = di->di_ib[INDIR_SINGLE] = bno;
		  record_poke (di, sizeof (struct dinode));
		}
	      else
		{
		  daddr_t *diblock;
	      
		  /* We need to set diblock to the double indirect
		     block array; see if the double indirect block is
		     allocated. */
		  if (indirs[2].bno == 0)
		    {
		      /* This assert because triple indirection is
			 not supported. */
		      assert (indirs[2].offset == -1);
		      
		      err = ffs_alloc (np, lblkno (sblock, address),
				       ffs_blkpref (np, lblkno (sblock,
								address),
						    INDIR_DOUBLE, di->di_ib),
				       sblock->fs_bsize, &bno, 0);
		      if (err)
			goto out;
		      zero_disk_block (bno);
		      indirs[2].bno = di->di_ib[INDIR_DOUBLE] = bno;
		      record_poke (di, sizeof (struct dinode));
		    }

		  diblock = indir_block (indirs[2].bno);
		  mark_indir_dirty (np, indirs[2].bno);
		  
		  /* Now we can allocate the single indirect block */
		  
		  err = ffs_alloc (np, lblkno (sblock, address),
				   ffs_blkpref (np, lblkno (sblock, address),
						indirs[1].offset, diblock),
				   sblock->fs_bsize, &bno, 0);
		  if (err)
		    goto out;
		  zero_disk_block (bno);
		  indirs[1].bno = diblock[indirs[1].offset] = bno;
		  record_poke (diblock, sblock->fs_bsize);
		}
	    }
	  
	  siblock = indir_block (indirs[1].bno);
	  mark_indir_dirty (np, indirs[1].bno);

	  /* Now we can allocate the data block. */

	  err = ffs_alloc (np, lblkno (sblock, address),
			   ffs_blkpref (np, lblkno (sblock, address),
					indirs[0].offset, siblock),
			   sblock->fs_bsize, &bno, 0);
	  if (err)
	    goto out;

	  dev_write_sync (fsbtodb (sblock, bno), zeroblock, sblock->fs_bsize);

	  indirs[0].bno = siblock[indirs[0].offset] = bno;
	  record_poke (siblock, sblock->fs_bsize);
	}
    }
  
 out:
  diskfs_end_catch_exception ();
  rwlock_writer_unlock (&dn->allocptrlock);
  return err;
}

/* Implement the pager_report_extent callback from the pager library.  See 
   <hurd/pager.h> for the interface description. */
inline error_t
pager_report_extent (struct user_pager_info *pager,
		     vm_address_t *offset,
		     vm_size_t *size)
{
  assert (pager->type == DISK || pager->type == FILE_DATA);

  *offset = 0;

  if (pager->type == DISK)
    *size = diskpagersize;
  else
    *size = pager->np->allocsize;
  
  return 0;
}

/* Implement the pager_clear_user_data callback from the pager library.
   See <hurd/pager.h> for the interface description. */
void
pager_clear_user_data (struct user_pager_info *upi)
{
  /* XXX Do the right thing for the disk pager here too. */
  if (upi->type == FILE_DATA)
    {
      spin_lock (&node2pagelock);
      if (upi->np->dn->fileinfo == upi)
	upi->np->dn->fileinfo = 0;
      spin_unlock (&node2pagelock);
      diskfs_nrele_light (upi->np);
    }
  free (upi);
}

void
pager_dropweak (struct user_pager_info *upi __attribute__ ((unused)))
{
}
    


static void
thread_function (any_t foo __attribute__ ((unused)))
{
  for (;;)
    ports_manage_port_operations_multithread (pager_bucket, pager_demuxer,
					      1000 * 60 * 2, 0,
					      1, MACH_PORT_NULL);
}

/* Create a the DISK pager, initializing DISKPAGER, and DISKPAGERPORT */
void
create_disk_pager ()
{
  pager_bucket = ports_create_bucket ();

  cthread_detach (cthread_fork ((cthread_fn_t) thread_function, (any_t) 0));
  
  diskpager = malloc (sizeof (struct user_pager_info));
  diskpager->type = DISK;
  diskpager->np = 0;
  diskpager->p = pager_create (diskpager, pager_bucket,
			       MAY_CACHE, MEMORY_OBJECT_COPY_NONE);
  diskpagerport = pager_get_port (diskpager->p);
  mach_port_insert_right (mach_task_self (), diskpagerport, diskpagerport,
			  MACH_MSG_TYPE_MAKE_SEND);
}  

/* This syncs a single file (NP) to disk.  Wait for all I/O to complete
   if WAIT is set.  NP->lock must be held.  */
void
diskfs_file_update (struct node *np,
		    int wait)
{
  struct dirty_indir *d, *tmp;
  struct user_pager_info *upi;

  spin_lock (&node2pagelock);
  upi = np->dn->fileinfo;
  if (upi)
    ports_port_ref (upi->p);
  spin_unlock (&node2pagelock);
  
  if (upi)
    {
      pager_sync (upi->p, wait);
      ports_port_deref (upi->p);
    }
  
  for (d = np->dn->dirty; d; d = tmp)
    {
      sync_disk_blocks (d->bno, sblock->fs_bsize, wait);
      tmp = d->next;
      free (d);
    }
  np->dn->dirty = 0;

  diskfs_node_update (np, wait);
}

/* Call this to create a FILE_DATA pager and return a send right.
   NP must be locked.  */
mach_port_t
diskfs_get_filemap (struct node *np)
{
  struct user_pager_info *upi;
  mach_port_t right;

  assert (S_ISDIR (np->dn_stat.st_mode)
	  || S_ISREG (np->dn_stat.st_mode)
	  || (S_ISLNK (np->dn_stat.st_mode)
	      && (!direct_symlink_extension 
		  || np->dn_stat.st_size >= sblock->fs_maxsymlinklen)));

  spin_lock (&node2pagelock);
  do
    if (!np->dn->fileinfo)
      {
	upi = malloc (sizeof (struct user_pager_info));
	upi->type = FILE_DATA;
	upi->np = np;
	diskfs_nref_light (np);
	upi->p = pager_create (upi, pager_bucket,
			       MAY_CACHE, MEMORY_OBJECT_COPY_DELAY);
	np->dn->fileinfo = upi;
	right = pager_get_port (np->dn->fileinfo->p);
	ports_port_deref (np->dn->fileinfo->p);
      }
    else
      {
	/* Because NP->dn->fileinfo->p is not a real reference,
	   this might be nearly deallocated.  If that's so, then
	   the port right will be null.  In that case, clear here
	   and loop.  The deallocation will complete separately. */
	right = pager_get_port (np->dn->fileinfo->p);
	if (right == MACH_PORT_NULL)
	  np->dn->fileinfo = 0;
      }
  while (right == MACH_PORT_NULL);

  spin_unlock (&node2pagelock);
  
  mach_port_insert_right (mach_task_self (), right, right,
			  MACH_MSG_TYPE_MAKE_SEND);

  return right;
} 

/* Call this when we should turn off caching so that unused memory object
   ports get freed.  */
void
drop_pager_softrefs (struct node *np)
{
  struct user_pager_info *upi;
  
  spin_lock (&node2pagelock);
  upi = np->dn->fileinfo;
  if (upi)
    ports_port_ref (upi->p);
  spin_unlock (&node2pagelock);

  if (MAY_CACHE && upi)
    pager_change_attributes (upi->p, 0, MEMORY_OBJECT_COPY_DELAY, 0);
  if (upi)
    ports_port_deref (upi->p);
}

/* Call this when we should turn on caching because it's no longer
   important for unused memory object ports to get freed.  */
void
allow_pager_softrefs (struct node *np)
{
  struct user_pager_info *upi;
  
  spin_lock (&node2pagelock);
  upi = np->dn->fileinfo;
  if (upi)
    ports_port_ref (upi->p);
  spin_unlock (&node2pagelock);
  
  if (MAY_CACHE && upi)
    pager_change_attributes (upi->p, 1, MEMORY_OBJECT_COPY_DELAY, 0);
  if (upi)
    ports_port_deref (upi->p);
}

/* Tell diskfs if there are pagers exported, and if none, then
   prevent any new ones from showing up.  */
int
diskfs_pager_users ()
{
  int npagers;
  
  error_t block_cache (void *arg)
    {
      struct pager *p = arg;
      
      pager_change_attributes (p, 0, MEMORY_OBJECT_COPY_DELAY, 1);
      return 0;
    }
  
  error_t enable_cache (void *arg)
    {
      struct pager *p = arg;
      struct user_pager_info *upi = pager_get_upi (p);
      
      pager_change_attributes (p, 1, MEMORY_OBJECT_COPY_DELAY, 0);

      /* It's possible that we didn't have caching on before, because
	 the user here is the only reference to the underlying node
	 (actually, that's quite likely inside this particular
	 routine), and if that node has no links.  So dinkle the node
	 ref counting scheme here, which will cause caching to be
	 turned off, if that's really necessary.  */
      if (upi->type == FILE_DATA)
	{
	  diskfs_nref (upi->np);
	  diskfs_nrele (upi->np);
	}

      return 0;
    }

  npagers = ports_count_bucket (pager_bucket);
  if (npagers <= 1)
    return 0;

  if (MAY_CACHE == 0)
    {
      ports_enable_bucket (pager_bucket);
      return 1;
    }
  
  /* Loop through the pagers and turn off caching one by one,
     synchronously.  That should cause termination of each pager. */
  ports_bucket_iterate (pager_bucket, block_cache);

  /* Give it a second; the kernel doesn't actually shutdown
     immediately.  XXX */
  sleep (1);
  
  npagers = ports_count_bucket (pager_bucket);
  if (npagers <= 1)
    return 0;
  
  /* Darn, there are actual honest users.  Turn caching back on,
     and return failure. */
  ports_bucket_iterate (pager_bucket, enable_cache);
  return 1;
}


/* Call this to find out the struct pager * corresponding to the
   FILE_DATA pager of inode IP.  This should be used *only* as a subsequent
   argument to register_memory_fault_area, and will be deleted when 
   the kernel interface is fixed.  NP must be locked.  */
struct pager *
diskfs_get_filemap_pager_struct (struct node *np)
{
  /* This is safe because fileinfo can't be cleared; there must be
     an active mapping for this to be called. */
  return np->dn->fileinfo->p;
}

/* Shutdown all the pagers. */
void
diskfs_shutdown_pager ()
{
  error_t shutdown_one (void *arg)
    {
      struct pager *p = arg;
      /* Make sure the disk pager is done last. */
      if (p != diskpager->p)
	pager_shutdown (p);
      return 0;
    }

  copy_sblock ();
  write_all_disknodes ();
  ports_bucket_iterate (pager_bucket, shutdown_one);
  pager_shutdown (diskpager->p);
}

/* Sync all the pagers. */
void
diskfs_sync_everything (int wait)
{
  error_t sync_one (void *arg)
    {
      struct pager *p = arg;
      /* Make sure the disk pager is done last. */
      if (p != diskpager->p)
	pager_sync (p, wait);
      return 0;
    }
  
  copy_sblock ();
  write_all_disknodes ();
  ports_bucket_iterate (pager_bucket, sync_one);
  sync_disk (wait);
}
  
