#pragma once
/*
 * Copyright (c) 2016 MariaDB Corporation Ab
 *
 * Use of this software is governed by the Business Source License included
 * in the LICENSE.TXT file and at www.mariadb.com/bsl11.
 *
 * Change Date: 2020-01-01
 *
 * On the date above, in accordance with the Business Source License, use
 * of this software will be governed by version 2 or later of the General
 * Public License.
 */

/**
 * @file service.h
 *
 * The service level definitions within the gateway
 */

#include <maxscale/cdefs.h>

#include <time.h>
#include <openssl/crypto.h>
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/dh.h>

#include <maxscale/protocol.h>
#include <maxscale/spinlock.h>
#include <maxscale/dcb.h>
#include <maxscale/server.h>
#include <maxscale/listener.h>
#include <maxscale/filter.h>
#include <maxscale/hashtable.h>
#include <maxscale/resultset.h>
#include <maxscale/config.h>
#include <maxscale/jansson.h>

MXS_BEGIN_DECLS

struct server;
struct mxs_router;
struct mxs_router_object;
struct users;

#define MAX_SERVICE_USER_LEN 1024
#define MAX_SERVICE_PASSWORD_LEN 1024
#define MAX_SERVICE_WEIGHTBY_LEN 1024
#define MAX_SERVICE_VERSION_LEN 1024

/**
 * The service statistics structure
 */
typedef struct
{
    time_t started;         /**< The time when the service was started */
    int    n_failed_starts; /**< Number of times this service has failed to start */
    int    n_sessions;      /**< Number of sessions created on service since start */
    int    n_current;       /**< Current number of sessions */
} SERVICE_STATS;

/**
 * The service user structure holds the information that is needed
 for this service to allow the gateway to login to the backend
 database and extact information such as the user table or other
 database status or configuration data.
*/
typedef struct
{
    char name[MAX_SERVICE_USER_LEN];     /**< The user name to use to extract information */
    char authdata[MAX_SERVICE_PASSWORD_LEN]; /**< The authentication data requied */
} SERVICE_USER;

/**
 * The service refresh rate holds the counter and last load time_t
 for this service to load users data from the backend database
*/
typedef struct
{
    time_t last;      /*<< When was the users loaded the last time. */
    bool   warned;    /**< Has it been warned that the limit has been exceeded. */
    bool   reentrant; /**< Has the check been applied before. */
} SERVICE_REFRESH_RATE;

typedef struct server_ref_t
{
    struct server_ref_t *next; /**< Next server reference */
    SERVER* server;            /**< The actual server */
    int weight;                /**< Weight of this server */
    int connections;           /**< Number of connections created through this reference */
    bool active;               /**< Whether this reference is valid and in use*/
} SERVER_REF;

/** Macro to check whether a SERVER_REF is active */
#define SERVER_REF_IS_ACTIVE(ref) (ref->active && SERVER_IS_ACTIVE(ref->server))

#define SERVICE_MAX_RETRY_INTERVAL 3600 /*< The maximum interval between service start retries */

/** Value of service timeout if timeout checks are disabled */
#define SERVICE_NO_SESSION_TIMEOUT 0

/**
 * Parameters that are automatically detected but can also be configured by the
 * user are initially set to this value.
 */
#define SERVICE_PARAM_UNINIT -1

/* Refresh rate limits for load users from database */
#define USERS_REFRESH_TIME_DEFAULT   30 /* Allowed time interval (in seconds) after last update*/
#define USERS_REFRESH_TIME_MIN       3 /* Minimum allowed time interval (in seconds)*/

/** Default timeout values used by the connections which fetch user authentication data */
#define DEFAULT_AUTH_CONNECT_TIMEOUT 3
#define DEFAULT_AUTH_READ_TIMEOUT    1
#define DEFAULT_AUTH_WRITE_TIMEOUT   2

/**
 * Defines a service within the gateway.
 *
 * A service is a combination of a set of backend servers, a routing mechanism
 * and a set of client side protocol/port pairs used to listen for new connections
 * to the service.
 */
