/* Process management
   Copyright (C) 1992, 1993, 1994, 1995 Free Software Foundation, Inc.

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

#include <mach.h>
#include <sys/types.h>
#include <errno.h>
#include <hurd/hurd_types.h>
#include <stdlib.h>
#include <string.h>
#include <mach/notify.h>
#include <sys/wait.h>
#include <mach/mig_errors.h>
#include <sys/resource.h>
#include <hurd/auth.h>
#include <assert.h>

#include "proc.h"
#include "process_S.h"
#include "ourmsg_U.h"
#include "proc_exc_S.h"
#include "proc_exc_U.h"
#include "proc_excrepl_U.h"
#include "proc_excrepl_S.h"

/* Create a new id structure with the given genuine uids and gids. */
static inline struct ids *
make_ids (uid_t *uids, int nuids, uid_t *gids, int ngids)
{
  struct ids *i;
  
  i = malloc (sizeof (struct ids));
  i->i_nuids = nuids;
  i->i_ngids = ngids;
  i->i_uids = malloc (sizeof (uid_t) * nuids);
  i->i_gids = malloc (sizeof (uid_t) * ngids);
  i->i_refcnt = 1;
  
  memcpy (i->i_uids, uids, sizeof (uid_t) * nuids);
  memcpy (i->i_gids, gids, sizeof (uid_t) * ngids);
  return i;
}

/* Free an id structure. */
static inline void
free_ids (struct ids *i)
{
  free (i->i_uids);
  free (i->i_gids);
  free (i);
}

/* Tell if process P has uid UID, or has root.  */
int
check_uid (struct proc *p, uid_t uid)
{
  int i;
  for (i = 0; i < p->p_id->i_nuids; i++)
    if (p->p_id->i_uids[i] == uid || p->p_id->i_uids[i] == 0)
      return 1;
  return 0;
}


/* Implement proc_reathenticate as described in <hurd/proc.defs>. */
kern_return_t
S_proc_reauthenticate (struct proc *p, mach_port_t rendport)
{
  error_t err;
  uid_t gubuf[50], aubuf[50], ggbuf[50], agbuf[50];
  uid_t *gen_uids, *aux_uids, *gen_gids, *aux_gids;
  u_int ngen_uids, naux_uids, ngen_gids, naux_gids;
  
  gen_uids = gubuf;
  aux_uids = aubuf;
  gen_gids = ggbuf;
  aux_gids = agbuf;
  
  ngen_uids = naux_uids = 50;
  ngen_gids = naux_gids = 50;

  err = auth_server_authenticate (authserver, p->p_reqport, 
				  MACH_MSG_TYPE_MAKE_SEND, 
				  rendport, MACH_MSG_TYPE_MOVE_SEND,
				  p->p_reqport, MACH_MSG_TYPE_MAKE_SEND,
				  &gen_uids, &ngen_uids,
				  &aux_uids, &naux_uids,
				  &gen_gids, &ngen_gids,
				  &aux_gids, &naux_gids);
  if (err)
    return err;

  if (!--p->p_id->i_refcnt)
    free_ids (p->p_id);
  p->p_id = make_ids (gen_uids, ngen_uids, gen_gids, ngen_gids);
  
  if (gen_uids != gubuf)
    vm_deallocate (mach_task_self (), (u_int) gen_uids,
		   ngen_uids * sizeof (uid_t));
  if (aux_uids != aubuf)
    vm_deallocate (mach_task_self (), (u_int) aux_uids,
		   naux_uids * sizeof (uid_t));
  if (gen_gids != ggbuf)
    vm_deallocate (mach_task_self (), (u_int) gen_gids,
		   ngen_gids * sizeof (uid_t));
  if (aux_gids != agbuf)
    vm_deallocate (mach_task_self (), (u_int) aux_gids,
		   naux_gids * sizeof (uid_t));

  return 0;
}

