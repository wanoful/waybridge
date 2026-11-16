#include "xdg-activation-client-protocol.h"
static struct xdg_activation_v1 *activation_manager;
static char *activation_replay;
static void activation_done(void *data, struct xdg_activation_token_v1 *token, const char *value) {
  (void)data;
  free(activation_replay); activation_replay = strdup(value);
  xdg_activation_v1_activate(activation_manager, value, windows[1].surface);
  xdg_activation_token_v1_destroy(token);
  puts("ACTIVATION_DONE"); fflush(stdout);
}
static const struct xdg_activation_token_v1_listener activation_listener = {.done=activation_done};
static void activation_key(uint32_t serial, uint32_t key) {
  if (!getenv("WAYBRIDGE_TEST_ACTIVATION") || !activation_manager) return;
  if (key == 30 || key == 48) {
    struct xdg_activation_token_v1 *token = xdg_activation_v1_get_activation_token(activation_manager);
    xdg_activation_token_v1_add_listener(token, &activation_listener, NULL);
    xdg_activation_token_v1_set_serial(token, key == 30 ? serial : serial+50000, seat);
    xdg_activation_token_v1_set_surface(token, windows[0].surface);
    xdg_activation_token_v1_commit(token);
  }
  if (key == 46 && activation_replay)
    xdg_activation_v1_activate(activation_manager, activation_replay, windows[1].surface);
}
