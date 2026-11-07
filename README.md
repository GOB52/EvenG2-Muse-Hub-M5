# M5StackMuseEvenG2

Even G2 のマイク音声を、Muse Gadget にした M5Stack に中継して Muse と会話するための実験プロジェクト。

| dir | 中身 |
|---|---|
| `hub/` | M5Stack StickS3 用 Muse Gadget ファーム (ESP-IDF) |

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

## SDK

`hub/third_party/muse-gadget-sdk` (submodule、
[facebookincubator/muse-gadget-sdk](https://github.com/facebookincubator/muse-gadget-sdk)、Apache-2.0)。
`hub/CMakeLists.txt` が SDK の `esp32/cmake/project.cmake` を取り込み、SDK の `main` と `components`
をそのまま使う。sdkconfig は SDK の `sdkconfig.defaults` → `devices/sdkconfig.muse` →
`devices/sdkconfig.muse-m5stack-sticks3` → `hub/sdkconfig.hub` → `hub/sdkconfig.local` の順に重ねる。
