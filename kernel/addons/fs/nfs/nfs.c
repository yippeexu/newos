/*
** Copyright 2002-2006, Travis Geiselbrecht. All rights reserved.
** Distributed under the terms of the NewOS License.
*/
#include <kernel/kernel.h>
#include <kernel/vfs.h>
#include <kernel/heap.h>
#include <kernel/khash.h>
#include <kernel/debug.h>
#include <kernel/lock.h>
#include <kernel/sem.h>
#include <kernel/vm.h>
#include <kernel/net/misc.h>

#include <newos/net.h>

#include <string.h>
#include <ctype.h>
#include <stdlib.h>

#include "nfs.h"
#include "nfs_fs.h"
#include "rpc.h"

#define NFS_TRACE 0

#if NFS_TRACE
#define TRACE(x...) dprintf(x)
#else
#define TRACE(x...)
#endif

static int nfs_getattr(nfs_fs *nfs, nfs_vnode *v, uint8 *attrstatbuf, nfs_attrstat *attrstat);

#if NFS_TRACE
static void dump_fhandle(const nfs_fhandle *handle)
{
	unsigned int i;

	for(i=0; i<sizeof(nfs_fhandle); i++)
		dprintf("%02x", ((const uint8 *)handle)[i]);
}
#endif

static nfs_vnode *new_vnode_struct(nfs_fs *fs)
{
	nfs_vnode *v;

	v = (nfs_vnode *)kmalloc(sizeof(nfs_vnode));
	if(!v)
		return NULL;

	if (mutex_init(&v->lock, "nfs vnode lock") < 0) {
		kfree(v);
		return NULL;
	}

	v->hash_next = NULL;
	v->fs = fs;

	return v;
}

static void destroy_vnode_struct(nfs_vnode *v)
{
	mutex_destroy(&v->lock);
	kfree(v);
}

/* ghetto x.x.x.x:/path parsing code */
static int parse_mount(const char *mount, char *address, int address_len, char *server_path, int path_len)
{
	int a, b;

	// trim the beginning
	for(a = 0; mount[a] != 0 && isspace(mount[a]); a++)
		;
	if(mount[a] == 0)
		return ERR_NOT_FOUND;

	// search for the ':'
	for(b = a; mount[b] != 0 && mount[b] != ':'; b++)
		;
	if(mount[b] == 0)
		return ERR_NOT_FOUND;

	// copy the address out
	memcpy(address, &mount[a], b - a);
	address[b - a] = 0;

	// grab the path
	strcpy(server_path, &mount[b+1]);

	return NO_ERROR;
}

static int nfs_handle_hash_compare(void *a, const void *key)
{
	struct nfs_vnode *v = (struct nfs_vnode *)a;
	nfs_fhandle *handle = (nfs_fhandle *)key;

	return memcmp(&v->nfs_handle, handle, sizeof(nfs_fhandle));
}

static unsigned int nfs_handle_hash(void *a, const void *key, unsigned int range)
{
	const nfs_fhandle *hashit;
	unsigned int hash;
	unsigned int i;

	if (key)
		hashit = (const nfs_fhandle *)key;
	else
		hashit = (const nfs_fhandle *)&((nfs_vnode *)a)->nfs_handle;

#if NFS_TRACE
	dprintf("nfs_handle_hash: hashit ");
	dump_fhandle(hashit);
#endif	

	hash = 0;
	for (i=0; i < sizeof(*hashit) / sizeof(unsigned int); i++) {
		hash += ((unsigned int *)hashit)[i];
	}
	hash += hash >> 16;

	hash %= range;

#if NFS_TRACE
	dprintf(" hash 0x%x\n", hash);
#endif

	return hash;
}

static int nfs_status_to_error(nfs_status status)
{
	switch (status) {
		case NFS_OK:
			return NO_ERROR;
		case NFSERR_PERM:
			return ERR_PERMISSION_DENIED;
		case NFSERR_NOENT:
			return ERR_VFS_PATH_NOT_FOUND;
		case NFSERR_IO:
			return ERR_IO_ERROR;
		case NFSERR_NXIO:
			return ERR_NOT_FOUND;
		case NFSERR_ACCES:
			return ERR_PERMISSION_DENIED;
		case NFSERR_EXIST:
			return ERR_VFS_ALREADY_EXISTS;
		case NFSERR_NODEV:
			return ERR_NOT_FOUND;
		case NFSERR_NOTDIR:
			return ERR_VFS_NOT_DIR;
		case NFSERR_ISDIR:
			return ERR_VFS_IS_DIR;
		case NFSERR_FBIG:
			return ERR_TOO_BIG;
		case NFSERR_NOSPC:
			return ERR_VFS_OUT_OF_SPACE;
		case NFSERR_ROFS:
			return ERR_VFS_READONLY_FS;
		case NFSERR_NAMETOOLONG:
			return ERR_VFS_PATH_TOO_LONG;
		case NFSERR_NOTEMPTY:
			return ERR_VFS_DIR_NOT_EMPTY;
		case NFSERR_DQUOT:
			return ERR_VFS_EXCEEDED_QUOTA;
		case NFSERR_STALE:
			return ERR_INVALID_HANDLE;
		default:
			return ERR_GENERAL;
	}
}

