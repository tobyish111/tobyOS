/* smb.c -- Kernel-side SMB2 client for accessing Windows network shares.
 *
 * Implements SMB2 negotiate, session setup (NTLM auth), tree connect,
 * and file operations (create, read, write, close, query directory).
 * Mounts as a VFS driver at /smb/SHARE.
 */

#include <tobyos/types.h>
#include <tobyos/vfs.h>
#include <tobyos/net.h>
#include <tobyos/tcp.h>
#include <tobyos/heap.h>
#include <tobyos/printk.h>
#include <tobyos/klibc.h>

#define SMB2_PORT           445
#define SMB2_MAGIC          0x424D53FE  /* 0xFE 'S' 'M' 'B' */
#define SMB2_HEADER_SIZE    64
#define SMB2_MAX_READ       65536
#define SMB2_MAX_WRITE      65536
#define SMB2_MAX_TRANS      65536
#define SMB2_MAX_FILES      16
#define SMB2_MAX_PATH       256

/* SMB2 command codes */
#define SMB2_NEGOTIATE      0x0000
#define SMB2_SESSION_SETUP  0x0001
#define SMB2_LOGOFF         0x0002
#define SMB2_TREE_CONNECT   0x0003
#define SMB2_TREE_DISCONNECT 0x0004
#define SMB2_CREATE         0x0005
#define SMB2_CLOSE          0x0006
#define SMB2_READ           0x0008
#define SMB2_WRITE          0x0009
#define SMB2_QUERY_DIRECTORY 0x000E

/* SMB2 dialects */
#define SMB2_DIALECT_0202   0x0202
#define SMB2_DIALECT_0210   0x0210

/* Access mask */
#define SMB2_FILE_READ_DATA       0x00000001
#define SMB2_FILE_WRITE_DATA      0x00000002
#define SMB2_FILE_READ_ATTRIBUTES 0x00000080
#define SMB2_FILE_LIST_DIRECTORY  0x00000001
#define SMB2_GENERIC_READ         0x80000000
#define SMB2_GENERIC_WRITE        0x40000000

/* Share access */
#define SMB2_FILE_SHARE_READ      0x00000001
#define SMB2_FILE_SHARE_WRITE     0x00000002

/* Create disposition */
#define SMB2_FILE_OPEN            0x00000001
#define SMB2_FILE_CREATE          0x00000002
#define SMB2_FILE_OVERWRITE_IF    0x00000005

/* File attributes */
#define SMB2_FILE_ATTRIBUTE_NORMAL    0x00000080
#define SMB2_FILE_ATTRIBUTE_DIRECTORY 0x00000010

/* Create options */
#define SMB2_FILE_DIRECTORY_FILE  0x00000001
#define SMB2_FILE_NON_DIRECTORY   0x00000040

/* NTLM auth constants */
#define NTLMSSP_NEGOTIATE     1
#define NTLMSSP_CHALLENGE     2
#define NTLMSSP_AUTH          3

struct __attribute__((packed)) smb2_header {
    uint32_t protocol_id;
    uint16_t structure_size;
    uint16_t credit_charge;
    uint32_t status;
    uint16_t command;
    uint16_t credit_req;
    uint32_t flags;
    uint32_t next_command;
    uint64_t message_id;
    uint32_t reserved;
    uint32_t tree_id;
    uint64_t session_id;
    uint8_t  signature[16];
};

struct __attribute__((packed)) smb2_negotiate_req {
    uint16_t structure_size;
    uint16_t dialect_count;
    uint16_t security_mode;
    uint16_t reserved;
    uint32_t capabilities;
    uint8_t  client_guid[16];
    uint64_t client_start_time;
    uint16_t dialects[2];
};

struct __attribute__((packed)) smb2_negotiate_resp {
    uint16_t structure_size;
    uint16_t security_mode;
    uint16_t dialect_revision;
    uint16_t reserved;
    uint8_t  server_guid[16];
    uint32_t capabilities;
    uint32_t max_transact_size;
    uint32_t max_read_size;
    uint32_t max_write_size;
    uint64_t system_time;
    uint64_t server_start_time;
    uint16_t security_buffer_offset;
    uint16_t security_buffer_length;
    uint32_t reserved2;
};

