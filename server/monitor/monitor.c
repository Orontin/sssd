/*
   SSSD

   Service monitor

   Copyright (C) Simo Sorce			2008

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#define _GNU_SOURCE
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/time.h>
#include <time.h>
#include "util/util.h"
#include "popt.h"
#include "tevent.h"
#include "confdb/confdb.h"
#include "db/sysdb.h"
#include "monitor/monitor.h"
#include "dbus/dbus.h"
#include "sbus/sssd_dbus.h"
#include "monitor/monitor_interfaces.h"

/* ping time cannot be less then once every few seconds or the
 * monitor will get crazy hammering children with messages */
#define MONITOR_DEF_PING_TIME 10
#define MONITOR_CONF_ENTRY "config/services/monitor"

struct mt_conn {
    struct sbus_conn_ctx *conn_ctx;
    struct mt_svc *svc_ptr;
};

struct mt_svc {
    struct mt_svc *prev;
    struct mt_svc *next;

    struct mt_conn *mt_conn;
    struct mt_ctx *mt_ctx;

    char *provider;
    char *command;
    char *name;
    char *identity;
    pid_t pid;

    int ping_time;

    int restarts;
    time_t last_restart;
    time_t last_pong;

    int debug_level;
};

struct mt_ctx {
    struct tevent_context *ev;
    struct confdb_ctx *cdb;
    struct btreemap *dom_map;
    char **services;
    struct mt_svc *svc_list;
    struct sbus_srv_ctx *sbus_srv;

    int service_id_timeout;
};

static int start_service(struct mt_svc *mt_svc);

static int dbus_service_init(struct sbus_conn_ctx *conn_ctx, void *data);
static void identity_check(DBusPendingCall *pending, void *data);

static int service_send_ping(struct mt_svc *svc);
static void ping_check(DBusPendingCall *pending, void *data);

static int service_check_alive(struct mt_svc *svc);

static void set_tasks_checker(struct mt_svc *srv);
static void set_global_checker(struct mt_ctx *ctx);

/* dbus_get_monitor_version
 * Return the monitor version over D-BUS */
static int dbus_get_monitor_version(DBusMessage *message,
                                    struct sbus_conn_ctx *sconn)
{
    const char *version = MONITOR_VERSION;
    DBusMessage *reply;
    dbus_bool_t ret;

    reply = dbus_message_new_method_return(message);
    if (!reply) return ENOMEM;
    ret = dbus_message_append_args(reply, DBUS_TYPE_STRING,
                                   &version, DBUS_TYPE_INVALID);
    if (!ret) {
        dbus_message_unref(reply);
        return EIO;
    }

    /* send reply back */
    sbus_conn_send_reply(sconn, reply);
    dbus_message_unref(reply);

    return EOK;
}

struct sbus_method monitor_methods[] = {
    { MONITOR_METHOD_VERSION, dbus_get_monitor_version},
    {NULL, NULL}
};

/* monitor_dbus_init
 * Set up the monitor service as a D-BUS Server */
static int monitor_dbus_init(struct mt_ctx *ctx)
{
    struct sbus_method_ctx *sd_ctx;
    struct sbus_srv_ctx *sbus_srv;
    char *sbus_address;
    char *default_monitor_address;
    int ret;

    default_monitor_address = talloc_asprintf(ctx, "unix:path=%s/%s",
                                              PIPE_PATH, SSSD_SERVICE_PIPE);
    if (!default_monitor_address) {
        return ENOMEM;
    }

    ret = confdb_get_string(ctx->cdb, ctx,
                            MONITOR_CONF_ENTRY, "sbusAddress",
                            default_monitor_address, &sbus_address);
    if (ret != EOK) {
        talloc_free(default_monitor_address);
        return ret;
    }
    talloc_free(default_monitor_address);

    sd_ctx = talloc_zero(ctx, struct sbus_method_ctx);
    if (!sd_ctx) {
        talloc_free(sbus_address);
        return ENOMEM;
    }

    /* Set up globally-available D-BUS methods */
    sd_ctx->interface = talloc_strdup(sd_ctx, MONITOR_DBUS_INTERFACE);
    if (!sd_ctx->interface) {
        talloc_free(sbus_address);
        talloc_free(sd_ctx);
        return ENOMEM;
    }
    sd_ctx->path = talloc_strdup(sd_ctx, MONITOR_DBUS_PATH);
    if (!sd_ctx->path) {
        talloc_free(sbus_address);
        talloc_free(sd_ctx);
        return ENOMEM;
    }
    sd_ctx->methods = monitor_methods;
    sd_ctx->message_handler = sbus_message_handler;

    ret = sbus_new_server(ctx, ctx->ev, sd_ctx, &sbus_srv, sbus_address, dbus_service_init, ctx);
    ctx->sbus_srv = sbus_srv;

    return ret;
}

