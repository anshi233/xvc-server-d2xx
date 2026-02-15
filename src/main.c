/*
 * main.c - XVC Server Main Entry
 * XVC Server for Digilent HS2
 * Multi-instance server with process isolation
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <arpa/inet.h>
#include <getopt.h>
#include <time.h>
#include "config.h"
#include "device_manager.h"
#include "ftdi_adapter.h"
#include "xvc_protocol.h"
#include "tcp_server.h"
#include "whitelist.h"
#include "logging.h"

#define VERSION "1.0.0"

/* Global state */
static volatile sig_atomic_t g_running = 1;
static volatile sig_atomic_t g_reload = 0;
static xvc_global_config_t g_config;
static device_manager_t g_device_mgr;

/* Instance context (for child processes) */
typedef struct {
    int instance_id;
    xvc_instance_config_t *config;
    ftdi_context_t *ftdi;
    tcp_server_t server;
    whitelist_t whitelist;
    xvc_context_t xvc;
    tcp_connection_t *active_xvc_conn;  /* Track active XVC session */
    
    /* Client IP locking for session persistence */
    char locked_client_ip[INET_ADDRSTRLEN];  /* IP address of locked client */
    time_t lock_until;                       /* Timestamp when lock expires */
    bool is_locked;                          /* Whether instance is locked to a client IP */
} instance_ctx_t;

/* Signal handlers */
static void signal_handler(int sig)
{
    switch (sig) {
        case SIGTERM:
        case SIGINT:
            g_running = 0;
            break;
        case SIGHUP:
            g_reload = 1;
            break;
        case SIGCHLD:
            /* Child process exited - will be handled in main loop */
            break;
    }
}

static void setup_signals(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);
    sigaction(SIGCHLD, &sa, NULL);
}

/* Helper: check if client IP matches the locked IP */
static bool client_ip_matches(instance_ctx_t *ctx, tcp_connection_t *conn)
{
    if (!ctx->is_locked) {
        return true;  /* No lock, any IP can connect */
    }
    
    char client_ip[INET_ADDRSTRLEN];
    tcp_connection_ip(conn, client_ip, sizeof(client_ip));
    
    return (strcmp(client_ip, ctx->locked_client_ip) == 0);
}

/* Helper: check if the client lock has expired */
static bool is_lock_expired(instance_ctx_t *ctx)
{
    if (!ctx->is_locked) {
        return true;  /* No lock */
    }
    
    time_t now = time(NULL);
    if (now >= ctx->lock_until) {
        return true;  /* Lock expired */
    }
    
    return false;
}

/* Helper: release the client IP lock */
static void release_client_lock(instance_ctx_t *ctx)
{
    if (ctx->is_locked) {
        LOG_INFO("Client IP lock released for %s", ctx->locked_client_ip);
        ctx->is_locked = false;
        ctx->locked_client_ip[0] = '\0';
        ctx->lock_until = 0;
    }
}

/* Helper: set client IP lock */
static void set_client_lock(instance_ctx_t *ctx, const char *client_ip)
{
    if (ctx->config->client_lock_timeout > 0) {
        strncpy(ctx->locked_client_ip, client_ip, INET_ADDRSTRLEN - 1);
        ctx->locked_client_ip[INET_ADDRSTRLEN - 1] = '\0';
        ctx->lock_until = time(NULL) + ctx->config->client_lock_timeout;
        ctx->is_locked = true;
        LOG_DBG("Client IP locked to %s (expires in %d seconds)", 
                client_ip, ctx->config->client_lock_timeout);
    }
}

/* Callback: handle new connection */
static int on_client_connect(void *user_data, tcp_connection_t *conn)
{
    instance_ctx_t *ctx = (instance_ctx_t*)user_data;
    char client_ip[INET_ADDRSTRLEN];
    tcp_connection_ip(conn, client_ip, sizeof(client_ip));
    
    /* Check if lock has expired (clean up old locks) */
    if (ctx->is_locked && is_lock_expired(ctx)) {
        LOG_INFO("Client IP lock for %s has expired, accepting new connections", 
                 ctx->locked_client_ip);
        release_client_lock(ctx);
    }
    
    /* Reject new connections if an XVC session is already active */
    if (ctx->active_xvc_conn != NULL && ctx->active_xvc_conn != conn) {
        LOG_WARN("Rejecting connection from %s - XVC session already active with fd=%d", 
                 client_ip, ctx->active_xvc_conn->fd);
        return 1;  /* Reject connection */
    }
    
    /* Check IP lock - only the locked IP can connect */
    if (ctx->is_locked && !client_ip_matches(ctx, conn)) {
        int remaining = (int)(ctx->lock_until - time(NULL));
        LOG_WARN("Rejecting connection from %s - instance is locked to %s for %d more seconds", 
                 client_ip, ctx->locked_client_ip, remaining);
        return 1;  /* Reject connection */
    }
    
    /* First connection to idle server - set the IP lock */
    if (!ctx->is_locked && ctx->config->client_lock_timeout > 0) {
        set_client_lock(ctx, client_ip);
        LOG_INFO("Instance locked to client IP %s (timeout: %d seconds)", 
                 client_ip, ctx->config->client_lock_timeout);
    }
    
    return 0;  /* Accept connection */
}

