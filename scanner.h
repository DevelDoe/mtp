#ifndef SCANNER_H
#define SCANNER_H

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

/* ---------------------- Configuration Macros ---------------------- */

// Debug Mode (set to 1 for extra logging, 0 for minimal logging)
#define DEBUG_MODE 0  // Change to 1 for debugging

// General Limits
#define MAX_SYMBOLS 50
#define MAX_QUEUE_SIZE 1024

// Price Movement Alert Thresholds
#define PRICE_MOVEMENT 1.0   // 1% price movement required for an alert
#define DEBOUNCE_TIME 3000   // 3-second cooldown between alerts (milliseconds)

// WebSocket Server URIs
#define LOCAL_ADDRESS "127.0.0.1"
#define LOCAL_PORT 8000
#define FINNHUB_HOST "ws.finnhub.io"
#define FINNHUB_PATH "/?token=cv0q3q1r01qo8ssi98cgcv0q3q1r01qo8ssi98d0"

/* ---------------------- Logging Macros ---------------------- */

#define LOG(level, fmt, ...) do { \
    if ((level == LOG_DEBUG && DEBUG_MODE) || level != LOG_DEBUG) { \
        syslog(level, "[%s] " fmt, __func__, ##__VA_ARGS__); \
    } \
} while (0)
#define DEBUG_PRINT(fmt, ...) LOG(LOG_DEBUG, fmt, ##__VA_ARGS__)

/* ---------------------- Struct & Type Definitions ---------------------- */

typedef struct {
    char symbol[32];
    double price;
    int volume;
    unsigned long timestamp;
} TradeMsg;

typedef struct {
    int symbol_index;
    double change;
    double price;
    int volume;
} AlertMsg;

typedef struct {
    TradeMsg trades[MAX_QUEUE_SIZE];
    int head, tail;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
} TradeQueue;

typedef struct {
    AlertMsg alerts[MAX_QUEUE_SIZE];
    int head, tail;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
} AlertQueue;

typedef struct {
    char *symbols[MAX_SYMBOLS];
    int num_symbols;
    double last_checked_price[MAX_SYMBOLS];
    unsigned long last_alert_time[MAX_SYMBOLS];

    struct lws *wsi_local;
    struct lws *wsi_finnhub;
    struct lws_context *context;

    TradeQueue trade_queue;
    AlertQueue alert_queue;

    pthread_mutex_t symbols_mutex;
    volatile int shutdown_flag;
    char scanner_id[64];
} ScannerState;

typedef struct {
    int sub_index;
} FinnhubSession;

/* ---------------------- Function Declarations ---------------------- */

// Utility functions
unsigned long get_current_time_ms();
void log_message(const char* message);

// Queue functions
int trade_queue_empty(TradeQueue *q);
int trade_queue_full(TradeQueue *q);
void queue_push_trade(TradeQueue *q, TradeMsg *trade);
void queue_pop_trade(TradeQueue *q, TradeMsg *trade);
int alert_queue_empty(AlertQueue *q);
int alert_queue_full(AlertQueue *q);
void queue_push_alert(AlertQueue *q, AlertMsg *alert);
void queue_pop_alert(AlertQueue *q, AlertMsg *alert);

// WebSocket Handlers
int handle_local_server_connection(ScannerState *state);
int handle_finnhub_connection(ScannerState *state);

// Trade & Alert processing
void send_alert(ScannerState *state, int symbol_idx, double change, double price, int volume);
void enqueue_trade(ScannerState *state, const char *symbol, double price, int volume);

// WebSocket callbacks
int local_server_callback(struct lws *wsi, enum lws_callback_reasons reason, void *user, void *in, size_t len);
int finnhub_callback(struct lws *wsi, enum lws_callback_reasons reason, void *user, void *in, size_t len);

// Worker threads
void* trade_processing_thread(void *lpParam);
void* alert_sending_thread(void *lpParam);

// Signal Handling
void handle_signal(int sig);
void setup_signal_handlers();

// Program initialization & cleanup
void initialize_state(ScannerState *state);
void cleanup_state(ScannerState *state);

#endif // SCANNER_H