static int parse_ipv4_addr_str(ipv4_addr *ip_addr, char *ip_addr_string)
{
	int a, b;

	*ip_addr = 0;

	// walk through the first number
	a = 0;
	b = 0;
	for(; ip_addr_string[b] != 0 && ip_addr_string[b] != '.'; b++)
		;
	if(ip_addr_string[b] == 0)
		return ERR_NOT_FOUND;
	ip_addr_string[b] = 0;
	*ip_addr = atoi(&ip_addr_string[a]) << 24;
	b++;

	// second digit
	a = b;
	for(; ip_addr_string[b] != 0 && ip_addr_string[b] != '.'; b++)
		;
	if(ip_addr_string[b] == 0)
		return ERR_NOT_FOUND;
	ip_addr_string[b] = 0;
	*ip_addr |= atoi(&ip_addr_string[a]) << 16;
	b++;

	// third digit
	a = b;
	for(; ip_addr_string[b] != 0 && ip_addr_string[b] != '.'; b++)
		;
	if(ip_addr_string[b] == 0)
		return ERR_NOT_FOUND;
	ip_addr_string[b] = 0;
	*ip_addr |= atoi(&ip_addr_string[a]) << 8;
	b++;

	// last digit
	a = b;
	for(; ip_addr_string[b] != 0 && ip_addr_string[b] != '.'; b++)
		;
	ip_addr_string[b] = 0;
	*ip_addr |= atoi(&ip_addr_string[a]);

	return NO_ERROR;
}

static int nfs_mount_fs(nfs_fs *nfs, const char *server_path)
{
	uint8 sendbuf[256];
	char buf[128];
	nfs_mountargs args;
	size_t arglen;
	int err;

	rpc_set_port(&nfs->rpc, nfs->mount_port);

	args.dirpath = server_path;
	arglen = nfs_pack_mountargs(sendbuf, &args);

	err = rpc_call(&nfs->rpc, MOUNTPROG, MOUNTVERS, MOUNTPROC_MNT, sendbuf, arglen, buf, sizeof(buf));
	if(err < 0)
		return err;

#if NFS_TRACE
	TRACE("nfs_mount_fs: have root fhandle: ");
	dump_fhandle((const nfs_fhandle *)&buf[4]);
	TRACE("\n");
#endif
	// we should have the root handle now
	memcpy(&nfs->root_vnode->nfs_handle, &buf[4], sizeof(nfs->root_vnode->nfs_handle));

	// set the rpc port to the nfs server
	rpc_set_port(&nfs->rpc, nfs->nfs_port);

	return 0;
}

static int nfs_unmount_fs(nfs_fs *nfs)
{
	uint8 sendbuf[256];
	size_t arglen;
	nfs_mountargs args;
	int err;

	rpc_set_port(&nfs->rpc, nfs->mount_port);

	args.dirpath = nfs->server_path;
	arglen = nfs_pack_mountargs(sendbuf, &args);

	err = rpc_call(&nfs->rpc, MOUNTPROG, MOUNTVERS, MOUNTPROC_UMNT, sendbuf, arglen, NULL, 0);

	return err;
}

int nfs_mount(fs_cookie *fs, fs_id id, const char *device, void *args, vnode_id *root_vnid)
{
	nfs_fs *nfs;
	int err;
	char ip_addr_str[128];
	ipv4_addr ip_addr;

	TRACE("nfs_mount: fsid 0x%x, device '%s'\n", id, device);

	/* create the fs structure */
	nfs = kmalloc(sizeof(nfs_fs));
	if(!nfs) {
		err = ERR_NO_MEMORY;
		goto err;
	}
	memset(nfs, 0, sizeof(nfs_fs));

	mutex_init(&nfs->lock, "nfs lock");

	err = parse_mount(device, ip_addr_str, sizeof(ip_addr_str), nfs->server_path, sizeof(nfs->server_path));
	if(err < 0) {
		err = ERR_NET_BAD_ADDRESS;
		goto err1;
	}

	err = parse_ipv4_addr_str(&ip_addr, ip_addr_str);
	if(err < 0) {
		err = ERR_NET_BAD_ADDRESS;
		goto err1;
	}

	nfs->id = id;
	nfs->server_addr.type = ADDR_TYPE_IP;
	nfs->server_addr.len = 4;
	NETADDR_TO_IPV4(nfs->server_addr) = ip_addr;

	// set up the rpc state
	rpc_init_state(&nfs->rpc);
	rpc_open_socket(&nfs->rpc, &nfs->server_addr);

	// look up the port numbers for mount and nfs
	rpc_pmap_lookup(&nfs->server_addr, MOUNTPROG, MOUNTVERS, IP_PROT_UDP, &nfs->mount_port);
	rpc_pmap_lookup(&nfs->server_addr, NFSPROG, NFSVERS, IP_PROT_UDP, &nfs->nfs_port);

	nfs->root_vnode = new_vnode_struct(nfs);
	nfs->root_vnode->st = STREAM_TYPE_DIR;

	// try to mount the filesystem
	err = nfs_mount_fs(nfs, nfs->server_path);
	if(err < 0)
		goto err2;

	// build the vnode hash table and stick the root vnode in it
	nfs->handle_hash = hash_init(1024, offsetof(struct nfs_vnode, hash_next), 
			nfs_handle_hash_compare,
			nfs_handle_hash);
	if (!nfs->handle_hash) {
		err = ERR_NO_MEMORY;
		goto err2;
	}
	hash_insert(nfs->handle_hash, nfs->root_vnode);

	*fs = nfs;
	*root_vnid = VNODETOVNID(nfs->root_vnode);

	return 0;

err2:
	destroy_vnode_struct(nfs->root_vnode);
	rpc_destroy_state(&nfs->rpc);
err1:
	mutex_destroy(&nfs->lock);
	kfree(nfs);
err:
	return err;
}

int nfs_unmount(fs_cookie fs)
{
	nfs_fs *nfs = (nfs_fs *)fs;

	TRACE("nfs_unmount: fsid 0x%x\n", nfs->id);

	// put_vnode on the root to release the ref to it
	vfs_put_vnode(nfs->id, VNODETOVNID(nfs->root_vnode));

	nfs_unmount_fs(nfs);

	hash_uninit(nfs->handle_hash);

	rpc_destroy_state(&nfs->rpc);

	mutex_destroy(&nfs->lock);
	kfree(nfs);

	return 0;
}

