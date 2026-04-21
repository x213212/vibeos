#include "user_internal.h"
#include "errno.h"
#include "libssh2.h"
#include "lwip/altcp.h"
#include "lwip/altcp_tcp.h"
#include "lwip/ip_addr.h"
#include "lwip/pbuf.h"

#define SSH_RX_BUF_SIZE (64 * 1024)
#define SSH_CONNECT_TIMEOUT_MS 15000U
#define SSH_AUTH_TIMEOUT_MS 15000U
#define SSH_EXEC_TIMEOUT_MS 20000U
#define SSH_DEFAULT_WRP_URL "http://192.168.123.100:9999"

static char ssh_user[32];
static char ssh_host[64];
static char ssh_target[96];
static uint16_t ssh_port = 22;
static char ssh_password[64];
static int ssh_password_set = 0;
static char ssh_wrp_url[160];

struct ssh_transport {
    struct altcp_pcb *pcb;
    ip_addr_t addr;
    uint16_t port;
    int connected;
    int closed;
    int err;
    uint8_t rx_buf[SSH_RX_BUF_SIZE];
    uint32_t rx_start;
    uint32_t rx_len;
};

static void ssh_transport_destroy(struct ssh_transport *t);
static void ssh_transport_pump(void);

static void ssh_set_msg(char *out, int out_max, const char *msg)
{
    if (!out || out_max <= 0) return;
    out[0] = '\0';
    if (!msg) return;
    lib_strcpy(out, msg);
}

static void ssh_log(const char *msg)
{
    if (msg) lib_printf("[SSH] %s\n", msg);
}

static void ssh_log_u32(const char *tag, uint32_t v)
{
    if (tag) lib_printf("[SSH] %s=%u\n", tag, (unsigned)v);
}

static void ssh_log_ptr(const char *tag, const void *p)
{
    if (tag) lib_printf("[SSH] %s=%lx\n", tag, (unsigned long)p);
}

static void ssh_log_build_stamp(void)
{
    const char *ver = libssh2_version(0);
    lib_printf("[SSH] build stamp %s %s %s libssh2=%s\n",
               __DATE__, __TIME__, __FILE__, ver ? ver : "-");
}

static void ssh_trace_handler(LIBSSH2_SESSION *session, void *context, const char *message, size_t length)
{
    char buf[256];
    size_t n;

    (void)session;
    (void)context;
    if (!message || length == 0) return;
    n = length;
    if (n >= sizeof(buf)) n = sizeof(buf) - 1;
    memcpy(buf, message, n);
    buf[n] = '\0';
    lib_printf("[SSH][TRACE] %s\n", buf);
}

static void ssh_log_session_error(LIBSSH2_SESSION *session, const char *stage)
{
    char *errmsg = 0;
    int errmsg_len = 0;
    int last_errno = -1;

    if (!stage) stage = "session";
    if (session) {
        last_errno = libssh2_session_last_error(session, &errmsg, &errmsg_len, 0);
    }
    lib_printf("[SSH] %s last_errno=%d errmsg=%s len=%d\n",
               stage,
               last_errno,
               errmsg ? errmsg : "-",
               errmsg_len);
}

