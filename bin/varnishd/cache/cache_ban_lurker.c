/*-
 * Copyright (c) 2006 Verdens Gang AS
 * Copyright (c) 2006-2015 Varnish Software AS
 * All rights reserved.
 *
 * Author: Poul-Henning Kamp <phk@phk.freebsd.dk>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 */

#include "config.h"

#include "cache.h"
#include "cache_ban.h"

#include "hash/hash_slinger.h"
#include "vtim.h"

static struct objcore oc_marker = { .magic = OBJCORE_MAGIC, };
static unsigned ban_batch;
static unsigned ban_generation;

pthread_cond_t	ban_lurker_cond;

void
ban_kick_lurker(void)
{
	Lck_Lock(&ban_mtx);
	ban_generation++;
	AZ(pthread_cond_signal(&ban_lurker_cond));
	Lck_Unlock(&ban_mtx);
}

static void
ban_cleantail(void)
{
	struct ban *b;

	do {
		Lck_Lock(&ban_mtx);
		b = VTAILQ_LAST(&ban_head, banhead_s);
		if (b != VTAILQ_FIRST(&ban_head) && b->refcount == 0) {
			if (b->flags & BANS_FLAG_COMPLETED)
				VSC_C_main->bans_completed--;
			if (b->flags & BANS_FLAG_OBJ)
				VSC_C_main->bans_obj--;
			if (b->flags & BANS_FLAG_REQ)
				VSC_C_main->bans_req--;
			VSC_C_main->bans--;
			VSC_C_main->bans_deleted++;
			VTAILQ_REMOVE(&ban_head, b, list);
			VSC_C_main->bans_persisted_fragmentation +=
			    ban_len(b->spec);
			ban_info(BI_DROP, b->spec, ban_len(b->spec));
		} else {
			b = NULL;
		}
		Lck_Unlock(&ban_mtx);
		if (b != NULL)
			BAN_Free(b);
	} while (b != NULL);
}

/*--------------------------------------------------------------------
 * Our task here is somewhat tricky:  The canonical locking order is
 * objhead->mtx first, then ban_mtx, because that is the order which
 * makes most sense in HSH_Lookup(), but we come the other way.
 * We optimistically try to get them the other way, and get out of
 * the way if that fails, and retry again later.
 */

static struct objcore *
ban_lurker_getfirst(struct vsl_log *vsl, struct ban *bt)
{
	struct objhead *oh;
	struct objcore *oc;

	while (1) {
		Lck_Lock(&ban_mtx);
		oc = VTAILQ_FIRST(&bt->objcore);
		CHECK_OBJ_NOTNULL(oc, OBJCORE_MAGIC);
		if (oc == &oc_marker) {
			VTAILQ_REMOVE(&bt->objcore, oc, ban_list);
			Lck_Unlock(&ban_mtx);
			return (NULL);
		}
		oh = oc->objhead;
		CHECK_OBJ_NOTNULL(oh, OBJHEAD_MAGIC);
		if (!Lck_Trylock(&oh->mtx)) {
			if (oc->refcnt == 0) {
				Lck_Unlock(&oh->mtx);
			} else {
				/*
				 * We got the lock, and the oc is not being
				 * dismantled under our feet, run with it...
				 */
				AZ(oc->flags & OC_F_BUSY);
				oc->refcnt += 1;
				VTAILQ_REMOVE(&bt->objcore, oc, ban_list);
				VTAILQ_INSERT_TAIL(&bt->objcore, oc, ban_list);
				Lck_Unlock(&oh->mtx);
				Lck_Unlock(&ban_mtx);
				break;
			}
		}

		/* Try again, later */
		Lck_Unlock(&ban_mtx);
		VSC_C_main->bans_lurker_contention++;
		VSL_Flush(vsl, 0);
		VTIM_sleep(cache_param->ban_lurker_sleep);
	}
	return (oc);
}

static void
ban_lurker_test_ban(struct worker *wrk, struct vsl_log *vsl, struct ban *bt,
    struct banhead_s *obans)
{
	struct ban *bl, *bln;
	struct objcore *oc;
	unsigned tests;
	int i;

	/*
	 * First see if there is anything to do, and if so, insert marker
	 */
	Lck_Lock(&ban_mtx);
	oc = VTAILQ_FIRST(&bt->objcore);
	if (oc != NULL)
		VTAILQ_INSERT_TAIL(&bt->objcore, &oc_marker, ban_list);
	Lck_Unlock(&ban_mtx);
	if (oc == NULL)
		return;