static void svc_try_restart(struct mt_svc *svc, time_t now)
{
    int ret;

    DLIST_REMOVE(svc->mt_ctx->svc_list, svc);
    if (svc->last_restart != 0) {
        if ((now - svc->last_restart) > 30) { /* TODO: get val from config */
            /* it was long ago reset restart threshold */
            svc->restarts = 0;
        }
    }

    /* restart the process */
    if (svc->restarts > 3) { /* TODO: get val from config */
        DEBUG(0, ("Process [%s], definitely stopped!\n", svc->name));
        talloc_free(svc);
        return;
    }

    ret = start_service(svc);
    if (ret != EOK) {
        DEBUG(0,("Failed to restart service '%s'\n", svc->name));
        talloc_free(svc);
        return;
    }

    svc->restarts++;
    svc->last_restart = now;
    return;
}

static void tasks_check_handler(struct tevent_context *ev,
                                struct tevent_timer *te,
                                struct timeval t, void *ptr)
{
    struct mt_svc *svc = talloc_get_type(ptr, struct mt_svc);
    time_t now = time(NULL);
    bool process_alive = true;
    int ret;

    ret = service_check_alive(svc);
    switch (ret) {
    case EOK:
        /* all fine */
        break;

    case ECHILD:
        DEBUG(1,("Process (%s) is stopped!\n", svc->name));
        process_alive = false;
        break;

    default:
        /* TODO: should we tear down it ? */
        DEBUG(1,("Checking for service %s(%d) failed!!\n",
                 svc->name, svc->pid));
        break;
    }

    if (process_alive) {
        ret = service_send_ping(svc);
        switch (ret) {
        case EOK:
            /* all fine */
            break;

        case ENXIO:
            DEBUG(1,("Child (%s) not responding! (yet)\n", svc->name));
            break;

        default:
            /* TODO: should we tear it down ? */
            DEBUG(1,("Sending a message to service (%s) failed!!\n", svc->name));
            break;
        }

        if (svc->last_pong != 0) {
            if ((now - svc->last_pong) > 30) { /* TODO: get val from config */
                /* too long since we last heard of this process */
                ret = kill(svc->pid, SIGUSR1);
                if (ret != EOK) {
                    DEBUG(0,("Sending signal to child (%s:%d) failed! "
                             "Ignore and pretend child is dead.\n",
                             svc->name, svc->pid));
                }
                process_alive = false;
            }
        }

    }

    if (!process_alive) {
        svc_try_restart(svc, now);
        return;
    }

    /* all fine, set up the task checker again */
    set_tasks_checker(svc);
}

static void set_tasks_checker(struct mt_svc *svc)
{
    struct tevent_timer *te = NULL;
    struct timeval tv;

    gettimeofday(&tv, NULL);
    tv.tv_sec += svc->ping_time;
    tv.tv_usec = 0;
    te = tevent_add_timer(svc->mt_ctx->ev, svc, tv, tasks_check_handler, svc);
    if (te == NULL) {
        DEBUG(0, ("failed to add event, monitor offline for [%s]!\n",
                  svc->name));
        /* FIXME: shutdown ? */
    }
}

