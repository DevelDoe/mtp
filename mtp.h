#ifndef MTP_H
#define MTP_H

// scanner
#include <json-c/json.h>
#include <libwebsockets.h>
#include <math.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <time.h>


/* ---------------------- Configuration Macros ---------------------- */

// Debug Mode (set to 1 for extra logging, 0 for minimal logging)
#define DEBUG_MODE 0 // Change to 1 for debugging

// General Limits
#define MAX_SYMBOLS 50
#define MAX_QUEUE_SIZE 1024

// Price Movement Alert Thresholds
#define PRICE_MOVEMENT 1.0 // 1% price movement required for an alert
#define DEBOUNCE_TIME 3000 // 3-second cooldown between alerts (milliseconds)

// WebSocket Server URIs
#define LOCAL_ADDRESS "127.0.0.1"
#define LOCAL_PORT 8000
#define FINNHUB_HOST "ws.finnhub.io"
#define FINNHUB_PATH "/?token=cv0q3q1r01qo8ssi98cgcv0q3q1r01qo8ssi98d0"

/* ---------------------- Logging Macros ---------------------- */

#define LOG(level, fmt, ...)                                                   \
  do {                                                                         \
    if ((level == LOG_DEBUG && DEBUG_MODE) || level != LOG_DEBUG) {            \
      syslog(level, "[%s] " fmt, __func__, ##__VA_ARGS__);                     \
    }                                                                          \
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

typedef struct ScannerSymbols {
  char client_id[32];          // Scanner ID
  char **symbols;              // Array of assigned symbols
  int symbol_count;            // Number of assigned symbols
  struct ScannerSymbols *next; // Pointer to the next scanner in the list
} ScannerSymbols;

/* ---------------------- Function Declarations ---------------------- */

// Utility functions
unsigned long get_current_time_ms();
void log_message(int level, const char *fmt, ...);

// Signal Handling
void handle_signal(int sig);
void setup_signal_handlers();

// Program initialization & cleanup
void initialize_state(ScannerState *state);
void cleanup_state(ScannerState *state);

#endif // MTP_H
