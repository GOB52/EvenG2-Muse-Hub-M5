# M5StackMuseEvenG2

Even G2 から、Muse Gadget にした M5Stack を経由して Muse 本人と会話するための実験プロジェクト。
G2 の Even AI (カスタムエージェント) の質問を、StickS3 のテキストターンで Muse に渡し、返事を G2 の HUD に返す。

| dir | 中身 |
|---|---|
| `hub/` | M5Stack StickS3 用 Muse Gadget ファーム (ESP-IDF) |
| `relay/` | Even AI カスタムエージェント → StickS3 (USB) → Muse のテキスト relay (Python) |

## hub のビルド

必要なもの: ESP-IDF **v6.0.3** (Muse Gadget SDK がこの版のみ対応)、M5Stack StickS3、
[SDK トークン](https://gadgets.muse.ai/settings/sdk-tokens)。

```sh
git clone --recursive <this repo>        # 既存 clone なら: git submodule update --init
. <esp-idf v6.0.3>/export.sh             # eim なら activate_idf_v6.0.3.sh

# SDK トークン (git-ignored)。エディタで次の 1 行を書く:
#   CONFIG_GADGET_SDK_TOKEN="mgst_…"
$EDITOR hub/sdkconfig.local

hub/tools/build.sh build
hub/tools/build.sh flash-monitor /dev/cu.usbmodemXXXX
```

`hub/sdkconfig.hub` / `hub/sdkconfig.local` を変えたとき (`sdkconfig.hub` の変更を pull したときも) は
`hub/build-sticks3/sdkconfig` を消してビルドし直す (sdkconfig の defaults は新しく作る sdkconfig にしか効かない)。

ペアリング: Muse アプリの Settings > Devices で Developer mode を有効にし、Add Device から
`MuseGadget-XXXXXX` を追加する。

## 動作

- 正面のボタンを押している間に話すと、Muse に voice note として届く。
- Muse の返事は**テキストのみ** (Muse はガジェットに音声を返さない)。画面に日本語で表示される
  (`hub/sdkconfig.hub` で `CONFIG_MUSE_CJK_FONT` を有効にしている)。
- USB 越しのテキスト会話 (SDK 付属ツール。ESP-IDF を activate したシェルで実行する。pyserial は IDF の
  Python 環境に入っている)。ポートを使う `monitor` / `flash-monitor` は先に閉じる:

  ```sh
  python hub/third_party/muse-gadget-sdk/esp32/tools/muse/chat.py --port /dev/cu.usbmodemXXXX "質問"
  python hub/third_party/muse-gadget-sdk/esp32/tools/muse/chat.py --port /dev/cu.usbmodemXXXX --status
  ```

## relay (Even AI → Muse)

```
G2「Hey Even …」→ Even アプリ (STT はクラウド) ─POST /v1/chat/completions─▶ relay (Mac) ─USB─▶ StickS3 ─▶ Muse
```

Even AI のカスタムエージェントの POST は、Even のクラウドではなく**スマホの Even アプリ** (`User-Agent: EvenCore/1.0`)
から届く。本文は OpenAI chat completions 形式で、`stream` なし、`messages` は最新の発話 1 つだけ。

```sh
cd relay
python3 -m venv .venv && .venv/bin/pip install -r requirements.txt
# relay/.env に RELAY_TOKEN=<32 文字以上の乱数> を書く (git-ignored)
.venv/bin/python -m muse_relay --port /dev/cu.usbmodemXXXX     # 127.0.0.1:8787
tailscale funnel --bg 8787                                       # https://<mac>.<tailnet>.ts.net
```

Even 側の登録:

1. [Even Hub](https://hub.evenrealities.com/login) に Even アプリと同じアカウントでサインインし、アプリを完全に終了して
   開き直す (これで開発者向けの設定が出る)。
2. Even アプリの 設定 → Even AI → エージェント設定 (Agent Configuration) で、Endpoint に
   `https://<mac>.<tailnet>.ts.net/v1/chat/completions`、API Key に `RELAY_TOKEN` の値を入れて Save & Activate。

- relay を起動するとシリアルポートを開くので、StickS3 は 1 回リセットされる。
- Even AI は約 30 秒で諦めるので、25 秒で返事が揃わなければ届いた分と「まだ考え中」を返す。返事は保持され、
  次に話しかけたとき (内容は問わない) に返る (10 分で破棄)。返事は 2000 文字で切る (G2 はスクロールする。hub では `CONFIG_HUB_MAX_CHARS` で変えられる)。
- StickS3 で音声の会話中はテキストのターンを受け付けない。
- Funnel は公開 URL なので、ボットのアクセスが来る。認証の無いリクエストは本文を読まずに 401 / 404 で返す。
- `--capture` で届いたリクエストを `relay/captures/` に保存する (認証系のヘッダは伏せる)。
- テスト: `cd relay && .venv/bin/python -m pytest -q`

## SDK

`hub/third_party/muse-gadget-sdk` (submodule、
[facebookincubator/muse-gadget-sdk](https://github.com/facebookincubator/muse-gadget-sdk)、Apache-2.0)。
`hub/CMakeLists.txt` が SDK の `esp32/cmake/project.cmake` を取り込み、SDK の `main` と `components`
をそのまま使う。sdkconfig は SDK の `sdkconfig.defaults` → `devices/sdkconfig.muse` →
`devices/sdkconfig.muse-m5stack-sticks3` → `hub/sdkconfig.hub` → `hub/sdkconfig.local` の順に重ねる。