static void global_checks_handler(struct tevent_context *ev,
                                  struct tevent_timer *te,
                                  struct timeval t, void *ptr)
{
    struct mt_ctx *ctx = talloc_get_type(ptr, struct mt_ctx);
    struct mt_svc *svc;
    int status;
    pid_t pid;

    errno = 0;
    pid = waitpid(0, &status, WNOHANG);
    if (pid == 0) {
        goto done;
    }

    if (pid == -1) {
        DEBUG(0, ("waitpid returned -1 (errno:%d[%s])\n",
                  errno, strerror(errno)));
        goto done;
    }

    /* let's see if it is a known service, and try to restart it */
    for (svc = ctx->svc_list; svc; svc = svc->next) {
        if (svc->pid == pid) {
            time_t now = time(NULL);
            DEBUG(1, ("Service [%s] did exit\n", svc->name));
            svc_try_restart(svc, now);
            goto done;
        }
    }
    if (svc == NULL) {
        DEBUG(0, ("Unknown child (%d) did exit\n", pid));
    }

done:
    set_global_checker(ctx);
}

static void set_global_checker(struct mt_ctx *ctx)
{
    struct tevent_timer *te = NULL;
    struct timeval tv;

    gettimeofday(&tv, NULL);
    tv.tv_sec += 1; /* once a second */
    tv.tv_usec = 0;
    te = tevent_add_timer(ctx->ev, ctx, tv, global_checks_handler, ctx);
    if (te == NULL) {
        DEBUG(0, ("failed to add global checker event! PANIC TIME!\n"));
        /* FIXME: is this right ? shoulkd we try to clean up first ?*/
        exit(-1);
    }
}

int get_monitor_config(struct mt_ctx *ctx)
{
    int ret;

    ret = confdb_get_int(ctx->cdb, ctx,
                         MONITOR_CONF_ENTRY, "sbusTimeout",
                         -1, &ctx->service_id_timeout);
    if (ret != EOK) {
        return ret;
    }

    ret = confdb_get_param(ctx->cdb, ctx,
                           "config/services", "activeServices",
                           &ctx->services);

    if (ctx->services[0] == NULL) {
        DEBUG(0, ("No services configured!\n"));
        return EINVAL;
    }

    return EOK;
}

int monitor_process_init(TALLOC_CTX *mem_ctx,
                         struct tevent_context *event_ctx,
                         struct confdb_ctx *cdb)
{
    struct mt_ctx *ctx;
    struct mt_svc *svc;
    struct sysdb_ctx *sysdb;
    const char **doms;
    int dom_count;
    char *path;
    int ret, i;

    ctx = talloc_zero(mem_ctx, struct mt_ctx);
    if (!ctx) {
        DEBUG(0, ("fatal error initializing monitor!\n"));
        return ENOMEM;
    }
    ctx->ev = event_ctx;
    ctx->cdb = cdb;

    ret = get_monitor_config(ctx);
    if (ret != EOK)
        return ret;

    /* Avoid a startup race condition between InfoPipe
     * and NSS. If the sysdb doesn't exist yet, both
     * will try to create it at the same time. So
     * we'll have the monitor create it before either of
     * those processes start.
     */
    ret = sysdb_init(mem_ctx, ctx->ev, ctx->cdb,
                     NULL, &sysdb);
    if (ret != EOK)
        return ret;
    talloc_free(sysdb);

    /* Initialize D-BUS Server
     * The monitor will act as a D-BUS server for all
     * SSSD processes */
    ret = monitor_dbus_init(ctx);
    if (ret != EOK) {
        return ret;
    }

