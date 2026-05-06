#ifndef SP_PRO_BUILD_FLAGS_H
#define SP_PRO_BUILD_FLAGS_H

/* Certification / validation build switch.
 * 1: keep certification-oriented behavior enabled by default.
 * 0: normal production behavior. */
#define APP_CERT_TEST_BUILD 1

#define APP_TEST_DISABLE_LOCAL_ALARM        0
/* Set to 1 to bypass the normal first_powered_on/CTR-completion handshake and
 * force a single startup replenish. Leave at 0 to test the real replenish
 * logic driven by first_powered_on + CTR completion. */
#define APP_TEST_FORCE_POWER_ON_REPLENISH   0

#endif /* SP_PRO_BUILD_FLAGS_H */
