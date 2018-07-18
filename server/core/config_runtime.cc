/*
 * Copyright (c) 2016 MariaDB Corporation Ab
 *
 * Use of this software is governed by the Business Source License included
 * in the LICENSE.TXT file and at www.mariadb.com/bsl11.
 *
 * Change Date: 2022-01-01
 *
 * On the date above, in accordance with the Business Source License, use
 * of this software will be governed by version 2 or later of the General
 * Public License.
 */

#include <maxscale/cppdefs.hh>

#include "internal/config_runtime.h"

#include <strings.h>
#include <string>
#include <sstream>
#include <set>
#include <iterator>
#include <algorithm>

#include <maxscale/atomic.h>
#include <maxscale/clock.h>
#include <maxscale/jansson.hh>
#include <maxscale/json_api.h>
#include <maxscale/paths.h>
#include <maxscale/platform.h>
#include <maxscale/spinlock.hh>
#include <maxscale/users.h>
#include <maxscale/router.h>

#include "internal/config.h"
#include "internal/monitor.h"
#include "internal/modules.h"
#include "internal/service.h"
#include "internal/filter.h"

typedef std::set<std::string> StringSet;

static mxs::SpinLock crt_lock;

#define RUNTIME_ERRMSG_BUFSIZE 512
thread_local char runtime_errmsg[RUNTIME_ERRMSG_BUFSIZE];

/** Attributes need to be in the declaration */
static void runtime_error(const char* fmt, ...) mxs_attribute((format (printf, 1, 2)));

static void runtime_error(const char* fmt, ...)
{
    va_list list;
    va_start(list, fmt);
    vsnprintf(runtime_errmsg, sizeof(runtime_errmsg), fmt, list);
    va_end(list);
}

static std::string runtime_get_error()
{
    std::string rval(runtime_errmsg);
    runtime_errmsg[0] = '\0';
    return rval;
}

static const MXS_MODULE_PARAM* get_type_parameters(const char* type)
{
    if (strcmp(type, CN_SERVICE) == 0)
    {
        return config_service_params;
    }
    else if (strcmp(type, CN_LISTENER) == 0)
    {
        return config_listener_params;
    }
    else if (strcmp(type, CN_MONITOR) == 0)
    {
        return config_monitor_params;
    }
    else if (strcmp(type, CN_FILTER) == 0)
    {
        return config_filter_params;
    }
    else if (strcmp(type, CN_SERVER) == 0)
    {
        return config_server_params;
    }

    MXS_NOTICE("Module type with no default parameters used: %s", type);
    ss_info_dassert(!true, "Module type with no default parameters used");
    return NULL;
}

/**
 * @brief Load module default parameters
 *
 * @param name        Name of the module to load
 * @param module_type Type of the module (MODULE_ROUTER, MODULE_PROTOCOL etc.)
 * @param object_type Type of the object (server, service, listener etc.)
 *
 * @return List of default parameters or NULL on error
 */
static MXS_CONFIG_PARAMETER* load_defaults(const char* name, const char* module_type,
                                           const char* object_type)
{
    MXS_CONFIG_PARAMETER* rval = NULL;
    CONFIG_CONTEXT ctx = {(char*)""};

    if (const MXS_MODULE* mod = get_module(name, module_type))
    {
        config_add_defaults(&ctx, get_type_parameters(object_type));
        config_add_defaults(&ctx, mod->parameters);
        rval = ctx.parameters;
    }
    else
    {
        MXS_ERROR("Failed to load module '%s'. See previous error messages for "
                  "more details.", name);
    }

    return rval;
}

bool runtime_link_server(SERVER *server, const char *target)
{
    mxs::SpinLockGuard guard(crt_lock);

    bool rval = false;
    SERVICE *service = service_find(target);
    MXS_MONITOR *monitor = service ? NULL : monitor_find(target);

    if (service)
    {
        if (serviceAddBackend(service, server))
        {
            service_serialize(service);
            rval = true;
        }
        else
        {
            runtime_error("Service '%s' already uses server '%s'",
                          service->name, server->name);
        }
    }
    else if (monitor)
    {
        if (monitor_add_server(monitor, server))
        {
            monitor_serialize(monitor);
            rval = true;
        }
        else
        {
            runtime_error("Server '%s' is already monitored", server->name);
        }
    }

    if (rval)
    {
        const char *type = service ? "service" : "monitor";
        MXS_NOTICE("Added server '%s' to %s '%s'", server->name, type, target);
    }

    return rval;
}

bool runtime_unlink_server(SERVER *server, const char *target)
{
    mxs::SpinLockGuard guard(crt_lock);

    bool rval = false;
    SERVICE *service = service_find(target);
    MXS_MONITOR *monitor = service ? NULL : monitor_find(target);

    if (service || monitor)
    {
        rval = true;

        if (service)
        {
            serviceRemoveBackend(service, server);
            service_serialize(service);
        }
        else if (monitor)
        {
            monitor_remove_server(monitor, server);
            monitor_serialize(monitor);
        }

        const char *type = service ? "service" : "monitor";
        MXS_NOTICE("Removed server '%s' from %s '%s'", server->name, type, target);
    }

    return rval;
}

bool runtime_create_server(const char *name, const char *address, const char *port,
                           const char *protocol, const char *authenticator)
{
    mxs::SpinLockGuard guard(crt_lock);
    bool rval = false;

    if (server_find_by_unique_name(name) == NULL)
    {
        if (protocol == NULL)
        {
            protocol = "mariadbbackend";
        }

        CONFIG_CONTEXT ctx{(char*)""};
        ctx.parameters = load_defaults(protocol, MODULE_PROTOCOL, CN_SERVER);

        if (ctx.parameters)
        {
            if (address)
            {
                config_replace_param(&ctx, "address", address);
            }
            if (port)
            {
                config_replace_param(&ctx, "port", port);
            }
            if (authenticator)
            {
                config_replace_param(&ctx, "authenticator", authenticator);
            }

            /** First check if this service has been created before */
            SERVER *server = server_repurpose_destroyed(name, protocol, authenticator,
                                                        address, port);

            if (server)
            {
                MXS_INFO("Reusing server '%s'", name);
            }
            else
            {
                server = server_alloc(name, ctx.parameters);
            }

            if (server && server_serialize(server))
            {
                rval = true;
                MXS_NOTICE("Created server '%s' at %s:%u", server->name,
                           server->address, server->port);
            }
            else
            {
                runtime_error("Failed to create server '%s', see error log for more details", name);
            }

            config_parameter_free(ctx.parameters);
        }
        else
        {
            runtime_error("Server creation failed when loading protocol module '%s'", protocol);
        }
    }
    else
    {
        runtime_error("Server '%s' already exists", name);
    }

    return rval;
}

bool runtime_destroy_server(SERVER *server)
{
    mxs::SpinLockGuard guard(crt_lock);
    bool rval = false;

    if (service_server_in_use(server) || monitor_server_in_use(server))
    {
        const char* err = "Cannot destroy server '%s' as it is used by at least "
                          "one service or monitor";
        runtime_error(err, server->name);
        MXS_ERROR(err, server->name);
    }
    else
    {
        char filename[PATH_MAX];
        snprintf(filename, sizeof(filename), "%s/%s.cnf", get_config_persistdir(),
                 server->name);

        if (unlink(filename) == -1)
        {
            if (errno != ENOENT)
            {
                MXS_ERROR("Failed to remove persisted server configuration '%s': %d, %s",
                          filename, errno, mxs_strerror(errno));
            }
            else
            {
                rval = true;
                MXS_WARNING("Server '%s' was not created at runtime. Remove the "
                            "server manually from the correct configuration file.",
                            server->name);
            }
        }
        else
        {
            rval = true;
        }

        if (rval)
        {
            MXS_NOTICE("Destroyed server '%s' at %s:%u", server->name,
                       server->address, server->port);
            server->is_active = false;
        }
    }

    return rval;
}

static SSL_LISTENER* create_ssl(const char *name, const char *key, const char *cert,
                                const char *ca, const char *version, const char *depth,
                                const char *verify)
{
    SSL_LISTENER *rval = NULL;
    CONFIG_CONTEXT *obj = config_context_create(name);

    if (obj)
    {
        if (config_add_param(obj, CN_SSL, CN_REQUIRED) &&
            (!key || config_add_param(obj, CN_SSL_KEY, key)) &&
            (!cert || config_add_param(obj, CN_SSL_CERT, cert)) &&
            config_add_param(obj, CN_SSL_CA_CERT, ca) &&
            (!version || config_add_param(obj, CN_SSL_VERSION, version)) &&
            (!depth || config_add_param(obj, CN_SSL_CERT_VERIFY_DEPTH, depth)) &&
            (!verify || config_add_param(obj, CN_SSL_VERIFY_PEER_CERTIFICATE, verify)))
        {
            config_create_ssl(name, obj->parameters, true, &rval);
        }

        config_context_free(obj);
    }

    return rval;
}

