/* Show files' passive translators.

   Copyright (C) 1995, 1996 Free Software Foundation, Inc.

   Written by Miles Bader <miles@gnu.ai.mit.edu>

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

#include <hurd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <argp.h>
#include <fcntl.h>
#include <unistd.h>

#include <error.h>
#include <argz.h>

static struct argp_option options[] =
{
  {"prefix",    'p', 0, 0, "always display `FILENAME: ' before translators"},
  {"no-prefix", 'P', 0, 0, "never display `FILENAME: ' before translators"},
  {"silent",    's', 0, 0, "no output; useful when checking error status"},
  {"quiet",     'q', 0, OPTION_ALIAS | OPTION_HIDDEN},
  {0, 0}
};

static char *args_doc = "FILE...";

static char *doc = "If there are no args, the translator on the node attached \
to standard input is printed.  A FILE argument of `-' also does this.";

/* ---------------------------------------------------------------- */

void 
main (int argc, char *argv[])
{
  /* The default exit status -- changed to 0 if we find any translators.  */
  int status = 1;
  /* Some option flags.  -1 for PRINT_PREFIX means use the default.  */
  int print_prefix = -1, silent = 0;

  /* If NODE is MACH_PORT_NULL, prints an error message and exits, otherwise
     prints the translator on NODE, possibly prefixed by `NAME:', and
     deallocates NODE.  */
  void print_node_trans (file_t node, char *name)
    {
      if (node == MACH_PORT_NULL)
	error (0, errno, "%s", name);
      else
	{
	  char buf[1024], *trans = buf;
	  int trans_len = sizeof (buf);
	  error_t err = file_get_translator (node, &trans, &trans_len);

	  switch (err)
	    {
	    case 0:
	      /* Make the '\0's in TRANS printable.  */
	      argz_stringify (trans, trans_len, ' ');

	      if (!silent)
		if (print_prefix)
		  printf ("%s: %s\n", name, trans);
		else
		  puts (trans);

	      if (trans != buf)
		vm_deallocate (mach_task_self (),
			       (vm_address_t)trans, trans_len);

	      status = 0;

	      break;

	    case EINVAL:
	      /* NODE just doesn't have a translator.  */
	      if (!silent && print_prefix)
		puts (name);
	      break;

	    default:
	      error (0, err, "%s", name);
	    }

	  mach_port_deallocate (mach_task_self (), node);
	}
    }

  /* Parse a command line option.  */
  error_t parse_opt (int key, char *arg, struct argp_state *state)
    {
      switch (key)
	{
	case ARGP_KEY_NO_ARGS:	/* The end of the argument list */
	case ARGP_KEY_ARG:	/* A FILE argument */
	  if (print_prefix < 0)
	    /* By default, only print a prefix if there are multiple files. */
	    print_prefix = state->next < state->argc;

	  if (arg && strcmp (arg, "-") != 0)
	    print_node_trans (file_name_lookup (arg, O_NOTRANS, 0), arg);
	  else
	    print_node_trans (getdport (0), "-");
	  break;

	  /* Options. */
	case 'p': print_prefix = 1; break;
	case 'P': print_prefix = 0; break;
	case 's': case 'q': silent = 1; break;

	default:  return EINVAL;
	}
      return 0;
    }

  struct argp argp = {options, parse_opt, args_doc, doc};

  argp_parse (&argp, argc, argv, 0, 0, 0);

  exit (status);
}
