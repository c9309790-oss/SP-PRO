#include "record_publish_port.h"

#include "mqtt_protocol.h"

bool record_publish_drink(const drink_record_t *record)
{
    if (!record) {
        return false;
    }

    publish_drink_record_to_mqtt((drink_record_t *)record);
    return true;
}

bool record_publish_event(const event_record_t *record)
{
    if (!record) {
        return false;
    }

    publish_event_record_to_mqtt((event_record_t *)record);
    return true;
}

bool record_publish_extraction_curve_batch(const extraction_curve_record_t *records,
                                           int record_count,
                                           bool curve_update)
{
    if (!records || record_count <= 0) {
        return false;
    }

    publish_extraction_curve_records_to_mqtt((extraction_curve_record_t *)records,
                                             record_count,
                                             curve_update);
    return true;
}
