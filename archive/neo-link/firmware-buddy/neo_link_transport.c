/**
 * @file neo_link_transport.c
 * @brief Decode HLT frames from Neo HID traffic; stub HTTP for LLM proxy research.
 */

#include "neo_link_transport.h"

#include "auth.h"
#include "neo_link_applet.h"
#include "neo_link_llm.h"
#include "neo_link_mailbox.h"
#include "neo_link_protocol.h"
#include "neo_link_text.h"
#include "usb_host_neo.h"

#include "esp_log.h"
#include "cJSON.h"

#include <stdlib.h>
#include <string.h>

static const char *TAG = "neo_link";

static char s_last_prompt[NEO_LINK_MAX_PAYLOAD + 1];
static char s_last_reply[NEO_LINK_REPLY_MAX + 1];
static neo_link_msg_type_t s_last_type;
static bool s_in_frame;

static void neo_link_on_message(const neo_link_message_t *msg)
{
    if (!msg) {
        return;
    }
    s_last_type = msg->type;
    if (msg->type == NEO_LINK_MSG_CHAT) {
        strlcpy(s_last_prompt, msg->text, sizeof(s_last_prompt));
        snprintf(s_last_reply, sizeof(s_last_reply), "(thinking...)");
        ESP_LOGI(TAG, "CHAT from Neo (%u bytes): %s", (unsigned)msg->text_len, msg->text);
        neo_link_llm_handle_chat_async(msg->text);
    } else if (msg->type == NEO_LINK_MSG_PING) {
        strlcpy(s_last_reply, "PONG", sizeof(s_last_reply));
        ESP_LOGI(TAG, "PING from Neo");
        neo_link_mailbox_deliver_async(s_last_reply, true);
    } else if (msg->type == NEO_LINK_MSG_ABORT) {
        ESP_LOGW(TAG, "ABORT from Neo");
    }
}

bool neo_link_transport_feed_text(const char *text, size_t len)
{
    if (!text || len == 0) {
        return false;
    }

    bool complete = false;
    neo_link_message_t msg;

    for (size_t i = 0; i < len; i++) {
        char ch = text[i];
        if (ch == '~') {
            s_in_frame = true;
        }
        if (neo_link_parser_feed(ch, &msg)) {
            neo_link_on_message(&msg);
            complete = true;
            s_in_frame = false;
        }
    }
    return complete;
}

const char *neo_link_transport_last_prompt(void)
{
    return s_last_prompt;
}

const char *neo_link_transport_last_reply(void)
{
    return s_last_reply;
}

void neo_link_transport_set_reply(const char *reply)
{
    if (!reply) {
        s_last_reply[0] = '\0';
        return;
    }
    strlcpy(s_last_reply, reply, sizeof(s_last_reply));
}

bool neo_link_transport_in_frame(void)
{
    return s_in_frame;
}

static bool link_req_auth(httpd_req_t *req)
{
    char auth_header[160];
    const char *prefix = "Bearer ";
    return httpd_req_get_hdr_value_str(req, "Authorization", auth_header, sizeof(auth_header)) == ESP_OK &&
           strncmp(auth_header, prefix, strlen(prefix)) == 0 &&
           auth_check_token(auth_header + strlen(prefix));
}

