# fcitx5 による日本語入力（試作）

CardputerZero のランチャー（APPLaunch）と ZClaw で、fcitx5 を使った日本語入力（かな漢字変換）を行うための試作です。
ディスプレイサーバー（X11/Wayland）は使いません。fcitx5 は D-Bus のセッションバスだけで動かし、未確定文字と変換候補の表示は `cp0_lvgl` が LVGL で描画します。

## 仕組み

```
TCA8418 キーボード ─▶ cp0_lvgl キーキュー ─▶ keypad_read()
                                              │ ① スクリーンセーバー
                                              │ ② key_filter（低バッテリー画面など）
                                              │ ③ cp0_ime_filter_key()  ← 追加
                                              │      │ 文字入力モード（KBD_INPUT_CONTEXT_TEXT）のときだけ
                                              │      ▼
                                              │   fcitx5（D-Bus: org.fcitx.Fcitx.InputContext1.ProcessKeyEvent）
                                              │      │ true  → キーは IME が使用（ページには届かない）
                                              │      │ false → そのままページへ
                                              ▼      ▼
                                   ページ（LV_EVENT_KEYBOARD）◀─ CommitString（確定文字）
                                   lv_layer_top() に未確定文字・候補を描画 ◀─ UpdateClientSideUI
```

- fcitx5 の DBus Frontend（`org.fcitx.Fcitx.InputMethod1` / `InputContext1`）に対して入力コンテキストを1つ作ります。`ClientSideInputPanel`（bit 39）を指定し、候補ウィンドウの描画をクライアント側で引き受けます。
- 確定文字は、1文字ずつ `LV_EVENT_KEYBOARD` の `key_item` としてページに渡します（`key_code=0`, `key_state=PRESSED`, `utf8`=1文字, `sym_name="IME_COMMIT"`）。既存のページは `item->utf8` を追記するだけなので、改修は不要です。
- fcitx5 は `CommitString` を ProcessKeyEvent の応答より先に送ります。クライアントは応答を受け取った直後にシグナルを処理するので、確定文字が後続のキーより後ろにずれることはありません。
- fcitx5 が起動していない場合は、キーをそのまま通します。fcitx5 が後から起動・再起動した場合も自動で再接続します。
- fcitx5 が応答しない場合（既定のタイムアウト 800ms、`CP0_IME_KEY_TIMEOUT_MS` で変更可）は、そのキーをページに渡し、**再同期**に入ります。再同期では `Reset` を送り、その応答が返るまでの間は次のように動きます。
  - キーは fcitx5 に問い合わせず、待たずにページへ渡す（1打鍵ごとに待たされない）
  - 遅れて届く `CommitString`・`ForwardKey`・パネル更新は捨てる（タイムアウトしたキーを fcitx5 が後から処理しても、同じ文字が二重に入らない）

  D-Bus は同じ相手からのメッセージの順序を保つため、`Reset` の応答は、それより前のキーに対するシグナルがすべて届いた後に返ります。応答が返った時点で通常の動作に戻ります。

## 変更ファイル