int nfs_sync(fs_cookie fs)
{
	nfs_fs *nfs = (nfs_fs *)fs;

	TOUCH(nfs);

	TRACE("nfs_sync: fsid 0x%x\n", nfs->id);
	return 0;
}

int nfs_lookup(fs_cookie fs, fs_vnode _dir, const char *name, vnode_id *id)
{
	nfs_fs *nfs = (nfs_fs *)fs;
	nfs_vnode *dir = (nfs_vnode *)_dir;
	int err;

	TRACE("nfs_lookup: fsid 0x%x, dirvnid 0x%Lx, name '%s'\n", nfs->id, VNODETOVNID(dir), name);

	mutex_lock(&dir->lock);

	{
		uint8 sendbuf[NFS_DIROPARGS_MAXLEN];
		nfs_diropargs args;
		size_t arglen;

		uint8 resbuf[NFS_DIROPRES_MAXLEN];
		nfs_diropres  res;

		/* set up the args structure */
		args.dir = &dir->nfs_handle;
		args.name.name = name;
		arglen = nfs_pack_diropargs(sendbuf, &args);

		err = rpc_call(&nfs->rpc, NFSPROG, NFSVERS, NFSPROC_LOOKUP, sendbuf, arglen, resbuf, sizeof(resbuf));
		if(err < 0) {
			err = ERR_NOT_FOUND;
			goto out;
		}

		nfs_unpack_diropres(resbuf, &res);

		/* see if the lookup was successful */
		if(res.status == NFS_OK) {
			nfs_vnode *v;
			nfs_vnode *v2;
			bool newvnode;

			/* successful lookup */
#if NFS_TRACE
			dprintf("nfs_lookup: result of lookup of '%s'\n", name);
			dprintf("\tfhandle: "); dump_fhandle(res.file); dprintf("\n");
			dprintf("\tsize: %d\n", res.attributes->size);
			nfs_handle_hash(NULL, res.file, 1024);
#endif

			/* see if the vnode already exists */
			newvnode = false;
			v = hash_lookup(nfs->handle_hash, res.file);
			if (v == NULL) {
				/* didn't find it, create a new one */
				v = new_vnode_struct(nfs);
				if(v == NULL) {
					err = ERR_NO_MEMORY;
					goto out;
				}

				/* copy the file handle over */
				memcpy(&v->nfs_handle, res.file, sizeof(v->nfs_handle));

				/* figure out the stream type from the return value and cache it */
				switch(res.attributes->ftype) {
					case NFREG:
						v->st = STREAM_TYPE_FILE;
						break;
					case NFDIR:
						v->st = STREAM_TYPE_DIR;
						break;
					default:
						v->st = -1;
				}

				/* add it to the handle -> vnode lookup table */
				mutex_lock(&nfs->lock);
				hash_insert(nfs->handle_hash, v);
				mutex_unlock(&nfs->lock);
				newvnode = true;
			}

			/* request that the vfs layer look it up */
			err = vfs_get_vnode(nfs->id, VNODETOVNID(v), (fs_vnode *)(void *)&v2);
			if(err < 0) {
				if (newvnode) {
					mutex_lock(&nfs->lock);
					hash_remove(nfs->handle_hash, v);
					mutex_unlock(&nfs->lock);
					destroy_vnode_struct(v);
				}
				err = ERR_NOT_FOUND;
				goto out;
			}

			ASSERT(v == v2);

			*id = VNODETOVNID(v);
		} else {
			TRACE("nfs_lookup: '%s' not found\n", name);
			err = ERR_NOT_FOUND;
			goto out;
		}
	}

	err = NO_ERROR;

out:
	mutex_unlock(&dir->lock);

	return err;
}

int nfs_getvnode(fs_cookie fs, vnode_id id, fs_vnode *v, bool r)
{
	nfs_fs *nfs = (nfs_fs *)fs;

	TOUCH(nfs);

	TRACE("nfs_getvnode: fsid 0x%x, vnid 0x%Lx\n", nfs->id, id);

	*v = VNIDTOVNODE(id);

	return NO_ERROR;
}

int nfs_putvnode(fs_cookie fs, fs_vnode _v, bool r)
{
	nfs_fs *nfs = (nfs_fs *)fs;
	nfs_vnode *v = (nfs_vnode *)_v;

	TOUCH(nfs);

	TRACE("nfs_putvnode: fsid 0x%x, vnid 0x%Lx\n", nfs->id, VNODETOVNID(v));

	mutex_lock(&nfs->lock);
	hash_remove(nfs->handle_hash, v);
	mutex_unlock(&nfs->lock);
	destroy_vnode_struct(v);

	return NO_ERROR;
}

int nfs_removevnode(fs_cookie fs, fs_vnode _v, bool r)
{
	nfs_fs *nfs = (nfs_fs *)fs;
	nfs_vnode *v = (nfs_vnode *)_v;

	TOUCH(nfs);TOUCH(v);

	TRACE("nfs_removevnode: fsid 0x%x, vnid 0x%Lx\n", nfs->id, VNODETOVNID(v));

	return ERR_UNIMPLEMENTED;
}

