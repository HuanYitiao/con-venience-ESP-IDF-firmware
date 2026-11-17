#include "ble_link.h"

#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

static const char *TAG = "ble_link";

// Wire format, unchanged from the old firmware.
#define CHUNK_SIZE 240
#define CHUNK_HDR 4
#define CHUNK_PKT (CHUNK_HDR + CHUNK_SIZE)
#define REQ_MAGIC 0xBB
#define HDR_MAGIC 0xAA
#define DEVICE_NAME "con-venience"

#define SYNC_TIMEOUT_MS 5000
// The old client used 5 s too.
#define CONNECT_TIMEOUT_MS 5000
#define OP_TIMEOUT_MS 5000
#define OP_RETRIES 5
#define RETRY_DELAY_MS 20
#define STOP_TIMEOUT_MS 3000
// LE data length: as the old client's setDataLen(251).
#define DATA_LEN_OCTETS 251
#define DATA_LEN_TIME_US 2120

#define CLIENT_TASK_STACK 4096
#define CLIENT_TASK_PRIO 5

// 128-bit UUIDs, bytes least significant first.
static const ble_uuid128_t svc_uuid = BLE_UUID128_INIT(
    0x4b, 0x91, 0x31, 0xc3, 0xc9, 0xc5, 0xcc, 0x8f, 0x9e, 0x45, 0xb5, 0x1f, 0x01, 0xc2, 0xaf, 0x4f);
static const ble_uuid128_t tx_uuid = BLE_UUID128_INIT(
    0xa8, 0x26, 0x1b, 0x36, 0x07, 0xea, 0xf5, 0xb7, 0x88, 0x46, 0xe1, 0x36, 0x3e, 0x48, 0xb5, 0xbe);
static const ble_uuid128_t rx_uuid = BLE_UUID128_INIT(
    0xa9, 0x26, 0x1b, 0x36, 0x07, 0xea, 0xf5, 0xb7, 0x88, 0x46, 0xe1, 0x36, 0x3e, 0x48, 0xb5, 0xbe);

static uint16_t tx_handle;
static uint16_t rx_handle;
static uint8_t  own_addr_type;

static SemaphoreHandle_t synced;
static SemaphoreHandle_t op_done;  // client: one GATT/GAP step finished
static portMUX_TYPE      finish_mux = portMUX_INITIALIZER_UNLOCKED;

// One transfer. Written by ble_link_start() before anything else runs, then
// by the host task (server) or the client task (client), never both.
static volatile bool      busy;
static volatile bool      finished;
static volatile bool      stop_requested;
static volatile bool      connected;
static volatile uint16_t  conn_handle = BLE_HS_CONN_HANDLE_NONE;
static ble_link_role_t    role;
static ble_link_done_cb_t done_cb;
static ble_addr_t         peer_addr;

static uint8_t  tx_buf[BLE_LINK_MAX_LEN];
static size_t   tx_len;
static uint8_t  rx_buf[BLE_LINK_MAX_LEN];
static size_t   rx_len;
static uint16_t rx_total;   // chunks expected from the peer, 0 = not known yet
static uint16_t rx_chunks;  // chunks received, in order

static volatile uint16_t req_chunk;  // server: chunk the client asked for

static volatile uint16_t        prog_done;
static volatile uint16_t        prog_total;
static volatile ble_link_prog_t prog_state;

// Client step results, written by NimBLE callbacks before op_done is given.
static volatile int op_status;
static uint16_t     svc_start;
static uint16_t     svc_end;
static uint16_t     peer_tx_handle;
static uint16_t     peer_rx_handle;
static uint8_t      read_buf[CHUNK_PKT];
static size_t       read_len;

static int gap_event(struct ble_gap_event *event, void *arg);

static uint16_t chunk_count(size_t len)
{
    return (uint16_t)((len + CHUNK_SIZE - 1) / CHUNK_SIZE);
}

// Builds TX chunk i of our data into pkt; 0 bytes once past the end.
static size_t build_chunk(uint16_t i, uint8_t *pkt)
{
    uint16_t total = chunk_count(tx_len);
    if (i >= total)
    {
        return 0;
    }
    size_t offset = (size_t)i * CHUNK_SIZE;
    size_t n = tx_len - offset < CHUNK_SIZE ? tx_len - offset : CHUNK_SIZE;
    put_be16(pkt, i);
    put_be16(pkt + 2, total);
    memcpy(pkt + CHUNK_HDR, tx_buf + offset, n);
    return CHUNK_HDR + n;
}