| ファイル | 内容 |
|---|---|
| `ext_components/cp0_lvgl/include/cp0_ime.h` | 公開 API（抑止、状態取得） |
| `ext_components/cp0_lvgl/src/ime/cp0_fcitx5_client.{hpp,cpp}` | fcitx5 D-Bus クライアント（GIO）。タイムアウト時の再同期もここで行う |
| `ext_components/cp0_lvgl/src/ime/cp0_ime_fcitx5.cpp` | LVGL 側ブリッジ（フォーカス管理、キー変換、確定文字の配送、候補パネル描画） |
| `ext_components/cp0_lvgl/src/cp0_ime_util.{h,c}` | キーシンボル・修飾キーの変換、候補の表示範囲計算（純粋関数） |
| `ext_components/cp0_lvgl/src/cp0_ime_stub.c` | 機能無効時の空実装 |
| `ext_components/cp0_lvgl/src/cp0_keyboard_deliver.h` | 合成キーをページへ渡す内部 API |
| `ext_components/cp0_lvgl/src/cp0/cp0_keyboard_lvgl_input.c`, `…/src/sdl/sdl_lvgl_keyboard.c` | キー処理への IME 段の追加、初期化・終了処理 |
| `ext_components/cp0_lvgl/Kconfig`, `SConstruct` | `CONFIG_CP0_LVGL_IME_FCITX5` オプション |
| `projects/APPLaunch/*config_defaults.mk`, `projects/ZClaw/*config_defaults.mk` | 上記オプションを有効化（実機・クロス・Linux SDL の各プロファイル） |
| `projects/APPLaunch/main/ui/settings/settings_wifi_page.cpp` | SSID・パスワード入力中は IME を抑止 |
| `projects/APPLaunch/main/ui/settings/settings_bluetooth_page.cpp` | ペアリングの PIN・パスキー入力中は IME を抑止（名前変更では IME を使える） |
| `projects/APPLaunch/main/ui/page_app/ui_app_ssh.cpp` | 接続先フォーム（Host/Port/User）では IME を抑止。ターミナル起動前に入力コンテキストを戻すよう順序を修正（下記） |
| `projects/APPLaunch/main/ui/page_app/ui_app_st*.{hpp,cpp}` | CLI（ターミナル）はシェル実行中だけ文字入力モードにする |
| `projects/ZClaw/main/ui/zclaw_input_dialog.{h,cpp}` | チャット以外（API キー、URI、ペアリングコード）の入力中は IME を抑止 |
| `projects/CardputerZero-Emulator/CMakeLists.txt` | Web/Windows 版エミュレータに空実装を追加 |
| `scripts/ime/fcitx5-cardputer.service` | fcitx5 用 systemd ユーザーユニット |
| `scripts/ime/install_fcitx5_ime.sh` | 実機用セットアップスクリプト |
| `ext_components/cp0_lvgl/tests/test_ime_util.c`, `tests/ime/*` | 単体テスト、モック fcitx5、LVGL 結合テスト |
| `projects/APPLaunch/tests/bluetooth_ui/test_bluetooth_ui.cpp` | ペアリング入力での IME 抑止の確認 |

## 実機への組み込み手順

前提：OS は CardputerZero 公式イメージ（Raspberry Pi OS trixie ベース）で、ネットワークに接続済み、SSH または ADB でシェルが使えること。

**ユーザー名について**：APPLaunch は UID 1000 のアカウントの systemd ユーザーサービスとして動いています。このアカウントはイメージ作成時に `pi` として作られ、初回セットアップ（LaunchWizard）や Raspberry Pi Imager で付けた名前に変更されます。そのため、ユーザー名は機種ごとに異なります。以下の `<user>` は、次のコマンドで確認した名前に置き換えてください。

```bash
getent passwd 1000 | cut -d: -f1
```

### 1. fcitx5 と変換エンジンを入れる（実機で実行）

リポジトリの `scripts/ime/` を実機にコピーして実行します。

```bash
# PC から（SSH の場合）
scp -r scripts/ime <user>@<デバイスのIP>:~/ime
# 実機で（<user> としてログインして実行）
sh ~/ime/install_fcitx5_ime.sh mozc      # anthy / skk も選べます
```

ADB などで root のシェルから実行しても構いません。その場合、スクリプトは UID 1000 のアカウントを自動で探し、パッケージの導入は root で、設定とサービス登録はそのアカウントで行います（`runuser` を使用）。それ以外のユーザーで実行すると、エラーで止まります。

スクリプトが行うこと：

1. `apt-get install --no-install-recommends fcitx5 fcitx5-mozc dbus-user-session`（GTK/Qt 用フロントエンドや設定 GUI は入れません）
2. `<user>` のホームの `~/.config/fcitx5/profile` に「keyboard-us + mozc」の入力メソッドグループを書き込む（既存のファイルは `profile.bak` に退避）
3. `~/.config/systemd/user/fcitx5-cardputer.service` を配置して、`<user>` のユーザーサービスとして有効化・起動（X/Wayland 関連のアドオンはすべて無効）
4. セッションバス上に `org.fcitx.Fcitx5` が現れたことを確認し、APPLaunch を再起動

