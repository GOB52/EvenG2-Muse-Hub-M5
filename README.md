# M5StackMuseEvenG2

Even G2 から、Muse Gadget にした M5Stack を経由して Muse 本人と会話するための実験プロジェクト。
G2 の Even AI (カスタムエージェント) の質問を、StickS3 のテキストターンで Muse に渡し、返事を G2 の HUD に返す。

| dir | 中身 |
|---|---|
| `hub/` | M5Stack StickS3 用 Muse Gadget ファーム (ESP-IDF)。Even AI のエンドポイントを LAN で直接受ける |
| `relay/` | 同じことを Mac で行う版 (StickS3 とは USB)。Python |

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

## hub が Even AI のエンドポイントを直接受ける (Mac 不要)

```
G2「Hey Even …」→ iPhone の Even アプリ (STT はクラウド)
   ─http://muse-hub.local:8787/v1/chat/completions─▶ StickS3 (iPhone のインターネット共有に Wi-Fi 接続) ─テキストターン─▶ Muse
```

Even AI カスタムエージェントの POST は **iPhone の Even アプリ自身** (`User-Agent: EvenCore/1.0`) から届く。
OpenAI chat completions 形式で `stream` なし、`messages` は最新の発話 1 つだけ。保存時に `GET` で確認が来る。

1. **トークン**: `hub/sdkconfig.local` (git-ignored) に、エディタで 1 行足す (32 文字以上の乱数)。
   ```
   CONFIG_HUB_RELAY_TOKEN="<token>"
   ```
   relay と同じ値にすれば、Even 側の API Key を変えずに切り替えられる。足したら `hub/build-sticks3/sdkconfig` を消してビルドし、書き込む。
2. **iPhone のインターネット共有**: 設定 → インターネット共有 で **「互換性を優先」をオン** (StickS3 は 2.4 GHz のみ)。
   ペアリングの間は共有の画面を開いておく (つながっている端末が無いと電波が止まり、StickS3 側で `reason=201` になる)。
3. **ペアリング**: Muse アプリで Add Device → `MuseGadget-XXXXXX`、Wi-Fi は iPhone の共有を選ぶ。
   Wi-Fi を変えるときは、アプリでリンクを解除 → StickS3 のボタンを 5 秒長押し → 追加し直す。
4. **iPhone のローカルネットワーク許可**: 設定 → プライバシーとセキュリティ → ローカルネットワーク で **Even Realities をオン**
   (オフだと HUD に「AI サーバーエラー」)。
5. **Even 側の登録**: [Even Hub](https://hub.evenrealities.com/login) に Even アプリと同じアカウントでサインインし、アプリを完全に終了して
   開き直す (開発者向けの設定が出る)。設定 → Even AI → エージェント設定 (Agent Configuration) で、Endpoint に
   `http://muse-hub.local:8787/v1/chat/completions`、API Key にトークンを入れて Save & Activate。
   `muse-hub.local` が引けないときは、起動ログの `hub: endpoint: http://172.20.10.x:8787/...` の IP を使う。

- hub は Muse への接続が 5 秒続いてから HTTP サーバと mDNS を起動する (セットアップ中は内部 RAM を SDK に譲る)。
- 返事の扱い (25 秒の待ち、「続き」/「終わりまで」のページ送り、保持の期限切れと空の返事の注記) は relay と同じ (下の relay の節)。
  1 ページの長さは `CONFIG_HUB_MAX_CHARS` (既定 2000)、「終わりまで」の上限は `CONFIG_HUB_ALL_MAX_CHARS` (既定 5000)。
- SDK へのパッチ (`hub/patches/`、`hub/tools/build.sh build` が submodule の作業ツリーに当てる。submodule のコミットは変えない):
  - `0001`: `main/main.c` に weak の `hub_app_start()` を足し、`app_run()` の前に呼ぶ。
  - `0002`: `muse_hatch_set_listener()` を足し、テキストターンの返事 (`@chat` の行) をファーム内で受け取れるようにする。
- ホストテスト (純粋処理とターン管理、Mac の cc + ASan/UBSan): `hub/test/host/run.sh` (一度 hub をビルドして cJSON を取得しておく)。

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
  「続き」と話しかけたときに返る (10 分で破棄)。
- 長い返事は 2000 文字ずつのページに分けて返す (G2 はスクロールする)。
  保持中のページは話しかける言葉で決まる (残りは同じく 10 分で破棄)。
  - 「続き」(つづき / 続けて / 次 / もっと): 次のページ。
  - 「終わりまで」(最後まで / 全部 / 全文 / 残り / 続きを全部): 残りをまとめて 5000 文字まで。
  - それ以外: 保持中のページを捨て、新しい質問として Muse に聞く。
  - 句読点と空白を除いた発話が「言葉」か「言葉 + 決まった語尾」(を / を見せて / を教えて / をお願い / をください /
    お願い / 見せて / して / のページ / ページ / 読んで / を読んで など) に一致するときだけページ送りとして扱う。
    「次の予定は?」「手続きの方法は?」「全部屋の温度は?」のような発話は新しい質問になる。
  - 10 分を過ぎて保持が切れた後の「続き」「終わりまで」は Muse に送らず、
    「(続きの保持は期限切れです。もう一度質問してください)」を返す。次の新しい質問でこの状態は消える。
    何も保持していなければ (返事を最後まで返した後など)、普通の質問として Muse に送る。
- ページの長さと「終わりまで」の上限は、relay では `relay/muse_relay/openai_api.py` の定数 `MAX_CHARS` /
  `ALL_MAX_CHARS`、hub では Kconfig の `CONFIG_HUB_MAX_CHARS` / `CONFIG_HUB_ALL_MAX_CHARS`。
- Muse の返事がリンクや画像だけでテキストが無いときは、空の返事の代わりにその旨の注記を返す。
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