bool runtime_enable_server_ssl(SERVER *server, const char *key, const char *cert,
                               const char *ca, const char *version, const char *depth,
                               const char *verify)
{
    bool rval = false;

    if (key && cert && ca)
    {
        mxs::SpinLockGuard guard(crt_lock);
        SSL_LISTENER *ssl = create_ssl(server->name, key, cert, ca, version, depth, verify);

        if (ssl)
        {
            /** TODO: Properly discard old SSL configurations.This could cause the
             * loss of a pointer if two update operations are done at the same time.*/
            ssl->next = server->server_ssl;

            /** Sync to prevent reads on partially initialized server_ssl */
            atomic_synchronize();
            server->server_ssl = ssl;

            if (server_serialize(server))
            {
                MXS_NOTICE("Enabled SSL for server '%s'", server->name);
                rval = true;
            }
        }
    }

    return rval;
}

/**
 * @brief Convert a string value to a positive integer
 *
 * If the value is not a positive integer, an error is printed to @c dcb.
 *
 * @param value String value
 * @return 0 on error, otherwise a positive integer
 */
static long get_positive_int(const char *value)
{
    char *endptr;
    long ival = strtol(value, &endptr, 10);

    if (*endptr == '\0' && ival > 0)
    {
        return ival;
    }

    return 0;
}

static inline bool is_valid_integer(const char* value)
{
    char* endptr;
    return strtol(value, &endptr, 10) >= 0 && *value && *endptr == '\0';
}

bool runtime_alter_server(SERVER *server, const char *key, const char *value)
{
    mxs::SpinLockGuard guard(crt_lock);
    bool valid = false;

    if (strcmp(key, CN_ADDRESS) == 0)
    {
        valid = true;
        server_update_address(server, value);
    }
    else if (strcmp(key, CN_PORT) == 0)
    {
        long ival = get_positive_int(value);

        if (ival)
        {
            valid = true;
            server_update_port(server, ival);
        }
    }
    else if (strcmp(key, CN_MONITORUSER) == 0)
    {
        valid = true;
        server_update_credentials(server, value, server->monpw);
    }
    else if (strcmp(key, CN_MONITORPW) == 0)
    {
        valid = true;
        server_update_credentials(server, server->monuser, value);
    }
    else if (strcmp(key, CN_PERSISTPOOLMAX) == 0)
    {
        if (is_valid_integer(value))
        {
            valid = true;
            server->persistpoolmax = atoi(value);
        }
    }
    else if (strcmp(key, CN_PERSISTMAXTIME) == 0)
    {
        if (is_valid_integer(value))
        {
            valid = true;
            server->persistmaxtime = atoi(value);
        }
    }
    else
    {
        if (!value[0] && !server_remove_parameter(server, key))
        {
            // Not a valid parameter
        }
        else if (value[0])
        {
            valid = true;
            server_update_parameter(server, key, value);

            /**
             * It's likely that this parameter is used as a weighting parameter.
             * We need to update the weights of services that use this.
             */
            service_update_weights();
        }
    }

    if (valid)
    {
        if (server_serialize(server))
        {
            MXS_NOTICE("Updated server '%s': %s=%s", server->name, key, value);
        }
    }
    else
    {
        runtime_error("Invalid server parameter: %s=%s", key, value);
    }

    return valid;
}

bool runtime_alter_monitor(MXS_MONITOR *monitor, const char *key, const char *value)
{
    mxs::SpinLockGuard guard(crt_lock);
    bool valid = false;
    const MXS_MODULE *mod = get_module(monitor->module_name, MODULE_MONITOR);

    if (strcmp(key, CN_USER) == 0)
    {
        valid = true;
        monitor_add_user(monitor, value, monitor->password);
    }
    else if (strcmp(key, CN_PASSWORD) == 0)
    {
        valid = true;
        monitor_add_user(monitor, monitor->user, value);
    }
    else if (strcmp(key, CN_MONITOR_INTERVAL) == 0)
    {
        long ival = get_positive_int(value);
        if (ival)
        {
            valid = true;
            monitor_set_interval(monitor, ival);
        }
    }
    else if (strcmp(key, CN_BACKEND_CONNECT_TIMEOUT) == 0)
    {
        long ival = get_positive_int(value);
        if (ival)
        {
            valid = true;
            monitor_set_network_timeout(monitor, MONITOR_CONNECT_TIMEOUT, ival, CN_BACKEND_CONNECT_TIMEOUT);
        }
    }
    else if (strcmp(key, CN_BACKEND_WRITE_TIMEOUT) == 0)
    {
        long ival = get_positive_int(value);
        if (ival)
        {
            valid = true;
            monitor_set_network_timeout(monitor, MONITOR_WRITE_TIMEOUT, ival, CN_BACKEND_WRITE_TIMEOUT);
        }
    }
    else if (strcmp(key, CN_BACKEND_READ_TIMEOUT) == 0)
    {
        long ival = get_positive_int(value);
        if (ival)
        {
            valid = true;
            monitor_set_network_timeout(monitor, MONITOR_READ_TIMEOUT, ival, CN_BACKEND_READ_TIMEOUT);
        }
    }
    else if (strcmp(key, CN_BACKEND_CONNECT_ATTEMPTS) == 0)
    {
        long ival = get_positive_int(value);
        if (ival)
        {
            valid = true;
            monitor_set_network_timeout(monitor, MONITOR_CONNECT_ATTEMPTS, ival, CN_BACKEND_CONNECT_ATTEMPTS);
        }
    }
    else if (strcmp(key, CN_JOURNAL_MAX_AGE) == 0)
    {
        long ival = get_positive_int(value);
        if (ival)
        {
            valid = true;
            monitor_set_journal_max_age(monitor, ival);
        }
    }
    else if (strcmp(key, CN_SCRIPT_TIMEOUT) == 0)
    {
        long ival = get_positive_int(value);
        if (ival)
        {
            valid = true;
            monitor_set_script_timeout(monitor, ival);
        }
    }
    else  if (config_param_is_valid(mod->parameters, key, value, NULL))
    {
        /** We're modifying module specific parameters and we need to stop the monitor */
        monitor_stop(monitor);

        if (monitor_remove_parameter(monitor, key) || value[0])
        {
            /** Either we're removing an existing parameter or adding a new one */
            valid = true;

            if (value[0])
            {
                MXS_CONFIG_PARAMETER p = {};
                p.name = const_cast<char*>(key);
                p.value = const_cast<char*>(value);
                monitor_add_parameters(monitor, &p);
            }
        }

        monitor_start(monitor, monitor->parameters);
    }

    if (valid)
    {
        monitor_serialize(monitor);
        MXS_NOTICE("Updated monitor '%s': %s=%s", monitor->name, key, value);
    }
    else
    {
        runtime_error("Invalid monitor parameter: %s", key);
    }

    return valid;
}