typedef struct service
{
    char *name;                        /**< The service name */
    int state;                         /**< The service state */
    int client_count;                  /**< Number of connected clients */
    int max_connections;               /**< Maximum client connections */
    SERV_LISTENER *ports;              /**< Linked list of ports and protocols
                                        * that this service will listen on */
    char *routerModule;                /**< Name of router module to use */
    char **routerOptions;              /**< Router specific option strings */
    struct mxs_router_object *router;  /**< The router we are using */
    struct mxs_router *router_instance;/**< The router instance for this service */
    char version_string[MAX_SERVICE_VERSION_LEN]; /**< version string for this service listeners */
    SERVER_REF *dbref;                 /**< server references */
    int         n_dbref;               /**< Number of server references */
    SERVICE_USER credentials;          /**< The cedentials of the service user */
    SPINLOCK spin;                     /**< The service spinlock */
    SERVICE_STATS stats;               /**< The service statistics */
    int enable_root;                   /**< Allow root user  access */
    int localhost_match_wildcard_host; /**< Match localhost against wildcard */
    MXS_CONFIG_PARAMETER* svc_config_param;/**<  list of config params and values */
    int svc_config_version;            /**<  Version number of configuration */
    bool svc_do_shutdown;              /**< tells the service to exit loops etc. */
    bool users_from_all;               /**< Load users from one server or all of them */
    bool strip_db_esc;                 /**< Remove the '\' characters from database names
                                        * when querying them from the server. MySQL Workbench seems
                                        * to escape at least the underscore character. */
    SERVICE_REFRESH_RATE *rate_limits; /**< The refresh rate limits for users of each thread */
    MXS_FILTER_DEF **filters;          /**< Ordered list of filters */
    int n_filters;                     /**< Number of filters */
    int64_t conn_idle_timeout;         /**< Session timeout in seconds */
    char weightby[MAX_SERVICE_WEIGHTBY_LEN]; /**< Service weighting parameter name */
    struct service *next;              /**< The next service in the linked list */
    bool retry_start;                  /**< If starting of the service should be retried later */
    bool log_auth_warnings;            /**< Log authentication failures and warnings */
    uint64_t capabilities;             /**< The capabilities of the service, @see enum routing_capability */
    int max_retry_interval;            /**< Maximum retry interval */
} SERVICE;

typedef enum count_spec_t
{
    COUNT_NONE = 0,
    COUNT_ATLEAST,
    COUNT_EXACT,
    COUNT_ATMOST
} count_spec_t;

#define SERVICE_STATE_ALLOC     1       /**< The service has been allocated */
#define SERVICE_STATE_STARTED   2       /**< The service has been started */
#define SERVICE_STATE_FAILED    3       /**< The service failed to start */
#define SERVICE_STATE_STOPPED   4       /**< The service has been stopped */

/**
 * Starting and stopping services
 */

/**
 * @brief Stop a service
 *
 * @param service Service to stop
 * @return True if service was stopped
 */
bool serviceStop(SERVICE *service);

/**
 * @brief Restart a stopped service
 *
 * @param service Service to restart
 * @return True if service was restarted
 */
bool serviceStart(SERVICE *service);

/**
 * @brief Start new a listener for a service
 *
 * @param service Service where the listener is linked
 * @param port Listener to start
 * @return True if listener was started
 */
bool serviceLaunchListener(SERVICE *service, SERV_LISTENER *port);

/**
 * @brief Stop a listener for a service
 *
 * @param service Service where the listener is linked
 * @param name Name of the listener
 * @return True if listener was stopped
 */
bool serviceStopListener(SERVICE *service, const char *name);

/**
 * @brief Restart a stopped listener
 *
 * @param service Service where the listener is linked
 * @param name Name of the listener
 * @return True if listener was restarted
 */
bool serviceStartListener(SERVICE *service, const char *name);

/**
 * Utility functions
 */
SERVICE* service_find(const char *name);

// TODO: Change binlogrouter to use the functions in config_runtime.h
bool serviceAddBackend(SERVICE *service, SERVER *server);

/**
 * @brief Check if a service uses a server
 * @param service Service to check
 * @param server Server being used
 * @return True if service uses the server
 */
bool serviceHasBackend(SERVICE *service, SERVER *server);

/**
 * @brief Find listener with specified properties.
 *
 * @param service Service to check
 * @param socket  Listener socket path
 * @param address Listener address
 * @param port    Listener port number
 *
 * @note Either socket should be NULL and port non-zero or socket
 *       non-NULL and port zero.
 *
 * @return True if service has the listener
 */
SERV_LISTENER* service_find_listener(SERVICE* service,
                                     const char* socket,
                                     const char* address,
                                     unsigned short port);

/**
 * @brief Check if a service has a listener
 *
 * @param service Service to check
 * @param protocol Listener protocol
 * @param address Listener address
 * @param port Listener port
 * @return True if service has the listener
 */
bool serviceHasListener(SERVICE* service, const char* name, const char* protocol,
                        const char* address, unsigned short port);

