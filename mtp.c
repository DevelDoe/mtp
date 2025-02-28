#include "mongoose.h"
#include <stdio.h>
#include <json-c/json.h>
#include <time.h>  // Include for time functions

/* ---------------------------- Configuration ------------------------------ */

static const char *s_listen_url = "http://0.0.0.0:8000";
#define SCANNER_TIMEOUT 60  // 60 seconds before removing inactive scanners

/* -------------------------- Client Management ---------------------------- */

typedef struct ClientNode {
    char client_id[32];
    struct mg_connection *conn;
    int is_scanner;
    time_t last_seen;  // New field for heartbeat tracking
    struct ClientNode *next;
} ClientNode;

static ClientNode *client_map = NULL;

/* Function Prototypes */
static ClientNode *find_client(const char *client_id);

/* -------------------------- Function Definitions ------------------------- */

static ClientNode *find_client(const char *client_id) {
    ClientNode *curr = client_map;
    while (curr) {
        if (strcmp(curr->client_id, client_id) == 0) {
            return curr;
        }
        curr = curr->next;
    }
    return NULL;
}

static void add_client(const char *client_id, struct mg_connection *conn) {
    if (find_client(client_id)) {
        printf("[SERVER] Client %s is already registered.\n", client_id);
        return;
    }

    ClientNode *node = malloc(sizeof(ClientNode));
    snprintf(node->client_id, sizeof(node->client_id), "%s", client_id);
    node->conn = conn;
    node->is_scanner = strncmp(client_id, "ss", 2) == 0 ? 1 : 0;
    node->last_seen = time(NULL);
    node->next = client_map;
    client_map = node;
    printf("[SERVER] Registered client: %s (Scanner: %d)\n", client_id, node->is_scanner);
}

static void remove_client(struct mg_connection *conn) {
    ClientNode **curr = &client_map;
    while (*curr) {
        if ((*curr)->conn == conn) {
            ClientNode *tmp = *curr;
            *curr = (*curr)->next;

            if (tmp->conn) {
                mg_ws_send(tmp->conn, "{\"error\":\"Client disconnected\"}", 30, WEBSOCKET_OP_TEXT);
                mg_close_conn(tmp->conn);  // Proper way to close a connection
                tmp->conn = NULL;
            }

            free(tmp);
            printf("[SERVER] Removed client\n");
            return;
        }
        curr = &(*curr)->next;
    }
}

static void remove_inactive_scanners() {
    time_t now = time(NULL);
    ClientNode **curr = &client_map;

    while (*curr) {
        if ((*curr)->is_scanner && (now - (*curr)->last_seen > SCANNER_TIMEOUT)) {
            printf("[SERVER] Removing inactive scanner: %s\n", (*curr)->client_id);

            ClientNode *tmp = *curr;
            *curr = (*curr)->next;

            if (tmp->conn) {
                mg_ws_send(tmp->conn, "{\"error\":\"Scanner timeout\"}", 27, WEBSOCKET_OP_TEXT);
                mg_close_conn(tmp->conn);
                tmp->conn = NULL;
            }

            free(tmp);
        } else {
            curr = &(*curr)->next;
        }
    }
}

/* ------------------------- HTTP Handlers ---------------------------------- */

static void handle_scanner_update(struct mg_connection *c, struct mg_http_message *hm) {
    printf("[SERVER] Received symbol update request\n");

    struct json_object *root = json_tokener_parse(hm->body.ptr);
    struct json_object *client_id_obj, *symbols_array;

    if (!root || !json_object_object_get_ex(root, "client_id", &client_id_obj) ||
               !json_object_object_get_ex(root, "symbols", &symbols_array)) {
        printf("[SERVER] Invalid symbol update request format\n");
        mg_http_reply(c, 400, "Content-Type: application/json\r\n",
                     "{\"error\": \"Invalid request format\"}");
        json_object_put(root);
        return;
    }

    const char *client_id = json_object_get_string(client_id_obj);
    int num_symbols = json_object_array_length(symbols_array);

    printf("[SERVER] Received %d symbols from %s\n", num_symbols, client_id);

    if (num_symbols == 0) {
        printf("[SERVER] No symbols to assign\n");
        mg_http_reply(c, 200, NULL, "No symbols to assign.");
        json_object_put(root);
        return;
    }

    ClientNode *scanner_node = find_client("ss1");
    struct mg_connection *scanner = scanner_node ? scanner_node->conn : NULL;

    if (!scanner) {
        printf("[SERVER] No active scanners available\n");
        mg_http_reply(c, 503, "Content-Type: application/json\r\n",
                     "{\"error\": \"No active scanner shards available\"}");
        json_object_put(root);
        return;
    }

    printf("[SERVER] Sending symbols to scanner\n");
    mg_ws_send(scanner, json_object_to_json_string(symbols_array),
               strlen(json_object_to_json_string(symbols_array)), WEBSOCKET_OP_TEXT);

    mg_http_reply(c, 200, NULL, "Symbols distributed.");
    json_object_put(root);
}

/* ------------------------- WebSocket Handlers ---------------------------- */

static void handle_ws_message(struct mg_connection *c, struct mg_ws_message *wm) {
    printf("[SERVER] Received WebSocket message: %.*s\n", (int)wm->data.len, wm->data.ptr);

    struct json_object *root = json_tokener_parse(wm->data.ptr);
    struct json_object *client_id_obj, *type_obj;

    if (!root || !json_object_object_get_ex(root, "client_id", &client_id_obj)) {
        printf("[SERVER] Invalid WebSocket message format\n");
        json_object_put(root);
        return;
    }

    const char *client_id = json_object_get_string(client_id_obj);
    ClientNode *client = find_client(client_id);

    if (!client) {
        printf("[SERVER] Registering new WebSocket client: %s\n", client_id);
        add_client(client_id, c);
        client = find_client(client_id);
    }

    // Handle heartbeat messages
    if (json_object_object_get_ex(root, "type", &type_obj)) {
        const char *type = json_object_get_string(type_obj);
        if (strcmp(type, "heartbeat") == 0) {
            client->last_seen = time(NULL);
            printf("[SERVER] Heartbeat received from %s\n", client_id);
        }
    }

    json_object_put(root);
}

/* ------------------------- Event Handling -------------------------------- */

static void event_handler(struct mg_connection *c, int ev, void *ev_data) {
    switch (ev) {
        case MG_EV_HTTP_MSG: {
            struct mg_http_message *hm = (struct mg_http_message *) ev_data;
            if (mg_http_match_uri(hm, "/update-scanner-symbols")) {
                handle_scanner_update(c, hm);
            } else {
                mg_http_reply(c, 404, NULL, "Not Found");
            }
            break;
        }
        case MG_EV_WS_OPEN:
            printf("[SERVER] WebSocket connection opened\n");
            break;
        case MG_EV_WS_MSG:
            handle_ws_message(c, (struct mg_ws_message *) ev_data);
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
    struct mg_connection *conn = mg_http_listen(&mgr, s_listen_url, event_handler, NULL);
    if (conn == NULL) {
        printf("[SERVER] Failed to start server on %s\n", s_listen_url);
        return 1;
    }
    printf("[SERVER] Server started on %s\n", s_listen_url);

    while (true) {
        mg_mgr_poll(&mgr, 1000);
        remove_inactive_scanners();
    }

    mg_mgr_free(&mgr);
    return 0;
}
