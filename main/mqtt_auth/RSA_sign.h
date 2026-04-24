#ifndef RSA_SIGN_H
#define RSA_SIGN_H

#include "esp_log.h"

int rsa_sign(const char *data,
             size_t data_len,
             const unsigned char *private_key_dec,
             size_t key_len,
             unsigned char *output,
             size_t output_buf_len,
             size_t *output_len);


#endif /* RSA_SIGN_H */
