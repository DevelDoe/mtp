#include "mtp.h"
#include "mongoose.h"

/* ---------------------------- Configuration ------------------------------ */
static const char *s_listen_url = "http://0.0.0.0:8000";

static const char **stored_symbols = NULL;
static int stored_symbols_count = 0;
static ScannerSymbols *scanner_symbols_list = NULL;

/* ------------------------ Function Prototypes ---------------------------- */
static void distribute_symbols_to_scanners();

/* -------------------------- Client Management ---------------------------- */
typedef struct ClientNode {
  char client_id[32];
  struct mg_connection *conn;
  bool is_scanner; // Track if this client is a scanner
  struct ClientNode *next;
} ClientNode;

static ClientNode *client_map = NULL;

static void add_client(const char *client_id, struct mg_connection *conn,
                       bool is_scanner) {
  ClientNode *node = malloc(sizeof(ClientNode));
  snprintf(node->client_id, sizeof(node->client_id), "%s", client_id);
  node->conn = conn;
  node->is_scanner = is_scanner; // Set the scanner flag
  node->next = client_map;
  client_map = node;

  LOG(LOG_INFO, "Registered client: %s (scanner: %d)", client_id, is_scanner);
}

static void remove_client(struct mg_connection *conn) {
  ClientNode **curr = &client_map;
  bool was_scanner = false; // Track if a scanner was removed

  while (*curr) {
    if ((*curr)->conn == conn) {
      ClientNode *tmp = *curr;
      *curr = (*curr)->next;

      was_scanner = tmp->is_scanner; // ✅ Only trigger if scanner was removed
      LOG(LOG_INFO, "Removed client: %s (scanner: %d)", tmp->client_id,
          tmp->is_scanner);

      free(tmp);
      break;
    }
    curr = &(*curr)->next;
  }

  if (was_scanner) {
    distribute_symbols_to_scanners(); // ✅ Only redistribute if a scanner was
                                      // removed
  }
}

static struct mg_connection *find_client(const char *client_id) {
  ClientNode *curr = client_map;
  while (curr && strcmp(curr->client_id, client_id) != 0)
    curr = curr->next;
  return curr ? curr->conn : NULL;
}

/* ------------------------ WebSocket Utilities ---------------------------- */
static void send_symbols_to_scanner(struct mg_connection *scanner,
                                    const char **symbols, int num_symbols) {
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

  LOG(LOG_INFO, "Sent %d symbols to scanner", num_symbols);
}

static void broadcast_alert(const char *alert_data) {
  LOG(LOG_INFO, "Broadcasting alert to all clients: %s", alert_data);
  for (ClientNode *curr = client_map; curr; curr = curr->next) {
    // Broadcast alert to all clients except scanners
    if (!curr->is_scanner) {
      LOG(LOG_INFO, "Sending alert to client: %s", curr->client_id);
      mg_ws_send(curr->conn, alert_data, strlen(alert_data), WEBSOCKET_OP_TEXT);
    }
  }
}

static void distribute_symbols_to_scanners() {
  if (!stored_symbols || stored_symbols_count == 0) {
    LOG(LOG_WARNING, "No stored symbols to distribute.");
    return;
  }

  int num_scanners = 0;
  for (ClientNode *c = client_map; c; c = c->next) {
    if (c->is_scanner)
      num_scanners++;
  }

  if (num_scanners == 0) {
    LOG(LOG_WARNING, "No scanners connected. Symbols not distributed.");
    return;
  }

  LOG(LOG_INFO, "Distributing %d symbols across %d scanners",
      stored_symbols_count, num_scanners);

  int batch_size = stored_symbols_count / num_scanners;
  int extra = stored_symbols_count % num_scanners;

  int sent = 0;

  for (ClientNode *curr = client_map; curr; curr = curr->next) {
    if (!curr->is_scanner)
      continue;

    int symbols_to_assign = batch_size + (extra > 0 ? 1 : 0);
    if (extra > 0)
      extra--;

    // Allocate memory for the assigned symbols
    char **assigned_symbols = malloc(symbols_to_assign * sizeof(char *));

    for (int i = 0; i < symbols_to_assign; i++) {
      assigned_symbols[i] =
          strdup(stored_symbols[sent + i]); // Copy symbol names
    }

    // Check if scanner already exists in the list
    ScannerSymbols *scanner_entry = scanner_symbols_list;
    ScannerSymbols *prev = NULL;
    while (scanner_entry) {
      if (strcmp(scanner_entry->client_id, curr->client_id) == 0) {
        break; // Scanner already exists
      }
      prev = scanner_entry;
      scanner_entry = scanner_entry->next;
    }

    if (scanner_entry) {
      // Update existing scanner entry
      free(scanner_entry->symbols); // Free old symbols
      scanner_entry->symbols = assigned_symbols;
      scanner_entry->symbol_count = symbols_to_assign;
    } else {
      // Add new scanner entry
      ScannerSymbols *new_entry = malloc(sizeof(ScannerSymbols));
      snprintf(new_entry->client_id, sizeof(new_entry->client_id), "%s",
               curr->client_id);
      new_entry->symbols = assigned_symbols;
      new_entry->symbol_count = symbols_to_assign;
      new_entry->next = NULL;

      if (prev) {
        prev->next = new_entry;
      } else {
        scanner_symbols_list = new_entry;
      }
    }

    // Send symbols to scanner
    send_symbols_to_scanner(curr->conn, (const char **)assigned_symbols,
                            symbols_to_assign);

    sent += symbols_to_assign;
    LOG(LOG_INFO, "Assigned %d symbols to scanner: %s", symbols_to_assign,
        curr->client_id);
  }
}