static int ssh_configure_session_methods(LIBSSH2_SESSION *session)
{
    int rc;
    if (!session) return -1;
    rc = libssh2_session_method_pref(session,
                                     LIBSSH2_METHOD_KEX,
                                     "ecdh-sha2-nistp256,diffie-hellman-group14-sha1");
    if (rc != 0) {
        lib_printf("[SSH] method_pref kex rc=%d\n", rc);
        return rc;
    }
    rc = libssh2_session_method_pref(session,
                                     LIBSSH2_METHOD_HOSTKEY,
                                     "ecdsa-sha2-nistp256,rsa-sha2-512,rsa-sha2-256,ssh-rsa");
    if (rc != 0) {
        lib_printf("[SSH] method_pref hostkey rc=%d\n", rc);
        return rc;
    }
    rc = libssh2_session_method_pref(session,
                                     LIBSSH2_METHOD_SIGN_ALGO,
                                     "ecdsa-sha2-nistp256,rsa-sha2-512,rsa-sha2-256,ssh-rsa");
    if (rc != 0) {
        lib_printf("[SSH] method_pref sign_algo rc=%d\n", rc);
        return rc;
    }
    rc = libssh2_session_method_pref(session, LIBSSH2_METHOD_CRYPT_CS, "aes128-ctr,aes192-ctr,aes256-ctr");
    if (rc != 0) {
        lib_printf("[SSH] method_pref crypt_cs rc=%d\n", rc);
        return rc;
    }
    rc = libssh2_session_method_pref(session, LIBSSH2_METHOD_CRYPT_SC, "aes128-ctr,aes192-ctr,aes256-ctr");
    if (rc != 0) {
        lib_printf("[SSH] method_pref crypt_sc rc=%d\n", rc);
        return rc;
    }
    rc = libssh2_session_method_pref(session, LIBSSH2_METHOD_MAC_CS, "hmac-sha2-256,hmac-sha2-512,hmac-sha1");
    if (rc != 0) {
        lib_printf("[SSH] method_pref mac_cs rc=%d\n", rc);
        return rc;
    }
    rc = libssh2_session_method_pref(session, LIBSSH2_METHOD_MAC_SC, "hmac-sha2-256,hmac-sha2-512,hmac-sha1");
    if (rc != 0) {
        lib_printf("[SSH] method_pref mac_sc rc=%d\n", rc);
        return rc;
    }
    lib_printf("[SSH] method preferences configured\n");
    return 0;
}

static void ssh_rebuild_target(void)
{
    ssh_target[0] = '\0';
    if (ssh_user[0]) {
        lib_strcpy(ssh_target, ssh_user);
        lib_strcat(ssh_target, "@");
    }
    lib_strcat(ssh_target, ssh_host);
    if (ssh_port != 22) {
        char portbuf[16];
        lib_strcat(ssh_target, ":");
        lib_itoa((uint32_t)ssh_port, portbuf);
        lib_strcat(ssh_target, portbuf);
    }
}

static int ssh_parse_target(const char *spec, char *out, int out_max)
{
    const char *p = spec;
    const char *at;
    const char *colon;
    char user[32];
    char host[64];
    uint16_t port = 22;

    while (*p == ' ' || *p == '\t') p++;
    if (*p == '\0') {
        ssh_set_msg(out, out_max, "ERR: ssh set <user@host[:port]>.");
        return -1;
    }

    user[0] = '\0';
    host[0] = '\0';
    at = strchr(p, '@');
    if (at) {
        int n = (int)(at - p);
        if (n <= 0 || n >= (int)sizeof(user)) {
            ssh_set_msg(out, out_max, "ERR: bad ssh user.");
            return -1;
        }
        memcpy(user, p, (uint32_t)n);
        user[n] = '\0';
        p = at + 1;
    }
    else {
        lib_strcpy(user, "root");
    }

    colon = strchr(p, ':');
    if (colon) {
        int n = (int)(colon - p);
        if (n <= 0 || n >= (int)sizeof(host)) {
            ssh_set_msg(out, out_max, "ERR: bad ssh host.");
            return -1;
        }
        memcpy(host, p, (uint32_t)n);
        host[n] = '\0';
        port = (uint16_t)atoi(colon + 1);
        if (port == 0) port = 22;
    }
    else {
        if ((int)strlen(p) >= (int)sizeof(host)) {
            ssh_set_msg(out, out_max, "ERR: bad ssh host.");
            return -1;
        }
        lib_strcpy(host, p);
    }

    if (host[0] == '\0') {
        ssh_set_msg(out, out_max, "ERR: bad ssh host.");
        return -1;
    }

    lib_strcpy(ssh_user, user);
    lib_strcpy(ssh_host, host);
    ssh_port = port;
    ssh_rebuild_target();

    if (ssh_wrp_url[0] == '\0') {
        lib_strcpy(ssh_wrp_url, "http://");
        lib_strcat(ssh_wrp_url, ssh_host);
        lib_strcat(ssh_wrp_url, ":8080");
    }

    ssh_set_msg(out, out_max, "OK: ssh target stored.");
    return 0;
}

static void ssh_transport_trim(struct ssh_transport *t)
{
    if (!t) return;
    if (t->rx_start == 0) return;
    if (t->rx_start >= t->rx_len) {
        t->rx_start = 0;
        t->rx_len = 0;
        return;
    }
    memmove(t->rx_buf, t->rx_buf + t->rx_start, t->rx_len - t->rx_start);
    t->rx_len -= t->rx_start;
    t->rx_start = 0;
}