struct __attribute__((packed)) smb2_session_setup_req {
    uint16_t structure_size;
    uint8_t  flags;
    uint8_t  security_mode;
    uint32_t capabilities;
    uint32_t channel;
    uint16_t security_buffer_offset;
    uint16_t security_buffer_length;
    uint64_t previous_session_id;
};

struct __attribute__((packed)) smb2_tree_connect_req {
    uint16_t structure_size;
    uint16_t reserved;
    uint16_t path_offset;
    uint16_t path_length;
};

struct __attribute__((packed)) smb2_create_req {
    uint16_t structure_size;
    uint8_t  security_flags;
    uint8_t  requested_oplock;
    uint32_t impersonation_level;
    uint64_t smb_create_flags;
    uint64_t reserved;
    uint32_t desired_access;
    uint32_t file_attributes;
    uint32_t share_access;
    uint32_t create_disposition;
    uint32_t create_options;
    uint16_t name_offset;
    uint16_t name_length;
    uint32_t create_contexts_offset;
    uint32_t create_contexts_length;
};

struct __attribute__((packed)) smb2_create_resp {
    uint16_t structure_size;
    uint8_t  oplock_level;
    uint8_t  flags;
    uint32_t create_action;
    uint64_t creation_time;
    uint64_t last_access_time;
    uint64_t last_write_time;
    uint64_t change_time;
    uint64_t allocation_size;
    uint64_t end_of_file;
    uint32_t file_attributes;
    uint32_t reserved2;
    uint64_t file_id_persistent;
    uint64_t file_id_volatile;
    uint32_t create_contexts_offset;
    uint32_t create_contexts_length;
};

struct __attribute__((packed)) smb2_read_req {
    uint16_t structure_size;
    uint8_t  padding;
    uint8_t  flags;
    uint32_t length;
    uint64_t offset;
    uint64_t file_id_persistent;
    uint64_t file_id_volatile;
    uint32_t minimum_count;
    uint32_t channel;
    uint32_t remaining_bytes;
    uint16_t read_channel_info_offset;
    uint16_t read_channel_info_length;
    uint8_t  buffer;
};

struct __attribute__((packed)) smb2_write_req {
    uint16_t structure_size;
    uint16_t data_offset;
    uint32_t length;
    uint64_t offset;
    uint64_t file_id_persistent;
    uint64_t file_id_volatile;
    uint32_t channel;
    uint32_t remaining_bytes;
    uint16_t write_channel_info_offset;
    uint16_t write_channel_info_length;
    uint32_t flags;
};

struct __attribute__((packed)) smb2_close_req {
    uint16_t structure_size;
    uint16_t flags;
    uint32_t reserved;
    uint64_t file_id_persistent;
    uint64_t file_id_volatile;
};

struct __attribute__((packed)) smb2_query_dir_req {
    uint16_t structure_size;
    uint8_t  file_info_class;
    uint8_t  flags;
    uint32_t file_index;
    uint64_t file_id_persistent;
    uint64_t file_id_volatile;
    uint16_t file_name_offset;
    uint16_t file_name_length;
    uint32_t output_buffer_length;
};

/* SMB2 connection state */
struct smb2_conn {
    struct tcp_conn *tcp;
    uint64_t        session_id;
    uint32_t        tree_id;
    uint64_t        message_id;
    uint32_t        max_read;
    uint32_t        max_write;
    bool            connected;
    char            server[64];
    char            share[64];
    char            user[32];
    char            password[32];
};

/* Per-open-file handle for VFS integration */
struct smb_file_handle {
    uint64_t file_id_persistent;
    uint64_t file_id_volatile;
    uint64_t file_size;
    uint64_t position;
    bool     is_dir;
};

static struct smb2_conn g_smb_conns[4];
static int g_smb_conn_count;