/* Implement proc_child as described in <hurd/proc.defs>. */
kern_return_t
S_proc_child (struct proc *parentp,
	      task_t childt)
{
  struct proc *childp = task_find (childt);

  if (!childp)
    return ESRCH;
  
  if (childp->p_parentset)
    return EBUSY;
  
  /* Process identification.
     Leave p_task and p_pid alone; all the rest comes from the
     new parent. */

  if (!--childp->p_login->l_refcnt)
    free (childp->p_login);
  childp->p_login = parentp->p_login;
  childp->p_login->l_refcnt++;

  childp->p_owner = parentp->p_owner;
  childp->p_noowner = parentp->p_noowner;

  if (!--childp->p_id->i_refcnt)
    free_ids (childp->p_id);
  childp->p_id = parentp->p_id;
  childp->p_id->i_refcnt++;

  /* Process hierarchy.  Remove from our current location 
     and place us under our new parent.  Sanity check to make sure
     parent is currently init. */
  assert (childp->p_parent == startup_proc);
  if (childp->p_sib)
    childp->p_sib->p_prevsib = childp->p_prevsib;
  *childp->p_prevsib = childp->p_sib;
  
  childp->p_parent = parentp;
  childp->p_sib = parentp->p_ochild;
  childp->p_prevsib = &parentp->p_ochild;
  if (parentp->p_ochild)
    parentp->p_ochild->p_prevsib = &childp->p_sib;
  parentp->p_ochild = childp;

  /* Process group structure. */  
  if (childp->p_pgrp != parentp->p_pgrp)
    {
      leave_pgrp (childp);
      childp->p_pgrp = parentp->p_pgrp;
      join_pgrp (childp);
      /* Not necessary to call newids ourself because join_pgrp does
	 it for us. */
    }
  else if (childp->p_msgport != MACH_PORT_NULL)
    nowait_msg_proc_newids (childp->p_msgport, childp->p_task, 
			    childp->p_parent->p_pid, childp->p_pgrp->pg_pgid,
			    !childp->p_pgrp->pg_orphcnt);
  childp->p_parentset = 1;
  return 0;
}

/* Implement proc_reassign as described in <hurd/proc.defs>. */
kern_return_t
S_proc_reassign (struct proc *p,
		 task_t newt)
{
  struct proc *stubp = task_find (newt);
  mach_port_t foo;

  if (!stubp)
    return ESRCH;
  
  if (stubp == p)
    return EINVAL;

  remove_proc_from_hash (p);

  task_terminate (p->p_task);
  mach_port_deallocate (mach_task_self (), p->p_task);
  p->p_task = newt;
  mach_port_request_notification (mach_task_self (), p->p_task,
				  MACH_NOTIFY_DEAD_NAME, 1, p->p_reqport,
				  MACH_MSG_TYPE_MAKE_SEND_ONCE, &foo);
  if (foo)
    mach_port_deallocate (mach_task_self (), foo);

  /* For security, we need use the request port from STUBP, and 
     not inherit this state. */
  mach_port_mod_refs (mach_task_self (), p->p_reqport,
		      MACH_PORT_RIGHT_RECEIVE, -1);
  p->p_reqport = stubp->p_reqport;
  mach_port_mod_refs (mach_task_self (), p->p_reqport,
		      MACH_PORT_RIGHT_RECEIVE, 1);

  /* Enqueued messages might refer to the old task port, so
     destroy them. */
  if (p->p_msgport != MACH_PORT_NULL)
    {
      mach_port_deallocate (mach_task_self (), p->p_msgport);
      p->p_msgport = MACH_PORT_NULL;
      p->p_deadmsg = 1;
    }

  /* These two are image dependent. */
  p->p_argv = stubp->p_argv;
  p->p_envp = stubp->p_envp;

  /* Destroy stubp */
  stubp->p_task = 0;		/* block deallocation */
  process_has_exited (stubp);
  
  add_proc_to_hash (p);

  return 0;
}

/* Implement proc_setowner as described in <hurd/proc.defs>. */
kern_return_t
S_proc_setowner (struct proc *p,
		 uid_t owner)
{
  if (! check_uid (p, owner))
    return EPERM;
  
  p->p_owner = owner;
  p->p_noowner = 0;
  return 0;
}

/* Implement proc_getpids as described in <hurd/proc.defs>. */
kern_return_t
S_proc_getpids (struct proc *p,
		pid_t *pid,
		pid_t *ppid,
		int *orphaned)
{
  *pid = p->p_pid;
  *ppid = p->p_parent->p_pid;
  *orphaned = !p->p_pgrp->pg_orphcnt;
  return 0;
}

/* Implement proc_set_arg_locations as described in <hurd/proc.defs>. */
kern_return_t
S_proc_set_arg_locations (struct proc *p,
			  vm_address_t argv,
			  vm_address_t envp)
{
  p->p_argv = argv;
  p->p_envp = envp;
  return 0;
}

/* Implement proc_get_arg_locations as described in <hurd/proc.defs>. */
kern_return_t
S_proc_get_arg_locations (struct proc *p,
			  vm_address_t *argv,
			  vm_address_t *envp)
{
  *argv = p->p_argv;
  *envp = p->p_envp;
  return 0;
}