bool runtime_alter_service(SERVICE *service, const char* zKey, const char* zValue)
{
    std::string key(zKey);
    std::string value(zValue);
    bool valid = false;

    const MXS_MODULE* module = get_module(service->routerModule, MODULE_ROUTER);
    ss_dassert(module);

    mxs::SpinLockGuard guard(crt_lock);

    if (key == CN_USER)
    {
        valid = true;
        serviceSetUser(service, value.c_str(), service->credentials.authdata);
    }
    else if (key == CN_PASSWORD)
    {
        valid = true;
        serviceSetUser(service, service->credentials.name, value.c_str());
    }
    else if (key == CN_ENABLE_ROOT_USER)
    {
        valid = true;
        serviceEnableRootUser(service, config_truth_value(value.c_str()));
    }
    else if (key == CN_MAX_RETRY_INTERVAL)
    {
        long i = get_positive_int(zValue);
        if (i)
        {
            valid = true;
            service_set_retry_interval(service, i);
        }
    }
    else if (key == CN_MAX_CONNECTIONS)
    {
        long i = get_positive_int(zValue);
        if (i)
        {
            valid = true;
            // TODO: Once connection queues are implemented, use correct values
            const int queued_connections = 0; // At most this many pending connections.
            const int timeout = 0;            // Wait at most this much for a connection.
            serviceSetConnectionLimits(service, i, queued_connections, timeout);
        }
    }
    else if (key == CN_CONNECTION_TIMEOUT)
    {
        long i = get_positive_int(zValue);
        if (i)
        {
            valid = true;
            serviceSetTimeout(service, i);
        }
    }
    else if (key == CN_AUTH_ALL_SERVERS)
    {
        valid = true;
        serviceAuthAllServers(service, config_truth_value(value.c_str()));
    }
    else if (key == CN_STRIP_DB_ESC)
    {
        valid = true;
        serviceStripDbEsc(service, config_truth_value(value.c_str()));
    }
    else if (key == CN_LOCALHOST_MATCH_WILDCARD_HOST)
    {
        valid = true;
        serviceEnableLocalhostMatchWildcardHost(service, config_truth_value(value.c_str()));
    }
    else if (key == CN_VERSION_STRING)
    {
        valid = true;
        serviceSetVersionString(service, value.c_str());
    }
    else if (key == CN_WEIGHTBY)
    {
        valid = true;
        serviceWeightBy(service, value.c_str());
    }
    else if (key == CN_LOG_AUTH_WARNINGS)
    {
        valid = true;
        // TODO: Move this inside the service source
        service->log_auth_warnings = config_truth_value(value.c_str());
    }
    else if (key == CN_RETRY_ON_FAILURE)
    {
        valid = true;
        serviceSetRetryOnFailure(service, value.c_str());
    }
    else if (config_param_is_valid(module->parameters, key.c_str(), value.c_str(), NULL) ||
             key == CN_ROUTER_OPTIONS)
    {
        if (service->router->configureInstance && service->capabilities & RCAP_TYPE_RUNTIME_CONFIG)
        {
            // Stash the old value in case the reconfiguration fails.
            std::string old_value = config_get_string(service->svc_config_param, key.c_str());
            service_replace_parameter(service, key.c_str(), value.c_str());

            if (service->router->configureInstance(service->router_instance, service->svc_config_param))
            {
                valid = true;
            }
            else
            {
                // Reconfiguration failed, restore the old value of the parameter
                if (old_value.empty())
                {
                    service_remove_parameter(service, key.c_str());
                }
                else
                {
                    service_replace_parameter(service, key.c_str(), old_value.c_str());
                }
                runtime_error("Reconfiguration of service '%s' failed. See log "
                              "file for more details.", service->name);
            }
        }
        else
        {
            runtime_error("Router '%s' does not support reconfiguration.",
                          service->routerModule);
        }
    }
    else
    {
        runtime_error("Invalid service parameter: %s=%s", key.c_str(), zValue);
        MXS_ERROR("Unknown parameter for service '%s': %s=%s",
                  service->name, key.c_str(), value.c_str());
    }

    if (valid)
    {
        service_serialize(service);
        MXS_NOTICE("Updated service '%s': %s=%s", service->name, key.c_str(), value.c_str());
    }

    return valid;
}

bool runtime_alter_maxscale(const char* name, const char* value)
{
    MXS_CONFIG& cnf = *config_get_global_options();
    std::string key = name;
    bool rval = false;

    mxs::SpinLockGuard guard(crt_lock);

    if (key == CN_AUTH_CONNECT_TIMEOUT)
    {
        int intval = get_positive_int(value);
        if (intval)
        {
            MXS_NOTICE("Updated '%s' from %d to %d", CN_AUTH_CONNECT_TIMEOUT,
                       cnf.auth_conn_timeout, intval);
            cnf.auth_conn_timeout = intval;
            rval = true;
        }
        else
        {
            runtime_error("Invalid timeout value for '%s': %s", CN_AUTH_CONNECT_TIMEOUT, value);
        }
    }
    else if (key == CN_AUTH_READ_TIMEOUT)
    {
        int intval = get_positive_int(value);
        if (intval)
        {
            MXS_NOTICE("Updated '%s' from %d to %d", CN_AUTH_READ_TIMEOUT,
                       cnf.auth_read_timeout, intval);
            cnf.auth_read_timeout = intval;
            rval = true;
        }
        else
        {
            runtime_error("Invalid timeout value for '%s': %s", CN_AUTH_READ_TIMEOUT, value);
        }
    }
    else if (key == CN_AUTH_WRITE_TIMEOUT)
    {
        int intval = get_positive_int(value);
        if (intval)
        {
            MXS_NOTICE("Updated '%s' from %d to %d", CN_AUTH_WRITE_TIMEOUT,
                       cnf.auth_write_timeout, intval);
            cnf.auth_write_timeout = intval;
            rval = true;
        }
        else
        {
            runtime_error("Invalid timeout value for '%s': %s", CN_AUTH_WRITE_TIMEOUT, value);
        }
    }
    else if (key == CN_ADMIN_AUTH)
    {
        int boolval = config_truth_value(value);

        if (boolval != -1)
        {
            MXS_NOTICE("Updated '%s' from '%s' to '%s'", CN_ADMIN_AUTH,
                       cnf.admin_auth ? "true" : "false",
                       boolval ? "true" : "false");
            cnf.admin_auth = boolval;
            rval = true;
        }
        else
        {
            runtime_error("Invalid boolean value for '%s': %s", CN_ADMIN_AUTH, value);
        }
    }
    else if (key == CN_ADMIN_LOG_AUTH_FAILURES)
    {
        int boolval = config_truth_value(value);

        if (boolval != -1)
        {
            MXS_NOTICE("Updated '%s' from '%s' to '%s'", CN_ADMIN_LOG_AUTH_FAILURES,
                       cnf.admin_log_auth_failures ? "true" : "false",
                       boolval ? "true" : "false");
            cnf.admin_log_auth_failures = boolval;
            rval = true;
        }
        else
        {
            runtime_error("Invalid boolean value for '%s': %s", CN_ADMIN_LOG_AUTH_FAILURES, value);
        }
    }
    else if (key == CN_PASSIVE)
    {
        int boolval = config_truth_value(value);

        if (boolval != -1)
        {
            MXS_NOTICE("Updated '%s' from '%s' to '%s'", CN_PASSIVE,
                       cnf.passive ? "true" : "false",
                       boolval ? "true" : "false");

            if (cnf.passive && !boolval)
            {
                // This MaxScale is being promoted to the active instance
                cnf.promoted_at = mxs_clock();
            }

            cnf.passive = boolval;
            rval = true;
        }
        else
        {
            runtime_error("Invalid boolean value for '%s': %s", CN_PASSIVE, value);
        }
    }
    else
    {
        runtime_error("Unknown global parameter: %s=%s", name, value);
    }

    if (rval)
    {
        config_global_serialize();
    }

    return rval;
}

bool runtime_create_listener(SERVICE *service, const char *name, const char *addr,
                             const char *port, const char *proto, const char *auth,
                             const char *auth_opt, const char *ssl_key,
                             const char *ssl_cert, const char *ssl_ca,
                             const char *ssl_version, const char *ssl_depth,
                             const char *verify_ssl)
{

    if (addr == NULL || strcasecmp(addr, CN_DEFAULT) == 0)
    {
        addr = "::";
    }
    if (port == NULL || strcasecmp(port, CN_DEFAULT) == 0)
    {
        port = "3306";
    }
    if (proto == NULL || strcasecmp(proto, CN_DEFAULT) == 0)
    {
        proto = "mariadbclient";
    }

    if (auth && strcasecmp(auth, CN_DEFAULT) == 0)
    {
        /** Set auth to NULL so the protocol default authenticator is used */
        auth = NULL;
    }

    if (auth_opt && strcasecmp(auth_opt, CN_DEFAULT) == 0)
    {
        /** Don't pass options to the authenticator */
        auth_opt = NULL;
    }

    unsigned short u_port = atoi(port);
    bool rval = false;

    mxs::SpinLockGuard guard(crt_lock);

    if (!serviceHasListener(service, name, proto, addr, u_port))
    {
        SSL_LISTENER *ssl = NULL;

        if (ssl_key && ssl_cert && ssl_ca &&
            (ssl = create_ssl(name, ssl_key, ssl_cert, ssl_ca, ssl_version, ssl_depth, verify_ssl)) == NULL)
        {
            MXS_ERROR("SSL initialization for listener '%s' failed.", name);
            runtime_error("SSL initialization for listener '%s' failed.", name);
        }
        else
        {
            const char *print_addr = addr ? addr : "::";
            SERV_LISTENER *listener = serviceCreateListener(service, name, proto, addr,
                                                            u_port, auth, auth_opt, ssl);

            if (listener && listener_serialize(listener))
            {
                MXS_NOTICE("Created %slistener '%s' at %s:%s for service '%s'",
                           ssl ? "TLS encrypted " : "",
                           name, print_addr, port, service->name);
                if (serviceLaunchListener(service, listener))
                {
                    rval = true;
                }
                else
                {
                    MXS_ERROR("Listener '%s' was created but failed to start it.", name);
                    runtime_error("Listener '%s' was created but failed to start it.", name);
                }
            }
            else
            {
                MXS_ERROR("Failed to create listener '%s' at %s:%s.", name, print_addr, port);
                runtime_error("Failed to create listener '%s' at %s:%s.", name, print_addr, port);
            }
        }
    }
    else
    {
        runtime_error("Listener '%s' already exists", name);
    }

    return rval;
}

