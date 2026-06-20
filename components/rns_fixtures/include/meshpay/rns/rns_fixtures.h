#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    RNS_FIXTURE_KIND_SCHEMA_CANARY = 0,
    RNS_FIXTURE_KIND_DESTINATION_NAME_HASH,
    RNS_FIXTURE_KIND_DESTINATION_HASH,
    RNS_FIXTURE_KIND_PACKET_RAW,
    RNS_FIXTURE_KIND_ANNOUNCE_RAW,
    RNS_FIXTURE_KIND_ENCRYPTED_TOKEN,
} rns_fixture_kind_t;

typedef struct {
    const char *name;
    rns_fixture_kind_t kind;
    const uint8_t *bytes;
    size_t len;
} rns_fixture_t;

const char *rns_fixtures_schema(void);
const char *rns_fixtures_source(void);
size_t rns_fixtures_count(void);
const rns_fixture_t *rns_fixtures_get(size_t index);

#ifdef __cplusplus
}
#endif
