/* 
   Copyright (C) 1995 Free Software Foundation, Inc.
   Written by Miles Bader and Michael I. Bushnell.

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

#include <hurd.h>
#include <mach/notify.h>
#include <mach.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <assert.h>
#include "fshelp.h"


/* The data passed in the various messages we're interested in.  */
struct fsys_startup_request
{
  mach_msg_header_t head;
  mach_msg_type_t control_portType;
  mach_port_t control_port;
};

struct fsys_startup_reply
{
  mach_msg_header_t head;
  mach_msg_type_t RetCodeType;
  kern_return_t RetCode;
  mach_msg_type_t realnodeType;
  mach_port_t realnode;
};

static const mach_msg_type_t control_portCheck =
{
  17,				/* msgt_name = */
  32,				/* msgt_size = */
  1,				/* msgt_number = */
  TRUE,				/* msgt_inline = */
  FALSE,			/* msgt_longform = */
  FALSE,			/* msgt_deallocate = */
  0				/* msgt_unused = */
};

static const mach_msg_type_t RetCodeType = {
  MACH_MSG_TYPE_INTEGER_32,	/* msgt_name = */
  32,				/* msgt_size = */
  1,				/* msgt_number = */
  TRUE,				/* msgt_inline = */
  FALSE,			/* msgt_longform = */
  FALSE,			/* msgt_deallocate = */
  0				/* msgt_unused = */
};

static const mach_msg_type_t realnodeType =
{
  -1,				/* msgt_name = */
  32,				/* msgt_size = */
  1,				/* msgt_number = */
  TRUE,				/* msgt_inline = */
  FALSE,			/* msgt_longform = */
  FALSE,			/* msgt_deallocate = */
  0				/* msgt_unused = */
};


/* Wait around for an fsys_startup message on the port PORT from the
   translator on NODE (timing out after TIMEOUT milliseconds), and return a
   send right for the resulting fsys control port in CONTROL.  If a dead-name
   notification is received on PORT, then it will be assumed that the
   translator died, and EDIED will be returned.  If an error occurs, the
   error code is returned, otherwise 0.  */
static error_t
service_fsys_startup(file_t node, mach_msg_type_name_t node_type,
		     mach_port_t port, long timeout,
		     fsys_t *control)
{
  error_t err;
  union
    {
      mach_msg_header_t head;
      struct fsys_startup_request startup;
      mach_dead_name_notification_t dead;
    }
  request;
  struct fsys_startup_reply reply;

  /* Wait for the fsys_startup message...  */
  err = mach_msg(&request.head, (MACH_RCV_MSG 
				 | (timeout ? MACH_RCV_TIMEOUT : 0)),
		 0, sizeof(request), port, timeout, MACH_PORT_NULL);
  if (err)
    return err;

  /* Check whether we actually got a dead-name notification instead.  */
  if (request.head.msgh_id == MACH_NOTIFY_NO_SENDERS)
    return EDIED;

  /* Construct our reply to the fsys_startup rpc.  */
  reply.head.msgh_size = sizeof(reply);
  reply.head.msgh_bits =
    MACH_MSGH_BITS(MACH_MSGH_BITS_REMOTE(request.head.msgh_bits), 0);
  reply.head.msgh_remote_port = request.head.msgh_remote_port;
  reply.head.msgh_local_port = MACH_PORT_NULL;
  reply.head.msgh_seqno = 0;
  reply.head.msgh_id = request.head.msgh_id + 100;
  reply.RetCodeType = RetCodeType;

  if (request.head.msgh_id != 22000)
    reply.RetCode = MIG_BAD_ID;
  else if (*(int *)&request.startup.control_portType
	   != *(int *)&control_portCheck)
    reply.RetCode = MIG_BAD_ARGUMENTS;
  else
    {
      *control = request.startup.control_port;

      reply.RetCode = KERN_SUCCESS;
      reply.realnodeType = realnodeType;

      reply.realnode = node;
      reply.realnodeType.msgt_name = node_type;
      if (node != MACH_PORT_NULL)
	/* The message can't be simple because of the port.  */
	reply.head.msgh_bits |= MACH_MSGH_BITS_COMPLEX;
    }

  err = mach_msg(&reply.head, MACH_SEND_MSG, sizeof(reply), 0,
		 request.head.msgh_remote_port,
		 MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);

  if (reply.RetCode)
    /* Make our error return be the earlier one.  */
    err = reply.RetCode;

  return err;
}