static uint32_t ssh_transport_free_space(const struct ssh_transport *t)
{
    if (!t) return 0;
    return (uint32_t)sizeof(t->rx_buf) - t->rx_len;
}

static void ssh_transport_push(struct ssh_transport *t, const uint8_t *data, uint32_t len)
{
    if (!t || !data || len == 0) return;
    if (ssh_transport_free_space(t) < len) {
        ssh_transport_trim(t);
    }
    if (ssh_transport_free_space(t) < len) {
        t->err = 1;
        return;
    }
    memcpy(t->rx_buf + t->rx_len, data, len);
    t->rx_len += len;
}

static err_t ssh_altcp_connected_cb(void *arg, struct altcp_pcb *pcb, err_t err)
{
    struct ssh_transport *t = (struct ssh_transport *)arg;
    (void)pcb;
    if (!t) return ERR_ARG;
    lib_printf("[SSH] tcp connected err=%d transport=%lx\n", (int)err, (unsigned long)t);
    if (err == ERR_OK) {
        t->connected = 1;
    }
    else {
        t->err = err;
    }
    wake_network_task();
    return ERR_OK;
}

static err_t ssh_altcp_recv_cb(void *arg, struct altcp_pcb *pcb, struct pbuf *p, err_t err)
{
    struct ssh_transport *t = (struct ssh_transport *)arg;
    (void)pcb;
    if (!t) {
        if (p) pbuf_free(p);
        return ERR_ARG;
    }
    if (err != ERR_OK) {
        if (p) pbuf_free(p);
        t->err = err;
        lib_printf("[SSH] tcp recv err=%d transport=%lx\n", (int)err, (unsigned long)t);
        wake_network_task();
        return ERR_ABRT;
    }
    if (p == NULL) {
        t->closed = 1;
        lib_printf("[SSH] tcp recv eof transport=%lx\n", (unsigned long)t);
        wake_network_task();
        return ERR_OK;
    }

    for (struct pbuf *tp = p; tp != NULL; tp = tp->next) {
        if (tp->len > ssh_transport_free_space(t)) {
            ssh_transport_trim(t);
        }
        if (tp->len > ssh_transport_free_space(t)) {
            t->err = ERR_MEM;
            break;
        }
        ssh_transport_push(t, (const uint8_t *)tp->payload, (uint32_t)tp->len);
    }
    altcp_recved(pcb, (u16_t)p->tot_len);
    pbuf_free(p);
    lib_printf("[SSH] tcp recv len=%u transport=%lx\n", (unsigned)p->tot_len, (unsigned long)t);
    wake_network_task();
    return ERR_OK;
}

static err_t ssh_altcp_sent_cb(void *arg, struct altcp_pcb *pcb, u16_t len)
{
    (void)arg;
    (void)pcb;
    (void)len;
    wake_network_task();
    return ERR_OK;
}

static void ssh_altcp_err_cb(void *arg, err_t err)
{
    struct ssh_transport *t = (struct ssh_transport *)arg;
    if (!t) return;
    t->closed = 1;
    t->err = err;
    lib_printf("[SSH] tcp err=%d transport=%lx\n", (int)err, (unsigned long)t);
    wake_network_task();
}

static ssize_t ssh_libssh2_send(libssh2_socket_t socket, const void *buffer, size_t length, int flags, void **abstract)
{
    struct ssh_transport *t;
    u16_t chunk;
    err_t rc;

    (void)socket;
    (void)flags;
    t = (struct ssh_transport *)(*abstract);
    lib_printf("[SSH] send socket=%d len=%u abstract=%lx t=%lx conn=%d closed=%d err=%d\n",
               (int)socket, (unsigned)length, (unsigned long)(*abstract), (unsigned long)t,
               t ? t->connected : -1, t ? t->closed : -1, t ? t->err : -1);
    if (!t || !t->pcb) return -EAGAIN;
    if (!t->connected) return -EAGAIN;
    if (t->closed || t->err) return -EPIPE;

    chunk = (u16_t)length;
    if (chunk == 0) return 0;
    rc = altcp_write(t->pcb, buffer, chunk, 1);
    if (rc != ERR_OK) {
        if (rc == ERR_MEM || rc == ERR_TIMEOUT) return -EAGAIN;
        return -EIO;
    }
    rc = altcp_output(t->pcb);
    if (rc != ERR_OK) {
        if (rc == ERR_MEM || rc == ERR_TIMEOUT) return -EAGAIN;
        return -EIO;
    }
    wake_network_task();
    return (ssize_t)chunk;
}

