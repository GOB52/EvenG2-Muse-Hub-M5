# EvenG2-Muse-Hub-M5

[日本語](README.md) | English

Talk to Muse from Even Realities G2 glasses, through a Gadget (an M5Stack device running as a Muse Gadget).
A question asked to the G2's Even AI (a custom agent) goes to Muse as a typed turn on the Gadget, and the reply comes back to the G2's HUD. No PC is needed.

```
G2 "Hey Even …" → Even app on the iPhone (speech-to-text in Even's cloud)
   ─http://muse-hub.local:8787/v1/chat/completions─▶ Gadget (on the iPhone's Personal Hotspot over Wi-Fi)
   ─typed turn─▶ Muse ─reply text─▶ G2 HUD
```

| dir | contents |
|---|---|
| `hub/` | Firmware for the Gadget (ESP-IDF). Serves the Even AI endpoint on the LAN |
| `relay/` | The same on a Mac (talks to the Gadget over USB). See the appendix |

The paging keywords and the notes the hub adds to replies are Japanese (the project is used in Japanese); see "Usage"
and "Localizing the keywords and notes".

## Requirements

- An M5Stack device for the Gadget: **StickS3**, **StickC Plus2** or **StopWatch** (see "Supported devices" for the differences)
- Even Realities G2, an iPhone (Even app and Muse app)
- ESP-IDF **v6.0.3** (the only version the Muse Gadget SDK supports)
- A [Muse SDK token](https://gadgets.muse.ai/settings/sdk-tokens)
- A token for Even AI (a random string of 32+ characters you make yourself)

## Building and flashing the Gadget (hub)

```sh
git clone --recursive https://github.com/GOB52/EvenG2-Muse-Hub-M5.git  # existing clone: git submodule update --init
. <esp-idf v6.0.3>/export.sh             # with eim: activate_idf_v6.0.3.sh

# write the tokens (git-ignored; see "Tokens" below)
$EDITOR hub/sdkconfig.local

hub/tools/build.sh build
hub/tools/build.sh flash-monitor /dev/cu.usbmodemXXXX

# for other devices than the StickS3 add BOARD (cplus2 for the StickC Plus2, stopwatch for the StopWatch; it builds in hub/build-<board>)
BOARD=cplus2 hub/tools/build.sh build
BOARD=cplus2 hub/tools/build.sh flash-monitor /dev/cu.usbserial-XXXX
```

After changing `hub/sdkconfig.hub`, `hub/sdkconfig.hub-<board>` or `hub/sdkconfig.local` (including pulling a change),
delete `hub/build-<board>/sdkconfig` and rebuild (sdkconfig defaults only seed a new sdkconfig).

### Tokens

- **SDK token** (`CONFIG_GADGET_SDK_TOKEN`): issue one at [gadgets.muse.ai](https://gadgets.muse.ai/settings/sdk-tokens)
  (Account > SDK tokens). After replacing it, pair again ("Setup", step 2).
- **Relay token** (`CONFIG_HUB_RELAY_TOKEN`, the Even AI API key): make a random string (32+ characters) in your own terminal.

  ```sh
  openssl rand -base64 32 | tr -d '=+/'    # about 40 characters
  ```

  Put the same value in `hub/sdkconfig.local`, the API key of the Even app's agent configuration, and, if you use the relay,
  `RELAY_TOKEN` in `relay/.env` (changing only one gives HTTP 401).

An example `hub/sdkconfig.local` (dummy values):

```
CONFIG_GADGET_SDK_TOKEN="mgst_xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
CONFIG_HUB_RELAY_TOKEN="<the random string made above>"
# to use a side chat (optional; see "Usage")
# CONFIG_HUB_SIDE_CHAT=y
# CONFIG_HUB_SIDE_CHAT_ID="<UUID>"
```

- `hub/sdkconfig.local` and `relay/.env` are git-ignored. Write the values in an editor (typed into a shell or an AI chat, they stay in its history).
- If one leaks, make a new one, put it in every place above, delete `hub/build-<board>/sdkconfig` and rebuild.

## Setup

1. **iPhone Personal Hotspot**: Settings → Personal Hotspot, turn **Maximize Compatibility on** (the Gadget is 2.4 GHz only).
   Keep the hotspot screen open while pairing (with no device connected the hotspot stops, and the Gadget logs `reason=201`).
2. **Pairing**: in the Muse app, Settings > Devices, turn on Developer mode and add `MuseGadget-XXXXXX` with Add Device.
   Choose the iPhone's hotspot for Wi-Fi. To change Wi-Fi: unlink in the app → reset the Gadget's pairing (on the StickS3, hold its button for 5 s) → add it again.
   **After replacing the SDK token**, pair again once flashed: the boot log shows `setup=advertising` (not connected to Muse,
   waiting to pair), so unlink the old link in the app → add it again with Add Device (no long press needed). The relay token has
   nothing to do with pairing (just set the Even app's API Key to the same value).
3. **iPhone Local Network permission**: Settings → Privacy & Security → Local Network, turn **Even Realities on**
   (off, the HUD shows an "AI server error").
4. **Registering with Even**: sign in to [Even Hub](https://hub.evenrealities.com/login) with the Even app's account, then quit
   and reopen the app (the developer settings appear). In Settings → Even AI → Agent Configuration, set the Endpoint to
   `http://muse-hub.local:8787/v1/chat/completions` and the API Key to the value of `CONFIG_HUB_RELAY_TOKEN`, then Save & Activate.
   If `muse-hub.local` doesn't resolve, use the IP from the boot log line `hub: endpoint: http://172.20.10.x:8787/...`.

## Usage

Say "Hey Even" to the G2. Muse's reply shows as text on the HUD (the G2 gets text; when you talk on the Gadget, the reply's voice also plays on the Gadget).

- **25-second wait**: Even AI gives up after about 30 s, so if the reply isn't complete in 25 s, the hub returns what has arrived and
  "(まだ考え中です。「続き」と話しかけると続きを返します)" ("still thinking; say 続き for the rest"). The reply is kept and returned on 「続き」 (dropped after 10 minutes).
- **Pages**: a long reply is returned a page at a time (2000 characters by default; the G2 scrolls), each page ending with
  "(続きは「続き」、残り全部は「終わりまで」と話しかけてください)". While pages are kept, what you say decides what comes back:
  - 「続き」 (つづき / 続けて / 次 / もっと — "next"): the next page.
  - 「終わりまで」 (最後まで / 全部 / 全文 / 残り / 続きを全部 — "all the rest"): the rest at once (up to 5000 characters by default).
  - Anything else: the kept pages are dropped and it is a new question to Muse.
  - Only an utterance (without punctuation and spaces) that is a keyword, or a keyword plus a fixed ending
    (を / を見せて / を教えて / をお願い / をください / お願い / 見せて / して / のページ / ページ / 読んで / を読んで …), pages.
    Questions like 「次の予定は?」, 「手続きの方法は?」 or 「全部屋の温度は?」 are new questions.
  - After the 10 minutes, 「続き」/「終わりまで」 isn't sent to Muse; the hub answers
    "(続きの保持は期限切れです。もう一度質問してください)" ("the rest has expired; ask again"). The next new question clears this.
    With nothing kept (e.g. after the last page), they are ordinary questions to Muse.
- **Widgets**: a list in Muse's reply becomes "title + ・item — description", a map "description + ・place — time etc.".
  What can't be text (image choices, HTML widgets, tap-to-reply buttons) becomes "(Muse アプリに表示: …)" ("shown in the Muse app").
- **Empty replies**: a reply with only links or images (no text or widgets) gives "(Muse の返事にテキストがありませんでした。…)".
- **Side chat** (with `CONFIG_HUB_SIDE_CHAT=y`): the Gadget's turns (from the G2 and spoken on the Gadget) go to one Muse side chat
  instead of the main chat, and replies are read from it. The G2's conversation stays apart in the Muse app and is never mixed up with
  an answer to a question typed in the main chat at the same time. The default is the main chat (as before).
  - Write `CONFIG_HUB_SIDE_CHAT=y` in `hub/sdkconfig.local`. To use a particular side chat, also write
    `CONFIG_HUB_SIDE_CHAT_ID="<UUID>"` (ask Muse and it tells you the ID of an existing side chat; a lowercase UUID).
    Without it, the hub makes one on first boot and keeps it in NVS (a full flash erase makes a new one).
  - After changing it, delete `hub/build-<board>/sdkconfig` and rebuild. The boot log shows `side chat: <ID>` and
    `chat subscription: side chat <ID>`.
  - A side chat that doesn't exist yet is created by the first message. If that first reply is over before the subscription is reopened,
    it is missed (the G2 shows "still thinking"); ask again.
- While a voice turn runs on the Gadget (while its front button is held), typed turns are refused.
- The Gadget alone talks to Muse too: hold the front button and speak (Japanese captions on screen, `CONFIG_MUSE_CJK_FONT`).

Settings (override them in `hub/sdkconfig.local`):

| Kconfig | default | meaning |
|---|---|---|
| `CONFIG_HUB_RELAY_TOKEN` | (none, required) | Even AI's API key (see "Tokens"). Under 32 characters, the HTTP server doesn't start |
| `CONFIG_HUB_HTTP_PORT` | 8787 | listening port |
| `CONFIG_HUB_MDNS_NAME` | `muse-hub` | mDNS name (`<name>.local`) |
| `CONFIG_HUB_WAIT_S` | 25 | seconds to wait for a reply |
| `CONFIG_HUB_MAX_CHARS` | 2000 | characters per page |
| `CONFIG_HUB_ALL_MAX_CHARS` | 5000 | characters returned at once for 「終わりまで」 |
| `CONFIG_HUB_SIDE_CHAT` | n | `y` sends the Gadget's turns to a Muse side chat |
| `CONFIG_HUB_SIDE_CHAT_ID` | (empty) | the side chat's UUID. Empty: make one and keep it in NVS |

## Localizing the keywords and notes

Only the hub's own words are Japanese: Muse answers in the language you speak to it. They are fixed strings (no Kconfig yet);
to use another language, change them in both the hub and the relay so the two keep behaving the same.

| what | hub (C) | relay (Python) |
|---|---|---|
| Paging keywords: next page (「続き」…), all the rest (「終わりまで」…), and the endings allowed after them (を / を見せて …) | `PAGE_CONTINUE_WORDS`, `PAGE_ALL_WORDS`, `PAGE_KEY_SUFFIXES` in `hub/components/hub/hub_proto.c` | the same names in `relay/muse_relay/turns.py` |
| Notes added to replies: still thinking, next page, expired, empty reply | `HUB_CONTINUE_NOTE`, `HUB_PAGE_NOTE`, `HUB_EXPIRED_NOTE`, `HUB_EMPTY_NOTE` in `hub/components/hub/hub_turns.c` | `CONTINUE_NOTE`, `PAGE_NOTE`, `EXPIRED_NOTE`, `EMPTY_NOTE` in `relay/muse_relay/turns.py` |
| Error replies: a turn is running, voice turn on the Gadget, Muse not set up, could not reach Muse | `HUB_STILL_WORKING`, `HUB_MSG_BUSY`, `HUB_MSG_NOT_SET_UP`, `HUB_MSG_INTERNAL` and the `Muse に届きませんでした (…)` format in `hub_turns.c` | `STILL_WORKING`, `MSG_INTERNAL` and the error replies in `turns.py` |
| Widgets that can't be text: "(Muse アプリに表示: …)" ("shown in the Muse app") | `HUB_UNKNOWN_WIDGET`, `SHOWN_HEAD` in `hub_proto.c` | `UNKNOWN_WIDGET`, `SHOWN_HEAD` in `relay/muse_relay/widgets.py` |

- Keywords are matched after removing the characters in `PAGE_KEY_IGNORED` (punctuation and spaces), as a whole utterance (keyword, or keyword + ending), so a new language needs
  its own list of endings ("please", "show me" …) and a check that ordinary questions starting with those words aren't taken as paging.
- The host tests (`hub/test/host/test_hub.c`, `test_turns.c`) and the relay's tests (`relay/tests/`) check these strings; update them too.
- Page cutting prefers to break after the strings in `PAGE_BREAKS` (。！？ and line feeds, in `hub_proto.c`); for other languages add `. `, `! `, `? ` (the relay's `PAGE_BREAKS` is in `relay/muse_relay/openai_api.py`).

## Supported devices

| Device | `BOARD` | Chip / PSRAM | Port | Differences |
|---|---|---|---|---|
| M5Stack StickS3 | `sticks3` (default) | ESP32-S3 / 8 MB | `/dev/cu.usbmodem*` | |
| M5Stack StickC Plus2 | `cplus2` | ESP32 / 2 MB | `/dev/cu.usbserial-*` | Flashed at 230400 baud (its USB-UART bridge drops out above that). Muse speaks through a buzzer, so replies are quiet |
| M5Stack StopWatch | `stopwatch` | ESP32-S3 / 8 MB | `/dev/cu.usbmodem*` | 16 MB of flash (`hub/sdkconfig.hub-stopwatch` picks the 16 MB partition table). The tightest internal RAM |

- All of them reset when their serial port opens.
- The StickC Plus2 has 2 MB of PSRAM, so the voice session's buffers are made smaller (`CONFIG_MUSE_HATCH_COMPACT_BUFFERS=y`
  in `hub/sdkconfig.hub-cplus2`). Without it the typed turns can't connect (`connect failed: Out of memory`).
- With more than one powered on, their mDNS names (`muse-hub.local`) clash. Use one at a time, or change `CONFIG_HUB_MDNS_NAME`.

## For developers

- **Patches to the SDK** (`hub/patches/`; `hub/tools/build.sh build` applies them to the submodule's working tree without changing its commit):
  - `0001`: adds a weak `hub_app_start()` to `main/main.c`, called before `app_run()`.
  - `0002`: adds `muse_hatch_set_listener()`, so the firmware receives typed turns' replies (the `@chat` lines).
  - `0003`: during a typed turn, forwards `delta.presentation` (Muse's widgets) as JSON without the HTML, in `@chat` `presentation` lines.
    The hub and the relay turn lists and maps into bullet lines, the rest into "(Muse アプリに表示: …)", and replace the text's `[[hatch_widget:<id>]]` with them.
  - `0004`: adds `muse_hatch_set_session_id()`. When set, `session_id` goes into the bodies of `/chat/stream` and `/chat/subscribe`
    (and of voice notes) to use the side chat. If it doesn't exist yet (the subscription gets 404), the connection stays and the
    subscription reopens after the first message's ack. Setting it while connected is refused.
- **hub component** (`hub/components/hub/`): `hub_proto` (request parsing, responses, UTF-8, pages, keywords),
  `hub_turns` (turns, holding, paging), `hub_http` (esp_http_server), `hub_side_chat` (the side chat ID), `hub_main` (startup, mDNS).
- **Internal RAM**: the Gadget has little internal RAM (in steady state about 18K free on the StickS3, about 35K on the StickC Plus2, about 20K on the StopWatch). The hub's task stacks and per-request buffers are in PSRAM
  (`hub_mem.h`), and the HTTP server and mDNS start once Muse has been connected for 5 s (setup leaves the RAM to the SDK).
- **Host tests** (pure code and turn handling, the Mac's cc with ASan/UBSan): `hub/test/host/run.sh` (build the hub once first to fetch cJSON).
- **Even AI's requests**: the POST comes from **the Even app on the iPhone itself** (`User-Agent: EvenCore/1.0`), in OpenAI chat completions form
  without `stream`, with only the latest utterance in `messages`. Saving the agent sends a `GET` probe (the hub answers 200).
- **Typed chat over USB** (an SDK tool; run in a shell with ESP-IDF activated, pyserial is in IDF's Python environment).
  Close `monitor` / `flash-monitor` first. The Gadget resets when its serial port opens.

  ```sh
  python hub/third_party/muse-gadget-sdk/esp32/tools/muse/chat.py --port /dev/cu.usbmodemXXXX "question"
  python hub/third_party/muse-gadget-sdk/esp32/tools/muse/chat.py --port /dev/cu.usbmodemXXXX --status
  ```

## Appendix: the relay on a Mac (USB)

`relay/` is a Python version of the hub that runs on a Mac. It talks to the Gadget over USB (tried on the StickS3) and hands turns over with the SDK's `chat.py`.
Use it to:

- try changes to how replies are handled without reflashing (edit the Python and run it).
- serve Even when the iPhone and the Gadget can't share a network (publish the Mac with Tailscale Funnel or on a tailnet).
- tell problems apart (the hub's HTTP side or Muse's).

```
G2 "Hey Even …" → Even app ─POST /v1/chat/completions─▶ relay (Mac) ─USB─▶ Gadget ─▶ Muse
```

```sh
cd relay
python3 -m venv .venv && .venv/bin/pip install -r requirements.txt
# write RELAY_TOKEN=<32+ random characters> in relay/.env (git-ignored; see "Tokens"). The same value as the hub's CONFIG_HUB_RELAY_TOKEN makes switching easy
.venv/bin/python -m muse_relay --port /dev/cu.usbmodemXXXX                 # 127.0.0.1:8787
.venv/bin/python -m muse_relay --port /dev/cu.usbmodemXXXX --listen <Mac's LAN IP>:8787   # serve the LAN directly
tailscale funnel --bg 8787                                                   # publish it on the internet
```

Even's Endpoint is `http://<Mac's LAN IP>:8787/v1/chat/completions` on the LAN, or
`https://<mac>.<tailnet>.ts.net/v1/chat/completions` with Funnel.

- Replies are handled as in "Usage" (25-second wait, pages, keywords, expiry, empty replies). The page length and the 「終わりまで」 limit
  are the constants `MAX_CHARS` / `ALL_MAX_CHARS` in `relay/muse_relay/openai_api.py`.
- Starting the relay opens the serial port, so the Gadget resets once. A Gadget with the hub firmware works as it is.
- Funnel is a public URL and bots will come. Requests without the token are answered 401 / 404 without reading the body. Stop it when done with
  `tailscale funnel --https=443 off`.
- `--capture` saves incoming requests to `relay/captures/` (authentication headers masked).
- Tests: `cd relay && .venv/bin/python -m pytest -q`

## SDK and licenses

`hub/third_party/muse-gadget-sdk` (a submodule,
[facebookincubator/muse-gadget-sdk](https://github.com/facebookincubator/muse-gadget-sdk), Apache-2.0).
`hub/CMakeLists.txt` includes the SDK's `esp32/cmake/project.cmake` and uses the SDK's `main` and `components` as they are.
sdkconfig layers the SDK's `sdkconfig.defaults` → `devices/sdkconfig.muse` →
`devices/sdkconfig.muse-m5stack-<device>` → `hub/sdkconfig.hub` → `hub/sdkconfig.hub-<board>` (if any) → `hub/sdkconfig.local`.

The firmware version is `999.0.0+hub.<hub version>` (e.g. `999.0.0+hub.0.0.1`, `PROJECT_VER` in `hub/CMakeLists.txt`).
OTA is on for Muse boards and installs only an image whose `MAJOR.MINOR.PATCH` is newer, so the SDK's `999.0.0` stays in front
to keep an official Muse image from replacing the hub. The part after `+hub.` is the hub's version (this repo's tag).

Licenses:

- This repo is MIT ([`LICENSE`](LICENSE)).
- Exceptions: `hub/patches/*.patch` change the SDK's files and are Apache-2.0 like the SDK. `hub/third_party/` (submodules) keep their own licenses.
- Japanese captions (`CONFIG_MUSE_CJK_FONT`) use the GNU Unifont bitmaps in the SDK. Unifont is available under the SIL Open Font License 1.1
  or the GPL v2 or later (with the font embedding exception); this repo's firmware uses it under **OFL 1.1**
  (the SDK's `esp32/components/muse/fonts/README.md`). When distributing a built firmware, include Unifont's OFL notice and the
  license notices of ESP-IDF, the SDK and each component.
