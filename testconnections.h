#ifndef TESTCONNECTIONS_H
#define TESTCONNECTIONS_H

#include "mariadb_nodes.h"

/**
 * @brief Class contains references to Master/Slave and Galera test setups
 * Test setup should consist of two setups: one Master/Slave and one Galera.
 *
 * Maxscale should be configured separatelly for every test.
 *
 * Test setup should be described by enviromental variables:
 * - Maxscale_IP - IP adress of Maxscale machine
 * - Maxscale_User - User name to access Maxscale services
 * - Maxscale_Password - Password to access Maxscale services
 * - Maxscale_sshkey - ssh key for Maxscale machine
 * - maxdir - Path to Maxscale home direcdtory
 * - maxscale_cnf - name of maxscale .cnf file (full)
 * - KillVMCommand - Command to kill a node (should handle one parameter: IP address of virtual machine to kill)
 * - StartVMCommand - Command to restart virtual machine (should handle one parameter: IP address of virtual machine to kill)
 * - GetLogsCommand - Command to copy log files from node virtual machines (should handle one parameter: IP address of virtual machine to kill)
 * - SysbenchDir - path to SysBench directory (sysbanch should be >= 0.5)
 * - repl_N - Number of Master/Slave setup nodes
 * - repl_NNN - IP address of node NNN (NNN - 3 digits node index starting from 000)
 * - repl_port_NNN - MariaDB port for node NNN
 * - repl_sshkey_NNN - ssh key to access node NNN (should be sutable for 'root' and 'ec2-user')
 * - repl_User - User name to access Master/Slav setup
 * - repl_Password - Password to access Master/Slave setup
 * - galera_N, galera_NNN, galera_port_NNN, galera_sshkey_NNN, galera_User, galera_Password - same for Galera setup
 *
 */
class TestConnections
{
public:
    /**
     * @brief TestConnections constructor: reads environmental variables, copies MaxScale.cnf for MaxScale machine
     * @param test_exec_name Path to currect executable
     */
    TestConnections(int argc, char *argv[]);

    /**
     * @brief TestConnections constructor: only reads environmental variables
     */
    TestConnections();

    /**
     * @brief global_result Result of test, 0 if PASSED
     */
    int global_result;

    /**
     * @brief test_name Neme of the test
     */
    char * test_name;

    /**
     * @brief rwsplit_port RWSplit service port
     */
    int rwsplit_port;

    /**
     * @brief readconn_master_port ReadConnection in master mode service port
     */
    int readconn_master_port;

    /**
     * @brief readconn_slave_port ReadConnection in slave mode service port
     */
    int readconn_slave_port;

    /**
     * @brief binlog_port binlog router service port
     */
    int binlog_port;

    /**
     * @brief conn_rwsplit  MYSQL connection struct to RWSplit service
     */
    MYSQL *conn_rwsplit;

    /**
     * @brief conn_master   MYSQL connection struct to ReadConnection in master mode service
     */
    MYSQL *conn_master;

    /**
     * @brief conn_slave MYSQL connection struct to ReadConnection in slave mode service
     */
    MYSQL *conn_slave;

    /**
     * @brief galera Mariadb_nodes object containing references to Galera setuo
     */
    Mariadb_nodes * galera;

    /**
     * @brief repl Mariadb_nodes object containing references to Master/Slave setuo
     */
    Mariadb_nodes * repl;

    /**
     * @brief Maxscale_IP   Maxscale machine IP address
     */
    char maxscale_IP[16];

    /**
     * @brief maxscale_hostname  Maxscale machine 'hostname' value
     */
    char maxscale_hostname[1024];

    /**
     * @brief Maxscale_User User name to access Maxscale services
     */
    char maxscale_user[256];

    /**
     * @brief Maxscale_Password Password to access Maxscale services
     */
    char maxscale_password[256];

    /**
     * @brief maxadmin_Password Password to access Maxadmin tool
     */
    char maxadmin_password[256];