`/run/user/1000/bus`（`<user>` のセッションバス）がまだない場合は、スクリプトが再起動を求めて止まります。再起動してからもう一度実行してください。

手動で行う場合は、上の1〜3を同じ内容で実施します。エンジンの選び方は次のとおりです。

| エンジン | パッケージ | 特徴 |
|---|---|---|
| mozc | `fcitx5-mozc` | 変換精度が高い。mozc_server が常駐するのでメモリ消費が最も大きい |
| anthy | `fcitx5-anthy` | 軽め。文節単位で変換できる |
| skk | `fcitx5-skk skkdic` | 最も軽い。SKK 方式なので操作体系が異なる |

512MB の機種なので、mozc を使う場合は `free -m` で空きメモリを確認してください。

### 2. APPLaunch をビルドする（PC でクロスコンパイル）

```bash
# この変更を含むブランチ（feature/jpinput）をチェックアウトした状態で
git submodule update --init --recursive
cd projects/APPLaunch
export CONFIG_DEFAULT_FILE=linux_x86_cross_cp0_config_defaults.mk   # macOS は mac_cross_cp0_config_defaults.mk
scons distclean
scons -j8
```

`CONFIG_CP0_LVGL_IME_FCITX5=y` は各 `*config_defaults.mk` に追加済みです。既存のビルドディレクトリを使い回す場合は、`scons menuconfig` で「Enable fcitx5 input method (IME) bridge」がオンになっていることを確認してください（設定キャッシュが残っていると反映されないため、`scons distclean` を推奨します）。

実機上でビルドする場合は、`projects/APPLaunch` で `scons distclean && scons -j4` を実行します（`config_defaults.mk` が使われます）。

### 3. 実機に入れ替える

**`scons push` を使う場合**：`projects/APPLaunch/setup.ini` を実機に合わせてから `scons push` を実行します。`dist/` が転送され、`/usr/share/APPLaunch` への配置と APPLaunch の再起動まで自動で行われます。upstream の `setup.ini` は `pi` / パスワード `pi` 前提なので、次の箇所を書き換えてください。

- `remote_host`：実機の IP
- `username`、`password`：`<user>` とそのパスワード
- `remote_file_path`：`/home/<user>/dist`
- `after_cmd`：中の `echo pi | sudo -S` を `echo <パスワード> | sudo -S` に、`/home/pi/dist` を `/home/<user>/dist` に

**手動の場合**：

```bash
# PC から
scp projects/APPLaunch/dist/M5CardputerZero-APPLaunch <user>@<デバイスのIP>:/tmp/
# 実機で（<user> として）
sudo cp -a /usr/share/APPLaunch/bin/M5CardputerZero-APPLaunch /usr/share/APPLaunch/bin/M5CardputerZero-APPLaunch.orig
sudo install -m 755 /tmp/M5CardputerZero-APPLaunch /usr/share/APPLaunch/bin/M5CardputerZero-APPLaunch
systemctl --user restart APPLaunch.service
```

手動の場合は、バイナリとは別にリソースファイルもコピーしてください。Memo のアイコン `memo_100.png` は今回新しく追加したファイルなので、実機にはまだありません（`scons push` なら `dist/` ごと転送されるので不要です）。

```bash
# PC から
scp projects/APPLaunch/dist/APPLaunch/share/images/memo_100.png <user>@<デバイスのIP>:/tmp/
# 実機で（<user> として）
sudo install -m 644 /tmp/memo_100.png /usr/share/APPLaunch/share/images/memo_100.png
systemctl --user restart APPLaunch.service
```

ホーム画面の Memo のアイコンが M5Stack のロゴになっている場合は、このファイルが実機にありません。ロゴは、アイコン画像を読み込めなかったときに表示される代替アイコンです。

ZClaw も日本語入力を使う場合は、`projects/ZClaw` で同じようにビルドし、`/usr/share/APPLaunch/bin/ZClaw` を差し替えてください。