/* Callback: handle incoming data on connection */
static int on_client_data(void *user_data, tcp_connection_t *conn)
{
    instance_ctx_t *ctx = (instance_ctx_t*)user_data;
    
    /* Mark this connection as having an active XVC session */
    if (ctx->active_xvc_conn == NULL) {
        ctx->active_xvc_conn = conn;
        LOG_DBG("XVC session started on fd=%d", conn->fd);
    }
    
    /* Handle XVC protocol - xvc_handle will re-init if socket changed */
    int ret = xvc_handle(&ctx->xvc, conn->fd, ctx->ftdi, ctx->config->xvc_buffer_size, ctx->config->frequency);
    
    if (ret != 0) {
        xvc_close(&ctx->xvc);
        xvc_free(&ctx->xvc);
        ctx->active_xvc_conn = NULL;  /* Clear active session */
        return 1;  /* Close connection */
    }
    
    return 0;  /* Continue */
}

/* Callback: handle connection disconnect */
static void on_client_disconnect(void *user_data, tcp_connection_t *conn)
{
    instance_ctx_t *ctx = (instance_ctx_t*)user_data;
    
    /* Clear active XVC session if this was the active connection */
    if (ctx->active_xvc_conn == conn) {
        char client_ip[INET_ADDRSTRLEN];
        tcp_connection_ip(conn, client_ip, sizeof(client_ip));
        
        LOG_DBG("XVC session ended on fd=%d from %s", conn->fd, client_ip);
        ctx->active_xvc_conn = NULL;
        
        /* Set/refresh the IP lock when client disconnects */
        if (ctx->config->client_lock_timeout > 0) {
            set_client_lock(ctx, client_ip);
            LOG_INFO("Client %s disconnected - instance locked for %d seconds", 
                     client_ip, ctx->config->client_lock_timeout);
        }
    }
}

/* Run instance process */
static int run_instance(xvc_instance_config_t *inst_config)
{
    instance_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    
    ctx.instance_id = inst_config->instance_id;
    ctx.config = inst_config;
    
    /* Update log prefix */
    log_set_instance(inst_config->instance_id);
    
    LOG_INFO("Instance %d starting on port %d", 
             inst_config->instance_id, inst_config->port);
    
    /* Open FTDI device with D2XX */
    LOG_DBG("[STEP 1] Opening FTDI device with D2XX...");
    
    ctx.ftdi = ftdi_adapter_create();
    if (!ctx.ftdi) {
        LOG_ERROR("Failed to create FTDI context");
        return 1;
    }
    LOG_DBG("[STEP 2] FTDI adapter created");
    
    int ret;
    const char *serial = inst_config->device_id.value;
    LOG_DBG("[STEP 3] Opening FTDI device (SN: %s)...", serial ? serial : "any");
    
    /* Use the serial number from config (discovered by parent) */
    ret = ftdi_adapter_open(ctx.ftdi, -1, -1, serial, 0, 0);
    
    if (ret < 0) {
        LOG_ERROR("Failed to open FTDI device: %s", ftdi_adapter_error(ctx.ftdi));
        ftdi_adapter_destroy(ctx.ftdi);
        return 1;
    }
    LOG_DBG("[STEP 4] FTDI device opened successfully");
    
    /* Set frequency and latency */
    if (inst_config->frequency > 0) {
        LOG_DBG("[STEP 5] Setting frequency to %u Hz", inst_config->frequency);
        ftdi_adapter_set_frequency(ctx.ftdi, inst_config->frequency);
        LOG_DBG("[STEP 5.1] Frequency set");
    }
    if (inst_config->latency_timer > 0) {
        LOG_DBG("[STEP 6] Setting latency timer to %d", inst_config->latency_timer);
        ftdi_adapter_set_latency(ctx.ftdi, inst_config->latency_timer);
        LOG_DBG("[STEP 6.1] Latency timer set");
    }
    LOG_DBG("[STEP 7] Device configuration complete");
    
    /* Load whitelist */
    whitelist_load(&ctx.whitelist, inst_config);
    
    /* Initialize TCP server */
    LOG_DBG("[STEP 8] Initializing TCP server...");
    if (tcp_server_init(&ctx.server, inst_config->port, &ctx.whitelist) < 0) {
        LOG_ERROR("Failed to initialize TCP server");
        ftdi_adapter_close(ctx.ftdi);
        ftdi_adapter_destroy(ctx.ftdi);
        return 1;
    }
    
    tcp_server_set_callbacks(&ctx.server, on_client_connect, on_client_data, on_client_disconnect, &ctx);
    
    if (tcp_server_start(&ctx.server) < 0) {
        LOG_ERROR("Failed to start TCP server on port %d", inst_config->port);
        ftdi_adapter_close(ctx.ftdi);
        ftdi_adapter_destroy(ctx.ftdi);
        return 1;
    }
    LOG_DBG("[STEP 9] TCP server started on port %d", inst_config->port);
    
    LOG_INFO("Instance %d ready: port=%d, device=%s", 
             inst_config->instance_id, inst_config->port, inst_config->device_id.value);
    
    /* Main event loop */
    while (g_running) {
        int ret = tcp_server_poll(&ctx.server, 1000);
        if (ret < 0) {
            LOG_ERROR("Server poll error");
            break;
        }
    }
    
    /* Cleanup */
    tcp_server_stop(&ctx.server);
    ftdi_adapter_close(ctx.ftdi);
    ftdi_adapter_destroy(ctx.ftdi);
    
    LOG_INFO("Instance %d stopped", inst_config->instance_id);
    return 0;
}