/* Implement proc_dostop as described in <hurd/proc.defs>. */
kern_return_t
S_proc_dostop (struct proc *p,
	       thread_t contthread)
{
  thread_t threadbuf[2], *threads = threadbuf;
  unsigned int nthreads = 2, i;
  error_t err;

  err = task_suspend (p->p_task);
  if (err)
    return err;
  err = task_threads (p->p_task, &threads, &nthreads);
  if (err)
    return err;
  for (i = 0; i < nthreads; i++)
    {
      if (threads[i] != contthread)
	err = thread_suspend (threads[i]);
      mach_port_deallocate (mach_task_self (), threads[i]);
    }
  if (threads != threadbuf)
    vm_deallocate (mach_task_self (), (vm_address_t) threads,
		   nthreads * sizeof (thread_t));
  err = task_resume (p->p_task);
  if (err)
    return err;

  mach_port_deallocate (mach_task_self (), contthread);
  return 0;
}

/* Implement proc_handle_exceptions as described in <hurd/process.defs>. */
kern_return_t
S_proc_handle_exceptions (struct proc *p,
			  mach_port_t msgport,
			  mach_port_t forwardport,
			  int flavor,
			  thread_state_t new_state,
			  mach_msg_type_number_t statecnt)
{
  struct exc *e = malloc (sizeof (struct exc)
			  + (statecnt * sizeof (natural_t)));
  mach_port_t foo;
  
  mach_port_request_notification (mach_task_self (), msgport,
				  MACH_NOTIFY_NO_SENDERS, 1, msgport,
				  MACH_MSG_TYPE_MAKE_SEND_ONCE, &foo);
  if (foo)
    mach_port_deallocate (mach_task_self (), foo);

  mach_port_move_member (mach_task_self (), msgport, request_portset);
  e->excport = msgport;
  e->forwardport = forwardport;
  e->replyport = MACH_PORT_NULL;
  e->flavor = flavor;
  e->statecnt = statecnt;
  bcopy (new_state, e->thread_state, statecnt * sizeof (natural_t));
  add_exc_to_hash (e);
  return 0;
}

/* Called on exception ports provided to proc_handle_exceptions.  Do
   the thread_set_state requested by proc_handle_exceptions and then
   send an exception_raise message as requested. */
kern_return_t
S_proc_exception_raise (mach_port_t excport,
			mach_port_t reply,
			mach_msg_type_name_t replyname,
			mach_port_t thread,
			mach_port_t task,
			int exception,
			int code,
			int subcode)
{
  struct exc *e = exc_find (excport);
  if (!e)
    return EOPNOTSUPP;
  if (e->replyport != MACH_PORT_NULL)
    return EBUSY;  /* This is wrong, but too much trouble to fix now */
  thread_set_state (thread, e->flavor, e->thread_state, e->statecnt); 
  proc_exception_raise (e->forwardport, e->excport, 
			MACH_MSG_TYPE_MAKE_SEND_ONCE,
			thread, task, exception, code, subcode);
  mach_port_deallocate (mach_task_self (), thread);
  mach_port_deallocate (mach_task_self (), task);
  return MIG_NO_REPLY;
}

/* Called by proc_handle_exception clients after they have received
   the exception_raise we send in S_proc_exception_raise.  Reply to
   the agent that generated the exception raise. */
kern_return_t
S_proc_exception_raise_reply (mach_port_t excport,
			      int replycode)
{
  struct exc *e = exc_find (excport);
  if (!e)
    return EOPNOTSUPP;
  if (e->replyport == MACH_PORT_NULL)
    return 0;
  proc_exception_raise_reply (e->replyport, e->replyporttype, replycode);
  return MIG_NO_REPLY;
}

/* Implement proc_getallpids as described in <hurd/proc.defs>. */
kern_return_t
S_proc_getallpids (struct proc *p,
		   pid_t **pids,
		   u_int *pidslen)
{
  int nprocs;
  pid_t *loc;
  
  void count_up (struct proc *p, void *counter)
    {
      ++*(int *)counter;
    }
  void store_pid (struct proc *p, void *loc)
    {
      *(*(pid_t **)loc)++ = p->p_pid;
    }

  add_tasks (0);
  
  nprocs = 0;
  prociterate (count_up, &nprocs);

  if (nprocs > *pidslen)
    vm_allocate (mach_task_self (), (vm_address_t *) pids,
		 nprocs * sizeof (pid_t), 1);

  loc = *pids;
  prociterate (store_pid, &loc);

  *pidslen = nprocs;
  return 0;
}