static int nfs_opendir(fs_cookie _fs, fs_vnode _v, dir_cookie *_cookie)
{
	struct nfs_vnode *v = (struct nfs_vnode *)_v;
	struct nfs_cookie *cookie;
	int err = 0;

	TRACE("nfs_opendir: vnode %p\n", v);

	if(v->st != STREAM_TYPE_DIR)
		return ERR_VFS_NOT_DIR;

	cookie = kmalloc(sizeof(struct nfs_cookie));
	if(cookie == NULL)
		return ERR_NO_MEMORY;

	cookie->v = v;
	cookie->u.dir.nfscookie = 0;
	cookie->u.dir.at_end = false;

	*_cookie = cookie;

	return err;
}

static int nfs_closedir(fs_cookie _fs, fs_vnode _v, dir_cookie _cookie)
{
	struct nfs_fs *fs = _fs;
	struct nfs_vnode *v = _v;
	struct nfs_cookie *cookie = _cookie;

	TOUCH(fs);TOUCH(v);TOUCH(cookie);

	TRACE("nfs_closedir: entry vnode %p, cookie %p\n", v, cookie);

	if(v->st != STREAM_TYPE_DIR)
		return ERR_VFS_NOT_DIR;

	if(cookie)
		kfree(cookie);

	return 0;
}

static int nfs_rewinddir(fs_cookie _fs, fs_vnode _v, dir_cookie _cookie)
{
	struct nfs_vnode *v = _v;
	struct nfs_cookie *cookie = _cookie;
	int err = 0;

	TOUCH(v);

	TRACE("nfs_rewinddir: vnode %p, cookie %p\n", v, cookie);

	if(v->st != STREAM_TYPE_DIR)
		return ERR_VFS_NOT_DIR;

	mutex_lock(&v->lock);

	cookie->u.dir.nfscookie = 0;
	cookie->u.dir.at_end = false;

	mutex_unlock(&v->lock);

	return err;
}

#define READDIR_BUF_SIZE (MAXNAMLEN + 64)

static ssize_t _nfs_readdir(nfs_fs *nfs, nfs_vnode *v, nfs_cookie *cookie, void *buf, ssize_t len)
{
	uint8 resbuf[READDIR_BUF_SIZE];
	nfs_readdirres  *res  = (nfs_readdirres *)resbuf;
	uint8 argbuf[NFS_READDIRARGS_MAXLEN];
	nfs_readdirargs args;
	size_t arglen;
	ssize_t err = 0;
	int i;
	int namelen;

	if(len < MAXNAMLEN)
		return ERR_VFS_INSUFFICIENT_BUF; // XXX not quite accurate

	/* see if we've already hit the end */
	if(cookie->u.dir.at_end)
		return 0;

	/* put together the message */
	args.dir = &v->nfs_handle;
	args.cookie = cookie->u.dir.nfscookie;
	args.count = min(len, READDIR_BUF_SIZE);
	arglen = nfs_pack_readdirargs(argbuf, &args);

	err = rpc_call(&nfs->rpc, NFSPROG, NFSVERS, NFSPROC_READDIR, argbuf, arglen, resbuf, sizeof(resbuf));
	if(err < 0)
		return err;

	/* get response */
	if(ntohl(res->status) != NFS_OK)
		return 0;

	/* walk into the buffer, looking for the first entry */
	if(ntohl(res->data[0]) == 0) {
		// end of list
		cookie->u.dir.at_end = true;
		return 0;
	}
	i = ntohl(res->data[0]);

	/* copy the data out of the first entry */
	strlcpy(buf, (char const *)&res->data[i + 2], ntohl(res->data[i + 1]) + 1);

	namelen = ROUNDUP(ntohl(res->data[i + 1]), 4);

	/* update the cookie */
	cookie->u.dir.nfscookie = res->data[i + namelen / 4 + 2];

	return ntohl(res->data[i + 1]);
}

static int nfs_readdir(fs_cookie _fs, fs_vnode _v, dir_cookie _cookie, void *buf, size_t len)
{
	struct nfs_fs *fs = _fs;
	struct nfs_vnode *v = _v;
	struct nfs_cookie *cookie = _cookie;
	int err = 0;

	TOUCH(v);

	TRACE("nfs_readdir: vnode %p, cookie %p, len 0x%x\n", v, cookie, len);

	if(v->st != STREAM_TYPE_DIR)
		return ERR_VFS_NOT_DIR;

	mutex_lock(&v->lock);

	err = _nfs_readdir(fs, v, cookie, buf, len);

	mutex_unlock(&v->lock);

	return err;
}

int nfs_open(fs_cookie fs, fs_vnode _v, file_cookie *_cookie, int oflags)
{
	nfs_fs *nfs = (nfs_fs *)fs;
	nfs_vnode *v = (nfs_vnode *)_v;
	nfs_cookie *cookie;
	int err;

	TOUCH(nfs);

	TRACE("nfs_open: fsid 0x%x, vnid 0x%Lx, oflags 0x%x\n", nfs->id, VNODETOVNID(v), oflags);

	if(v->st == STREAM_TYPE_DIR) {
		err = ERR_VFS_IS_DIR;
		goto err;
	}

	cookie = kmalloc(sizeof(nfs_cookie));
	if(cookie == NULL) {
		err = ERR_NO_MEMORY;
		goto err;
	}
	cookie->v = v;

	cookie->u.file.pos = 0;
	cookie->u.file.oflags = oflags;

	*_cookie = (file_cookie)cookie;
	err = NO_ERROR;

err:
	return err;
}

int nfs_close(fs_cookie fs, fs_vnode _v, file_cookie _cookie)
{
	nfs_fs *nfs = (nfs_fs *)fs;
	nfs_vnode *v = (nfs_vnode *)_v;

	TOUCH(nfs);TOUCH(v);

	TRACE("nfs_close: fsid 0x%x, vnid 0x%Lx\n", nfs->id, VNODETOVNID(v));

	if(v->st == STREAM_TYPE_DIR) 
		return ERR_VFS_IS_DIR;

	return NO_ERROR;
}

