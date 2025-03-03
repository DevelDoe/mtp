#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <curl/curl.h>
#include <json-c/json.h>
#include <unistd.h>

struct MemoryBuffer {
    char *data;
    size_t size;
};

size_t WriteMemoryCallback(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t realsize = size * nmemb;
    struct MemoryBuffer *mem = (struct MemoryBuffer *)userp;
    char *ptr = realloc(mem->data, mem->size + realsize + 1);
    if (!ptr) {
        fprintf(stderr, "Memory allocation error\n");
        return 0;
    }
    mem->data = ptr;
    memcpy(&(mem->data[mem->size]), contents, realsize);
    mem->size += realsize;
    mem->data[mem->size] = '\0';
    return realsize;
}

void build_query(char *query, size_t len, const char *api_key,
                 const char *market_cap_upper, const char *price_lower, const char *price_upper,
                 const char *min_volume, const char *exchange, const char *trading,
                 const char *etf, const char *fund) {

    char params[1024] = "";

    if (market_cap_upper && strlen(market_cap_upper))
        snprintf(params + strlen(params), sizeof(params) - strlen(params), "&marketCapLowerThan=%s", market_cap_upper);
    if (price_lower && strlen(price_lower))
        snprintf(params + strlen(params), sizeof(params) - strlen(params), "&priceMoreThan=%s", price_lower);
    if (price_upper && strlen(price_upper))
        snprintf(params + strlen(params), sizeof(params) - strlen(params), "&priceLowerThan=%s", price_upper);
    if (min_volume && strlen(min_volume))
        snprintf(params + strlen(params), sizeof(params) - strlen(params), "&volumeMoreThan=%s", min_volume);
    if (exchange && strlen(exchange))
        snprintf(params + strlen(params), sizeof(params) - strlen(params), "&exchange=%s", exchange);
    if (trading && strlen(trading))
        snprintf(params + strlen(params), sizeof(params) - strlen(params), "&isActivelyTrading=%s", trading);
    if (etf && strlen(etf))
        snprintf(params + strlen(params), sizeof(params) - strlen(params), "&isEtf=%s", etf);
    if (fund && strlen(fund))
        snprintf(params + strlen(params), sizeof(params) - strlen(params), "&isFund=%s", fund);

    snprintf(query, len,
             "https://financialmodelingprep.com/api/v3/stock-screener?apikey=%s%s",
             api_key, params);
}

#include <json-c/json.h>

// Extracts symbols into a simplified JSON object with client_id
struct json_object *extract_symbols(const char *json_data) {
    // Parse the original JSON data
    struct json_object *parsed_json = json_tokener_parse(json_data);
    if (!parsed_json || json_object_get_type(parsed_json) != json_type_array) {
        fprintf(stderr, "[ERROR] Invalid or unexpected JSON format\n");
        if (parsed_json) json_object_put(parsed_json);
        return NULL;
    }

    // Create the root JSON object
    struct json_object *result = json_object_new_object();

    // Create the data object to store symbols
    struct json_object *data = json_object_new_object();
    struct json_object *symbols = json_object_new_array();

    int num_objects = json_object_array_length(parsed_json);
    for (int i = 0; i < num_objects; ++i) {
        struct json_object *obj = json_object_array_get_idx(parsed_json, i);
        struct json_object *symbol_obj;

        // Assuming the symbol is under the "symbol" key
        if (json_object_object_get_ex(obj, "symbol", &symbol_obj)) {
            const char *symbol = json_object_get_string(symbol_obj);
            json_object_array_add(symbols, json_object_new_string(symbol));
        }
    }

    // Add symbols and length to the `data` object
    json_object_object_add(data, "symbols", symbols);
    json_object_object_add(data, "length", json_object_new_int(json_object_array_length(symbols)));

    // Add `data` and `client_id` to the root object
    json_object_object_add(result, "data", data);
    json_object_object_add(result, "client_id", json_object_new_string("scanner"));

    // Clean up parsed JSON
    json_object_put(parsed_json);

    return result;
}

int main() {
    CURL *curl;
    CURLcode res;

    curl_global_init(CURL_GLOBAL_DEFAULT);

    // Your filter parameters here
    const char *api_key = "RFWWfFzwRZJrDNSxzF4M64RKcuXq3T0O";
    const char *market_cap_upper = "30000000";
    const char *price_lower = "1";
    const char *price_upper = "7";
    const char *min_volume = "100000";
    const char *exchange = "NASDAQ";
    const char *trading = "true";
    const char *etf = "false";
    const char *fund = "false";

    while (1) {
        struct MemoryBuffer chunk = {malloc(1), 0};
        char query[2048];

        build_query(query, sizeof(query), api_key,
                    market_cap_upper, price_lower, price_upper,
                    min_volume, exchange, trading, etf, fund);

        curl = curl_easy_init();
        if (curl) {
            curl_easy_setopt(curl, CURLOPT_URL, query);
            curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteMemoryCallback);
            curl_easy_setopt(curl, CURLOPT_WRITEDATA, &chunk);

            res = curl_easy_perform(curl);

            long status_code = 0;
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status_code);

            printf("FMP GET Response Status: %ld\n", status_code);

            if (res != CURLE_OK) {
                fprintf(stderr, "GET request failed: %s\n", curl_easy_strerror(res));
            } else {
                struct json_object *parsed_json = json_tokener_parse(chunk.data);
                if (parsed_json && json_object_get_type(parsed_json) == json_type_array) {
                    int num_objects = json_object_array_length(parsed_json);
                    printf("Number of objects received: %d\n", num_objects);

                    // Extract the symbols into a simplified JSON structure
                    // Extract the symbols into a simplified JSON structure
struct json_object *simplified_json = extract_symbols(chunk.data);
if (simplified_json) {
    // Send the simplified JSON with client_id
    CURL *curl_post = curl_easy_init();
    if (curl_post) {
        curl_easy_setopt(curl_post, CURLOPT_URL, "http://localhost:8000/update-scanner-symbols");
        curl_easy_setopt(curl_post, CURLOPT_POST, 1L);
        curl_easy_setopt(curl_post, CURLOPT_POSTFIELDS, json_object_to_json_string(simplified_json));

        struct curl_slist *headers = NULL;
        headers = curl_slist_append(headers, "Content-Type: application/json; charset=UTF-8");
        curl_easy_setopt(curl_post, CURLOPT_HTTPHEADER, headers);

        CURLcode res = curl_easy_perform(curl_post);

        if (res != CURLE_OK) {
            fprintf(stderr, "POST request failed: %s\n", curl_easy_strerror(res));
        } else {
            printf("Simplified data with client_id posted successfully to server\n");
        }

        curl_slist_free_all(headers);
        curl_easy_cleanup(curl_post);
    }

    // Clean up the simplified JSON
    json_object_put(simplified_json);
}

                } else {
                    fprintf(stderr, "Failed to parse JSON or unexpected format\n");
                }
                json_object_put(parsed_json);
            }

            curl_easy_cleanup(curl);
        }

        free(chunk.data);

        sleep(600);
        printf("Looping again...\n\n");
    }

    curl_global_cleanup();
    return 0;
}
