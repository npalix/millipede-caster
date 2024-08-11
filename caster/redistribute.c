#include <event2/event.h>
#include <event2/buffer.h>
#include <event2/bufferevent.h>

#include "conf.h"
#include "redistribute.h"
#include "ntripcli.h"
#include "ntripsrv.h"

/*
 * Required lock: ntrip_state
 *
 * Switch client from a given source to another.
 */
int redistribute_switch_source(struct ntrip_state *this, char *new_mountpoint, pos_t *mountpoint_pos, struct livesource *livesource) {
	ntrip_log(this, LOG_INFO, "Switching virtual source from %s to %s\n", this->virtual_mountpoint, new_mountpoint);
	new_mountpoint = mystrdup(new_mountpoint);
	if (new_mountpoint == NULL)
		return -1;
	if (this->subscription) {
		livesource_del_subscriber(this->subscription, this);
	}
	this->subscription = livesource_add_subscriber(livesource, this);
	this->subscription->virtual = 1;
	if (this->virtual_mountpoint)
		strfree(this->virtual_mountpoint);
	this->virtual_mountpoint = new_mountpoint;
	this->mountpoint_pos = *mountpoint_pos;
	return 0;
}

/*
 * Redistribute source stream.
 * Step 1 -- prepare argument structure.
 */
struct redistribute_cb_args *
redistribute_args_new(struct ntrip_state *st, char *mountpoint, pos_t *mountpoint_pos, int reconnect_delay, int persistent) {
	struct redistribute_cb_args *redis_args;
	redis_args = (struct redistribute_cb_args *)malloc(sizeof(struct redistribute_cb_args));
	char *dup_mountpoint = mystrdup(mountpoint);
	if (redis_args != NULL && dup_mountpoint != NULL) {
		redis_args->mountpoint = dup_mountpoint;
		redis_args->mountpoint_pos = *mountpoint_pos;
		redis_args->caster = st->caster;
		redis_args->requesting_st = st;
		redis_args->source_st = NULL;
		redis_args->ev = NULL;
		redis_args->persistent = persistent;
		return redis_args;
	} else {
		if (redis_args)
			free(redis_args);
		if (dup_mountpoint)
			strfree(dup_mountpoint);
		return NULL;
	}
}

void
redistribute_args_free(struct redistribute_cb_args *this) {
	if (this->ev)
		event_del(this->ev);
	free(this->mountpoint);
	free(this);
}

/*
 * Redistribute source stream.
 * Step 2 (optional) -- schedule for later
 */
int
redistribute_schedule(struct ntrip_state *st, struct redistribute_cb_args *redis_args) {
	struct timeval timeout_interval = { st->caster->config->reconnect_delay, 0 };
	struct event *ev = event_new(st->caster->base, -1, 0, redistribute_cb, redis_args);
	if (ev != NULL) {
		ntrip_log(st, LOG_INFO, "Scheduling retry callback for source %s in %d seconds\n", redis_args->mountpoint, st->caster->config->reconnect_delay);
		redis_args->ev = ev;
		event_add(ev, &timeout_interval);
		return 0;
	} else {
		ntrip_log(st, LOG_CRIT, "Can't schedule retry callback for source %s in %d seconds, canceling\n", redis_args->mountpoint, st->caster->config->reconnect_delay);
		if (redis_args->requesting_st) {
			struct ntrip_state *st = redis_args->requesting_st;
			bufferevent_lock(st->bev);
			redis_args->requesting_st->callback_subscribe_arg = NULL;
			redis_args->requesting_st = NULL;
			bufferevent_unlock(st->bev);
		}
		redistribute_args_free(redis_args);
		return -1;
	}
}

/*
 * Redistribute source stream.
 *
 * Step 3 (optional) -- called back after a timeout
 */
void
redistribute_cb(evutil_socket_t fd, short what, void *cbarg) {
	struct redistribute_cb_args *redis_args = (struct redistribute_cb_args *)cbarg;
	struct caster_state *caster = redis_args->caster;
	logfmt(&caster->flog, "Trying to restart source %s\n", redis_args->mountpoint);
	event_del(redis_args->ev);
	redis_args->ev = NULL;
	redistribute_source_stream(redis_args, NULL);
}

/*
 * Redistribute source stream.
 * Step 4 -- start a connection attempt.
 *
 * Required lock: ntrip_state
 */