    /* start all services */
    for (i = 0; ctx->services[i]; i++) {

        svc = talloc_zero(ctx, struct mt_svc);
        if (!svc) {
            talloc_free(ctx);
            return ENOMEM;
        }
        svc->mt_ctx = ctx;

        svc->name = talloc_strdup(svc, ctx->services[i]);
        if (!svc->name) {
            talloc_free(ctx);
            return ENOMEM;
        }

        svc->identity = talloc_strdup(svc, ctx->services[i]);
        if (!svc->identity) {
            talloc_free(ctx);
            return ENOMEM;
        }

        path = talloc_asprintf(svc, "config/services/%s", svc->name);
        if (!path) {
            talloc_free(ctx);
            return ENOMEM;
        }

        ret = confdb_get_string(cdb, svc, path, "command",
                                NULL, &svc->command);
        if (ret != EOK) {
            DEBUG(0,("Failed to start service '%s'\n", svc->name));
            talloc_free(svc);
            continue;
        }

        if (!svc->command) {
            svc->command = talloc_asprintf(svc, "%s/sssd_%s -d %d",
                                           SSSD_LIBEXEC_PATH, svc->name,
                                           debug_level);
            if (!svc->command) {
                talloc_free(ctx);
                return ENOMEM;
            }
        }

        ret = confdb_get_int(cdb, svc, path, "timeout",
                             MONITOR_DEF_PING_TIME, &svc->ping_time);
        if (ret != EOK) {
            DEBUG(0,("Failed to start service '%s'\n", svc->name));
            talloc_free(svc);
            continue;
        }

        talloc_free(path);

        /* Add this service to the queue to be started once the monitor
         * enters its mainloop.
         */
        ret = start_service(svc);
        if (ret != EOK) {
            DEBUG(0,("Failed to start service '%s'\n", svc->name));
            talloc_free(svc);
            continue;
        }
    }

    /* now start the data providers */
    ret = confdb_get_domains_list(cdb, ctx,
                                  &(ctx->dom_map), &doms, &dom_count);
    if (ret != EOK) {
        DEBUG(2, ("No domains configured. LOCAL should always exist!\n"));
        return ret;
    }

    for (i = 0; i < dom_count; i++) {
        svc = talloc_zero(ctx, struct mt_svc);
        if (!svc) {
            talloc_free(ctx);
            return ENOMEM;
        }
        svc->mt_ctx = ctx;

        svc->name = talloc_strdup(svc, doms[i]);
        if (!svc->name) {
            talloc_free(ctx);
            return ENOMEM;
        }

        svc->identity = talloc_asprintf(svc, "%%BE_%s", svc->name);
        if (!svc->identity) {
            talloc_free(ctx);
            return ENOMEM;
        }

        path = talloc_asprintf(svc, "config/domains/%s", doms[i]);
        if (!path) {
            talloc_free(ctx);
            return ENOMEM;
        }

        ret = confdb_get_string(cdb, svc, path,
                                "provider", NULL, &svc->provider);
        if (ret != EOK) {
            DEBUG(0, ("Failed to find provider from [%s] configuration\n", doms[i]));
            talloc_free(svc);
            continue;
        }

        ret = confdb_get_string(cdb, svc, path,
                                "command", NULL, &svc->command);
        if (ret != EOK) {
            DEBUG(0, ("Failed to find command from [%s] configuration\n", doms[i]));
            talloc_free(svc);
            continue;
        }

        ret = confdb_get_int(cdb, svc, path, "timeout",
                             MONITOR_DEF_PING_TIME, &svc->ping_time);
        if (ret != EOK) {
            DEBUG(0,("Failed to start service '%s'\n", svc->name));
            talloc_free(svc);
            continue;
        }

        talloc_free(path);

        /* if no provider is present do not run the domain */
        if (!svc->provider) {
            talloc_free(svc);
            continue;
        }

        /* if there are no custom commands, build a default one */
        if (!svc->command) {
            svc->command = talloc_asprintf(svc,
                                "%s/sssd_be -d %d --provider %s --domain %s",
                                SSSD_LIBEXEC_PATH, debug_level,
                                svc->provider, svc->name);
            if (!svc->command) {
                talloc_free(ctx);
                return ENOMEM;
            }
        }

        ret = start_service(svc);
        if (ret != EOK) {
            DEBUG(0,("Failed to start provider for '%s'\n", doms[i]));
            talloc_free(svc);
            continue;
        }
    }

    /* now start checking for global events */
    set_global_checker(ctx);

    return EOK;
}

