#include "mongoose.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <json-c/json.h>

/* ---------------------------- Configuration ------------------------------ */
static const char *s_listen_url = "http://0.0.0.0:8000";
#define MAX_SYMBOLS_PER_BATCH 50

/* --------------------- Global Variables for Symbols ---------------------- */
static const char **all_symbols = NULL;
static int total_symbols = 0;

/* -------------------------- Scanner Management --------------------------- */
typedef struct ScannerNode {
    char client_id[32];
    struct mg_connection *conn;
    struct ScannerNode *next;
} ScannerNode;

static ScannerNode *scanner_list = NULL;

static void remove_scanner(struct mg_connection *conn) {
    ScannerNode **curr = &scanner_list;
    while (*curr) {
        if ((*curr)->conn == conn) {
            ScannerNode *tmp = *curr;
            *curr = (*curr)->next;
            free(tmp);
            printf("[SERVER] Removed scanner\n");
            return;
        }
        curr = &(*curr)->next;
    }
}

static void add_scanner(const char *client_id, struct mg_connection *conn) {
    ScannerNode *curr = scanner_list;

    // Check if scanner already exists
    while (curr) {
        if (strcmp(curr->client_id, client_id) == 0) {
            // Existing scanner reconnecting
            curr->conn = conn;
            printf("[SERVER] Scanner %s reconnected\n", client_id);
            return;
        }
        curr = curr->next;
    }

    // New scanner, add to list
    ScannerNode *new_scanner = (ScannerNode *)malloc(sizeof(ScannerNode));
    snprintf(new_scanner->client_id, sizeof(new_scanner->client_id), "%s", client_id);
    new_scanner->conn = conn;
    new_scanner->next = scanner_list;
    scanner_list = new_scanner;
    printf("[SERVER] Registered new scanner: %s\n", client_id);

    // Reassign symbols to all scanners
    if (total_symbols > 0) {
        distribute_symbols_to_scanners();
    }
}

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

/* ---------------------- WebSocket Symbol Distribution -------------------- */
static void send_symbols_to_scanner(struct mg_connection *scanner, const char **symbols, int num_symbols) {
    struct json_object *root = json_object_new_object();
    struct json_object *symbols_array = json_object_new_array();

    for (int i = 0; i < num_symbols; i++) {
        json_object_array_add(symbols_array, json_object_new_string(symbols[i]));
    }

    json_object_object_add(root, "symbols", symbols_array);
    const char *data = json_object_to_json_string(root);

    mg_ws_send(scanner, data, strlen(data), WEBSOCKET_OP_TEXT);
    json_object_put(root);
}

static void distribute_symbols_to_scanners() {
    ScannerNode *curr = scanner_list;
    int symbol_index = 0;

    if (!curr || total_symbols == 0) {
        printf("[SERVER] No scanners available or no symbols to assign.\n");
        return;
    }

    printf("[SERVER] Reassigning symbols to all scanners\n");

    while (curr && symbol_index < total_symbols) {
        int batch_size = (total_symbols - symbol_index < MAX_SYMBOLS_PER_BATCH)
                         ? (total_symbols - symbol_index)
                         : MAX_SYMBOLS_PER_BATCH;

        send_symbols_to_scanner(curr->conn, &all_symbols[symbol_index], batch_size);
        symbol_index += batch_size;
        curr = curr->next;  // Move to next scanner
    }

    printf("[SERVER] All symbols assigned successfully.\n");
}

/* ------------------------- HTTP Handlers ---------------------------------- */
static void handle_scanner_update(struct mg_connection *c, struct mg_http_message *hm) {
    printf("[SERVER] Received symbol update request\n");

    struct json_object *root = json_tokener_parse(hm->body.ptr);
    struct json_object *symbols_array;

    if (!root || !json_object_object_get_ex(root, "symbols", &symbols_array)) {
        printf("[SERVER] Invalid symbol update request format\n");
        mg_http_reply(c, 400, "Content-Type: application/json\r\n", "{\"error\": \"Invalid request format\"}");
        json_object_put(root);
        return;
    }

    int num_symbols = json_object_array_length(symbols_array);
    all_symbols = realloc(all_symbols, num_symbols * sizeof(char *));
    total_symbols = num_symbols;

    for (int i = 0; i < num_symbols; i++) {
        all_symbols[i] = json_object_get_string(json_object_array_get_idx(symbols_array, i));
    }

    printf("[SERVER] Updated symbol list. Total: %d\n", total_symbols);
    distribute_symbols_to_scanners();
    json_object_put(root);
    mg_http_reply(c, 200, NULL, "Symbols processed and forwarded to scanners");
}

/* ---------------------- WebSocket Handlers ------------------------------- */
static void handle_ws_message(struct mg_connection *c, struct mg_ws_message *wm) {
    printf("[SERVER] Received WebSocket message: %.*s\n", (int)wm->data.len, wm->data.ptr);

    struct json_object *root = json_tokener_parse(wm->data.ptr);
    struct json_object *client_id_obj;

    if (!root || !json_object_object_get_ex(root, "client_id", &client_id_obj)) {
        printf("[SERVER] Invalid WebSocket message format\n");
        return;
    }

    const char *client_id = json_object_get_string(client_id_obj);

    if (strncmp(client_id, "scanner", 7) == 0) {
        printf("[SERVER] Registering new scanner: %s\n", client_id);
        add_scanner(client_id, c);
    } else {
        if (!find_client(client_id)) {
            printf("[SERVER] Registering new client: %s\n", client_id);
            add_client(client_id, c);
        }
    }

    json_object_put(root);
}

/* ------------------------- Event Handling -------------------------------- */
static void event_handler(struct mg_connection *c, int ev, void *ev_data) {
    switch (ev) {
        case MG_EV_HTTP_MSG: {
            struct mg_http_message *hm = (struct mg_http_message *)ev_data;

            if (mg_http_match_uri(hm, "/update-scanner-symbols")) {
                handle_scanner_update(c, hm);
            }
            break;
        }

        case MG_EV_WS_MSG: {
            struct mg_ws_message *wm = (struct mg_ws_message *)ev_data;
            handle_ws_message(c, wm);
            break;
        }

        case MG_EV_CLOSE:
            if (c->is_websocket) {
                remove_client(c);
                remove_scanner(c);
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