/**
 * @brief Check if a MaxScale service listens on a port
 *
 * @param port The port to check
 * @return True if a MaxScale service uses the port
 */
bool service_port_is_used(unsigned short port);

/**
 * @brief Check if the service has a listener with a matching name
 *
 * @param service Service to check
 * @param name    Name to compare to
 *
 * @return True if the service has a listener with a matching name
 */
bool service_has_named_listener(SERVICE *service, const char *name);

int   serviceGetUser(SERVICE *service, char **user, char **auth);
int   serviceSetUser(SERVICE *service, const char *user, const char *auth);
bool  serviceSetFilters(SERVICE *service, char *filters);
int   serviceEnableRootUser(SERVICE *service, int action);
int   serviceSetTimeout(SERVICE *service, int val);
int   serviceSetConnectionLimits(SERVICE *service, int max, int queued, int timeout);
void  serviceSetRetryOnFailure(SERVICE *service, const char* value);
void  serviceWeightBy(SERVICE *service, const char *weightby);
const char* serviceGetWeightingParameter(SERVICE *service);
int   serviceEnableLocalhostMatchWildcardHost(SERVICE *service, int action);
int   serviceStripDbEsc(SERVICE* service, int action);
int   serviceAuthAllServers(SERVICE *service, int action);
void  serviceSetVersionString(SERVICE *service, const char* value);
int   service_refresh_users(SERVICE *service);
void  service_reset_rate_limit(SERVICE *service);

/**
 * Diagnostics
 */

/**
 * @brief Print service authenticator diagnostics
 *
 * @param dcb     DCB to print to
 * @param service The service to diagnose
 */
void service_print_users(DCB *, const SERVICE *);

/**
 * @brief Convert a service to JSON
 *
 * @param service Service to convert
 * @param host    Hostname of this server
 *
 * @return JSON representation of the service
 */
json_t* service_to_json(const SERVICE* service, const char* host);

/**
 * @brief Convert all services to JSON
 *
 * @param host Hostname of this server
 *
 * @return A JSON array with all services
 */
json_t* service_list_to_json(const char* host);

/**
 * @brief Convert service listeners to JSON
 *
 * @param service Service whose listeners are converted
 * @param host    Hostname of this server
 *
 * @return Array of JSON format listeners
 */
json_t* service_listener_list_to_json(const SERVICE* service, const char* host);

/**
 * @brief Convert service listener to JSON
 *
 * @param service Service whose listener is converted
 * @param name    The name of the listener
 * @param host    Hostname of this server
 *
 * @return JSON format listener
 */
json_t* service_listener_to_json(const SERVICE* service, const char* name, const char* host);

/**
 * @brief Get links to services that relate to a server
 *
 * @param server Server to inspect
 * @param host   Hostname of this server
 *
 * @return Array of service links or NULL if no relations exist
 */
json_t* service_relations_to_server(const SERVER* server, const char* host);

/**
 * @brief Get links to services that relate to a filter
 *
 * @param filter Filter to inspect
 * @param host   Hostname of this server
 *
 * @return Array of service links
 */
json_t* service_relations_to_filter(const MXS_FILTER_DEF* filter, const char* host);

void       dprintAllServices(DCB *dcb);
void       dprintService(DCB *dcb, SERVICE *service);
void       dListServices(DCB *dcb);
void       dListListeners(DCB *dcb);
int        serviceSessionCountAll(void);
RESULTSET* serviceGetList(void);
RESULTSET* serviceGetListenerList(void);

/**
 * Get the capabilities of the servive.
 *
 * The capabilities of a service are the union of the capabilities of
 * its router and all filters.
 *
 * @return The service capabilities.
 */
static inline uint64_t service_get_capabilities(const SERVICE *service)
{
    return service->capabilities;
}

typedef enum service_version_which_t
{
    SERVICE_VERSION_ANY,  /*< Any version of the servers of a service. */
    SERVICE_VERSION_MIN,  /*< The minimum version. */
    SERVICE_VERSION_MAX,  /*< The maximum version. */
} service_version_which_t;

/**
 * Return the version of the service. The returned version can be
 *
 * - the version of any (in practice the first) server associated
 *   with the service,
 * - the smallest version of any of the servers associated with
 *   the service, or
 * - the largest version of any of the servers associated with
 *   the service.
 *
 * @param service  The service.
 * @param which    Which version.
 *
 * @return The version of the service.
 */
uint64_t service_get_version(const SERVICE *service, service_version_which_t which);

MXS_END_DECLS
