/**
  Copyright Red Hat, Inc. 2006

  This program is free software; you can redistribute it and/or modify it
  under the terms of the GNU General Public License as published by the
  Free Software Foundation; either version 2, or (at your option) any
  later version.

  This program is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
  General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; see the file COPYING.  If not, write to the
  Free Software Foundation, Inc.,  675 Mass Ave, Cambridge, 
  MA 02139, USA.

  Author: Lon Hohberger <lhh at redhat.com>
 */
/**
  @file Misc. Quorum daemon context utilities / high-level functions
 */
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <string.h>
#include <errno.h>
#include <disk.h>
#include <platform.h>
#include <unistd.h>
#include <sys/time.h>
#include <time.h>


inline void
_diff_tv(struct timeval *dest, struct timeval *start, struct timeval *end)
{
	dest->tv_sec = end->tv_sec - start->tv_sec;
	dest->tv_usec = end->tv_usec - start->tv_usec;

	if (dest->tv_usec < 0) {
		dest->tv_usec += 1000000;
		dest->tv_sec--;
	}
}


/**
 *
 * Grab the uptime from /proc/uptime.
 * 
 * @param tv		Timeval struct to store time in.  The sec
 * 			field contains seconds, the usec field 
 * 			contains the hundredths-of-seconds (converted
 * 			to micro-seconds)
 * @return		-1 on failure, 0 on success.
 */
static inline int
getuptime(struct timeval *tv)
{
	FILE *fp;
	struct timeval junk;
	int rv;
	
	fp = fopen("/proc/uptime","r");
	if (!fp)
		return -1;

#if defined(__sparc__) || defined(__sparc64__)
	rv = fscanf(fp,"%ld.%d %ld.%d\n", &tv->tv_sec, &tv->tv_usec,
		    &junk.tv_sec, &junk.tv_usec);
#else
	rv = fscanf(fp,"%ld.%ld %ld.%ld\n", &tv->tv_sec, &tv->tv_usec,
		    &junk.tv_sec, &junk.tv_usec);
#endif
	fclose(fp);
	
	if (rv != 4) {
		return -1;
	}
	
	tv->tv_usec *= 10000;
	
	return 0;
}


inline int
get_time(struct timeval *tv, int use_uptime)
{
	if (use_uptime) {
		return getuptime(tv);
	} else {
		return gettimeofday(tv, NULL);
	}
}

 
/**
  Update write times and calculate a new average time
 */
void
qd_update_wtime(qd_ctx *ctx, struct timeval *newtime)
{
	int x;
	int max = HISTORY_LENGTH;
	uint64_t sum = 0;

	/* Store the thing */
	ctx->qc_writes++;
	ctx->qc_last[ctx->qc_writes % HISTORY_LENGTH].tv_sec = newtime->tv_sec;
	ctx->qc_last[ctx->qc_writes % HISTORY_LENGTH].tv_usec = newtime->tv_usec;

	if (ctx->qc_writes < HISTORY_LENGTH)
		max = ctx->qc_writes;

	for (x = 0; x < max; x++) {
		sum += (ctx->qc_last[x].tv_sec * 1000000);
		sum += ctx->qc_last[x].tv_usec;
	}

	sum /= max;

	ctx->qc_average.tv_sec = (sum / 1000000);
	ctx->qc_average.tv_usec = (sum % 1000000);
}


/**
  Write a status block to disk, given state, nodeid, message, and the
  membership mask.
 */
int
qd_write_status(qd_ctx *ctx, int nid, disk_node_state_t state,
		disk_msg_t *msg, memb_mask_t mask, memb_mask_t master)
{
	status_block_t ps;
	struct timeval start, end;
	int utime_ok = 1;

	if (!ctx) {
		errno = EINVAL;
		return -1;
	}

	if (nid <= 0) {
		errno = EINVAL;
		return -1;
	}

	ps.ps_magic = STATE_MAGIC_NUMBER;
	ps.ps_nodeid = nid;
	ps.ps_updatenode = ctx->qc_my_id;
	ps.pad0 = 0;
	ps.ps_timestamp = (uint64_t)time(NULL);
	ps.ps_state = (uint8_t)state;
	ps.pad1[0] = 0;
	ps.ps_flags = 0;
	ps.ps_score = 0;
	ps.ps_scoremax = 0;
	ps.ps_ca_sec = ctx->qc_average.tv_sec;
	ps.ps_ca_usec = ctx->qc_average.tv_usec;
	ps.ps_incarnation = ctx->qc_incarnation;
	if (mask) {
		memcpy(ps.ps_mask, mask, sizeof(memb_mask_t));
	} else {
		memset(ps.ps_mask, 0, sizeof(memb_mask_t));
	}
	if (master) {
		memcpy(ps.ps_master_mask, master, sizeof(memb_mask_t));
	} else {
		memset(ps.ps_master_mask, 0, sizeof(memb_mask_t));
	}

	if (ctx->qc_writes) {
		ps.ps_lc_sec =
		   ctx->qc_last[(ctx->qc_writes - 1) % HISTORY_LENGTH].tv_sec;
		ps.ps_lc_usec =
		   ctx->qc_last[(ctx->qc_writes - 1) % HISTORY_LENGTH].tv_usec;
	} else {
		ps.ps_lc_sec = ps.ps_lc_usec = 0;
	}
	ps.ps_nodeid = nid;

	/* Argh! */
	if (msg) {
		ps.ps_msg = msg->m_msg;
		ps.ps_seq = msg->m_seq;
		ps.ps_arg = msg->m_arg;
	} else {
		ps.ps_msg = 0;
		ps.ps_seq = 0;
		ps.ps_arg = 0;
	}

	if (get_time(&start, ctx->qc_flags&RF_UPTIME) < 0)
		utime_ok = 0;
	swab_status_block_t(&ps);
	if (qdisk_write(&ctx->qc_disk,
			qdisk_nodeid_offset(nid, ctx->qc_disk.d_blksz),
			&ps, sizeof(ps)) < 0) {
		printf("Error writing node ID block %d\n", nid);
		return -1;
	}
	if (utime_ok && (get_time(&end, ctx->qc_flags&RF_UPTIME) < 0))
		utime_ok = 0;

	if (utime_ok) {
		_diff_tv(&start,&start,&end);
	} else {
		/* Use heuristic */
		start.tv_sec = ctx->qc_average.tv_sec;
		start.tv_usec = ctx->qc_average.tv_usec;
	}
	qd_update_wtime(ctx, &start);

	return 0;
}


