/*
** Copyright 2001, Travis Geiselbrecht. All rights reserved.
** Distributed under the terms of the NewOS License.
*/
#include <kernel/kernel.h>
#include <kernel/khash.h>
#include <kernel/lock.h>
#include <kernel/heap.h>
#include <kernel/arch/cpu.h>
#include <kernel/net/udp.h>
#include <kernel/net/socket.h>

typedef struct netsocket {
	struct netsocket *next;
	sock_id id;
	int type;
	void *prot_data;
} netsocket;

static netsocket *sock_table;
static mutex sock_mutex;
static sock_id next_sock_id;

static int sock_compare_func(void *_s, void *_key)
{
	netsocket *s = _s;
	sock_id *id = _key;

	if(s->id == *id)
		return 0;
	else
		return 1;
}

static unsigned int sock_hash_func(void *_s, void *_key, int range)
{
	netsocket *s = _s;
	sock_id *id = _key;

	if(s)
		return s->id % range;
	else
		return *id % range;
}

sock_id socket_create(int type, int flags, sockaddr *addr)
{
	netsocket *s;
	void *prot_data;
	int err;

	switch(type) {
		case SOCK_PROTO_UDP:
			err = udp_open(&addr->addr, addr->port, &prot_data);
			break;
		default:
			prot_data = NULL;
			err = ERR_INVALID_ARGS;
	}

	if(err < 0)
		return err;

	// create the net socket
	s = kmalloc(sizeof(netsocket));
	if(!s) {
		err = ERR_NO_MEMORY;
		goto err;
 	}

	// set it up
	s->id = atomic_add(&next_sock_id, 1);
	s->prot_data = prot_data;
	s->type = type;

	// insert it in the socket list
	mutex_lock(&sock_mutex);
	hash_insert(sock_table, s);
	mutex_unlock(&sock_mutex);

	return s->id;

err:
	switch(type) {
		case SOCK_PROTO_UDP:
			udp_close(prot_data);
			break;
	}
	return err;
}

ssize_t socket_read(sock_id id, void *buf, ssize_t len)
{
	netsocket *s;
	ssize_t err;

	// insert it in the socket list
	mutex_lock(&sock_mutex);
	s = hash_lookup(sock_table, &id);
	mutex_unlock(&sock_mutex);

	switch(s->type) {
		case SOCK_PROTO_UDP:
			err = udp_read(s->prot_data, buf, len);
			break;
		default:
			err = ERR_INVALID_ARGS;
	}
	return err;
}


int socket_init(void)
{
	netsocket s;

	next_sock_id = 0;

	mutex_init(&sock_mutex, "socket list mutex");

	sock_table = hash_init(256, (addr)&s.next - (addr)&s, &sock_compare_func, &sock_hash_func);
	if(!sock_table)
		return ERR_NO_MEMORY;

	return 0;
}
