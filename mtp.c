#include "mongoose.h"
#include <stdio.h>
#include <json-c/json.h>

/* ---------------------------- Configuration ------------------------------ */
static const char *s_listen_url = "http://0.0.0.0:8000";
#define MAX_SYMBOLS_PER_BATCH 50

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
static void send_symbols_to_scanner(struct mg_connection *scanner, const char **symbols, int num_symbols) {
    // Create a JSON object containing the symbols
    struct json_object *root = json_object_new_object();
    struct json_object *symbols_array = json_object_new_array();

    for (int i = 0; i < num_symbols; i++) {
        json_object_array_add(symbols_array, json_object_new_string(symbols[i]));
    }

    json_object_object_add(root, "symbols", symbols_array);
    const char *data = json_object_to_json_string(root);

    // Send the symbols to the scanner
    mg_ws_send(scanner, data, strlen(data), WEBSOCKET_OP_TEXT);
    json_object_put(root);
}

static void broadcast_alert(const char *alert_data) {
    printf("[SERVER] Broadcasting alert to all clients: %s\n", alert_data);
    for (ClientNode *curr = client_map; curr; curr = curr->next) {
        // Broadcast alert to all clients except for the scanner
        if (strcmp(curr->client_id, "scanner") != 0) {
            printf("[SERVER] Sending alert to client: %s\n", curr->client_id);
            mg_ws_send(curr->conn, alert_data, strlen(alert_data), WEBSOCKET_OP_TEXT);
        }
    }
}

static void distribute_symbols_to_scanners(const char **symbols, int total_symbols) {
    ScannerNode *curr = scanner_list;
    int symbol_index = 0;

    printf("[SERVER] Reassigning symbols to all scanners\n");

    while (curr && symbol_index < total_symbols) {
        int batch_size = (total_symbols - symbol_index < MAX_SYMBOLS_PER_BATCH)
                         ? (total_symbols - symbol_index)
                         : MAX_SYMBOLS_PER_BATCH;

        assign_symbols_to_scanner(curr, &symbols[symbol_index], batch_size);
        symbol_index += batch_size;
        curr = curr->next;  // Move to next scanner
    }

    if (symbol_index < total_symbols) {
        printf("[SERVER] Not enough scanners for all symbols. %d symbols unassigned.\n",
               total_symbols - symbol_index);
    } else {
        printf("[SERVER] All symbols assigned successfully.\n");
    }
}


/* ------------------------- HTTP Handlers ---------------------------------- */
static void handle_root(struct mg_connection *c) {
    printf("[SERVER] Received request for root endpoint\n");
    mg_http_reply(c, 200, "Content-Type: application/json\r\n", "{\"status\": \"Server is running\"}");
}

static void handle_ws_upgrade(struct mg_connection *c, struct mg_http_message *hm) {
    printf("[SERVER] Upgrading connection to WebSocket\n");
    mg_ws_upgrade(c, hm, NULL);
}

static void handle_scanner_update(struct mg_connection *c, struct mg_http_message *hm) {
    printf("[SERVER] Received symbol update request\n");

    struct json_object *root = json_tokener_parse(hm->body.ptr);
    struct json_object *client_id_obj, *data_obj;

    if (!root || !json_object_object_get_ex(root, "client_id", &client_id_obj) ||
               !json_object_object_get_ex(root, "data", &data_obj)) {
        printf("[SERVER] Invalid symbol update request format\n");
        mg_http_reply(c, 400, "Content-Type: application/json\r\n", "{\"error\": \"Invalid request format\"}");
        json_object_put(root);
        return;
    }

    const char *client_id = json_object_get_string(client_id_obj);
    const char *data = json_object_to_json_string(data_obj);

    printf("[SERVER] Received symbols from client %s: %s\n", client_id, data);

    // Convert symbols into a list of strings (you may use your own format)
    struct json_object *symbols_array = json_object_object_get(data_obj, "symbols");
    int num_symbols = json_object_array_length(symbols_array);
    const char *symbols[num_symbols];

    for (int i = 0; i < num_symbols; i++) {
        symbols[i] = json_object_get_string(json_object_array_get_idx(symbols_array, i));
    }

    // Distribute symbols to scanners
    distribute_symbols_to_scanners(symbols, num_symbols);

    mg_http_reply(c, 200, NULL, "Symbols processed and forwarded to scanners");
    json_object_put(root);
}

/* ---------------------- WebSocket Handlers ------------------------------- */
static void handle_ws_message(struct mg_connection *c, struct mg_ws_message *wm) {
    printf("[SERVER] Received WebSocket message: %.*s\n", (int)wm->data.len, wm->data.ptr);

    struct json_object *root = json_tokener_parse(wm->data.ptr);
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
    if (strcmp(client_id, "scanner") == 0 && json_object_object_get_ex(root, "data", &data_obj)) {
        const char *alert = json_object_to_json_string(data_obj);
        printf("[SERVER] Received alert from scanner: %s\n", alert);
        broadcast_alert(alert);  // Broadcast the alert to all clients
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

    while (true) mg_mgr_poll(&mgr, 1000);

    mg_mgr_free(&mgr);
    return 0;
}