static ssize_t ssh_libssh2_recv(libssh2_socket_t socket, void *buffer, size_t length, int flags, void **abstract)
{
    struct ssh_transport *t;
    size_t available;
    size_t n;

    (void)socket;
    (void)flags;
    t = (struct ssh_transport *)(*abstract);
    lib_printf("[SSH] recv socket=%d len=%u abstract=%lx t=%lx conn=%d closed=%d err=%d rx=%u/%u\n",
               (int)socket, (unsigned)length, (unsigned long)(*abstract), (unsigned long)t,
               t ? t->connected : -1, t ? t->closed : -1, t ? t->err : -1,
               t ? (unsigned)t->rx_start : 0, t ? (unsigned)t->rx_len : 0);
    if (!t) return -EAGAIN;

    if (t->rx_start >= t->rx_len) {
        t->rx_start = 0;
        t->rx_len = 0;
        if (t->closed) return 0;
        if (t->err) return -EPIPE;
        return -EAGAIN;
    }

    available = (size_t)(t->rx_len - t->rx_start);
    n = (available < length) ? available : length;
    memcpy(buffer, t->rx_buf + t->rx_start, (uint32_t)n);
    t->rx_start += (uint32_t)n;
    if (t->rx_start >= t->rx_len) {
        t->rx_start = 0;
        t->rx_len = 0;
    }
    return (ssize_t)n;
}

static void ssh_transport_pump(void)
{
    wake_network_task();
    task_os();
}

static int ssh_transport_wait_ready(struct ssh_transport *t, uint32_t timeout_ms, char *out, int out_max, const char *what)
{
    uint32_t start = sys_now();
    while (!t->connected && !t->closed && !t->err) {
        if (timeout_ms && (uint32_t)(sys_now() - start) > timeout_ms) {
            if (out) {
                lib_strcpy(out, "ERR: ");
                lib_strcat(out, what);
                lib_strcat(out, " timeout.");
            }
            return -1;
        }
        ssh_transport_pump();
    }
    if (!t->connected || t->err || t->closed) {
        if (out && out[0] == '\0') {
            lib_strcpy(out, "ERR: ");
            lib_strcat(out, what);
            lib_strcat(out, " failed.");
        }
        return -1;
    }
    return 0;
}

void ssh_client_reset(void)
{
    ssh_user[0] = '\0';
    ssh_host[0] = '\0';
    ssh_target[0] = '\0';
    ssh_port = 22;
    ssh_password[0] = '\0';
    ssh_password_set = 0;
    ssh_wrp_url[0] = '\0';
}

int ssh_client_set_target(const char *spec, char *out, int out_max)
{
    if (!spec || !*spec) {
        ssh_set_msg(out, out_max, "ERR: ssh set <user@host[:port]>.");
        return -1;
    }
    return ssh_parse_target(spec, out, out_max);
}

int ssh_client_set_password(const char *password, char *out, int out_max)
{
    if (!password || !*password) {
        ssh_set_msg(out, out_max, "ERR: ssh auth <password>.");
        return -1;
    }
    if ((int)strlen(password) >= (int)sizeof(ssh_password)) {
        ssh_set_msg(out, out_max, "ERR: password too long.");
        return -1;
    }
    lib_strcpy(ssh_password, password);
    ssh_password_set = 1;
    ssh_set_msg(out, out_max, "OK: ssh password stored.");
    return 0;
}

int ssh_client_set_wrp_url(const char *url, char *out, int out_max)
{
    if (!url || !*url) {
        ssh_set_msg(out, out_max, "ERR: wrp set <http://host:8080>.");
        return -1;
    }
    ssh_wrp_url[0] = '\0';
    lib_strcpy(ssh_wrp_url, url);
    ssh_set_msg(out, out_max, "OK: wrp url stored.");
    return 0;
}