// Takes chunk idx of total from the peer. Only the next chunk in order is
// accepted, every chunk but the last must be full, and the whole transfer
// must fit rx_buf. The old code appended whatever arrived, unchecked.
static bool accept_chunk(uint16_t idx, uint16_t total, const uint8_t *data, size_t n)
{
    if (total == 0 || total > chunk_count(BLE_LINK_MAX_LEN) || (rx_total && total != rx_total))
    {
        return false;
    }
    if (idx != rx_chunks || n == 0 || n > CHUNK_SIZE || (idx + 1 < total && n != CHUNK_SIZE)
        || rx_len + n > BLE_LINK_MAX_LEN)
    {
        return false;
    }
    rx_total = total;
    memcpy(rx_buf + rx_len, data, n);
    rx_len += n;
    rx_chunks++;
    return true;
}

static bool rx_complete(void)
{
    return rx_total > 0 && rx_chunks == rx_total;
}

// Ends the transfer exactly once, from whichever side gets there first.
static void finish(bool success)
{
    taskENTER_CRITICAL(&finish_mux);
    bool first = busy && !finished;
    finished = true;
    taskEXIT_CRITICAL(&finish_mux);
    if (!first)
    {
        return;
    }

    ble_gap_adv_stop();
    connected = false;
    ESP_LOGI(TAG, "%s, received %u bytes", success ? "Done" : "Failed", (unsigned)rx_len);
    if (!stop_requested && done_cb)
    {
        done_cb(success, rx_buf, rx_len);
    }
    busy = false;
}

// ── Server ───────────────────────────────────────────────

// Phase 1 progress is approximated by the chunk the client asked for (that
// many are already read), phase 2 by the chunks received. Never overshoots.
static void server_update_progress(void)
{
    prog_done = req_chunk + rx_chunks;
}

static int server_write(const uint8_t *buf, uint16_t len)
{
    if (len == 3 && buf[0] == REQ_MAGIC)
    {
        req_chunk = get_be16(buf + 1);
        server_update_progress();
        return 0;
    }
    if (len == 3 && buf[0] == HDR_MAGIC)
    {
        uint16_t bytes = get_be16(buf + 1);
        if (bytes == 0 || bytes > BLE_LINK_MAX_LEN)
        {
            return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        }
        rx_total = chunk_count(bytes);
        prog_total = chunk_count(tx_len) + rx_total;
        prog_state = BLE_LINK_PROG_ACTIVE;
        server_update_progress();
        ESP_LOGI(TAG, "Server: peer sends %u bytes (%u chunks)", bytes, rx_total);
        return 0;
    }
    if (len < CHUNK_HDR)
    {
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }
    uint16_t idx = get_be16(buf);
    if (idx < rx_chunks)
    {
        return 0;  // repeated chunk, already have it
    }
    if (!accept_chunk(idx, get_be16(buf + 2), buf + CHUNK_HDR, len - CHUNK_HDR))
    {
        ESP_LOGW(TAG, "Server: chunk %u rejected", idx);
        return BLE_ATT_ERR_UNLIKELY;
    }
    server_update_progress();
    return 0;
}

static int chr_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (!busy || role != BLE_LINK_SERVER || conn != conn_handle)
    {
        return BLE_ATT_ERR_UNLIKELY;
    }

    if (attr == tx_handle && ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR)
    {
        // Called again for each part of a long read; the stack applies the
        // offset, so always return the whole chunk.
        uint8_t pkt[CHUNK_PKT];
        size_t  n = build_chunk(req_chunk, pkt);
        return os_mbuf_append(ctxt->om, pkt, n) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }

    if (attr == rx_handle && ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR)
    {
        uint8_t  buf[CHUNK_PKT];
        uint16_t len = 0;
        if (OS_MBUF_PKTLEN(ctxt->om) > sizeof(buf)
            || ble_hs_mbuf_to_flat(ctxt->om, buf, sizeof(buf), &len) != 0)
        {
            return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        }
        return server_write(buf, len);
    }

    return BLE_ATT_ERR_UNLIKELY;
}

