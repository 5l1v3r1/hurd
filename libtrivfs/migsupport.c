/* 
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

#include "priv.h"

struct trivfs_protid *
trivfs_begin_using_protid (mach_port_t port)
{
  if (trivfs_protid_nportclasses > 1)
    {
      struct port_info *pi = ports_lookup_port (0, port, 0);
      int i;
      for (i = 0; i < trivfs_protid_nportclasses; i++)
	if (pi->class == trivfs_protid_portclasses[i])
	  return (struct trivfs_protid *) pi;
      ports_port_deref ((void *)port);
      return 0;
    }
  else
    return ports_lookup_port (0, port, trivfs_protid_portclasses[0]);
}

void 
trivfs_end_using_protid (struct trivfs_protid *cred)
{
  ports_port_deref (cred);
}

struct trivfs_control *
trivfs_begin_using_control (mach_port_t port)
{
  if (trivfs_cntl_nportclasses > 1)
    {
      struct port_info *pi = ports_lookup_port (0, port, 0);
      int i;
      for (i = 0; i < trivfs_cntl_nportclasses; i++)
	if (pi->class == trivfs_cntl_portclasses[i])
	  return (struct trivfs_control *) pi;
      ports_port_deref ((void *)port);
      return 0;
    }
  else
    return ports_lookup_port (0, port, trivfs_cntl_portclasses[0]);
}

void 
trivfs_end_using_control (struct trivfs_control *cred)
{
  ports_port_deref (cred);
}
