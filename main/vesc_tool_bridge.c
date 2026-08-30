#include "vesc_tool_bridge.h"

#include "ebike_can.h"

static vesc_tool_reply_fn s_reply_callback;
static void *s_reply_user_data;

esp_err_t vesc_tool_bridge_send(const uint8_t *payload, size_t length)
{
    return ebike_can_send_vesc_payload(VESC_TOOL_BRIDGE_CAN_ID, payload, length);
}

void vesc_tool_bridge_set_reply_callback(vesc_tool_reply_fn callback,
                                         void *user_data)
{
    s_reply_callback = callback;
    s_reply_user_data = user_data;
}

void vesc_tool_bridge_deliver_reply(const uint8_t *payload, size_t length)
{
    if (s_reply_callback != NULL) {
        s_reply_callback(payload, length, s_reply_user_data);
    }
}