static const struct ble_gatt_svc_def gatt_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &svc_uuid.u,
        .characteristics =
            (struct ble_gatt_chr_def[]){
                {
                    .uuid = &tx_uuid.u,
                    .access_cb = chr_access,
                    .flags = BLE_GATT_CHR_F_READ,
                    .val_handle = &tx_handle,
                },
                {
                    .uuid = &rx_uuid.u,
                    .access_cb = chr_access,
                    .flags = BLE_GATT_CHR_F_WRITE,
                    .val_handle = &rx_handle,
                },
                {0},
            },
    },
    {0},
};

static int start_advertising(void)
{
    struct ble_hs_adv_fields fields = {
        .flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP,
        .uuids128 = &svc_uuid,
        .num_uuids128 = 1,
        .uuids128_is_complete = 1,
    };
    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0)
    {
        return rc;
    }
    struct ble_hs_adv_fields rsp = {
        .name = (const uint8_t *)DEVICE_NAME,
        .name_len = sizeof(DEVICE_NAME) - 1,
        .name_is_complete = 1,
    };
    rc = ble_gap_adv_rsp_set_fields(&rsp);
    if (rc != 0)
    {
        return rc;
    }
    struct ble_gap_adv_params params = {
        .conn_mode = BLE_GAP_CONN_MODE_UND,
        .disc_mode = BLE_GAP_DISC_MODE_GEN,
    };
    return ble_gap_adv_start(own_addr_type, NULL, BLE_HS_FOREVER, &params, gap_event, NULL);
}

// Server: only the peer from ACOM may connect; anyone else is dropped.
static void server_on_connect(uint16_t conn)
{
    struct ble_gap_conn_desc desc;
    if (ble_gap_conn_find(conn, &desc) != 0
        || memcmp(desc.peer_id_addr.val, peer_addr.val, sizeof(peer_addr.val)) != 0)
    {
        ESP_LOGW(TAG, "Server: unexpected device, dropping it");
        ble_gap_terminate(conn, BLE_ERR_REM_USER_CONN_TERM);
        return;
    }
    conn_handle = conn;
    connected = true;
    ESP_LOGI(TAG, "Server: peer connected");
}

// ── Client steps (run in the client task, completed by callbacks) ───────

static int wait_op(void)
{
    if (xSemaphoreTake(op_done, pdMS_TO_TICKS(OP_TIMEOUT_MS)) != pdTRUE)
    {
        return BLE_HS_ETIMEOUT;
    }
    return op_status;
}

static void op_finish(int status)
{
    op_status = status;
    xSemaphoreGive(op_done);
}

static int on_mtu(uint16_t conn, const struct ble_gatt_error *error, uint16_t mtu, void *arg)
{
    op_finish(error->status);
    return 0;
}

static int on_svc(uint16_t conn, const struct ble_gatt_error *error,
                  const struct ble_gatt_svc *svc, void *arg)
{
    if (error->status == 0)
    {
        svc_start = svc->start_handle;
        svc_end = svc->end_handle;
        return 0;
    }
    op_finish(error->status == BLE_HS_EDONE ? (svc_start ? 0 : BLE_HS_ENOENT) : error->status);
    return 0;
}

static int on_chr(uint16_t conn, const struct ble_gatt_error *error,
                  const struct ble_gatt_chr *chr, void *arg)
{
    if (error->status == 0)
    {
        if (ble_uuid_cmp(&chr->uuid.u, &tx_uuid.u) == 0 && (chr->properties & BLE_GATT_CHR_PROP_READ))
        {
            peer_tx_handle = chr->val_handle;
        }
        else if (ble_uuid_cmp(&chr->uuid.u, &rx_uuid.u) == 0
                 && (chr->properties & BLE_GATT_CHR_PROP_WRITE))
        {
            peer_rx_handle = chr->val_handle;
        }
        return 0;
    }
    bool found = peer_tx_handle && peer_rx_handle;
    op_finish(error->status == BLE_HS_EDONE ? (found ? 0 : BLE_HS_ENOENT) : error->status);
    return 0;
}

static int on_write(uint16_t conn, const struct ble_gatt_error *error, struct ble_gatt_attr *attr,
                    void *arg)
{
    op_finish(error->status);
    return 0;
}