int nfs_freecookie(fs_cookie fs, fs_vnode _v, file_cookie _cookie)
{
	nfs_fs *nfs = (nfs_fs *)fs;
	nfs_vnode *v = (nfs_vnode *)_v;
	nfs_cookie *cookie = (nfs_cookie *)_cookie;

	TOUCH(nfs);TOUCH(v);

	TRACE("nfs_freecookie: fsid 0x%x, vnid 0x%Lx\n", nfs->id, VNODETOVNID(v));

	if(v->st == STREAM_TYPE_DIR) 
		return ERR_VFS_IS_DIR;

	kfree(cookie);

	return NO_ERROR;
}

int nfs_fsync(fs_cookie fs, fs_vnode _v)
{
	nfs_fs *nfs = (nfs_fs *)fs;
	nfs_vnode *v = (nfs_vnode *)_v;

	TOUCH(nfs);TOUCH(v);

	TRACE("nfs_fsync: fsid 0x%x, vnid 0x%Lx\n", nfs->id, VNODETOVNID(v));

	return NO_ERROR;
}

#define READ_BUF_SIZE 1024

static ssize_t nfs_readfile(nfs_fs *nfs, nfs_vnode *v, nfs_cookie *cookie, void *buf, off_t pos, ssize_t len, bool updatecookiepos)
{
	uint8 resbuf[NFS_READRES_MAXLEN + READ_BUF_SIZE];
	nfs_readres  res;

	uint8 argbuf[NFS_READARGS_MAXLEN];
	nfs_readargs args;
	size_t arglen;
	int err;
	ssize_t total_read = 0;

	TRACE("nfs_readfile: v %p, buf %p, pos %Ld, len %d\n", v, buf, pos, len);

	/* check args */
	if(pos < 0)
		pos = cookie->u.file.pos;
	/* can't do more than 32-bit offsets right now */
	if(pos > 0xffffffff)
		return 0;
	/* negative or zero length means nothing */
	if(len <= 0)
		return 0;

	while(len > 0) {
		ssize_t to_read = min(len, READ_BUF_SIZE);

		/* put together the message */
		args.file = &v->nfs_handle;
		args.offset = pos;
		args.count = to_read;
		args.totalcount = 0; // unused
		arglen = nfs_pack_readargs(argbuf, &args);

		err = rpc_call(&nfs->rpc, NFSPROG, NFSVERS, NFSPROC_READ, argbuf, arglen, resbuf, sizeof(resbuf));
		if(err < 0)
			break;

		nfs_unpack_readres(resbuf, &res);

		/* get response */
		if(res.status != NFS_OK)
			break;

		/* see how much we read */
		err = user_memcpy((uint8 *)buf + total_read, res.data, res.len);
		if(err < 0) {
			total_read = err; // bad user give me bad buffer
			break;
		}

		pos += res.len;
		len -= res.len;
		total_read += res.len;

		/* short read, we're done */
		if((ssize_t)res.len != to_read)
			break;
	}

	if (updatecookiepos)
		cookie->u.file.pos = pos;

	return total_read;
}

ssize_t nfs_read(fs_cookie fs, fs_vnode _v, file_cookie _cookie, void *buf, off_t pos, ssize_t len)
{
	nfs_fs *nfs = (nfs_fs *)fs;
	nfs_vnode *v = (nfs_vnode *)_v;
	nfs_cookie *cookie = (nfs_cookie *)_cookie;
	ssize_t err;

	TRACE("nfs_read: fsid 0x%x, vnid 0x%Lx, buf %p, pos 0x%Lx, len %ld\n", nfs->id, VNODETOVNID(v), buf, pos, len);

	if(v->st == STREAM_TYPE_DIR)
		return ERR_VFS_IS_DIR;

	mutex_lock(&v->lock);

	err = nfs_readfile(nfs, v, cookie, buf, pos, len, true);

	mutex_unlock(&v->lock);

	return err;
}

#define WRITE_BUF_SIZE 1024

static ssize_t nfs_writefile(nfs_fs *nfs, nfs_vnode *v, nfs_cookie *cookie, const void *buf, off_t pos, ssize_t len, bool updatecookiepos)
{
#if 0
	uint8 abuf[sizeof(nfs_writeargs) + WRITE_BUF_SIZE];
	nfs_writeargs *args = (nfs_writeargs *)abuf;
	nfs_attrstat  *res  = (nfs_attrstat *)abuf;
	int err;
	ssize_t total_written = 0;

	/* check args */
	if(pos < 0)
		pos = cookie->u.file.pos;
	/* can't do more than 32-bit offsets right now */
	if(pos > 0xffffffff)
		return 0;
	/* negative or zero length means nothing */
	if(len <= 0)
		return 0;

	while(len > 0) {
		ssize_t to_write = min(len, WRITE_BUF_SIZE);

		/* put together the message */
		memcpy(&args->file, &v->nfs_handle, sizeof(args->file));
		args->beginoffset = 0; // unused
		args->offset = htonl(pos);
		args->totalcount = htonl(to_write);
		err = user_memcpy(args->data, (const uint8 *)buf + total_written, to_write);
		if(err < 0) {
			total_written = err;
			break;
		}

		err = rpc_call(&nfs->rpc, NFSPROG, NFSVERS, NFSPROC_WRITE, args, sizeof(*args) + to_write, abuf, sizeof(abuf));
		if(err < 0)
			break;

		/* get response */
		if(ntohl(res->status) != NFS_OK)
			break;

		pos += to_write;
		len -= to_write;
		total_written += to_write;
	}

	if (updatecookiepos)
		cookie->u.file.pos = pos;

	return total_written;
#endif
	return ERR_UNIMPLEMENTED;
}