bool runtime_destroy_listener(SERVICE *service, const char *name)
{
    bool rval = false;
    char filename[PATH_MAX];
    snprintf(filename, sizeof(filename), "%s/%s.cnf", get_config_persistdir(), name);

    mxs::SpinLockGuard guard(crt_lock);

    if (unlink(filename) == -1)
    {
        if (errno != ENOENT)
        {
            MXS_ERROR("Failed to remove persisted listener configuration '%s': %d, %s",
                      filename, errno, mxs_strerror(errno));
        }
        else
        {
            runtime_error("Listener '%s' was not created at runtime. Remove the listener "
                          "manually from the correct configuration file.", name);
        }
    }
    else
    {
        rval = true;
    }

    if (rval)
    {
        rval = serviceStopListener(service, name);

        if (rval)
        {
            MXS_NOTICE("Destroyed listener '%s' for service '%s'. The listener "
                       "will be removed after the next restart of MaxScale.",
                       name, service->name);
        }
        else
        {
            MXS_ERROR("Failed to destroy listener '%s' for service '%s'", name, service->name);
            runtime_error("Failed to destroy listener '%s' for service '%s'", name, service->name);
        }
    }

    return rval;
}

bool runtime_create_monitor(const char *name, const char *module)
{
    mxs::SpinLockGuard guard(crt_lock);
    bool rval = false;

    if (monitor_find(name) == NULL)
    {

        MXS_MONITOR *monitor = monitor_repurpose_destroyed(name, module);

        if (monitor)
        {
            MXS_DEBUG("Repurposed monitor '%s'", name);
        }
        else if (MXS_CONFIG_PARAMETER* params = load_defaults(module, MODULE_MONITOR, CN_MONITOR))
        {
            if ((monitor = monitor_create(name, module, params)) == NULL)
            {
                runtime_error("Could not create monitor '%s' with module '%s'", name, module);
            }

            config_parameter_free(params);
        }

        if (monitor)
        {
            if (monitor_serialize(monitor))
            {
                MXS_NOTICE("Created monitor '%s'", name);
                rval = true;
            }
            else
            {
                runtime_error("Failed to serialize monitor '%s'", name);
            }
        }
    }
    else
    {
        runtime_error("Can't create monitor '%s', it already exists", name);
    }

    return rval;
}

bool runtime_create_filter(const char *name, const char *module, MXS_CONFIG_PARAMETER* params)
{
    mxs::SpinLockGuard guard(crt_lock);
    bool rval = false;

    if (filter_def_find(name) == NULL)
    {
        MXS_FILTER_DEF* filter = NULL;
        CONFIG_CONTEXT ctx{(char*)""};
        ctx.parameters = load_defaults(module, MODULE_FILTER, CN_FILTER);

        if (ctx.parameters)
        {
            for (MXS_CONFIG_PARAMETER* p = params; p; p = p->next)
            {
                config_replace_param(&ctx, p->name, p->value);
            }

            if ((filter = filter_alloc(name, module, ctx.parameters)) == NULL)
            {
                runtime_error("Could not create filter '%s' with module '%s'", name, module);
            }

            config_parameter_free(ctx.parameters);
        }

        if (filter)
        {
            if (filter_serialize(filter))
            {
                MXS_NOTICE("Created filter '%s'", name);
                rval = true;
            }
            else
            {
                runtime_error("Failed to serialize filter '%s'", name);
            }
        }
    }
    else
    {
        runtime_error("Can't create filter '%s', it already exists", name);
    }

    return rval;
}

bool runtime_destroy_monitor(MXS_MONITOR *monitor)
{
    bool rval = false;
    char filename[PATH_MAX];
    snprintf(filename, sizeof(filename), "%s/%s.cnf", get_config_persistdir(), monitor->name);

    mxs::SpinLockGuard guard(crt_lock);

    if (unlink(filename) == -1 && errno != ENOENT)
    {
        MXS_ERROR("Failed to remove persisted monitor configuration '%s': %d, %s",
                  filename, errno, mxs_strerror(errno));
    }
    else
    {
        rval = true;
    }

    if (rval)
    {
        monitor_stop(monitor);

        while (monitor->monitored_servers)
        {
            monitor_remove_server(monitor, monitor->monitored_servers->server);
        }
        monitor_deactivate(monitor);
        MXS_NOTICE("Destroyed monitor '%s'", monitor->name);
    }

    return rval;
}

static MXS_CONFIG_PARAMETER* extract_parameters_from_json(json_t* json)
{
    CONFIG_CONTEXT ctx{(char*)""};

    if (json_t* parameters = mxs_json_pointer(json, MXS_JSON_PTR_PARAMETERS))
    {
        const char* key;
        json_t* value;

        json_object_foreach(parameters, key, value)
        {
            config_add_param(&ctx, key, json_string_value(value));
        }
    }

    return ctx.parameters;
}

static bool extract_relations(json_t* json, StringSet& relations,
                              const char* relation_type,
                              bool (*relation_check)(const std::string&, const std::string&))
{
    bool rval = true;
    json_t* arr = mxs_json_pointer(json, relation_type);

    if (arr && json_is_array(arr))
    {
        size_t size = json_array_size(arr);

        for (size_t j = 0; j < size; j++)
        {
            json_t* obj = json_array_get(arr, j);
            json_t* id = json_object_get(obj, CN_ID);
            json_t* type = mxs_json_pointer(obj, CN_TYPE);

            if (id && json_is_string(id) &&
                type && json_is_string(type))
            {
                std::string id_value = json_string_value(id);
                std::string type_value = json_string_value(type);

                if (relation_check(type_value, id_value))
                {
                    relations.insert(id_value);
                }
                else
                {
                    rval = false;
                }
            }
            else
            {
                rval = false;
            }
        }
    }

    return rval;
}

static inline bool is_null_relation(json_t* json, const char* relation)
{
    std::string str(relation);
    size_t pos = str.rfind("/data");

    ss_dassert(pos != std::string::npos);
    str = str.substr(0, pos);

    json_t* data = mxs_json_pointer(json, relation);
    json_t* base = mxs_json_pointer(json, str.c_str());

    return (data && json_is_null(data)) || (base && json_is_null(base));
}

static inline const char* get_string_or_null(json_t* json, const char* path)
{
    const char* rval = NULL;
    json_t* value = mxs_json_pointer(json, path);

    if (value && json_is_string(value))
    {
        rval = json_string_value(value);
    }

    return rval;
}
static inline bool is_string_or_null(json_t* json, const char* path)
{
    bool rval = true;
    json_t* value = mxs_json_pointer(json, path);

    if (value && !json_is_string(value))
    {
        runtime_error("Parameter '%s' is not a string", path);
        rval = false;
    }

    return rval;
}

static inline bool is_bool_or_null(json_t* json, const char* path)
{
    bool rval = true;
    json_t* value = mxs_json_pointer(json, path);

    if (value && !json_is_boolean(value))
    {
        runtime_error("Parameter '%s' is not a boolean", path);
        rval = false;
    }

    return rval;
}

static inline bool is_count_or_null(json_t* json, const char* path)
{
    bool rval = true;
    json_t* value = mxs_json_pointer(json, path);

    if (value)
    {
        if (!json_is_integer(value))
        {
            runtime_error("Parameter '%s' is not an integer", path);
            rval = false;
        }
        else if (json_integer_value(value) <= 0)
        {
            runtime_error("Parameter '%s' is not a positive integer", path);
            rval = false;
        }
    }

    return rval;
}

/** Check that the body at least defies a data member */
static bool is_valid_resource_body(json_t* json)
{
    bool rval = true;

    if (mxs_json_pointer(json, MXS_JSON_PTR_DATA) == NULL)
    {
        runtime_error("No '%s' field defined", MXS_JSON_PTR_DATA);
        rval = false;
    }

    return rval;
}