// Long read: called per part with status 0, then once with BLE_HS_EDONE.
static int on_read(uint16_t conn, const struct ble_gatt_error *error, struct ble_gatt_attr *attr,
                   void *arg)
{
    if (error->status == 0)
    {
        uint16_t n = OS_MBUF_PKTLEN(attr->om);
        uint16_t copied = 0;
        if (read_len + n > sizeof(read_buf)
            || ble_hs_mbuf_to_flat(attr->om, read_buf + read_len, n, &copied) != 0)
        {
            op_finish(BLE_HS_EMSGSIZE);
            return BLE_HS_EMSGSIZE;  // aborts the procedure
        }
        read_len += copied;
        return 0;
    }
    op_finish(error->status == BLE_HS_EDONE ? 0 : error->status);
    return 0;
}

// Write with response; a long write if it does not fit one ATT packet.
static int client_write(const uint8_t *data, uint16_t len)
{
    int rc;
    if (len <= ble_att_mtu(conn_handle) - 3)
    {
        rc = ble_gattc_write_flat(conn_handle, peer_rx_handle, data, len, on_write, NULL);
    }
    else
    {
        struct os_mbuf *om = ble_hs_mbuf_from_flat(data, len);
        if (!om)
        {
            return BLE_HS_ENOMEM;
        }
        rc = ble_gattc_write_long(conn_handle, peer_rx_handle, 0, om, on_write, NULL);
    }
    return rc != 0 ? rc : wait_op();
}

static int client_read(void)
{
    read_len = 0;
    int rc = ble_gattc_read_long(conn_handle, peer_tx_handle, 0, on_read, NULL);
    return rc != 0 ? rc : wait_op();
}

static bool client_connect(void)
{
    int rc = ble_gap_connect(own_addr_type, &peer_addr, CONNECT_TIMEOUT_MS, NULL, gap_event, NULL);
    if (rc != 0)
    {
        ESP_LOGW(TAG, "Client: connect failed to start: %d", rc);
        return false;
    }
    // The GAP connect event reports the outcome, also on timeout.
    if (xSemaphoreTake(op_done, pdMS_TO_TICKS(CONNECT_TIMEOUT_MS + 1000)) != pdTRUE)
    {
        ble_gap_conn_cancel();
        return false;
    }
    if (op_status != 0)
    {
        ESP_LOGW(TAG, "Client: connect failed: %d", op_status);
        return false;
    }

    ble_gap_set_data_len(conn_handle, DATA_LEN_OCTETS, DATA_LEN_TIME_US);
    rc = ble_gattc_exchange_mtu(conn_handle, on_mtu, NULL);
    if (rc == 0)
    {
        rc = wait_op();
    }
    ESP_LOGI(TAG, "Client: connected, MTU %u", ble_att_mtu(conn_handle));

    svc_start = svc_end = peer_tx_handle = peer_rx_handle = 0;
    rc = ble_gattc_disc_svc_by_uuid(conn_handle, &svc_uuid.u, on_svc, NULL);
    if (rc == 0)
    {
        rc = wait_op();
    }
    if (rc == 0)
    {
        rc = ble_gattc_disc_all_chrs(conn_handle, svc_start, svc_end, on_chr, NULL);
    }
    if (rc == 0)
    {
        rc = wait_op();
    }
    if (rc != 0)
    {
        ESP_LOGW(TAG, "Client: service or characteristics not found: %d", rc);
        return false;
    }
    return true;
}

// Fetches the server's data, chunk by chunk: request, then read.
static bool client_receive(uint16_t tx_total)
{
    uint16_t i = 0;
    uint16_t total = 1;
    int      retries = 0;
    while (i < total && connected && !stop_requested)
    {
        uint8_t req[3] = {REQ_MAGIC};
        put_be16(req + 1, i);
        bool ok = client_write(req, sizeof(req)) == 0 && client_read() == 0
                  && read_len >= CHUNK_HDR && get_be16(read_buf) == i;
        if (ok)
        {
            total = get_be16(read_buf + 2);
            ok = accept_chunk(i, total, read_buf + CHUNK_HDR, read_len - CHUNK_HDR);
        }
        if (!ok)
        {
            if (++retries > OP_RETRIES)
            {
                return false;
            }
            vTaskDelay(pdMS_TO_TICKS(RETRY_DELAY_MS));
            continue;
        }
        retries = 0;
        if (i == 0)
        {
            prog_total = total + tx_total;
            prog_state = BLE_LINK_PROG_ACTIVE;
        }
        i++;
        prog_done = i;
    }
    return rx_complete();
}

