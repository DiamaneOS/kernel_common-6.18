/* SPDX-License-Identifier: GPL-2.0-only */
/* DiamaneOS: a guest's empty fair queue needs no reservation timer. */
static void diamaneos_fair_idle(struct rq *rq)
{
	struct sched_dl_entity *server = &rq->fair_server;

	if (!IS_ENABLED(CONFIG_GUNYAH_GUEST) || rq->cfs.h_nr_queued ||
	    !dl_server_active(server))
		return;
	lockdep_assert_rq_held(rq);
	/* Preserve the zero-laxity wakeup case without refilling the budget. */
	if (server->runtime < 0)
		server->runtime = 0;
	dl_server_stop(server);
}