static int mt_conn_destructor(void *ptr)
{
    struct mt_conn *mt_conn;
    struct mt_svc *svc;

    mt_conn = talloc_get_type(ptr, struct mt_conn);
    svc = mt_conn->svc_ptr;

    /* now clear up so that the rest of the code will know there
     * is no connection attached to the service anymore */
    svc->mt_conn = NULL;

    return 0;
}

/*
 * dbus_service_init
 * This function should initiate a query to the newly connected
 * service to discover the service's identity (invoke the getIdentity
 * method on the new client). The reply callback for this request
 * should set the connection destructor appropriately.
 */
static int dbus_service_init(struct sbus_conn_ctx *conn_ctx, void *data)
{
    struct mt_ctx *ctx;
    struct mt_svc *svc;
    struct mt_conn *mt_conn;
    DBusMessage *msg;
    DBusPendingCall *pending_reply;
    DBusConnection *conn;
    dbus_bool_t dbret;

    DEBUG(3, ("Initializing D-BUS Service\n"));

    ctx = talloc_get_type(data, struct mt_ctx);
    conn = sbus_get_connection(conn_ctx);

    /* hang off this memory to the connection so that when the connection
     * is freed we can call a destructor to clear up the structure and
     * have a way to know we need to restart the service */
    mt_conn = talloc(conn_ctx, struct mt_conn);
    if (!mt_conn) {
        DEBUG(0,("Out of memory?!\n"));
        talloc_free(conn_ctx);
        return ENOMEM;
    }
    mt_conn->conn_ctx = conn_ctx;

    /* at this stage we still do not know what service is this
     * we will know only after we get its identity, so we make
     * up a temporary fake service and complete the operation
     * when we receive the reply */
    svc = talloc_zero(mt_conn, struct mt_svc);
    if (!svc) {
        talloc_free(conn_ctx);
        return ENOMEM;
    }
    svc->mt_ctx = ctx;
    svc->mt_conn = mt_conn;

    mt_conn->svc_ptr = svc;
    talloc_set_destructor((TALLOC_CTX *)mt_conn, mt_conn_destructor);

    /*
     * Set up identity request
     * This should be a well-known path and method
     * for all services
     */
    msg = dbus_message_new_method_call(NULL,
                                       SERVICE_PATH,
                                       SERVICE_INTERFACE,
                                       SERVICE_METHOD_IDENTITY);
    if (msg == NULL) {
        DEBUG(0,("Out of memory?!\n"));
        talloc_free(conn_ctx);
        return ENOMEM;
    }
    dbret = dbus_connection_send_with_reply(conn, msg, &pending_reply,
                                            ctx->service_id_timeout);
    if (!dbret) {
        /*
         * Critical Failure
         * We can't communicate on this connection
         * We'll drop it using the default destructor.
         */
        DEBUG(0, ("D-BUS send failed.\n"));
        dbus_message_unref(msg);
        talloc_free(conn_ctx);
        return EIO;
    }

    /* Set up the reply handler */
    dbus_pending_call_set_notify(pending_reply, identity_check, svc, NULL);
    dbus_message_unref(msg);

    return EOK;
}

