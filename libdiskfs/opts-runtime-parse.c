/* A default diskfs_parse_runtime_options routine

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

#include "priv.h"

error_t
diskfs_parse_runtime_options (int argc, char **argv,
			      const struct argp *standard_argp)
{
  return argp_parse (standard_argp, argc, argv,
		     ARGP_NO_ERRS | ARGP_NO_HELP | ARGP_PARSE_ARGV0,
		     0);
}
