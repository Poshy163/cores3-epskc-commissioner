#include <assert.h>

#include "../main/rcp_update_policy.h"

#define BUNDLED "openthread-esp32/051e4c4e-43cc05a; esp32h2;  2026-11-20 08:47:41 UTC"
#define STOCK "openthread-esp32/50c71d69b84-a12ff0d0f; esp32h2;  2026-04-10 09:57:39 UTC"

int main(void)
{
    /* Nothing bundled: never touch the H2, whatever else is recorded. */
    assert(rcp_update_decide(NULL, NULL, false, 0) == RCP_UPDATE_SKIP_NO_IMAGE);
    assert(rcp_update_decide("", BUNDLED, true, 0) == RCP_UPDATE_SKIP_NO_IMAGE);
    assert(rcp_update_decide("", NULL, true, RCP_UPDATE_MAX_ATTEMPTS) == RCP_UPDATE_SKIP_NO_IMAGE);

    /* First boot after installing this firmware: no record, so flash. */
    assert(rcp_update_decide(BUNDLED, NULL, false, 0) == RCP_UPDATE_FLASH);
    assert(rcp_update_decide(BUNDLED, "", false, 0) == RCP_UPDATE_FLASH);

    /* A different bundled image (firmware update) flashes again. */
    assert(rcp_update_decide(BUNDLED, STOCK, false, 0) == RCP_UPDATE_FLASH);

    /* Already flashed with this image: leave it alone... */
    assert(rcp_update_decide(BUNDLED, BUNDLED, false, 0) == RCP_UPDATE_SKIP_CURRENT);
    assert(rcp_update_decide(BUNDLED, BUNDLED, false, RCP_UPDATE_MAX_ATTEMPTS) == RCP_UPDATE_SKIP_CURRENT);
    /* ...unless a reflash was requested. */
    assert(rcp_update_decide(BUNDLED, BUNDLED, true, 0) == RCP_UPDATE_FLASH);
    assert(rcp_update_decide(BUNDLED, BUNDLED, true, RCP_UPDATE_MAX_ATTEMPTS - 1) == RCP_UPDATE_FLASH);

    /* Attempts that never led to a verified boot stop automatic retries. */
    assert(rcp_update_decide(BUNDLED, NULL, false, RCP_UPDATE_MAX_ATTEMPTS) == RCP_UPDATE_GIVE_UP);
    assert(rcp_update_decide(BUNDLED, STOCK, true, RCP_UPDATE_MAX_ATTEMPTS) == RCP_UPDATE_GIVE_UP);
    assert(rcp_update_decide(BUNDLED, STOCK, false, UINT8_MAX) == RCP_UPDATE_GIVE_UP);

    /* Versions are compared exactly, including the double space ESP-IDF emits. */
    assert(rcp_update_decide(BUNDLED,
                             "openthread-esp32/051e4c4e-43cc05a; esp32h2; 2026-11-20 08:47:41 UTC",
                             false, 0) == RCP_UPDATE_FLASH);

    /* Running-version check after OpenThread starts. */
    assert(!rcp_update_running_differs(BUNDLED, BUNDLED));
    assert(rcp_update_running_differs(BUNDLED, STOCK));
    assert(!rcp_update_running_differs(NULL, STOCK));
    assert(!rcp_update_running_differs("", STOCK));
    assert(!rcp_update_running_differs(BUNDLED, NULL));
    assert(!rcp_update_running_differs(BUNDLED, ""));
    return 0;
}
