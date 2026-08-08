/**
 * @file neo_device.c
 * @brief High-level NEO device transport helpers built on top of the USB shim.
 *
 * Implements NeoTools-style hello / dialogue / block transfer / RESTART.
 * Raw USB stays in usb_host_neo so protocol and host scheduling stay separable.
 *
 * Pitfalls learned on hardware (keep these):
 *   - Hello response is 2 bytes (protocol version), not an 8-byte framed message.
 *   - Applet IDs are uint16_t (AlphaWord 0xA000); 8-bit truncation breaks everything.
 *   - After reset/switch, drain carefully — a leftover IN can block the "Switched" read.
 *   - RESTART may drop the device from the bus; treat dialogue_end errors as soft.
 */

#include "neo_device.h"
#include "neo_applet.h"
#include "usb_host_neo.h"
#include "neo_debug.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>

static const char *TAG = "neo_device";

static SemaphoreHandle_t s_proto_lock;

void neo_device_lock(void)
{
    if (!s_proto_lock) {
        s_proto_lock = xSemaphoreCreateRecursiveMutex();
    }
    if (s_proto_lock) {
        xSemaphoreTakeRecursive(s_proto_lock, portMAX_DELAY);
    }
}

void neo_device_unlock(void)
{
    if (s_proto_lock) {
        xSemaphoreGiveRecursive(s_proto_lock);
    }
}

/**
 * @brief Compute a simple additive checksum for a data buffer.
 *
 * This function implements the checksum algorithm used by the NEO block
 * transfer protocol: a simple 16-bit truncating sum of all bytes.
 *
 * @param buffer Pointer to the data to checksum (may be NULL if length==0).
 * @param length Number of bytes to include in the checksum.
 * @return 16-bit checksum value.
 */
uint16_t neo_device_data_checksum(const uint8_t *buffer, size_t length)
{
    uint32_t checksum = 0;
    for (size_t index = 0; index < length; index++) checksum += buffer[index];
    return (uint16_t)checksum;
}


#define NEO_DIALOGUE_TIMEOUT_MS 1000
/** NeoTools uses 100 ms; allow extra for ESP-IDF USB host scheduling. */
#define NEO_HELLO_TIMEOUT_MS 400
#define NEO_HELLO_RETRIES 10
/** Pause only between hello retries after a reset (NeoTools: sleep(0.1)). */
#define NEO_HELLO_RETRY_DELAY_MS 100