static esp_err_t link_status_get_handler(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        httpd_resp_send_500(req);
        return ESP_OK;
    }
    cJSON_AddBoolToObject(root, "enabled", true);
    cJSON_AddBoolToObject(root, "in_frame", s_in_frame);
    const char *type = "none";
    if (s_last_type == NEO_LINK_MSG_CHAT) {
        type = "CHAT";
    } else if (s_last_type == NEO_LINK_MSG_PING) {
        type = "PING";
    }
    cJSON_AddStringToObject(root, "last_type", type);
    esp_err_t mb_err = neo_link_mailbox_last_error();
    cJSON_AddStringToObject(root, "mailbox", mb_err == ESP_OK ? "ok" : esp_err_to_name(mb_err));
    size_t applet_len = 0;
    (void)neo_link_applet_blob(&applet_len);
    cJSON_AddBoolToObject(root, "applet_bundled", applet_len > 0);
    cJSON_AddNumberToObject(root, "applet_id", NEO_LINK_APPLET_ID);
    cJSON_AddNumberToObject(root, "applet_bytes", (double)applet_len);

    neo_link_applet_status_t ast;
    neo_link_applet_get_status(&ast);
    char verbuf[24];
    snprintf(verbuf, sizeof(verbuf), "%u.%u.%c", (unsigned)ast.bundled_major,
             (unsigned)ast.bundled_minor, (char)ast.bundled_rev);
    cJSON_AddStringToObject(root, "applet_bundled_version", verbuf);
    cJSON_AddNumberToObject(root, "applet_bundled_ram", (double)ast.bundled_ram);
    cJSON_AddBoolToObject(root, "applet_installed", ast.installed);
    cJSON_AddBoolToObject(root, "applet_up_to_date", ast.up_to_date);
    cJSON_AddBoolToObject(root, "applet_needs_update",
                            ast.installed ? !ast.up_to_date : ast.bundled_major > 0);
    cJSON_AddBoolToObject(root, "applet_sync_busy", ast.sync_busy);
    if (ast.installed) {
        snprintf(verbuf, sizeof(verbuf), "%u.%u.%c", (unsigned)ast.installed_major,
                 (unsigned)ast.installed_minor, (char)ast.installed_rev);
        cJSON_AddStringToObject(root, "applet_installed_version", verbuf);
        cJSON_AddNumberToObject(root, "applet_installed_ram", (double)ast.installed_ram);
    }
    if (ast.last_sync_msg[0]) {
        cJSON_AddStringToObject(root, "applet_sync_msg", ast.last_sync_msg);
    }
    if (ast.last_sync_err != ESP_OK) {
        cJSON_AddStringToObject(root, "applet_sync_error", esp_err_to_name(ast.last_sync_err));
    }

    cJSON_AddBoolToObject(root, "neo_connected", usb_host_neo_is_connected());
    neo_link_llm_status_json(root);
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) {
        httpd_resp_send_500(req);
        return ESP_OK;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
    free(json);
    return ESP_OK;
}

static esp_err_t link_last_get_handler(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        httpd_resp_send_500(req);
        return ESP_OK;
    }
    cJSON_AddStringToObject(root, "prompt", s_last_prompt);
    cJSON_AddStringToObject(root, "reply", s_last_reply);
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) {
        httpd_resp_send_500(req);
        return ESP_OK;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
    free(json);
    return ESP_OK;
}

static esp_err_t link_echo_post_handler(httpd_req_t *req)
{
    if (!link_req_auth(req)) {
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_send(req, "unauthorized", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    char *buf = malloc(NEO_LINK_REPLY_MAX + 1);
    if (!buf) {
        httpd_resp_send_500(req);
        return ESP_OK;
    }
    int received = httpd_req_recv(req, buf, NEO_LINK_REPLY_MAX);
    if (received <= 0) {
        free(buf);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "empty body");
        return ESP_OK;
    }
    buf[received] = '\0';
    neo_link_transport_set_reply(buf);
    esp_err_t err = neo_link_mailbox_deliver(buf, true);
    free(buf);
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        httpd_resp_send_500(req);
        return ESP_OK;
    }
    cJSON_AddBoolToObject(root, "ok", err == ESP_OK);
    if (err != ESP_OK) {
        cJSON_AddStringToObject(root, "error", esp_err_to_name(err));
    }
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) {
        httpd_resp_send_500(req);
        return ESP_OK;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
    free(json);
    return ESP_OK;
}

static esp_err_t link_send_post_handler(httpd_req_t *req)
{
    if (!link_req_auth(req)) {
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_send(req, "unauthorized", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    char *buf = malloc(NEO_LINK_REPLY_MAX + 1);
    if (!buf) {
        httpd_resp_send_500(req);
        return ESP_OK;
    }
    int received = httpd_req_recv(req, buf, NEO_LINK_REPLY_MAX);
    if (received <= 0) {
        free(buf);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "empty body");
        return ESP_OK;
    }
    buf[received] = '\0';
    neo_link_transport_set_reply(buf);
    esp_err_t err = neo_link_mailbox_deliver(buf, true);
    free(buf);
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        httpd_resp_send_500(req);
        return ESP_OK;
    }
    cJSON_AddBoolToObject(root, "ok", err == ESP_OK);
    cJSON_AddBoolToObject(root, "delivered", err == ESP_OK);
    if (err != ESP_OK) {
        cJSON_AddStringToObject(root, "error", esp_err_to_name(err));
    }
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) {
        httpd_resp_send_500(req);
        return ESP_OK;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
    free(json);
    return ESP_OK;
}

