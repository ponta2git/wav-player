# WAV Player の再生設計

WAV の読み込みと形式変換を先読みスレッドで済ませ、ASIO コールバックには左右の完成済みブロックを渡す。機器の操作、再生位置の確定、エラー処理は制御スレッドが引き受ける。対応形式と利用者から見た動作は[製品要件](requirements.md)、SDK の規定とホスト側の判断は[ASIO 契約](asio-contract.md)を参照する。

実装の入口は[コード配置](#現在のコード配置)にある。再生中の資源の寿命は[曲の準備](#同一レートの区間を準備する)、[開始](#バッファーの作成と開始)、[停止・解放](#故障再初期化解放)の順に追える。

## 層と依存の向き

ソースの `src/wavplay/<層>/` と名前空間 `wavplay::<層>` を対応させる。製品ヘッダーは `src/` を起点に `wavplay/<層>/<ファイル>.h` で取り込む。ファイル形式とドライバーの能力を突き合わせるのは `playback` である。

| 層 | 責務と主な入口 | 直接依存できる製品層 |
|---|---|---|
| `core` | バイト順・整列・ブロックコピー、Win32 の資源・ページ・待機・電源要求 | なし |
| `audio` | `SampleFormat`、`StreamFormat`、左右の出力を作る変換器 | `core` |
| `media` | `ReadOnlyFile` がファイルを保護し、`WavReader` がヘッダー検査とフレーム位置を扱う | `core`、`audio` |
| `stream` | `GaplessRun` が同一レートの曲をつなぎ、`PrefetchRing` が読み込みスレッドとリングを所有する | `core`、`audio`、`media` |
| `asio` | 登録照会、機器設定、SDK 型と音声型の対応、`DriverSession` による機器の所有 | `core`、`audio` |
| `playback` | `TrackPlayer` が再生要求、`run_segment` が同一レート区間、`SegmentRenderer` が出力を担当する | `core`、`audio`、`media`、`stream`、`asio` |
| `settings` | UTF-8 設定の読み取りと原文を保つウィンドウ配置の保存 | `core`、`asio` |
| `playlist` | UTF-8 一覧とローカル WAV の検証、順序・重複・除外件数の保持 | `core`、`media` |
| `control` | `PlaybackController` が非同期操作を直列化し、再生状態を公開する | `core`、`asio`、`settings`、`playlist`、`playback` |
| `ui` | Win32 ウィンドウ、表示と操作可否、起動引数、単一起動 | `core`、`asio`、`control` |

`playback` の層外に公開するのは `play_tracks.h`、`track_player.h`、`playback_types.h`、`control` は `playback_controller.h` だけ。`control` と `ui` が扱う形式情報は再生層の公開型を通す。SDK ヘッダーの取り込み口は `asio/asio_sdk.h` に限る。`settings` と `ui` が使う機器設定・登録の宣言、UI 全体・`src/app/main.cpp`・`control/playback_controller.h` からたどれる製品ヘッダーは SDK 非依存に保つ。

### 現在のコード配置

製品の GUI 入口 [main.cpp](../src/app/main.cpp) が使うのは `ui` と `core` だけ。製品とコンソール試験は[同じ製品ソース一覧](../build/WavplaySources.props)をビルドする。代表的な変更箇所は次のとおり。

| 変更するもの | 実装を読む順序 |
|---|---|
| ファイル形式・無損失変換 | `media/wav_header.cpp` → `media/wav_reader.cpp` → `audio/planar_conversion.cpp`。機器の出力型との対応は `asio/sample_types.cpp` と `playback/output_format.cpp` |
| 先読み・バッファー切替 | `stream/gapless_run.cpp` → `stream/prefetch_ring.cpp` → `playback/segment_renderer.cpp` |
| 開始・停止・機器の再初期化 | `playback/track_player.cpp` → `playback/request.cpp` → `playback/segment.cpp`。クロックの復元とドライバー解放は `asio/driver_session.cpp` |
| 画面・操作・設定 | `ui/player_window.cpp` → `control/playback_controller.cpp`。表示と操作可否は `ui/player_text.cpp`、コントロールと配置は `ui/player_controls.cpp`、設定 I/O は `settings/device_config.cpp` |

## スレッドとリアルタイム境界

| 実行するスレッド | 所有するもの | 他のスレッドとの受け渡し |
|---|---|---|
| UI | 表示ウィンドウ・コントロール・描画済み状態 | 操作を `submit` し、通知を受けて同期済みの状態を読む |
| 制御スレッド（COM STA） | 初期化済みドライバー、所有ウィンドウ、不変の一覧、現在曲と再生要求 | ドライバーの生成・照会・開始・停止・解放を直列化する。待機中も Windows メッセージと操作を処理する |
| 読み込みスレッド | 曲列の準備、ファイル I/O、左右への変換、リングの書き込み側 | 左右の書き込みを終えたスロットだけを公開する |
| ASIO コールバック | 現在区間の読み出し側、選択ペアの出力半面 | 指定半面へのコピー、無音化、ロックフリーなカウンター更新、必要な起床通知を行う |

### コールバックの寿命

SDK のコールバックには呼び出し元を識別する引数がない。`callback_routing` がプロセスに一組のコールバック表と `PlaybackSignal` を同時に一つの Renderer へ接続する。切替コールバックは入口で要求の世代と区間の段階を記録し、半面番号を検査する。その半面の書き込みを占有してから接続先を読み、入口で記録した世代・段階と一致すれば描画して占有を解く。遅れた旧区間の切替は新しいリングに触れず、占有を解いて戻る。

`CallbackState` は `constinit` と `core::NoDestroy` を使う。静的オブジェクトの破棄が始まった後の通知にも状態とイベントが生存する。接続・切離し・`streaming` の切替、Renderer の構築・遅延設定・B の事前充填は制御側が行う。`detach` は新しい書き込みを遮断し、進行中の両半面の書き込みの終了を待つ。

コールバックから到達する範囲は [segment_renderer.cpp](../src/wavplay/playback/segment_renderer.cpp) の切替・レート通知・ASIO メッセージ・描画、`PlaybackSignal` のインライン操作、`PrefetchRing::Consumer::pop/read_failed` と `PrefetchRing::pop_block`、[block_copy.h](../src/wavplay/core/block_copy.h) のコピー・ゼロ埋め。`Renderer` は開始前にリングの専用ページを指す Consumer と Pause/Stop の atomic への参照を保持し、外側の管理オブジェクトをたどらない。

この経路では確保、ロック、待機、ディスク I/O、ログ、GUI 操作を行わない。コールバックからの通知は生産者の起床閾値と停止・故障の確定時の `SetEvent` に限る。ブロックコピーは左右ごとの 128 bit 整数処理で呼び出し元に展開し、外部バッファーの整列や余分な読み書き可能領域を前提にしない。CRT コピー・CPU 再判定を経由しない。

### 待機とスレッドの設定

制御 STA の `core::wait_for_events_until` は同時に立ったイベントのうち番号の小さいものを消費する。起床ごとに最大32件のメッセージを処理し、単調時計の期限を再確認する。期限前に入った `WM_QUIT` は外側のループへ残し、同時に消費したイベントを立て直す。再生待機の `quit` は `stopped`、待機失敗は `driver_error` になる。リングの初期充填待機では充填完了を優先し、再生通知による起床では `core::WakeHook` から制御側を呼ぶ。

自アプリのスレッドには `WAV Player UI`、`WAV Player control`、`WAV Player reader` の名前を一度付ける。UI の実行速度はウィンドウ作成後から終了まで、読み込み側は初期充填の完了通知後の補充から Windows 既定の `system` とする。`core/thread_hints` のヒントの失敗は再生を妨げない。制御 STA とドライバーのスレッドは OS の方針に委ねる。優先度・アフィニティ・MMCSS・プロセス全体の電源設定やタイマー分解能は変更しない。[Windows の Quality of Service](https://learn.microsoft.com/en-us/windows/win32/procthread/quality-of-service)

## 1. ファイルから左右の出力バイト列まで

### 一覧とファイルの保護

`playlist::load` が一覧全体を UTF-8 として検証し、引き続きローカル WAV を項目ごとに検証する。一覧そのものの読み取り・文字コードの失敗は全体のエラー。開けない曲、不正・空の WAV は項目ごとに除外する。同一表記のパスは検証結果を共有し、件数・表示順・重複は行単位で残す。起動時の一覧はパスだけのスナップショットで、再生に使うファイルはあらためて開いて保護する。[行の扱いと操作](requirements.md#プレイリストと操作)

起動時の Close は atomic の旗で伝え、有効な項目の検証前に確認する。検証中の一曲を終えた後に次の項目があれば `nullopt` で打ち切り、途中の一覧を公開しない。最後の項目なら完全な一覧を公開して機器初期化の前で止まる。入力中の WAV の検証や同期 I/O は中断しない。

ローカルの DOS 絶対パス、拡張長 DOS パス、volume GUID パスを受け付ける。`ReadOnlyFile` は通常の DOS 絶対パスを拡張長形式へ変換するときだけ、区切りと `.` / `..` を正規化する。既に拡張長のパスはそのまま開く。末尾の空白・点と表示用の原文を保つ。項目の事前確認に加え、開いた実体を `GetFinalPathNameByHandleW`（DOS、次に volume GUID）と `GetDriveTypeW` で照合する。固定・リムーバブル・光学・RAM ディスクを許し、ネットワークへ向くリンクや実体を確認できない入力を除く。[ファイルの実体を照会する Windows API](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-getfinalpathnamebyhandlew)

`ReadOnlyFile` は読み取りだけを共有し、検証から使用終了まで書き込み・削除を防ぐ。媒体照会・ファイル I/O は呼び出しスレッドだけの `core::CriticalErrorScope` で囲む。メディア未挿入などを I/O エラーにでき、ドライバーのエラーモードに影響しない。

### コンテナーと64ビット位置

[ヘッダー解析](../src/wavplay/media/wav_header.cpp)は RIFF/WAVE と RF64 でチャンク走査を共有する。ヘッダー・本体・奇数長パディングがコンテナーと実ファイルの両方に収まることを加算前に検査する。外側の宣言長は実ファイル長から先頭8バイトを引いた値と一致させる。`fmt` と `data` はそれぞれ一つ必要で、未知チャンクは長さで読み飛ばす。BW64 は対象外。

- **RF64** は `RF64/WAVE` の直後に長さ既知の `ds64` を一つ要求する。外側の長さ・data 長・Float の fact 値は32 bit 欄が `0xFFFFFFFF` のときだけ対応する64 bit 値で置き換える。RIFF 内の `ds64` は未知チャンクとして扱う。[EBU Tech 3306 §3.4](https://tech.ebu.ch/docs/tech/tech3306v1_1.pdf)
- **`ds64` の追加表**は固定28バイトと1件12バイトの宣言表が収まることを確認し、後続の予約領域を許す。data 以外の長さの置換には同じチャンク ID の項目を出現順に使う。必要な ID の検索位置だけを保持し、1回最大8,184バイトで参照する。必要な項目の欠落は拒否する。
- **Extensible** は `fmt` が40バイト以上、`cbSize` が22以上で拡張全体が収まること、SubFormat GUID 全16バイトを確認する。チャンネルマスクはモノで 0 / 0x4、ステレオで 0 / 0x3。通常 PCM の `fmt` は16または18バイト、IEEE Float32 は16バイトを互換受理し、18バイトなら `cbSize=0` を要求する。どの形式も `blockAlign`・`byteRate` を照合する。[MicrosoftのExtensible形式](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/ksmedia/ns-ksmedia-waveformatextensible)
- **Float の `fact`** は互換性のため省略を許す。存在すれば単一かつ4バイト以上で、宣言フレーム数が `data` から求めた完全フレーム数に一致すること。RF64 では fact 値が `0xFFFFFFFF` の場合だけ `ds64.sampleCount` を使う。PCM では fact の内容を使わない。[Float と WAVE の契約](asio-contract.md#float入力と浮動小数点出力)
- **位置と長さ**は `WavInfo`、`WavReader` の残量・seek、`GaplessRun` の曲長合計、リングの公開・消費カウンター、`TrackPlayer`、UI の状態まで64 bit を保つ。ファイル位置は Windows の非負の符号付き64 bit 範囲で検査し、区間合計は加算前に検査する。`PoppedBlock::valid_frames` だけは寸法検証済みの1ブロック内の32 bit 値である。

### 検証と変換

`WavReader` は `FILE_FLAG_SEQUENTIAL_SCAN` の同期・キャッシュ付きハンドルで、`ReadFile` ごとに `OVERLAPPED` に絶対位置を指定する。残りフレーム数が位置の正本なので seek は I/O を伴わず、`ReOpenFile` で同一ファイルを開き直すと検証済みメタデータと独立のカーソルを得る。指定位置の EOF と短い読み取りはともに読み取り失敗にする。

Extensible PCM 24/32 では上位24 bit が有効で、下位8 bit は全サンプルでゼロでなければならない。`WavReader` の通常構築時に最大64 KiB の専用ヒープ領域へ読み、16バイト単位の整数 SIMD と末尾0～3サンプルのスカラー検査で確認する。同一レート区間の後続曲も開始前に検証し、曲間の先読みを全件検査で止めない。保護ハンドルからの再開は検証結果を継承する。

[出力形式の交渉](../src/wavplay/playback/output_format.cpp)は選択した両チャンネルが同じ対応型であることを確かめる。`asio::to_sample_format` が SDK 型を記述し、`audio::find_planar_converter` が入力形式全体を無損失に変換できる場合だけ変換器を返す。モノは両側に複製し、ステレオは左右を分離する。PCM/Float とも読み込み時に整数のビット操作だけで最終バイト列を作る。[入力と出力の組み合わせ](requirements.md#入力と出力の一致)・[有効ビットの配置](asio-contract.md#整数出力の有効幅と配置)

## 2. ドライバーを開き、一曲を開始する

### 機器の所有と初期化

1. `PlaybackController` のワーカースレッドが `load_settings` → `load_list` → `open_device` の順に進む。設定と一覧は順に公開する。各段階の入口と単曲 `--wav` の検証後に Close を確認する。
2. 機器未設定なら一覧の公開後に `NO DEVICE` を報告する。設定エラー・未設定ではドライバー用ウィンドウを作らない。正常時はファイル検証を終え、機器を開く直前に非表示の所有ウィンドウを作る。長いファイル I/O の間に Windows のブロードキャスト送信元を待たせないためである。
3. [DriverSession](../src/wavplay/asio/driver_session.cpp) が OLE1 DDE を使わない COM STA と指定 CLSID のドライバーを所有する。x64 登録を確認し、CLSID を CLSID と IID の両方に使ってインプロセスの `IASIO` を取得し、所有ウィンドウで初期化する。
4. `TrackPlayer` / `play_tracks` は `RequestClaim` でプロセスの再生を一つに制限し、要求中だけ `DriverLease` でドライバーを借りる。最後の故障確認と位置の確定まで占有を保つ。GUI は一つのコントローラーだけを使い、直接の同期 API を並行して呼ばない。

### 同一レートの区間を準備する

[run_request](../src/wavplay/playback/request.cpp) は曲列と `RequestOptions`（開始位置、診断、進捗、拒否方針）を、Pause/Stop を受ける `SegmentControl` と別に受け取る。曲ごとの拒否を記録する `RefusalPolicy::record_each` には項目番号を返す進捗が必須で、欠ける要求は機器へアクセスする前に拒否する。

- **クロックとペアを選ぶ前に**、ドライバーの overload 報告能力と元レートを照会する。実レートが WAV と異なるときだけ `canSampleRate` → `setSampleRate` → 読み戻しを行う。能力照会までは元レートの通知を期待し、設定直前に期待値を切り替える。新レートで出力数・指定ペアの型・バッファー範囲を照会する。
- **幅を決める `asio::select_buffer_frames`** は、有効な範囲に対して、要求0なら現在の推奨値、正値なら検証した要求値を選ぶ。粒度 −1 は2の冪、正値は最小幅からの刻み、0 は追加制約なし。`accepts_buffer_frames` と判定を共有する。指定のペア・型・幅を使えない場合は別設定で再試行しない。
- **先頭曲は制御 STA が開き**、読み込みスレッドが同一レートの後続曲を検証して `GaplessRun` に加える。`SourceCache` は同一表記のパスに保護ハンドルを共用し、最後の使用時は所有参照を移す。使用回数は open 成功後に減らす。異なるレートの次曲は開いたハンドルのまま次区間へ渡す。
- **リングの初期充填中も**、制御 STA は `PrimingPoll` から進捗と操作を確認する。読み込み側の曲列・キャッシュ・拒否記録には触れない。後続曲を開く前に Stop または世代・段階の変化を調べ、検証中の一曲と打ち切った区間の充填後、`createBuffers` 前に停止できる。Pause はこの準備を打ち切らない。

`GaplessRun` は一つのブロック内でも曲末ごとに次曲を先頭から続けて書く。`advance_track` は前の曲末を確定し、所有権を移して seek(0) してからブロックが満杯かを判定する。1ブロック内に複数の曲境界があっても、読み込み失敗やフレーム順を保つ。後続曲の検証・変換可否で拒否が決まったら、そこまでの曲を区間として再生する。

### バッファーの作成と開始

1. 制御側がコールバック表と通知先を用意し、初期充填した区間の幅で選択ペアの `createBuffers` を呼ぶ。作成後に実レート、左右の型・有効状態、4半面の書き込み可能範囲と非重複、出力遅延を検証する。2秒を超える報告遅延は開始前に拒否して解放する。[バッファーの検証と部分失敗](asio-contract.md#任意機能と未定義時の復旧)
2. Renderer が A（index 0）を無音にし、B（index 1）に開始無音または先頭の PCM を充填する。開始無音は `StartSilence`（既定100 ms、0～1000 ms）をブロック単位に切り上げた長さで、B から数える。初期充填後に生産者が失敗していれば開始せず `read_error` を返す。
3. `core::SystemSleepGuard` が独立した `PowerRequestSystemRequired` を取得し、Renderer を接続してから `start` する。`start` の中で最初の A を要求されても次のブロックを渡せる。電源要求を取得できなければ `driver_error` として片付ける。排出・停止・バッファー解放まで要求を保持し、例外時も解く。画面点灯は要求しない。[Windows の電源要求](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-powersetrequest)
4. `start` の成功後に `playing` と Play の完了番号を公開する。`PlaybackDiagnostics::buffer_capabilities` は確定レートと有効な範囲を幅選択前に記録し、`BufferSetup` は作成後の検証に成功した実幅を記録する。拒否された幅を「確保済み」と表示しない。双方を区間ごとの列に保持し、次の Play・再初期化で消す。

## 3. 曲間、一時停止、エラーの境界

### 提出位置と排出

Renderer はリングから ASIO に渡した**有効 PCM フレーム数**だけを `progress_frames_` に数える。開始無音と末尾のゼロ埋めは除く。制御側の `TrackPlayer` は区間内の各曲長と照合して曲番号・曲内位置を求める。異レート曲のフレーム数を合算せず、先読みカーソルや `getSamplePosition` から聴取位置を推測しない。

| 境界 | 音声と資源の扱い | 位置と次の開始 |
|---|---|---|
| 同一レート・互換曲 | 同じリングを使い、ブロックの余りは次曲で埋める | 現在曲を進め、曲間に無音を挟まない |
| 区間の正常終端 | 最終ブロックの有効フレーム以降をゼロ埋めし、提出済み PCM を排出して停止 | 最終曲は `completed` と曲末位置。異レートの次曲は区間解放後に機器を開き直す |
| Pause | 次の PCM を取り出す前に供給を止め、通常終端と同じ方法で排出する。最終ブロック提出済みなら区間終端を優先 | 停止・解放成功後の有効位置を保つ。保護中の同一ファイルから独立カーソルと新しいリングを用意し、保持位置から開始無音を経て再開 |
| Stop | 要求フラグと既存イベントで制御側を起こす。排出を打ち切り、`stop`・バッファー解放・読み込み側終了まで待つ | 選択を保ち、成功後に位置0。次の Play は選択曲の先頭から新しい要求として開始 |
| 曲ごとの拒否 | 読めない次曲・無損失にできない形式・受け付けないレートや幅を該当項目で止める | 拒否項目を選択、位置0の `stopped`。理由は `track_error`。別の曲は明示的に選べる |

排出はフレーム単位で行う。少なくとも B の1ブロック先行を含む出力遅延を使い、ハードウェア始動前にそれ以上のブロックを先行提出した場合は超過分も足す。最後の有効フレームを出した後は無音を返しながらさらに2ブロック分の実時間を待つ。Pause/Stop で波形を書き換えず、ゼロクロス待ちやフェードを挟まない。[ASIO の開始・排出規定](asio-contract.md#開始から停止まで)

異レートの曲間は前区間を排出・停止してバッファーとリングを解放し、`DriverLease::reopen` で元レートを復元してからドライバーを手放す。同じ CLSID・所有ウィンドウで初期化し直し、能力と実レートを照会して次区間を準備する。再 open に失敗したセッションにドライバーは残らない。自動復元の対象は変更したレートだけで、推奨幅を戻す目的の `createBuffers`・`start` は行わない。[推奨バッファー幅の契約](asio-contract.md#推奨バッファー幅の保持と終了時の方針)

### 故障、再初期化、解放

- `PlaybackSignal` のトークンは**要求の世代・段階・停止理由**を含む。`try_begin` がアイドル時の故障確認と次世代の公開を compare-exchange で結び、独立カーソルを作る間の通知も保つ。期待クロックは未使用側のスロットに書いてから段階を公開する。故障は再初期化要求より、再初期化要求は Stop/Pause/EOF より優先する。Stop は Pause/EOF の排出中も受け付ける。
- **再生中の故障**には空リング、読み込み失敗、無効な半面・順序、書き込み競合、overload、クロック喪失がある。コールバックは停止理由を確定し、以後は古い PCM を残さず無音を返す。制御側が停止する。生産者の例外は join 後に `read_error()` → `RequestProgress::fault_error` → `TrackPlayer::result_cause()` で UI に渡す。
- **コールバック不着**を検出するため、`start` 成功後の監視周期を `max(制御の起床周期, 4ブロック時間)` とする。制御の起床周期は `max(500 ms, 1000 ms − 20 ms − 1ブロック時間)` で、ブロック時間を ms に切り上げる。区間ごとに決めた単調時計の期限で `switches_` に変化がなければ `driver_error`。進捗・メッセージ・無関係な通知で監視期限を延ばさない。[切替の監視と STA の待機](asio-contract.md#切替の監視と-sta-の待機)
- **再初期化が必要な通知**（reset・buffer-size-change）は `reinitialize_required` で伝える。`TrackPlayer` は待機中も `idle_fault_event` / `take_idle_fault` で故障を監視する。コントローラーが `reinitializing` を公開して保留操作を取り消し、Close だけを受け付ける。旧機器を解放し、同じ STA・CLSID・ウィンドウ・起動時設定で再初期化する。保護中の WAV は保持し、成功時は位置0で停止する。次の明示 Play で幅・出力とリングを準備する。
- **旧ドライバーの解放中**に届いた reset・buffer-size-change は進行中の再初期化に合流させる。新しいドライバーの初期化・照会での再発、復元・再 open の失敗、致命的な故障は `failed` とし、再試行し続けない。Close はドライバー呼び出しの境界で再初期化を打ち切る。`TrackPlayer::prepare_release` は正常な復元レート通知を許し、すでに届いた故障と Release 中の通知を保持する。
- **開始を試みた区間**は `stop`、コールバック切離し、進行中の writer の終了、`disposeBuffers`、生産者の join の順で片付ける。`stop` の失敗時も切離してから両面を無音にする。切離した後の切替は書き込まない。元レートの復元を読み戻しで確かめ、復元を拒否されてもドライバー参照を解放する。
- **ASIO エラーの報告**には `asio::throw_if_error` を使い、操作名・ASIO エラー名・数値を例外に残す。`createBuffers`・`start` の部分失敗を片付ける途中の `stop`・`disposeBuffers` も失敗した場合は例外を入れ子にして全原因を保つ。部分的な `createBuffers` 失敗時、返されたアドレスがあれば書き込まず一度解放を試す。`ASE_InvalidMode` は元の作成エラーを報告し、他の解放失敗は `ASIO disposeBuffers failed after createBuffers error` を外側に残す。作成成功後の例外処理中の解放失敗は `ASIO disposeBuffers failed during cleanup` とする。コールバック内では例外を生成しない。

同期 API の `TrackPlayer::stop()` は停止後の位置を戻す操作で、再生中は `request_stop` を使う。アイドル中の非同期操作を次の Play へ持ち越さない。Pause/Stop 後は検証済みの選択曲と初期化済みのドライバーを保持し、リングと ASIO バッファーは保持しない。故障した要求は再開しない。

## 4. 操作画面、設定、実機の接続

### プロセス起動と設定

`main.cpp` は CRT の Unicode 引数 `__wargv` を `ui::parse_player_options` に渡す。通常は実行ファイル隣の `playlist.txt`、`--wav path` なら単曲を使う。`--help` または `/?` の単独指定で使い方を表示する。引数エラーも機器を開く前に扱う。設定は同じディレクトリの `settings.ini` から読む。製品は `CommandLineToArgvW` / SHELL32.dll を直接取り込まない。両実行ファイルに [app.manifest](../build/app.manifest) を埋め込み、PerMonitorV2、Common Controls v6、通常ユーザー権限、長いパスへの対応を宣言する。

正しい引数を解析した後、設定や機器を開く前に `ui::SingleInstance` がユーザー SID と Windows セッションの `Local` 名前空間で占有を決める。先行 GUI の準備・終了を最大5秒待ち、先行プロセスが終了していれば取得し直す。無応答・期限超過はエラーとし、二重に機器を開かない。後発プロセスは対象 PID に前面化権限を渡し、先行 GUI は最小化を戻して前面化を要求する。Windows に制限されたときはタスクバーを点滅させる。後発起動の引数は適用しない。[前面化権限](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-allowsetforegroundwindow)

`settings::DeviceConfig::apply` が明示値だけを既定値に重ね、`asio::DeviceSettings::validate` が検証する。機器・ペア・幅・開始無音は起動時に決定し、曲間や再初期化では機器の能力だけを取り直す。1 MiB 上限の UTF-8 検証、設定キーと範囲、配置の単位は[設定の仕様](requirements.md#設定と診断)を参照する。

配置は UI が同期済みの状態から受け取り、通常時の外枠を現在のモニターの作業領域・最低寸法に合わせる。初回も `SetWindowPlacement` を使い、最小化状態は保存しない。制御側は起動時の設定読込に成功した場合に限り、終了時に機器を解放してから最新原文を再読する。`Window` の既知キーだけを書き換え、同じディレクトリの一時ファイルをフラッシュして置換する。手動編集・コメント・BOM を保ち、読み直した設定を再生には適用しない。保存失敗時は旧ファイルを保持して終了エラーを公開する。

### 操作の受付と完了

`PlaybackController::submit` と `select_track(index, play)` は固定32件のキューに操作を登録し、完了を待たずに受付番号を返す。満杯、範囲外選択、空一覧への Play、故障・終了・再初期化中の通常操作は0で拒否する。Close は満杯を迂回し、以後の受付を閉じ、保留操作を取り消して停止する。

- **受付と完了**は `PlaybackSnapshot` で分ける。`accepted` は受付、`settled` は完了または取消しが確定した最後の番号。Play は `start` 成功、一時停止と Stop は停止・解放・生産者終了後に確定する。状態欄は確定状態を、保留欄は未確定操作を示す。
- **操作の配送**では、アイドル用イベントと再生中の通知イベントを、キューが空から非空になったとき、または Close 時にロック外で通知する。制御 STA は準備の境界・開始成功・待機復帰で最大32件を扱い、実行可能な残りは再通知する。`poll` は進捗とキューを同じロックで確認する。先行する Pause/Stop などの完了を待つ Play はキューに残す。メッセージはキューのロック外で処理する。
- **選択と再生位置**を別々に保持する。`selected_index` はユーザーが選んだ行、`current_index` は再生中の曲。手動選択だけでは現在の音と時間表示を切り替えない。停止時に連続する Select は、直後が Select または Select and Play の場合だけ末尾の一件で開いて検証し、先行番号もその結果で確定する。Play/Pause/Stop をまたいで選択をまとめない。
- **Play の開始位置**は、未選択なら先頭、Pause 後は保持位置、Stop 後は選択項目の先頭を使う。Select and Play は進行中の要求を Stop し、解放を待って指定曲の先頭から新しい要求を開始する。同じ曲を選び直した場合も同じ手順を通る。
- プレイリストの選択・検証での媒体エラーは**項目単位の拒否**とし、`next_track_rejected` / `next_rate_rejected` と理由を公開する。幅の不一致は指定値・min/preferred/max・粒度とレートを返す。`bad_alloc`、論理エラー、再生途中の読み取り失敗、ドライバーと後始末の失敗は致命的な故障として扱う。

一覧の実体は `shared_ptr<const vector<wstring>>` で制御側・`TrackPlayer`・状態表示が共有する。制御側はロック外で更新中のスナップショットを参照しない。曲の所有権は絶対インデックスが変わったときに更新し、現在曲での Pause/再 Play と将来の重複に必要な保護ハンドルを残す。

### 状態通知と描画

1. 制御側は mutex 下で状態と `revision` を公開し、`wait_for_change` の待機者を起こす。`ChangeListener` は通常の変更を `changed`、進捗だけなら指定時の `progressed` に一度通知する。通知中はロックを保持するので、リスナーからコントローラーを呼び戻さない。
2. UI は未処理の通知を一つの `PostMessageW` にまとめ、サイズ変更等のモーダルループからも処理する。投稿できない場合・通知先がない場合や強制更新にはイベントを使う。投稿処理の先頭で保留印を解除し、その後の変更も届くようにする。待機失敗は最初に一度だけ表示し、50 ms 後に待ち直す。常時のタイマーやポーリングは使わない。
3. `revision` は再読が必要かを決めるヒントで、正本はロックされた `PlaybackSnapshot`。`snapshot()`、`snapshot(into)`、`wait_for_change` は完全な状態を返す。`refresh_snapshot` は、正常に描画し終えた前回の状態を再利用できる同一曲の通常進捗だけ `revision` と `position_frames` を更新する。初回・曲境界・操作・故障・強制更新は完全コピー。描画が失敗した版を描画済みにしない。
4. 通常進捗は、制御側が提出済みフレーム数から求めた曲番号か整数秒が変わったときだけ公開する。最小化中は進捗の版だけを記録し、状態・故障・Close の通知は続ける。復帰時は完全な状態を一度読み直す。`command_state()` は操作に必要な値だけを同じ mutex で取得し、表示と同じ `ui::available` で可否を調べる。実際の受付でも再検査する。

`player_text.cpp` は時刻・機器・出力・バッファー・エラーの文字列を HWND に触れずに作る。時間は当該曲の64 bit フレーム数をレートで整数除算する。バッファー時間は小数第3位で偶数丸めし、正の整数レートを確認できなければ ms・Hz を省く。要求・照会済み能力・直近に確保した実幅は別々に表示する。故障は `faulted()` で判定し、`read_error` では固定文に生産者の例外を添える。入れ子の原因は外側から最大4段、Win32 エラーは `FormatMessageW` で表示する。

`PlayerControls` は標準の単一選択 ListBox とシステムフォントを所有する。行はソートせず、不変一覧が入れ替わったときだけ `LB_INITSTORAGE` で確保して作り直す。フォント・DPI が変われば実幅を再測定する。Buffer/File/Error は Tab 移動、選択・コピー、折返しとスクロールに対応する。通常描画は文字列の作業領域を共用し、`SetWindowTextW` が成功した後だけ表示キャッシュと入れ替える。同じ文字列なら選択・スクロールを保つ。

`player_window.cpp` は修飾なしの Space/Escape を子へ配送する前に処理し、自動反復を無視する。Tab・アクセスキーは `IsDialogMessageW`、ボタンの Enter は `DM_GETDEFID`、リストの Enter は選択曲の再生を使う。再アクティブ化では有効・表示中の元の子へフォーカスを戻し、復元できなければ有効なリスト、次にメインウィンドウを使う。編集入力がないので `ImmDisableIME(0)` は UI スレッドにだけ適用する。

### 通常終了、UI 故障、セッション終了

通常の Close は配置を渡して制御側へ要求し、UI は待ち続けて通知先を維持する。制御側はドライバー → WAV → 所有ウィンドウ → STA の順に手放し、配置保存後に `closed` を公開する。UI はその後ウィンドウを破棄して join する。終了中に初めて現れたエラーは表示し、再 Close で非ゼロ終了する。外部 `WM_QUIT` は確認操作を待たず故障時の終了コード1を優先する。

UI 自身が故障したら通常描画を終了する。`show_text` と `PlaybackController::closed()` により、スナップショットの文字列コピーに依存せず Close・解放・表示を完結させる。状態/Stop/Close の表示は故障直後と新しい版でだけ更新する。ウィンドウが先に破棄されても通知オブジェクトを制御側の解放完了まで保持する。

`WM_QUERYENDSESSION` は副作用なく TRUE、取り消された `WM_ENDSESSION` は何もしない。確定時は Close を送り、投稿先を一時的に外してイベントで `closed` を最大約4秒待つ。`ShutdownBlockReasonCreate` で理由を示し、`QS_SENDMESSAGE` に限って処理することで、ドライバーの同期メッセージには応答しつつ入力や投稿処理の再入を防ぐ。期限後は OS に制御を戻し、ウィンドウが残れば通知先と通常の終了処理を復元する。

## チューニングの入口

### ファイル I/O とリング

| 調整箇所 | 値と設計上の境界 |
|---|---|
| `WavReader::staging_bytes` | 通常の `ReadFile` は最大8 KiB。読み込み側のスタックにキャッシュライン整列した作業領域を一つ置き、変換まで同期的に貸す |
| `WavReader::validation_bytes` | 24/32 の全件検証だけは最大64 KiB。検証は同一レート区間の再生前に終える |
| `stream::PrefetchPolicy` | 目標約2秒、左右の音声領域とリング専用管理ページの実コミット合計は16 MiB 以下。短い曲列は終端まで。通常はリング1周当たり約4回、生産者をイベントで起こす |
| `GaplessRun::next_batch_blocks` | 現在曲の元データ8 KiB を目安に整数ブロック数（最小1）を準備時に求める。曲末を含むブロックで区切り、次の補充から新しい入力幅を使う |
| `plan_block_shape` / `plan_ring_layout` | 桁あふれを防いでブロック幅・周回容量を計算する。予算内にステレオ2ブロックを置けない寸法は開始前に拒否 |

生産者は曲のバッチ上限を空き容量・リングの折返し・未消費量で絞り、`PlanarBlocks` の有効範囲へ直接変換する。密なスロットはまとめ、整列の隙間があれば一定の stride で進む。`check_planar_blocks` が I/O 前に `_umul128` でブロック幅と最後のオフセットの積を検査し、続いて末尾の加算、実アドレスと左右の重複を調べる。外接範囲が重なるときは stride を S、有効幅を B、左右 base 間距離の S に対する剰余を r とし、`r ≥ B` かつ `S − r ≥ B` を要求する。空の要求は0、部分ブロックは要求範囲だけを扱う。

片側32バイト以下の左右は同じ64バイトラインの前半・後半、それより大きい幅は独立した左右平面に置く。同じスロットは常に左右一緒に所有・公開し、別ブロックは別ラインに置く。`channel_offset` は右の位置、`plane_bytes` は周回幅を表す。

リングが満杯になるか、同一レート区間の末尾まで初期充填できた時点で初めて公開する。区間全体が収まれば開始前に生産者を join する。収まらない場合、生産者は最終ブロックの公開後も最後のファイルを保持して待つ。`run_segment` が戻ると `queue.stop()` が生産者を起こして join する。例外で抜けてもリングの破棄時に同じ停止処理を行う。曲間では済んだ曲の参照を落とす。読み込み失敗は生産者が公開する。最終ブロック公開後の待機が失敗すればスレッドは先に終了するが、公開済みデータは読み出せる。[先読みの実装](../src/wavplay/stream/prefetch_ring.cpp)

### メモリ配置

`core::PageAllocation` は `VirtualAlloc` の要求サイズとページ単位の実確保量を区別する。最小・最大ワーキングセットを実確保量だけ広げて `VirtualLock` を試み、解放時に元へ戻す。移動時はページと調整量を一緒に渡す。固定を拒否された場合は調整を戻し、ページング可能なまま使用する。`ResidentObject` はリング管理情報・Renderer・Pause/Stop フラグを各専用ページで所有する。

共有書き込みによる干渉を避けるため、リングの不変レイアウト、生産者、消費者のカーソルを別々の128バイト（隣接キャッシュライン対）に置く。Renderer の先頭の対はコールバック専用、次は低頻度の行と制御側が読む共有行。定常 PCM の切替が触れるのは3行で、制御側は接続中に `progress_frames_` と `switches_` だけを読む。切離し後は writer の解放と同期して最終値を確定する。プロセス共通の `CallbackState` は経路の行と、通知・SDK コールバック表の行を一組の128バイトに納める。

`main.cpp` は単一起動の確定後・機器初期化前に `core::lock_process_image` を一度呼び、実行イメージの `__ImageBase` から `SizeOfImage`（ヘッダーと全セクション）までを終了まで固定するよう要求する。`/SWAPRUN:CD,NET` はリムーバブル・ネットワーク媒体のイメージをページングファイルに写す指定。リング初期充填で全ページに触れ、`BufferSetup` に音声平面・管理情報・Renderer・要求フラグ・実行イメージの5種類の固定結果を残す。確認は診断コマンドで行う。共有 CRT ヒープ、ドライバー DLL・バッファー、OS、各スレッドのスタックは対象外。[ページ管理](../src/wavplay/core/memory.cpp)

### 変換カーネル

整数カーネルはバイト順・有効幅を選択時に確定し、左右分離・バイト反転・複製を直接行う。32 bit 容器を組むときは上位配置の中間値から出力有効幅へ算術右シフトし、上位を符号拡張する。MSB は可能な箇所で shuffle にバイト反転を含める。LSB 同幅モノは一括コピーを両側に使う。出力後の並べ替えは不要である。

128 bit 整数 SIMD の非整列ロード・ストアは常に有効範囲に収める。packed24 の16バイトロードは残量で実行を制限し、packed24 → 32 bit の2ロードは24バイトに収める。Float 用 packed24 は8＋4バイトずつ4サンプルを読む。固定幅のカーネルは4・8フレームに満たない末尾をスカラー処理する。入力の padding や出力スロット間の隙間への書き込みを前提にしない。

Float32 同幅は PCM32 と同じコピー・左右分離・バイト反転、PCM → Float と Float32 → Float64 は4サンプルずつ整数演算で IEEE の符号・指数・仮数を組み立てる。`PABSD` は INT32_MIN の大きさを 0x80000000 として扱い、正規化は `VPSLLVD` を使う。Float32 の subnormal を含むベクトルだけ追加の正規化を行う。符号付きゼロ・無限大・NaN の符号、quiet/signaling ビット、ペイロードを保持し、FTZ/DAZ や MXCSR の状態に依存しない。[浮動小数点出力の契約](asio-contract.md#float入力と浮動小数点出力)

## 命令セットの前提

対象は Windows 11 25H2 以降の x64。製品と試験の通常コードは `/arch:AVX2` を使う。[CPU 判定](../src/wavplay/core/cpu_features.cpp)は CPUID で AVX2、BMI1/BMI2、LZCNT、MOVBE、FMA、F16C 等を、`GetEnabledXStateFeatures` と Windows の機能照会で XMM/YMM の状態保存と AVX/AVX2 の利用可否を確認する。非対応時は固定メッセージと非ゼロ終了で知らせる。低い ISA の再生経路は持たない。

- **起動ガード**を置く `src/app/startup.cpp` と `core/cpu_features.cpp` だけは SSE4.2・LTCG 対象外とし、通常の CRT 起動を保つ。製品側の CRT 初期化、TLS callback、登録された終了処理を含め、判定前と非対応での終了時はベースラインの命令だけに限る。Windows とプリビルド CRT 内の機能判定は信頼境界とする。
- **言語と Windows API の範囲**は共通ビルド設定で固定する。`LanguageStandard=stdcpp23`（`/std:c++23preview`、`_HAS_CXX26=0`）で C++23 に限定する。`_WIN32_WINNT` と `WINVER` は `_WIN32_WINNT_WIN10`、`NTDDI_VERSION` は `NTDDI_WIN11_GE`（25H2 と共通の 24H2 API 集合）。ターゲット SDK の更新時も最低 OS に合わせて API の可視範囲を見直す。`NDEBUG` は Release だけで定義する。[共通ビルド設定](../build/Common.props)
- **実時間コードの ISA** は起動時に決定する。変換ループとコールバックに再 dispatch を設けず、浮動小数点演算・AVX512 は使用しない。ドライバーの double クロック値も `asio/clock.h` のビット比較で検査し、signaling NaN による例外や MXCSR の変更を避ける。
- **プロセス全体に効く** CET シャドースタックと CFG を有効にすると、同じプロセス内のドライバーのスレッドにも call/ret や間接呼び出しの検査が及ぶ。ドライバーの実行環境を変えない方針から `/CETCOMPAT:NO` とし、CFG と Spectre 緩和も有効にしない。代償として信頼できない WAV の処理でこれらの悪用緩和は働かない。ヘッダーと範囲を入力側で検証し、Release で PE の GUARD_CF（0x4000）と CET compatible の不在を監査する。判断を変えるときはビルド設定・監査・設計を併せて更新する。

## 最適化の方針

Release は上記の起動ガードを除き両プロジェクトで `/GL` と非増分 `/LTCG` を使用する。Debug と Debug-ASan は全体最適化を使わない。Release の `.obj` は中間表現なので、監査対象は最終 `wavplay.exe` の機械語。リンク時の `/FAcs` には `/OPT:REF` で除去される関数も含み得る。`/OPT:ICF` は明示しないが、別々の関数に別アドレスが割り当てられる保証もない。

`ui`・`settings`・`playlist` はサイズ優先の `/O1`、制御 STA を含むそれ以外の製品翻訳単位は速度優先の `/O2` とする。設定は [WavplaySources.props](../build/WavplaySources.props) でディレクトリごとに共有する。LTCG 時の共有インライン関数・テンプレートの生成元はリンカーが選ぶ。名前空間で到達先を調べる検査だけでは、展開後のコードの最適化水準までは判定できない。

通常経路から低頻度の処理を分ける。`GaplessRun::advance_track` は `noinline` とし、曲境界の所有権更新を各読み取りへ展開しない。`PoppedBlock` の状態と有効フレーム数は64 bit のレジスターで返す。時間や命令数の変化はその条件での観測として扱い、音質や消費電力は別に測る。

## 拡張点

- **画面**の追加は `ui` で `ChangeListener` と同期済みの状態を使う。制御側の公開ヘッダーを SDK 非依存に保つ。
- **曲列やコンテナー**を追加するときは、GUI・コールバックまで形式分岐を広げず、`playlist::parse/load`、`media` の検証、`run_request` の同一レート区間を入口にする。拡張した経路でも検証済みの所有権と曲内フレーム位置を保つ。
- **機器・ペアの選択**は現行では起動時の手動設定である。将来切替を実装する場合は制御側で停止・再初期化し、準備後は明示 Play を待つ。

## 品質特性と変更時の確認

変更した境界に応じて、保守性・信頼性・性能効率性を確認する。先読み性能を比べる場合は、同じ入力形式・出力型の[測定手順](test-build.md#先読みの実測比較)を使う。[ISO/IEC 25010:2023の製品品質モデル](https://www.iso.org/standard/78176.html)

| 変更した境界 | 確認すること |
|---|---|
| 層・ヘッダー・ビルド登録 | `scripts/check-project-layout.ps1` で直接の依存、公開ヘッダー、SDK への推移的な依存、製品・試験のソースと Visual Studio 登録を照合する |
| 入力・変換・リング寸法 | 拒否時は読み取り位置と出力を保持する。独立な期待バイト、保護ページ、スロットの隙間で入力末尾・左右・有効幅を調べる。RF64 は4 GiB 境界の seek まで確かめる |
| コールバック・停止・クロック | 到達コードに確保・待機・I/O がないことを読み、Release の機械語も監査する。模擬 DAC の時系列、故障注入、解放後の状態で Pause・Stop・EOF・再初期化を区別する |
| メモリ・先読み・表示 | 初期充填と補充、専用ページの固定と解放、状態の公開と通知・描画を分けて観測する。実機での物理出力と聴感は[実機試験](test-specification.md#実機試験の手順)で扱う |

### テストの構成

[試験仕様](test-specification.md)が合否と実行手順の正本。`tests/wavplay_tests/` は製品ソースをそのままビルドし、`DriverSession` の生成関数 `asio::ActivateDriver` にプロジェクト所有の模擬 ASIO を渡す。グローバルなドライバー差し替え口は置かない。

| 配置 | 判定する境界 |
|---|---|
| `framework/`、`suites/suites.h` | `TestSuite`・期待値判定、ハング・異常終了報告、各群の子プロセス分離。既定の最大並列数は4（`--jobs N`）。`--list`、`--suite`、`--in-process` で範囲を選べる |
| `fixtures/`、`doubles/` | WAV 生成、製品変換に依存しない `sample_oracle`、ファイル・スレッドを止めるゲート、模擬 ASIO と停止時に未再生分を捨てる模擬 DAC |
| `suites/<層>/` | 純粋変換と型は直接試す。`core` から `asio` の試験は再生層・コントローラー・GUI・再生用 fixture を直接取り込まず、`asio` より下位では模擬 ASIO も使わない |
| `suites/playback/`、`control/`・`ui/` の試験群 | 一区間の音声と停止、異レートの機器呼び出し順、入力形式と出力型の時系列、操作番号と実ウィンドウの接続をそれぞれ試す |
| `diagnostics/` | `--probe-asio`、`--probe-asio-rate-lifecycle`、`--audition-asio*`、`--measure-prefetch`。先読み測定は音源形式と ASIO 出力名を選べる。実機や測定の入口では製品と同様にイメージ固定を一度試みる |

`scripts/verify.ps1` がビルド・静的解析・試験を順に実行する。`scripts/verification/` では `build-settings.ps1` が評価済み MSBuild 設定、`release-image.ps1` が PE・manifest・version、`realtime-code.ps1` がコールバックから到達する機械語、`startup-code.ps1` が起動ガード前と拒否後の到達先を監査する。PE・取り込み DLL・機械語の自己検査は違反を入れた入力の拒否も確認する。詳細なケースと環境は[試験仕様](test-specification.md)を参照する。