static bool server_contains_required_fields(json_t* json)
{
    json_t* id = mxs_json_pointer(json, MXS_JSON_PTR_ID);
    json_t* port = mxs_json_pointer(json, MXS_JSON_PTR_PARAM_PORT);
    json_t* address = mxs_json_pointer(json, MXS_JSON_PTR_PARAM_ADDRESS);
    bool rval = false;

    if (!id)
    {
        runtime_error("Request body does not define the '%s' field", MXS_JSON_PTR_ID);
    }
    else if (!json_is_string(id))
    {
        runtime_error("The '%s' field is not a string", MXS_JSON_PTR_ID);
    }
    else if (!address)
    {
        runtime_error("Request body does not define the '%s' field", MXS_JSON_PTR_PARAM_ADDRESS);
    }
    else if (!json_is_string(address))
    {
        runtime_error("The '%s' field is not a string", MXS_JSON_PTR_PARAM_ADDRESS);
    }
    else if (!port)
    {
        runtime_error("Request body does not define the '%s' field", MXS_JSON_PTR_PARAM_PORT);
    }
    else if (!json_is_integer(port))
    {
        runtime_error("The '%s' field is not an integer", MXS_JSON_PTR_PARAM_PORT);
    }
    else
    {
        rval = true;
    }

    return rval;
}

static bool server_relation_is_valid(const std::string& type, const std::string& value)
{
    return (type == CN_SERVICES && service_find(value.c_str())) ||
           (type == CN_MONITORS && monitor_find(value.c_str()));
}

static bool filter_relation_is_valid(const std::string& type, const std::string& value)
{
    return type == CN_SERVICES && service_find(value.c_str());
}

static bool unlink_server_from_objects(SERVER* server, StringSet& relations)
{
    bool rval = true;

    for (StringSet::iterator it = relations.begin(); it != relations.end(); it++)
    {
        if (!runtime_unlink_server(server, it->c_str()))
        {
            rval = false;
        }
    }

    return rval;
}

static bool link_server_to_objects(SERVER* server, StringSet& relations)
{
    bool rval = true;

    for (StringSet::iterator it = relations.begin(); it != relations.end(); it++)
    {
        if (!runtime_link_server(server, it->c_str()))
        {
            unlink_server_from_objects(server, relations);
            rval = false;
            break;
        }
    }

    return rval;
}

static std::string json_int_to_string(json_t* json)
{
    char str[25]; // Enough to store any 64-bit integer value
    int64_t i = json_integer_value(json);
    snprintf(str, sizeof(str), "%ld", i);
    return std::string(str);
}

static inline bool have_ssl_json(json_t* params)
{
    return mxs_json_pointer(params, CN_SSL_KEY) ||
           mxs_json_pointer(params, CN_SSL_CERT) ||
           mxs_json_pointer(params, CN_SSL_CA_CERT) ||
           mxs_json_pointer(params, CN_SSL_VERSION) ||
           mxs_json_pointer(params, CN_SSL_CERT_VERIFY_DEPTH);
}

static bool validate_ssl_json(json_t* params)
{
    bool rval = true;

    if (is_string_or_null(params, CN_SSL_KEY) &&
        is_string_or_null(params, CN_SSL_CERT) &&
        is_string_or_null(params, CN_SSL_CA_CERT) &&
        is_string_or_null(params, CN_SSL_VERSION) &&
        is_count_or_null(params, CN_SSL_CERT_VERIFY_DEPTH))
    {
        if ((mxs_json_pointer(params, CN_SSL_KEY) ||
             mxs_json_pointer(params, CN_SSL_CERT) ||
             mxs_json_pointer(params, CN_SSL_CA_CERT)) &&
            (!mxs_json_pointer(params, CN_SSL_KEY) ||
             !mxs_json_pointer(params, CN_SSL_CERT) ||
             !mxs_json_pointer(params, CN_SSL_CA_CERT)))
        {
            runtime_error("SSL configuration requires '%s', '%s' and '%s' parameters",
                          CN_SSL_KEY, CN_SSL_CERT, CN_SSL_CA_CERT);
            rval = false;
        }

        json_t* ssl_version = mxs_json_pointer(params, CN_SSL_VERSION);
        const char* ssl_version_str = ssl_version ? json_string_value(ssl_version) : NULL;

        if (ssl_version_str && string_to_ssl_method_type(ssl_version_str) == SERVICE_SSL_UNKNOWN)
        {
            runtime_error("Invalid value for '%s': %s", CN_SSL_VERSION, ssl_version_str);
            rval = false;
        }
    }

    return rval;
}

static bool process_ssl_parameters(SERVER* server, json_t* params)
{
    ss_dassert(server->server_ssl == NULL);
    bool rval = true;

    if (have_ssl_json(params))
    {
        if (validate_ssl_json(params))
        {
            char buf[20]; // Enough to hold the string form of the ssl_cert_verify_depth
            char buf_verify[20]; // Enough to hold the string form of the ssl_verify_peer_certificate
            const char* key = json_string_value(mxs_json_pointer(params, CN_SSL_KEY));
            const char* cert = json_string_value(mxs_json_pointer(params, CN_SSL_CERT));
            const char* ca = json_string_value(mxs_json_pointer(params, CN_SSL_CA_CERT));
            const char* version = json_string_value(mxs_json_pointer(params, CN_SSL_VERSION));
            const char* depth = NULL;
            json_t* depth_json = mxs_json_pointer(params, CN_SSL_CERT_VERIFY_DEPTH);

            if (depth_json)
            {
                snprintf(buf, sizeof(buf), "%lld", json_integer_value(depth_json));
                depth = buf;
            }

            const char* verify = NULL;
            json_t* verify_json = mxs_json_pointer(params, CN_SSL_VERIFY_PEER_CERTIFICATE);

            if (verify_json)
            {
                snprintf(buf_verify, sizeof(buf), "%s", json_boolean_value(verify_json) ? "true" : "false");
                verify = buf_verify;
            }

            if (!runtime_enable_server_ssl(server, key, cert, ca, version, depth, verify))
            {
                runtime_error("Failed to initialize SSL for server '%s'. See "
                              "error log for more details.", server->name);
                rval = false;
            }
        }
        else
        {
            rval = false;
        }
    }

    return rval;
}

SERVER* runtime_create_server_from_json(json_t* json)
{
    SERVER* rval = NULL;

    if (is_valid_resource_body(json) &&
        server_contains_required_fields(json))
    {
        const char* name = json_string_value(mxs_json_pointer(json, MXS_JSON_PTR_ID));
        const char* address = json_string_value(mxs_json_pointer(json, MXS_JSON_PTR_PARAM_ADDRESS));

        /** The port needs to be in string format */
        std::string port = json_int_to_string(mxs_json_pointer(json, MXS_JSON_PTR_PARAM_PORT));

        /** Optional parameters */
        const char* protocol = get_string_or_null(json, MXS_JSON_PTR_PARAM_PROTOCOL);
        const char* authenticator = get_string_or_null(json, MXS_JSON_PTR_PARAM_AUTHENTICATOR);

        StringSet relations;

        if (extract_relations(json, relations, MXS_JSON_PTR_RELATIONSHIPS_SERVICES, server_relation_is_valid) &&
            extract_relations(json, relations, MXS_JSON_PTR_RELATIONSHIPS_MONITORS, server_relation_is_valid))
        {
            if (runtime_create_server(name, address, port.c_str(), protocol, authenticator))
            {
                rval = server_find_by_unique_name(name);
                ss_dassert(rval);
                json_t* param = mxs_json_pointer(json, MXS_JSON_PTR_PARAMETERS);

                if (!process_ssl_parameters(rval, param) ||
                    !link_server_to_objects(rval, relations))
                {
                    runtime_destroy_server(rval);
                    rval = NULL;
                }
            }
        }
        else
        {
            runtime_error("Invalid relationships in request JSON");
        }
    }

    return rval;
}