static bool client_send(uint16_t tx_total)
{
    for (uint16_t j = 0; j < tx_total && connected && !stop_requested; j++)
    {
        uint8_t pkt[CHUNK_PKT];
        size_t  n = build_chunk(j, pkt);
        int     rc = client_write(pkt, n);
        if (rc != 0)
        {
            ESP_LOGW(TAG, "Client: chunk %u not sent: %d", j, rc);
            return false;
        }
        prog_done = rx_total + j + 1;
    }
    return true;
}

static void client_task(void *arg)
{
    bool     received = false;
    uint16_t tx_total = chunk_count(tx_len);

    if (client_connect())
    {
        // Size header first, so the server knows the full transfer up front.
        uint8_t hdr[3] = {HDR_MAGIC};
        put_be16(hdr + 1, (uint16_t)tx_len);
        client_write(hdr, sizeof(hdr));

        received = client_receive(tx_total);
        ESP_LOGI(TAG, "Client: %s %u bytes", received ? "received" : "failed after",
                 (unsigned)rx_len);
        // Send ours even if theirs failed, as the old client did: the
        // server judges its own side.
        bool sent = client_send(tx_total);
        ESP_LOGI(TAG, "Client: %s", sent ? "sent ours" : "sending ours failed");
    }

    if (connected)
    {
        ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        xSemaphoreTake(op_done, pdMS_TO_TICKS(OP_TIMEOUT_MS));  // disconnect event
    }
    // Success is about the peer's data, as in the old firmware.
    finish(received);
    vTaskDelete(NULL);
}

// ── GAP events (host task) ───────────────────────────────

static int gap_event(struct ble_gap_event *event, void *arg)
{
    switch (event->type)
    {
        case BLE_GAP_EVENT_CONNECT:
            if (role == BLE_LINK_SERVER)
            {
                if (event->connect.status == 0)
                {
                    server_on_connect(event->connect.conn_handle);
                }
                else if (busy && !stop_requested)
                {
                    start_advertising();
                }
            }
            else
            {
                if (event->connect.status == 0)
                {
                    conn_handle = event->connect.conn_handle;
                    connected = true;
                }
                op_finish(event->connect.status);
            }
            return 0;

        case BLE_GAP_EVENT_DISCONNECT:
            if (event->disconnect.conn.conn_handle != conn_handle)
            {
                // A dropped stranger: keep waiting for the peer.
                if (role == BLE_LINK_SERVER && busy && !stop_requested)
                {
                    start_advertising();
                }
                return 0;
            }
            ESP_LOGI(TAG, "Disconnected, reason 0x%x", event->disconnect.reason);
            connected = false;
            conn_handle = BLE_HS_CONN_HANDLE_NONE;
            if (role == BLE_LINK_SERVER)
            {
                finish(rx_complete());
            }
            else
            {
                op_finish(BLE_HS_ENOTCONN);
            }
            return 0;

        case BLE_GAP_EVENT_MTU:
            ESP_LOGI(TAG, "MTU %u", event->mtu.value);
            return 0;

        default:
            return 0;
    }
}

// ── Host ─────────────────────────────────────────────────

static void on_sync(void)
{
    int rc = ble_hs_util_ensure_addr(0);
    if (rc == 0)
    {
        rc = ble_hs_id_infer_auto(0, &own_addr_type);
    }
    if (rc != 0)
    {
        ESP_LOGE(TAG, "No usable address: %d", rc);
        return;
    }
    xSemaphoreGive(synced);
}

static void on_reset(int reason)
{
    ESP_LOGW(TAG, "Host reset, reason %d", reason);
}

static void host_task(void *param)
{
    nimble_port_run();  // returns only after nimble_port_stop()
    nimble_port_freertos_deinit();
}