static void identity_check(DBusPendingCall *pending, void *data)
{
    struct mt_svc *fake_svc;
    struct mt_svc *svc;
    struct sbus_conn_ctx *conn_ctx;
    DBusMessage *reply;
    DBusError dbus_error;
    dbus_uint16_t svc_ver;
    char *svc_name;
    dbus_bool_t ret;
    int type;

    fake_svc = talloc_get_type(data, struct mt_svc);
    conn_ctx = fake_svc->mt_conn->conn_ctx;
    dbus_error_init(&dbus_error);

    reply = dbus_pending_call_steal_reply(pending);
    if (!reply) {
        /* reply should never be null. This function shouldn't be called
         * until reply is valid or timeout has occurred. If reply is NULL
         * here, something is seriously wrong and we should bail out.
         */
        DEBUG(0, ("Serious error. A reply callback was called but no reply was received and no timeout occurred\n"));

        /* Destroy this connection */
        sbus_disconnect(conn_ctx);
        goto done;
    }

    type = dbus_message_get_type(reply);
    switch (type) {
    case DBUS_MESSAGE_TYPE_METHOD_RETURN:
        ret = dbus_message_get_args(reply, &dbus_error,
                                    DBUS_TYPE_STRING, &svc_name,
                                    DBUS_TYPE_UINT16, &svc_ver,
                                    DBUS_TYPE_INVALID);
        if (!ret) {
            DEBUG(1,("Failed, to parse message, killing connection\n"));
            if (dbus_error_is_set(&dbus_error)) dbus_error_free(&dbus_error);
            sbus_disconnect(conn_ctx);
            goto done;
        }

        /* search this service in the list */
        svc = fake_svc->mt_ctx->svc_list;
        while (svc) {
            ret = strcasecmp(svc->identity, svc_name);
            if (ret == 0) {
                break;
            }
            svc = svc->next;
        }
        if (!svc) {
            DEBUG(0,("Unable to find peer [%s] in list of services, killing connection!\n", svc_name));
            sbus_disconnect(conn_ctx);
            goto done;
        }

        /* transfer all from the fake service and get rid of it */
        fake_svc->mt_conn->svc_ptr = svc;
        svc->mt_conn = fake_svc->mt_conn;
        talloc_free(fake_svc);

        DEBUG(1, ("Service %s connected\n", svc->name));

        /* Set up the destructor for this service */
        break;

    case DBUS_MESSAGE_TYPE_ERROR:
        DEBUG(0,("getIdentity returned an error [%s], closing connection.\n",
                 dbus_message_get_error_name(reply)));
        /* Falling through to default intentionally*/
    default:
        /*
         * Timeout or other error occurred or something
         * unexpected happened.
         * It doesn't matter which, because either way we
         * know that this connection isn't trustworthy.
         * We'll destroy it now.
         */
        sbus_disconnect(conn_ctx);
        return;
    }

done:
    dbus_pending_call_unref(pending);
    dbus_message_unref(reply);
}

/* service_send_ping
 * this function send a dbus ping to a service.
 * It returns EOK if all is fine or ENXIO if the connection is
 * not available (either not yet set up or teared down).
 * Returns e generic error in other cases.
 */
static int service_send_ping(struct mt_svc *svc)
{
    DBusMessage *msg;
    DBusPendingCall *pending_reply;
    DBusConnection *conn;
    dbus_bool_t dbret;

    if (!svc->mt_conn) {
        return ENXIO;
    }

    DEBUG(4,("Pinging %s\n", svc->name));

    conn = sbus_get_connection(svc->mt_conn->conn_ctx);

    /*
     * Set up identity request
     * This should be a well-known path and method
     * for all services
     */
    msg = dbus_message_new_method_call(NULL,
                                       SERVICE_PATH,
                                       SERVICE_INTERFACE,
                                       SERVICE_METHOD_PING);
    if (!msg) {
        DEBUG(0,("Out of memory?!\n"));
        talloc_free(svc->mt_conn->conn_ctx);
        return ENOMEM;
    }

    dbret = dbus_connection_send_with_reply(conn, msg, &pending_reply,
                                            svc->mt_ctx->service_id_timeout);
    if (!dbret) {
        /*
         * Critical Failure
         * We can't communicate on this connection
         * We'll drop it using the default destructor.
         */
        DEBUG(0, ("D-BUS send failed.\n"));
        talloc_free(svc->mt_conn->conn_ctx);
        return EIO;
    }

    /* Set up the reply handler */
    dbus_pending_call_set_notify(pending_reply, ping_check, svc, NULL);
    dbus_message_unref(msg);

    return EOK;
}