    /**
     * @brief Maxscale_sshkey   ssh key for Maxscale machine
     */
    char maxscale_sshkey[4096];

    /**
     * @brief GetLogsCommand    Command to copy log files from node virtual machines (should handle one parameter: IP address of virtual machine to kill)
     */
    char get_logs_command[4096];

    /**
     * @brief SysbenchDir   path to SysBench directory (sysbanch should be >= 0.5)
     */
    char sysbench_dir[4096];

    /**
     * @brief maxdir path to MaxScale
     */
    char maxdir[4096];

    /**
      * @brief maxscale_cnf full name of Maxscale configuration file
      */
    char maxscale_cnf[4096];

    /**
      * @brief maxscale_log_dir name of log files directory
      */
    char maxscale_log_dir[4096];

    /**
      * @brief maxscale_lbinog_dir name of binlog files (for binlog router) directory
      */
    char maxscale_binlog_dir[4096];

    /**
     * @brief test_dir path to test application
     */
    char test_dir[4096];

    /**
     * @brief maxscale_access_user username to access test machines
     */
    char maxscale_access_user[256];

    /**
     * @brief maxscale_access_homedir home directory of access_user
     */
    char maxscale_access_homedir[256];

    /**
     * @brief maxscale_access_sudo empty if sudo is not needed or "sudo " if sudo is needed.
     */
    char maxscale_access_sudo[64];

    /**
     * @brief no_maxscale_stop if true copy_all_logs() does not stop Maxscale
     */
    bool no_maxscale_stop;

    /**
     * @brief no_maxscale_start if true Maxscale won't be started and Maxscale.cnf won't be uploaded
     */
    bool no_maxscale_start;

    /**
     * @brief no_nodes_check if true nodes are not checked before test and are not restarted
     */
    bool no_nodes_check;

    /**
     * @brief verbose if true more printing activated
     */
    bool verbose;

    /**
     * @brief smoke if true all tests are executed in quick mode
     */
    bool smoke;

    /**
     * @brief binlog_cmd_option index of mariadb start option
     */
    int binlog_cmd_option;

    /**
     * @brief ssl if true ssl  will be used
     */
    int ssl;

    /**
     * @brief timeout seconds until test termination
     */
    int timeout;

    /**
     * @brief timeout_thread_p pointer to timeout thread
     */
    pthread_t timeout_thread_p;

    /**
     * @brief start_time time when test was started (used by printf to print Timestamp)
     */
    timeval start_time;

    /**
     * @brief add_result adds result to global_result and prints error message if result is not 0
     * @param result 0 if step PASSED
     * @param format ... message to pring if result is not 0
     */
    void add_result(int result, const char *format, ...);

    /**
     * @brief ReadEnv Reads all Maxscale and Master/Slave and Galera setups info from environmental variables
     * @return 0 in case of success
     */
    int read_env();

    /**
     * @brief PrintIP   Prints all Maxscale and Master/Slave and Galera setups info
     * @return 0
     */
    int print_env();

    /**
     * @brief InitMaxscale  Copies MaxSclae.cnf and start MaxScale
     * @return 0 if case of success
     */
    int init_maxscale();

    /**
     * @brief ConnectMaxscale   Opens connections to RWSplit, ReadConn master and ReadConn slave Maxscale services
     * Opens connections to RWSplit, ReadConn master and ReadConn slave Maxscale services
     * Connections stored in conn_rwsplit, conn_master and conn_slave MYSQL structs
     * @return 0 in case of success
     */
    int connect_maxscale();

    /**
     * @brief CloseMaxscaleConn Closes connection that were opened by ConnectMaxscale()
     * @return 0
     */
    int close_maxscale_connections();