ssize_t nfs_write(fs_cookie fs, fs_vnode _v, file_cookie _cookie, const void *buf, off_t pos, ssize_t len)
{
	nfs_fs *nfs = (nfs_fs *)fs;
	nfs_vnode *v = (nfs_vnode *)_v;
	nfs_cookie *cookie = (nfs_cookie *)_cookie;
	ssize_t err;

	TRACE("nfs_write: fsid 0x%x, vnid 0x%Lx, buf %p, pos 0x%Lx, len %ld\n", nfs->id, VNODETOVNID(v), buf, pos, len);

	mutex_lock(&v->lock);

	switch(v->st) {
		case STREAM_TYPE_FILE:
			err = nfs_writefile(nfs, v, cookie, buf, pos, len, true);
			break;
		case STREAM_TYPE_DIR:
			err = ERR_NOT_ALLOWED;
			break;
		default:
			err = ERR_GENERAL;
	}

	mutex_unlock(&v->lock);

	return err;
}

int nfs_seek(fs_cookie fs, fs_vnode _v, file_cookie _cookie, off_t pos, seek_type st)
{
	nfs_fs *nfs = (nfs_fs *)fs;
	nfs_vnode *v = (nfs_vnode *)_v;
	nfs_cookie *cookie = (nfs_cookie *)_cookie;
	int err = NO_ERROR;
	uint8 attrstatbuf[NFS_ATTRSTAT_MAXLEN];
	nfs_attrstat attrstat;
	off_t file_len;

	TRACE("nfs_seek: fsid 0x%x, vnid 0x%Lx, pos 0x%Lx, seek_type %d\n", nfs->id, VNODETOVNID(v), pos, st);

	if(v->st == STREAM_TYPE_DIR) 
		return ERR_VFS_IS_DIR;

	mutex_lock(&v->lock);

	err = nfs_getattr(nfs, v, attrstatbuf, &attrstat);
	if(err < 0)
		goto out;

	file_len = attrstat.attributes->size;

	switch(st) {
		case _SEEK_SET:
			if(pos < 0)
				pos = 0;
			if(pos > file_len)
				pos = file_len;
			cookie->u.file.pos = pos;
			break;
		case _SEEK_CUR:
			if(pos + cookie->u.file.pos > file_len)
				cookie->u.file.pos = file_len;
			else if(pos + cookie->u.file.pos < 0)
				cookie->u.file.pos = 0;
			else
				cookie->u.file.pos += pos;
			break;
		case _SEEK_END:
			if(pos > 0)
				cookie->u.file.pos = file_len;
			else if(pos + file_len < 0)
				cookie->u.file.pos = 0;
			else
				cookie->u.file.pos = pos + file_len;
			break;
		default:
			err = ERR_INVALID_ARGS;
	}

out:
	mutex_unlock(&v->lock);

	return err;
}

int nfs_ioctl(fs_cookie fs, fs_vnode _v, file_cookie cookie, int op, void *buf, size_t len)
{
	nfs_fs *nfs = (nfs_fs *)fs;
	nfs_vnode *v = (nfs_vnode *)_v;

	TOUCH(nfs);TOUCH(v);

	TRACE("nfs_ioctl: fsid 0x%x, vnid 0x%Lx, op %d, buf %p, len %ld\n", nfs->id, VNODETOVNID(v), op, buf, len);

	return ERR_UNIMPLEMENTED;
}

int nfs_canpage(fs_cookie fs, fs_vnode _v)
{
	nfs_fs *nfs = (nfs_fs *)fs;
	nfs_vnode *v = (nfs_vnode *)_v;

	TOUCH(nfs);TOUCH(v);

	TRACE("nfs_canpage: fsid 0x%x, vnid 0x%Lx\n", nfs->id, VNODETOVNID(v));

	if(v->st == STREAM_TYPE_FILE) 
		return 1;
	else
		return 0;
}

ssize_t nfs_readpage(fs_cookie fs, fs_vnode _v, iovecs *vecs, off_t pos)
{
	nfs_fs *nfs = (nfs_fs *)fs;
	nfs_vnode *v = (nfs_vnode *)_v;
	unsigned int i;
	ssize_t readfile_return;
	ssize_t total_bytes_read = 0;

	TOUCH(nfs);TOUCH(v);

	TRACE("nfs_readpage: fsid 0x%x, vnid 0x%Lx, vecs %p, pos 0x%Lx\n", nfs->id, VNODETOVNID(v), vecs, pos);
	
	if(v->st == STREAM_TYPE_DIR)
		return ERR_VFS_IS_DIR;

	mutex_lock(&v->lock);

	for (i=0; i < vecs->num; i++) {
		readfile_return = nfs_readfile(nfs, v, NULL, vecs->vec[i].start, pos, vecs->vec[i].len, false);
		TRACE("nfs_readpage: nfs_readfile returns %d\n", readfile_return);
		if (readfile_return < 0)
			goto out;

		pos += readfile_return;
		total_bytes_read += readfile_return;

		if ((size_t)readfile_return < vecs->vec[i].len) {
			/* we have hit the end of file, zero out the rest */
			for (; i < vecs->num; i++) {
				memset(vecs->vec[i].start + readfile_return, 0, vecs->vec[i].len - readfile_return);
				total_bytes_read += vecs->vec[i].len - readfile_return;

				readfile_return = 0; // after the first pass, wipe out entire pages
			}
		}
	}

out:
	mutex_unlock(&v->lock);

	return total_bytes_read;
}