static esp_err_t link_pull_post_handler(httpd_req_t *req)
{
    if (!link_req_auth(req)) {
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_send(req, "unauthorized", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    /* Read NeoLinkOut from Neo Link Chat applet only (never AlphaWord). */
    char prompt[NEO_LINK_MAX_PAYLOAD + 1];
    size_t plen = 0;
    esp_err_t err = neo_link_mailbox_fetch_out(prompt, sizeof(prompt), &plen, true);
    if (err != ESP_OK || plen == 0) {
        cJSON *root = cJSON_CreateObject();
        if (!root) {
            httpd_resp_send_500(req);
            return ESP_OK;
        }
        cJSON_AddBoolToObject(root, "ok", false);
        cJSON_AddStringToObject(root, "error", esp_err_to_name(err));
        char *json = cJSON_PrintUnformatted(root);
        cJSON_Delete(root);
        if (!json) {
            httpd_resp_send_500(req);
            return ESP_OK;
        }
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
        free(json);
        return ESP_OK;
    }

    strlcpy(s_last_prompt, prompt, sizeof(s_last_prompt));
    s_last_type = NEO_LINK_MSG_CHAT;
    snprintf(s_last_reply, sizeof(s_last_reply), "(thinking...)");
    neo_link_llm_handle_chat_async(prompt);

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        httpd_resp_send_500(req);
        return ESP_OK;
    }
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddStringToObject(root, "prompt", prompt);
    cJSON_AddStringToObject(root, "path", "mailbox_out");
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) {
        httpd_resp_send_500(req);
        return ESP_OK;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
    free(json);
    return ESP_OK;
}

static esp_err_t link_install_applet_post_handler(httpd_req_t *req)
{
    if (!link_req_auth(req)) {
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_send(req, "unauthorized", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    if (!usb_host_neo_is_connected()) {
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"neo_not_connected\"}");
        return ESP_OK;
    }

    bool replace = true;
    char replace_hdr[8];
    if (httpd_req_get_hdr_value_str(req, "X-Neo-Replace", replace_hdr, sizeof(replace_hdr)) == ESP_OK) {
        replace = strcmp(replace_hdr, "true") == 0 || strcmp(replace_hdr, "1") == 0;
    }

    esp_err_t err = neo_link_applet_ensure_current(replace);

    size_t len = 0;
    (void)neo_link_applet_blob(&len);

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        httpd_resp_send_500(req);
        return ESP_OK;
    }
    cJSON_AddBoolToObject(root, "ok", err == ESP_OK);
    cJSON_AddNumberToObject(root, "applet_id", NEO_LINK_APPLET_ID);
    cJSON_AddNumberToObject(root, "bytes", (double)len);
    cJSON_AddBoolToObject(root, "replaced", replace);
    if (err != ESP_OK) {
        cJSON_AddStringToObject(root, "error", esp_err_to_name(err));
    } else {
        cJSON_AddStringToObject(root, "name", "Neo Link Chat");
        cJSON_AddStringToObject(root, "hint",
                                "Open Neo Link Chat (Left Shift+Tab at power-on). Files: NeoLinkIn + NeoLinkOut.");
    }
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) {
        httpd_resp_send_500(req);
        return ESP_OK;
    }
    if (err != ESP_OK) {
        httpd_resp_set_status(req, "500 Internal Server Error");
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
    free(json);
    return ESP_OK;
}

esp_err_t neo_link_web_register(httpd_handle_t server)
{
    if (!server) {
        return ESP_ERR_INVALID_ARG;
    }

    httpd_uri_t status_uri = {
        .uri = "/api/v1/link/status",
        .method = HTTP_GET,
        .handler = link_status_get_handler,
    };
    httpd_uri_t last_uri = {
        .uri = "/api/v1/link/last",
        .method = HTTP_GET,
        .handler = link_last_get_handler,
    };
    httpd_uri_t echo_uri = {
        .uri = "/api/v1/link/echo",
        .method = HTTP_POST,
        .handler = link_echo_post_handler,
    };
    httpd_uri_t send_uri = {
        .uri = "/api/v1/link/send",
        .method = HTTP_POST,
        .handler = link_send_post_handler,
    };
    httpd_uri_t pull_uri = {
        .uri = "/api/v1/link/pull",
        .method = HTTP_POST,
        .handler = link_pull_post_handler,
    };
    httpd_uri_t install_uri = {
        .uri = "/api/v1/link/install-applet",
        .method = HTTP_POST,
        .handler = link_install_applet_post_handler,
    };

    esp_err_t r = httpd_register_uri_handler(server, &status_uri);
    if (r != ESP_OK) {
        return r;
    }
    r = httpd_register_uri_handler(server, &last_uri);
    if (r != ESP_OK) {
        return r;
    }
    r = httpd_register_uri_handler(server, &echo_uri);
    if (r != ESP_OK) {
        return r;
    }
    r = httpd_register_uri_handler(server, &send_uri);
    if (r != ESP_OK) {
        return r;
    }
    r = httpd_register_uri_handler(server, &pull_uri);
    if (r != ESP_OK) {
        return r;
    }
    r = httpd_register_uri_handler(server, &install_uri);
    if (r != ESP_OK) {
        return r;
    }
    return neo_link_llm_web_register(server);
}