error_t
fshelp_start_translator_long (file_t underlying, 
			      mach_msg_type_name_t underlying_type,
			      char *name, char *argz, int argz_len,
			      mach_port_t *fds, 
			      mach_msg_type_name_t fds_type, int fds_len,
			      mach_port_t *ports,
			      mach_msg_type_name_t ports_type, int ports_len,
			      int *ints, int ints_len,
			      int timeout, fsys_t *control)
{
  error_t err;
  file_t executable;
  mach_port_t bootstrap, task, prev_notify, proc;
  int ports_moved = 0;

  /* Find the translator itself.  Since argz has zero-separated elements, we
     can use it as a normal string representing the first element.  */
  executable = file_name_lookup(name, O_EXEC, 0);
  if (executable == MACH_PORT_NULL)
    return errno;
  
  /* Create a bootstrap port for the translator.  */
  err =
    mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &bootstrap);
  if (err)
    goto lose;

  /* Create the task for the translator.  */
  err = task_create (mach_task_self (), 0, &task);
  if (err)
    goto lose;

  /* Designate TASK as our child.  */
  proc = getproc ();
  proc_child (proc, task);
  mach_port_deallocate (mach_task_self (), proc);
  
  assert (ports_len > INIT_PORT_BOOTSTRAP);
  switch (ports_type)
    {
    case MACH_MSG_TYPE_MAKE_SEND:
    case MACH_MSG_TYPE_MAKE_SEND_ONCE:
      break;

    case MACH_MSG_TYPE_MOVE_SEND:
      if (ports[INIT_PORT_BOOTSTRAP] != MACH_PORT_NULL)
	mach_port_deallocate (mach_task_self (), ports[INIT_PORT_BOOTSTRAP]);
      mach_port_insert_right (mach_task_self (), bootstrap, bootstrap,
			      MACH_MSG_TYPE_MAKE_SEND);
      break;

    case MACH_MSG_TYPE_COPY_SEND:
      mach_port_insert_right (mach_task_self (), bootstrap, bootstrap,
			      MACH_MSG_TYPE_MAKE_SEND);
      break;

    default:
      abort ();
    }
  ports[INIT_PORT_BOOTSTRAP] = bootstrap;

  /* Try and exec the translator in TASK...  */
  err = file_exec (executable, task, EXEC_DEFAULTS, argz, argz_len,
		   0, 0,
		   fds, fds_type, fds_len,
		   ports, ports_type, ports_len,
		   ints, ints_len, 0, 0, 0, 0);
  ports_moved = 1;
  if (err)
    goto lose;

  /* Ask to be told if TASK dies.  */
  err =
    mach_port_request_notification(mach_task_self(),
				   bootstrap, MACH_NOTIFY_NO_SENDERS, 0,
				   bootstrap, MACH_MSG_TYPE_MAKE_SEND_ONCE,
				   &prev_notify);
  if (err)
    return err;

  /* Ok, cool, we've got a running(?) program, now rendezvous with it if
     possible using the startup protocol on the bootstrap port... */
  err = service_fsys_startup(underlying, underlying_type, bootstrap, 
			     timeout, control);

 lose:
  if (!ports_moved)
    {
      int i;
      
      if (fds_type == MACH_MSG_TYPE_MOVE_SEND)
	for (i = 0; i < fds_len; i++)
	  mach_port_deallocate (mach_task_self (), fds[i]);
      if (ports_type == MACH_MSG_TYPE_MOVE_SEND)
	for (i = 0; i < ports_len; i++)
	  mach_port_deallocate (mach_task_self (), ports[i]);
    }
  if (bootstrap != MACH_PORT_NULL)
    mach_port_destroy(mach_task_self(), bootstrap);
  if (executable != MACH_PORT_NULL)
    mach_port_deallocate(mach_task_self(), executable);
  if (task != MACH_PORT_NULL)
    mach_port_deallocate(mach_task_self(), task);

  return err;
}