/* Create a process for TASK, which is not otherwise known to us.
   The task will be placed as a child of init and in init's pgrp. */
struct proc *
new_proc (task_t task)
{
  struct proc *p;
  mach_port_t foo;
  
  /* Because these have a reference count of one before starting,
     they can never be freed, so we're safe. */
  static struct login *nulllogin;
  static struct ids nullids = {0, 0, 0, 0, 1};
  
  if (!nulllogin)
    {
      nulllogin = malloc (sizeof (struct login) + 7);
      nulllogin->l_refcnt = 1;
      strcpy (nulllogin->l_name, "<none>");
    }

  /* Pid 0 is us; pid 1 is init.  We handle those here specially;
     all other processes inherit from init here (though proc_child
     will move them to their actual parent usually).  */

  p = malloc (sizeof (struct proc));
  mach_port_allocate (mach_task_self (), MACH_PORT_RIGHT_RECEIVE,
		      &p->p_reqport);
  mach_port_move_member (mach_task_self (), p->p_reqport, 
			 request_portset);

  p->p_pid = genpid ();
  p->p_task = task;
  mach_port_request_notification (mach_task_self (), p->p_task,
				  MACH_NOTIFY_DEAD_NAME, 1, p->p_reqport,
				  MACH_MSG_TYPE_MAKE_SEND_ONCE, &foo);
  if (foo != MACH_PORT_NULL)
    mach_port_deallocate (mach_task_self (), foo);
				  
  
  switch (p->p_pid)
    {
    case 0:
      p->p_login = malloc (sizeof (struct login) + 5);
      p->p_login->l_refcnt = 1;
      strcpy (p->p_login->l_name, "root");
      break;
      
    case 1:
      p->p_login = self_proc->p_login;
      p->p_login->l_refcnt++;
      break;

    default:
      p->p_login = nulllogin;
      p->p_login->l_refcnt++;
    }
  
  p->p_owner = 0;
      
  if (p->p_pid == 0)
    {
      uid_t foo = 0;
      p->p_id = make_ids (&foo, 1, &foo, 1);
      p->p_parent = p;
      p->p_sib = 0;
      p->p_prevsib = &p->p_ochild;
      p->p_ochild = p;
      p->p_loginleader = 1;
      p->p_parentset = 1;
    }
  else if (p->p_pid == 1)
    {
      p->p_id = self_proc->p_id;
      p->p_id->i_refcnt++;
      p->p_parent = self_proc;

      p->p_sib = self_proc->p_ochild;
      p->p_prevsib = &self_proc->p_ochild;
      if (p->p_sib)
	p->p_sib->p_prevsib = &p->p_sib;
      self_proc->p_ochild = p;
      p->p_loginleader = 1;
      p->p_ochild = 0;
      p->p_parentset = 1;
    }
  else
    {
      p->p_id = &nullids;
      p->p_id->i_refcnt++;

      /* Our parent is init for now */
      p->p_parent = startup_proc;

      p->p_sib = startup_proc->p_ochild;
      p->p_prevsib = &startup_proc->p_ochild;
      if (p->p_sib)
	p->p_sib->p_prevsib = &p->p_sib;
      startup_proc->p_ochild = p;
      p->p_loginleader = 0;
      p->p_ochild = 0;
      p->p_parentset = 0;
    }
  
  if (p->p_pid < 2)
    boot_setsid (p);
  else
    p->p_pgrp = startup_proc->p_pgrp;
 
  p->p_msgport = MACH_PORT_NULL;
  
  p->p_argv = p->p_envp = p->p_status = 0;
  
  p->p_exec = 0;
  p->p_stopped = 0;
  p->p_waited = 0;
  p->p_exiting = 0;
  p->p_waiting = 0;
  p->p_traced = 0;
  p->p_nostopcld = 0;
  p->p_deadmsg = (p->p_pid == 1);
  p->p_checkmsghangs = 0;
  p->p_msgportwait = 0;
  p->p_noowner = 1;

  if (p->p_pid > 1)
    {
      add_proc_to_hash (p);
      join_pgrp (p);
    }

  return p;
}

/* The task associated with process P has died.  Free everything, and
   record our presence in the zombie table, then return wait if necessary. */
