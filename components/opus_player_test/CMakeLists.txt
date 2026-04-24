idf_component_register(SRCS "opus_player_test.c"
                    INCLUDE_DIRS "include"
                    REQUIRES opus_player esp_wifi esp_event nvs_flash driver)
