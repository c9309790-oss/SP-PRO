#ifndef MQTT_AUTH_H
#define MQTT_AUTH_H

#include <stdbool.h>

void mqtt_auth_init();
bool mqtt_auth_is_in_progress(void);
extern char Auth_mqtt_user_name[64],Auth_mqtt_password[64];

#endif /* MQTT_AUTH_H */