esp_err_t neo_device_read_exact(uint8_t *buffer, size_t length, int timeout_ms, size_t *out_length)
{
    if (!buffer || !out_length || length == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (timeout_ms <= 0) {
        timeout_ms = NEO_DEFAULT_TIMEOUT_MS;
    }
    esp_err_t result = usb_host_neo_read(buffer, length, timeout_ms, out_length);
    if (result != ESP_OK) {
        return result;
    }
    return *out_length == length ? ESP_OK : ESP_ERR_TIMEOUT;
}

static bool neo_device_response_is_switched(const uint8_t *buf, size_t len)
{
    return len == 8 && memcmp(buf, "Switched", 8) == 0;
}

static bool neo_device_try_read_switched(uint8_t *buf, size_t buf_len, size_t *out_len, int timeout_ms)
{
    if (!buf || buf_len < 8 || !out_len) {
        return false;
    }
    *out_len = 0;
    esp_err_t err = neo_device_read_exact(buf, 8, timeout_ms, out_len);
    return err == ESP_OK && neo_device_response_is_switched(buf, *out_len);
}

/**
 * @brief Read an exact number of bytes from the NEO transport.
 *
 * This helper repeatedly calls `usb_host_neo_read` until `length` bytes have
 * been received or an error/timeout occurs. Partial reads are accumulated in
 * `buffer` and the resulting number of bytes is written to `out_length`.
 *
 * @param buffer Destination buffer to receive data.
 * @param length Number of bytes to read.
 * @param timeout_ms Per-call timeout in milliseconds (use 0 for default).
 * @param out_length Pointer to size_t receiving the actual number of bytes read.
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG for bad parameters, or an
 *         error code propagated from the underlying USB shim.
 */
esp_err_t neo_device_read(uint8_t *buffer, size_t length, int timeout_ms, size_t *out_length)
{
    if (!buffer || !out_length) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_length = 0;
    if (length == 0) {
        return ESP_OK;
    }
    if (timeout_ms <= 0) {
        timeout_ms = NEO_DEFAULT_TIMEOUT_MS;
    }

    /* usb_host_neo_read now accumulates the full request in one USB-client job
     * (NeoTools 8-byte short-packet loop runs there — no per-8-byte task hop). */
    esp_err_t result = usb_host_neo_read(buffer, length, timeout_ms, out_length);
    if (result != ESP_OK) {
        return result;
    }
    if (*out_length != length) {
        neo_debug_event("read short: need %u got %u", (unsigned)length, (unsigned)*out_length);
    }
    return *out_length == length ? ESP_OK : (*out_length > 0 ? ESP_OK : ESP_ERR_INVALID_SIZE);
}

/**
 * @brief Write bytes to the NEO transport.
 *
 * A thin wrapper around `usb_host_neo_write` that validates arguments and
 * normalizes the timeout value.
 *
 * @param buffer Pointer to bytes to send.
 * @param length Number of bytes to send.
 * @param timeout_ms Per-call timeout in milliseconds (use 0 for default).
 * @return ESP_OK on success or an esp_err_t from the transport layer.
 */
esp_err_t neo_device_write(const uint8_t *buffer, size_t length, int timeout_ms)
{
    if (!buffer && length != 0) return ESP_ERR_INVALID_ARG;
    return usb_host_neo_write(buffer, length, timeout_ms > 0 ? timeout_ms : NEO_DEFAULT_TIMEOUT_MS);
}

/**
 * @brief Send a single 8-byte NEO protocol message.
 *
 * The message checksum must already be valid when calling this function.
 *
 * @param msg Pointer to the initialized `neo_message_t` to send.
 * @return ESP_OK on success or an esp_err_t from the write operation.
 */
esp_err_t neo_device_send_message(const neo_message_t *msg)
{
    if (!msg || !neo_message_checksum_is_valid(msg)) {
        neo_debug_event("send_message invalid msg");
        return ESP_ERR_INVALID_ARG;
    }
    neo_debug_message("REQUEST", msg->data);
    return neo_device_write(msg->data, sizeof(msg->data), NEO_DEFAULT_TIMEOUT_MS);
}

/**
 * @brief Receive a single 8-byte NEO protocol message and validate it.
 *
 * Reads exactly eight bytes from the transport and checks the message
 * checksum before returning.
 *
 * @param msg Pointer to the message structure that will receive the data.
 * @param timeout_ms Timeout in milliseconds for transport reads (0 uses default).
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG for bad args, or an error
 *         code indicating transport/CRC failure.
 */
esp_err_t neo_device_receive_message(neo_message_t *msg, int timeout_ms)
{
    if (!msg) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t received = 0;
    esp_err_t result = neo_device_read_exact(msg->data, sizeof(msg->data), timeout_ms, &received);
    if (result != ESP_OK) {
        neo_debug_event("receive_message read failed: %s (got %u bytes)", esp_err_to_name(result),
                        (unsigned)received);
        return result;
    }
    if (!neo_message_checksum_is_valid(msg)) {
        neo_debug_message("msg_in_bad", msg->data);
        return ESP_ERR_INVALID_CRC;
    }
    neo_debug_message("RESPONSE", msg->data);
    return ESP_OK;
}

/**
 * @brief Send a request message and optionally enforce the expected response.
 *
 * This convenience wraps a message send followed by a single reply receive.
 * If `expected_response` is non-zero the function validates the reply's
 * command byte and returns `ESP_FAIL` on mismatch (the unexpected response
 * is still returned to the caller when `response` is non-NULL).
 *
 * @param request Pointer to the request message to send.
 * @param expected_response Command byte expected in the response (0 = accept any).
 * @param timeout_ms Per-call timeout (ms); used for both send and receive.
 * @param response Optional out parameter receiving the response message.
 * @return ESP_OK on success, or an esp_err_t describing the failure.
 */
esp_err_t neo_device_send_command(const neo_message_t *request, uint8_t expected_response,
                                  int timeout_ms, neo_message_t *response)
{
    if (timeout_ms <= 0) {
        timeout_ms = NEO_DIALOGUE_TIMEOUT_MS;
    }

    esp_err_t result = neo_device_send_message(request);
    if (result != ESP_OK) {
        neo_debug_event("send_command tx failed cmd=0x%02x: %s", neo_message_command(request),
                        esp_err_to_name(result));
        /* Do not park an IN transfer after a failed OUT. */
        return result;
    }

    neo_message_t local_response;
    neo_message_t *actual_response = response ? response : &local_response;
    result = neo_device_receive_message(actual_response, timeout_ms);
    if (result != ESP_OK) {
        neo_debug_event("send_command rx failed cmd=0x%02x expect=0x%02x: %s", neo_message_command(request),
                        expected_response, esp_err_to_name(result));
        return result;
    }
    if (expected_response == 0 || neo_message_command(actual_response) == expected_response) {
        neo_debug_command_exchange(request->data, actual_response->data, expected_response, ESP_OK);
        return ESP_OK;
    }
    ESP_LOGW(TAG, "Unexpected response 0x%02x: %s", neo_message_command(actual_response),
             neo_message_error_string(neo_message_command(actual_response)));
    neo_debug_command_exchange(request->data, actual_response->data, expected_response, ESP_FAIL);
    return ESP_FAIL;
}

/**
 * @brief Enter a command dialogue and select the target applet on the device.
 *
 * Performs the legacy small handshake (hello/reset) and instructs the device
 * to switch to `applet_id`. Returns `ESP_ERR_NOT_SUPPORTED` when the
 * connected device's firmware does not implement the required handshake.
 *
 * @param applet_id Numeric identifier of the applet to select.
 * @return ESP_OK on success, ESP_ERR_NOT_SUPPORTED for incompatible devices,
 *         or an esp_err_t for transport errors.
 */
esp_err_t neo_device_dialogue_start(uint16_t applet_id)
{
    /*
     * ASM dialogue open — copied from NeoTools session start.
     *
     * Step A — Hello (NOT an 8-byte framed message):
     *   Host sends 1 byte: 0x01
     *   Neo replies 2 bytes: protocol version big-endian (we require >= 0x0220)
     *   Wrong: reading 8 bytes here — Neo never sends a full packet and USB stalls.
     *
     * Step B — Reset (8-byte ASCII command, no immediate reply expected):
     *   "?\\xff\\0reset" clears ASM state between operations.
     *
     * Step C — Switch applet (8-byte ASCII + applet id in bytes 6-7 BE):
     *   "?Switch" + applet_id → Neo must reply exactly 8 bytes "Switched"
     *   System applet 0x0000 is used for LIST_APPLETS, file attrs, RESTART, etc.
     *   AlphaWord is 0xA000 (uint16 — never truncate to uint8).
     */
    static const uint8_t reset_command[8] = {'?', 0xff, 0x00, 'r', 'e', 's', 'e', 't'};
    uint8_t hello = 0x01;
    uint8_t response[8];
    size_t received = 0;
    uint16_t proto = 0;
    bool hello_ok = false;

    neo_debug_event("dialogue_start applet=0x%04x", applet_id);
    usb_host_neo_prepare_dialogue();
    /* NeoTools hello() starts immediately — no fixed settle before the first 0x01. */

    for (int attempt = 0; attempt < NEO_HELLO_RETRIES; attempt++) {
        if (attempt > 0) {
            neo_debug_event("hello retry %d", attempt);
            /* NeoTools: reset + short delay between hello attempts. */
            (void)neo_device_write(reset_command, sizeof(reset_command), NEO_DIALOGUE_TIMEOUT_MS);
            vTaskDelay(pdMS_TO_TICKS(NEO_HELLO_RETRY_DELAY_MS));
        }
        neo_debug_raw("hello", "REQUEST", &hello, 1);
        esp_err_t result = neo_device_write(&hello, 1, NEO_HELLO_TIMEOUT_MS);
        if (result != ESP_OK) {
            neo_debug_event("hello write failed: %s", esp_err_to_name(result));
            continue;
        }
        received = 0;
        /* NeoTools: hello reply is exactly 2 bytes (version). Reading 8 hung the dialogue. */
        result = neo_device_read_exact(response, 2, NEO_HELLO_TIMEOUT_MS, &received);
        if (result == ESP_OK && received == 2) {
            proto = ((uint16_t)response[0] << 8) | response[1];
            neo_debug_raw("hello", "RESPONSE", response, received);
            neo_debug_event("hello protocol version 0x%04x", proto);
            hello_ok = true;
            break;
        }
        neo_debug_event("hello unexpected len %u", (unsigned)received);
        if (received > 0) {
            neo_debug_raw("hello", "RESPONSE", response, received);
        }
        received = 0;
    }
    if (!hello_ok || received < 2) {
        neo_debug_event("hello failed — device not responding in ASM mode");
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (proto < 0x0220) {
        neo_debug_event("hello protocol too old 0x%04x", proto);
        return ESP_ERR_NOT_SUPPORTED;
    }

    /* NeoTools: reset is write-only. Do not speculative-read first — that parks
     * an IN transfer on 0x82 and blocks the later "Switched" read. */
    neo_debug_raw("reset", "REQUEST", reset_command, sizeof(reset_command));
    esp_err_t result = neo_device_write(reset_command, sizeof(reset_command), NEO_DIALOGUE_TIMEOUT_MS);
    if (result != ESP_OK) {
        neo_debug_event("reset write failed: %s", esp_err_to_name(result));
        return result;
    }

    uint8_t switch_command[8] = {'?', 'S', 'w', 't', 'c', 'h', (uint8_t)(applet_id >> 8), (uint8_t)applet_id};
    neo_debug_raw("switch", "REQUEST", switch_command, sizeof(switch_command));
    result = neo_device_write(switch_command, sizeof(switch_command), NEO_DIALOGUE_TIMEOUT_MS);
    if (result != ESP_OK) {
        neo_debug_event("switch write failed: %s", esp_err_to_name(result));
        return result;
    }
    received = 0;
    if (!neo_device_try_read_switched(response, sizeof(response), &received, NEO_DIALOGUE_TIMEOUT_MS)) {
        neo_debug_event("switch failed (got %u bytes)", (unsigned)received);
        if (received > 0) {
            neo_debug_raw("switch", "RESPONSE", response, received);
        }
        return ESP_ERR_TIMEOUT;
    }
    neo_debug_raw("switch", "RESPONSE", response, received);
    neo_debug_event("dialogue_start ok applet=0x%04x", applet_id);
    return ESP_OK;
}

esp_err_t neo_device_query_version_message(neo_message_t *response)
{
    if (!response) {
        return ESP_ERR_INVALID_ARG;
    }

    /* NeoTools get_version: send REQUEST_VERSION, then wait for RESPONSE_VERSION.
     * Do not listen first — a speculative IN parks the endpoint. */
    neo_message_t request;
    neo_message_init(&request, NEO_REQUEST_VERSION, NULL);
    esp_err_t result = neo_device_send_message(&request);
    if (result != ESP_OK) {
        neo_debug_event("VERSION send failed: %s", esp_err_to_name(result));
        return result;
    }

    result = neo_device_receive_message(response, NEO_DIALOGUE_TIMEOUT_MS);
    if (result != ESP_OK) {
        neo_debug_event("VERSION receive failed: %s", esp_err_to_name(result));
        return result;
    }
    if (neo_message_command(response) != NEO_RESPONSE_VERSION) {
        neo_debug_event("VERSION unexpected response cmd=0x%02x", neo_message_command(response));
        return ESP_FAIL;
    }
    return ESP_OK;
}

/**
 * @brief End the current dialogue with the device.
 *
 * This sends the documented reset sequence used by the NEO protocol to
 * terminate a command dialogue and return the device to its normal state.
 *
 * @return ESP_OK on success or an esp_err_t from the write operation.
 */
esp_err_t neo_device_dialogue_end(void)
{
    static const uint8_t reset_command[8] = {'?', 0xff, 0x00, 'r', 'e', 's', 'e', 't'};
    neo_debug_event("dialogue_end");
    neo_debug_raw("reset", "REQUEST", reset_command, sizeof(reset_command));
    esp_err_t result = neo_device_write(reset_command, sizeof(reset_command), NEO_DEFAULT_TIMEOUT_MS);
    if (result != ESP_OK) {
        neo_debug_event("dialogue_end reset failed: %s", esp_err_to_name(result));
    }
    return result;
}

/**
 * @brief Read a multi-block data stream produced by the device.
 *
 * The device provides data in blocks; each block is announced with a
 * BLOCK_READ response that contains the upcoming block length and checksum.
 * This helper validates checksums and concatenates blocks into `buffer`.
 *
 * @param buffer Destination buffer for the assembled data.
 * @param capacity Capacity of `buffer` in bytes.
 * @param expected_length Total number of bytes expected from the device.
 * @param out_length Out parameter receiving the number of bytes actually read.
 * @return ESP_OK on success, or an esp_err_t for transport, size, or CRC errors.
 */
esp_err_t neo_device_read_extended(uint8_t *buffer, size_t capacity, size_t expected_length,
                                   size_t *out_length)
{
    /*
     * NeoTools block read loop:
     *   repeat until expected_length bytes collected:
     *     send REQUEST_BLOCK_READ (8-byte packet)
     *     receive RESPONSE_BLOCK_READ or BLOCK_READ_EMPTY
     *     RESPONSE carries: bytes 1-4 = chunk length, bytes 5-6 = 16-bit byte-sum CRC
     *     read `length` raw bytes from USB (not framed)
     *     verify sum(raw chunk) == CRC from message
     */
    if (!buffer || !out_length || expected_length > capacity) {
        neo_debug_event("read_extended invalid args cap=%u expect=%u", (unsigned)capacity,
                        (unsigned)expected_length);
        return ESP_ERR_INVALID_ARG;
    }
    *out_length = 0;
    while (*out_length < expected_length) {
        neo_message_t request;
        neo_message_t response;
        neo_message_init(&request, NEO_REQUEST_BLOCK_READ, NULL);
        esp_err_t result = neo_device_send_command(&request, 0, NEO_DEFAULT_TIMEOUT_MS, &response);
        if (result != ESP_OK) {
            neo_debug_event("read_extended cmd failed at %u/%u: %s", (unsigned)*out_length,
                            (unsigned)expected_length, esp_err_to_name(result));
            return result;
        }
        if (neo_message_command(&response) == NEO_RESPONSE_BLOCK_READ_EMPTY) {
            neo_debug_event("read_extended empty block at %u/%u", (unsigned)*out_length,
                            (unsigned)expected_length);
            return ESP_OK;
        }
        if (neo_message_command(&response) != NEO_RESPONSE_BLOCK_READ) {
            neo_debug_event("read_extended bad response 0x%02x at %u/%u", neo_message_command(&response),
                            (unsigned)*out_length, (unsigned)expected_length);
            return ESP_FAIL;
        }
        size_t block_length = neo_message_argument(&response, 1, 4);
        if (block_length > expected_length - *out_length) {
            neo_debug_event("read_extended block too large %u at offset %u", (unsigned)block_length,
                            (unsigned)*out_length);
            return ESP_ERR_INVALID_SIZE;
        }
        size_t received = 0;
        result = neo_device_read_exact(buffer + *out_length, block_length, (int)(block_length * 10 + 600),
                                       &received);
        if (result != ESP_OK || received != block_length) {
            neo_debug_event("read_extended data read failed block=%u got=%u: %s", (unsigned)block_length,
                            (unsigned)received, esp_err_to_name(result));
            return result != ESP_OK ? result : ESP_ERR_TIMEOUT;
        }
        if (neo_device_data_checksum(buffer + *out_length, received) != neo_message_argument(&response, 5, 2)) {
            neo_debug_event("read_extended CRC mismatch block=%u got=%u", (unsigned)received,
                            (unsigned)neo_message_argument(&response, 5, 2));
            return ESP_ERR_INVALID_CRC;
        }
        *out_length += received;
    }
    neo_debug_event("read_extended ok bytes=%u", (unsigned)*out_length);
    return ESP_OK;
}

/**
 * @brief Write a large buffer to the device using multiple block writes.
 *
 * The host announces each block's length and checksum using a BLOCK_WRITE
 * request before sending the block contents. The function splits `buffer`
 * into protocol-sized chunks and performs the necessary handshakes.
 *
 * @param buffer Pointer to the data to write.
 * @param length Length of the data in bytes.
 * @return ESP_OK on success or an esp_err_t if a sub-operation fails.
 */
esp_err_t neo_device_write_extended(const uint8_t *buffer, size_t length)
{
    /*
     * NeoTools block write loop (file/applet payload upload):
     *   for each up-to-1KiB slice:
     *     BLOCK_WRITE message with length + CRC of slice
     *     write slice bytes on USB bulk OUT
     *     wait RESPONSE_BLOCK_WRITE_DONE (8-byte packet)
     */
    if (!buffer && length != 0) return ESP_ERR_INVALID_ARG;
    neo_debug_event("write_extended start bytes=%u", (unsigned)length);
    for (size_t offset = 0; offset < length;) {
        size_t block_length = length - offset;
        if (block_length > NEO_BLOCK_SIZE) block_length = NEO_BLOCK_SIZE;
        const uint32_t args[][3] = {
            {block_length, 1, 4}, {neo_device_data_checksum(buffer + offset, block_length), 5, 2}, {0, 0, 0}
        };
        neo_message_t request;
        neo_message_init(&request, NEO_REQUEST_BLOCK_WRITE, args);
        esp_err_t result = neo_device_send_command(&request, NEO_RESPONSE_BLOCK_WRITE, NEO_DEFAULT_TIMEOUT_MS, NULL);
        if (result != ESP_OK) {
            neo_debug_event("write_extended BLOCK_WRITE failed at %u: %s", (unsigned)offset,
                            esp_err_to_name(result));
            return result;
        }
        result = neo_device_write(buffer + offset, block_length, NEO_DEFAULT_TIMEOUT_MS);
        if (result != ESP_OK) {
            neo_debug_event("write_extended data write failed at %u len=%u: %s", (unsigned)offset,
                            (unsigned)block_length, esp_err_to_name(result));
            return result;
        }
        neo_message_t response;
        result = neo_device_receive_message(&response, NEO_DEFAULT_TIMEOUT_MS);
        if (result != ESP_OK) {
            neo_debug_event("write_extended BLOCK_WRITE_DONE rx failed at %u: %s", (unsigned)offset,
                            esp_err_to_name(result));
            return result;
        }
        if (neo_message_command(&response) != NEO_RESPONSE_BLOCK_WRITE_DONE) {
            neo_debug_event("write_extended bad done response 0x%02x at %u", neo_message_command(&response),
                            (unsigned)offset);
            return ESP_FAIL;
        }
        offset += block_length;
    }
    neo_debug_event("write_extended ok bytes=%u", (unsigned)length);
    return ESP_OK;
}

esp_err_t neo_device_write_applet_content(const uint8_t *buffer, size_t length)
{
    if (!buffer && length != 0) {
        return ESP_ERR_INVALID_ARG;
    }
    neo_debug_event("write_applet_content start bytes=%u", (unsigned)length);
    for (size_t offset = 0; offset < length;) {
        size_t block_length = length - offset;
        if (block_length > NEO_BLOCK_SIZE) {
            block_length = NEO_BLOCK_SIZE;
        }
        const uint32_t args[][3] = {
            {block_length, 1, 4}, {neo_device_data_checksum(buffer + offset, block_length), 5, 2}, {0, 0, 0}
        };
        neo_message_t request;
        neo_message_init(&request, NEO_REQUEST_BLOCK_WRITE, args);
        esp_err_t result = neo_device_send_command(&request, NEO_RESPONSE_BLOCK_WRITE, 600, NULL);
        if (result != ESP_OK) {
            neo_debug_event("write_applet BLOCK_WRITE failed at %u: %s", (unsigned)offset,
                            esp_err_to_name(result));
            return result;
        }
        result = neo_device_write(buffer + offset, block_length, 600);
        if (result != ESP_OK) {
            neo_debug_event("write_applet data write failed at %u: %s", (unsigned)offset,
                            esp_err_to_name(result));
            return result;
        }
        neo_message_t response;
        result = neo_device_receive_message(&response, 300);
        if (result != ESP_OK) {
            neo_debug_event("write_applet BLOCK_WRITE_DONE rx failed at %u: %s", (unsigned)offset,
                            esp_err_to_name(result));
            return result;
        }
        if (neo_message_command(&response) != NEO_RESPONSE_BLOCK_WRITE_DONE) {
            neo_debug_event("write_applet bad done response 0x%02x at %u", neo_message_command(&response),
                            (unsigned)offset);
            return ESP_FAIL;
        }
        neo_message_init(&request, NEO_REQUEST_PROGRAMMING_APPLET_BLOCK, NULL);
        result = neo_device_send_command(&request, NEO_RESPONSE_PROGRAMMING_APPLET_BLOCK, 5000, NULL);
        if (result != ESP_OK) {
            neo_debug_event("write_applet PROGRAMMING_BLOCK failed at %u: %s", (unsigned)offset,
                            esp_err_to_name(result));
            return result;
        }
        offset += block_length;
    }
    neo_debug_event("write_applet_content ok bytes=%u", (unsigned)length);
    return ESP_OK;
}

esp_err_t neo_device_get_file_attributes_open(uint16_t applet_id, uint8_t index, uint8_t *buf, size_t buf_len)
{
    if (!buf || buf_len < 40) {
        neo_debug_event("file_attrs invalid buffer");
        return ESP_ERR_INVALID_ARG;
    }
    if (!usb_host_neo_is_comms_ready()) {
        neo_debug_event("file_attrs comms not ready");
        return ESP_ERR_INVALID_STATE;
    }

    const uint32_t args[][3] = {{index, 4, 1}, {applet_id, 5, 2}, {0, 0, 0}};
    neo_message_t request;
    neo_message_t response;
    neo_message_init(&request, NEO_REQUEST_GET_FILE_ATTRIBUTES, args);
    esp_err_t result = neo_device_send_command(&request, 0, NEO_DEFAULT_TIMEOUT_MS, &response);
    if (result != ESP_OK) {
        neo_debug_event("file_attrs GET_FILE_ATTRIBUTES failed applet=0x%04x index=%u: %s", applet_id,
                        index, esp_err_to_name(result));
        return result;
    }
    if (neo_message_command(&response) == NEO_ERROR_PARAMETER) {
        neo_debug_event("file_attrs applet=0x%04x index=%u not found", applet_id, index);
        return ESP_ERR_NOT_FOUND;
    }
    if (neo_message_command(&response) != NEO_RESPONSE_GET_FILE_ATTRIBUTES) {
        neo_debug_event("file_attrs bad response 0x%02x", neo_message_command(&response));
        return ESP_FAIL;
    }

    size_t length = neo_message_argument(&response, 1, 4);
    uint16_t checksum = (uint16_t)neo_message_argument(&response, 5, 2);
    if (length != 40 || length > buf_len) {
        neo_debug_event("file_attrs bad length %u applet=0x%04x index=%u", (unsigned)length, applet_id,
                        index);
        return ESP_ERR_INVALID_SIZE;
    }

    size_t received = 0;
    result = neo_device_read_exact(buf, length, (int)(length * 10 + 600), &received);
    if (result != ESP_OK || received != length) {
        neo_debug_event("file_attrs payload read failed applet=0x%04x index=%u: %s (got %u/%u)", applet_id, index,
                        esp_err_to_name(result), (unsigned)received, (unsigned)length);
        return result != ESP_OK ? result : ESP_ERR_TIMEOUT;
    }
    if (neo_device_data_checksum(buf, received) != checksum) {
        neo_debug_event("file_attrs CRC mismatch applet=0x%04x index=%u", applet_id, index);
        return ESP_ERR_INVALID_CRC;
    }
    return ESP_OK;
}

esp_err_t neo_device_read_file_attributes(uint16_t applet_id, uint8_t index, uint8_t *buf, size_t buf_len)
{
    if (!buf || buf_len < 40) {
        neo_debug_event("file_attrs invalid buffer");
        return ESP_ERR_INVALID_ARG;
    }
    if (!usb_host_neo_is_comms_ready()) {
        neo_debug_event("file_attrs comms not ready");
        return ESP_ERR_INVALID_STATE;
    }

    neo_debug_event("file_attrs start applet=0x%04x index=%u", applet_id, index);
    neo_device_lock();
    esp_err_t result = neo_device_dialogue_start(NEO_APPLET_ID_SYSTEM);
    if (result != ESP_OK) {
        neo_debug_event("file_attrs dialogue_start failed applet=0x%04x: %s", applet_id,
                        esp_err_to_name(result));
        neo_device_unlock();
        return result;
    }

    result = neo_device_get_file_attributes_open(applet_id, index, buf, buf_len);
    neo_device_dialogue_end();
    if (result == ESP_OK) {
        neo_debug_event("file_attrs ok applet=0x%04x index=%u", applet_id, index);
    }
    neo_device_unlock();
    return result;
}

esp_err_t neo_device_restart(void)
{
    neo_device_lock();
    neo_debug_event("RESTART start");
    esp_err_t result = neo_device_dialogue_start(NEO_APPLET_ID_SYSTEM);
    if (result == ESP_OK) {
        neo_message_t request;
        neo_message_init(&request, NEO_REQUEST_RESTART, NULL);
        result = neo_device_send_command(&request, NEO_RESPONSE_RESTART, NEO_DEFAULT_TIMEOUT_MS, NULL);
    }
    /* Device may leave the bus after RESTART — ignore dialogue_end transport errors. */
    (void)neo_device_dialogue_end();
    neo_device_unlock();
    if (result == ESP_OK) {
        neo_debug_event("RESTART ok");
        usb_host_neo_invalidate_comms();
    } else {
        neo_debug_event("RESTART failed: %s", esp_err_to_name(result));
    }
    return result;
}