bool server_to_object_relations(SERVER* server, json_t* old_json, json_t* new_json)
{
    if (mxs_json_pointer(new_json, MXS_JSON_PTR_RELATIONSHIPS) == NULL)
    {
        /** No change to relationships */
        return true;
    }

    const char* server_relation_types[] =
    {
        MXS_JSON_PTR_RELATIONSHIPS_SERVICES,
        MXS_JSON_PTR_RELATIONSHIPS_MONITORS,
        NULL
    };

    bool rval = true;
    StringSet old_relations;
    StringSet new_relations;

    for (int i = 0; server_relation_types[i]; i++)
    {
        // Extract only changed or deleted relationships
        if (is_null_relation(new_json, server_relation_types[i]) ||
            mxs_json_pointer(new_json, server_relation_types[i]))
        {
            if (!extract_relations(new_json, new_relations, server_relation_types[i], server_relation_is_valid) ||
                !extract_relations(old_json, old_relations, server_relation_types[i], server_relation_is_valid))
            {
                rval = false;
                break;
            }
        }
    }

    if (rval)
    {
        StringSet removed_relations;
        StringSet added_relations;

        std::set_difference(old_relations.begin(), old_relations.end(),
                            new_relations.begin(), new_relations.end(),
                            std::inserter(removed_relations, removed_relations.begin()));

        std::set_difference(new_relations.begin(), new_relations.end(),
                            old_relations.begin(), old_relations.end(),
                            std::inserter(added_relations, added_relations.begin()));

        if (!unlink_server_from_objects(server, removed_relations) ||
            !link_server_to_objects(server, added_relations))
        {
            rval = false;
        }
    }

    return rval;
}

bool runtime_alter_server_from_json(SERVER* server, json_t* new_json)
{
    bool rval = false;
    mxs::Closer<json_t*> old_json(server_to_json(server, ""));
    ss_dassert(old_json.get());

    if (is_valid_resource_body(new_json) &&
        server_to_object_relations(server, old_json.get(), new_json))
    {
        rval = true;
        json_t* parameters = mxs_json_pointer(new_json, MXS_JSON_PTR_PARAMETERS);
        json_t* old_parameters = mxs_json_pointer(old_json.get(), MXS_JSON_PTR_PARAMETERS);

        ss_dassert(old_parameters);

        if (parameters)
        {
            const char* key;
            json_t* value;

            json_object_foreach(parameters, key, value)
            {
                json_t* new_val = json_object_get(parameters, key);
                json_t* old_val = json_object_get(old_parameters, key);

                if (old_val && new_val && mxs::json_to_string(new_val) == mxs::json_to_string(old_val))
                {
                    /** No change in values */
                }
                else if (!runtime_alter_server(server, key, mxs::json_to_string(value).c_str()))
                {
                    rval = false;
                }
            }
        }
    }

    return rval;
}

static bool is_valid_relationship_body(json_t* json)
{
    bool rval = true;

    json_t* obj = mxs_json_pointer(json, MXS_JSON_PTR_DATA);

    if (!obj)
    {
        runtime_error("Field '%s' is not defined", MXS_JSON_PTR_DATA);
        rval = false;
    }
    else if (!json_is_array(obj))
    {
        runtime_error("Field '%s' is not an array", MXS_JSON_PTR_DATA);
        rval = false;
    }

    return rval;
}

bool runtime_alter_server_relationships_from_json(SERVER* server, const char* type, json_t* json)
{
    bool rval = false;
    mxs::Closer<json_t*> old_json(server_to_json(server, ""));
    ss_dassert(old_json.get());

    if (is_valid_relationship_body(json))
    {
        mxs::Closer<json_t*> j(json_pack("{s: {s: {s: {s: O}}}}", "data",
                                         "relationships", type, "data",
                                         json_object_get(json, "data")));

        if (server_to_object_relations(server, old_json.get(), j.get()))
        {
            rval = true;
        }
    }

    return rval;
}

static bool object_relation_is_valid(const std::string& type, const std::string& value)
{
    return type == CN_SERVERS && server_find_by_unique_name(value.c_str());
}

/**
 * @brief Do a coarse validation of the monitor JSON
 *
 * @param json          JSON to validate
 * @param paths         List of paths that must be string values
 * @param relationships Path to relationships to validate
 * @param validator     Validation function
 *
 * @return True of the JSON is valid
 */
static bool validate_object_json(json_t* json, std::vector<std::string> paths,
                                 const char* relationships = nullptr,
                                 bool (*validator)(const std::string&, const std::string&) = nullptr)
{
    bool rval = false;
    json_t* value;

    if (is_valid_resource_body(json))
    {
        if (!(value = mxs_json_pointer(json, MXS_JSON_PTR_ID)))
        {
            runtime_error("Value not found: '%s'", MXS_JSON_PTR_ID);
        }
        else if (!json_is_string(value))
        {
            runtime_error("Value '%s' is not a string", MXS_JSON_PTR_ID);
        }
        else
        {
            for (auto&& a: paths)
            {
                if (!(value = mxs_json_pointer(json, a.c_str())))
                {
                    runtime_error("Invalid value for '%s'", a.c_str());
                }
                else if (!json_is_string(value))
                {
                    runtime_error("Value '%s' is not a string", a.c_str());
                }
            }

            if (relationships)
            {
                ss_dassert(validator);
                StringSet relations;
                if (extract_relations(json, relations, relationships, validator))
                {
                    rval = true;
                }
            }
        }
    }

    return rval;
}

static bool unlink_object_from_servers(const char* target, StringSet& relations)
{
    bool rval = true;

    for (StringSet::iterator it = relations.begin(); it != relations.end(); it++)
    {
        SERVER* server = server_find_by_unique_name(it->c_str());

        if (!server || !runtime_unlink_server(server, target))
        {
            rval = false;
            break;
        }
    }

    return rval;
}

static bool link_object_to_servers(const char* target, StringSet& relations)
{
    bool rval = true;

    for (StringSet::iterator it = relations.begin(); it != relations.end(); it++)
    {
        SERVER* server = server_find_by_unique_name(it->c_str());

        if (!server || !runtime_link_server(server, target))
        {
            unlink_server_from_objects(server, relations);
            rval = false;
            break;
        }
    }

    return rval;
}

MXS_MONITOR* runtime_create_monitor_from_json(json_t* json)
{
    MXS_MONITOR* rval = NULL;

    if (validate_object_json(json, {MXS_JSON_PTR_MODULE},
                             MXS_JSON_PTR_RELATIONSHIPS_SERVERS,
                             object_relation_is_valid))
    {
        const char* name = json_string_value(mxs_json_pointer(json, MXS_JSON_PTR_ID));
        const char* module = json_string_value(mxs_json_pointer(json, MXS_JSON_PTR_MODULE));

        if (runtime_create_monitor(name, module))
        {
            rval = monitor_find(name);
            ss_dassert(rval);

            if (!runtime_alter_monitor_from_json(rval, json))
            {
                runtime_destroy_monitor(rval);
                rval = NULL;
            }
        }
    }

    return rval;
}

MXS_FILTER_DEF* runtime_create_filter_from_json(json_t* json)
{
    MXS_FILTER_DEF* rval = NULL;

    if (validate_object_json(json, {MXS_JSON_PTR_MODULE},
                             MXS_JSON_PTR_RELATIONSHIPS_SERVICES,
                             filter_relation_is_valid))
    {
        const char* name = json_string_value(mxs_json_pointer(json, MXS_JSON_PTR_ID));
        const char* module = json_string_value(mxs_json_pointer(json, MXS_JSON_PTR_MODULE));
        MXS_CONFIG_PARAMETER* params = extract_parameters_from_json(json);

        if (runtime_create_filter(name, module, params))
        {
            rval = filter_def_find(name);
            ss_dassert(rval);
        }

        config_parameter_free(params);
    }

    return rval;
}

bool object_to_server_relations(const char* target, json_t* old_json, json_t* new_json)
{
    if (mxs_json_pointer(new_json, MXS_JSON_PTR_RELATIONSHIPS) == NULL)
    {
        /** No change to relationships */
        return true;
    }

    bool rval = false;
    StringSet old_relations;
    StringSet new_relations;
    const char* object_relation = MXS_JSON_PTR_RELATIONSHIPS_SERVERS;

    if (extract_relations(old_json, old_relations, object_relation, object_relation_is_valid) &&
        extract_relations(new_json, new_relations, object_relation, object_relation_is_valid))
    {
        StringSet removed_relations;
        StringSet added_relations;

        std::set_difference(old_relations.begin(), old_relations.end(),
                            new_relations.begin(), new_relations.end(),
                            std::inserter(removed_relations, removed_relations.begin()));

        std::set_difference(new_relations.begin(), new_relations.end(),
                            old_relations.begin(), old_relations.end(),
                            std::inserter(added_relations, added_relations.begin()));

        if (unlink_object_from_servers(target, removed_relations) &&
            link_object_to_servers(target, added_relations))
        {
            rval = true;
        }
    }
    else
    {
        runtime_error("Invalid object relations for '%s'", target);
    }

    return rval;
}

