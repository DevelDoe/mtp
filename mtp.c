#include "mongoose.h"
#include <stdio.h>
#include <json-c/json.h>
#include <time.h>  // Include for time functions

/* ---------------------------- Configuration ------------------------------ */

static const char *s_listen_url = "http://0.0.0.0:8000";

#define SCANNER_TIMEOUT 60  // 10 seconds before removing inactive scanners


/* -------------------------- Client Management ---------------------------- */

typedef struct ClientNode {
    char client_id[32];
    struct mg_connection *conn;
    int is_scanner;
    time_t last_seen;  // New field for heartbeat tracking
    struct ClientNode *next;
} ClientNode;


static ClientNode *client_map = NULL;

static void add_client(const char *client_id, struct mg_connection *conn) {
    // Check if client is already registered
    if (find_client(client_id)) {
        printf("[SERVER] Client %s is already registered.\n", client_id);
        return;
    }

    ClientNode *node = malloc(sizeof(ClientNode));
    snprintf(node->client_id, sizeof(node->client_id), "%s", client_id);
    node->conn = conn;
    node->is_scanner = strncmp(client_id, "ss", 2) == 0 ? 1 : 0;
    node->last_seen = time(NULL);  // Initialize heartbeat timestamp
    node->next = client_map;
    client_map = node;
    printf("[SERVER] Registered client: %s (Scanner: %d)\n", client_id, node->is_scanner);
}

static ClientNode *find_least_used_scanner() {
    ClientNode *selected = NULL;
    int min_load = 50; // Maximum load per scanner

    for (ClientNode *curr = client_map; curr; curr = curr->next) {
        if (curr->is_scanner) {
            int load = json_object_array_length(curr->conn); // Symbols assigned
            if (load < min_load) {
                min_load = load;
                selected = curr;
            }
        }
    }
    return selected;
}

static void remove_client(struct mg_connection *conn) {
    ClientNode **curr = &client_map;
    while (*curr) {
        if ((*curr)->conn == conn) {
            ClientNode *tmp = *curr;
            *curr = (*curr)->next;

            if (tmp->conn) {
                mg_ws_send(tmp->conn, "{\"error\":\"Client disconnected\"}", 30, WEBSOCKET_OP_TEXT);
                mg_ws_close(tmp->conn);
                mg_mgr_remove_conn(tmp->conn);
                tmp->conn = NULL;
            }

            free(tmp);
            printf("[SERVER] Removed client\n");
            return;
        }
        curr = &(*curr)->next;
    }
}

static struct mg_connection *find_scanner(const char *scanner_id) {
    ClientNode *curr = client_map;
    while (curr) {
        if (strncmp(curr->client_id, scanner_id, strlen(scanner_id)) == 0) {
            return curr->conn;
        }
        curr = curr->next;
    }
    return NULL;
}

static void remove_inactive_scanners() {
    time_t now = time(NULL);
    ClientNode **curr = &client_map;

    while (*curr) {
        if ((*curr)->is_scanner && (now - (*curr)->last_seen > SCANNER_TIMEOUT)) {
            printf("[SERVER] Removing inactive scanner: %s\n", (*curr)->client_id);

            // Double-check before removal (ensure it's still inactive)
            if (find_client((*curr)->client_id)) {
                ClientNode *tmp = *curr;
                *curr = (*curr)->next;

                if (tmp->conn) {
                    mg_ws_send(tmp->conn, "{\"error\":\"Scanner timeout\"}", 27, WEBSOCKET_OP_TEXT);
                    mg_ws_close(tmp->conn);
                    mg_mgr_remove_conn(tmp->conn);
                    tmp->conn = NULL;
                }

                free(tmp);
            }
        } else {
            curr = &(*curr)->next;
        }
    }
}


/* ------------------------ WebSocket Utilities ---------------------------- */

static void broadcast_alert(const char *alert_data) {
    printf("[SERVER] Broadcasting alert: %s\n", alert_data);
    for (ClientNode *curr = client_map; curr; curr = curr->next) {
        if (curr->is_scanner) {
            printf("[SERVER] Sending alert to %s\n", curr->client_id);
            if (mg_ws_send(curr->conn, alert_data, strlen(alert_data), WEBSOCKET_OP_TEXT) <= 0) {
                printf("[SERVER] Failed to send alert to %s. Removing client.\n", curr->client_id);
                remove_client(curr->conn);
            }
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

    // Get all active scanner shards (ss1, ss2, ss3, ...)
    ClientNode *scanners[10];  // Assume max 10 scanners
    int scanner_count = 0;

    for (ClientNode *curr = client_map; curr; curr = curr->next) {
        if (curr->is_scanner) {
            scanners[scanner_count++] = curr;
        }
    }

    if (scanner_count == 0) {
        printf("[SERVER] No active scanners available\n");
        mg_http_reply(c, 503, "Content-Type: application/json\r\n",
                     "{\"error\": \"No active scanner shards available\"}");
        json_object_put(root);
        return;
    }

    printf("[SERVER] Distributing %d symbols among %d scanners\n", num_symbols, scanner_count);

    int assigned = 0;
    int scanner_index = 0;

    while (assigned < num_symbols && scanner_index < scanner_count) {
        struct json_object *batch = json_object_new_array();
        ClientNode *target_scanner = scanners[scanner_index];

        for (int j = 0; j < 50 && assigned < num_symbols; j++, assigned++) {
            struct json_object *symbol = json_object_array_get_idx(symbols_array, assigned);
            json_object_array_add(batch, json_object_get(symbol));
        }

        const char *batch_data = json_object_to_json_string(batch);
        printf("[SERVER] Assigning %d symbols to %s: %s\n",
               json_object_array_length(batch), target_scanner->client_id, batch_data);

        mg_ws_send(target_scanner->conn, batch_data, strlen(batch_data), WEBSOCKET_OP_TEXT);
        json_object_put(batch);

        scanner_index++;  // Move to the next scanner
    }

    // If there are remaining symbols after all scanners are full, discard them
    if (assigned < num_symbols) {
        printf("[SERVER] Ignoring %d symbols since all scanners are full.\n", num_symbols - assigned);
    }

    mg_http_reply(c, 200, NULL, "Symbols distributed.");
    json_object_put(root);
}


/* ---------------------- WebSocket Handlers ------------------------------- */
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
        printf("[SERVER] Registering new client: %s\n", client_id);
        add_client(client_id, c);
        client = find_client(client_id);
    }

    // Handle heartbeat messages
    if (json_object_object_get_ex(root, "type", &type_obj)) {
        const char *type = json_object_get_string(type_obj);
        if (strcmp(type, "heartbeat") == 0) {
            client->last_seen = time(NULL);  // Update last seen time
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

            if (mg_http_match_uri(hm, "/")) {
                handle_root(c);
            } else if (mg_http_match_uri(hm, "/ws")) {
                handle_ws_upgrade(c, hm);
            } else if (mg_http_match_uri(hm, "/update-scanner-symbols")) {
                handle_scanner_update(c, hm);
            } else {
                printf("[SERVER] Received request for unknown endpoint: %.*s\n", (int)hm->uri.len, hm->uri.ptr);
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

    while (true) {
        mg_mgr_poll(&mgr, 1000);  // Run event loop
        remove_inactive_scanners();  // Cleanup inactive scanners
    }

    mg_mgr_free(&mgr);
    return 0;
}
