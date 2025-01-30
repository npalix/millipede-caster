#include <event2/event.h>
#include <event2/buffer.h>
#include <event2/bufferevent.h>

#include "conf.h"
#include "jobs.h"
#include "redistribute.h"
#include "ntripcli.h"
#include "ntrip_task.h"

static void redistribute_end_cb(int ok, void *arg);

/*
 * Required lock: ntrip_state
 *
 * Switch client from a given source to another.
 */
int redistribute_switch_source(struct ntrip_state *this, char *new_mountpoint, pos_t *mountpoint_pos, struct livesource *livesource) {
	ntrip_log(this, LOG_INFO, "Switching virtual source from %s to %s", this->virtual_mountpoint, new_mountpoint);
	new_mountpoint = mystrdup(new_mountpoint);
	if (new_mountpoint == NULL)
		return -1;
	if (this->subscription) {
		livesource_del_subscriber(this);
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
redistribute_args_new(struct caster_state *caster, struct livesource *livesource, char *mountpoint, pos_t *mountpoint_pos, int reconnect_delay, int persistent) {
	struct redistribute_cb_args *this;
	this = (struct redistribute_cb_args *)malloc(sizeof(struct redistribute_cb_args));
	char *uri = (char *)strmalloc(strlen(mountpoint)+2);

	if (this == NULL || uri == NULL) {
		free(this);
		strfree(uri);
		return NULL;
	}

	/*
	 * Will set the host/port/uri later, as it will depend
	 * on the chosen caster when we start.
	 */
	this->task = ntrip_task_new(caster, NULL, 0, NULL, 0,
		persistent?caster->config->reconnect_delay:0, 0, 0,
		"source_fetcher", NULL);

	this->task->method = "GET";
	this->task->end_cb = redistribute_end_cb;
	this->task->end_cb_arg = this;
	this->task->restart_cb = (void(*)(void *))redistribute_source_stream;
	this->task->restart_cb_arg = this;

	sprintf(uri, "/%s", mountpoint);
	this->mountpoint = uri+1;
	this->uri = uri;
	if (mountpoint_pos)
		this->mountpoint_pos = *mountpoint_pos;
	else {
		this->mountpoint_pos.lat = 0.;
		this->mountpoint_pos.lon = 0.;
	}
	this->caster = caster;
	this->persistent = persistent;
	this->livesource = livesource;
	return this;
}

void
redistribute_args_free(struct redistribute_cb_args *this) {
	ntrip_task_stop(this->task);
	if (this->task)
		ntrip_task_free(this->task);
	strfree(this->uri);
	free(this);
}

/*
 * Redistribute source stream.
 * Step 2 -- start a connection attempt.
 *
 * Required lock: ntrip_state
 */
void
redistribute_source_stream(struct redistribute_cb_args *this) {
	struct sourcetable *sp = NULL;

	struct sourceline *s = stack_find_pullable(&this->caster->sourcetablestack, this->mountpoint, &sp);
	if (s == NULL) {
		logfmt(&this->caster->flog, LOG_WARNING, "Can't find pullable mountpoint %s", this->mountpoint);
		redistribute_args_free(this);
		return;
	}

	strfree(this->task->host);
	this->task->host = mystrdup(sp->caster);
	strfree((char *)this->task->uri);
	this->task->uri = mystrdup(this->uri);

	if (this->task->host == NULL || this->task->uri == NULL) {
		strfree(this->task->host);
		strfree((char *)this->task->uri);
		logfmt(&this->caster->flog, LOG_CRIT, "Can't allocate memory, cannot redistribute %s", this->mountpoint);
		return;
	}

	this->task->port = sp->port;
	this->task->tls = 0;
	this->task->read_timeout = this->caster->config->on_demand_source_timeout;
	this->task->write_timeout = this->caster->config->on_demand_source_timeout;

	if (ntripcli_start(this->caster, this->task->host, this->task->port, this->task->tls, this->task->uri,
		"source_fetcher", this->task, this->livesource, this->persistent) < 0) {
		logfmt(&this->caster->flog, LOG_CRIT, "ntripcli_start failed, cannot redistribute %s", this->mountpoint);
		return;
	}
}

/*
 * Redistribute source stream.
 * Step 3 -- end of connection.
 * Restart if necessary.
 *
 * Required lock: ntrip_state
 */
static void
redistribute_end_cb(int ok, void *arg) {
	struct redistribute_cb_args *this = (struct redistribute_cb_args *)arg;
	struct ntrip_state *st = this->task->st;
	this->task->st = NULL;
	if (!ok && st->own_livesource) {
		if (this->persistent) {
			livesource_set_state(this->livesource, LIVESOURCE_FETCH_PENDING);
			ntrip_task_reschedule(this->task, this);
		} else {
			ntrip_unregister_livesource(st);
			redistribute_args_free(this);
		}
	}
}
