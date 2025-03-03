#include "mtp.h"
/* ---------------------- Function Declarations ---------------------- */

// Queue functions
static int trade_queue_empty(TradeQueue *q);
static int trade_queue_full(TradeQueue *q);
static void queue_push_trade(TradeQueue *q, TradeMsg *trade);
static void queue_pop_trade(TradeQueue *q, TradeMsg *trade);
static int alert_queue_empty(AlertQueue *q);
static int alert_queue_full(AlertQueue *q);
static void queue_push_alert(AlertQueue *q, AlertMsg *alert);
static void queue_pop_alert(AlertQueue *q, AlertMsg *alert);

// WebSocket Handlers
static int handle_local_server_connection(ScannerState *state);
static int handle_finnhub_connection(ScannerState *state);

// Trade & Alert processing
static void send_alert(ScannerState *state, int symbol_idx, double change, double price, int volume);
static void enqueue_trade(ScannerState *state, const char *symbol, double price, int volume);

// WebSocket callbacks
static int local_server_callback(struct lws *wsi, enum lws_callback_reasons reason, void *user, void *in, size_t len);
static int finnhub_callback(struct lws *wsi, enum lws_callback_reasons reason, void *user, void *in, size_t len);

// Worker threads
static void* trade_processing_thread(void *lpParam);
static void* alert_sending_thread(void *lpParam);

/* ----------------------------- Utility functions ------------------------------ */
unsigned long get_current_time_ms() {
    struct timespec ts;
    // You can use CLOCK_REALTIME or CLOCK_MONOTONIC depending on your needs.
    clock_gettime(CLOCK_REALTIME, &ts);
    return ts.tv_sec * 1000UL + ts.tv_nsec / 1000000UL;
}
void log_message(int level, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vsyslog(level, fmt, args);
    va_end(args);
}
/* ----------------------------- Queue Functions ---------------------------- */
// TradeQueue functions
static int trade_queue_empty(TradeQueue *q) { return q->head == q->tail; }
static int trade_queue_full(TradeQueue *q) { return ((q->tail + 1) % MAX_QUEUE_SIZE) == q->head; }
static void queue_push_trade(TradeQueue *q, TradeMsg *trade) {
    if (trade_queue_full(q)) {
      LOG(LOG_WARNING, "Trade queue full, dropping trade for %s at %.2f\n",
          trade->symbol, trade->price);
      return;
    }
    q->trades[q->tail] = *trade;
    q->tail = (q->tail + 1) % MAX_QUEUE_SIZE;
}
static void queue_pop_trade(TradeQueue *q, TradeMsg *trade) {
    if (trade_queue_empty(q)) return;
    *trade = q->trades[q->head];
    q->head = (q->head + 1) % MAX_QUEUE_SIZE;
}
// AlertQueue functions
static int alert_queue_empty(AlertQueue *q) { return q->head == q->tail; }
static int alert_queue_full(AlertQueue *q) { return ((q->tail + 1) % MAX_QUEUE_SIZE) == q->head; }
static void queue_push_alert(AlertQueue *q, AlertMsg *alert) {
    if (alert_queue_full(q)) {
      LOG(LOG_WARNING, "Alert queue full, dropping alert for symbol index %d\n", alert->symbol_index);
        return;
    }
    q->alerts[q->tail] = *alert;
    q->tail = (q->tail + 1) % MAX_QUEUE_SIZE;
}
static void queue_pop_alert(AlertQueue *q, AlertMsg *alert) {
    if (alert_queue_empty(q)) return;
    *alert = q->alerts[q->head];
    q->head = (q->head + 1) % MAX_QUEUE_SIZE;
}
/* ----------------------------- Initialization ----------------------------- */
void initialize_state(ScannerState *state) {
    memset(state, 0, sizeof(*state));
    pthread_mutex_init(&state->symbols_mutex, NULL);

    // Initialize trade queue mutex/cond
    pthread_mutex_init(&state->trade_queue.mutex, NULL);
    pthread_cond_init(&state->trade_queue.cond, NULL);

    // Initialize alert queue mutex/cond
    pthread_mutex_init(&state->alert_queue.mutex, NULL);
    pthread_cond_init(&state->alert_queue.cond, NULL);
}
void cleanup_state(ScannerState *state) {
    // Free symbols
    pthread_mutex_lock(&state->symbols_mutex);
    for (int i = 0; i < state->num_symbols; i++) {
        free(state->symbols[i]);
    }
    pthread_mutex_unlock(&state->symbols_mutex);
    pthread_mutex_destroy(&state->symbols_mutex);

    pthread_mutex_destroy(&state->trade_queue.mutex);
    pthread_cond_destroy(&state->trade_queue.cond);
    pthread_mutex_destroy(&state->alert_queue.mutex);
    pthread_cond_destroy(&state->alert_queue.cond);
}
/* ----------------------------- WebSocket Handlers ----------------------------- */
static int handle_local_server_connection(ScannerState *state) {
    if (!state || !state->context) {
        LOG(LOG_ERR, "Invalid state or context when connecting to local server\n");
        return -1;
    }

    struct lws_client_connect_info ccinfo = {0};
    ccinfo.context = state->context;
    ccinfo.address = LOCAL_ADDRESS;
    ccinfo.port = LOCAL_PORT;
    ccinfo.path = "/ws";
    ccinfo.host = LOCAL_ADDRESS;
    ccinfo.origin = LOCAL_ADDRESS;
    ccinfo.protocol = "local-server";
    ccinfo.ssl_connection = 0;

    state->wsi_local = lws_client_connect_via_info(&ccinfo);
    if (!state->wsi_local) {
        LOG(LOG_ERR, "Failed to initiateo local server\n");
        return -1;
    }

    return 0;
}
static int handle_finnhub_connection(ScannerState *state) {
    if (!state || !state->context) {
        LOG(LOG_ERR, "Invalid state or context when connecting to Finnhub\n");
        return -1;
    }

    struct lws_client_connect_info ccinfo = {0};
    ccinfo.context = state->context;
    ccinfo.address = FINNHUB_HOST;
    ccinfo.port = 443;
    ccinfo.path = FINNHUB_PATH;
    ccinfo.host = FINNHUB_HOST;
    ccinfo.origin = FINNHUB_HOST;
    ccinfo.protocol = "finnhub";
    ccinfo.ssl_connection = LCCSCF_USE_SSL;

    state->wsi_finnhub = lws_client_connect_via_info(&ccinfo);
    if (!state->wsi_finnhub) {
      LOG(LOG_ERR, "Failed to initiate Finnhub connection: host=%s, path=%s, port=%d, errno=%d (%s)\n",
      FINNHUB_HOST, FINNHUB_PATH, ccinfo.port, errno, strerror(errno));
        return -1;
    }

    LOG(LOG_NOTICE, "Finnhub connection initiated successfully\n");
    return 0;
}

