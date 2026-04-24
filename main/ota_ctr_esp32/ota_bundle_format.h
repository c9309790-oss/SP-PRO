#ifndef OTA_BUNDLE_FORMAT_H
#define OTA_BUNDLE_FORMAT_H

#include <stdint.h>

/*
 * OTA merged bundle container format
 *
 * The bundle is a custom binary container, not a zip archive.
 * It supports carrying one or two payloads in a single file:
 *   - ESP32 application image
 *   - CTR firmware image
 *
 * File layout:
 *   [4 KB fixed header][payload 0][alignment padding][payload 1]...
 *
 * All multi-byte fields are little-endian.
 */

#define OTA_BUNDLE_MAGIC        0x4D41544FU /* 'OTAM' */
#define OTA_BUNDLE_VERSION      0x0001U
#define OTA_BUNDLE_HEADER_SIZE  4096U
#define OTA_BUNDLE_ALIGN        4096U
#define OTA_BUNDLE_MAX_ENTRIES  2U

typedef enum {
    OTA_BUNDLE_PAYLOAD_NONE  = 0,
    OTA_BUNDLE_PAYLOAD_ESP32 = 1,
    OTA_BUNDLE_PAYLOAD_CTR   = 2,
} ota_bundle_payload_type_t;

typedef struct __attribute__((packed)) {
    uint32_t type;              /* ota_bundle_payload_type_t */
    uint32_t offset;            /* Payload offset in bundle */
    uint32_t size;              /* Payload size in bytes */
    uint8_t md5[16];            /* Raw 16-byte MD5 */
    uint32_t crc32;             /* CRC32 of payload */
    char name[32];              /* Suggested source name */
    char version[32];           /* Optional version string */
    uint32_t flags;             /* Reserved for future use */
    uint8_t reserved[28];
} ota_bundle_entry_t;

typedef struct __attribute__((packed)) {
    uint32_t magic;             /* OTA_BUNDLE_MAGIC */
    uint16_t format_version;    /* OTA_BUNDLE_VERSION */
    uint16_t header_size;       /* OTA_BUNDLE_HEADER_SIZE */
    uint16_t entry_count;       /* 1..OTA_BUNDLE_MAX_ENTRIES */
    uint16_t flags;             /* Reserved for future use */
    uint32_t package_size;      /* Whole bundle size in bytes */
    uint8_t package_md5[16];    /* Raw 16-byte MD5 of whole bundle with this field zeroed */
    char package_version[32];   /* Optional package version string */
    ota_bundle_entry_t entries[OTA_BUNDLE_MAX_ENTRIES];
    uint8_t reserved[3776];
} ota_bundle_header_t;

#endif /* OTA_BUNDLE_FORMAT_H */