void
process_has_exited (struct proc *p)
{
  alert_parent (p);

  prociterate ((void (*) (struct proc *, void *))check_message_dying, p);
  
  mach_port_mod_refs (mach_task_self (), p->p_reqport, 
		      MACH_PORT_RIGHT_RECEIVE, -1);

  remove_proc_from_hash (p);
  
  mach_port_deallocate (mach_task_self (), p->p_task);

  if (!--p->p_login->l_refcnt)
    free (p->p_login);
  
  if (!--p->p_id->i_refcnt)
    free_ids (p->p_id);
  
  /* Reparent our children to init by attaching the head and tail 
     of our list onto init's. */
  if (p->p_ochild)
    {
      struct proc *tp;		/* will point to the last one */
      
      /* first tell them their parent is changing */
      for (tp = p->p_ochild; tp->p_sib; tp = tp->p_sib)
	{
	  if (tp->p_msgport != MACH_PORT_NULL)
	    nowait_msg_proc_newids (tp->p_msgport, tp->p_task,
				    1, tp->p_pgrp->pg_pgid,
				    !tp->p_pgrp->pg_orphcnt);
	  tp->p_parent = startup_proc;
	}
      if (tp->p_msgport != MACH_PORT_NULL)
	nowait_msg_proc_newids (tp->p_msgport, tp->p_task,
				1, tp->p_pgrp->pg_pgid,
				!tp->p_pgrp->pg_orphcnt);
      tp->p_parent = startup_proc;

      /* And now nappend the lists. */
      tp->p_sib = startup_proc->p_ochild;
      if (tp->p_sib)
	tp->p_sib->p_prevsib = &tp->p_sib;
      startup_proc->p_ochild = p->p_ochild;
      p->p_ochild->p_prevsib = &startup_proc->p_ochild;
    }

  reparent_zombies (p);
  
  /* Remove us from our parent's list of children. */
  if (p->p_sib)
    p->p_sib->p_prevsib = p->p_prevsib;
  *p->p_prevsib = p->p_sib;
  
  leave_pgrp (p);
  
  mach_port_deallocate (mach_task_self (), p->p_msgport);
  
  if (p->p_waiting)
    mach_port_deallocate (mach_task_self (), 
			  p->p_continuation.wait_c.reply_port);
  if (p->p_msgportwait)
    mach_port_deallocate (mach_task_self (),
			  p->p_continuation.getmsgport_c.reply_port);
  
  free (p);
}


/* Get the list of all tasks from the kernel and start adding them.
   If we encounter TASK, then don't do any more and return its proc.
   If TASK is null or we never find it, then return 0. */
struct proc *
add_tasks (task_t task)
{
  mach_port_t *psets;
  u_int npsets;
  int i;
  struct proc *foundp = 0;

  host_processor_sets (mach_host_self (), &psets, &npsets);
  for (i = 0; i < npsets; i++)
    {
      mach_port_t psetpriv;
      mach_port_t *tasks;
      u_int ntasks;
      int j;

      if (!foundp)
	{
	  host_processor_set_priv (master_host_port, psets[i], &psetpriv);
	  processor_set_tasks (psetpriv, &tasks, &ntasks);
	  for (j = 0; j < ntasks; j++)
	    {
	      int set = 0;
	      if (!foundp)
		{
		  struct proc *p = task_find_nocreate (tasks[j]);
		  if (!p)
		    {
		      p = new_proc (tasks[j]);
		      set = 1;
		    }
		  if (!foundp && tasks[j] == task)
		    foundp = p;
		}
	      if (!set)
		mach_port_deallocate (mach_task_self (), tasks[j]);
	    }
	  vm_deallocate (mach_task_self (), (vm_address_t) tasks,
			 ntasks * sizeof (task_t));
	  mach_port_deallocate (mach_task_self (), psetpriv);
	}
      mach_port_deallocate (mach_task_self (), psets[i]);
    }
  vm_deallocate (mach_host_self (), (vm_address_t) psets, 
		 npsets * sizeof (mach_port_t));
  return foundp;
}

/* Allocate a new pid.  The first two times this is called it must return
   0 and 1 in order; after that it must simply return an unused pid. 
   (Unused means it is neither the pid nor pgrp of any relevant data.) */
int
genpid ()
{
#define WRAP_AROUND 30000  
#define START_OVER 100
  static int nextpid = 0;
  static int wrap = WRAP_AROUND;

  int wrapped = 0;

  while (!pidfree (nextpid))
    {
      ++nextpid;
      if (nextpid > wrap)
	{
	  if (wrapped)
	    {
	      wrap *= 2;
	      wrapped = 0;
	    }
	  else
	    {
	      nextpid = START_OVER;
	      wrapped = 1;
	    }
	}
    }

  return nextpid++;
}
