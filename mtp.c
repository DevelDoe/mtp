#inclue <mtp.h?

/* ---------------------------- Configuration ------------------------------ */
static const char *s_listen_url = "http://0.0.0.0:8000";

static const char **stored_symbols = NULL;
static int stored_symbols_count = 0;

/* ------------------------ Function Prototypes ---------------------------- */
static void distribute_symbols_to_scanners();

/* -------------------------- Client Management ---------------------------- */
typedef struct ClientNode {
    char client_id[32];
    struct mg_connection *conn;
    bool is_scanner;  // Track if this client is a scanner
    struct ClientNode *next;
} ClientNode;

static ClientNode *client_map = NULL;

static void add_client(const char *client_id, struct mg_connection *conn, bool is_scanner) {
    ClientNode *node = malloc(sizeof(ClientNode));
    snprintf(node->client_id, sizeof(node->client_id), "%s", client_id);
    node->conn = conn;
    node->is_scanner = is_scanner;  // Set the scanner flag
    node->next = client_map;
    client_map = node;
    printf("[SERVER] Registered client: %s (scanner: %d)\n", client_id, is_scanner);
}

static void remove_client(struct mg_connection *conn) {
    ClientNode **curr = &client_map;
    while (*curr) {
        if ((*curr)->conn == conn) {
            ClientNode *tmp = *curr;
            *curr = (*curr)->next;
            printf("[SERVER] Removed client: %s (scanner: %d)\n", tmp->client_id, tmp->is_scanner);
            free(tmp);

            // Redistribute symbols if a scanner was removed
            distribute_symbols_to_scanners();
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
        // Broadcast alert to all clients except scanners
        if (!curr->is_scanner) {
            printf("[SERVER] Sending alert to client: %s\n", curr->client_id);
            mg_ws_send(curr->conn, alert_data, strlen(alert_data), WEBSOCKET_OP_TEXT);
        }
    }
}

static void distribute_symbols_to_scanners() {
    if (!stored_symbols || stored_symbols_count == 0) return;

    int total = stored_symbols_count;
    int num_scanners = 0;

    // Count active scanners
    for (ClientNode *c = client_map; c; c = c->next) {
        if (c->is_scanner) num_scanners++;
    }

    if (num_scanners == 0) {
        printf("[SERVER] No scanners connected. Symbols not distributed.\n");
        return;
    }

    int max_symbols = num_scanners * MAX_SYMBOLS;
    int to_send = (total < max_symbols) ? total : max_symbols;

    int sent = 0;
    ClientNode *curr = client_map;
    while (sent < to_send && curr) {
        if (!curr->is_scanner) {
            curr = curr->next;
            continue;
        }

        int batch_size = (to_send - sent > MAX_SYMBOLS)
                         ? MAX_SYMBOLS : (to_send - sent);
        send_symbols_to_scanner(curr->conn, &stored_symbols[sent], batch_size);
        sent += batch_size;
        curr = curr->next;
    }

    printf("[SERVER] Distributed %d symbols to %d scanners\n", sent, num_scanners);
}

/* ------------------------- HTTP Handlers ---------------------------------- */
static void handle_root(struct mg_connection *c) {
    printf("[SERVER] Received request for root endpoint\n");
    mg_http_reply(c, 200, "Content-Type: application/json\r\n", "{\"status\": \"Server is running\"}");
}

static void handle_ws_upgrade(struct mg_connection *c, struct mg_http_message *hm) {
    printf("[SERVER] Upgrading connection to WebSocket\n");

    // Validate WebSocket handshake
    if (mg_http_match_uri(hm, "/ws")) {
        mg_ws_upgrade(c, hm, NULL);
    } else {
        printf("[SERVER] Invalid WebSocket upgrade request\n");
        mg_http_reply(c, 400, NULL, "Invalid WebSocket upgrade request");
    }
}

static void handle_scanner_update(struct mg_connection *c, struct mg_http_message *hm) {
    printf("[SERVER] Received symbol update request\n");

    struct json_object *root = json_tokener_parse(hm->body.ptr);
    struct json_object *client_id_obj, *data_obj;

    if (!root || !json_object_object_get_ex(root, "client_id", &client_id_obj) ||
               !json_object_object_get_ex(root, "data", &data_obj)) {
        printf("[SERVER] Invalid symbol update request format\n");
        mg_http_reply(c, 400, "Content-Type: application/json\r\n", "{\"error\": \"Invalid request format\"}");
        if (root) json_object_put(root);
        return;
    }

    const char *client_id = json_object_get_string(client_id_obj);
    struct json_object *symbols_array = json_object_object_get(data_obj, "symbols");
    int num_symbols = json_object_array_length(symbols_array);

    // Log the symbols being processed
    printf("[SERVER] Processing %d symbols:\n", num_symbols);
    for (int i = 0; i < num_symbols; i++) {
        struct json_object *item = json_object_array_get_idx(symbols_array, i);
        const char *symbol = json_object_get_string(item);
        printf("  - %s\n", symbol);
    }

    // Free existing symbols
    if (stored_symbols) {
        for (int i = 0; i < stored_symbols_count; i++) free((char *)stored_symbols[i]);
        free(stored_symbols);
    }

    // Copy new symbols with strdup()
    stored_symbols = malloc(num_symbols * sizeof(const char *));
    for (int i = 0; i < num_symbols; i++) {
        struct json_object *item = json_object_array_get_idx(symbols_array, i);
        stored_symbols[i] = strdup(json_object_get_string(item));
    }
    stored_symbols_count = num_symbols;

    // Distribute symbols to scanners
    distribute_symbols_to_scanners();

    mg_http_reply(c, 200, NULL, "Symbols processed and forwarded to scanners");
    json_object_put(root);
}

/* ---------------------- WebSocket Handlers ------------------------------- */
static void handle_ws_message(struct mg_connection *c, struct mg_ws_message *wm) {
    printf("[SERVER] Received WebSocket message: %.*s\n", (int)wm->data.len, wm->data.ptr);

    struct json_object *root = json_tokener_parse(wm->data.ptr);
    if (!root) {
        printf("[SERVER] Invalid WebSocket message format\n");
        return;
    }

    struct json_object *client_id_obj;
    if (!json_object_object_get_ex(root, "client_id", &client_id_obj)) {
        printf("[SERVER] Missing client_id in WebSocket message\n");
        json_object_put(root);
        return;
    }

    const char *client_id = json_object_get_string(client_id_obj);

    // Determine if client is a scanner (e.g., client_id starts with "ss")
    bool is_scanner = (strncmp(client_id, "ss", 2) == 0);

    // Register new client if not already registered
    if (!find_client(client_id)) {
        printf("[SERVER] Registering new client: %s (scanner: %d)\n", client_id, is_scanner);
        add_client(client_id, c, is_scanner);

        // Redistribute symbols if this is a new scanner
        if (is_scanner) {
            distribute_symbols_to_scanners();
        }
    }

    // Handle scanner alerts
    if (is_scanner) {
        struct json_object *data_obj;
        if (json_object_object_get_ex(root, "data", &data_obj)) {
            const char *alert = json_object_to_json_string(data_obj);
            printf("[SERVER] Received alert from scanner: %s\n", alert);
            broadcast_alert(alert);  // Broadcast the alert to all clients
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
            if (wm->flags & WEBSOCKET_OP_TEXT) {
                handle_ws_message(c, wm);
            } else {
                printf("[SERVER] Received non-text WebSocket frame (opcode: %d)\n", wm->flags & 0x0F);
            }
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
