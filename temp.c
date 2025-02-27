#include "mongoose.h"
#include <stdio.h>
#include <json-c/json.h>  // JSON-C Library

static const char *s_listen_url = "http://0.0.0.0:8000";

typedef struct ClientNode {
  char client_id[32];
  struct mg_connection *conn;
  struct ClientNode *next;
} ClientNode;

ClientNode *client_map = NULL;

// Utility function to add a client
void add_client(const char *client_id, struct mg_connection *conn) {
  ClientNode *new_node = malloc(sizeof(ClientNode));
  if (!new_node) {
    printf("[ERROR] Memory allocation failed\n");
    return;
  }
  strncpy(new_node->client_id, client_id, sizeof(new_node->client_id) - 1);
  new_node->client_id[sizeof(new_node->client_id) - 1] = '\0'; // Ensure null-termination
  new_node->conn = conn;
  new_node->next = client_map;
  client_map = new_node;
}

// Utility function to remove a client
void remove_client(struct mg_connection *conn) {
  ClientNode **curr = &client_map;
  while (*curr) {
    if ((*curr)->conn == conn) {
      ClientNode *to_free = *curr;
      *curr = (*curr)->next;
      free(to_free);
      return;
    }
    curr = &((*curr)->next);
  }
}

// Utility function to find a client by `client_id`
struct mg_connection *find_client(const char *client_id) {
  ClientNode *curr = client_map;
  while (curr) {
    if (strcmp(curr->client_id, client_id) == 0) {
      return curr->conn;
    }
    curr = curr->next;
  }
  return NULL;
}

// Function to broadcast alerts to all clients except the scanner
void broadcast_alert(const char *alert_data) {
  ClientNode *curr = client_map;
  while (curr) {
    if (strcmp(curr->client_id, "scanner") != 0) { // Avoid sending back to scanner
      printf("[SERVER] Broadcasting alert to %s: %s\n", curr->client_id, alert_data); // Log the broadcast
      mg_ws_send(curr->conn, alert_data, strlen(alert_data), WEBSOCKET_OP_TEXT);
    }
    curr = curr->next;
  }
}

// Event handler
static void event_handler(struct mg_connection *c, int ev, void *ev_data) {
  if (ev == MG_EV_HTTP_MSG) {
    struct mg_http_message *hm = (struct mg_http_message *) ev_data;

    if (mg_http_match_uri(hm, "/")) {
      mg_http_reply(c, 200, "Content-Type: application/json\r\n", "{\"message\": \"Hello, World!\"}\n");
    }
    else if (mg_http_match_uri(hm, "/ws")) {
      mg_ws_upgrade(c, hm, NULL);
    }
    else if (mg_http_match_uri(hm, "/update-scanner-symbols")) {
      // FMP sends the list of symbols here
      struct json_object *msg = json_tokener_parse(hm->body.ptr);
      struct json_object *client_id_obj, *data_obj;

      if (msg && json_object_object_get_ex(msg, "client_id", &client_id_obj) &&
      json_object_object_get_ex(msg, "data", &data_obj)) {

        const char *client_id = json_object_get_string(client_id_obj);
        const char *data = json_object_to_json_string(data_obj);
        printf("[SERVER] Received symbol update from %s: %s\n", client_id, data);

        // Send symbols **only** to the scanner
        struct mg_connection *scanner_conn = find_client("scanner");
        if (scanner_conn) {
          mg_ws_send(scanner_conn, data, strlen(data), WEBSOCKET_OP_TEXT);
          mg_http_reply(c, 200, "Content-Type: application/json\r\n", "{\"message\": \"Symbols sent to scanner\"}\n");
        } else {
          mg_http_reply(c, 404, "Content-Type: application/json\r\n", "{\"error\": \"Scanner not connected\"}\n");
        }
      } else {
        mg_http_reply(c, 400, "Content-Type: application/json\r\n", "{\"error\": \"Invalid JSON format\"}\n");
      }
      json_object_put(msg);
    }
    else {
      mg_http_reply(c, 404, "", "Not Found\n");
    }
  }
  else if (ev == MG_EV_WS_OPEN) {
    printf("🔌 WebSocket connection established (client: %p)\n", c);
  }
  else if (ev == MG_EV_WS_MSG) {
    struct mg_ws_message *wm = (struct mg_ws_message *) ev_data;
    struct json_object *msg = json_tokener_parse(wm->data.ptr);
    struct json_object *client_id_obj, *alert_data_obj;

    if (msg) {
      printf("[SERVER] Received WebSocket message: %.*s\n", (int)wm->data.len, wm->data.ptr);

      if (json_object_object_get_ex(msg, "client_id", &client_id_obj)) {
        const char *client_id = json_object_get_string(client_id_obj);

        // Register new clients
        if (!find_client(client_id)) {
          add_client(client_id, c);
          printf("[INFO] Registered client ID: %s\n", client_id);
        }

        // If message is from scanner, broadcast it to all clients
        if (strcmp(client_id, "scanner") == 0) {
          if (json_object_object_get_ex(msg, "data", &alert_data_obj)) {
            const char *alert_data = json_object_to_json_string(alert_data_obj);
            printf("[SERVER] Broadcasting alert: %s\n", alert_data);
            broadcast_alert(alert_data);
          }
        }
      }
    } else {
      printf("[SERVER] Failed to parse WebSocket message\n");
    }

    json_object_put(msg);
  }
  else if (ev == MG_EV_CLOSE) {
    if (c->is_websocket) {
      printf("🔌 WebSocket connection closed (client: %p)\n", c);
      remove_client(c);
    }
  }
}

int main(void) {
  struct mg_mgr mgr;
  mg_mgr_init(&mgr);

  mg_http_listen(&mgr, s_listen_url, event_handler, NULL);
  printf("🚀 Server started on %s\n", s_listen_url);

  for (;;) mg_mgr_poll(&mgr, 1);

  mg_mgr_free(&mgr);
  return 0;
}