void
redistribute_source_stream(struct redistribute_cb_args *redis_args,
	void (*switch_source_cb)(struct redistribute_cb_args *redis_args, int success))
{
	struct bufferevent *bev;

	if (threads)
		bev = bufferevent_socket_new(redis_args->caster->base, -1, BEV_OPT_CLOSE_ON_FREE|BEV_OPT_THREADSAFE);
	else
		bev = bufferevent_socket_new(redis_args->caster->base, -1, BEV_OPT_CLOSE_ON_FREE);

	if (bev == NULL) {
		logfmt(&redis_args->caster->flog, "Out of memory, cannot redistribute %s\n", redis_args->mountpoint);
		return;
	}

	struct sourcetable *sp = NULL;

	/*
	 * Create new client state.
	 */
	struct ntrip_state *st = ntrip_new(redis_args->caster, NULL, 0, redis_args->mountpoint);
	if (st == NULL) {
		logfmt(&redis_args->caster->flog, "Out of memory, cannot redistribute %s\n", redis_args->mountpoint);
		return;
	}
	st->bev = bev;
	st->type = "source_fetcher";

	struct sourceline *s = stack_find_pullable(&st->caster->sourcetablestack, redis_args->mountpoint, &sp);
	if (s == NULL) {
		logfmt(&redis_args->caster->flog, "Can't find pullable mountpoint %s\n", redis_args->mountpoint);
		ntrip_deferred_free(st, "redistribute_source_stream");
		return;
	}
	st->host = sp->caster;
	st->port = sp->port;
	st->redistribute = 1;
	st->persistent = redis_args->persistent;
	redis_args->source_st = st;

	logfmt(&redis_args->caster->flog, "Starting socket connect to %s:%d for /%s\n", st->host, st->port, redis_args->mountpoint);

	/*
	 * Set-up the callback for when our task completes.
	 */
	st->callback_subscribe = switch_source_cb;
	st->callback_subscribe_arg = redis_args;

	if (threads)
		bufferevent_setcb(bev, ntripcli_workers_readcb, ntripcli_workers_writecb, ntripcli_workers_eventcb, st);
	else
		bufferevent_setcb(bev, ntripcli_readcb, ntripcli_writecb, ntripcli_eventcb, st);

	bufferevent_enable(bev, EV_READ|EV_WRITE);
        struct timeval read_timeout = { st->caster->config->on_demand_source_timeout, 0 };
        struct timeval write_timeout = { st->caster->config->on_demand_source_timeout, 0 };
        bufferevent_set_timeouts(bev, &read_timeout, &write_timeout);
	gettimeofday(&redis_args->t0, NULL);
	bufferevent_socket_connect_hostname(bev, redis_args->caster->dns_base, AF_UNSPEC, sp->caster, sp->port);
}

/*
 * Redistribute source stream.
 * Last step (optional): switch the requester to the source.
 */
void
redistribute_switch_source_cb(struct redistribute_cb_args *redis_args, int success) {
	struct timeval t1;
	struct ntrip_state *st = redis_args->requesting_st;

	redis_args->source_st->callback_subscribe_arg = NULL;
	if (redis_args->requesting_st)
		redis_args->requesting_st->callback_subscribe_arg = NULL;

	if (st == NULL) {
		logfmt(&redis_args->caster->flog, "switch source callback: requester went away\n");
		redistribute_args_free(redis_args);
		return;
	}

	logfmt(&redis_args->caster->flog, "switch source callback\n");


	if (success) {
		struct livesource *livesource = livesource_find(st->caster, redis_args->mountpoint);
		if (livesource) {
			/*
			 * redistribute_switch_source will lock st in the right order
			 */
			redistribute_switch_source(st, redis_args->mountpoint, &redis_args->mountpoint_pos, livesource);
			gettimeofday(&t1, NULL);
			timersub(&t1, &redis_args->t0, &t1);

			ntrip_log(st, LOG_INFO, "On-demand source subscribed from %s:%d/%s, %.3f ms\n",
				redis_args->source_st->host,
				redis_args->source_st->port,
				redis_args->mountpoint,
				t1.tv_sec*1000 + t1.tv_usec/1000.);
		} else
			ntrip_log(st, LOG_INFO, "callback called but no on-demand source ready %p\n", st);
	}

	/*
	 * We need to take an explicit lock on st since this callback is called in the
	 * context of another ntrip_state.
	 */
	struct bufferevent *bev = st->bev;
	bufferevent_lock(bev);
	redistribute_args_free(redis_args);

	if (!success) {
		/*
		 * Failed to get the requested source.
		 *
		 * Close the requesting connection.
		 * We should do something more clever here in the case of "virtual" bases,
		 * since we can try another source.
		 */
		ntrip_deferred_free(st, "redistribute_switch_source_cb");
	}
	bufferevent_unlock(bev);
}