    /**
     * @brief ConnectRWSplit    Opens connections to RWSplit and store MYSQL struct in conn_rwsplit
     * @return 0 in case of success
     */
    int connect_rwsplit() {conn_rwsplit = open_conn(rwsplit_port, maxscale_IP, maxscale_user, maxscale_password, ssl); if (conn_rwsplit == NULL){return(1);} else {return(0);}}

    /**
     * @brief ConnectReadMaster Opens connections to ReadConn master and store MYSQL struct in conn_master
     * @return 0 in case of success
     */
    int connect_readconn_master() {conn_master = open_conn(readconn_master_port, maxscale_IP, maxscale_user, maxscale_password, ssl);  if (conn_master == NULL){return(1);} else {return(0);}}

    /**
     * @brief ConnectReadSlave Opens connections to ReadConn slave and store MYSQL struct in conn_slave
     * @return 0 in case of success
     */
    int connect_readconn_slave() {conn_slave = open_conn(readconn_slave_port, maxscale_IP, maxscale_user, maxscale_password, ssl); if (conn_slave == NULL){return(1);} else {return(0);}}

    /**
     * @brief OpenRWSplitConn   Opens new connections to RWSplit and returns MYSQL struct
     * To close connection mysql_close() have to be called
     * @return MYSQL struct
     */
    MYSQL * open_rwsplit_connection() {return open_conn(rwsplit_port, maxscale_IP, maxscale_user, maxscale_password, ssl);}

    /**
     * @brief OpenReadMasterConn    Opens new connections to ReadConn master and returns MYSQL struct
     * To close connection mysql_close() have to be called
     * @return MYSQL struct
     */
    MYSQL * open_readconn_master_connection() {return open_conn(readconn_master_port, maxscale_IP, maxscale_user, maxscale_password, ssl);}

    /**
     * @brief OpenReadSlaveConn    Opens new connections to ReadConn slave and returns MYSQL struct
     * To close connection mysql_close() have to be called
     * @return  MYSQL struct
     */
    MYSQL * open_readconn_slave_connection() {return open_conn(readconn_slave_port, maxscale_IP, maxscale_user, maxscale_password, ssl);}

    /**
     * @brief CloseRWSplit Closes RWplit connections stored in conn_rwsplit
     */
    void close_rwsplit(){mysql_close(conn_rwsplit);}

    /**
     * @brief CloseReadMaster Closes ReadConn master connections stored in conn_master
     */
    void close_readconn_master(){mysql_close(conn_master);}

    /**
     * @brief CloseReadSlave Closes ReadConn slave connections stored in conn_slave
     */
    void close_readconn_slave(){mysql_close(conn_slave);}

    /**
     * @brief restart_maxscale Issues 'service maxscale restart' command
     */
    int restart_maxscale();

    /**
     * @brief start_maxscale Issues 'service maxscale start' command
     */
    int start_maxscale();

    /**
     * @brief stop_maxscale Issues 'service maxscale stop' command
     */
    int stop_maxscale();

    /**
     * @brief start_binlog configure first node as Master, Second as slave connected to Master and others as slave connected to MaxScale binlog router
     * @return  0 in case of success
     */
    int start_binlog();

    /**
     * @brief start_mm configure first node as Master for second, Second as Master for first
     * @return  0 in case of success
     */
    int start_mm();

    /**
     * @brief Copy_all_logs Copies all MaxScale logs and (if happens) core to current workspace
     */
    int copy_all_logs();

    /**
     * @brief Execute a command via ssh on the MaxScale machine
     * @param ssh ssh command to execute on the MaxScale machine
     * @return Output of the command or NULL if the command failed to execute
     */
    char* execute_ssh_maxscale(char* ssh);

    /**
     * @brief Test that connections to MaxScale are in the expected state
     * @param rw_split State of the MaxScale connection to Readwritesplit. True for working connection, false for no connection. 
     * @param rc_master State of the MaxScale connection to Readconnroute Master. True for working connection, false for no connection.
     * @param rc_slave State of the MaxScale connection to Readconnroute Slave. True for working connection, false for no connection.
     * @return  True if connections are in the expected state
     */
    bool test_maxscale_connections(bool rw_split,
                                   bool rc_master,
                                   bool rc_slave);

