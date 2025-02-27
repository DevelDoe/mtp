#include "mongoose.h"
#include <stdio.h>
#include <json-c/json.h>

/* ---------------------------- Configuration ------------------------------ */
static const char *s_listen_url = "http://0.0.0.0:8000";

/* -------------------------- Client Management ---------------------------- */
typedef struct ClientNode {
    char client_id[32];
    struct mg_connection *conn;
    struct ClientNode *next;
} ClientNode;

static ClientNode *client_map = NULL;

static void add_client(const char *client_id, struct mg_connection *conn) {
    ClientNode *node = malloc(sizeof(ClientNode));
    snprintf(node->client_id, sizeof(node->client_id), "%s", client_id);
    node->conn = conn;
    node->next = client_map;
    client_map = node;
    printf("[SERVER] Registered client: %s\n", client_id);
}

static void remove_client(struct mg_connection *conn) {
    ClientNode **curr = &client_map;
    while (*curr) {
        if ((*curr)->conn == conn) {
            ClientNode *tmp = *curr;
            *curr = (*curr)->next;
            free(tmp);
            printf("[SERVER] Removed client\n");
            return;
        }
        curr = &(*curr)->next;
    }
}

static struct mg_connection *find_client(const char *client_id) {
    ClientNode *curr = client_map;
    while (curr && strcmp(curr->client_id, client_id) != 0) curr = curr->next;
    return curr ? curr->conn : NULL;
}

/* ------------------------ WebSocket Utilities ---------------------------- */
static void broadcast_alert(const char *alert_data) {
    printf("[SERVER] Broadcasting alert to all clients: %s\n", alert_data);
    for (ClientNode *curr = client_map; curr; curr = curr->next) {
        if (strcmp(curr->client_id, "scanner") != 0) {
            printf("[SERVER] Sending alert to client: %s\n", curr->client_id);
            mg_ws_send(curr->conn, alert_data, strlen(alert_data), WEBSOCKET_OP_TEXT);
        }
    }
}

/* ------------------------- HTTP Handlers ---------------------------------- */
static void handle_root(struct mg_connection *c) {
    printf("[SERVER] Received request for root endpoint\n");
    mg_http_reply(c, 200, "Content-Type: application/json\r\n",
                 "{\"status\": \"Server is running\"}");
}

static void handle_ws_upgrade(struct mg_connection *c, struct mg_http_message *hm) {
    printf("[SERVER] Upgrading connection to WebSocket\n");
    mg_ws_upgrade(c, hm, NULL);
}

static void handle_scanner_update(struct mg_connection *c, struct mg_http_message *hm) {
    printf("[SERVER] Received symbol update request\n");

    struct json_object *root = json_tokener_parse(hm->bodymg_str);
    struct json_object *client_id_obj, *data_obj;

    if (!root || !json_object_object_get_ex(root, "client_id", &client_id_obj) ||
               !json_object_object_get_ex(root, "data", &data_obj)) {
        printf("[SERVER] Invalid symbol update request format\n");
        mg_http_reply(c, 400, "Content-Type: application/json\r\n",
                     "{\"error\": \"Invalid request format\"}");
        json_object_put(root);
        return;
    }

    const char *client_id = json_object_get_string(client_id_obj);
    const char *data = json_object_to_json_string(data_obj);

    printf("[SERVER] Received symbols from client %s: %s\n", client_id, data);

    struct mg_connection *scanner = find_client("scanner");

    if (scanner) {
        printf("[SERVER] Forwarding symbols to scanner\n");
        mg_ws_send(scanner, data, strlen(data), WEBSOCKET_OP_TEXT);
        mg_http_reply(c, 200, NULL, "Symbols forwarded to scanner");
    } else {
        printf("[SERVER] Scanner is offline\n");
        mg_http_reply(c, 503, "Content-Type: application/json\r\n",
                     "{\"error\": \"Scanner offline\"}");
    }

    json_object_put(root);
}

/* ---------------------- WebSocket Handlers ------------------------------- */
static void handle_ws_message(struct mg_connection *c, struct mg_ws_message *wm) {
    printf("[SERVER] Received WebSocket message: %.*s\n", (int)wm->data.len, wm->datamg_str);

    struct json_object *root = json_tokener_parse(wm->datamg_str);
    struct json_object *client_id_obj, *data_obj;

    if (!root || !json_object_object_get_ex(root, "client_id", &client_id_obj)) {
        printf("[SERVER] Invalid WebSocket message format\n");
        return;
    }

    const char *client_id = json_object_get_string(client_id_obj);

    // Register new clients (except scanner which connects via HTTP first)
    if (!find_client(client_id)) {
        printf("[SERVER] Registering new client: %s\n", client_id);
        add_client(client_id, c);
    }

    // Handle scanner alerts
    if (strcmp(client_id, "scanner") == 0 &&
        json_object_object_get_ex(root, "data", &data_obj)) {
        const char *alert = json_object_to_json_string(data_obj);
        printf("[SERVER] Received alert from scanner: %s\n", alert);
        broadcast_alert(alert);
    }

    json_object_put(root);
}

/* ------------------------- Event Handling -------------------------------- */
static void event_handler(struct mg_connection *c, int ev, void *ev_data) {
    switch (ev) {
        case MG_EV_HTTP_MSG: {
            struct mg_http_message *hm = (struct mg_http_message *) ev_data;

            if (mg_http_match_uri(hm, "/")) {
                handle_root(c);
            } else if (mg_http_match_uri(hm, "/ws")) {
                handle_ws_upgrade(c, hm);
            } else if (mg_http_match_uri(hm, "/update-scanner-symbols")) {
                handle_scanner_update(c, hm);
            } else {
                printf("[SERVER] Received request for unknown endpoint: %.*s\n", (int)hm->uri.len, hm->urimg_str);
                mg_http_reply(c, 404, NULL, "Not Found");
            }
            break;
        }

        case MG_EV_WS_MSG: {
            struct mg_ws_message *wm = (struct mg_ws_message *) ev_data;
            handle_ws_message(c, wm);
            break;
        }

        case MG_EV_WS_OPEN:
            printf("[SERVER] WebSocket connection opened\n");
            break;

        case MG_EV_CLOSE:
            if (c->is_websocket) {
                printf("[SERVER] WebSocket connection closed\n");
                remove_client(c);
            }
            break;
    }
}

/* ---------------------------- Main Function ------------------------------ */
int main(void) {
    struct mg_mgr mgr;
    mg_mgr_init(&mgr);
    mg_http_listen(&mgr, s_listen_url, event_handler, NULL);
    printf("[SERVER] Server started on %s\n", s_listen_url);

    while (true) mg_mgr_poll(&mgr, 1000);

    mg_mgr_free(&mgr);
    return 0;
}
