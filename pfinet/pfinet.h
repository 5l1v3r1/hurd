/* 
   Copyright (C) 1995 Free Software Foundation, Inc.
   Written by Michael I. Bushnell, p/BSG.

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
   Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111, USA. */

#ifndef PFINET_H_
#define PFINET_H_

#include <device/device.h>
#include <hurd/ports.h>
#include <linux/netdevice.h>
#include <hurd/trivfs.h>

extern device_t master_device;

void incoming_net_packet (void);

extern struct proto_ops *proto_ops;

struct mutex global_lock;

struct port_bucket *pfinet_bucket;
struct port_class *addrport_class;
struct port_class *socketport_class;

extern struct device ether_dev;

/* A port on SOCK.  Multiple sock_user's can point to the same socket. */
struct sock_user
{
  struct port_info pi;
  int isroot;
  struct socket *sock;
};

/* Socket address ports. */
struct sock_addr
{
  struct port_info pi;
  size_t len;
  struct sockaddr address[0];
};

int ethernet_demuxer (mach_msg_header_t *, mach_msg_header_t *);
void setup_ethernet_device (char *);
void become_task_protid (struct trivfs_protid *);
void become_task (struct sock_user *);
struct sock_user *make_sock_user (struct socket *, int);
error_t make_sockaddr_port (struct socket *, int, 
			    mach_port_t *, mach_msg_type_name_t *);
void init_devices (void);
void init_time (void);
void inet_proto_init (struct net_proto *);
void ip_rt_add (short, u_long, u_long, u_long, struct device *, 
		u_short, u_long);
int tcp_readable (struct sock *);


struct sock_user *begin_using_socket_port (socket_t);
struct sock_addr *begin_using_sockaddr_port (socket_t);
void end_using_socket_port (struct sock_user *);
void end_using_sockaddr_port (struct sock_addr *);
void clean_addrport (void *);
void clean_socketport (void *);

/* MiG bogosity */
typedef struct sock_user *sock_user_t;
typedef struct sock_addr *sock_addr_t;
typedef struct trivfs_protid *trivfs_protid_t;

#endif
