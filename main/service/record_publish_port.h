#ifndef RECORD_PUBLISH_PORT_H
#define RECORD_PUBLISH_PORT_H

#include <stdbool.h>
#include "service_domain_types.h"

bool record_publish_drink(const drink_record_t *record);
bool record_publish_event(const event_record_t *record);
bool record_publish_extraction_curve_batch(const extraction_curve_record_t *records,
                                           int record_count,
                                           bool curve_update);

#endif /* RECORD_PUBLISH_PORT_H */