bool runtime_alter_monitor_from_json(MXS_MONITOR* monitor, json_t* new_json)
{
    bool rval = false;
    mxs::Closer<json_t*> old_json(monitor_to_json(monitor, ""));
    ss_dassert(old_json.get());

    if (is_valid_resource_body(new_json) &&
        object_to_server_relations(monitor->name, old_json.get(), new_json))
    {
        rval = true;
        bool changed = false;
        json_t* parameters = mxs_json_pointer(new_json, MXS_JSON_PTR_PARAMETERS);
        json_t* old_parameters = mxs_json_pointer(old_json.get(), MXS_JSON_PTR_PARAMETERS);

        ss_dassert(old_parameters);

        if (parameters)
        {
            const char* key;
            json_t* value;

            json_object_foreach(parameters, key, value)
            {
                json_t* new_val = json_object_get(parameters, key);
                json_t* old_val = json_object_get(old_parameters, key);

                if (old_val && new_val && mxs::json_to_string(new_val) == mxs::json_to_string(old_val))
                {
                    /** No change in values */
                }
                else if (runtime_alter_monitor(monitor, key, mxs::json_to_string(value).c_str()))
                {
                    changed = true;
                }
                else
                {
                    rval = false;
                }
            }
        }

        if (changed)
        {
            /** A configuration change was made, restart the monitor */
            monitor_stop(monitor);
            monitor_start(monitor, monitor->parameters);
        }
    }

    return rval;
}

bool runtime_alter_monitor_relationships_from_json(MXS_MONITOR* monitor, json_t* json)
{
    bool rval = false;
    mxs::Closer<json_t*> old_json(monitor_to_json(monitor, ""));
    ss_dassert(old_json.get());

    if (is_valid_relationship_body(json))
    {
        mxs::Closer<json_t*> j(json_pack("{s: {s: {s: {s: O}}}}", "data",
                                         "relationships", "servers", "data",
                                         json_object_get(json, "data")));

        if (object_to_server_relations(monitor->name, old_json.get(), j.get()))
        {
            rval = true;
        }
    }

    return rval;
}

bool runtime_alter_service_relationships_from_json(SERVICE* service, json_t* json)
{
    bool rval = false;
    mxs::Closer<json_t*> old_json(service_to_json(service, ""));
    ss_dassert(old_json.get());

    if (is_valid_relationship_body(json))
    {
        mxs::Closer<json_t*> j(json_pack("{s: {s: {s: {s: O}}}}", "data",
                                         "relationships", "servers", "data",
                                         json_object_get(json, "data")));

        if (object_to_server_relations(service->name, old_json.get(), j.get()))
        {
            rval = true;
        }
    }

    return rval;
}

/**
 * @brief Check if the service parameter can be altered at runtime
 *
 * @param key Parameter name
 * @return True if the parameter can be altered
 */
static bool is_dynamic_param(const std::string& key)
{
    return key != CN_TYPE &&
           key != CN_ROUTER &&
           key != CN_SERVERS;
}

bool runtime_alter_service_from_json(SERVICE* service, json_t* new_json)
{
    bool rval = false;
    mxs::Closer<json_t*> old_json(service_to_json(service, ""));
    ss_dassert(old_json.get());

    if (is_valid_resource_body(new_json) &&
        object_to_server_relations(service->name, old_json.get(), new_json))
    {
        rval = true;
        json_t* parameters = mxs_json_pointer(new_json, MXS_JSON_PTR_PARAMETERS);
        json_t* old_parameters = mxs_json_pointer(old_json.get(), MXS_JSON_PTR_PARAMETERS);

        ss_dassert(old_parameters);

        if (parameters)
        {
            /** Create a set of accepted service parameters */
            StringSet paramset;
            for (int i = 0; config_service_params[i].name; i++)
            {
                if (is_dynamic_param(config_service_params[i].name))
                {
                    paramset.insert(config_service_params[i].name);
                }
            }

            const MXS_MODULE *mod = get_module(service->routerModule, MODULE_ROUTER);

            for (int i = 0; mod->parameters[i].name; i++)
            {
                paramset.insert(mod->parameters[i].name);
            }

            const char* key;
            json_t* value;

            json_object_foreach(parameters, key, value)
            {
                json_t* new_val = json_object_get(parameters, key);
                json_t* old_val = json_object_get(old_parameters, key);

                if (old_val && new_val && mxs::json_to_string(new_val) == mxs::json_to_string(old_val))
                {
                    /** No change in values */
                }
                else if (paramset.find(key) != paramset.end())
                {
                    /** Parameter can be altered */
                    if (!runtime_alter_service(service, key, mxs::json_to_string(value).c_str()))
                    {
                        rval = false;
                    }
                }
                else
                {
                    std::string v = mxs::json_to_string(value);

                    if (!is_dynamic_param(key))
                    {
                        runtime_error("Runtime modifications to static service parameters is not supported: %s=%s", key, v.c_str());
                    }
                    else
                    {
                        runtime_error("Parameter '%s' cannot be modified at runtime", key);
                    }

                    rval = false;
                }
            }
        }
    }

    return rval;
}

bool validate_logs_json(json_t* json)
{
    json_t* param = mxs_json_pointer(json, MXS_JSON_PTR_PARAMETERS);
    bool rval = false;

    if (param && json_is_object(param))
    {
        rval = is_bool_or_null(param, "highprecision") &&
               is_bool_or_null(param, "maxlog") &&
               is_bool_or_null(param, "syslog") &&
               is_bool_or_null(param, "log_info") &&
               is_bool_or_null(param, "log_warning") &&
               is_bool_or_null(param, "log_notice") &&
               is_bool_or_null(param, "log_debug") &&
               is_count_or_null(param, "throttling/count") &&
               is_count_or_null(param, "throttling/suppress_ms") &&
               is_count_or_null(param, "throttling/window_ms");
    }

    return rval;
}

bool runtime_alter_logs_from_json(json_t* json)
{
    bool rval = false;

    if (validate_logs_json(json))
    {
        json_t* param = mxs_json_pointer(json, MXS_JSON_PTR_PARAMETERS);
        json_t* value;
        rval = true;

        if ((value = mxs_json_pointer(param, "highprecision")))
        {
            mxs_log_set_highprecision_enabled(json_boolean_value(value));
        }

        if ((value = mxs_json_pointer(param, "maxlog")))
        {
            mxs_log_set_maxlog_enabled(json_boolean_value(value));
        }

        if ((value = mxs_json_pointer(param, "syslog")))
        {
            mxs_log_set_syslog_enabled(json_boolean_value(value));
        }

        if ((value = mxs_json_pointer(param, "log_info")))
        {
            mxs_log_set_priority_enabled(LOG_INFO, json_boolean_value(value));
        }

        if ((value = mxs_json_pointer(param, "log_warning")))
        {
            mxs_log_set_priority_enabled(LOG_WARNING, json_boolean_value(value));
        }

        if ((value = mxs_json_pointer(param, "log_notice")))
        {
            mxs_log_set_priority_enabled(LOG_NOTICE, json_boolean_value(value));
        }

        if ((value = mxs_json_pointer(param, "log_debug")))
        {
            mxs_log_set_priority_enabled(LOG_DEBUG, json_boolean_value(value));
        }

        if ((param = mxs_json_pointer(param, "throttling")) && json_is_object(param))
        {
            MXS_LOG_THROTTLING throttle;
            mxs_log_get_throttling(&throttle);

            if ((value = mxs_json_pointer(param, "count")))
            {
                throttle.count = json_integer_value(value);
            }

            if ((value = mxs_json_pointer(param, "suppress_ms")))
            {
                throttle.suppress_ms = json_integer_value(value);
            }

            if ((value = mxs_json_pointer(param, "window_ms")))
            {
                throttle.window_ms = json_integer_value(value);
            }

            mxs_log_set_throttling(&throttle);
        }
    }

    return rval;
}

static bool validate_listener_json(json_t* json)
{
    bool rval = false;
    json_t* param;

    if (!(param = mxs_json_pointer(json, MXS_JSON_PTR_ID)))
    {
        runtime_error("Value not found: '%s'", MXS_JSON_PTR_ID);
    }
    else if (!json_is_string(param))
    {
        runtime_error("Value '%s' is not a string", MXS_JSON_PTR_ID);
    }
    else if (!(param = mxs_json_pointer(json, MXS_JSON_PTR_PARAMETERS)))
    {
        runtime_error("Value not found: '%s'", MXS_JSON_PTR_PARAMETERS);
    }
    else if (!json_is_object(param))
    {
        runtime_error("Value '%s' is not an object", MXS_JSON_PTR_PARAMETERS);
    }
    else if (is_count_or_null(param, CN_PORT) &&
             is_string_or_null(param, CN_ADDRESS) &&
             is_string_or_null(param, CN_AUTHENTICATOR) &&
             is_string_or_null(param, CN_AUTHENTICATOR_OPTIONS) &&
             validate_ssl_json(param))
    {
        rval = true;
    }

    return rval;
}