/* Spawn child process for instance */
static pid_t spawn_instance(xvc_instance_config_t *inst_config)
{
    pid_t pid = fork();
    
    if (pid < 0) {
        LOG_ERROR("fork() failed: %s", strerror(errno));
        return -1;
    }
    
    if (pid == 0) {
        /* Child process */
        int ret = run_instance(inst_config);
        exit(ret);
    }
    
    /* Parent process */
    inst_config->pid = pid;
    LOG_INFO("Spawned instance %d (PID %d) on port %d",
             inst_config->instance_id, pid, inst_config->port);
    
    return pid;
}

/* Wait for child process */
static void check_children(void)
{
    int status;
    pid_t pid;
    
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        /* Find which instance this was */
        for (int i = 0; i < g_config.instance_count; i++) {
            xvc_instance_config_t *inst = &g_config.instances[i];
            if (inst->pid == pid) {
                if (WIFEXITED(status)) {
                    LOG_WARN("Instance %d (PID %d) exited with status %d",
                             inst->instance_id, pid, WEXITSTATUS(status));
                } else if (WIFSIGNALED(status)) {
                    LOG_WARN("Instance %d (PID %d) killed by signal %d",
                             inst->instance_id, pid, WTERMSIG(status));
                }
                
                inst->pid = 0;
                
                /* Auto-restart if still running */
                if (g_running && inst->enabled) {
                    LOG_INFO("Restarting instance %d...", inst->instance_id);
                    sleep(1);  /* Brief delay before restart */
                    spawn_instance(inst);
                }
                break;
            }
        }
    }
}

static void print_usage(const char *prog)
{
    printf("Usage: %s [OPTIONS] <config-file>\n\n", prog);
    printf("Run XVC server for Digilent HS2 devices.\n\n");
    printf("Options:\n");
    printf("  -d, --daemon           Run as daemon\n");
    printf("  -p, --port PORT        Override base port\n");
    printf("  -v, --verbose          Increase log level (can be used multiple times)\n");
    printf("                          0 = INFO (default)\n");
    printf("                          1 = DEBUG (-v)\n");
    printf("                          2+ = TRACE (-vv, -vvv)\n");
    printf("  -h, --help             Display this help message\n");
    printf("  -V, --version          Display version information\n");
    printf("\nExamples:\n");
    printf("  %s /etc/xvc-server/xvc-server-multi.conf\n", prog);
    printf("  %s -d /etc/xvc-server/xvc-server-multi.conf\n", prog);
    printf("  %s -v /etc/xvc-server/xvc-server-multi.conf\n", prog);
    printf("  %s -vv /etc/xvc-server/xvc-server-multi.conf\n", prog);
}

static void print_version(void)
{
    printf("xvc-server version %s\n", VERSION);
    printf("XVC Server for Digilent HS2 JTAG Adapters\n");
}