/* ------------------------- HTTP Handlers ---------------------------------- */
static void handle_root(struct mg_connection *c) {
  LOG(LOG_INFO, "Received request for root endpoint");
  mg_http_reply(c, 200, "Content-Type: application/json\r\n",
                "{\"status\": \"Server is running\"}");
}

static void handle_ws_upgrade(struct mg_connection *c,
                              struct mg_http_message *hm) {
  printf("[SERVER] Upgrading connection to WebSocket\n");

  // Validate WebSocket handshake
  if (mg_http_match_uri(hm, "/ws")) {
    mg_ws_upgrade(c, hm, NULL);
  } else {
    printf("[SERVER] Invalid WebSocket upgrade request\n");
    mg_http_reply(c, 400, NULL, "Invalid WebSocket upgrade request");
  }
}

static void handle_scanner_update(struct mg_connection *c,
                                  struct mg_http_message *hm) {
  LOG(LOG_INFO, "Received symbol update request");

  struct json_object *root = json_tokener_parse(hm->body.ptr);
  struct json_object *client_id_obj, *data_obj;

  if (!root || !json_object_object_get_ex(root, "client_id", &client_id_obj) ||
      !json_object_object_get_ex(root, "data", &data_obj)) {
    LOG(LOG_WARNING, "Invalid symbol update request format");
    mg_http_reply(c, 400, "Content-Type: application/json\r\n",
                  "{\"error\": \"Invalid request format\"}");
    if (root)
      json_object_put(root);
    return;
  }

  const char *client_id = json_object_get_string(client_id_obj);
  struct json_object *symbols_array =
      json_object_object_get(data_obj, "symbols");
  int num_symbols = json_object_array_length(symbols_array);

  LOG(LOG_INFO, "Processing %d symbols from client: %s", num_symbols,
      client_id);

  // Free existing symbols
  if (stored_symbols) {
    for (int i = 0; i < stored_symbols_count; i++)
      free((char *)stored_symbols[i]);
    free(stored_symbols);
  }

  // Copy new symbols with strdup()
  stored_symbols = malloc(num_symbols * sizeof(const char *));
  for (int i = 0; i < num_symbols; i++) {
    struct json_object *item = json_object_array_get_idx(symbols_array, i);
    stored_symbols[i] = strdup(json_object_get_string(item));
  }
  stored_symbols_count = num_symbols;

  LOG(LOG_INFO, "Updated stored symbols. Total: %d", stored_symbols_count);

  // Distribute symbols to scanners
  distribute_symbols_to_scanners();

  mg_http_reply(c, 200, NULL, "Symbols processed and forwarded to scanners");
  json_object_put(root);
}

static void handle_scanners(struct mg_connection *c) {
  struct json_object *root = json_object_new_object();
  struct json_object *scanners_array = json_object_new_array();

  ScannerSymbols *s = scanner_symbols_list;
  while (s) {
    struct json_object *scanner_obj = json_object_new_object();
    json_object_object_add(scanner_obj, "id",
                           json_object_new_string(s->client_id));

    struct json_object *symbols_array = json_object_new_array();
    for (int i = 0; i < s->symbol_count; i++) {
      json_object_array_add(symbols_array,
                            json_object_new_string(s->symbols[i]));
    }

    json_object_object_add(scanner_obj, "symbols", symbols_array);
    json_object_array_add(scanners_array, scanner_obj);
    s = s->next;
  }

  json_object_object_add(root, "scanners", scanners_array);
  const char *response = json_object_to_json_string(root);
  mg_http_reply(c, 200, "Content-Type: application/json\r\n", "%s", response);
  json_object_put(root);
}