esp_err_t ble_link_init(void)
{
    if (synced)
    {
        return ESP_ERR_INVALID_STATE;
    }
    synced = xSemaphoreCreateBinary();
    op_done = xSemaphoreCreateBinary();
    if (!synced || !op_done)
    {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = nimble_port_init();
    if (err != ESP_OK)
    {
        return err;
    }
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;

    ble_svc_gap_init();
    ble_svc_gatt_init();
    int rc = ble_gatts_count_cfg(gatt_svcs);
    if (rc == 0)
    {
        rc = ble_gatts_add_svcs(gatt_svcs);
    }
    if (rc == 0)
    {
        rc = ble_svc_gap_device_name_set(DEVICE_NAME);
    }
    if (rc != 0)
    {
        ESP_LOGE(TAG, "GATT setup failed: %d", rc);
        return ESP_FAIL;
    }

    nimble_port_freertos_init(host_task);
    if (xSemaphoreTake(synced, pdMS_TO_TICKS(SYNC_TIMEOUT_MS)) != pdTRUE)
    {
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

esp_err_t ble_link_own_addr(uint8_t addr[6], uint8_t *type)
{
    uint8_t val[6];
    if (!synced || ble_hs_id_copy_addr(own_addr_type, val, NULL) != 0)
    {
        return ESP_ERR_INVALID_STATE;
    }
    // NimBLE keeps addresses least significant byte first.
    for (int i = 0; i < 6; i++)
    {
        addr[i] = val[5 - i];
    }
    if (type)
    {
        *type = own_addr_type;
    }
    return ESP_OK;
}

esp_err_t ble_link_start(const uint8_t peer[6], uint8_t peer_type, ble_link_role_t r,
                         const uint8_t *data, size_t len, ble_link_done_cb_t cb)
{
    if (!synced || busy)
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (!peer || !data || len == 0 || len > BLE_LINK_MAX_LEN || !cb)
    {
        return ESP_ERR_INVALID_ARG;
    }

    peer_addr.type = peer_type;
    for (int i = 0; i < 6; i++)
    {
        peer_addr.val[i] = peer[5 - i];
    }
    memcpy(tx_buf, data, len);
    tx_len = len;
    role = r;
    done_cb = cb;
    rx_len = 0;
    rx_total = 0;
    rx_chunks = 0;
    req_chunk = 0;
    prog_done = 0;
    prog_total = 0;
    prog_state = BLE_LINK_PROG_CONNECTING;
    connected = false;
    conn_handle = BLE_HS_CONN_HANDLE_NONE;
    stop_requested = false;
    finished = false;
    xSemaphoreTake(op_done, 0);  // drop a stale completion
    busy = true;

    if (role == BLE_LINK_SERVER)
    {
        int rc = start_advertising();
        if (rc != 0)
        {
            busy = false;
            ESP_LOGE(TAG, "Advertising failed: %d", rc);
            return ESP_FAIL;
        }
        ESP_LOGI(TAG, "Server: advertising, %u bytes to send", (unsigned)len);
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Client: connecting to %02X:%02X:%02X:%02X:%02X:%02X, %u bytes to send", peer[0],
             peer[1], peer[2], peer[3], peer[4], peer[5], (unsigned)len);
    if (xTaskCreate(client_task, "ble_client", CLIENT_TASK_STACK, NULL, CLIENT_TASK_PRIO, NULL)
        != pdPASS)
    {
        busy = false;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void ble_link_stop(void)
{
    if (!busy)
    {
        return;
    }
    stop_requested = true;
    ble_gap_adv_stop();
    if (role == BLE_LINK_CLIENT)
    {
        ble_gap_conn_cancel();  // if still connecting
    }
    if (connected)
    {
        ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    }

    TickType_t t0 = xTaskGetTickCount();
    while (busy && xTaskGetTickCount() - t0 < pdMS_TO_TICKS(STOP_TIMEOUT_MS))
    {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    // A server that was only advertising gets no disconnect event.
    if (busy && role == BLE_LINK_SERVER && !connected)
    {
        finish(false);
    }
}

bool ble_link_is_busy(void)
{
    return busy;
}

bool ble_link_is_connected(void)
{
    return connected;
}

ble_link_prog_t ble_link_progress(uint16_t *done, uint16_t *total)
{
    if (done)
    {
        *done = prog_done;
    }
    if (total)
    {
        *total = prog_total;
    }
    return prog_state;
}