static void ping_check(DBusPendingCall *pending, void *data)
{
    struct mt_svc *svc;
    struct sbus_conn_ctx *conn_ctx;
    DBusMessage *reply;
    const char *dbus_error_name;
    int type;

    svc = talloc_get_type(data, struct mt_svc);
    conn_ctx = svc->mt_conn->conn_ctx;

    reply = dbus_pending_call_steal_reply(pending);
    if (!reply) {
        /* reply should never be null. This function shouldn't be called
         * until reply is valid or timeout has occurred. If reply is NULL
         * here, something is seriously wrong and we should bail out.
         */
        DEBUG(0, ("A reply callback was called but no reply was received"
                  " and no timeout occurred\n"));

        /* Destroy this connection */
        sbus_disconnect(conn_ctx);
        goto done;
    }

    type = dbus_message_get_type(reply);
    switch (type) {
    case DBUS_MESSAGE_TYPE_METHOD_RETURN:
        /* ok peer replied,
         * set the reply timestamp into the service structure */

        DEBUG(4,("Service %s replied to ping\n", svc->name));

        svc->last_pong = time(NULL);
        break;

    case DBUS_MESSAGE_TYPE_ERROR:

        dbus_error_name = dbus_message_get_error_name(reply);

        /* timeouts are handled in the main service check function */
        if (strcmp(dbus_error_name, DBUS_ERROR_TIMEOUT) == 0)
            break;

        DEBUG(0,("A service PING returned an error [%s], closing connection.\n",
                 dbus_error_name));
        /* Falling through to default intentionally*/
    default:
        /*
         * Timeout or other error occurred or something
         * unexpected happened.
         * It doesn't matter which, because either way we
         * know that this connection isn't trustworthy.
         * We'll destroy it now.
         */
        sbus_disconnect(conn_ctx);
    }

done:
    dbus_pending_call_unref(pending);
    dbus_message_unref(reply);
}



/* service_check_alive
 * This function checks if the service child is still alive
 */
static int service_check_alive(struct mt_svc *svc)
{
    int status;
    pid_t pid;

    DEBUG(4,("Checking service %s(%d) is still alive\n", svc->name, svc->pid));

    pid = waitpid(svc->pid, &status, WNOHANG);
    if (pid == 0) {
        return EOK;
    }

    if (pid != svc->pid) {
        DEBUG(1, ("bad return (%d) from waitpid() waiting for %d\n",
                  pid, svc->pid));
        /* TODO: what do we do now ? */
        return EINVAL;
    }

    if (WIFEXITED(status)) { /* children exited on it's own */
        /* TODO: check configuration to see if it was removed
         * from the list of process to run */
        DEBUG(0,("Process [%s] exited\n", svc->name));
    }

    return ECHILD;
}

static void free_args(char **args)
{
    int i;

    if (args) {
        for (i = 0; args[i]; i++) free(args[i]);
        free(args);
    }
}


/* parse a string into arguments.
 * arguments are separated by a space
 * '\' is an escape character and can be used only to escape
 * itself or the white space.
 */
static char **parse_args(const char *str)
{
    const char *p;
    char **ret, **r;
    char *tmp;
    int num;
    int i, e;

    tmp = malloc(strlen(str) + 1);
    if (!tmp) return NULL;

    ret = NULL;
    num = 0;
    e = 0;
    i = 0;
    p = str;
    while (*p) {
        switch (*p) {
        case '\\':
            if (e) {
                tmp[i] = '\\';
                i++;
                e = 0;
            } else {
                e = 1;
            }
            break;
        case ' ':
            if (e) {
                tmp[i] = ' ';
                i++;
                e = 0;
            } else {
                tmp[i] = '\0';
                i++;
            }
            break;
        default:
            if (e) {
                tmp[i] = '\\';
                i++;
                e = 0;
            }
            tmp[i] = *p;
            i++;
            break;
        }

        p++;

        /* check if this was the last char */
        if (*p == '\0') {
            if (e) {
                tmp[i] = '\\';
                i++;
                e = 0;
            }
            tmp[i] = '\0';
            i++;
        }
        if (tmp[i-1] != '\0' || strlen(tmp) == 0) {
            /* check next char and skip multiple spaces */
            continue;
        }

        r = realloc(ret, (num + 2) * sizeof(char *));
        if (!r) goto fail;
        ret = r;
        ret[num+1] = NULL;
        ret[num] = strdup(tmp);
        if (!ret[num]) goto fail;
        num++;
        i = 0;
    }

    free(tmp);
    return ret;

fail:
    free(tmp);
    free_args(ret);
    return NULL;
}