ssize_t nfs_writepage(fs_cookie fs, fs_vnode _v, iovecs *vecs, off_t pos)
{
	nfs_fs *nfs = (nfs_fs *)fs;
	nfs_vnode *v = (nfs_vnode *)_v;

	TOUCH(nfs);TOUCH(v);

	TRACE("nfs_writepage: fsid 0x%x, vnid 0x%Lx, vecs %p, pos 0x%Lx\n", nfs->id, VNODETOVNID(v), vecs, pos);

	if(v->st == STREAM_TYPE_DIR)
		return ERR_VFS_IS_DIR;

	return ERR_UNIMPLEMENTED;
}

static int _nfs_create(nfs_fs *nfs, nfs_vnode *dir, const char *name, stream_type type, vnode_id *new_vnid)
{
	int err;
	uint8 argbuf[NFS_CREATEARGS_MAXLEN];
	nfs_createargs args;
	size_t arglen;
	uint8 resbuf[NFS_DIROPRES_MAXLEN];
	nfs_diropres res;
	nfs_vnode *v, *v2;
	int proc;

	/* start building the args */
	args.where.dir = &dir->nfs_handle;
	args.where.name.name = name;
	args.attributes.mode = 0777;
	args.attributes.uid = 0;
	args.attributes.gid = 0;
	args.attributes.size = 0;
	args.attributes.atime.seconds = 0;
	args.attributes.atime.useconds = 0;
	args.attributes.mtime.seconds = 0;
	args.attributes.mtime.useconds = 0;
	arglen = nfs_pack_createopargs(argbuf, &args);

	switch (type) {
		case STREAM_TYPE_FILE:
			proc = NFSPROC_CREATE;
			break;
		case STREAM_TYPE_DIR:
			proc = NFSPROC_MKDIR;
			break;
		default:
			panic("_nfs_create asked to make file type it doesn't understand\n");
	}

	err = rpc_call(&nfs->rpc, NFSPROG, NFSVERS, proc, argbuf, arglen, resbuf, sizeof(resbuf));
	if (err < 0)
		return err;

	nfs_unpack_diropres(resbuf, &res);

	if (res.status != NFS_OK) {
		err = nfs_status_to_error(res.status);
		goto err;
	}

	/* if new_vnid is null, the layers above us aren't requesting that we bring the vnode into existence */
	if (new_vnid == NULL) {
		err = 0;
		goto out;
	}

	/* create a new vnode */
	v = new_vnode_struct(nfs);
	if (v == NULL) {
		err = ERR_NO_MEMORY;
		/* weird state here. we've created a file but failed */
		goto err;
	}

	/* copy the file handle over */
	memcpy(&v->nfs_handle, res.file, sizeof(v->nfs_handle));

	/* figure out the stream type from the return value and cache it */
	switch (res.attributes->ftype) {
		case NFREG:
			v->st = STREAM_TYPE_FILE;
			break;
		case NFDIR:
			v->st = STREAM_TYPE_DIR;
			break;
		default:
			v->st = -1;
	}

	/* add it to the handle -> vnode lookup table */
	mutex_lock(&nfs->lock);
	hash_insert(nfs->handle_hash, v);
	mutex_unlock(&nfs->lock);

	/* request that the vfs layer look it up */
	err = vfs_get_vnode(nfs->id, VNODETOVNID(v), (fs_vnode *)(void *)&v2);
	if (err < 0) {
		mutex_lock(&nfs->lock);
		hash_remove(nfs->handle_hash, v);
		mutex_unlock(&nfs->lock);
		destroy_vnode_struct(v);
		err = ERR_NOT_FOUND;
		goto err;
	}

	*new_vnid = VNODETOVNID(v);

	err = 0;

out:
err:
	return err;
}

int nfs_create(fs_cookie fs, fs_vnode _dir, const char *name, void *create_args, vnode_id *new_vnid)
{
	nfs_fs *nfs = (nfs_fs *)fs;
	nfs_vnode *dir = (nfs_vnode *)_dir;
	int err;

	TOUCH(nfs);TOUCH(dir);

	TRACE("nfs_create: fsid 0x%x, vnid 0x%Lx, name '%s'\n", nfs->id, VNODETOVNID(dir), name);

	mutex_lock(&dir->lock);

	err = _nfs_create(nfs, dir, name, STREAM_TYPE_FILE, new_vnid);

	mutex_unlock(&dir->lock);

	return err;
}

static int _nfs_unlink(nfs_fs *nfs, nfs_vnode *dir, const char *name, stream_type type)
{
	int err;
	uint8 argbuf[NFS_DIROPARGS_MAXLEN];
	nfs_diropargs args;
	size_t arglen;
	nfs_status res;
	int proc;

	/* start building the args */
	args.dir = &dir->nfs_handle;
	args.name.name = name;
	arglen = nfs_pack_diropargs(argbuf, &args);

	switch (type) {
		case STREAM_TYPE_FILE:
			proc = NFSPROC_REMOVE;
			break;
		case STREAM_TYPE_DIR:
			proc = NFSPROC_RMDIR;
			break;
		default:
			panic("_nfs_unlink asked to remove file type it doesn't understand\n");
	}

	err = rpc_call(&nfs->rpc, NFSPROG, NFSVERS, proc, argbuf, arglen, &res, sizeof(res));
	if (err < 0)
		return err;

	return nfs_status_to_error(ntohl(res));
}

int nfs_unlink(fs_cookie fs, fs_vnode _dir, const char *name)
{
	nfs_fs *nfs = (nfs_fs *)fs;
	nfs_vnode *dir = (nfs_vnode *)_dir;
	int err;

	TOUCH(nfs);TOUCH(dir);

	TRACE("nfs_unlink: fsid 0x%x, vnid 0x%Lx, name '%s'\n", nfs->id, VNODETOVNID(dir), name);

	mutex_lock(&dir->lock);

	err = _nfs_unlink(nfs, dir, name, STREAM_TYPE_FILE);

	mutex_unlock(&dir->lock);

	return err;
}