static void handle_pool(struct mg_connection *c) {
  struct json_object *root = json_object_new_object();
  struct json_object *symbols_array = json_object_new_array();

  for (int i = 0; i < stored_symbols_count; i++) {
    json_object_array_add(symbols_array,
                          json_object_new_string(stored_symbols[i]));
  }

  json_object_object_add(root, "symbols", symbols_array);
  json_object_object_add(root, "total",
                         json_object_new_int(stored_symbols_count));

  const char *response = json_object_to_json_string(root);
  mg_http_reply(c, 200, "Content-Type: application/json\r\n", "%s", response);
  json_object_put(root);
}

/* ---------------------- WebSocket Handlers ------------------------------- */
static void handle_ws_message(struct mg_connection *c,
                              struct mg_ws_message *wm) {
  LOG(LOG_INFO, "Received WebSocket message: %.*s", (int)wm->data.len,
      wm->data.ptr);

  struct json_object *root = json_tokener_parse(wm->data.ptr);
  if (!root) {
    LOG(LOG_WARNING, "Invalid WebSocket message format");
    return;
  }

  struct json_object *client_id_obj;
  if (!json_object_object_get_ex(root, "client_id", &client_id_obj)) {
    LOG(LOG_WARNING, "Missing client_id in WebSocket message");
    json_object_put(root);
    return;
  }

  const char *client_id = json_object_get_string(client_id_obj);
  bool is_scanner = (strncmp(client_id, "ss", 2) == 0);

  // Register new client if not already registered
  if (!find_client(client_id)) {
    LOG(LOG_INFO, "Registering new client: %s (scanner: %d)", client_id,
        is_scanner);
    add_client(client_id, c, is_scanner);

    if (is_scanner) {
      distribute_symbols_to_scanners(); // ✅ Only distribute if a scanner
                                        // connects
    }
  }

  // Handle scanner alerts
  struct json_object *data_obj;
  if (is_scanner && json_object_object_get_ex(root, "data", &data_obj)) {
    const char *alert = json_object_to_json_string(data_obj);
    LOG(LOG_INFO, "Received alert from scanner: %s", alert);
    broadcast_alert(alert);
  }

  json_object_put(root);
}

/* ------------------------- Event Handling -------------------------------- */
static void event_handler(struct mg_connection *c, int ev, void *ev_data) {
  switch (ev) {
  case MG_EV_HTTP_MSG: {
    struct mg_http_message *hm = (struct mg_http_message *)ev_data;

    if (mg_http_match_uri(hm, "/")) {
      handle_root(c);
    } else if (mg_http_match_uri(hm, "/ws")) {
      handle_ws_upgrade(c, hm);
    } else if (mg_http_match_uri(hm, "/update-scanner-symbols")) {
      handle_scanner_update(c, hm);
    } else if (mg_http_match_uri(hm, "/scanners")) {
      handle_scanners(c); // New route
    } else if (mg_http_match_uri(hm, "/pool")) {
      handle_pool(c); // New route
    } else {
      LOG(LOG_WARNING, "Received request for unknown endpoint: %.*s",
          (int)hm->uri.len, hm->uri.ptr);
      mg_http_reply(c, 404, NULL, "Not Found");
    }
    break;
  }

  case MG_EV_WS_MSG: {
    struct mg_ws_message *wm = (struct mg_ws_message *)ev_data;
    if (wm->flags & WEBSOCKET_OP_TEXT) {
      handle_ws_message(c, wm);
    } else {
      LOG(LOG_WARNING, "Received non-text WebSocket frame (opcode: %d)",
          wm->flags & 0x0F);
    }
    break;
  }

  case MG_EV_WS_OPEN:
    LOG(LOG_INFO, "WebSocket connection opened");
    break;

  case MG_EV_CLOSE:
    if (c->is_websocket) {
      LOG(LOG_INFO, "WebSocket connection closed");
      remove_client(c);
    }
    break;
  }
}

/* ---------------------------- Main Function ------------------------------ */
int main(void) {
  LOG(LOG_NOTICE, "Starting WebSocket Server on %s", s_listen_url);

  struct mg_mgr mgr;
  mg_mgr_init(&mgr);
  mg_http_listen(&mgr, s_listen_url, event_handler, NULL);

  LOG(LOG_NOTICE, "Server started successfully. Listening for connections...");

  while (true) {
    mg_mgr_poll(&mgr, 1000);
  }

  mg_mgr_free(&mgr);
  LOG(LOG_NOTICE, "Server shutting down.");
  return 0;
}
