/* Parse run-time options

   Copyright (C) 1995, 1996 Free Software Foundation, Inc.

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

#include "netfs.h"

static const struct argp_option
std_runtime_options[] =
{
  {0, 0}
};

static error_t
parse_runtime_opt (int key, char *arg, struct argp_state *state)
{
  return EINVAL;
}

error_t
netfs_set_options (int argc, char **argv)
{
  const struct argp argp = { std_runtime_options, parse_runtime_opt, 0, 0, 0 };

  /* Call the user option parsing routine, giving it our set of options to do
     with as it pleases.  */
  return netfs_parse_runtime_options (argc, argv, &argp);
}
