/* Fill on the first boot, replay on the next. The harness counts port writes. */
#include "fc_app.h"

FC_APP_ABI()

#define Q FC_STR("samples")
#define COUNT 680

int32_t init(void)
{
    uint32_t id = 0;
    int32_t value = -1;
    int count = fc_config_int(FC_STR("count"), COUNT);
    if (count < 1 || count > COUNT) return 9;
    int drain = fc_config_int(FC_STR("drain"), 0);
    if (count < COUNT) drain = fc_queue_peek(Q, &value, 4, &id) == 4;
    if (!drain) {
        if (fc_queue_peek(Q, &value, 4, &id) != FC_ENOENT) return 1;
        for (int i = 0; i < count; i++) {
            if (fc_queue_push(Q, &i, sizeof(i)) != 0) return 2;
        }
        if (count == COUNT && (fc_queue_push(Q, &value, 4) != FC_ENOSPC ||
                               fc_kv_set(FC_STR("ab"), "123", 3) != FC_ENOSPC)) return 3;
        if (fc_queue_peek(Q, &value, 3, &id) != FC_ENOSPC || id != 0 || value != -1) return 4;
        if (count == COUNT) FC_STATE("full");
        else FC_STATE("saved");
        return 0;
    }
    for (int i = 0; i < count; i++) {
        if (fc_queue_peek(Q, &value, 4, &id) != 4 || value != i || id != 1u + (uint32_t)i) return 5;
        if (fc_queue_ack(Q, id - 1) != FC_EAGAIN || fc_queue_ack(Q, id) != 0) return 6;
    }
    if (fc_queue_peek(Q, &value, 4, &id) != FC_ENOENT) return 7;
    if (count < COUNT) {
        FC_STATE("replayed");
        return 0;
    }
    /* A replayed ack for the last sample cannot delete the first new one. */
    if (fc_queue_push(Q, &value, 4) != 0 || fc_queue_ack(Q, id) != FC_EAGAIN) return 8;
    FC_STATE("replayed");
    return 0;
}

void tick(void) {}
