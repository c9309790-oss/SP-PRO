#pragma once

#include <stdint.h>

void event_record_publish_brew_cleaning(int ml);
void event_record_publish_steam_cleaning(int ml);
void event_record_publish_empty_water(int ml);
void event_record_publish_descaling_ticks(uint32_t ticks);