int ssh_client_get_target(char *out, int out_max)
{
    (void)out_max;
    if (!out) return -1;
    if (ssh_target[0] == '\0') {
        out[0] = '\0';
        return -1;
    }
    lib_strcpy(out, ssh_target);
    return 0;
}

int ssh_client_has_password(void)
{
    return ssh_password_set;
}

int ssh_client_get_wrp_url(char *out, int out_max)
{
    (void)out_max;
    if (!out) return -1;
    if (ssh_wrp_url[0] == '\0') {
        lib_strcpy(out, SSH_DEFAULT_WRP_URL);
        return 0;
    }
    lib_strcpy(out, ssh_wrp_url);
    return 0;
}

static int ssh_connect_transport(struct ssh_transport **out_t, const char *host, uint16_t port, char *out, int out_max)
{
    struct ssh_transport *t;
    ip_addr_t tmp_addr;
    err_t rc;
    uint32_t start;

    t = (struct ssh_transport *)malloc(sizeof(struct ssh_transport));
    if (!t) {
        ssh_set_msg(out, out_max, "ERR: No Memory.");
        return -1;
    }
    memset(t, 0, sizeof(*t));
    t->port = port;
    if (!ipaddr_aton(host, &tmp_addr)) {
        free(t);
        ssh_set_msg(out, out_max, "ERR: ssh host must be numeric IPv4.");
        return -1;
    }
    t->addr = tmp_addr;
    lib_printf("[SSH] connect host=%s port=%u transport=%lx\n", host, (unsigned)port, (unsigned long)t);

    t->pcb = altcp_tcp_new_ip_type(IPADDR_TYPE_V4);
    if (!t->pcb) {
        free(t);
        ssh_set_msg(out, out_max, "ERR: No TCP PCB.");
        return -1;
    }

    altcp_arg(t->pcb, t);
    altcp_recv(t->pcb, ssh_altcp_recv_cb);
    altcp_sent(t->pcb, ssh_altcp_sent_cb);
    altcp_err(t->pcb, ssh_altcp_err_cb);

    rc = altcp_connect(t->pcb, &t->addr, t->port, ssh_altcp_connected_cb);
    lib_printf("[SSH] altcp_connect rc=%d transport=%lx\n", (int)rc, (unsigned long)t);
    if (rc != ERR_OK && rc != ERR_INPROGRESS) {
        ssh_transport_destroy(t);
        ssh_set_msg(out, out_max, "ERR: SSH connect failed.");
        return -1;
    }

    start = sys_now();
    while (!t->connected && !t->closed && !t->err) {
        if ((uint32_t)(sys_now() - start) > SSH_CONNECT_TIMEOUT_MS) {
            ssh_transport_destroy(t);
            ssh_set_msg(out, out_max, "ERR: SSH connect timeout.");
            return -1;
        }
        ssh_transport_pump();
    }

    if (t->err || t->closed || !t->connected) {
        ssh_transport_destroy(t);
        ssh_set_msg(out, out_max, "ERR: SSH connect failed.");
        return -1;
    }

    *out_t = t;
    return 0;
}

static void ssh_transport_destroy(struct ssh_transport *t)
{
    if (!t) return;
    if (t->pcb) {
        altcp_arg(t->pcb, NULL);
        altcp_recv(t->pcb, NULL);
        altcp_sent(t->pcb, NULL);
        altcp_err(t->pcb, NULL);
        altcp_abort(t->pcb);
        t->pcb = NULL;
    }
    free(t);
}

static int ssh_session_handshake_nonblock(LIBSSH2_SESSION *session, char *out, int out_max)
{
    int rc;
    uint32_t start = sys_now();
    lib_printf("[SSH] handshake begin session=%lx\n", (unsigned long)session);
    for (;;) {
        rc = libssh2_session_handshake(session, LIBSSH2_INVALID_SOCKET);
        lib_printf("[SSH] handshake rc=%d session=%lx\n", rc, (unsigned long)session);
        if (rc == LIBSSH2_ERROR_EAGAIN) {
            if ((uint32_t)(sys_now() - start) > SSH_CONNECT_TIMEOUT_MS) {
                ssh_set_msg(out, out_max, "ERR: SSH handshake timeout.");
                ssh_log_session_error(session, "handshake timeout");
                return -1;
            }
            ssh_transport_pump();
            continue;
        }
        if (rc != 0) {
            ssh_set_msg(out, out_max, "ERR: SSH handshake failed.");
            ssh_log_session_error(session, "handshake fail");
            return -1;
        }
        ssh_log_session_error(session, "handshake ok");
        return 0;
    }
}

