/* Create a new port structure

   Copyright (C) 1995, 1996 Free Software Foundation, Inc.
   Written by Michael I. Bushnell.

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
   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA. */

#include "ports.h"
#include <assert.h>
#include <cthreads.h>
#include <hurd/ihash.h>

/* Create and return in RESULT a new port in CLASS and BUCKET; SIZE bytes
   will be allocated to hold the port structure and whatever private data the
   user desires.  */
error_t
ports_create_port (struct port_class *class, struct port_bucket *bucket,
		   size_t size, void *result)
{
  mach_port_t port;
  error_t err;
  struct port_info *pi;

  err = mach_port_allocate (mach_task_self (), MACH_PORT_RIGHT_RECEIVE,
			    &port);
  if (err)
    return err;

  if (size < sizeof (struct port_info))
    size = sizeof (struct port_info);

  pi = malloc (size);
  if (! pi)
    {
      mach_port_deallocate (mach_task_self (), port);
      return ENOMEM;
    }

  pi->class = class;
  pi->refcnt = 1;
  pi->weakrefcnt = 0;
  pi->cancel_threshold = 0;
  pi->mscount = 0;
  pi->flags = 0;
  pi->port_right = port;
  pi->current_rpcs = 0;
  pi->bucket = bucket;
  
  mutex_lock (&_ports_lock);
  
 loop:
  if (class->flags & PORT_CLASS_NO_ALLOC)
    { 
      class->flags |= PORT_CLASS_ALLOC_WAIT;
      if (hurd_condition_wait (&_ports_block, &_ports_lock))
	goto cancelled;
      goto loop;
    }
  if (bucket->flags & PORT_BUCKET_NO_ALLOC)
    {
      bucket->flags |= PORT_BUCKET_ALLOC_WAIT;
      if (hurd_condition_wait (&_ports_block, &_ports_lock))
	goto cancelled;
      goto loop;
    }

  err = ihash_add (bucket->htable, port, pi, &pi->hentry);
  if (err)
    goto lose;

  pi->next = class->ports;
  pi->prevp = &class->ports;
  if (class->ports)
    class->ports->prevp = &pi->next;
  class->ports = pi;
  bucket->count++;
  class->count++;
  mutex_unlock (&_ports_lock);
  
  mach_port_move_member (mach_task_self (), pi->port_right, bucket->portset);

  *(void **)result = pi;
  return 0;

 cancelled:
  err = EINTR;
 lose:
  mutex_unlock (&_ports_lock);
  mach_port_mod_refs (mach_task_self (), port, MACH_PORT_RIGHT_RECEIVE, -1);
  free (pi);

  return err;
}