    /**
     * @brief Reconfigure MaxScale according to a configuration template
     * @param config_template Name on the configuration template
     * @return  0 in case of success
     */
    int reconfigure_maxscale(char* config_template);

    /**
     * @brief Create a number of connections to all services, run simple query, close all connections
     * @param conn_N number of connections
     * @return  0 in case of success
     */
    int create_connections(int conn_N);


    /**
     * Trying to get client IP address by connection to DB and execution 'show processlist'
     *
     * @param ip	connections from this IP address are counted
     * @param db    name of DB to which connections are counted
     * @return number of connections
     */
    int get_client_ip(char * ip);

    /**
     * @brief set_timeout startes timeout thread which terminates test application after timeout_seconds
     * @param timeout_seconds timeout time
     * @return 0 if success
     */
    int set_timeout(int timeout_seconds);

    /**
     * @brief stop_timeout stops timeout thread
     * @return 0
     */
    int stop_timeout();

    /**
     * @brief printf adds timestam to printf
     * @param __format
     * @return
     */

    int tprintf(const char *format, ...);


    /**
     * @brief Creats t1 table, insert data into it and checks if data can be correctly read from all Maxscale services
     * @param Test Pointer to TestConnections object that contains references to test setup
     * @param N number of INSERTs; every next INSERT is longer 16 times in compare with previous one: for N=4 last INSERT is about 700kb long
     * @return 0 in case of no error and all checks are ok
     */
    int insert_select(int N);

    /**
     * @brief Executes USE command for all Maxscale service and all Master/Slave backend nodes
     * @param Test Pointer to TestConnections object that contains references to test setup
     * @param db Name of DB in 'USE' command
     * @return 0 in case of success
     */
    int use_db(char * db);

    /**
     * @brief Checks if table t1 exists in DB
     * @param Test Pointer to TestConnections object that contains references to test setup
     * @param presence expected result
     * @param db DB name
     * @return 0 if (t1 table exists AND presence=TRUE) OR (t1 table does not exist AND presence=FALSE)
     */

    int check_t1_table(bool presence, char * db);

    /**
     * @brief CheckLogErr Reads error log and tried to search for given string
     * @param err_msg Error message to search in the log
     * @param expected TRUE if err_msg is expedted in the log, FALSE if err_msg should NOT be in the log
     * @return 0 if (err_msg is found AND expected is TRUE) OR (err_msg is NOT found in the log AND expected is FALSE)
     */
    void check_log_err(char * err_msg, bool expected);

    /**
     * @brief FindConnectedSlave Finds slave node which has connections from MaxScale
     * @param Test TestConnections object which contains info about test setup
     * @param global_result pointer to variable which is increased in case of error
     * @return index of found slave node
     */
    int find_connected_slave(int * global_result);

    /**
     * @brief FindConnectedSlave1 same as FindConnectedSlave() but does not increase global_result
     * @param Test  TestConnections object which contains info about test setup
     * @return index of found slave node
     */
    int find_connected_slave1();

    /**
     * @brief CheckMaxscaleAlive Checks if MaxScale is alive
     * Reads test setup info from enviromental variables and tries to connect to all Maxscale services to check if i is alive.
     * Also 'show processlist' query is executed using all services
     * @return 0 in case if success
     */
    int check_maxscale_alive();

    /**
     * @brief try_query Executes SQL query and repors error
     * @param conn MYSQL struct
     * @param sql SQL string
     * @return 0 if ok
     */
    int try_query(MYSQL *conn, const char *sql);
};



/**
 * @brief timeout_thread Thread which terminates test application after 'timeout' milliseconds
 * @param ptr pointer to TestConnections object
 * @return void
 */
void * timeout_thread(void *ptr );

#endif // TESTCONNECTIONS_H