int
qd_print_status(target_info_t *disk, status_block_t *ps)
{
	int x;

	printf("Data @ offset %d:\n",
	       (int)qdisk_nodeid_offset(ps->ps_nodeid, disk->d_blksz));
	printf("status_block_t {\n");
	printf("\t.ps_magic = %08x;\n", (int)ps->ps_magic);
	printf("\t.ps_nodeid = %d;\n", (int)ps->ps_nodeid);
	printf("\t.ps_updatenode = %d;\n", (int)ps->ps_updatenode);
	printf("\t.pad0 = %d;\n", (int)ps->pad0);
	printf("\t.ps_timestamp = %llu;\n", (long long unsigned)
		ps->ps_timestamp);
	printf("\t.ps_state = %d;\n", ps->ps_state);
	printf("\t.pad1[0] = %d;\n", ps->pad1[0]);
	printf("\t.ps_flags = %d;\n", ps->ps_flags);
	printf("\t.ps_score = %d;\n", ps->ps_score);
	printf("\t.ps_scoremax = %d;\n", ps->ps_scoremax);
	printf("\t.ps_ca_sec = %d;\n", ps->ps_ca_sec);
	printf("\t.ps_ca_usec = %d;\n", ps->ps_ca_usec);
	printf("\t.ps_lc_sec = %d;\n", ps->ps_lc_sec);
	printf("\t.ps_lc_usec = %d;\n", ps->ps_lc_usec);
	printf("\t.ps_mask = 0x");
	for (x = (sizeof(memb_mask_t)-1); x >= 0; x--)
		printf("%02x", ps->ps_mask[x]);
	printf("\n");
	printf("\t.ps_master_mask = 0x");
	for (x = (sizeof(memb_mask_t)-1); x >= 0; x--)
		printf("%02x", ps->ps_mask[x]);
	printf("\n");

	printf("}\n");

	return 0;
}


int
qd_read_print_status(target_info_t *disk, int nid)
{
	status_block_t ps;

	if (!disk || disk->d_fd < 0) {
		errno = EINVAL;
		return -1;
	}

	if (nid <= 0) {
		errno = EINVAL;
		return -1;
	}

	if (qdisk_read(disk, qdisk_nodeid_offset(nid, disk->d_blksz), &ps,
			sizeof(ps)) < 0) {
		printf("Error reading node ID block %d\n", nid);
		return -1;
	}
	swab_status_block_t(&ps);
	qd_print_status(disk, &ps);

	return 0;
}


/**
  Generate a token based on the current system time.
 */
uint64_t
generate_token(void)
{
	uint64_t my_token = 0;
	struct timeval tv;

        while(my_token == 0) {
                gettimeofday(&tv, NULL);

                my_token = ((uint64_t) (tv.tv_sec) << 32) |
                        (uint64_t) (tv.tv_sec & 0x00000000ffffffff);
        }

	return my_token;
}


/**
  Initialize a quorum disk context, given a CMAN handle and a nodeid.
 */
int
qd_init(qd_ctx *ctx, cman_handle_t ch, int me)
{
	if (!ctx || !ch || !me) {
		errno = EINVAL;
		return -1;
	}	

	memset(ctx, 0, sizeof(*ctx));
	ctx->qc_incarnation = generate_token();
	ctx->qc_ch = ch;
	ctx->qc_my_id = me;
	ctx->qc_status_sock = -1;

	return 0;
}


/**
  Destroy a quorum disk context
 */
void
qd_destroy(qd_ctx *ctx)
{
	if (ctx->qc_my_id == 0)
		return;
	if (ctx->qc_device) {
		free(ctx->qc_device);
		ctx->qc_device = NULL;
	}
	qdisk_close(&ctx->qc_disk);
}
