/*
   Copyright (C) 1994 Free Software Foundation

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

/* If NP->dn_set_ctime is set, then modify NP->dn_stat.st_ctime
   appropriately; do the analogous operation for atime and mtime as well. */
void
diskfs_set_node_times (struct node *np)
{
  int secs, usecs, ret;
  
  do 
    {
      secs = diskfs_mtime->seconds;
      usecs = diskfs_mtime->microseconds;
    }
  while (secs != diskfs_mtime->check_seconds);
  
  if (np->dn_set_mtime)
    {
      np->dn_stat.st_mtime = secs;
      np->dn_stat.st_mtime_usec = usecs;
    }
  if (np->dn_set_atime)
    {
      np->dn_stat.st_atime = secs;
      np->dn_stat.st_atime_usec = usecs;
    }
  if (np->dn_set_ctime)
    {
      np->dn_stat.st_ctime = secs;
      np->dn_stat.st_ctime_usec = usecs;
    }
  
  if (np->dn_set_mtime || np->dn_set_atime || np->dn_set_ctime)
    np->dn_stat_dirty = 1;
  np->dn_set_mtime = np->dn_set_atime = np->dn_set_ctime = 0;
}