static int ssh_userauth_password_nonblock(LIBSSH2_SESSION *session, const char *user, const char *pass, char *out, int out_max)
{
    int rc;
    uint32_t start = sys_now();
    lib_printf("[SSH] auth begin user=%s session=%lx\n", user ? user : "-", (unsigned long)session);
    for (;;) {
        rc = libssh2_userauth_password_ex(session, user, (unsigned int)strlen(user), pass, (unsigned int)strlen(pass), NULL);
        lib_printf("[SSH] auth rc=%d user=%s session=%lx\n", rc, user ? user : "-", (unsigned long)session);
        if (rc == LIBSSH2_ERROR_EAGAIN) {
            if ((uint32_t)(sys_now() - start) > SSH_AUTH_TIMEOUT_MS) {
                ssh_set_msg(out, out_max, "ERR: SSH auth timeout.");
                ssh_log_session_error(session, "auth timeout");
                return -1;
            }
            ssh_transport_pump();
            continue;
        }
        if (rc != 0) {
            ssh_set_msg(out, out_max, "ERR: SSH auth failed.");
            ssh_log_session_error(session, "auth fail");
            return -1;
        }
        ssh_log_session_error(session, "auth ok");
        return 0;
    }
}

static int ssh_channel_exec_nonblock(LIBSSH2_SESSION *session, const char *cmd, char *out, int out_max)
{
    LIBSSH2_CHANNEL *channel = NULL;
    int rc;
    uint32_t start = sys_now();
    lib_printf("[SSH] channel exec begin cmd='%s' session=%lx\n", cmd ? cmd : "-", (unsigned long)session);

    for (;;) {
        channel = libssh2_channel_open_session(session);
        lib_printf("[SSH] open_channel rc_channel=%lx session=%lx\n", (unsigned long)channel, (unsigned long)session);
        if (channel) break;
        if (libssh2_session_last_errno(session) != LIBSSH2_ERROR_EAGAIN) {
            ssh_set_msg(out, out_max, "ERR: SSH channel open failed.");
            ssh_log_session_error(session, "channel open fail");
            return -1;
        }
        if ((uint32_t)(sys_now() - start) > SSH_EXEC_TIMEOUT_MS) {
            ssh_set_msg(out, out_max, "ERR: SSH channel open timeout.");
            ssh_log_session_error(session, "channel open timeout");
            return -1;
        }
        ssh_transport_pump();
    }

    for (;;) {
        rc = libssh2_channel_exec(channel, cmd);
        lib_printf("[SSH] channel_exec rc=%d cmd='%s' channel=%lx\n", rc, cmd ? cmd : "-", (unsigned long)channel);
        if (rc == LIBSSH2_ERROR_EAGAIN) {
            if ((uint32_t)(sys_now() - start) > SSH_EXEC_TIMEOUT_MS) {
                libssh2_channel_free(channel);
                ssh_set_msg(out, out_max, "ERR: SSH exec timeout.");
                ssh_log_session_error(session, "channel exec timeout");
                return -1;
            }
            ssh_transport_pump();
            continue;
        }
        if (rc != 0) {
            libssh2_channel_free(channel);
            ssh_set_msg(out, out_max, "ERR: SSH exec failed.");
            ssh_log_session_error(session, "channel exec fail");
            return -1;
        }
        break;
    }

    out[0] = '\0';
    while (1) {
        char buf[128];
        ssize_t n = libssh2_channel_read_ex(channel, 0, buf, sizeof(buf) - 1);
        lib_printf("[SSH] channel_read n=%ld channel=%lx\n", (long)n, (unsigned long)channel);
        if (n == LIBSSH2_ERROR_EAGAIN) {
            ssh_transport_pump();
            continue;
        }
        if (n < 0) {
            libssh2_channel_free(channel);
            ssh_set_msg(out, out_max, "ERR: SSH read failed.");
            ssh_log_session_error(session, "channel read fail");
            return -1;
        }
        if (n == 0) {
            if (libssh2_channel_eof(channel)) break;
            ssh_transport_pump();
            continue;
        }
        if ((int)strlen(out) + (int)n >= out_max) {
            break;
        }
        buf[n] = '\0';
        lib_strcat(out, buf);
    }

    if (out[0] == '\0') {
        lib_strcpy(out, ">> SSH Exec Done.");
    }

    for (;;) {
        rc = libssh2_channel_close(channel);
        if (rc == LIBSSH2_ERROR_EAGAIN) {
            ssh_transport_pump();
            continue;
        }
        break;
    }
    libssh2_channel_free(channel);
    ssh_log_session_error(session, "channel exec done");
    return 0;
}

