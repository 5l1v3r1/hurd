/* The type ps_context_t, for per-procserver and somewhat global state.

   Copyright (C) 1995 Free Software Foundation, Inc.

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
#include <assert.h>
#include <hurd/term.h>

#include "ps.h"
#include "common.h"

/* ---------------------------------------------------------------- */

/* Returns in PC a new ps_context_t for the proc server SERVER.  If a memory
   allocation error occurs, ENOMEM is returned, otherwise 0.  */
error_t
ps_context_create(process_t server, ps_context_t *pc)
{
  error_t err_procs, err_ttys, err_ttys_by_cttyid, err_users;

  *pc = NEW(struct ps_context);
  if (*pc == NULL)
    return ENOMEM;

  (*pc)->server = server;
  (*pc)->user_hooks = 0;
  err_procs = ihash_create(&(*pc)->procs);
  err_ttys = ihash_create(&(*pc)->ttys);
  err_ttys_by_cttyid = ihash_create(&(*pc)->ttys_by_cttyid);
  err_users = ihash_create(&(*pc)->users);

  if (err_procs || err_ttys || err_ttys_by_cttyid)
    /* Some allocation error occurred, backout any successful ones and fail. */
    {
      if (!err_procs) ihash_free((*pc)->procs);
      if (!err_users) ihash_free((*pc)->users);
      if (!err_ttys)  ihash_free((*pc)->ttys);
      if (!err_ttys_by_cttyid) ihash_free((*pc)->ttys_by_cttyid);
      free(*pc);
      return ENOMEM;
    }

  ihash_set_cleanup((*pc)->procs,
		    (void (*)(void *, void *arg))_proc_stat_free,
		    NULL);
  ihash_set_cleanup((*pc)->ttys,
		    (void (*)(void *, void *arg))ps_tty_free,
		    NULL);
  ihash_set_cleanup((*pc)->users,
		    (void (*)(void *, void *arg))ps_user_free,
		    NULL);

  return 0;
}

/* Frees PC and any resources it consumes.  */
void
ps_context_free(ps_context_t pc)
{
  ihash_free(pc->procs);
  ihash_free(pc->procs);
  free(pc);
}

/* ---------------------------------------------------------------- */

/* Return the value in HT indexed by the key ID.  If it doesn't exist create
   it by calling CREATE with ID and a return location pointer as arguments
   (CREATE should return either an error-code or 0 if no error occurs), and
   cache it in HT.  */
static error_t
lookup(int id, ihash_t ht, error_t (*create)(int id, void **), void **value)
{
  *value = ihash_find(ht, id);
  if (*value == NULL)
    {
      error_t err = create(id, value);
      if (err)
	return err;
      ihash_add(ht, id, *value, NULL);
    }
  return 0;
}

/* Find a proc_stat_t for the process referred to by PID, and return it in
   PS.  If an error occurs, it is returned, otherwise 0.  */
error_t
ps_context_find_proc_stat(ps_context_t pc, pid_t pid, proc_stat_t *ps)
{
  error_t create(int pid, void **value)
    {
      return _proc_stat_create(pid, pc, (proc_stat_t *)value);
    }
  return lookup(pid, pc->procs, create, (void **)ps);
}

/* Find a ps_tty_t for the terminal referred to by the port TTY_PORT, and
   return it in TTY.  If an error occurs, it is returned, otherwise 0.  */
error_t
ps_context_find_tty(ps_context_t pc, mach_port_t tty_port, ps_tty_t *tty)
{
  return lookup(tty_port,
		pc->ttys,
		(error_t (*)(int id, void **result))ps_tty_create,
		(void **)tty);
}

/* Find a ps_tty_t for the terminal referred to by the ctty id port
   CTTYID_PORT, and return it in TTY.  If an error occurs, it is returned,
   otherwise 0.  */
error_t
ps_context_find_tty_by_cttyid(ps_context_t pc, mach_port_t cttyid_port, ps_tty_t *tty)
{
  error_t create(int cttyid_port, void **value)
    {
      if (cttyid_port == MACH_PORT_NULL)
	{
	  *value = 0;
	  return 0;
	}
      else
	{
	  int tty_port;
	  error_t err = termctty_open_terminal(cttyid_port, 0, &tty_port);
	  if (err)
	    return err;
	  else
	    return ps_context_find_tty(pc, tty_port, (ps_tty_t *)value);
	}
    }

  return lookup(cttyid_port, pc->ttys, create, (void **)tty);
}

/* Find a ps_user_t for the user referred to by UID, and return it in U.  */
error_t
ps_context_find_user(ps_context_t pc, uid_t uid, ps_user_t *u)
{
  return lookup(uid,
		pc->users,
		(error_t (*)(int id, void **result))ps_user_create,
		(void **)u);
}