static void service_startup_handler(struct tevent_context *ev,
                                    struct tevent_timer *te,
                                    struct timeval t, void *ptr);

static int start_service(struct mt_svc *svc)
{
    struct tevent_timer *te;
    struct timeval tv;

    DEBUG(4,("Queueing service %s for startup\n", svc->name));

    /* Add a timed event to start up the service.
     * We have to do this in order to avoid a race
     * condition where the service being started forks
     * and attempts to connect to the SBUS before
     * the monitor is serving it.
     */
    gettimeofday(&tv, NULL);
    te = tevent_add_timer(svc->mt_ctx->ev, svc, tv,
                         service_startup_handler, svc);
    if (te == NULL) {
        DEBUG(0, ("Unable to queue service %s for startup\n", svc->name));
        return ENOMEM;
    }
    return EOK;
}

static void service_startup_handler(struct tevent_context *ev,
                                    struct tevent_timer *te,
                                    struct timeval t, void *ptr)
{
    struct mt_svc *mt_svc;
    char **args;

    mt_svc = talloc_get_type(ptr, struct mt_svc);
    if (mt_svc == NULL) {
        return;
    }

    mt_svc->pid = fork();
    if (mt_svc->pid != 0) {
        if (mt_svc->pid == -1) {
            DEBUG(0, ("Could not fork child to start service [%s]. Continuing.\n", mt_svc->name))
            return;
        }

        /* Parent */
        mt_svc->last_pong = time(NULL);
        DLIST_ADD(mt_svc->mt_ctx->svc_list, mt_svc);
        set_tasks_checker(mt_svc);

        return;
    }

    /* child */

    args = parse_args(mt_svc->command);
    execvp(args[0], args);

    /* If we are here, exec() has failed
     * Print errno and abort quickly */
    DEBUG(0,("Could not exec %s, reason: %s\n", mt_svc->command, strerror(errno)));

    /* We have to call _exit() instead of exit() here
     * because a bug in D-BUS will cause the server to
     * close its socket at exit() */
    _exit(1);
}

int main(int argc, const char *argv[])
{
    int opt;
    poptContext pc;
    int opt_daemon = 0;
    int opt_interactive = 0;
    int flags = 0;
    struct main_context *main_ctx;
    int ret;

    struct poptOption long_options[] = {
        POPT_AUTOHELP
        SSSD_MAIN_OPTS
        {"daemon", 'D', POPT_ARG_NONE, &opt_daemon, 0, \
         "Become a daemon (default)", NULL }, \
        {"interactive",	'i', POPT_ARG_NONE, &opt_interactive, 0, \
         "Run interactive (not a daemon)", NULL}, \
        { NULL }
    };

    pc = poptGetContext(argv[0], argc, argv, long_options, 0);
    while((opt = poptGetNextOpt(pc)) != -1) {
        switch(opt) {
        default:
            fprintf(stderr, "\nInvalid option %s: %s\n\n",
                    poptBadOption(pc, 0), poptStrerror(opt));
            poptPrintUsage(pc, stderr, 0);
            return 1;
        }
    }

    if (opt_daemon && opt_interactive) {
        fprintf(stderr, "Option -i|--interactive is not allowed together with -D|--daemon\n");
        poptPrintUsage(pc, stderr, 0);
        return 1;
    }

    poptFreeContext(pc);

    if (opt_daemon) flags |= FLAGS_DAEMON;
    if (opt_interactive) flags |= FLAGS_INTERACTIVE;

    /* we want a pid file check */
    flags |= FLAGS_PID_FILE;

    /* set up things like debug , signals, daemonization, etc... */
    ret = server_setup("sssd", flags, MONITOR_CONF_ENTRY, &main_ctx);
    if (ret != EOK) return 2;

    ret = monitor_process_init(main_ctx,
                               main_ctx->event_ctx,
                               main_ctx->confdb_ctx);
    if (ret != EOK) return 3;

    /* loop on main */
    server_loop(main_ctx);

    return 0;
}


