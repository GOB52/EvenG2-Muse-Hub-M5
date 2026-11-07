# M5StackMuseEvenG2

日本語 | [English](README.en.md)

Even G2 から、Muse Gadget にした M5Stack StickS3 を経由して Muse と会話する。
G2 の Even AI (カスタムエージェント) の質問を、StickS3 のテキストターンで Muse に渡し、返事を G2 の HUD に返す。PC は要らない。

```
G2「Hey Even …」→ iPhone の Even アプリ (STT は Even のクラウド)
   ─http://muse-hub.local:8787/v1/chat/completions─▶ StickS3 (iPhone のインターネット共有に Wi-Fi 接続)
   ─テキストターン─▶ Muse ─返事のテキスト─▶ G2 の HUD
```

| dir | 中身 |
|---|---|
| `hub/` | StickS3 用の Muse Gadget ファーム (ESP-IDF)。Even AI のエンドポイントを LAN で受ける |
| `relay/` | 同じことを Mac で行う版 (StickS3 とは USB)。付録を参照 |

## 必要なもの

- M5Stack StickS3、Even Realities G2、iPhone (Even アプリと Muse アプリ)
- ESP-IDF **v6.0.3** (Muse Gadget SDK がこの版のみ対応)
- [Muse の SDK トークン](https://gadgets.muse.ai/settings/sdk-tokens)
- Even AI 用のトークン (32 文字以上の乱数。自分で作る)

## StickS3 (hub) のビルドと書き込み

```sh
git clone --recursive <this repo>        # 既存 clone なら: git submodule update --init
. <esp-idf v6.0.3>/export.sh             # eim なら activate_idf_v6.0.3.sh

# トークンを書く (git-ignored。下の「トークン」を参照)
$EDITOR hub/sdkconfig.local

hub/tools/build.sh build
hub/tools/build.sh flash-monitor /dev/cu.usbmodemXXXX
```

`hub/sdkconfig.hub` / `hub/sdkconfig.local` を変えたとき (`sdkconfig.hub` の変更を pull したときも) は
`hub/build-sticks3/sdkconfig` を消してビルドし直す (sdkconfig の defaults は新しく作る sdkconfig にしか効かない)。

### トークン

- **SDK のトークン** (`CONFIG_GADGET_SDK_TOKEN`): [gadgets.muse.ai](https://gadgets.muse.ai/settings/sdk-tokens) の
  Account > SDK tokens で発行する。取り替えたら Muse アプリでペアリングし直す。
- **relay のトークン** (`CONFIG_HUB_RELAY_TOKEN`、Even AI の API Key): 自分のターミナルで乱数を作る (32 文字以上)。

  ```sh
  openssl rand -base64 32 | tr -d '=+/'    # 40 文字前後
  ```

  同じ値を `hub/sdkconfig.local`、Even アプリのエージェント設定の API Key、relay を使うなら `relay/.env` の
  `RELAY_TOKEN` に入れる (片方だけ変えると HTTP 401 で届かない)。

`hub/sdkconfig.local` の例 (値はダミー):

```
CONFIG_GADGET_SDK_TOKEN="mgst_xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
CONFIG_HUB_RELAY_TOKEN="<上で作った乱数>"
# side chat を使うとき (任意。「使い方」を参照)
# CONFIG_HUB_SIDE_CHAT=y
# CONFIG_HUB_SIDE_CHAT_ID="<UUID>"
```

- `hub/sdkconfig.local` と `relay/.env` は git の対象外。値はエディタで書く (シェルの履歴や AI との会話に打ち込むと記録に残る)。
- 漏れたら作り直し、上の各所を同じ値に変えてから `hub/build-sticks3/sdkconfig` を消してビルドし直す。

## セットアップ

1. **iPhone のインターネット共有**: 設定 → インターネット共有 で **「互換性を優先」をオン** (StickS3 は 2.4 GHz のみ)。
   ペアリングの間は共有の画面を開いておく (つながっている端末が無いと電波が止まり、StickS3 側で `reason=201` になる)。
2. **ペアリング**: Muse アプリの Settings > Devices で Developer mode をオンにし、Add Device から `MuseGadget-XXXXXX` を追加する。
   Wi-Fi は iPhone の共有を選ぶ。Wi-Fi を変えるときは、アプリでリンクを解除 → StickS3 のボタンを 5 秒長押し → 追加し直す。
3. **iPhone のローカルネットワーク許可**: 設定 → プライバシーとセキュリティ → ローカルネットワーク で **Even Realities をオン**
   (オフだと HUD に「AI サーバーエラー」)。
4. **Even 側の登録**: [Even Hub](https://hub.evenrealities.com/login) に Even アプリと同じアカウントでサインインし、アプリを完全に
   終了して開き直す (開発者向けの設定が出る)。設定 → Even AI → エージェント設定 (Agent Configuration) で、Endpoint に
   `http://muse-hub.local:8787/v1/chat/completions`、API Key に `CONFIG_HUB_RELAY_TOKEN` の値を入れて Save & Activate。
   `muse-hub.local` が引けないときは、起動ログの `hub: endpoint: http://172.20.10.x:8787/...` の IP を使う。

## 使い方

G2 で「Hey Even」と話しかける。返事は Muse から、テキストで HUD に出る (G2 にはテキストで返る。StickS3 で話したときは返事の音声も StickS3 で再生される)。

- **25 秒の待ち**: Even AI は約 30 秒で諦めるので、25 秒で返事が揃わなければ届いた分と
  「(まだ考え中です。「続き」と話しかけると続きを返します)」を返す。返事は保持され、「続き」で返る (10 分で破棄)。
- **ページ送り**: 長い返事は 1 ページ (既定 2000 文字、G2 はスクロールする) ずつ返し、ページの末尾に
  「(続きは「続き」、残り全部は「終わりまで」と話しかけてください)」を付ける。保持中のページは話しかける言葉で決まる。
  - 「続き」(つづき / 続けて / 次 / もっと): 次のページ。
  - 「終わりまで」(最後まで / 全部 / 全文 / 残り / 続きを全部): 残りをまとめて (既定 5000 文字まで)。
  - それ以外: 保持中のページを捨て、新しい質問として Muse に聞く。
  - 句読点と空白を除いた発話が「言葉」か「言葉 + 決まった語尾」(を / を見せて / を教えて / をお願い / をください /
    お願い / 見せて / して / のページ / ページ / 読んで / を読んで など) に一致するときだけページ送りとして扱う。
    「次の予定は?」「手続きの方法は?」「全部屋の温度は?」のような発話は新しい質問になる。
  - 10 分を過ぎて保持が切れた後の「続き」「終わりまで」は Muse に送らず、
    「(続きの保持は期限切れです。もう一度質問してください)」を返す。次の新しい質問でこの状態は消える。
    何も保持していなければ (返事を最後まで返した後など)、普通の質問として Muse に送る。
- **ウィジェット**: Muse の返事のリストは「見出し + ・項目 — 説明」、地図は「説明 + ・場所 — 日時など」の文字にして返す。
  画像の選択肢・HTML のウィジェット・タップで返信するボタンなど文字にできないものは「(Muse アプリに表示: …)」になる。
- **空の返事**: Muse の返事がリンクや画像だけでテキストもウィジェットも無いときは、「(Muse の返事にテキストがありませんでした。…)」を返す。
- **side chat** (`CONFIG_HUB_SIDE_CHAT=y` のとき): StickS3 の会話 (G2 からも StickS3 で話したものも) を Muse の
  メインのチャットではなく side chat 1 つに送り、返事もそこから受け取る。Muse アプリで G2 の会話が分かれ、
  メインのチャットで同時に打った質問の答えと取り違えない。既定はメインのチャット (今までどおり)。
  - `hub/sdkconfig.local` に `CONFIG_HUB_SIDE_CHAT=y` を書く。決まった side chat に送るなら
    `CONFIG_HUB_SIDE_CHAT_ID="<UUID>"` も書く (Muse に聞くと作成済みの side chat の ID を教えてくれる。小文字の UUID)。
    書かなければ初回の起動で作って NVS に保存し、以後それを使う (フラッシュを全消去すると作り直す)。
  - 書き換えたら `hub/build-sticks3/sdkconfig` を消してビルドし直す。起動ログの `side chat: <ID>` と
    `chat subscription: side chat <ID>` で確かめられる。
  - まだ無い side chat は最初の 1 通で作られる。その 1 通目の返事が受信を開き直す前に終わると取りこぼす
    (G2 には「まだ考え中」が出る)。そのときはもう一度聞く。
- StickS3 で音声の会話中 (正面のボタンを押して話している間) はテキストのターンを受け付けない。
- StickS3 単体でも、正面のボタンを押して話せば Muse と会話できる (画面に日本語で表示。`CONFIG_MUSE_CJK_FONT`)。

設定 (`hub/sdkconfig.local` に書いて上書きできる):

| Kconfig | 既定 | 意味 |
|---|---|---|
| `CONFIG_HUB_RELAY_TOKEN` | (なし、必須) | Even AI の API Key (作り方は「トークン」)。32 文字未満なら HTTP サーバを起動しない |
| `CONFIG_HUB_HTTP_PORT` | 8787 | 待ち受けポート |
| `CONFIG_HUB_MDNS_NAME` | `muse-hub` | mDNS 名 (`<name>.local`) |
| `CONFIG_HUB_WAIT_S` | 25 | 返事を待つ秒数 |
| `CONFIG_HUB_MAX_CHARS` | 2000 | 1 ページの文字数 |
| `CONFIG_HUB_ALL_MAX_CHARS` | 5000 | 「終わりまで」で一度に返す文字数 |
| `CONFIG_HUB_SIDE_CHAT` | n | `y` で StickS3 の会話を Muse の side chat に送る |
| `CONFIG_HUB_SIDE_CHAT_ID` | (空) | side chat の UUID。空なら作って NVS に保存 |

## 開発者向け

- **SDK へのパッチ** (`hub/patches/`。`hub/tools/build.sh build` が submodule の作業ツリーに当てる。submodule のコミットは変えない):
  - `0001`: `main/main.c` に weak の `hub_app_start()` を足し、`app_run()` の前に呼ぶ。
  - `0002`: `muse_hatch_set_listener()` を足し、テキストターンの返事 (`@chat` の行) をファーム内で受け取れるようにする。
  - `0003`: テキストターン中の `delta.presentation` (Muse のウィジェット) を、HTML を除いた JSON にして `@chat` の `presentation` 行で流す。
    hub と relay はリスト・地図を箇条書きに、それ以外を「(Muse アプリに表示: …)」にして返事に入れ、本文の `[[hatch_widget:<id>]]` もそれに置き換える。
  - `0004`: `muse_hatch_set_session_id()` を足す。設定すると `/chat/stream` と `/chat/subscribe` の本文に `session_id` を入れて
    side chat を使う (音声ノートの本文にも)。未作成 (受信が 404) なら接続を切らず、最初のメッセージの ack の後に受信を開き直す。
    接続中の設定は受け付けない。
- **hub component** (`hub/components/hub/`): `hub_proto` (リクエスト解釈・応答生成・UTF-8・ページ分割・言葉の判定)、
  `hub_turns` (ターン管理・保持・ページ送り)、`hub_http` (esp_http_server)、`hub_side_chat` (side chat の ID)、`hub_main` (起動・mDNS)。
- **内部 RAM**: StickS3 は内部 RAM が少ない (定常で約 18K 空き)。hub のタスクのスタックとリクエストごとのバッファは PSRAM に置き
  (`hub_mem.h`)、HTTP サーバと mDNS は Muse への接続が 5 秒続いてから起動する (セットアップ中は SDK に譲る)。
- **ホストテスト** (純粋処理とターン管理、Mac の cc + ASan/UBSan): `hub/test/host/run.sh` (一度 hub をビルドして cJSON を取得しておく)。
- **Even AI のリクエスト**: POST は **iPhone の Even アプリ自身** (`User-Agent: EvenCore/1.0`) から届く。OpenAI chat completions 形式で
  `stream` なし、`messages` は最新の発話 1 つだけ。保存時に `GET` で確認が来る (hub は 200 を返す)。
- **USB 越しのテキスト会話** (SDK 付属ツール。ESP-IDF を activate したシェルで実行する。pyserial は IDF の Python 環境に入っている)。
  ポートを使う `monitor` / `flash-monitor` は先に閉じる。StickS3 はシリアルポートを開くとリセットされる。

  ```sh
  python hub/third_party/muse-gadget-sdk/esp32/tools/muse/chat.py --port /dev/cu.usbmodemXXXX "質問"
  python hub/third_party/muse-gadget-sdk/esp32/tools/muse/chat.py --port /dev/cu.usbmodemXXXX --status
  ```

## 付録: Mac の relay (USB 版)

`relay/` は、hub と同じことを Mac の上で行う Python 版。StickS3 とは USB でつなぎ、SDK の `chat.py` でテキストターンを渡す。
次のような場面で使う。

- 返事の扱いを、ファームを書き換えずに試す (Python を直してすぐ動かせる)。
- iPhone と StickS3 を同じネットワークに置けないとき (Mac を Tailscale Funnel や tailnet で公開する)。
- 不具合の切り分け (hub の HTTP 側か、Muse 側か)。

```
G2「Hey Even …」→ Even アプリ ─POST /v1/chat/completions─▶ relay (Mac) ─USB─▶ StickS3 ─▶ Muse
```

```sh
cd relay
python3 -m venv .venv && .venv/bin/pip install -r requirements.txt
# relay/.env に RELAY_TOKEN=<32 文字以上の乱数> を書く (git-ignored、作り方は「トークン」)。hub の CONFIG_HUB_RELAY_TOKEN と同じ値にすると切り替えが楽
.venv/bin/python -m muse_relay --port /dev/cu.usbmodemXXXX                 # 127.0.0.1:8787
.venv/bin/python -m muse_relay --port /dev/cu.usbmodemXXXX --listen <Mac の LAN IP>:8787   # LAN で直接受ける場合
tailscale funnel --bg 8787                                                   # インターネットに公開する場合
```

Even 側の Endpoint は、LAN なら `http://<Mac の LAN IP>:8787/v1/chat/completions`、Funnel なら
`https://<mac>.<tailnet>.ts.net/v1/chat/completions`。

- 返事の扱い (25 秒の待ち、ページ送り、言葉、期限切れ、空の返事) は「使い方」と同じ。ページの長さと「終わりまで」の上限は、
  `relay/muse_relay/openai_api.py` の定数 `MAX_CHARS` / `ALL_MAX_CHARS`。
- relay を起動するとシリアルポートを開くので、StickS3 は 1 回リセットされる。hub のファームが入った StickS3 でも、そのまま使える。
- Funnel は公開 URL なので、ボットのアクセスが来る。認証の無いリクエストは本文を読まずに 401 / 404 で返す。使い終わったら
  `tailscale funnel --https=443 off` で止める。
- `--capture` で届いたリクエストを `relay/captures/` に保存する (認証系のヘッダは伏せる)。
- テスト: `cd relay && .venv/bin/python -m pytest -q`

## SDK とライセンス

`hub/third_party/muse-gadget-sdk` (submodule、
[facebookincubator/muse-gadget-sdk](https://github.com/facebookincubator/muse-gadget-sdk)、Apache-2.0)。
`hub/CMakeLists.txt` が SDK の `esp32/cmake/project.cmake` を取り込み、SDK の `main` と `components`
をそのまま使う。sdkconfig は SDK の `sdkconfig.defaults` → `devices/sdkconfig.muse` →
`devices/sdkconfig.muse-m5stack-sticks3` → `hub/sdkconfig.hub` → `hub/sdkconfig.local` の順に重ねる。

ライセンス:

- この repo は MIT ([`LICENSE`](LICENSE))。
- 例外: `hub/patches/*.patch` は SDK のファイルの変更なので、SDK と同じ Apache-2.0。`hub/third_party/` (submodule) はそれぞれのライセンス。
- 日本語の表示 (`CONFIG_MUSE_CJK_FONT`) は SDK に入っている GNU Unifont のビットマップを使う。Unifont は SIL Open Font License 1.1
  と GPL v2 以上 (font embedding exception 付き) のどちらでも使え、この repo のファームは **OFL 1.1** で使う
  (SDK の `esp32/components/muse/fonts/README.md`)。ビルドしたファームを配るときは、Unifont の OFL の表示と、
  ESP-IDF・SDK・各 component のライセンス表示を同梱する。