bool runtime_create_listener_from_json(SERVICE* service, json_t* json)
{
    bool rval = false;

    if (validate_listener_json(json))
    {
        std::string port = json_int_to_string(mxs_json_pointer(json, MXS_JSON_PTR_PARAM_PORT));

        const char* id = get_string_or_null(json, MXS_JSON_PTR_ID);
        const char* address = get_string_or_null(json, MXS_JSON_PTR_PARAM_ADDRESS);
        const char* protocol = get_string_or_null(json, MXS_JSON_PTR_PARAM_PROTOCOL);
        const char* authenticator = get_string_or_null(json, MXS_JSON_PTR_PARAM_AUTHENTICATOR);
        const char* authenticator_options = get_string_or_null(json, MXS_JSON_PTR_PARAM_AUTHENTICATOR_OPTIONS);
        const char* ssl_key = get_string_or_null(json, MXS_JSON_PTR_PARAM_SSL_KEY);
        const char* ssl_cert = get_string_or_null(json, MXS_JSON_PTR_PARAM_SSL_CERT);
        const char* ssl_ca_cert = get_string_or_null(json, MXS_JSON_PTR_PARAM_SSL_CA_CERT);
        const char* ssl_version = get_string_or_null(json, MXS_JSON_PTR_PARAM_SSL_VERSION);
        const char* ssl_cert_verify_depth = get_string_or_null(json, MXS_JSON_PTR_PARAM_SSL_CERT_VERIFY_DEPTH);
        const char* ssl_verify_peer_certificate = get_string_or_null(json, MXS_JSON_PTR_PARAM_SSL_VERIFY_PEER_CERT);

        rval = runtime_create_listener(service, id, address, port.c_str(), protocol,
                                       authenticator, authenticator_options,
                                       ssl_key, ssl_cert, ssl_ca_cert, ssl_version,
                                       ssl_cert_verify_depth, ssl_verify_peer_certificate);
    }

    return rval;
}

json_t* runtime_get_json_error()
{
    json_t* obj = NULL;
    std::string errmsg = runtime_get_error();

    if (errmsg.length())
    {
        obj = mxs_json_error(errmsg.c_str());
    }

    return obj;
}

bool validate_user_json(json_t* json)
{
    bool rval = false;
    json_t* id = mxs_json_pointer(json, MXS_JSON_PTR_ID);
    json_t* type = mxs_json_pointer(json, MXS_JSON_PTR_TYPE);
    json_t* password = mxs_json_pointer(json, MXS_JSON_PTR_PASSWORD);
    json_t* account = mxs_json_pointer(json, MXS_JSON_PTR_ACCOUNT);

    if (!id)
    {
        runtime_error("Request body does not define the '%s' field", MXS_JSON_PTR_ID);
    }
    else if (!json_is_string(id))
    {
        runtime_error("The '%s' field is not a string", MXS_JSON_PTR_ID);
    }
    else if (!type)
    {
        runtime_error("Request body does not define the '%s' field", MXS_JSON_PTR_TYPE);
    }
    else if (!json_is_string(type))
    {
        runtime_error("The '%s' field is not a string", MXS_JSON_PTR_TYPE);
    }
    else if (!account)
    {
        runtime_error("Request body does not define the '%s' field", MXS_JSON_PTR_ACCOUNT);
    }
    else if (!json_is_string(account))
    {
        runtime_error("The '%s' field is not a string", MXS_JSON_PTR_ACCOUNT);
    }
    else if (json_to_account_type(account) == USER_ACCOUNT_UNKNOWN)
    {
        runtime_error("The '%s' field is not a valid account value", MXS_JSON_PTR_ACCOUNT);
    }
    else
    {
        if (strcmp(json_string_value(type), CN_INET) == 0)
        {
            if (!password)
            {
                runtime_error("Request body does not define the '%s' field", MXS_JSON_PTR_PASSWORD);
            }
            else if (!json_is_string(password))
            {
                runtime_error("The '%s' field is not a string", MXS_JSON_PTR_PASSWORD);
            }
            else
            {
                rval = true;
            }
        }
        else if (strcmp(json_string_value(type), CN_UNIX) == 0)
        {
            rval = true;
        }
        else
        {
            runtime_error("Invalid value for field '%s': %s", MXS_JSON_PTR_TYPE,
                          json_string_value(type));
        }
    }

    return rval;
}

bool runtime_create_user_from_json(json_t* json)
{
    bool rval = false;

    if (validate_user_json(json))
    {
        const char* user = json_string_value(mxs_json_pointer(json, MXS_JSON_PTR_ID));
        const char* password = json_string_value(mxs_json_pointer(json, MXS_JSON_PTR_PASSWORD));
        std::string strtype = json_string_value(mxs_json_pointer(json, MXS_JSON_PTR_TYPE));
        user_account_type type = json_to_account_type(mxs_json_pointer(json, MXS_JSON_PTR_ACCOUNT));
        const char* err = NULL;

        if (strtype == CN_INET && (err = admin_add_inet_user(user, password, type)) == ADMIN_SUCCESS)
        {
            MXS_NOTICE("Create network user '%s'", user);
            rval = true;
        }
        else if (strtype == CN_UNIX && (err = admin_enable_linux_account(user, type)) == ADMIN_SUCCESS)
        {
            MXS_NOTICE("Enabled account '%s'", user);
            rval = true;
        }
        else if (err)
        {
            runtime_error("Failed to add user '%s': %s", user, err);
        }
    }

    return rval;
}

bool runtime_remove_user(const char* id, enum user_type type)
{
    bool rval = false;
    const char* err = type == USER_TYPE_INET ?
                      admin_remove_inet_user(id) :
                      admin_disable_linux_account(id);

    if (err == ADMIN_SUCCESS)
    {
        MXS_NOTICE("%s '%s'", type == USER_TYPE_INET ?
                   "Deleted network user" : "Disabled account", id);
        rval = true;
    }
    else
    {
        runtime_error("Failed to remove user '%s': %s", id, err);
    }

    return rval;
}

bool validate_maxscale_json(json_t* json)
{
    bool rval = false;
    json_t* param = mxs_json_pointer(json, MXS_JSON_PTR_PARAMETERS);

    if (param)
    {
        rval = is_count_or_null(param, CN_AUTH_CONNECT_TIMEOUT) &&
               is_count_or_null(param, CN_AUTH_READ_TIMEOUT) &&
               is_count_or_null(param, CN_AUTH_WRITE_TIMEOUT) &&
               is_bool_or_null(param, CN_ADMIN_AUTH) &&
               is_bool_or_null(param, CN_ADMIN_LOG_AUTH_FAILURES);
    }

    return rval;
}

bool ignored_core_parameters(const char* key)
{
    static const char* params[] =
    {
        "libdir",
        "datadir",
        "process_datadir",
        "cachedir",
        "configdir",
        "config_persistdir",
        "module_configdir",
        "piddir",
        "logdir",
        "langdir",
        "execdir",
        "connector_plugindir",
        NULL
    };

    for (int i = 0; params[i]; i++)
    {
        if (strcmp(key, params[i]) == 0)
        {
            return true;
        }
    }

    return false;
}

bool runtime_alter_maxscale_from_json(json_t* new_json)
{
    bool rval = false;

    if (validate_maxscale_json(new_json))
    {
        rval = true;
        json_t* old_json = config_maxscale_to_json("");
        ss_dassert(old_json);

        json_t* new_param = mxs_json_pointer(new_json, MXS_JSON_PTR_PARAMETERS);
        json_t* old_param = mxs_json_pointer(old_json, MXS_JSON_PTR_PARAMETERS);

        const char* key;
        json_t* value;

        json_object_foreach(new_param, key, value)
        {
            json_t* new_val = json_object_get(new_param, key);
            json_t* old_val = json_object_get(old_param, key);

            if (old_val && new_val && mxs::json_to_string(new_val) == mxs::json_to_string(old_val))
            {
                /** No change in values */
            }
            else if (ignored_core_parameters(key))
            {
                /** We can't change these at runtime */
                MXS_INFO("Ignoring runtime change to '%s': Cannot be altered at runtime", key);
            }
            else if (!runtime_alter_maxscale(key, mxs::json_to_string(value).c_str()))
            {
                rval = false;
            }
        }
    }

    return rval;
}