	while (1) {
		if (++ban_batch > cache_param->ban_lurker_batch) {
			VTIM_sleep(cache_param->ban_lurker_sleep);
			ban_batch = 0;
		}
		oc = ban_lurker_getfirst(vsl, bt);
		if (oc == NULL)
			return;
		i = 0;
		VTAILQ_FOREACH_REVERSE_SAFE(bl, obans, banhead_s, l_list, bln) {
			if (bl->flags & BANS_FLAG_COMPLETED) {
				/* Ban was overtaken by new (dup) ban */
				VTAILQ_REMOVE(obans, bl, l_list);
				continue;
			}
			tests = 0;
			i = ban_evaluate(wrk, bl->spec, oc, NULL, &tests);
			VSC_C_main->bans_lurker_tested++;
			VSC_C_main->bans_lurker_tests_tested += tests;
			if (i)
				break;
		}
		if (i) {
			VSLb(vsl, SLT_ExpBan, "%u banned by lurker",
			    ObjGetXID(wrk, oc));

			EXP_Rearm(oc, oc->exp.t_origin, 0, 0, 0);
					// XXX ^ fake now
			VSC_C_main->bans_lurker_obj_killed++;
		}
		(void)HSH_DerefObjCore(wrk, &oc);
	}
}

/*--------------------------------------------------------------------
 * Ban lurker thread
 */

static int
ban_lurker_work(struct worker *wrk, struct vsl_log *vsl)
{
	struct ban *b, *bt;
	struct banhead_s obans;
	double d;
	int i;

	/* Make a list of the bans we can do something about */
	VTAILQ_INIT(&obans);
	Lck_Lock(&ban_mtx);
	b = ban_start;
	Lck_Unlock(&ban_mtx);
	i = 0;
	d = VTIM_real() - cache_param->ban_lurker_age;
	while (b != NULL) {
		if (b->flags & BANS_FLAG_COMPLETED) {
			;
		} else if (b->flags & BANS_FLAG_REQ) {
			;
		} else if (b == VTAILQ_LAST(&ban_head, banhead_s)) {
			;
		} else if (ban_time(b->spec) > d) {
			;
		} else {
			VTAILQ_INSERT_TAIL(&obans, b, l_list);
			i++;
		}
		b = VTAILQ_NEXT(b, list);
	}
	if (DO_DEBUG(DBG_LURKER))
		VSLb(vsl, SLT_Debug, "lurker: %d actionable bans", i);
	if (i == 0)
		return (0);

	/* Go though all the bans to test the objects */
	VTAILQ_FOREACH_REVERSE(bt, &ban_head, banhead_s, list) {
		if (bt == VTAILQ_LAST(&obans, banhead_s)) {
			if (DO_DEBUG(DBG_LURKER))
				VSLb(vsl, SLT_Debug,
				    "Lurk bt completed %p", bt);
			Lck_Lock(&ban_mtx);
			/* We can be raced by a new ban */
			if (!(bt->flags & BANS_FLAG_COMPLETED))
				ban_mark_completed(bt);
			Lck_Unlock(&ban_mtx);
			VTAILQ_REMOVE(&obans, bt, l_list);
			if (VTAILQ_EMPTY(&obans))
				break;
		}
		if (DO_DEBUG(DBG_LURKER))
			VSLb(vsl, SLT_Debug, "Lurk bt %p", bt);
		ban_lurker_test_ban(wrk, vsl, bt, &obans);
		if (VTAILQ_EMPTY(&obans))
			break;
	}
	return (1);
}

void * __match_proto__(bgthread_t)
ban_lurker(struct worker *wrk, void *priv)
{
	struct vsl_log vsl;
	volatile double d;

	CHECK_OBJ_NOTNULL(wrk, WORKER_MAGIC);
	AZ(priv);

	VSL_Setup(&vsl, NULL, 0);

	while (!ban_shutdown) {
		d = cache_param->ban_lurker_sleep;
		if (d <= 0.0 || !ban_lurker_work(wrk, &vsl))
			d = 0.609;	// Random, non-magic
		ban_cleantail();
		d += VTIM_real();
		Lck_Lock(&ban_mtx);
		(void)Lck_CondWait(&ban_lurker_cond, &ban_mtx, d);
		Lck_Unlock(&ban_mtx);
	}
	pthread_exit(0);
	NEEDLESS_RETURN(NULL);
}