int ssh_client_exec_remote(const char *cmd, char *out, int out_max)
{
    LIBSSH2_SESSION *session = NULL;
    struct ssh_transport *transport = NULL;
    int rc;
    void **abstract_slot;

    if (!out || out_max <= 0) return -1;
    if (ssh_target[0] == '\0' || ssh_host[0] == '\0') {
        ssh_set_msg(out, out_max, "ERR: ssh target not set.");
        return -1;
    }
    if (!ssh_password_set) {
        ssh_set_msg(out, out_max, "ERR: ssh auth <password> first.");
        return -1;
    }
    if (!cmd || !*cmd) {
        ssh_set_msg(out, out_max, "ERR: ssh exec <command>.");
        return -1;
    }

    ssh_log_build_stamp();
    lib_printf("[SSH] exec begin target='%s' cmd='%s'\n", ssh_target, cmd);
    libssh2_init(0);

    rc = ssh_connect_transport(&transport, ssh_host, ssh_port, out, out_max);
    if (rc != 0) {
        libssh2_exit();
        return -1;
    }

    session = libssh2_session_init_ex(NULL, NULL, NULL, NULL);
    if (!session) {
        ssh_transport_destroy(transport);
        libssh2_exit();
        ssh_set_msg(out, out_max, "ERR: SSH session alloc failed.");
        return -1;
    }
    ssh_log_ptr("session", session);

    libssh2_session_set_blocking(session, 0);
    abstract_slot = libssh2_session_abstract(session);
    *abstract_slot = transport;
    ssh_log_ptr("abstract_slot", abstract_slot);
    ssh_log_ptr("transport", transport);
    libssh2_session_callback_set2(session, LIBSSH2_CALLBACK_SEND, (libssh2_cb_generic *)ssh_libssh2_send);
    libssh2_session_callback_set2(session, LIBSSH2_CALLBACK_RECV, (libssh2_cb_generic *)ssh_libssh2_recv);
    if (ssh_configure_session_methods(session) != 0) {
        ssh_set_msg(out, out_max, "ERR: SSH method pref failed.");
        libssh2_session_free(session);
        ssh_transport_destroy(transport);
        libssh2_exit();
        return -1;
    }

    rc = ssh_session_handshake_nonblock(session, out, out_max);
    if (rc != 0) {
        ssh_log("handshake failed");
        libssh2_session_free(session);
        ssh_transport_destroy(transport);
        libssh2_exit();
        return -1;
    }

    rc = ssh_userauth_password_nonblock(session, ssh_user[0] ? ssh_user : "root", ssh_password, out, out_max);
    if (rc != 0) {
        ssh_log("auth failed");
        libssh2_session_disconnect_ex(session, SSH_DISCONNECT_BY_APPLICATION, "auth failed", "");
        libssh2_session_free(session);
        ssh_transport_destroy(transport);
        libssh2_exit();
        return -1;
    }

    rc = ssh_channel_exec_nonblock(session, cmd, out, out_max);
    lib_printf("[SSH] exec rc=%d\n", rc);

    for (;;) {
        int fr = libssh2_session_disconnect_ex(session, SSH_DISCONNECT_BY_APPLICATION, "bye", "");
        lib_printf("[SSH] disconnect rc=%d\n", fr);
        if (fr == LIBSSH2_ERROR_EAGAIN) {
            ssh_transport_pump();
            continue;
        }
        break;
    }
    libssh2_session_free(session);
    ssh_transport_destroy(transport);
    libssh2_exit();
    return rc;
}