> 公式リリースによる APPLaunch の自動更新が走ると、差し替えたバイナリは上書きされます。

### 4. 動作確認

`systemctl --user` と `journalctl --user` は `<user>` として実行します（root のシェルからは `runuser -u <user> -- env XDG_RUNTIME_DIR=/run/user/1000 journalctl --user -u APPLaunch -b` のようにします）。

```bash
journalctl --user -u APPLaunch -b | grep IME
#   [IME] input context ready at /org/freedesktop/portal/inputcontext/N   ← 接続成功
busctl --user call org.fcitx.Fcitx5 /controller org.fcitx.Fcitx.Controller1 DebugInfo
#   "program:M5CardputerZero-APPLaunch frontend:dbus" の IC が見えれば接続済み
#   （program には実行ファイル名が入る。ZClaw なら "program:ZClaw"）
```

ホーム画面の「Memo」（日本語で書けるメモ帳）、設定 → Bluetooth の名前変更、CLI（ターミナル）などの文字入力欄で操作します。Memo のメモは `~/Documents/Memo/memo-YYYYMMDD-HHMMSS.txt`（UTF-8）に保存されます。CLI はシェルの実行中だけ文字入力モードになり、ヘルプ表示中や「Press any key to exit」の待ち状態では IME は働きません。

CLI で確定した日本語が `\346\227\245` のような表示になる場合は、シェルのロケールが UTF-8 になっていません。CLI で `locale` を確認し、`LANG=C` などになっていれば `~/.bashrc` に `export LANG=C.UTF-8` を追加してください。

- 入力欄にフォーカスすると右下に入力モードの表示が出ます（`A` = 直接入力、`あ` = 日本語）
- **Ctrl+Space** で日本語入力と直接入力を切り替え（fcitx5 標準のトリガーキー）
- ローマ字で入力 → 画面下部に下線付きの未確定文字
- **Space** で変換／次の候補、**Enter** で確定、**Esc** で取り消し、**Backspace** で1文字削除（ここまでは mozc の標準操作です。候補を数字キーで選ぶ、矢印で文節や候補を移動する、といった操作もエンジン側の機能としてそのまま使えます）
- 未確定文字がないときの Esc や Backspace は、従来どおりページに届きます（画面を閉じる、文字を消す）

## 設定

### APPLaunch 側（環境変数）

`systemctl --user edit APPLaunch.service` で `[Service]` に `Environment=...` を追加します。

| 変数 | 既定値 | 内容 |
|---|---|---|
| `CP0_IME` | 有効 | `0` で IME ブリッジを無効化 |
| `CP0_IME_DEBUG` | 無効 | `1` でキーごとのログを出力 |
| `CP0_IME_KEY_TIMEOUT_MS` | `800` | fcitx5 の応答を待つ時間（ms、50〜10000）。超えると再同期に入る（「仕組み」を参照） |
| `CP0_IME_FONT` | 自動 | 候補表示のフォント（既定では `share/font/NotoSansCJK-Regular.ttc` → `/usr/share/APPLaunch/share/font/…` → `/usr/share/fonts/opentype/noto/…` の順に探す） |
| `CP0_IME_FONT_SIZE` | `14` | 候補表示の文字サイズ（px） |
| `CP0_IME_BADGE` | 表示 | `0` で右下のモード表示を隠す |

### fcitx5 側

- 切り替えキーを変える：`~/.config/fcitx5/config` に以下のように書き、`systemctl --user restart fcitx5-cardputer` で反映します。
  ```ini
  [Hotkey/TriggerKeys]
  0=Control+space
  1=Shift+space
  ```
- エンジンを変える：`sh install_fcitx5_ime.sh anthy` を再実行するか、`profile` の `DefaultIM` と `Items/1` の `Name` を書き換えます。
- fcitx5 は入力コンテキスト単位で状態を持ち、APPLaunch 全体で1つのコンテキストを使います。そのため、日本語モードは入力欄をまたいで引き継がれます。

## アプリ側 API（`cp0_ime.h`）

ページの改修は不要です。生の文字列を入力させたい欄では、次の関数で IME を外せます。

