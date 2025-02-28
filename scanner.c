#include <json-c/json.h>
#include <libwebsockets.h>
#include <math.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <syslog.h>

// Function to get the current time in milliseconds
unsigned long get_current_time_ms() {
    struct timespec ts;
    // You can use CLOCK_REALTIME or CLOCK_MONOTONIC depending on your needs.
    clock_gettime(CLOCK_REALTIME, &ts);
    return ts.tv_sec * 1000UL + ts.tv_nsec / 1000000UL;
}

// Function to log messages (using syslog on Unix)
void log_message(const char* message) {
    syslog(LOG_INFO, "%s", message);
}

/* ----------------------------- Configuration ------------------------------ */
#define DEBUG_MODE 1
#define MAX_SYMBOLS 50
#define PRICE_MOVEMENT 1.0  // 1% price movement
#define DEBOUNCE_TIME 3000  // 3 seconds in milliseconds
#define LOCAL_SERVER_URI "ws://192.168.1.17:8000/ws"
#define FINNHUB_URI "wss://ws.finnhub.io/?token=your_token"
#define MAX_QUEUE_SIZE 1024  // For both trade and alert queues

static char scanner_id[16];  // Global variable for scanner ID

/* ----------------------------- Helper Macros ------------------------------ */
#define LOG(fmt, ...) printf("[%s] " fmt, __func__, ##__VA_ARGS__)
#define DEBUG_PRINT(fmt, ...) \
    if (DEBUG_MODE) LOG(fmt, ##__VA_ARGS__)

/* ----------------------------- Data Structures ---------------------------- */

// Trade message (from Finnhub)
typedef struct {
    char symbol[32];
    double price;
    int volume;
    unsigned long timestamp;
} TradeMsg;

// Alert message to be sent to local server
typedef struct {
    int symbol_index;
    double change;  // Signed change (positive or negative)
    double price;
    int volume;
} AlertMsg;

// Thread-safe queue for trades
typedef struct {
    TradeMsg trades[MAX_QUEUE_SIZE];
    int head, tail;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
} TradeQueue;

// Thread-safe queue for alerts
typedef struct {
    AlertMsg alerts[MAX_QUEUE_SIZE];
    int head, tail;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
} AlertQueue;

// Global scanner state; note that we separate the symbols data mutex because
// symbols may be updated by the local server callback.
typedef struct {
    char *symbols[MAX_SYMBOLS];
    int num_symbols;
    double last_checked_price[MAX_SYMBOLS];
    unsigned long last_alert_time[MAX_SYMBOLS];

    // WebSocket connection handles
    struct lws *wsi_local;
    struct lws *wsi_finnhub;
    struct lws_context *context;

    // Queues for inter-thread communication
    TradeQueue trade_queue;
    AlertQueue alert_queue;

    // Mutex for protecting symbols array updates
    pthread_mutex_t symbols_mutex;

    // Global shutdown flag
    volatile int shutdown_flag;
} ScannerState;

// Session data for Finnhub connection
typedef struct {
    int sub_index;
} FinnhubSession;

/* ----------------------------- Queue Functions ---------------------------- */
// TradeQueue functions
static int trade_queue_empty(TradeQueue *q) { return q->head == q->tail; }

static int trade_queue_full(TradeQueue *q) { return ((q->tail + 1) % MAX_QUEUE_SIZE) == q->head; }

static void queue_push_trade(TradeQueue *q, TradeMsg *trade) {
    if (trade_queue_full(q)) {
        LOG("Trade queue full, dropping trade\n");
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
        LOG("Alert queue full, dropping alert\n");
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
static void initialize_state(ScannerState *state) {
    memset(state, 0, sizeof(*state));
    pthread_mutex_init(&state->symbols_mutex, NULL);

    // Initialize trade queue mutex/cond
    pthread_mutex_init(&state->trade_queue.mutex, NULL);
    pthread_cond_init(&state->trade_queue.cond, NULL);

    // Initialize alert queue mutex/cond
    pthread_mutex_init(&state->alert_queue.mutex, NULL);
    pthread_cond_init(&state->alert_queue.cond, NULL);
}

static void cleanup_state(ScannerState *state) {
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
        LOG("Invalid state or context\n");
        return -1;
    }

    struct lws_client_connect_info ccinfo = {0};
    ccinfo.context = state->context;
    ccinfo.address = "127.0.0.1";
    ccinfo.port = 8000;
    ccinfo.path = "/ws";
    ccinfo.host = ccinfo.address;
    ccinfo.origin = ccinfo.address;
    ccinfo.protocol = "local-server";
    ccinfo.ssl_connection = 0;

    state->wsi_local = lws_client_connect_via_info(&ccinfo);
    if (!state->wsi_local) {
        LOG("Failed to connect to local server\n");
        state->wsi_local = NULL;  // Explicitly reset on failure
        return -1;
    }

    LOG("Local server connection initiated\n");
    return 0;
}

static int handle_finnhub_connection(ScannerState *state) {
    if (!state || !state->context) {
        LOG("Invalid state or context\n");
        return -1;
    }

    struct lws_client_connect_info ccinfo = {0};
    ccinfo.context = state->context;
    ccinfo.address = "ws.finnhub.io";
    ccinfo.port = 443;
    ccinfo.path = "/?token=your_token";  // Replace with your token
    ccinfo.host = ccinfo.address;
    ccinfo.origin = ccinfo.address;
    ccinfo.protocol = "finnhub";
    ccinfo.ssl_connection = LCCSCF_USE_SSL;

    state->wsi_finnhub = lws_client_connect_via_info(&ccinfo);
    if (!state->wsi_finnhub) {
        LOG("Failed to connect to Finnhub\n");
        return -1;
    }

    LOG("Finnhub connection initiated\n");
    return 0;
}

/* ----------------------------- Alert Sending ----------------------------- */
static void send_alert(ScannerState *state, int symbol_idx, double change, double price, int volume) {
    if (symbol_idx < 0 || symbol_idx >= state->num_symbols || !state->symbols[symbol_idx]) {
        LOG("Invalid symbol index or null symbol\n");
        return;
    }

    const char *direction = (change > 0) ? "UP" : "DOWN";

    char payload[256];
    snprintf(payload, sizeof(payload),
             "{\"client_id\":\"%s\",\"data\":{"
             "\"symbol\":\"%s\",\"direction\":\"%s\","  // Use dynamic client ID
             "\"change_percent\":%.2f,\"price\":%.2f,\"volume\":%d}}",
             scanner_id, state->symbols[symbol_idx], direction, fabs(change), price, volume);

    unsigned char buf[LWS_PRE + 256];
    unsigned char *p = &buf[LWS_PRE];
    size_t len = strlen(payload);
    memcpy(p, payload, len);

    if (state->wsi_local && !lws_send_pipe_choked(state->wsi_local)) {
        lws_write(state->wsi_local, p, len, LWS_WRITE_TEXT);
        LOG("Alert sent: %s\n", payload);
    } else {
        LOG("Local server connection unavailable or choked\n");
    }
}

/* ----------------------------- Enqueue Trade ----------------------------- */
void enqueue_trade(ScannerState *state, const char *symbol, double price, int volume) {
    if (!state || !symbol) {
        LOG("Invalid arguments in enqueue_trade\n");
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
        LOG("Trade queue full, dropping trade\n");
    } else {
        queue_push_trade(&state->trade_queue, &trade);
        pthread_cond_signal(&state->trade_queue.cond);
    }
    pthread_mutex_unlock(&state->trade_queue.mutex);
}

/* ----------------------------- Main Function ----------------------------- */
void handle_signal(int sig) {
    if (sig == SIGINT || sig == SIGTERM) {
        shutdown_flag = 1;
    } else if (sig == SIGSEGV || sig == SIGABRT) {
        restart_flag = 1;
    }
}

void setup_signal_handlers() {
    struct sigaction sa;
    sa.sa_handler = handle_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;

    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGABRT, &sa, NULL);
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <scanner_id>\n", argv[0]);
        return EXIT_FAILURE;
    }
    snprintf(scanner_id, sizeof(scanner_id), "%s", argv[1]);

    setup_signal_handlers();

    while (!shutdown_flag) {
        ScannerState state;
        initialize_state(&state);

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
            LOG("lws_create_context failed\n");
            cleanup_state(&state);
            return -1;
        }

        handle_local_server_connection(&state);
        handle_finnhub_connection(&state);

        pthread_t hTradeThread, hAlertThread;
        if (pthread_create(&hTradeThread, NULL, trade_processing_thread, &state) != 0) {
            LOG("Failed to create trade processing thread\n");
            cleanup_state(&state);
            return -1;
        }
        if (pthread_create(&hAlertThread, NULL, alert_sending_thread, &state) != 0) {
            LOG("Failed to create alert sending thread\n");
            cleanup_state(&state);
            return -1;
        }

        while (!shutdown_flag && !restart_flag) {
            lws_service(state.context, 50);
            DEBUG_PRINT("Running WebSocket event loop\n");
        }

        state.shutdown_flag = 1;
        pthread_cond_broadcast(&state.trade_queue.cond);
        pthread_cond_broadcast(&state.alert_queue.cond);
        pthread_join(hTradeThread, NULL);
        pthread_join(hAlertThread, NULL);

        lws_context_destroy(state.context);
        cleanup_state(&state);

        if (restart_flag) {
            LOG("Restarting program due to crash...\n");
            restart_flag = 0;
        }
    }

    LOG("Program shutdown gracefully.\n");
    return 0;
}