/* ----------------------------- Alert Sending ----------------------------- */
static void send_alert(ScannerState *state, int symbol_idx, double change, double price, int volume) {
    if (symbol_idx < 0 || symbol_idx >= state->num_symbols || !state->symbols[symbol_idx]) {
        LOG(LOG_ERR, "Invalid symbol index (%d) or null symbol in send_alert\n", symbol_idx);
        return;
    }

    const char *direction = (change > 0) ? "UP" : "DOWN";

    char payload[256];
    snprintf(payload, sizeof(payload),
             "{\"client_id\":\"%s\",\"data\":{"
             "\"symbol\":\"%s\",\"direction\":\"%s\","
             "\"change_percent\":%.2f,\"price\":%.2f,\"volume\":%d}}",
             state->scanner_id, state->symbols[symbol_idx], direction, fabs(change), price, volume);

    unsigned char buf[LWS_PRE + 256];
    unsigned char *p = &buf[LWS_PRE];
    size_t len = strlen(payload);
    memcpy(p, payload, len);

    if (state->wsi_local && !lws_send_pipe_choked(state->wsi_local)) {
        lws_write(state->wsi_local, p, len, LWS_WRITE_TEXT);
        DEBUG_PRINT("Alert sent: %s\n", payload);
    } else {
        LOG(LOG_ERR, "Local server connection unavailable or choked, alert for %s lost\n",
            state->symbols[symbol_idx]);
    }
}
/* ----------------------------- Enqueue Trade ----------------------------- */
static void enqueue_trade(ScannerState *state, const char *symbol, double price, int volume) {
    if (!state || !symbol) {
        LOG(LOG_ERR, "Invalid arguments in enqueue_trade (symbol: %s)\n", symbol ? symbol : "NULL");
        return;
    }

    TradeMsg trade;
    strncpy(trade.symbol, symbol, sizeof(trade.symbol) - 1);
    trade.symbol[sizeof(trade.symbol) - 1] = '\0';
    trade.price = price;
    trade.volume = volume;
    trade.timestamp = (unsigned long)time(NULL);

    pthread_mutex_lock(&state->trade_queue.mutex);
    if (trade_queue_full(&state->trade_queue)) {
        LOG(LOG_WARNING, "Trade queue full, dropping trade (symbol: %s, price: %.2f, volume: %d)\n",
            trade.symbol, trade.price, trade.volume);
    } else {
        queue_push_trade(&state->trade_queue, &trade);
        pthread_cond_signal(&state->trade_queue.cond);
        DEBUG_PRINT("Trade enqueued: symbol=%s, price=%.2f, volume=%d\n",
                    trade.symbol, trade.price, trade.volume);
    }
    pthread_mutex_unlock(&state->trade_queue.mutex);
}
/* ----------------------------- WebSocket Callbacks ----------------------------- */
static int local_server_callback(struct lws *wsi, enum lws_callback_reasons reason, void *user, void *in, size_t len) {
    ScannerState *state = (ScannerState *)lws_context_user(lws_get_context(wsi));

    switch (reason) {
        case LWS_CALLBACK_CLIENT_ESTABLISHED:
            LOG(LOG_NOTICE, "Connected to local server\n");
            state->wsi_local = wsi;
            {
                char register_msg[128];
                snprintf(register_msg, sizeof(register_msg), "{\"client_id\":\"%s\"}", state->scanner_id);
                unsigned char buf[LWS_PRE + 128];
                unsigned char *p = &buf[LWS_PRE];
                size_t msg_len = strlen(register_msg);
                memcpy(p, register_msg, msg_len);
                if (lws_write(wsi, p, msg_len, LWS_WRITE_TEXT) < 0)
                    LOG(LOG_ERR, "Failed to send registration message to local server\n");
                else
                    DEBUG_PRINT("Sent registration message: %s\n", register_msg);
            }
            break;

        case LWS_CALLBACK_CLIENT_RECEIVE: {
            struct json_object *msg = json_tokener_parse((char *)in);
            struct json_object *symbols_array;
            if (json_object_object_get_ex(msg, "symbols", &symbols_array)) {
                pthread_mutex_lock(&state->symbols_mutex);

                // Unsubscribe from old symbols
                if (state->wsi_finnhub) {
                    for (int i = 0; i < state->num_symbols; i++) {
                        char unsubscribe_msg[128];
                        snprintf(unsubscribe_msg, sizeof(unsubscribe_msg), "{\"type\":\"unsubscribe\",\"symbol\":\"%s\"}", state->symbols[i]);
                        unsigned char buf[LWS_PRE + 128];
                        unsigned char *p = &buf[LWS_PRE];
                        size_t msg_len = strlen(unsubscribe_msg);
                        memcpy(p, unsubscribe_msg, msg_len);
                        lws_write(state->wsi_finnhub, p, msg_len, LWS_WRITE_TEXT);
                    }
                    DEBUG_PRINT("Unsubscribed from %d symbols\n", state->num_symbols);
                }

                // Free old symbols
                for (int i = 0; i < state->num_symbols; i++) {
                    free(state->symbols[i]);
                    state->symbols[i] = NULL;
                }

                // Update symbols list
                state->num_symbols = json_object_array_length(symbols_array);
                if (state->num_symbols > MAX_SYMBOLS) state->num_symbols = MAX_SYMBOLS;
                for (int i = 0; i < state->num_symbols; i++) {
                    const char *sym = json_object_get_string(json_object_array_get_idx(symbols_array, i));
                    state->symbols[i] = strdup(sym);
                    state->last_checked_price[i] = 0.0;
                    state->last_alert_time[i] = 0;
                }

                pthread_mutex_unlock(&state->symbols_mutex);
                DEBUG_PRINT("Now subscribing to %d symbols\n", state->num_symbols);

                // Trigger re-subscription on Finnhub
                if (state->wsi_finnhub) {
                    FinnhubSession *session = (FinnhubSession *)lws_wsi_user(state->wsi_finnhub);
                    session->sub_index = 0;
                    lws_callback_on_writable(state->wsi_finnhub);
                }
            }
            json_object_put(msg);
            break;
        }

        case LWS_CALLBACK_CLIENT_CLOSED:
            LOG(LOG_NOTICE, "Local server connection closed, attempting to reconnect...\n");
            state->wsi_local = NULL;
            handle_local_server_connection(state);
            break;

        default:
            break;
    }
    return 0;
}
static int finnhub_callback(struct lws *wsi, enum lws_callback_reasons reason, void *user, void *in, size_t len) {
    FinnhubSession *session = (FinnhubSession *)user;
    ScannerState *state = (ScannerState *)lws_context_user(lws_get_context(wsi));

    switch (reason) {
        case LWS_CALLBACK_CLIENT_ESTABLISHED:
            LOG(LOG_NOTICE, "Connected to Finnhub\n");
            lws_callback_on_writable(wsi);
            break;

        case LWS_CALLBACK_CLIENT_WRITEABLE:
            if (session->sub_index < state->num_symbols) {
                char subscribe_msg[128];
                pthread_mutex_lock(&state->symbols_mutex);
                if (session->sub_index < state->num_symbols) {
                    snprintf(subscribe_msg, sizeof(subscribe_msg), "{\"type\":\"subscribe\",\"symbol\":\"%s\"}", state->symbols[session->sub_index]);
                }
                pthread_mutex_unlock(&state->symbols_mutex);
                unsigned char buf[LWS_PRE + 128];
                unsigned char *p = &buf[LWS_PRE];
                size_t msg_len = strlen(subscribe_msg);
                memcpy(p, subscribe_msg, msg_len);
                lws_write(wsi, p, msg_len, LWS_WRITE_TEXT);

                if (session->sub_index == 0) {
                    LOG(LOG_INFO, "Total symbols subscribed: %d\n", state->num_symbols);
                }

                DEBUG_PRINT("Subscribed to: %s\n", subscribe_msg);
                session->sub_index++;
                if (session->sub_index < state->num_symbols) lws_callback_on_writable(wsi);
            }
            break;

            case LWS_CALLBACK_CLIENT_RECEIVE: {
                  LOG_DEBUG("Finnhub received data: %.*s\n", (int)len, (char *)in);
                  // Ignore ping messages
                  if (len == 4 && strncmp((char *)in, "ping", 4) == 0) {
                      LOG("Ignored ping message.\n");
                      break;
                  }
                  struct json_object *msg = json_tokener_parse((char *)in);
                  if (!msg) {
                      LOG("Failed to parse JSON message: %.*s\n", (int)len, (char *)in);
                      break;
                  }
                  // If error type, log and ignore
                  struct json_object *type_obj;
                  if (json_object_object_get_ex(msg, "type", &type_obj)) {
                      const char *msg_type = json_object_get_string(type_obj);
                      if (strcmp(msg_type, "error") == 0) {
                          LOG("Error message received: %s\n", json_object_to_json_string(msg));
                          json_object_put(msg);
                          break;
                      }
                  }
                  // Process the "data" array of trades
                  struct json_object *data_array;
                  if (!json_object_object_get_ex(msg, "data", &data_array) || json_object_get_type(data_array) != json_type_array) {
                      LOG("JSON does not contain a valid 'data' array. Ignoring message.\n");
                      json_object_put(msg);
                      break;
                  }
                  int arr_len = json_object_array_length(data_array);
                  for (int i = 0; i < arr_len; i++) {
                      struct json_object *trade_obj = json_object_array_get_idx(data_array, i);
                      struct json_object *sym_obj, *price_obj, *vol_obj;
                      if (json_object_object_get_ex(trade_obj, "s", &sym_obj) && json_object_object_get_ex(trade_obj, "p", &price_obj) && json_object_object_get_ex(trade_obj, "v", &vol_obj)) {
                          const char *symbol = json_object_get_string(sym_obj);
                          double price = json_object_get_double(price_obj);
                          int volume = json_object_get_int(vol_obj);
                          LOG_DEBUG("Received trade: symbol=%s, price=%.2f, volume=%d\n", symbol, price, volume);
                          enqueue_trade(state, symbol, price, volume);
                      } else {
                          LOG("Missing required trade data fields. Skipping entry.\n");
                      }
                  }
                  json_object_put(msg);
                  break;
              }

        case LWS_CALLBACK_CLIENT_CONNECTION_ERROR:
            LOG(LOG_ERR, "Finnhub connection error: %s\n", in ? (char *)in : "Unknown error");
            handle_finnhub_connection(state);
            break;

        case LWS_CALLBACK_CLOSED:
            LOG(LOG_NOTICE, "Finnhub connection closed\n");
            handle_finnhub_connection(state);
            break;

        default:
            break;
    }
    return 0;
}
/* ----------------------------- Worker Threads ----------------------------- */
static void* trade_processing_thread(void *lpParam) {
    ScannerState *state = (ScannerState *)lpParam;
    while (!state->shutdown_flag) {
        TradeMsg trade;
        pthread_mutex_lock(&state->trade_queue.mutex);
        while (trade_queue_empty(&state->trade_queue) && !state->shutdown_flag) {
            pthread_cond_wait(&state->trade_queue.cond, &state->trade_queue.mutex);
        }
        if (state->shutdown_flag) {
            pthread_mutex_unlock(&state->trade_queue.mutex);
            break;
        }
        queue_pop_trade(&state->trade_queue, &trade);
        pthread_mutex_unlock(&state->trade_queue.mutex);

        // Find the symbol index (protect access to symbols)
        int idx = -1;
        pthread_mutex_lock(&state->symbols_mutex);
        for (int i = 0; i < state->num_symbols; i++) {
            if (strcmp(state->symbols[i], trade.symbol) == 0) {
                idx = i;
                break;
            }
        }
        pthread_mutex_unlock(&state->symbols_mutex);

        if (idx < 0) {
            DEBUG_PRINT("Trade received but symbol not found: %s\n", trade.symbol);
            continue;
        }

        // Protect access to last_checked_price and last_alert_time
        pthread_mutex_lock(&state->symbols_mutex);
        double old_price = state->last_checked_price[idx];
        if (old_price <= 0 || isnan(old_price) || isinf(old_price)) {
            LOG(LOG_WARNING, "Invalid old price for symbol %s: %.2f\n", trade.symbol, old_price);
            state->last_checked_price[idx] = trade.price;
            pthread_mutex_unlock(&state->symbols_mutex);
            continue;
        }

        // Get precise current time in milliseconds
        unsigned long current_time = get_current_time_ms();
        double change = ((trade.price - old_price) / old_price) * 100.0;

        if (fabs(change) >= PRICE_MOVEMENT && (current_time - state->last_alert_time[idx] >= DEBOUNCE_TIME)) {
            AlertMsg alert;
            alert.symbol_index = idx;
            alert.change = change;
            alert.price = trade.price;
            alert.volume = trade.volume;

            pthread_mutex_lock(&state->alert_queue.mutex);
            if (alert_queue_full(&state->alert_queue)) {
                LOG(LOG_WARNING, "Alert queue full, dropping alert for symbol: %s\n", trade.symbol);
            } else {
                queue_push_alert(&state->alert_queue, &alert);
                pthread_cond_signal(&state->alert_queue.cond);
            }
            pthread_mutex_unlock(&state->alert_queue.mutex);

            state->last_alert_time[idx] = current_time;
        }

        // Update last checked price
        state->last_checked_price[idx] = trade.price;
        pthread_mutex_unlock(&state->symbols_mutex);
    }
    return 0;
}
static void* alert_sending_thread(void *lpParam) {
    ScannerState *state = (ScannerState *)lpParam;
    while (!state->shutdown_flag) {
        AlertMsg alert;
        pthread_mutex_lock(&state->alert_queue.mutex);
        while (alert_queue_empty(&state->alert_queue) && !state->shutdown_flag) {
            pthread_cond_wait(&state->alert_queue.cond, &state->alert_queue.mutex);
        }
        if (state->shutdown_flag) {
            pthread_mutex_unlock(&state->alert_queue.mutex);
            break;
        }
        queue_pop_alert(&state->alert_queue, &alert);
        pthread_mutex_unlock(&state->alert_queue.mutex);

        // Send alert using the local websocket connection.
        if (state->wsi_local && !lws_send_pipe_choked(state->wsi_local)) {
            send_alert(state, alert.symbol_index, alert.change, alert.price, alert.volume);
            DEBUG_PRINT("Alert sent for symbol index %d\n", alert.symbol_index);
        } else {
            LOG(LOG_WARNING, "Failed to send alert: WebSocket connection unavailable or choked\n");
        }
    }
    return 0;
}
/* ----------------------------- Signal Handling ----------------------------- */
volatile sig_atomic_t shutdown_flag = 0;
volatile sig_atomic_t restart_flag = 0;
void handle_signal(int sig) {
    switch (sig) {
        case SIGINT:
        case SIGTERM:
            LOG(LOG_NOTICE, "Received signal %d (%s), initiating shutdown...\n", sig, strsignal(sig));
            shutdown_flag = 1;
            break;

        case SIGSEGV:
        case SIGABRT:
            LOG(LOG_ERR, "Critical error: signal %d (%s) received, restarting program...\n", sig, strsignal(sig));
            restart_flag = 1;
            break;

        default:
            DEBUG_PRINT("Unhandled signal received: %d (%s)\n", sig, strsignal(sig));
            break;
    }
}
void setup_signal_handlers() {
    struct sigaction sa;
    sa.sa_handler = handle_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART; // Ensures interrupted syscalls are restarted

    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGABRT, &sa, NULL);
}
/* ----------------------------- Main Function ----------------------------- */
int main(int argc, char *argv[]) {
    openlog("scanner", LOG_PID | LOG_CONS, LOG_USER);  // Open syslog
    LOG(LOG_NOTICE, "Scanner started\n");

    if (argc < 2) {
        LOG(LOG_ERR, "Invalid usage: Scanner ID missing. Usage: %s {scanner_id}\n", argv[0]);
        return 1;
    }

    const char *scanner_id = argv[1];

    setup_signal_handlers();

    while (!shutdown_flag) {
        ScannerState state;
        initialize_state(&state);

        // Store scanner_id in state
        strncpy(state.scanner_id, scanner_id, sizeof(state.scanner_id) - 1);
        state.scanner_id[sizeof(state.scanner_id) - 1] = '\0';

        LOG(LOG_INFO, "Scanner initialized with ID: %s\n", scanner_id);

        struct lws_protocols protocols[] = {
            {"local-server", local_server_callback, 0, 0},
            {"finnhub", finnhub_callback, sizeof(FinnhubSession), 0},
            {NULL, NULL, 0, 0}
        };

        struct lws_context_creation_info info = {0};
        info.protocols = protocols;
        info.user = &state;
        info.options = LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT;

        state.context = lws_create_context(&info);
        if (!state.context) {
            LOG(LOG_ERR, "lws_create_context failed, terminating program\n");
            cleanup_state(&state);
            return -1;
        }

        handle_local_server_connection(&state);
        handle_finnhub_connection(&state);

        pthread_t hTradeThread, hAlertThread;
        if (pthread_create(&hTradeThread, NULL, trade_processing_thread, &state) != 0) {
            LOG(LOG_ERR, "Failed to create trade processing thread\n");
            cleanup_state(&state);
            return -1;
        }
        if (pthread_create(&hAlertThread, NULL, alert_sending_thread, &state) != 0) {
            LOG(LOG_ERR, "Failed to create alert sending thread\n");
            cleanup_state(&state);
            return -1;
        }

        while (!shutdown_flag && !restart_flag) {
            lws_service(state.context, 50);
            DEBUG_PRINT("Running WebSocket event loop\n"); // Debug only
        }

        state.shutdown_flag = 1;
        pthread_cond_broadcast(&state.trade_queue.cond);
        pthread_cond_broadcast(&state.alert_queue.cond);
        pthread_join(hTradeThread, NULL);
        pthread_join(hAlertThread, NULL);

        lws_context_destroy(state.context);
        cleanup_state(&state);

        if (restart_flag) {
            LOG(LOG_ERR, "Restarting program due to crash...\n");
            restart_flag = 0;
        }
    }

    LOG(LOG_NOTICE, "Program shutdown gracefully.\n");
    closelog();
    return 0;
}