int nfs_rename(fs_cookie fs, fs_vnode _olddir, const char *oldname, fs_vnode _newdir, const char *newname)
{
	nfs_fs *nfs = (nfs_fs *)fs;
	nfs_vnode *olddir = (nfs_vnode *)_olddir;
	nfs_vnode *newdir = (nfs_vnode *)_newdir;

	TOUCH(nfs);TOUCH(olddir);TOUCH(newdir);

	TRACE("nfs_rename: fsid 0x%x, vnid 0x%Lx, oldname '%s', newdir 0x%Lx, newname '%s'\n", nfs->id, VNODETOVNID(olddir), oldname, VNODETOVNID(newdir), newname);

	return ERR_UNIMPLEMENTED;
}

int nfs_mkdir(fs_cookie _fs, fs_vnode _base_dir, const char *name)
{
	nfs_fs *nfs = (nfs_fs *)_fs;
	nfs_vnode *dir = (nfs_vnode *)_base_dir;
	int err;

	TOUCH(nfs);TOUCH(dir);

	TRACE("nfs_mkdir: fsid 0x%x, vnid 0x%Lx, name '%s'\n", nfs->id, VNODETOVNID(dir), name);

	mutex_lock(&dir->lock);

	err = _nfs_create(nfs, dir, name, STREAM_TYPE_DIR, NULL);

	mutex_unlock(&dir->lock);

	return err;
}

int nfs_rmdir(fs_cookie _fs, fs_vnode _base_dir, const char *name)
{
	nfs_fs *nfs = (nfs_fs *)_fs;
	nfs_vnode *dir = (nfs_vnode *)_base_dir;
	int err;

	TOUCH(nfs);TOUCH(dir);

	TRACE("nfs_rmdir: fsid 0x%x, vnid 0x%Lx, name '%s'\n", nfs->id, VNODETOVNID(dir), name);

	mutex_lock(&dir->lock);

	err = _nfs_unlink(nfs, dir, name, STREAM_TYPE_DIR);

	mutex_unlock(&dir->lock);

	return err;
}

static int nfs_getattr(nfs_fs *nfs, nfs_vnode *v, uint8 *buf, nfs_attrstat *attrstat)
{
	int err;

	err = rpc_call(&nfs->rpc, NFSPROG, NFSVERS, NFSPROC_GETATTR, &v->nfs_handle, sizeof(v->nfs_handle), buf, NFS_ATTRSTAT_MAXLEN);
	if (err < 0)
		return err;

	nfs_unpack_attrstat(buf, attrstat);

	return nfs_status_to_error(attrstat->status);
}

int nfs_rstat(fs_cookie fs, fs_vnode _v, struct file_stat *stat)
{
	nfs_fs *nfs = (nfs_fs *)fs;
	nfs_vnode *v = (nfs_vnode *)_v;
	uint8 attrstatbuf[NFS_ATTRSTAT_MAXLEN];
	nfs_attrstat attrstat;
	int err;

	TRACE("nfs_rstat: fsid 0x%x, vnid 0x%Lx, stat %p\n", nfs->id, VNODETOVNID(v), stat);

	mutex_lock(&v->lock);

	err = nfs_getattr(nfs, v, attrstatbuf, &attrstat);
	if(err < 0)
		goto out;

	if(attrstat.status != NFS_OK) {
		err = ERR_IO_ERROR;
		goto out;
	}

	/* copy the stat over from the nfs attrstat */
	stat->vnid = VNODETOVNID(v);
	stat->size = attrstat.attributes->size;
	switch(attrstat.attributes->ftype) {
		case NFREG:
			stat->type = STREAM_TYPE_FILE;
			break;
		case NFDIR:
			stat->type = STREAM_TYPE_DIR;
			break;
		default:
			stat->type = STREAM_TYPE_DEVICE; // XXX should have unknown type
			break;
	}

	err = NO_ERROR;

out:
	mutex_unlock(&v->lock);

	return err;
}

int nfs_wstat(fs_cookie fs, fs_vnode _v, struct file_stat *stat, int stat_mask)
{
	nfs_fs *nfs = (nfs_fs *)fs;
	nfs_vnode *v = (nfs_vnode *)_v;

	TOUCH(nfs);TOUCH(v);

	TRACE("nfs_wstat: fsid 0x%x, vnid 0x%Lx, stat %p, stat_mask 0x%x\n", nfs->id, VNODETOVNID(v), stat, stat_mask);

	return ERR_UNIMPLEMENTED;
}

static struct fs_calls nfs_calls = {
	&nfs_mount,
	&nfs_unmount,
	&nfs_sync,

	&nfs_lookup,

	&nfs_getvnode,
	&nfs_putvnode,
	&nfs_removevnode,

	&nfs_opendir,
	&nfs_closedir,
	&nfs_rewinddir,
	&nfs_readdir,

	&nfs_open,
	&nfs_close,
	&nfs_freecookie,
	&nfs_fsync,

	&nfs_read,
	&nfs_write,
	&nfs_seek,
	&nfs_ioctl,

	&nfs_canpage,
	&nfs_readpage,
	&nfs_writepage,

	&nfs_create,
	&nfs_unlink,
	&nfs_rename,

	&nfs_mkdir,
	&nfs_rmdir,

	&nfs_rstat,
	&nfs_wstat
};

int fs_bootstrap(void);
int fs_bootstrap(void)
{
	dprintf("bootstrap_nfs: entry\n");
	return vfs_register_filesystem("nfs", &nfs_calls);
}

