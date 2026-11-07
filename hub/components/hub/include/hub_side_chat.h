/* The Muse side chat the gadget's turns go to (CONFIG_HUB_SIDE_CHAT). */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/*
 * With CONFIG_HUB_SIDE_CHAT, picks the side chat (CONFIG_HUB_SIDE_CHAT_ID, else
 * the one kept in NVS, else a new one kept there) and hands it to the SDK.
 * Call from hub_app_start(), before Muse connects. Does nothing otherwise.
 */
void hub_side_chat_start(void);

#ifdef __cplusplus
}
#endif
