#ifndef INITIALISE_WIFI_TIME_H
#define INITIALISE_WIFI_TIME_H

// --- Type Definitions for Callbacks ---

/**
 * @brief A function pointer type for success/failure notification callbacks.
 */
typedef void (*init_callback_t)(void);


// --- Public Function Prototypes ---

/**
 * @brief (Asynchronous) Starts the process of initializing Wi-Fi and syncing time.
 *
 * This function is NON-BLOCKING. It initiates the initialization process in the
 * background and returns immediately. The outcome of the initialization will be
 * communicated through the provided callback functions.
 *
 * @param on_success_cb Pointer to a function to call on successful Wi-Fi connection and time sync.
 * @param on_failure_cb Pointer to a function to call if the process fails.
 */
void initialise_wifi_and_time_async(init_callback_t on_success_cb, init_callback_t on_failure_cb);

#endif // INITIALISE_WIFI_TIME_H