```c
cp0_ime_push_suppress();   // パスワード欄などに入るとき
cp0_ime_pop_suppress();    // 出るとき（入れ子にできます）
cp0_ime_get_state();       // UNAVAILABLE / DIRECT / COMPOSING
```

次の入力欄は、この API で IME を抑止しています。

- Wi-Fi 設定の SSID・パスワード
- Bluetooth のペアリング（PIN・パスキー）
- SSH の接続先フォーム（Host・Port・User）
- ZClaw のチャット以外の入力（API キー、URI、ペアリングコード）

入力コンテキストを保存・復元するページの中から、同じように保存・復元する別のページ（CLI など）を開く場合は、**子ページを開く前に親ページ側の保存を戻してください**。保存と復元が入れ子になっていないと、子ページが閉じるときに親の文字入力モードを復元してしまいます。その結果、ホーム画面（文字入力モードではキーを無視する）が操作できなくなります。SSH ページはこの順序に修正済みです。機能を無効にしたビルドでも空実装がリンクされるので、そのまま呼び出せます。

## テスト

```bash
# 単体テスト + fcitx5 プロトコルテスト（モック fcitx5 を専用のセッションバスで起動）
sh ext_components/cp0_lvgl/tests/run_tests.sh

# LVGL を含む結合テスト（ヘッドレス 320x170、PNG スクリーンショットを出力）
sh ext_components/cp0_lvgl/tests/ime/run_lvgl_sim.sh <LVGL 9.5 のディレクトリ> <lv_conf.h のディレクトリ> /tmp/ime_shots
```

プロトコルテストでは、fcitx5 が応答に間に合わなかった場合の再同期（遅れて届いた確定文字を捨てる、再同期中はキーを待たずに通す）も確認します。モックは、テスト専用の state ビット（bit 24）が付いたキーを受け取ると、処理の前に 400ms 停止します。

`tests/ime/mock_fcitx5_server.c` は、fcitx5 の `src/frontend/dbusfrontend/dbusfrontend.cpp` と同じシグネチャで最小限のメソッドとシグナルを実装した模擬サーバーです。ローマ字かな変換と3語の辞書を持ち、`CommitString` を応答の前に、パネル更新を idle で後から送る点も fcitx5 と同じにしてあります。

## 既知の制限

- 未確定文字はインライン表示ではなく、画面下部のパネルに表示されます（入力欄のカーソル位置には出ません）。
- `.desktop` から起動される外部アプリは別プロセスです。日本語入力を使うには、そのアプリも `CONFIG_CP0_LVGL_IME_FCITX5=y` でビルドする必要があります（ZClaw は有効化済み）。LaunchWizard は対象外です。
- 文字入力モード（`KBD_INPUT_CONTEXT_TEXT`）を使わない独自の入力欄では、IME は動きません。
- fcitx5 は文字入力モード中のキーをすべて受け取るため、キーごとに D-Bus の往復が1回発生します。
- mozc は最初の入力で mozc_server を起動するため、最初のキーの応答が遅くなることがあります。タイムアウトすると、そのキーは日本語入力されずにそのまま入力され、再同期の後に通常の動作に戻ります。実機で頻繁に起きる場合は、`CP0_IME_KEY_TIMEOUT_MS` を大きくしてください。
- 未確定文字があるときの Esc は変換の取り消しに使われるため、その押下ではランチャーの「Esc 長押しでホームへ戻る」の表示が始まりません。未確定文字がないときは従来どおりです（CLI の Esc 長押しと、固まったときの強制復帰は、キーキューの段階で判定するため影響を受けません）。

## 元に戻す

`<user>` として実行します。

```bash
systemctl --user disable --now fcitx5-cardputer.service
sudo install -m 755 /usr/share/APPLaunch/bin/M5CardputerZero-APPLaunch.orig /usr/share/APPLaunch/bin/M5CardputerZero-APPLaunch
systemctl --user restart APPLaunch.service
# 不要なら: sudo apt-get remove fcitx5 fcitx5-mozc
```