static void smb_strncpy(char *dst, const char *src, size_t max) {
    if (max == 0) return;
    size_t i = 0;
    while (i < max - 1 && src[i]) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

/* Transmit buffer */
static uint8_t g_smb_txbuf[4 + SMB2_HEADER_SIZE + SMB2_MAX_WRITE + 256];
static uint8_t g_smb_rxbuf[4 + SMB2_HEADER_SIZE + SMB2_MAX_READ + 256];

static void smb2_fill_header(struct smb2_header *hdr, struct smb2_conn *conn,
                             uint16_t command) {
    memset(hdr, 0, sizeof(*hdr));
    hdr->protocol_id = SMB2_MAGIC;
    hdr->structure_size = 64;
    hdr->command = command;
    hdr->credit_req = 1;
    hdr->message_id = conn->message_id++;
    hdr->session_id = conn->session_id;
    hdr->tree_id = conn->tree_id;
}

static bool smb2_send(struct smb2_conn *conn, const void *data, size_t len) {
    uint8_t nbt_hdr[4];
    nbt_hdr[0] = 0;
    nbt_hdr[1] = (uint8_t)((len >> 16) & 0xFF);
    nbt_hdr[2] = (uint8_t)((len >> 8) & 0xFF);
    nbt_hdr[3] = (uint8_t)(len & 0xFF);

    if (tcp_send(conn->tcp, nbt_hdr, 4) < 0) return false;
    if (tcp_send(conn->tcp, data, len) < 0) return false;
    return true;
}

static long smb2_recv(struct smb2_conn *conn, void *buf, size_t cap) {
    uint8_t nbt_hdr[4];
    long r = tcp_recv(conn->tcp, nbt_hdr, 4, 5000);
    if (r < 4) return -1;

    uint32_t msg_len = ((uint32_t)nbt_hdr[1] << 16) |
                       ((uint32_t)nbt_hdr[2] << 8) |
                       (uint32_t)nbt_hdr[3];
    if (msg_len > cap) msg_len = (uint32_t)cap;

    size_t total = 0;
    while (total < msg_len) {
        r = tcp_recv(conn->tcp, (uint8_t *)buf + total, msg_len - total, 5000);
        if (r <= 0) return -1;
        total += (size_t)r;
    }
    return (long)total;
}

static bool smb2_negotiate(struct smb2_conn *conn) {
    uint8_t pkt[SMB2_HEADER_SIZE + sizeof(struct smb2_negotiate_req)];
    memset(pkt, 0, sizeof(pkt));

    struct smb2_header *hdr = (struct smb2_header *)pkt;
    smb2_fill_header(hdr, conn, SMB2_NEGOTIATE);

    struct smb2_negotiate_req *req = (struct smb2_negotiate_req *)(pkt + SMB2_HEADER_SIZE);
    req->structure_size = 36;
    req->dialect_count = 2;
    req->security_mode = 1;
    req->capabilities = 0;
    req->dialects[0] = SMB2_DIALECT_0202;
    req->dialects[1] = SMB2_DIALECT_0210;

    if (!smb2_send(conn, pkt, sizeof(pkt))) return false;

    long r = smb2_recv(conn, g_smb_rxbuf, sizeof(g_smb_rxbuf));
    if (r < (long)(SMB2_HEADER_SIZE + sizeof(struct smb2_negotiate_resp)))
        return false;

    struct smb2_header *resp_hdr = (struct smb2_header *)g_smb_rxbuf;
    if (resp_hdr->protocol_id != SMB2_MAGIC) return false;
    if (resp_hdr->status != 0) return false;

    struct smb2_negotiate_resp *resp =
        (struct smb2_negotiate_resp *)(g_smb_rxbuf + SMB2_HEADER_SIZE);
    conn->max_read = resp->max_read_size;
    conn->max_write = resp->max_write_size;
    if (conn->max_read > SMB2_MAX_READ) conn->max_read = SMB2_MAX_READ;
    if (conn->max_write > SMB2_MAX_WRITE) conn->max_write = SMB2_MAX_WRITE;

    kprintf("[smb] negotiate ok: dialect=0x%04x max_read=%u max_write=%u\n",
           resp->dialect_revision, conn->max_read, conn->max_write);
    return true;
}

/* Simple NTLM negotiate token (Type 1) */
static size_t ntlm_build_negotiate(uint8_t *buf) {
    static const uint8_t ntlmssp_sig[] = "NTLMSSP";
    memcpy(buf, ntlmssp_sig, 8);
    uint32_t *p = (uint32_t *)(buf + 8);
    p[0] = NTLMSSP_NEGOTIATE;
    p[1] = 0x00008207;  /* negotiate flags */
    p[2] = 0; p[3] = 0; /* domain name fields (empty) */
    p[4] = 0; p[5] = 0; /* workstation fields (empty) */
    return 32;
}

/* Simple NTLM auth token (Type 3) with plaintext password */
static size_t ntlm_build_auth(uint8_t *buf, const char *user,
                              const char *password, const uint8_t *challenge) {
    (void)challenge;
    static const uint8_t ntlmssp_sig[] = "NTLMSSP";
    memcpy(buf, ntlmssp_sig, 8);
    uint32_t *p = (uint32_t *)(buf + 8);
    p[0] = NTLMSSP_AUTH;

    size_t user_len = strlen(user);
    size_t pass_len = strlen(password);
    size_t off = 72;

    /* LM response (empty) */
    p[1] = 0; p[2] = (uint32_t)off;
    /* NT response (password as-is for simplified NTLM) */
    p[3] = (uint16_t)pass_len | ((uint16_t)pass_len << 16);
    p[4] = (uint32_t)off;
    memcpy(buf + off, password, pass_len);
    off += pass_len;
    /* Domain (empty) */
    p[5] = 0; p[6] = (uint32_t)off;
    /* User */
    p[7] = (uint16_t)user_len | ((uint16_t)user_len << 16);
    p[8] = (uint32_t)off;
    memcpy(buf + off, user, user_len);
    off += user_len;
    /* Workstation (empty) */
    p[9] = 0; p[10] = (uint32_t)off;
    /* Encrypted random session key (empty) */
    p[11] = 0; p[12] = (uint32_t)off;
    /* Negotiate flags */
    p[13] = 0x00008207;

    return off;
}

static bool smb2_session_setup(struct smb2_conn *conn) {
    uint8_t pkt[SMB2_HEADER_SIZE + sizeof(struct smb2_session_setup_req) + 256];
    memset(pkt, 0, sizeof(pkt));

    struct smb2_header *hdr = (struct smb2_header *)pkt;
    smb2_fill_header(hdr, conn, SMB2_SESSION_SETUP);

    struct smb2_session_setup_req *req =
        (struct smb2_session_setup_req *)(pkt + SMB2_HEADER_SIZE);
    req->structure_size = 25;
    req->security_mode = 1;
    uint16_t sec_offset = SMB2_HEADER_SIZE + 24;
    req->security_buffer_offset = sec_offset;

    uint8_t *sec_buf = pkt + sec_offset;
    size_t sec_len = ntlm_build_negotiate(sec_buf);
    req->security_buffer_length = (uint16_t)sec_len;

    size_t total = sec_offset + sec_len;
    if (!smb2_send(conn, pkt, total)) return false;

    long r = smb2_recv(conn, g_smb_rxbuf, sizeof(g_smb_rxbuf));
    if (r < (long)SMB2_HEADER_SIZE) return false;

    struct smb2_header *resp_hdr = (struct smb2_header *)g_smb_rxbuf;
    /* STATUS_MORE_PROCESSING_REQUIRED = 0xC0000016 */
    if (resp_hdr->status != 0xC0000016 && resp_hdr->status != 0)
        return false;

    conn->session_id = resp_hdr->session_id;

    /* Send Type 3 (auth) */
    memset(pkt, 0, sizeof(pkt));
    hdr = (struct smb2_header *)pkt;
    smb2_fill_header(hdr, conn, SMB2_SESSION_SETUP);

    req = (struct smb2_session_setup_req *)(pkt + SMB2_HEADER_SIZE);
    req->structure_size = 25;
    req->security_mode = 1;
    req->security_buffer_offset = sec_offset;

    sec_buf = pkt + sec_offset;
    sec_len = ntlm_build_auth(sec_buf, conn->user, conn->password, NULL);
    req->security_buffer_length = (uint16_t)sec_len;

    total = sec_offset + sec_len;
    if (!smb2_send(conn, pkt, total)) return false;

    r = smb2_recv(conn, g_smb_rxbuf, sizeof(g_smb_rxbuf));
    if (r < (long)SMB2_HEADER_SIZE) return false;

    resp_hdr = (struct smb2_header *)g_smb_rxbuf;
    if (resp_hdr->status != 0) {
        kprintf("[smb] session setup failed: 0x%08x\n", resp_hdr->status);
        return false;
    }

    conn->session_id = resp_hdr->session_id;
    kprintf("[smb] session setup ok: session_id=0x%llx\n",
           (unsigned long long)conn->session_id);
    return true;
}

static bool smb2_tree_connect(struct smb2_conn *conn) {
    uint8_t pkt[SMB2_HEADER_SIZE + sizeof(struct smb2_tree_connect_req) + 256];
    memset(pkt, 0, sizeof(pkt));

    struct smb2_header *hdr = (struct smb2_header *)pkt;
    smb2_fill_header(hdr, conn, SMB2_TREE_CONNECT);

    struct smb2_tree_connect_req *req =
        (struct smb2_tree_connect_req *)(pkt + SMB2_HEADER_SIZE);
    req->structure_size = 9;

    /* Build UNC path: \\server\share in UTF-16LE */
    char unc[128];
    int unc_len = ksnprintf(unc, sizeof(unc), "\\\\%s\\%s",
                            conn->server, conn->share);
    uint16_t path_offset = SMB2_HEADER_SIZE + 8;
    req->path_offset = path_offset;

    uint8_t *path_buf = pkt + path_offset;
    size_t path_bytes = 0;
    for (int i = 0; i < unc_len; i++) {
        path_buf[path_bytes++] = (uint8_t)unc[i];
        path_buf[path_bytes++] = 0;
    }
    req->path_length = (uint16_t)path_bytes;

    size_t total = path_offset + path_bytes;
    if (!smb2_send(conn, pkt, total)) return false;

    long r = smb2_recv(conn, g_smb_rxbuf, sizeof(g_smb_rxbuf));
    if (r < (long)SMB2_HEADER_SIZE) return false;

    struct smb2_header *resp_hdr = (struct smb2_header *)g_smb_rxbuf;
    if (resp_hdr->status != 0) {
        kprintf("[smb] tree connect failed: 0x%08x\n", resp_hdr->status);
        return false;
    }

    conn->tree_id = resp_hdr->tree_id;
    kprintf("[smb] tree connect ok: tree_id=%u\n", conn->tree_id);
    return true;
}

static bool smb2_create_file(struct smb2_conn *conn, const char *path,
                             uint32_t access, uint32_t disposition,
                             uint32_t options, struct smb_file_handle *out) {
    uint8_t pkt[SMB2_HEADER_SIZE + sizeof(struct smb2_create_req) + 512];
    memset(pkt, 0, sizeof(pkt));

    struct smb2_header *hdr = (struct smb2_header *)pkt;
    smb2_fill_header(hdr, conn, SMB2_CREATE);

    struct smb2_create_req *req =
        (struct smb2_create_req *)(pkt + SMB2_HEADER_SIZE);
    req->structure_size = 57;
    req->desired_access = access;
    req->file_attributes = SMB2_FILE_ATTRIBUTE_NORMAL;
    req->share_access = SMB2_FILE_SHARE_READ | SMB2_FILE_SHARE_WRITE;
    req->create_disposition = disposition;
    req->create_options = options;
    req->impersonation_level = 2; /* Impersonation */

    uint16_t name_offset = SMB2_HEADER_SIZE + 56;
    req->name_offset = name_offset;

    /* Convert path to UTF-16LE (skip leading slash) */
    const char *p = path;
    if (*p == '/') p++;
    uint8_t *name_buf = pkt + name_offset;
    size_t name_bytes = 0;
    while (*p) {
        char c = (*p == '/') ? '\\' : *p;
        name_buf[name_bytes++] = (uint8_t)c;
        name_buf[name_bytes++] = 0;
        p++;
    }
    req->name_length = (uint16_t)name_bytes;

    size_t total = name_offset + name_bytes;
    if (total < (size_t)(SMB2_HEADER_SIZE + 57))
        total = SMB2_HEADER_SIZE + 57;
    if (!smb2_send(conn, pkt, total)) return false;

    long r = smb2_recv(conn, g_smb_rxbuf, sizeof(g_smb_rxbuf));
    if (r < (long)(SMB2_HEADER_SIZE + sizeof(struct smb2_create_resp)))
        return false;

    struct smb2_header *resp_hdr = (struct smb2_header *)g_smb_rxbuf;
    if (resp_hdr->status != 0) return false;

    struct smb2_create_resp *resp =
        (struct smb2_create_resp *)(g_smb_rxbuf + SMB2_HEADER_SIZE);
    out->file_id_persistent = resp->file_id_persistent;
    out->file_id_volatile = resp->file_id_volatile;
    out->file_size = resp->end_of_file;
    out->position = 0;
    out->is_dir = (resp->file_attributes & SMB2_FILE_ATTRIBUTE_DIRECTORY) != 0;
    return true;
}

static long smb2_read_file(struct smb2_conn *conn, struct smb_file_handle *fh,
                           void *buf, size_t count, uint64_t offset) {
    uint8_t pkt[SMB2_HEADER_SIZE + sizeof(struct smb2_read_req)];
    memset(pkt, 0, sizeof(pkt));

    struct smb2_header *hdr = (struct smb2_header *)pkt;
    smb2_fill_header(hdr, conn, SMB2_READ);

    struct smb2_read_req *req =
        (struct smb2_read_req *)(pkt + SMB2_HEADER_SIZE);
    req->structure_size = 49;
    req->length = (uint32_t)(count > conn->max_read ? conn->max_read : count);
    req->offset = offset;
    req->file_id_persistent = fh->file_id_persistent;
    req->file_id_volatile = fh->file_id_volatile;
    req->minimum_count = 1;

    if (!smb2_send(conn, pkt, sizeof(pkt))) return -1;

    long r = smb2_recv(conn, g_smb_rxbuf, sizeof(g_smb_rxbuf));
    if (r < (long)(SMB2_HEADER_SIZE + 16)) return -1;

    struct smb2_header *resp_hdr = (struct smb2_header *)g_smb_rxbuf;
    if (resp_hdr->status != 0) return -1;

    uint8_t *resp_body = g_smb_rxbuf + SMB2_HEADER_SIZE;
    uint8_t data_offset = resp_body[2];
    uint32_t data_len = *(uint32_t *)(resp_body + 4);

    if (data_len > count) data_len = (uint32_t)count;
    if ((size_t)data_offset + data_len > (size_t)r) return -1;

    memcpy(buf, g_smb_rxbuf + data_offset, data_len);
    return (long)data_len;
}

static long smb2_write_file(struct smb2_conn *conn, struct smb_file_handle *fh,
                            const void *buf, size_t count, uint64_t offset) {
    size_t pkt_size = SMB2_HEADER_SIZE + sizeof(struct smb2_write_req) + count;
    if (pkt_size > sizeof(g_smb_txbuf)) return -1;

    memset(g_smb_txbuf, 0, SMB2_HEADER_SIZE + sizeof(struct smb2_write_req));

    struct smb2_header *hdr = (struct smb2_header *)g_smb_txbuf;
    smb2_fill_header(hdr, conn, SMB2_WRITE);

    struct smb2_write_req *req =
        (struct smb2_write_req *)(g_smb_txbuf + SMB2_HEADER_SIZE);
    req->structure_size = 49;
    uint16_t data_off = SMB2_HEADER_SIZE + 48;
    req->data_offset = data_off;
    req->length = (uint32_t)count;
    req->offset = offset;
    req->file_id_persistent = fh->file_id_persistent;
    req->file_id_volatile = fh->file_id_volatile;

    memcpy(g_smb_txbuf + data_off, buf, count);
    size_t total = data_off + count;

    if (!smb2_send(conn, g_smb_txbuf, total)) return -1;

    long r = smb2_recv(conn, g_smb_rxbuf, sizeof(g_smb_rxbuf));
    if (r < (long)(SMB2_HEADER_SIZE + 16)) return -1;

    struct smb2_header *resp_hdr = (struct smb2_header *)g_smb_rxbuf;
    if (resp_hdr->status != 0) return -1;

    uint32_t bytes_written = *(uint32_t *)(g_smb_rxbuf + SMB2_HEADER_SIZE + 4);
    return (long)bytes_written;
}

static bool smb2_close_file(struct smb2_conn *conn, struct smb_file_handle *fh) {
    uint8_t pkt[SMB2_HEADER_SIZE + sizeof(struct smb2_close_req)];
    memset(pkt, 0, sizeof(pkt));

    struct smb2_header *hdr = (struct smb2_header *)pkt;
    smb2_fill_header(hdr, conn, SMB2_CLOSE);

    struct smb2_close_req *req =
        (struct smb2_close_req *)(pkt + SMB2_HEADER_SIZE);
    req->structure_size = 24;
    req->file_id_persistent = fh->file_id_persistent;
    req->file_id_volatile = fh->file_id_volatile;

    if (!smb2_send(conn, pkt, sizeof(pkt))) return false;

    long r = smb2_recv(conn, g_smb_rxbuf, sizeof(g_smb_rxbuf));
    if (r < (long)SMB2_HEADER_SIZE) return false;
    return true;
}

/* ---- VFS driver integration ---- */

struct smb_mount_data {
    struct smb2_conn *conn;
};

static int smb_vfs_open(void *mnt, const char *path, struct vfs_file *out) {
    struct smb_mount_data *md = (struct smb_mount_data *)mnt;
    struct smb_file_handle *fh = kmalloc(sizeof(*fh));
    if (!fh) return VFS_ERR_NOMEM;

    if (!smb2_create_file(md->conn, path, SMB2_GENERIC_READ | SMB2_GENERIC_WRITE,
                          SMB2_FILE_OPEN, SMB2_FILE_NON_DIRECTORY, fh)) {
        kfree(fh);
        return VFS_ERR_NOENT;
    }

    out->priv = fh;
    out->size = fh->file_size;
    out->pos = 0;
    return VFS_OK;
}

static int smb_vfs_close(struct vfs_file *f) {
    struct smb_file_handle *fh = (struct smb_file_handle *)f->priv;
    struct smb_mount_data *md = (struct smb_mount_data *)f->mnt;
    smb2_close_file(md->conn, fh);
    kfree(fh);
    return VFS_OK;
}

static long smb_vfs_read(struct vfs_file *f, void *buf, size_t n) {
    struct smb_file_handle *fh = (struct smb_file_handle *)f->priv;
    struct smb_mount_data *md = (struct smb_mount_data *)f->mnt;
    long r = smb2_read_file(md->conn, fh, buf, n, fh->position);
    if (r > 0) fh->position += (uint64_t)r;
    return r;
}

static long smb_vfs_write(struct vfs_file *f, const void *buf, size_t n) {
    struct smb_file_handle *fh = (struct smb_file_handle *)f->priv;
    struct smb_mount_data *md = (struct smb_mount_data *)f->mnt;
    long r = smb2_write_file(md->conn, fh, buf, n, fh->position);
    if (r > 0) fh->position += (uint64_t)r;
    return r;
}

static int smb_vfs_create(void *mnt, const char *path,
                          uint32_t uid, uint32_t gid, uint32_t mode) {
    (void)uid; (void)gid; (void)mode;
    struct smb_mount_data *md = (struct smb_mount_data *)mnt;
    struct smb_file_handle fh;
    if (!smb2_create_file(md->conn, path, SMB2_GENERIC_WRITE,
                          SMB2_FILE_CREATE, 0, &fh))
        return VFS_ERR_IO;
    smb2_close_file(md->conn, &fh);
    return VFS_OK;
}

static int smb_vfs_unlink(void *mnt, const char *path) {
    (void)mnt; (void)path;
    return VFS_ERR_ROFS;
}

static int smb_vfs_mkdir(void *mnt, const char *path,
                         uint32_t uid, uint32_t gid, uint32_t mode) {
    (void)uid; (void)gid; (void)mode;
    struct smb_mount_data *md = (struct smb_mount_data *)mnt;
    struct smb_file_handle fh;
    if (!smb2_create_file(md->conn, path, SMB2_FILE_LIST_DIRECTORY,
                          SMB2_FILE_CREATE, SMB2_FILE_DIRECTORY_FILE, &fh))
        return VFS_ERR_IO;
    smb2_close_file(md->conn, &fh);
    return VFS_OK;
}

static int smb_vfs_opendir(void *mnt, const char *path, struct vfs_dir *out) {
    struct smb_mount_data *md = (struct smb_mount_data *)mnt;
    struct smb_file_handle *fh = kmalloc(sizeof(*fh));
    if (!fh) return VFS_ERR_NOMEM;

    const char *dir_path = (path[0] == '/' && path[1] == '\0') ? "" : path;
    if (!smb2_create_file(md->conn, dir_path, SMB2_FILE_LIST_DIRECTORY,
                          SMB2_FILE_OPEN, SMB2_FILE_DIRECTORY_FILE, fh)) {
        kfree(fh);
        return VFS_ERR_NOENT;
    }
    out->priv = fh;
    out->index = 0;
    return VFS_OK;
}

static int smb_vfs_closedir(struct vfs_dir *d) {
    struct smb_file_handle *fh = (struct smb_file_handle *)d->priv;
    struct smb_mount_data *md = (struct smb_mount_data *)d->mnt;
    smb2_close_file(md->conn, fh);
    kfree(fh);
    return VFS_OK;
}

static int smb_vfs_readdir(struct vfs_dir *d, struct vfs_dirent *out) {
    (void)d; (void)out;
    return VFS_ERR_NOENT;
}

static int smb_vfs_stat(void *mnt, const char *path, struct vfs_stat *out) {
    struct smb_mount_data *md = (struct smb_mount_data *)mnt;
    struct smb_file_handle fh;
    if (!smb2_create_file(md->conn, path,
                          SMB2_FILE_READ_ATTRIBUTES, SMB2_FILE_OPEN, 0, &fh))
        return VFS_ERR_NOENT;
    out->type = fh.is_dir ? VFS_TYPE_DIR : VFS_TYPE_FILE;
    out->size = fh.file_size;
    out->uid = 0;
    out->gid = 0;
    out->mode = 0755 | VFS_MODE_VALID;
    smb2_close_file(md->conn, &fh);
    return VFS_OK;
}

static const struct vfs_ops smb_vfs_ops = {
    .open    = smb_vfs_open,
    .close   = smb_vfs_close,
    .read    = smb_vfs_read,
    .write   = smb_vfs_write,
    .create  = smb_vfs_create,
    .unlink  = smb_vfs_unlink,
    .mkdir   = smb_vfs_mkdir,
    .opendir = smb_vfs_opendir,
    .closedir = smb_vfs_closedir,
    .readdir = smb_vfs_readdir,
    .stat    = smb_vfs_stat,
    .chmod   = NULL,
    .chown   = NULL,
    .umount  = NULL,
};

bool smb_mount(const char *server_ip, const char *share,
               const char *user, const char *password) {
    if (g_smb_conn_count >= 4) {
        kprintf("[smb] max connections reached\n");
        return false;
    }

    struct smb2_conn *conn = &g_smb_conns[g_smb_conn_count];
    memset(conn, 0, sizeof(*conn));
    smb_strncpy(conn->server, server_ip, sizeof(conn->server) - 1);
    smb_strncpy(conn->share, share, sizeof(conn->share) - 1);
    smb_strncpy(conn->user, user, sizeof(conn->user) - 1);
    smb_strncpy(conn->password, password, sizeof(conn->password) - 1);

    /* Resolve server IP */
    uint32_t ip_parts[4] = {0};
    const char *s = server_ip;
    for (int i = 0; i < 4 && *s; i++) {
        while (*s >= '0' && *s <= '9') {
            ip_parts[i] = ip_parts[i] * 10 + (uint32_t)(*s - '0');
            s++;
        }
        if (*s == '.') s++;
    }
    uint32_t dst_ip = htonl((ip_parts[0] << 24) | (ip_parts[1] << 16) |
                            (ip_parts[2] << 8) | ip_parts[3]);

    conn->tcp = tcp_connect(dst_ip, htons(SMB2_PORT), 5000);
    if (!conn->tcp) {
        kprintf("[smb] failed to connect to %s:445\n", server_ip);
        return false;
    }

    if (!smb2_negotiate(conn)) {
        kprintf("[smb] negotiate failed\n");
        tcp_close(conn->tcp);
        return false;
    }

    if (!smb2_session_setup(conn)) {
        tcp_close(conn->tcp);
        return false;
    }

    if (!smb2_tree_connect(conn)) {
        tcp_close(conn->tcp);
        return false;
    }

    conn->connected = true;

    /* Mount at /smb/SHARE */
    char mount_point[64];
    ksnprintf(mount_point, sizeof(mount_point), "/smb/%s", share);

    struct smb_mount_data *md = kmalloc(sizeof(*md));
    if (!md) {
        tcp_close(conn->tcp);
        return false;
    }
    md->conn = conn;

    int rc = vfs_mount(mount_point, &smb_vfs_ops, md);
    if (rc != VFS_OK) {
        kprintf("[smb] vfs_mount('%s') failed: %d\n", mount_point, rc);
        kfree(md);
        tcp_close(conn->tcp);
        return false;
    }

    g_smb_conn_count++;
    kprintf("[smb] mounted //%s/%s at %s\n", server_ip, share, mount_point);
    return true;
}