int main(int argc, char *argv[])
{
    static struct option long_options[] = {
        {"daemon", no_argument,       0, 'd'},
        {"port",    required_argument, 0, 'p'},
        {"verbose", no_argument,       0, 'v'},
        {"help",    no_argument,       0, 'h'},
        {"version", no_argument,       0, 'V'},
        {0, 0, 0, 0}
    };
    
    int daemonize = 0;
    int verbose = 0;
    int port_override = 0;
    
    int opt;
    while ((opt = getopt_long(argc, argv, "dp:vhV", long_options, NULL)) != -1) {
        switch (opt) {
            case 'd':
                daemonize = 1;
                break;
            case 'p':
                port_override = atoi(optarg);
                break;
            case 'v':
                verbose++;
                break;
            case 'h':
                print_usage(argv[0]);
                return 0;
            case 'V':
                print_version();
                return 0;
            default:
                print_usage(argv[0]);
                return 1;
        }
    }
    
    if (optind >= argc) {
        fprintf(stderr, "Error: Configuration file required\n\n");
        print_usage(argv[0]);
        return 1;
    }
    
    const char *config_file = argv[optind];
    
    /* Map verbose count to log level */
    log_level_t log_level;
    if (verbose == 0) {
        log_level = XVC_LOG_INFO;
    } else if (verbose == 1) {
        log_level = XVC_LOG_DEBUG;
    } else {
        log_level = XVC_LOG_TRACE;
    }
    
    /* Initialize logging */
    log_config_t log_cfg = {
        .level = log_level,
        .targets = LOG_TARGET_STDERR | (daemonize ? LOG_TARGET_SYSLOG : 0),
        .include_timestamp = true,
        .include_level = true,
        .include_source = verbose >= 2,
        .instance_id = 0
    };
    log_init(&log_cfg);
    
    LOG_INFO("XVC Server %s starting...", VERSION);
    LOG_INFO("Log level: %s", log_level_name(log_level));
    
    /* Setup signal handlers */
    setup_signals();
    
    /* Load configuration */
    if (config_load(&g_config, config_file) < 0) {
        LOG_FATAL("Failed to load configuration: %s", config_file);
        return 1;
    }
    
    if (port_override) {
        g_config.base_port = port_override;
        for (int i = 0; i < g_config.instance_count; i++) {
            g_config.instances[i].port = port_override + i;
        }
    }
    
    /* Daemonize if requested */
    if (daemonize) {
        if (daemon(0, 0) < 0) {
            LOG_FATAL("daemon() failed: %s", strerror(errno));
            return 1;
        }
    }
    
    /* For single instance, run directly without fork to avoid D2XX issues */
    if (g_config.instance_count == 1 && g_config.instances[0].enabled) {
        LOG_INFO("Single instance mode: running directly (no fork)");
        int ret = run_instance(&g_config.instances[0]);
        config_free(&g_config);
        log_shutdown();
        return ret;
    }
    
    LOG_INFO("Multi-instance mode: spawning children (D2XX may have issues with fork)");

    /* Spawn instances */
    for (int i = 0; i < g_config.instance_count; i++) {
        xvc_instance_config_t *inst = &g_config.instances[i];
        if (inst->enabled) {
            spawn_instance(inst);
        }
    }
    
    /* Main loop - monitor children */
    LOG_INFO("Instance manager running, %d instance(s) active", g_config.instance_count);
    
    while (g_running) {
        check_children();
        
        if (g_reload) {
            LOG_INFO("Reload requested (SIGHUP)");
            /* TODO: Implement config reload */
            g_reload = 0;
        }
        
        sleep(1);
    }
    
    /* Shutdown */
    LOG_INFO("Shutting down...");
    
    /* Kill child processes */
    for (int i = 0; i < g_config.instance_count; i++) {
        if (g_config.instances[i].pid > 0) {
            LOG_INFO("Stopping instance %d (PID %d)", 
                     g_config.instances[i].instance_id,
                     g_config.instances[i].pid);
            kill(g_config.instances[i].pid, SIGTERM);
        }
    }
    
    /* Wait for children */
    for (int i = 0; i < g_config.instance_count; i++) {
        if (g_config.instances[i].pid > 0) {
            waitpid(g_config.instances[i].pid, NULL, 0);
        }
    }
    
    /* Cleanup */
    config_free(&g_config);
    device_manager_shutdown(&g_device_mgr);
    log_shutdown();
    
    LOG_INFO("Shutdown complete");
    return 0;
}
