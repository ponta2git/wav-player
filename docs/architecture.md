# WAV Player の再生設計

[製品要件](requirements.md)を実装するための設計。現在動く範囲は [README](../README.md) に記す。

## 層と依存の向き

製品コードは `src/wavplay/<層>/` に置き、名前空間 `wavplay::<層>` で区切る。ヘッダーはインクルードルート `src/` から `#include "wavplay/<層>/<ファイル>.h"` と書く。下の表の上ほど下位の層で、各層は表で自分より上にある層だけに依存する。`asio` は `media` と `stream` を知らず、ファイル形式とドライバーは `playback` で初めて出会う。

依存は管理対象の重要度で濃淡をつける。語彙と純粋関数（`core` の整列・エンディアン、`audio`）は信号処理の層（`media`・`stream`・`asio`・`playback`）から自由に使えるが、`control` と `ui` は形式を `playback` の公開入口越しにだけ扱い `audio` に依存しない。変わりやすい実装詳細は層の中に閉じる。`playback` の他層向けの入口は `play_tracks.h`・`track_player.h`・`playback_types.h` だけで、コールバック側の描画（`segment_renderer`）、要求シグナル（`playback_signal`）、区間・要求の制御（`segment`・`request`）、出力形式の取り決め（`output_format`）は内部ヘッダーとする。`control` の入口は `playback_controller.h` だけ。SDK は `asio/asio_sdk.h` からだけ取り込み、`ui` 層・`src/app/main.cpp`・`control/playback_controller.h` からたどれる製品ヘッダーのどれにも含めない。これらは `scripts/check-project-layout.ps1` が直接のinclude、公開ヘッダー、推移的なinclude閉包として検査する。

| 層 | 責務 | 依存先 |
|---|---|---|
| `core/` | エンディアン（`load_le`・`store_le`）、Windows に依存しない整列（`alignment.h` の `cache_line_size`・`align_up`、別スレッドが書くデータだけを隔てる128バイトの隣接ライン対 `adjacent_line_pair`）、実時間経路のブロックのコピーとゼロ埋め（`block_copy.h` の `copy_block`・`copy_stereo_blocks`・`duplicate_block`・`zero_block`。128 bit整数の16バイト単位で、呼び出し元へインライン展開する）、Win32 のハンドル・`LocalFree` で返す領域・例外（`UniqueHandle`・`LocalPtr`・`throw_last_error`・`throw_win32_error`）、厳格なUTF-8からUTF-16への変換（`utf8_to_wide`。不正なUTF-8では値を返さず例外を投げないので、設定・一覧・エラー表示がそれぞれの文で報告する）、自前のファイルアクセスの間だけ呼び出しスレッドの重大エラーをダイアログでなく失敗にする範囲（`CriticalErrorScope`）、一度だけ照会して共有するページサイズ（`page_size`）、ページ単位の確保、専用ページのC++オブジェクト、破棄しないプロセス寿命オブジェクト（`PageAllocation`・`ResidentObject`・`NoDestroy`）、自動スリープ抑止要求の所有（`SystemSleepGuard`）、自アプリのスレッドだけに付ける名前と実行速度のヒント（`thread_hints.h` の `name_current_thread`・`ExecutionSpeed`・`set_current_thread_speed`・`ThreadSpeedScope`。`SetThreadDescription` と `SetThreadInformation(ThreadPowerThrottling)` を呼び、失敗は無視する）、STA の期限付き待機とメッセージ処理（複数イベントの `wait_for_events_until` と1つの `wait_for_event_until`・`pump_messages`・`quit_pending`）、下位層の待機で上位層の確認を呼ぶ起床フック（`WakeHook`。イベントと関数ポインターだけで、上位層の型を持たない） | なし |
| `audio/` | サンプル形式の語彙（`SampleFormat`・`StreamFormat`・対応レート）と、インターリーブから左右の平面への純粋な変換カーネル（`find_planar_converter`） | `core` |
| `media/` | ストレージの入口（`ReadOnlyFile`。ローカルvolumeとみなすドライブ種別 `is_local_drive_type` は一覧の事前確認と共有する）、WAV コンテナーの検証（`parse_wav_header`）、フレーム単位のカーソルと変換（`WavReader`） | `core`・`audio` |
| `stream/` | 同一レートの曲をつなぐ生産者側の供給元（`GaplessRun`）、リングの寸法計算（`plan_block_shape`・`plan_ring_layout`・`PrefetchPolicy`）、読み込みスレッドと SPSC リング（`PrefetchRing`） | `core`・`audio`・`media` |
| `asio/` | SDK の境界。SDK ヘッダーの唯一の取り込み口（`asio_sdk.h`）、登録とアクティベーション（`driver_registry`）、STA とドライバーの寿命・元レートの復元（`DriverSession`・`DriverLease`）、停止中の照会（`driver_queries`）、ドライバーのクロック値をビットで検査する補助（`clock.h`）、`ASIOSampleType` から `audio::SampleFormat` への対応（`sample_types`）。機器別設定（`device_settings.h`）と登録の宣言（`driver_registry.h`）は SDK を取り込まない | `core`・`audio` |
| `playback/` | リアルタイム再生エンジン。出力ペアの形式の取り決め（`output_format`）、要求の世代と停止理由（`PlaybackSignal`）、1区間のバッファー寿命と要求ごとのPause/Stop（`segment`・`SegmentControl`）、コールバック側の描画（`segment_renderer`）、曲間のレート変更を含む1要求（`request`）、公開入口（`play_tracks`・`TrackPlayer`。待機中の故障監視を含む） | 上記すべて |
| `settings/` | UTF-8 INIの解析、機器・機器別設定・ウィンドウ状態の起動時読み取り、原文を保持したウィンドウ状態の保存。監視は行わない | `core`・`asio`（SDK非依存のヘッダー） |
| `playlist/` | UTF-8リストの全体検証、ローカル絶対パスとWAVの検証、順序・重複を保った一覧と除外件数（`Playlist`）。起動時に一度読む。停止の旗で項目の単位に打ち切り、途中までの一覧は返さない | `core`・`media` |
| `control/` | 専用 STA で機器・一覧・WAVを所有し、別スレッドからの再生・選択操作を直列化する非同期コントローラー（`PlaybackController`） | `playback`（公開入口）・`asio`・`core`・`settings`・`playlist` |
| `ui/` | プレイリストのWin32ウィンドウとメッセージループ（`player_window`）、ウィンドウに触れない表示文字列と操作可否（`player_text`・`available`。数値は整数演算だけで書き、`std::format`・ロケール・浮動小数点の文字列化を使わない）、標準コントロール・配置と子コントロールID（`player_controls`・`control_id`）、起動引数（`player_options`）、プロセス間の単一起動と前面化（`single_instance`）、例外と入れ子の原因のUnicode表示（`error_message`） | `control`・`asio`（SDK非依存のヘッダー）・`core` |

アプリ本体（`wavplay.exe`）の入口は `src/app/main.cpp` だけで、`ui` と `core` だけを取り込む。コンソールテストは `tests/wavplay_tests/` にある。両方が [同じ製品ソース一覧](../build/WavplaySources.props) をビルドし、プレイリストGUI（`ui` 層）の実際のウィンドウも模擬ASIOで試験する。引数なしの起動では実行ファイル隣の `playlist.txt` を読む。`settings.ini` で使用機器、機器別の出力ペア・BufferSize・StartSilenceを指定し、ウィンドウ位置・サイズ・最大化状態を保存する。単曲の確認用に `--wav path` も残す。

## スレッドとリアルタイム境界

| 実行の場 | 担当するコード | 守ること |
|---|---|---|
| UIスレッド | `src/wavplay/ui/player_window.cpp` のWin32ウィンドウとメッセージループ | 操作は `submit`、表示は `refresh_snapshot`（必要時は完全コピー）。ドライバーやファイルを直接扱わず、初期化・停止・解放を待たない。例外はセッション終了の `WM_ENDSESSION` だけで、解放と保存を最大約4秒待つ（送信メッセージだけを処理する。[§4](#4-操作画面設定実機の接続)）。スレッド名は `WAV Player UI`（製品の主スレッドとして `main.cpp` で付ける）。ウィンドウ作成後から `run_player_window` が戻るまで `ui::ui_thread_speed` を要求する。現在はWindows既定の `system` で、終了時も既定へ戻す |
| 制御スレッド（COM STA） | `asio::DriverSession`、`playback::run_request`・`run_segment`、`control::PlaybackController` の作業スレッド | ドライバー呼び出しとドライバーの生成（`asio::ActivateDriver`）はすべてここ。待機は `core::wait_for_events_until`（1つなら `wait_for_event_until`）で単調時計の期限を守り、待機中もメッセージを処理する。結果は `core::WaitResult`（`signaled`・`timed_out`・`quit`・`failed`）と、`signaled` なら消費したイベントの番号で返し、同時に立てば番号の小さいイベントを優先する。WM_QUITは外側のループへ残して `quit` とし、同じ起床で消費したイベントは立て直す。期限前に投稿されたWM_QUITは、処理の上限で後回しになっても期限切れより優先する。再生の待機は `quit` をStopと同じ `stopped`、`failed` を `driver_error` で終える。先読みリングの初期充填を待つ間も同じ待機を使い、再生通知のイベントでも起きて制御の確認（`SegmentControl::poll`）を行う。初期充填の完了と同時なら完了を優先する。スレッド名は `WAV Player control`。ドライバーの呼び出しとドライバーによるスレッドの生成がここで起きるので、実行速度のヒントは付けない |
| 読み込みスレッド | `stream::PrefetchRing` の生産者、`GaplessRun`、`media::WavReader`、`audio` の変換カーネル | ファイル I/O と変換を行い、完成したブロックだけを公開する。スレッド名は `WAV Player reader`。準備（後続曲の24/32の全件検証を含む）と初期充填はスレッドの既定のまま行い、初期充填の完了を知らせた直後に1回だけ `stream::reader_refill_speed`（現在はWindows既定の `system`）を要求して、以後の補充に使う |
| ASIO コールバック | `playback/segment_renderer.cpp` のコールバック関数と `SegmentRenderer` の描画（下記）、`stream::PrefetchRing::Consumer::pop/read_failed` と `PrefetchRing::pop_block`、`PlaybackSignal` のインライン操作、`core/block_copy.h` のインラインのコピー・ゼロ埋め | 準備済みデータのコピー、ロックフリーなカウンター更新、必要な閾値での通知に限る。通知はカーネルイベントの `SetEvent` だけで、生産者の起床閾値（目標約2秒のリングでは約0.5秒ごと）と停止理由の確定時に呼ぶ。確保、ロック（`WaitOnAddress`/`WakeByAddress` の利用者モードのロックを含む）、待機、ディスク I/O、ログ、GUI 操作を持ち込まない。ドライバーのスレッドの名前・優先度・アフィニティ・実行速度には触れない |

コールバックから到達するコードは `segment_renderer.cpp` という1つの翻訳単位に集めてある。このうちコールバック文脈で動くのは `switch_buffer`・`sample_rate_changed`・`asio_message`・`is_streaming` と `SegmentRenderer::render`（手順の `note_switch`・`accept_sequence`・`accept_stop`・`deliver` と `pop` は強制インライン展開）・`drain_latency`・`silence` で、同じファイルの `callback_routing::attach`・`detach`・`set_streaming`、`SegmentRenderer` の構築・`set_output_latency`・`prefill` は制御スレッドのコードである（`detach` は書き込み中のコピーを待って譲る）。ほかにコールバックが触れるのは `PlaybackSignal` のインライン操作（クロックの検査は `asio/clock.h` のビット比較）、`pop_block`・`silence` へ展開される `core/block_copy.h` のコピー・ゼロ埋め（CRTの `memcpy`・`memset` は呼ばない）と専用ページ内のPause/Stop要求フラグだけ。RendererはリングのresidentなConsumerと要求atomicへの参照を開始前に保持し、pageableなPrefetchRing/SegmentControlの外側をコールバックからたどらない。提出フレーム数は `SegmentRenderer` の1つのatomic（`progress_frames_`）だけに数え、PCMブロックの提出ごとにロックなしのloadとstoreで更新する（書き手はpopする1つのRendererだけ）。制御側はこの値を区間内の曲番号・位置へ写像する。切替の回数（`switches_`）も同じ行に数え、callback不着の監視は期限ごとの読み取りを前回の読み取りと比べる。両面の描画が重なって増分を1つ失っても、変化の有無だけを見る監視には影響しない。書き手の接続中、制御側がRendererで読むのはこの行だけで（監視と排出の期限に使うブロック幅は接続前に `SegmentRunner` へ控える）、Rendererには書かない。Rendererの最初の128バイトの対はコールバック専用の2行、次の対はまれにしか使わない行とこの共有行で、定常のPCMの切替が触れるRendererの行は3つ。`SegmentRunner` の破棄時はdetachで全writerを待ち（writerのフラグでacquireする）、同じatomicから確定値を読む。次区間では提出数を0から数え直す。[試験計画](test-specification.md)の「コールバックから到達する関数を実装変更時に読む」確認は、このファイルと `PrefetchRing::Consumer::pop/read_failed`・`PrefetchRing::pop_block`（read_failedはatomicの読み取りのみ）、`core/block_copy.h` を読めば足りる。SDK のコールバックには文脈がないため、プロセスに1つのコールバック表と `PlaybackSignal` を、同時に1つの `SegmentRenderer` へ振り向ける（`callback_routing`）。切替ごとに書く `writers`・`active`・`streaming` の行と、`PlaybackSignal` とドライバーへ渡すコールバック表の行は、1つの128バイトの塊（`CallbackState`。1ページ・隣接行の対1つ）に置き、定常の切替が触れる静的なデータはこの塊だけになる（閾値と停止・故障で呼ぶ `SetEvent` のインポートアドレス表の欄は別のページにある。製品ではどちらのページもコードとともに、起動時に固定するイメージに含まれる。[チューニングの入口](#チューニングの入口)のメモリ配置）。2行目はコールバックから見て読むことが多いだけで、制御側も起床（`segment.cpp` の `control_wake_period`。0.5秒～1秒弱に1回）ごとに `PlaybackSignal` を読み、書くのは要求と相の境界、停止・故障のときだけである。静的破棄の後に届く通知にも備えて塊全体を破棄せず（`core::NoDestroy`）、どのメンバーの寿命も終わらない。`constinit` で定数初期化するので、動的初期化の順序に依存しない。ただし、`NoDestroy` の空のデストラクターをatexitへ登録する動的初期化子が1つ残る（何も破棄しない）。

スレッドの名前と実行速度のヒントは `core/thread_hints` だけで扱い、自アプリの読み込み・制御・UIのスレッドにだけ名前を付ける。名前はETW・デバッガー・ダンプでドライバーのスレッドと区別するためで、スレッドの開始時に1回だけ付ける。実行速度はWindowsのQoSへのヒント（`SetThreadInformation(ThreadPowerThrottling)`）で、失敗は無視する。優先度・アフィニティ・MMCSS・メモリやI/Oの優先度は変えず、`THREAD_MODE_BACKGROUND_BEGIN` も使わない（リングのページと先読みの優先度まで下げるため）。

明示のヒントがないスレッドのQoSは、ウィンドウの状態、OSによる音声再生の判定などからWindowsが決める。[公式の分類](https://learn.microsoft.com/en-us/windows/win32/procthread/quality-of-service)では音声再生中と判定されたプロセスもHighQoSの対象で、非前面なら必ず低くなるとは限らない。特定のコアへの配置や起床間隔を保証する設定ではない。起床はコールバック1回と同じではなく、起床遅延とブロック時間だけを比べて故障判定しない。

| 対象 | 現在の方針 |
|---|---|
| 読み込み | 準備・全件検証・初期充填は既定。初期充填後の補充も `system` を指定し、Windowsの判断に委ねる |
| UI | `system` でWindowsの判断に委ねる。HighQoS/EcoQoSを固定せず、終了時も既定へ戻す |
| 制御STA・ドライバー | 個別ヒントとプロセス全体のHighQoSを付けず、Windowsの判断に委ねる |
| タイマー分解能 | 製品は`timeBeginPeriod`を呼ばず、ドライバーの既存の要求をOS既定の方針で扱う。`PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION`のopt-outも追加しない |

読み込みの補充とUIはWindows既定の実行速度を使い、GUIや設定ファイルからQoSを切り替える機能は持たない。通常進捗の軽量更新も完全snapshotと同じロックを使うが、コピーはrevisionとpositionの2欄に限る。C++のmutexは優先度継承を契約にせず、本実装もOSの一時的な優先度調整を保証として使わない。ロックの保持中は制御STAの確認が待ち得るが、コールバックはこのロックを取らない。

## チューニングの入口

読み込み・変換・メモリ配置の調整箇所を次のように分ける。リアルタイム側の `pop` と `render` は、これらの設定を変えても独立して保つ。

| 対象 | 入口 | 現在の値・方式 |
|---|---|---|
| ストレージ | `media::ReadOnlyFile`（共有・アクセスフラグ）、`media::WavReader::staging_bytes`・`validation_bytes` | `FILE_FLAG_SEQUENTIAL_SCAN` の同期キャッシュ読み取り。各 `ReadFile` は同期ハンドルのまま `OVERLAPPED` で絶対位置を指定し（`ReadOnlyFile::read_exact_at`）、ハンドルのファイルポインターを位置の正本にしない。位置は符号付き64 bitの範囲に限り、ファイルポインターから読む指定（`FILE_USE_FILE_POINTER_POSITION`、−2）を含む範囲外は読み取り前に拒否する。`WavReader` は残りフレーム数から読み取り位置を求めるため、seek・曲境界ではI/Oを行わず、reopenも `ReOpenFile` で別ハンドルを開くだけで位置合わせのI/Oを伴わない。末尾到達（`ERROR_HANDLE_EOF`）と短い読み取りはどちらも「Unexpected end of file」とする。`ReadOnlyFile` のopen・reopen・サイズ取得・読み取りとプレイリストのvolume照会は、その呼び出しの間だけスレッド単位の `SEM_FAILCRITICALERRORS`（`core::CriticalErrorScope`）で行い、媒体のないドライブではシステムのダイアログで止まらず失敗する。再生データの1回の `ReadFile` は最大 8 KiB で、キャッシュライン境界のステージング領域に読んでから変換する。再生と重ならない24/32の事前検証だけは `WavReader::validation_bytes`（64 KiB）ずつ読む。非バッファー・重複 I/O はセクター境界の確保が要るため、この領域の確保方法と合わせて変える。読み取り単位を変える場合はI/O回数とサンプル一致を併せて検証する。 |
| 変換・CPU | `audio/planar_conversion.cpp` のカーネルと `find_planar_converter` | [命令セットの前提](#命令セットの前提)に従う128 bit整数AVX2と非整列ロード・ストア。左右分離・Float64組み立て・PCM24の読み込みは既存の128 bit構成を保つ。正規化の条件付きシフトを `VPSLLVD` にまとめ、単純な固定幅ループはSIMD終端を事前計算する。packed出力の安全な読み込みに必要な追加フレーム条件は維持する。LSB同幅モノの `core::duplicate_block` は既存のコピーを左右へ別々に行う。Float32 subnormalの判定単位は4サンプルを維持し、Float出力も整数演算だけで組み立てる。端数は有効範囲だけを扱い、入力paddingを要求しない。範囲外アクセスと変換後の全バイトを独立した期待値で検証する。 |
| メモリ配置 | `core::PageAllocation`、`stream::plan_block_shape`、`PrefetchRing::Impl` | `VirtualAlloc` のコミット済みページを確保時に `VirtualLock` で固定し、確保の間だけ最小・最大ワーキングセットを確保量だけ広げ、解放時に戻す。`ResidentObject<T>` は専用ページでC++オブジェクトを構築・破棄し、リングのImpl、Renderer、Pause/Stopフラグにも固定を試みる。共有CRTヒープは固定しない。片側32バイト以下の左右は同じ64バイトラインの前半・後半へ置き、異なるブロックは同じラインへ詰めない。Implでは `pop` が読む配置情報、生産者・消費者のカーソルをそれぞれ別の128バイトの対へ置く。`main.cpp` は単一起動の確定後、ドライバー初期化前に `core::lock_process_image` を1回呼び、`[__ImageBase, __ImageBase + SizeOfImage)` のヘッダーと全セクションをプロセス終了まで固定する。ワーキングセットは実際の `SizeOfImage` だけ広げる。書き込みで専用コピーになる `.data` の固定も試験で確認し、コールバックから再固定する処理は持たない。固定対象はアプリ所有の領域だけで、ドライバーDLL、システムDLL、カーネル、スレッドのスタック、CRTヒープ、ドライバーバッファー全体は保証しない。`/SWAPRUN:CD,NET` ではリムーバブル・ネットワーク媒体の実行イメージをWindowsがページングファイルへ写す。固定を拒否された領域はページング可能なまま使い、`BufferSetup` の音声平面・リング管理情報・Renderer・要求フラグ・イメージの5項目へ結果を分けて残す。イメージはプロセスで1回の結果で、固定しない試験プロセスでは `false`。これらは診断コマンドだけが表示し、GUIには表示しない。初期充填では全ページへ書き込む。ラージページやNUMA配置の変更も `PageAllocation` の責務とする |
| 先読み量 | `stream::PrefetchPolicy` | 両チャンネルと専用管理ページの実コミット量を合わせて16 MiB以下、目標約2秒、リング1周あたり4回を目安とする生産者起床。1バッチは現在の曲の元データ 8 KiB を目安とする整数ブロック数（最低1ブロック）で、出力のサンプル幅に依存しない。曲ごとの上限は準備時に計算し、曲末を含むブロックまでに区切って次の形式へ追随する。大きな単一ブロックは複数回の読み取りで埋める。変換はステージング領域から整列済みスロットへ直接書き（`audio::PlanarBlocks`）、左右それぞれの有効範囲だけを書き、どちらにも属さない隙間には触れない。起床の頻度とバッチの大きさは、この入口で管理する。 |
| コールバック | `segment_renderer.cpp`、`core/block_copy.h`、`PoppedBlock`（2値を RAX 上で組み立てて返す） | `core::copy_stereo_blocks` が左右を別々の `copy_block` でコピーし、無音化は `zero_block`。128 bitの定数幅を保ち、末尾は有効範囲内で重ねたアクセスを使う。外部ASIOバッファーの整列やpaddingを仮定しない。CRTコピー・非テンポラルストア・ISA再dispatchは使わず、呼び出し元へ展開する。満杯で待つ生産者は自動リセットのイベントで起こす。`PoppedBlock` はレジスター上で組み立てる。 |
| ドライバー・USB | `asio::DeviceSettings`、`segment.cpp` の `switch_watchdog_periods`・`control_wake_period`（監視期限とその下限）と `driver_queue_linger_periods`（排出後の待ち） | 開始時の無音は既定100 ms。監視窓は「制御の起床周期と4ブロック時間の長い方」とし、起床周期は区間ごとに1秒から20 msと1ブロック時間を引いた値（下限0.5秒）。排出後に2ブロック分の実時間を待つ。報告された出力遅延が2秒を超える機器は、排出が終わらないため開始前に拒否する。`outputReady`・TimeInfo・その場での再同期などのASIOの任意機能は採らない（[契約](asio-contract.md#任意機能と未定義時の復旧)） |
| スレッドの実行速度 | `stream::reader_refill_speed`（`prefetch_policy.h`）、`player_window.h` の `ui_thread_speed`、`core::ExecutionSpeed` | 読み込みの補充とUIはともにWindows既定（`system`）を使う。制御STA・ドライバーのスレッドへ個別の実行速度ヒントを加えない |

平面出力の寸法は毎回I/Oより前に検査する。`check_planar_blocks` はx64の `_umul128` でブロックのバイト数と最後のブロックのoffsetを計算し、上位積がゼロで下位積が上限内の場合だけ再利用する。最後のサンプルの加算、実アドレス、左右の重複も従来順に検査する。空の要求は寸法によらず0を返し、部分読み取りにブロック全体の格納領域を要求しない。読み取りごとの呼び出しと保存・復元を避けるため、この内部検査を明示的にインライン化する。公開APIを「検証済み」型へ分岐させない。

初期充填の時間と読み取り回数は `wavplay_tests.exe --measure-prefetch`（PCM16）、`--measure-prefetch pcm24`（PCM24）、`--measure-prefetch pcm32`（PCM32）、`--measure-prefetch ext24in32`（24/32の全件検証込み）、`--measure-prefetch float32`・`--measure-prefetch extfloat32`（通常/Extensible Float32）で測る（[README](../README.md#ビルドと検証)）。

PCM24/32もPCM16と同じ元データ量に応じたバッチ、整列スロットへの直接変換、密配置の一括変換、ページ固定、コールバックを使う。形式専用のリングや制御経路は持たない。PCM24→packed24のSIMDは16バイトの入力ロードが範囲内になる残りフレーム数でだけ実行し、SIMDストアで各出力へ余分に書くモノ4・ステレオ2バイトは次の反復か端数処理で上書きする。PCM24→32 bitの2回のロードは変換するフレームの24バイトに収まり、先読みしない。16 bit・Int32・Float出力では4・8フレーム単位のSIMDに満たない端数をスカラーで処理し、回数を `frames & 3`・`frames & 7` で数えて上限を見せ、MSVCが到達しないベクトルループと別名検査・レジスター退避を生成しないようにする。入力・出力の端を保護ページへ接した試験と、隙間のガードを使う分割変換試験で、範囲外アクセスとスロット間の書き換えを検査する。

整数出力14型もこの経路を共有する。出力のバイト順・有効幅をテンプレート引数として選び、サンプルごとの形式分岐を置かない。MSB出力では、可能な箇所でバイト反転を左右分離・packed24のshuffleへ融合する。PCM24→32 bitは融合せず、`format_int32` のバイト反転を1回加える。32 bit容器では上位配置のInt32値を共通の作業表現とし、有効Nbitへ算術右シフトして上位の符号拡張を保ち、必要な場合だけバイト順を反転する。LSB同幅のモノは一括コピーのまま、SIMD幅も従来のままとし、変換後の並べ替えバッファーやコールバック処理を追加しない。先読み計測は第2引数に `ASIOSTInt32MSB18` や `ASIOSTFloat64MSB` などの出力名を渡せる。

Floatの4出力型も同じリング・制御・GUI経路を使う。Float32→Float32はPCM32のコピー/左右分離/バイト反転カーネルをそのまま使い、PCM→FloatとFloat32→Float64は4サンプル単位の整数SIMDで符号・正規化・指数・仮数を構成する。PCMの符号なし絶対値はSSSE3 PABSDで求める（INT32_MINは0x80000000）。Int32スケールへ左詰めしたPCM16は非ゼロなら先行ゼロが15以下なので16bitの正規化段を省く。Float32→Float64は通常値・符号付きゼロ・無限大・NaNを指数補正と仮数移動だけの経路で求め、subnormal（指数0で仮数が非ゼロ）を含む4サンプルのベクトルだけ整数正規化へ進むので、無音のゼロでも処理量は通常の音楽と同じになる。Floatの符号化と正規化の関数は `__forceinline` とし、カーネルのループから呼び出しをなくす。packed24のロードは8＋4バイトで4サンプル分に収め、Float64は16バイトずつ2回書く。端数のスカラー処理も整数だけを使う。余分な中間配列やFloat専用のコールバックは設けない。

小ブロックの左右同居は、同じブロックの両チャンネルを生産者が所有し、両方の書き込み後に一度だけ公開する既存の契約に基づく。`RingLayout::channel_offset` は右チャンネルの位置、`plane_bytes` は周回幅として区別する。標本領域の実容量で2秒目標と予算を計算し、専用管理ページを含むコミット量16 MiB上限を保つ。大きいブロックは従来の独立した左右平面を使う。

`PlanarBlocks` の左右の外接範囲が重なる場合は、共通strideをS、有効ブロック幅をB、base間距離のSに対する剰余をrとして `r >= B && S - r >= B` を追加で確認する。これにより同じスロットと隣接スロットの両方で実際の有効範囲が重ならない。アドレス・フレーム範囲のオーバーフロー検査を先に行い、隙間の書き込みを許す契約へ緩めない。要求した部分範囲の外接範囲がもともと非重複の場合は、従来どおりその範囲を扱う。
## 命令セットの前提

対象OSはWindows 11 25H2以降のx64とし、製品本体とテストは `/arch:AVX2`（[build/Common.props](../build/Common.props)）でビルドする。CPUはAVX2に加え、MSVCが使用できるBMI1/BMI2・LZCNT・MOVBE・FMA・F16C等を持ち、WindowsがXMM/YMMの状態保存を有効にしている必要がある。`core::query_cpu_features` がCPUID、`GetEnabledXStateFeatures`、WindowsのAVX/AVX2機能照会を集め、純粋な `supports_required_cpu` が必要bitをすべて検査する。CPU名やベンダーでは判定しない。対応しない場合は固定メッセージを表示して非ゼロで終了し、再生用のSSEフォールバックは持たない。対応CPUでの128 bit整数処理と端数のスカラー処理は許す。

ガードを置く `src/app/startup.cpp` と `core/cpu_features.cpp` だけはSSE4.2・LTCG対象外でビルドする。通常のCRT起動を保ち、本体へ進む前に命令セットを確認する。`scripts/verification/startup-code.ps1` はReleaseのmap・PE・機械語から、製品側のCRT初期化表、TLS callback、登録される終了処理とガードの到達先を調べ、判定前と拒否後の終了にAVX2命令が混入する変更を拒否する。WindowsとプリビルドCRT内部の安全な機能判定は信頼境界とする。起動ガードの外側の静的初期化へ処理を足す際も、この制約を満たす必要がある。

手書きの音声カーネルは整数のVEX128/AVX2を使い、音声を浮動小数点演算へ渡さない。ISAの選択は起動時の一度だけで、変換ループとコールバックに再dispatchを入れない。CRT/STL内部の機能判定は残るが、実時間のコピーとゼロ埋めは自前の範囲内アクセスで行う。`verify.ps1` のコールバック監査は整数AVX2/BMI等を許し、浮動小数点演算、AVX512、CRTコピー、不要な間接呼び出しとCPU再dispatchを引き続き拒否する。非テンポラルストアやソフトウェアprefetchは測定なしに加えない。

言語規格は `/std:c++23preview`（`LanguageStandard=stdcpp23`）でビルドし、C++23 の上限をコンパイラーに強制させる（`_HAS_CXX26=0`）。ツールセットが `/std:c++23` を提供したらそれへ移る。Windows API の可視範囲は `_WIN32_WINNT`・`WINVER`（`_WIN32_WINNT_WIN10`）と `NTDDI_VERSION=NTDDI_WIN11_GE`（25H2 と共通の 24H2 の API 集合）で、要求の最小 OS に固定する。SDK（`WindowsTargetPlatformVersion`）とこの固定は同じ変更で見直し、SDK を更新しても値は変えず、最小 OS を上げるときだけ上げる。planar_conversionの試験群が、コンパイラーから見た言語規格・`__cplusplus`・規格準拠のプリプロセッサー・APIの固定・Unicodeの値をコンパイル時に確かめ（SDK 10.0.26100では固定を外しても既定値が同じなので検出しない）、`verify.ps1` が両プロジェクトの評価した設定の規格と共通のスイッチを確かめる。翻訳単位ごとの `AdditionalOptions` は共通のスイッチと上記ガードの明示的なSSE4.2例外だけを許し、他の後置きの上書きを拒否する。Releaseだけが `NDEBUG` を定義し、製品の標準の `assert` はDebugだけで評価する（MSVCのSTLの検査は `NDEBUG` ではなく `_ITERATOR_DEBUG_LEVEL` で決まる）。`verify.ps1` は生成したReleaseの `wavplay.exe` にCRTの `_wassert` がないことを確かめる。

CET シャドースタックと CFG（Control Flow Guard）はどちらもプロセス全体に効くため、有効にするとプロセス内で動くドライバーのコールバックスレッドの実行環境も変わる。ドライバーのスレッドの優先度・アフィニティを変えないという要求と同じ趣旨で、どちらも有効にしない。CETCOMPAT の EXE ではドライバーのスレッドにもシャドースタックが付き、毎回の call/ret に保存と照合が加わる。Windows 既定の互換モードは CETCOMPAT でないモジュール（ドライバー DLL など）での違反を許容し、強制終了は厳格モードに限られるので、論点はクラッシュではなく実行環境の変化である。`/guard:cf` の EXE では、CFG 対応のシステム DLL がドライバーのスレッドを含む全スレッドで実際のビットマップ検査を行う。リンクは `/CETCOMPAT:NO` を明示し、CFG と Spectre 緩和は既定（無効）のままとする。`scripts/verify.ps1` は Release の `DllCharacteristics` に GUARD_CF（0x4000）がなく、ヘッダーに `CET compatible` がないことを確かめる。代償として、信頼できない WAV 入力の解析・変換に対する悪用緩和は Microsoft SDL の推奨（両方を有効）より弱く、ヘッダー検証と境界検査に依存する。判断を変えるときは `build/Common.props`・`verify.ps1`・この段落を同じ変更で改める。

浮動小数点の演算は音声データに使わない。ドライバーが返すクロック値（double）は `asio/clock.h` でビットとして検査・比較し、シグナリング NaN による例外や MXCSR の変化を避ける。PCM・Floatとも入力を整数ビット列として扱い、IEEEの符号・指数・仮数も整数演算で構成する。同幅Float32は全ビットをコピーし、Float64への拡張は符号付きゼロ・subnormal・無限大・NaNの符号とquiet/signalingビット・ペイロードを保持する。PCMの数値は `sample / 2^(valid_bits - 1)` へ正規化する。丸め・クリップ・FTZ/DAZへの依存やMXCSRの変更を加えない。

## 最適化の方針

`TrackPlayer` は、再生要求内では各indexの供給元が不変であることを使い、現在の絶対indexが同じ間は `track_` を再代入しない。最初の入力は保持中の `track_` を使い、曲境界でだけ次の所有権へ更新する。同じパスが別の項目として続く場合もindexは異なり、定めた解放順序を守る。同じ項目の再スタートでも、別の要求として先頭から再生する契約は変えない。

要求内の `SourceCache::open` は、後で同じパスを使う間だけキャッシュの所有参照を複製する。最後の使用ではその参照を返値へ直接移し、複製直後にresetする参照数更新を省く。残り使用数はopen成功後にだけ減らす。呼び出し元が同じ保護ハンドルを保持するため、ファイルの保護期間・再検証・重複項目の順序は変わらない。

`GaplessRun` は、曲境界の所有権更新とseekをprivateな `advance_track` に分離し、MSVCの `noinline` を指定する。通常の読み取り側へ境界用のコピーやメタデータ更新を展開させないためである。呼び出す位置は従来どおり、前曲末を確認した後、出力ブロックが満杯かを判定する前とし、move→seek(0)の順序、同一ブロック内の複数境界、読み取り失敗の伝播を保つ。準備時の `append` は、次曲の保持成功後にだけ総フレーム数を更新する。

生成コードは最終EXEで確認する。命令数や実行時間の変化だけを、電力・アナログノイズ・音質の改善とはみなさない。

最適化はビルド構成とディレクトリで決める。Releaseは起動ガードの2翻訳単位を除き、両プロジェクトとも全体最適化（`/GL` と非増分の `/LTCG`。`Microsoft.Cpp.props` の後に読む `build/Common.props` なので、ClCompile・Linkの項目メタデータで指定する）でビルドし、DebugとDebug-ASanは全体最適化を使わない。非増分なので、前回のリンクの `.iobj` が残っていても生成コードは同じになる。`verify.ps1` は、評価した設定でReleaseだけが全体最適化であることと、生成したReleaseの `wavplay.exe` のデバッグディレクトリにLTCGの印があることを確かめる。`.obj` は中間表現で機械語を持たないため、機械語は生成したRelease実行ファイル（`verify.ps1` の監査と `dumpbin /disasm`）か、リンク時に書かれる `/FAcs` の一覧で確かめる（一覧は最終の機械語を示すが、`/OPT:REF` で除かれる関数も含む）。COMDATの畳み込み（`EnableCOMDATFolding`）は指定しない。明示した `/OPT:REF` に伴う限定的な畳み込みが同一のコードの関数をすでに1つの番地へ重ねるので、別の関数が別の番地を持つことは今も保証しない。明示的な `/OPT:ICF` は追加しない。

最適化の水準はディレクトリごとに決める。`ui`・`settings`・`playlist` の翻訳単位はReleaseでサイズ優先（`/O1`）、それ以外の層と、ドライバーを所有して待機とドライバー呼び出しを行う制御STAの `control`、`main.cpp` は速度優先（`/O2`）とする。`build/WavplaySources.props` のInclude群の後の1つのUpdate項目で指定するので、両プロジェクトで同じになり、ファイルを加えても層のディレクトリが水準を決める（`check-project-layout.ps1` はInclude項目だけを読む）。`verify.ps1` は両プロジェクトのReleaseの評価結果で、製品の翻訳単位ごとに、この3つのディレクトリ（下位のディレクトリを含む）では `/O1`、それ以外では `/O2` であることを確かめる。Update項目の非再帰の指定が下位のディレクトリのファイルに届かなければ失敗する。全体最適化では、`/O1` と `/O2` の翻訳単位が共有するインライン関数・テンプレートは、リンカーが中間表現を採った側の設定で生成される。`verify.ps1` の監査は、コールバックの入口とそこからたどった関数に `wavplay::ui`・`wavplay::settings`・`wavplay::playlist` の関数があれば失敗する。関数名で判定するので、コールバックの関数へ展開されたコードと、他の名前空間の共有関数が `/O1` の中間表現から生成された場合は検出しない。共有関数の生成元や展開後の最適化水準については、この名前空間検査だけでは保証しない。

## 拡張点

| 追加するもの | 変更する場所 |
|---|---|
| GUI | `src/wavplay/ui/` の標準リストと選択操作を使う。`ui` の各ファイル・`main.cpp`・`control/playback_controller.h` からたどれる製品ヘッダーに SDK を含めない（構成検査が推移的に確認する）。`ChangeListener` は集約した `PostMessageW` で通知し（イベントは投稿できない場合だけ）、UIスレッドで `refresh_snapshot` を読み、通常進捗以外は完全コピーする。進捗だけの公開は任意の `progressed` で受け、最小化中はUIを起こさない |
| 連続再生 | `playback::run_request` は任意長の曲列と、開始フレーム・診断・進捗・拒否方針（`RefusalPolicy`）を指定子付きで渡す `RequestOptions` を、Pause/Stop（`SegmentControl`）から独立に受け取る。各項目の拒否を記録する方針（`record_each`）は拒否項目を返す進捗を必須とし、なければドライバーに触れず `invalid_argument` とする。同一レート区間だけを1つのリングへ渡し、`TrackPlayer` が提出フレーム数を曲番号と曲内位置へ写像する |
| プレイリスト | `playlist::parse/load` はGUIに依存せず、一覧と除外件数を返す。表示と再生は制御層が所有する不変一覧を使い、監視やリスト編集は行わない。設定は `settings/` に分離する |

## コンテナーと64ビット位置

`media::parse_wav_header` はRIFF/WAVEとRF64でチャンク走査と形式検証を共用する。RF64だけは先頭の `RF64/WAVE` 直後に `ds64` を要求し、外側のサイズ・dataサイズ・factのフレーム数が `0xFFFFFFFF` の場合に対応する64 bit値を使う。それ以外は元の32 bit値を使い、参照しない64 bit値との一致は要求しない。この置換規則は [EBU Tech 3306-2009 §3.4](https://tech.ebu.ch/docs/tech/tech3306v1_1.pdf) に従う。BW64は受理しない。

`ds64` は28バイトの固定領域と宣言された12バイト単位の追加表がチャンク内に収まることを検査し、追加の予約領域・奇数長パディングを許す。data以外のサイズが `0xFFFFFFFF` のチャンクは追加表から長さを解決する。同じIDの項目は出現順に使用し、異なるID間の順序は問わない。表はファイル上の位置・件数として保持し、実際に要求されたIDの検索位置だけを記憶して、最大8,184バイトずつ読む。未使用の表全体をメモリへ確保したり、先行して走査したりしない。使わない表の項目は無視する。

チャンクのヘッダー・本体・パディングを、加算前にコンテナー末尾と実ファイル長との差で制限する。fmt/dataの欠落・重複、RF64内の2つ目のds64、必要な表項目の欠落は拒否する。RIFF内のds64は従来どおり未知チャンクとして扱う。Float32はfact欠落を互換受理し、存在するfactをdataから求めた完全フレーム数と照合する。RF64のfact値が `0xFFFFFFFF` の場合だけds64.sampleCountを使い、通常値なら32 bitの宣言値を使う。PCMでは従来どおりfactを検査せず、ds64.sampleCountも用いない。

`WavInfo` のデータ位置・フレーム数から、`WavReader` の残量・seek位置、`GaplessRun` の曲長合計、リングの全ブロック数・公開/消費カウンター、`TrackPlayer` の保持位置、制御層のsnapshotまで64 bitを保つ。1回の読み取り要求だけを、再生では出力容量と8 KiBバッチ、24/32の事前検証では64 KiBへ制限する。各ファイルのデータ範囲はWindowsの非負signed 64 bitファイル長以内。同一レート区間へ曲を追加するときは、64 bitの合計が桁あふれしないことを加算前に確認する。1ブロックの `PoppedBlock::valid_frames` は32 bitだが、16 MiBのリング予算で制限されるブロック幅だけを格納する。GUIはsnapshotの64 bitフレーム数を当該曲のレートで整数除算し、秒単位の時間を表示する。フレーム数を32 bitへ縮小せず、長いRF64でも位置と総時間を保つ。

RF64が変えるのは媒体層の長さ・位置の解釈だけで、変換カーネル、8 KiB先読み、リング、コールバック、Play/Pause/Stop/Closeの製品経路は共通である。媒体層だけの変更は、4 GiB前後の疎ファイル読み込み・seek、RIFFとRF64の曲境界、GUIまでの全バイト・操作の自動試験で判定する。ドライバーへ渡すサンプルや呼び出し順を変える修正が入った場合は、実機検証の要否を改めて判断する。

## 現在のコード配置

| 配置 | 責務 |
|---|---|
| `media/` | `ReadOnlyFile` がハンドルを所有する。`wav_header.cpp` で RIFF/RF64 の通常/Extensible PCM16・PCM24・PCM32・Float32を検証してデータ範囲（`WavInfo`）を返す。各読み取りは検証済みの境界内の絶対位置を指定し、ハンドルのファイルポインターに依存せず、呼び出し側が位置を持つ。`WavReader` がデータ開始位置と残りフレーム数から読みながら `audio` のカーネルで左右の出力へ変換する |
| `stream/` | 読み込みスレッド上の `GaplessRun` が同一レートの WAV をつなぐ。`PrefetchRing` は固定容量の SPSC リングを所有し、左右がそろったスロットを公開する |
| `asio/` | `driver_session.cpp` は x64 登録から指定した CLSID を開き（`driver_registry.cpp`）、制御スレッドの COM STA（OLE1 DDEなし）と初期化済みドライバーを所有する。異レート区間・再初期化の再openはどちらも元レートの復元→解放→初期化の同じ手順を通る。`driver_queries.cpp` は機器の能力照会と出力バッファーの検証、`sample_types.cpp` は ASIO の型の記述を担当する |
| `playback/` | `request.cpp` は再生要求全体と各区間のレート設定、`segment.cpp` は同一レート1区間のバッファー寿命、`segment_renderer.cpp` はそのコールバックを担当する。`output_format.cpp` は ASIO の型から WAV の出力形式を選ぶ |

`playback::play_tracks` は任意長の `TrackInput` 列を受け取り、従来の先頭曲・次曲の入口も同じ `run_request` へまとめる。要求側は現在の出力と曲列を `PrefetchRing` へ渡し、既存の読み込みスレッドが同一レートの連続範囲を開いて全件検証し、初期充填する。制御STAはその間も既存のメッセージ待機を使い、検証・充填済みの1区間だけを開始する。この待機は再生通知のイベントでも起き、そのたびに他の準備の境界と同じ制御の確認を行う（`request.cpp` の `PrimingPoll`。リングの `core::WakeHook` 経由）。確認が触れるのはコントローラーの状態と `TrackPlayer` の進捗だけで、読み込みスレッドが使う `SourceCache`・区間の曲列・次曲・拒否の記録には触れない。読み込みスレッドは次の曲を開く前に、要求のStop、または通知による段階の変化（resetや故障）を見て区間を打ち切る。打ち切った区間は次曲も拒否も持たずに初期充填し、区間の最初の確認が `createBuffers` の前に結果を返す。Pauseはこの判定に使わない。`GaplessRun` は曲末がブロック途中に来るたびに次曲をその続きへ直接読み、1ブロック内に3曲以上の境界があっても順序を保つ。異レートの次曲は検証済みハンドルのまま要求側に保持し、前の区間を排出・解放してから次の区間を作る。コールバックは曲数を知らず、`pop` で完成済みブロックを受け取り、区間の最終ブロックで出力遅延のカウントダウンを始める。

単曲とプレイリストの一時停止・再開と Stop は `playback::TrackPlayer` が担当する。プレイリストではパス列、現在曲の番号と保護ハンドル、曲内フレーム位置を持ち、アイドル時の `select(index)` は選択曲の位置を0にする。`play` は制御スレッドで開始し、一時停止・Stop・終端・故障まで待つ。別スレッドまたは制御側のメッセージ処理から `request_pause` を送れる。コールバックは次のブロックを取り出す前に要求を受理し、PCM の追加供給を止める。それまでに提出した PCM を通常の終端と同じ遅延計算と2ブロック分の実時間待ちで排出してから、制御側が stop・dispose を行う。`paused` が戻った時点で停止完了となるため、操作から停止まで排出待ちがある。開始時の無音中で PCM をまだ提出していなければ位置は0のまま。区間の最終ブロックを既に提出していた場合は一時停止より区間終端を優先する。

再生中の `request_stop` は残りの PCM を打ち切る要求で、別スレッドから送れる。atomic の要求フラグと既存の通知イベントで制御側を起こすため、追加のコールバックがなくても停止処理へ進む。同じ要求の重複ではイベントを再通知しない。受理後は一時停止・正常終端の排出待ちを省き、stop・dispose・読み込みスレッドの終了を完了してから `stopped` と位置0を返す。同期的なドライバー呼び出しや実行中のファイル I/O 自体は中断しない。故障は Stop より、Stop は一時停止・正常終端より優先する。位置の確定までに受けた Stop は stop・dispose・生産者終了の途中でも反映する。再生要求の排他（`RequestClaim`）は各公開入口で取得し、最後のStop・故障の確認と位置の確定が済むまで保持する。途中で別の再生が通知の世代を置き換えることを防ぐ。

`play` が戻った後は、制御スレッドの `stop()` で一時停止位置や曲末位置を0に戻せる。再生中にこの同期操作を呼ぶと例外にし、`request_stop` を使う。非同期要求は再生中だけの操作で、アイドル中に送った要求は次の `play` に持ち越さない。Stop 後は同じオブジェクトの `play` で先頭から新しいリングと ASIO バッファーを作る。選択した WAV と初期化済みの `asio::DriverSession` は保持し、再生用のリング・生産者・ASIO バッファーは保持しない。Stop は故障や再初期化要求を解除しない。reset・buffer-size-changeは `reinitialize_required`、overload・クロック喪失等はそれぞれの故障結果として次の Play を止める。同期APIは通知の結果を返して終了し、通知後の再初期化は下記のコントローラーが担う。

一時停止の保持位置には提出済みの有効 PCM フレーム数だけを使い、開始時の無音と末尾のゼロ埋めを含めない。位置取得が全0や未対応でも同じ経路を使い、`getSamplePosition` や先読み位置から停止位置を推測しない。プレイリストでは提出数を区間単位に保ち、検証済みの各曲フレーム数から現在曲と曲内位置を求める。異レートをまたぐリスト全体のフレーム総和は持たない。同じファイルの重複は同一表記のパスをキーとして保護ハンドルを共有し、生産者が順番にカーソルを先頭へ戻す。提出済みの過去項目の参照は制御側で落とし、現在曲のPause/再Playと将来の重複に必要な参照を残す。単曲の再開は `ReOpenFile`（`ReadOnlyFile::reopen`）で同じ保護ファイルの別ハンドルと独立したフレーム位置を取得する。保持位置をそのフレーム位置へ設定し（I/Oなし）、新しいリングを充填し、開始時の無音も改めて付けて `start` する。読み込み失敗やドライバー故障は一時停止より優先し、stop・dispose・生産者の終了まで成功したときだけ位置を確定する。故障した要求の再開は拒否し、新しい要求を必要とする。

`TrackPlayer` は検証済みで未読の `media::WavReader` も受け取り、試聴側からハンドルを移譲できる。`play` は要求ごとの `SegmentControl`（Pause/Stopの要求、提出フレーム数、観測者）を作って `run_request` に渡す。Pause/Stopを持たない `play_tracks` も決して立たない要求で同じ経路を通るため、コールバックに「制御の有無」の分岐はない。独立カーソルは保護中のファイルの検証済みメタデータを引き継ぎ、再開のたびに RIFF/RF64 のチャンクを読み直さない。一時停止中の通知確認と新しい再生世代の公開は compare-exchange で結び、ファイルを開き直す間に届いた故障を上書きしない。この競合は `PlaybackSignal::try_begin` が例外ではなく故障の結果として返し、要求はドライバーに触れずにその結果で終える。期待レートは現在使われていないスロットへ書いてから世代を公開し、旧世代を観測中の通知が読む値を保つ。

区間ごとに左右の ASIO 型を照合し（`playback::negotiate_output_format`）、対応する `audio::SampleFormat` をリングに渡す。読み込み側で変換を済ませ、コールバックは1チャンネル分のブロックをそのままコピーする。幅544フレームの24 bit 出力ならコピーと無音化は各1,632バイトで、排出に使う時間はフレームで数える。`createBuffers` 後に左右の型がリングの形式と一致しなければ、初回の書き込み前にバッファーを解放する。

PCM16/24/32入力はそれぞれ2/3/4バイト格納のモノ・ステレオを受け付け、読み込み側だけで左右に分離・複製する。ASIO整数出力は16/24/32 bitと32 bit容器内の有効16/18/20/24 bit、それぞれ両バイト順の計14型。PCM16は14型、PCM24（24/32を含む）は6型、PCM32は全32 bitの2型へ無損失で変換する。32 bit容器内の有効Nbitは低位配置で上位を符号拡張し、Extensible入力の上位配置と区別する。精度不足は値が偶然収まる音源でも拒否する。ASIO型の記述は `to_sample_format`、対応・精度判定は `find_planar_converter` に一元化し、再生層の重複した許可リストを廃止した。Float32/64出力も両バイト順に対応済み。PCM16/24はFloat32/64、PCM32はFloat64へ正確に正規化する。Float32入力はFloat32/64だけを許し、PCM32→Float32とFloat32→整数は無音でも拒否する。GUIと制御層は入力・出力形式で分岐せず、同じPlay/Pause/Stop/Closeの経路を使う。

Extensibleのfmtは40バイト以上、cbSizeは22以上かつ宣言された拡張がfmt内に収まること、PCMまたはIEEE_FLOATのSubFormat GUID全16バイトを検証する。未知の追加fmt領域はチャンク境界を確認してスキップする。それぞれ通常PCM・Float32と同じ形式記述へ正規化し、24/32だけは `int24_in32_le`（4バイト格納・24有効ビット・上位配置）で区別する。現在の左右出力契約を保つため、マスクはモノ0/0x4・ステレオ0/0x3だけを許す。これは本製品の対応範囲であり、他のマスクをWAVE仕様全体で不正とするものではない。[MicrosoftのExtensible定義](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/ksmedia/ns-ksmedia-waveformatextensible)・[チャンネル配置](https://learn.microsoft.com/en-us/windows/win32/xaudio2/xaudio2-default-channel-mapping)。

`WavReader` の通常構築はヘッダー検証後に24/32の全データを走査する。走査ごとに1つ確保するヒープ領域（最大 `WavReader::validation_bytes` ＝64 KiB、スタックには置かない）へ1回最大64 KiBずつ読み、4サンプルずつ整数SIMDでORしてSSE4.1のPTESTで各レーンの最下位バイトを検査する。完全な16バイトだけをロードし、末尾0～3サンプルはスカラーで調べる。走査は自前の位置で読み、終了後にファイル位置を戻す処理はない。書き込み・削除を共有しないハンドルでデータを保護し、`reopen` は検証済みメタデータと検査結果を引き継ぐ。プレイリストでは同一レート区間の後続曲を開始前にすべて検証するので、曲境界で全件検査を始めることはない。不正・無損失変換不能の次曲があればそこで区間を切り、手前の有効な曲を最後まで再生してから拒否項目を選択する。変換カーネルの未使用ビット0は媒体層で保証する事前条件で、コールバックへ検査を追加しない。

先読み量、リングの16 MiB 上限、生産者への通知間隔は `stream::PrefetchPolicy` と `plan_ring_layout` で決まる。読み取りバッチは `GaplessRun` が各曲の入力幅から準備時に計算する。`PrefetchRing` はその上限を空き容量・折返し・公開済みの残量で制限する。密に並ぶスロットは1回の変換カーネルでまとめて書き、整列用の隙間がある幅では最初のスロットだけ除算で求め、その後は一定の間隔で進めながら各スロットへ直接変換する。変換後の並べ直しは行わない。`GaplessRun::next_batch_blocks` は現在の曲の計算済み上限を返し、曲末付近だけ残りフレーム数から切り上げたブロック数へ縮める。境界を含む1ブロックは次曲以降で埋め、次の補充から新しい入力幅を使う。大きな単一ブロックは複数回の読み取りになるが、再生データの1回の `ReadFile` は常に8 KiB以下である。64 KiBで読むのは、区間の再生と重ならない24/32の事前検証だけである。公開カウンターが最初に進むのは初期充填を終えた時点で、全区間が収まれば読み込みスレッドも再生開始前に終了する。収まらない区間では最後のブロックを公開した後も、読み込みスレッドは最後の曲のハンドルを持ったまま待機し、`stop` で終了する。区間の再生中に読み込み側はスレッドを作らず、自らページも解放しない（リングのページは初期充填の前に確保して停止後に解放し、読み取りの8 KiBステージング領域は `produce` のスタックに1個置き、`fill_slots`・`GaplessRun`・`WavReader` へ固定長spanで貸す。受け手はspanを保存せず、同期読み取り中だけ使用する）。読み取りまたはこの待機の失敗のとき以外は、スレッドの終了（スタック・TEBの解放、DLLへのスレッド終了通知、最後のファイルのクローズ）を停止後へ回す。再生中のカーネル・ローダーの処理を減らすためで、音への効果は測定していない。曲末に達した前の曲のハンドルは、従来どおり次の曲へ進んだ時点で閉じる。生産者が最後の参照を持つ曲では、このとき小さなヒープ領域も1つ解放する（ヒープがそこでページを返すかは測定していない）。`stop` はスレッドの終了済み・未終了によらず、それ以後のPCM供給を止める。試聴コマンドは2曲の形式を検証したファイルハンドルを再生要求（`play_tracks` から `run_request`）へ移譲する。リングを作るのは要求が準備した曲列だけで、2つのハンドルを直接リングへ渡す入口はない。初期充填と初期充填後の補充は [試験仕様](test-specification.md) に従い、模擬ASIOと実機の確認を区別する。

SDK のヘッダーは `asio/asio_sdk.h` からだけ取り込む。先頭曲の WAV ヘッダーからレートを取得し、機器の現在値と異なるときだけ切り替える。切り替えた場合は実レートを読み戻し、そのレートでの出力型とバッファー範囲を確認して最初のリングを作る。幅はDeviceSettings::buffer_framesが0なら推奨、正なら指定値をそのまま検証して使う。異レートを含む任意長の曲列でもドライバーの占有は1回の再生要求全体に及ぶ。前の区間を排出して停止し、バッファーとリングを解放してからレートを変更する。再生は常に `asio::DriverSession` が所有するドライバーで行い、この曲間で変更済みのレートを元に戻して解放し、同じ CLSID・同じウィンドウで初期化し直す（`DriverLease::reopen`）。異レートへ移るときは初期化状態を引き継がず、レートごとに出力型とバッファー条件を取得し直すホスト側の方針である。新しいインスタンスにも overload の能力照会を行い、実レートが次曲と異なるときだけ設定する。再初期化に失敗した場合は例外で再生要求を終え、セッションにドライバーは残らない。ストリームを開始するたびに、最初の PCM ブロックの前へ既定100 ms の無音を置く。無音はブロック単位に切り上げ、事前充填する B から返す。これは開始直後に無音区間を設けるホスト側の方針であり、全ドライバーで欠けやクリックを防ぐ保証ではない。同一レートの曲は1区間でつなぐので、曲境界には無音が入らない。無音の長さはセッションの生成時に `asio::DeviceSettings` の `start_silence` として0～1,000 ms で指定する。GUIは起動時に読み込んだ機器別 `StartSilence` を渡す（未指定は既定値）。実レート・出力型・選択幅を確認した後、新しいリングへ次曲のハンドルを渡す。

実機診断では `DriverSession` のスコープ内で `asio::query_capabilities` にドライバーを貸す。照会は現在のレート、出力数、出力1/2の個別の型、バッファー幅を値として返す。初期化・照会の途中で例外が生じても、取得したドライバー参照を解放してから STA の初期化を解除する。診断コマンドは同じスレッドとウィンドウで同じ CLSID をもう一度開き、この寿命の境界を検査する。

能力照会中は旧クロックのまま停止している。`canSampleRate` が成功し、旧区間が正常に排出済みであることを確認してから、`setSampleRate` の直前に通知の期待レートを次曲へ切り替える。reset やバッファー幅変更は、曲間で届いても現在の再生要求全体を `reinitialize_required` で中断する。再初期化要求はStop・Pause・EOFより優先し、読み取り・overload・クロック喪失等の故障は再初期化要求より優先する。再生要求内では、クロックの照会前、元レートの確定後、各レート変更の直前で通知を区別する。古いクロックの通知やバッファー切替が遅れて戻っても、新しい区間の先読み位置や停止理由を変えない。

## 品質特性と変更時の確認

変更時には、現行の契約と以下の品質特性を確認する。測定結果はその実行条件での観測として扱い、製品全体の保証へ読み替えない。

[ISO/IEC 25010:2023の製品品質モデル](https://www.iso.org/standard/78176.html)を参照し、この実装では次の3観点を変更と検証に結び付ける。

| 観点 | 実装上の方針 | 確認方法 |
|---|---|---|
| 保守性 | 層の依存方向、層の公開ヘッダー、SDK境界、製品の共有ソース一覧を一元管理する。制御確認、出力準備、終了結果の公開を分け、成功と失敗で診断情報の保持方法をそろえる | `scripts/check-project-layout.ps1` で直接の層間include、`playback`・`control` の公開ヘッダー以外への層外include、SDKの取り込み口、`ui`・`main.cpp`・`playback_controller.h` から推移的にたどれるincludeにSDKがないこと、下位層の試験群が再生エンジン・GUI・再生用fixtureを直接取り込まないこと（`asio` の試験群が使う模擬 ASIO は `playback/segment.h` を取り込む）、ソースとVisual Studio登録、試験ファイルと `suites.h` の表の1対1対応、Solution Items、製品のソースに `std::format`・`std::to_chars`・iostream・ロケールがないこと、README.mdとdocs/*.mdの断片付きリンクが見出しを指すことを検査。`verify.ps1` からローカルとCIで実行する。`verify.ps1` はビルドの前に両プロジェクトの評価した設定を、Releaseのビルドの後に版情報・PEヘッダー・コールバックの機械語を監査する。PEヘッダー・取り込みDLLと逆アセンブルの監査は、違反を1つ加えた実入力の複製を拒否することも毎回確かめる。設定と版情報の監査にこの自己検査はなく、否定確認は木の複製での手動の確認だけである |
| 信頼性 | 不正な状態を型で表せないようにする。変換器は `find_planar_converter` だけが作るため記述の不整合は起こらず、別のWAV形式向けの変換器と不正な寸法はI/O前に拒否する。失敗した入力検査でカーソルや出力を変えない。短曲の停止、開始無音中の故障、生産者終了時の故障も同じ停止契約に従う | 故障注入、境界値、Pause/Stop/Close、解放後の診断を模擬ASIOと実ファイルで照合。Debug/Release・静的解析・ASanで検証する |
| 性能効率性 | 変換は連続領域をまとめ、整列スロットの余分な除算を避ける。リングは予算内に収め、固定ページの作業セット増分を実ページ単位で返す。リアルタイム境界は維持する | 先読みの時間と読み取り回数を同一PCで比較し、ページ固定・解放・移動を実Win32 APIで検査。コールバックから到達するコードもレビューする |

`core::SystemSleepGuard` は独立した `PowerRequestSystemRequired` を所有する。区間の開始直前から排出・停止・バッファー解放までWindowsへ自動スリープ抑止を要求し、成功・例外のどちらでも解除する。画面の点灯は要求せず、呼び出し元やドライバーのスレッド電源設定も変更しない。要求の実効性にはWindowsの電源方針が適用され、ユーザーによる明示的なスリープは妨げない（[Windows APIの契約](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-powersetrequest)）。`PowerCreateRequest`・`PowerSetRequest` が失敗した場合は開始前に区間を中止し、バッファーを解放してWin32エラーの例外を返す。その要求の結果は `driver_error` に分類する。ドライバーの故障ではないが、[要件](requirements.md#再生経路と検証)が再生中の自動スリープ抑止を求めるため、抑止できない再生は始めず、開始前の他の失敗と同じ終了経路で扱う。

`PageAllocation` は要求サイズとページ単位に切り上げた実確保サイズを分けて保持し、固定・解除・作業セット調整には実確保サイズを使う。移動では所有権と増分を一緒に渡し、移動元は空として扱う。リング寸法は演算前に検査し、極端に大きな目標時間も桁あふれさせず予算上限へ収める。

## テストの構成

`tests/wavplay_tests/` は製品と同じソース一覧をビルドし、ドライバー固有の呼び出しだけをプロジェクト所有の模擬 ASIO に置き換える。再生も製品と同じく `asio::DriverSession` を通し、そのアクティベーションの関数で模擬 ASIO を渡す。判定の範囲は [試験計画](test-specification.md) に記す。

`scripts/verify.ps1` はビルド・解析・試験の順序と外部ツールの終了判定を担当する。具体的な監査は `scripts/verification/` に分け、`build-settings.ps1` が評価済みMSBuild設定、`release-image.ps1` がPE・manifest・version、`realtime-code.ps1` がcallback到達範囲とCRT呼出し、`startup-code.ps1` がCPU確認前のコードを検査する。各ファイルは関数定義だけを持ち、入口から明示的に読み込む。監査対象へ違反を挿入して拒否を確認する自己検査も、それぞれの監査と同じ場所に保持する。

| 配置 | 役割 |
|---|---|
| `framework/` | 外部依存のない判定と実行の枠組み。`TestSuite`・`expect`・`expect_true`・`expect_throw`（判定の失敗は製品の例外と区別する `Failure` で送出し、`expect_throw` はこれを捕まえない）、ケースを再利用されない専用スレッドで動かす `TestThread`、監視の上限を Debug・ASan で拡大する `hang_budget` と停止したスレッドを結合せずに終了する `abandon_hung`、CRT の報告・`abort`・`std::terminate`・未処理の例外をダイアログなしに現在の群とケースへ帰属させる `failure_reporting`、各群を子プロセスで分離し並行して判定する `supervisor`。1件でも失敗すれば非ゼロで終了する |
| `fixtures/` | WAV の組み立てと一時ファイル（`wav_builder.h`。並行して動く試験群のプロセスが名前を取り合わないよう、プロセスごとの `%TEMP%\wavplay_tests.<PID>` に置く）、製品の変換を使わない独立の期待サンプル（`sample_oracle.h`）、読み取り器を直接呼ぶ補助（`wav_reader_fixture.h`）、実行イメージの各ページの常駐・固定の照会（`image_residency.h`）、スレッドの実行速度・優先度・メモリー優先度、プロセスの優先度クラス・電源調整、スレッドの名前の照会と判定の補助（`thread_state.h`）、共有デスクトップのフォーカスと前面化を使う試験群を名前付きミューテックスで直列にする `UiDesktopGuard`（`ui_desktop_guard.h`）、製品と同じ準備済みの曲列でリングを作る補助（`make_ring`）とリングのブロック待ち・ファイルの排他（`ring_fixture.h`）、模擬 ASIO での再生と照合（`playback_fixture.h`）、単曲の再生を模擬 DAC で照合する補助（`track_player_fixture.h`）、製品のスレッドでドライバー呼び出しを止めておく `CallGate` と、batch oplockでファイルのopenを止めておく `OpenGate`（`call_gate.h`） |
| `doubles/` | 模擬 ASIO（`mock_asio.h`）、出力遅延を持ち stop でキュー内の未再生分を捨てる模擬 DAC（`dac_queue_asio.h`）、登録・生成・初期化と所有セッションの寿命を模擬する機器（`registered_driver.h`）。呼び出しの記録と、入口で契約を検査する `check_entry` を持つ。`activation()` は機器自身に束縛した `asio::ActivateDriver` を返し、`MockSession`・コントローラー・GUI はそれを受け取って自分のスレッドで開く。プロセス全体の差し込み口は持たない |
| `suites/<層>/` | 製品の層ごとの試験。`suites/suites.h` の表が実行順と `--suite` の名前を決める。期待バイト列は製品の変換関数で作らず、独立に書く。変換カーネル・リング寸法・ASIO 型の対応・要求の通知の符号化のような純粋な部分は、ファイルや模擬 ASIO なしで直接試す。`core`〜`asio` の試験群は再生エンジン・コントローラー・GUI と `playback_fixture.h` を直接取り込まず、`asio` より下は模擬 ASIO も使わない。`asio` の試験群が使う模擬 ASIO は、ドライバーのクロックをコールバックの経路（`playback::is_streaming`・`notifications`）に合わせるため `playback/segment.h` を取り込む |
| `diagnostics/` | 実機用のコマンド。能力の照会（`capability_probe`：`--probe-asio`）、クロック変更前後の照会（`lifecycle_probe`：`--probe-asio-rate-lifecycle`）、試聴（`audition`・`controller_audition`：`--audition-asio*`）、先読みの測定（`prefetch_measurement`：`--measure-prefetch`）。模擬 ASIO でも同じ経路を `suites/diagnostics/` から試す |

試験群の分担は次の通り。WAV のコンテナー適合は `wav_header`、読み取りカーソル・形式変換・未使用ビットは `wav_reader`、ASIO の出力ペアの交渉は `output_format`、要求の世代・段階・結果の符号化と待機は `playback_signal`、STA のメッセージ待ちは `message_wait` で試す。模擬 ASIO では、1区間の出力内容と停止を `suites/playback/segment_tests.cpp` で、区間をまたぐ機器の呼び出し順・拒否・割り込みを `suites/playback/rate_transition_tests.cpp` で検証する。単曲の再生は模擬 DAC から実際に出たサンプルを元 WAV と照合し、3群に分ける。`track_player` は PCM16 を基準に操作の意味（一時停止・再開の境界、Stop と先頭からの再Play、Stop と Pause の優先、故障と後始末の失敗、アイドル中の故障、検証済み WAV の所有、start 内の要求と EOF の排出）を、`track_source_formats` は PCM24・Extensible PCM24/32・PCM32 の音源を保持できる LSB 整数出力へ同じ境界で通し、`track_output_types` は整数の出力型ごとの一時停止・Stop/再Play・短曲 EOF と、Float32 と整数の音源から各 Float 型への DAC 時系列を全バイトで照合する。形式と出力型の組み合わせはこの層で網羅し、GUI の試験群は代表形式で配線を確かめる。

プレイリストのUTF-8・行区切り・ローカル絶対パス・Unicode/長パス・除外件数・ファイルエラー・停止の旗による打切りは `playlist` で、起動時の一覧の検証中のCloseは `playback_controller` で試す。任意曲数の同一レート接続・異レート反復・拒否位置・重複ハンドル・曲内Pause/Stopは再生層、一覧の不変共有・選択と現在曲・受付/完了番号・拒否後の再選択・連続した選択の合流・snapshotの再利用は `playback_controller`、一覧表示・ダブルクリック/Enter/Space/Escape・空一覧・拒否項目の選択は実ウィンドウの `player_window` で確認する。

引数なしの実行は各群を別プロセスに分離し、製品のプロセス全体の状態（ASIO コールバックの通知や要求の占有）や停止・異常終了を群の間で共有しない。分離しているので群は並行して動かせ（既定で最大4群、`--jobs N`）、時間のかかる群（`long_running`）から始める。`wavplay_tests.exe --list` で試験群の名前を表示し、`--suite 名前` で一部だけを1つのプロセスで実行できる。`--in-process` は全群を1つのプロセスで実行する。プロセスの終了まで続く実行イメージの固定は、`memory` と `segment` の群が試験の実行ファイルの子プロセス（`--image-lock-child`・`--locked-segment-child`）でだけ試し、試験群のプロセス自身は固定しないので、`--in-process` でも他の群へ影響しない。実機の診断コマンド（`--probe-asio*`・`--audition-asio*`・`--measure-prefetch`）は製品と同じプロセス全体の状態で動くよう、コマンドの前に `core::lock_process_image` を1回呼んで試験の実行ファイルの実際のイメージ範囲を固定する（試験群はこの経路を通らない）。

## 1. ファイルから左右の出力バイト列まで

1. 制御STAが `playlist::load` でUTF-8全体を検証してから項目を公開する。不正なUTF-8、不在・読み取り失敗は一覧全体のエラーとなり、部分リストは返さない。BOMは先頭だけ任意。CR/LFだけを行区切りとして除き、空行と先頭が `#` の行を無視する。他の空白・順序・重複は保持する。ローカルの絶対パス以外、欠落・解析不能・空データのWAVは項目単位に除外し、件数を返す。同一表記の重複は検証結果を共有するが、行と除外件数は重複分も数える。起動時のハンドルは読み込み後に解放し、一覧はパスだけのスナップショットとする。再生に使うファイルは改めて開いたハンドルで検証・保護する。停止の旗（`std::atomic<bool>`）を受け取る `parse`・`load` は、各項目を検証する前に旗を読み（空行・`#` の行では読まない）、立っていれば一覧を返さない（`nullopt`。途中までの一覧も例外も返さない）。検証中の1項目は最後まで行うため、大きなExtensible PCM 24/32の1項目は全件の走査を終えてから止まる。最後の項目の検証中に立った旗は、後に読む項目がないので完全な一覧を返す（その行の改行の有無・CR/LFの別に依らない）。`std::stop_source::request_stop` はこの静的CRTのビルドでSTLの `WakeByAddressAll`・`WaitOnAddress`（`api-ms-win-core-synch-l1-2-0.dll`）を取り込み、`verify.ps1` の取り込みDLLの許可リストから外れるため、`std::stop_token` は使わない。
2. チャンクのヘッダー、本体、奇数長パディングがコンテナーと実ファイルの両方に収まることを、加算・乗算の前に確認する。RF64で32 bitの長さが `0xFFFFFFFF` なら `ds64` から解決する。未知チャンクは読まず、次のチャンクの位置を計算して進む。完全なフレーム数、必須チャンク、形式の検証が済んでからデータ位置を公開する。
3. Extensible PCM 24/32 の未使用ビットはヘッダーだけでは検証できない。同一レートで連続再生する区間にこの形式があれば、区間の開始前に対象のデータを固定長バッファーで走査する。曲境界に全件検査が持ち越されると先読みが尽きるためである。検証したファイルは書き込み・削除を共有しないハンドルから再生する。同一表記のパスの重複項目はハンドルを共有し、現在曲と将来の使用に必要な参照がなくなれば閉じる（`request.cpp` の `SourceCache`。要求の先頭曲だけを制御スレッドが開き、以降の曲（次のレート区間の先頭曲を含む）は各リングの初期充填を待つ間に読み込みスレッドが開くため、同時には使わない）。大きな24/32音源を並べると再生開始までの待ち時間が増えるが、曲全体の音声をメモリへ置かない。この待ち時間のStop・Closeは後続曲の単位で効き、検証中の1曲と打ち切った区間の残りの初期充填（リングの予算内）だけを待つ。制御STAが開いて検証する先頭曲は中断できない。
4. 選択ペアの左右が同じ対応型であることを確認し、WAV の有効ビット数で無損失性を判定する。読み込みスレッドでフレームを分離・変換し、モノは左右に複製する。整数は符号・スケール・配置・バイト順を、Float32 は符号付きゼロ、subnormal、NaN ペイロードを含む入力表現を保つ。後続曲の検証で再生不能と分かった場合は、そこを区間の終端とし、手前の有効な曲を再生する。

絶対パスの入口は通常の `C:\...`、拡張長の `\\?\C:\...`、ローカルvolume GUID形式に限る。UNC・device名前空間・相対パスを先に除外し、ドライブ種別とmount/junction先のvolumeを確認する。ローカルとみなすドライブ種別（固定・リムーバブル・光学・RAMディスク）は `media::is_local_drive_type` の1か所に置き、この事前確認と下記の開いた実体の照合が同じ判定を使う。入力の根を読む2つの解析（利用者の表記と、`GetFinalPathNameByHandleW` の正規の表記）は文法が違うので共有しない。`ReadOnlyFile` は通常DOS絶対パスを拡張長形式へ変換し、区切りと明示的な `.` / `..` だけを正規化する。ファイル名の末尾空白や点を削らず、表示用のパスは一切変更しない。さらに `CreateFileW` で開いた実体を `GetFinalPathNameByHandleW` のDOS形式（解決できなければvolume GUID形式）と `GetDriveTypeW` で照合する。`FILE_NAME_OPENED` を使い、UNCの実体とremote driveを拒否するため、ローカルのsymlinkがネットワークへ向く場合も対象となる。ネットワーク共有にはvolume GUIDがなく、Mount Managerを使わない一部の非標準ファイルシステムはDOS/GUID形式を返さないという[Windows APIの契約](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-getfinalpathnamebyhandlew)に従い、実体を確認できないファイルは読み取りエラーとする。再生時のopenでも同じ判定を行う。保護中のハンドルの `reopen` は同じ実体なので再解決しない。リムーバブル・光学ドライブも受け付けるため、媒体のないカードリーダーや光学ドライブの「ディスクがありません」等のシステムのダイアログが制御STAや読み込みスレッドを止めないよう、volume照会とファイルアクセスの間だけ呼び出しスレッドの重大エラーを失敗にする（`core::CriticalErrorScope`。`ERROR_NOT_READY` 等は既存の除外・拒否になる）。プロセス全体の `SetErrorMode` はプロセス内のドライバーにも及ぶので使わず、ドライバー呼び出しを範囲に含めない。`SetThreadErrorMode` はスレッド環境ブロックだけを書き換え、システムコールを伴わない（Windows 11 build 26200のntdll `RtlSetThreadErrorMode` の逆アセンブルで確認）。

ここまではドライバー登録もGUIも不要で、実ファイルを使う `playlist`・`wav_reader` 試験群で検証する。

## 2. ドライバーを開き、一曲を開始する

Win32 の制御スレッドが COM の STA とメッセージポンプを持ち、選択中の `IASIO` を所有する。UI は操作要求を送り、制御側の状態を表示する。読み込みスレッドはファイルとリングの書き込み側を、ドライバーからのコールバックは読み出し側を担当する。

単曲とプレイリストの非同期入口は `control::PlaybackController` とする。構築時に専用スレッドを起動し、そのスレッド上で設定、一覧、WAV（`playback::TrackPlayer`）、非表示の所有ウィンドウ、`asio::DriverSession` の順に用意する（`load_settings`・`load_list`・`open_device`）。公開は設定→一覧→NO DEVICE→機器の順とする。Closeは各段階の始めと `--wav` の検証の後で確かめ、受付後は次の段階へ進まず（実行中の段階は最後まで行う。ただしプレイリストの検証は項目の単位で打ち切る）、例外を使わずに片付けへ移る。`submit` がCloseを受け付けるとき、`close_requested` と同じロックの中で停止の旗も立てる（読み手は検証中の `playlist::load` だけで、ロックを取らずにrelaxedで読む）。Closeはどの経路でも `submit` を通るので、検証中に受け付けたCloseは必ず届く（検証中は所有ウィンドウもメッセージ処理もないため、実際の経路は `close` とデストラクター）。打ち切った読み込みは一覧を返さず、`load_list` は何も公開しない（検証済みの先頭部分も公開しない）。最後の項目の検証中に受け付けたCloseでは読み込みが完了して一覧を公開し、`open_device` の始めの確認で片付けへ移る。`std::thread` の所有とデストラクターでの明示的なjoinは変えない。機器の段階（`open_device`）より前に受け付けたCloseでは、NO DEVICEの報告も `--wav` の検証もしない。所有ウィンドウはドライバーを開く直前に作る。設定の読み込みと一覧・`--wav` の検証の間はメッセージを処理しないため、その間にトップレベルのウィンドウがあるとブロードキャストの送信元を待たせる。設定エラーとNO DEVICEではウィンドウを作らない。`control` は `playback` の公開入口だけを使い、要求シグナルのトークンやイベントを直接扱わない。呼び出し元の `submit` と `select_track(index, play)` は固定32件のキュー（`CommandQueue`）を短時間だけロックして要求番号を返し、初期化・再生・ドライバー呼び出しの完了を待たない。起床のイベントはロックを外してから通知する（どちらも自動リセットで、制御STAは起床ごとにロック内でキューを読み直す）。キュー満杯・再初期化中・故障後・終了受付後の通常要求、範囲外選択、空一覧へのPlayは0で拒否する。空の正常な一覧は停止状態で表示し、機器や一覧の読み取り失敗と区別する。追加の音声処理スレッドや外部依存は作らず、既存の `playback::TrackPlayer` をこのSTAから呼ぶ。1プロセスに1コントローラーとし、同時に直接の再生APIを使わない。

要求と完了は `PlaybackSnapshot` の `accepted` と `settled` で区別する。`settled` 以下の要求は結果が確定済みで、成功だけでなく故障・終了による取消しも含む。Play はASIOのstart成功時、一時停止とStopは停止・バッファー解放・生産者終了後に確定する。状態と結果を合わせて判定し、Playの受付だけで再生中と表示しない。`revision` を使った待機で最新状態を取得できるが、全状態遷移の履歴は保存しない。待機スレッドを持たない GUI は、構築時に渡す `ChangeListener` で新しい版の公開を知り、通知が公開するrevisionをacquireで読み、描画済みと異なる場合だけ自分のスレッドで `snapshot()` を読む。revisionはmutex取得を省くヒントであり、正本は同期されたsnapshotとする。`snapshot(into)` は同じ内容を呼び出し側のオブジェクトへロック中にコピー代入する。文字列と配列の領域を再利用するため、定常の更新ではロック中にメモリーを確保しない（値で返す `snapshot()` は毎回、診断の2配列を確保し、8文字以上の機器名でもう1つ確保する）。GUIの描画（`refresh()`）は保持した1つのsnapshotへ `refresh_snapshot` で読み、操作の受付とCloseは値で返す `snapshot()` を使う。進捗だけの公開（`publish_progress` による現在曲とそのレート・長さ、整数秒の更新。選択は現在曲に従う）は、`ChangeListener` の任意の `progressed` があればそちらへ、なければ `changed` へ伝える。どの公開もrevisionを進めて `wait_for_change` の待機者を起こし、通知は1回の公開につきどちらか一方だけとする。通知は状態のロック中に呼ばれるため、その中でコントローラーを呼ばない。停止位置は排出・解放後に確定したフレーム数とする。再生中の現在曲への追従は、制御側のpollで観測した提出済みPCM数から求め、先読みカーソルや機器の位置推測を使わない。選択だけを先に反映しても、準備中のPlayや排出中のPause/Stopより先にその完了番号を進めない。

`refresh_snapshot(into, rendered_revision)` はUIがこのコントローラーから取得し、正常に描画し終えた未改変のsnapshotだけを再利用する入口である。描画済みrevisionを0で渡すと必ず完全コピーする。同じmutex内で前回のrevision、非進捗のstate_revision、Playing・操作完了、現在曲・選択・レート・長さ、Closeと故障を確認し、同一曲の通常進捗ならrevisionとpositionの2欄だけを更新してtrueを返す。それ以外は従来どおり完全コピーしfalseを返す。UIは初回、強制再表示、Closeでは0を渡し、コピー・描画の例外や再入で無効化された状態のrevisionを描画済みに進めない。既存の `snapshot()`・`snapshot(into)`・`wait_for_change` の完全コピー契約は維持する。

操作の可否だけを調べる `command_state()` は、同じmutex下で現在の状態・受付/完了番号・位置・一覧の有無・故障の有無を値として返す。`PlaybackSnapshot::command_state()` が唯一の射影で、表示側と操作側は同じ `ui::available` を使う。文字列・診断履歴・共有所有参照を複製せず、古い描画状態を操作判定へ流用しない。これは操作の予約ではなく、実際の受付は従来どおりsubmitが再検査する。Closeの完全snapshotとUI故障後の `closed()` は維持する。

この前提を守るため、区間開始・再生終了の診断コピーと選択拒否の例外生成は、共有状態を書き換える前にローカルへ準備する。成功した値を同じmutex内で例外を出さないmoveにより反映し、状態と一緒に公開する。診断の一部だけが更新された未公開の状態を、軽量更新の前提にしない。

制御の `poll` は、進捗を確認・公開する同じmutex内でClose・故障・要求キューを照会する。配送する要求がなければ、空の `deliver_active` のための二度目のmutex取得を省く。空判定後に届く操作は `submit` が同じmutexで空から非空への変化を確認して既存のidle/activeイベントを通知し、Closeは常に通知する。要求があれば元の配送をそのまま使い、Play barrierや1回32件の上限・再通知、配送中のロック範囲を変更しない。進捗・watchdog・補充の起床周期を変える最適化ではない。

| 要求 | コントローラーの順序と完了 |
|---|---|
| Play | 未選択なら先頭曲。停止後・曲末後は選択曲の先頭、一時停止後は現在曲の保持位置から再開する。停止・一時停止中に別の曲を選んだ場合はその曲の先頭から開始する。再生中の重複は何もしない。先行するPause/Stopの完了を待ってから次のPlayを実行する |
| Pause | 再生中は排出後にpaused（曲末提出済みならcompleted）となる。アイドル中は何もしない |
| Stop | 一時停止の排出待ちを打ち切れる。停止が完了してから位置0を公開し、手動の選択も保持する。準備中のPlayは開始前に取り消せる。後続曲の検証・初期充填中のStop・Closeも曲の単位で効き、検証中の1曲と打ち切った区間の残りの初期充填（リングの予算内）の後に `createBuffers` なしで終わる。制御STAが開いて検証する先頭曲の検証は中断できない |
| Select | 再生中は選択だけを更新し、現在の音を止めない。停止中は選択曲の位置を0にする。停止中に連続したSelect（直後がSelectまたはSelect and Play）は最後の1件だけが曲を開いて検証し、先行する受付番号はその完了・拒否（または故障・Close）でまとめて確定する。直後がPlay・Pause・StopのSelectは省かない。先行Playが準備中、またはPause/Stopが未完了なら選択の完了番号も保留し、後続のStop/Closeの配送は妨げない |
| Select and Play | 進行中の要求をStopして解放完了を待ち、指定曲の先頭から新しい要求を開始する。その後は一覧の残りを順番に再生する |
| Close | 満杯のキューを迂回して受け付け、後続の受付を即座に閉じる。保留要求を取り消し、進行中の再生をStopする。起動時のプレイリストの検証は次の項目の前で打ち切り、一覧を公開しない（最後の項目の検証中なら完全な一覧を公開し、機器の段階へ進まない）。停止・解放・元レート復元・ドライバー解放、WAV解放、所有ウィンドウ破棄、STA終了の順で片付け、closedを公開する |

専用イベントはアイドル待機を、既存の再生通知イベントは再生中の制御待機を起こす。通常操作はキューの空→非空で起こし、Closeは常に通知する。最大32件の処理後にも実行可能な要求が残れば再通知し、次のPlayを待つ境界では不要な起床を作らない。再生中の要求（Playと最後に届けた要求の番号、保留中のPause/Stop、start成功）は `TrackPlayer::play` の間だけ1つの記録（`ActiveRequest`）として持ち、戻った時点の1か所で消す。キューの先頭を待たせるか（Select and Playと、未開始または一時停止・Stopの途中のPlay）は、配送と再通知で同じ判定を使う。制御スレッドだけが準備の境界（初期充填の待機を含む）・start成功・待機復帰でキューを処理する。音声コールバックからキュー、状態ロック、UIを呼ばない。要求ごとのウィンドウメッセージは蓄積せず、一度の処理件数を制限する。アイドル側は最初と最大32要求ごとにWindowsメッセージを処理し、再生中もイベントで起きた際に最大32メッセージを処理する（`core::message_batch_limit`）。操作の連打がドライバーのSTAやWM_CLOSEを妨げず、逆にメッセージが続いても操作処理へ戻る。メッセージ処理はキューのロック外で行う。

一覧は `PlaybackSnapshot::playlist` の `shared_ptr<const vector<wstring>>` で共有し、snapshotごとに全パスを複製しない。`TrackPlayer` も同じ不変一覧を共有して保持するので、最初のPlay・選択でパスを複製せず、要求へ渡すパスの指す先も生存する。制御スレッドは公開した一覧への自分の参照を使い、snapshotをロック外で読まない。`selected_index` はユーザーの選択、`current_index` は現在の再生曲として分ける。曲の進行は制御側で現在曲へ選択を追従させるが、同じ曲を再生中のクリックは選択だけを変える。Stopはその選択を保持し、次のPlayで選択曲から再開する。最終曲の後は状態を `stopped`、結果を `completed` として最終曲内の位置を残し、Stopで0へ戻す。

プレイリストのファイル・形式・レート拒否は `next_track_rejected` / `next_rate_rejected` とし、拒否項目の番号を現在曲・選択曲へ反映して位置0・`stopped` にする。理由は `track_error` に分け、`faulted()` とはしない。拒否に例外があればそれを理由とする。読み込みスレッドが後続曲を開く・検証する際の失敗はその例外を `rejected` と並べて記録し、初期充填またはjoinの後に制御側が読む。バッファー幅の拒否は選択関数と同じ文に曲のレートを加えた `invalid_argument` とする。この2つは `RequestProgress::rejected_error` を経る。開けなかった拒否項目を次のPlayで `TrackPlayer::play` が開き直す失敗は、その例外を直接記録する。3つとも `TrackPlayer::result_cause()` から `track_error` になる。例外のないレート・形式の拒否は従来の固定文を使う。後ろの再生可能曲を自動で飛ばして再生せず、次の明示的な選択・Playを受け付ける。最初に選んだ曲の媒体・レート拒否も同じ扱いにする。制御STAが選択曲を開く・検証する際は `std::runtime_error`（媒体エラーと、Win32 I/O失敗の `system_error`）だけを拒否とし、メモリー不足（`bad_alloc`）や論理エラーはコントローラーの故障とする（`playlist::load`・`TrackPlayer::play` と同じ方針）。読み取り途中のエラー、ドライバーの故障、停止・解放失敗は引き続き致命的であり、媒体の拒否に読み替えない。

停止中の機器が直前の要求に対して報告する reset・buffer-size-change・overload・クロック喪失は `TrackPlayer` が監視する。待機中は `idle_fault_event()` のイベントも待ち、`take_idle_fault()` が通知の結果を返す。アイドル監視より先に次の `play` へ届いた場合も、ドライバーを開始せず通知の結果を返す。reset・buffer-size-changeの `reinitialize_required` は再初期化へ、それ以外の故障は `failed` へ進める。選択の変更や媒体の拒否後も監視を保つ。故障は通常操作と再初期化要求より優先し、未完の要求を取り消してClose以外を拒否する。

通知後の再初期化では、コントローラーが保留操作を取り消して `reinitializing` を公開し、その間はCloseだけを受け付ける。旧ASIOバッファー・リング・生産者の解放後に、旧ドライバーの変更済みレートを元へ戻してReleaseする。復元中の正常なレート通知は許容し、競合する故障を消さない。新しい通知世代で同じCLSID・制御STA・所有ウィンドウ・起動時の機器別設定を使って初期化し、overloadの能力と現在のクロックを再照会する。WAVの保護ハンドルと検証済みメタデータは保持する。成功後は位置0・`stopped` を公開し、新しいユーザーのPlayを待つ。次のPlayでレート・出力形式・バッファー範囲を確認し直すため、再初期化中にASIOバッファーや先読みリングを作らず、INIも読み直さない。

新しいドライバーの初期化・照会中のreset・buffer-size-changeや故障、復元・Release後の再open・照会の失敗では `failed` とし、無限に再試行しない。旧ドライバーの解放中のreset・buffer-size-changeは処理中の要求へ合流させる。Closeはドライバー呼び出しの境界で再初期化を中止し、以後は解放だけを行う。最終解放前に `TrackPlayer::prepare_release()` でクロック復元用の通知段階へ進め、停止結果を保ったまま正常な復元レート通知を許容する。既着・競合する通知をこの切替で消さず、Release中のreset・buffer-size-change・overload・クロック喪失も解放後に再確認する。Close中は再初期化せず結果と例外を残す。元レートへ戻ったこと自体はDriverSessionが読み戻して検証する。

同期的なドライバー呼び出し自体は中断できないため、Closeの受付は即時でも終了完了には待ち時間がある。呼び出し元はclosedを待ってから `join` し、デストラクターもCloseとjoinで全資源の終了を保証する。コントローラー内の再生要求と制御スレッド内のDriverSessionは `optional` で直接保持し、個別のヒープ確保を使わない。最終破棄では、ドライバー解放→WAV解放→ウィンドウ破棄→STA終了の順序を保つ。

- `settings.ini` の `SelectedDevice` だけを使用機器とし、x64 レジストリで登録を確認する。GUI の列挙・選択・変更操作は持たない。SDK の `host/pc/asiolist.cpp` に従い、`CoCreateInstance(clsid, nullptr, CLSCTX_INPROC_SERVER, clsid, ...)` で `IASIO` を取得し、所有ウィンドウのハンドルで `init` する。元のレートを記録してから変更する。
- WAV レートに合わせ、変更後のチャンネル情報、バッファー最小/最大/推奨/粒度を読み直す。保存ペアと手動設定を検証し、選んだ 2 出力のバッファーを作成する。失敗時は取得済み資源を戻し、代替設定での再試行は行わない。
- コールバック表と停止通知の受け口は `createBuffers` 前に準備する。実際のブロック幅と左右のバイト数から、要件のアラインメント、容量上限、先読み目標を満たす固定リングを決める。上限を優先し、ダブルバッファーの供給が不可能な構成は再生開始前に拒否する。
- 各ブロックの完成後にだけ読み出し側へ公開する。短い曲では末尾に達した時点で先読み完了とする。SDK の `common/asio.h` にある開始手順に従い、出力バッファー B（index 1）を先頭ブロックで満たし、コールバックに再生セッションを公開してから `start` する。最初のコールバックが A（index 0）を要求したら次のブロックを渡す。

## 3. 曲間、一時停止、エラーの境界

| 事象 | 制御とデータの扱い |
|---|---|
| 通常のコールバック | 指定されたダブルバッファーに完成済みの左右のブロックをコピーし、消費位置を公開する。追加の先読みや停止が必要になったときだけ通知する |
| 同一レートの互換曲 | 次曲を先読みし、曲末がブロック途中なら残りを次曲の先頭で埋める。短い曲が連続してもフレームの順序を保つ |
| 正常な終端 | 最後の有効フレームの位置を記録する。ブロックの余りだけをゼロで埋め、出力遅延を考慮して末尾が出力されてから停止する。報告遅延を超えてドライバーが溜めた分も鳴らすため、フレームで数えた排出の後も無音を返しながら2ブロック分の時間を待ってから stop する。レートが変わるなら、その後に再構成して次曲を始める。次曲が機器非対応なら選択して停止したままにする |
| アンダーラン・読み込み失敗 | 正常な終端とは別の停止理由を一度通知する。再生位置の進行を打ち切り、制御側で直ちに stop する。停止が効くまでに返す出力バッファーに古い再生音を残さず、先読みが復旧しても自動で曲に戻らない。読み込み失敗は生産者の例外（`ReadFile` の `system_error` 等）を生産者のjoin後に `PrefetchRing::read_error()` から取り出し、`RequestProgress::fault_error`・`TrackPlayer::result_cause()` を経て、結果 `read_error` の例外として既存の故障の表示経路へ渡す。GUIは結果の固定文の後に原因として示す |
| 一時停止・再開 | 要求を受理したブロック境界で PCM の追加供給を止め、提出済みの音を排出してから制御側で stop する。実際に排出した有効フレームの境界を保持し、先読み位置は再開位置に使わない。古いリングを破棄し、同じファイルの保持位置から再充填して start する |
| ユーザーの Stop・先頭からの Play | 追加供給を止め、未再生分の排出を待たずに制御側で stop する。解放が成功したら位置を0に戻し、次の Play で同じ選択曲を先頭から出す。Stop は一時停止・EOFより優先するが、故障を解除しない |
| reset、buffer-size-change | コールバックは `reinitialize_required` を通知し、制御側が停止・再初期化する。保留Playを取り消し、同じ選択曲の位置0で停止待機する。回復後の明示Playまで開始しない |
| 機器・ペア変更（拡張） | 制御側で停止・再初期化し、新しい構成の準備が終わっても停止状態を保つ |

停止・再開の境界でも元のサンプル値を変えず、音声と無音の不連続によるクリックを許容する。一時停止位置は上記のブロック境界と排出で決め、ゼロクロスを探して停止を遅らせたり、フェードを加えたりしない。一時停止から未再生フレームを欠落・重複なく再開する契約と、同一レートの互換曲間の連続性は維持する。Stop では未再生分を意図的に打ち切り、次の Play の先頭から終端までを欠落・重複なく出す。[製品要件](requirements.md#入力と出力の一致)

制御側は stop から戻った後、非同期コールバックからセッションを切り離し（`callback_routing::detach`）、進行中の書き込みが終わってから disposeBuffers を呼ぶ。stop が失敗した場合もこの順序を守り、切り離しの後に両面を無音で埋めてから disposeBuffers を呼ぶ。切り離した後の切替はどのスレッドから届いても何も書かないので、disposeBuffers 自身が同期的に要求したバッファーにも無音が残る。失敗した処理の後始末で disposeBuffers も失敗した場合は、その失敗（`ASIO disposeBuffers failed during cleanup: ASE_名前 (値)`）を `std::throw_with_nested` で投げ、元の例外を型のまま入れ子の原因として残す。外側の型は従来どおり `runtime_error` とする。読み込みスレッドの終了を待ってリングを破棄する。再初期化・終了時は、変更したレートを旧ドライバーの解放前に元の値へ戻すよう試みる。復元を拒否されても参照を解放し、制御側に復元失敗を報告する。再生中だけ保持する電源要求はエラー経路でも解除する。

推奨バッファー幅は終了・再初期化時に自動復元しない。`DriverSession` は元レートだけを復元用に保持し、推奨幅を戻す目的で追加の `createBuffers` や `start` を呼ばない。停止・バッファー解放・ドライバー解放を完了し、終了前後の推奨幅の差だけでは失敗としない。指定幅が解放後の推奨値に残るか、元レートへの復帰等で別の値になるかはドライバーに依存する。[復元範囲とASIO契約](asio-contract.md#推奨バッファー幅の保持と終了時の方針)

コールバック不着の監視は、単調時計で決めた期限を無関係な通知で延長しない。期限に達したとき、その区間にコールバックがあった場合だけ次の監視区間を始める。排出と試聴の再Play待機も、期限に達すれば通知が残っていても待機を終える。通知の有無と再生状態は分けて確認する。

ASIOエラーの文字列と例外連鎖は `asio::throw_if_error` に集約する。通常の失敗には空の入れ子を付けず、別の失敗を処理している途中で後始末も失敗した場合だけ現在の例外を型のまま保持する。部分的な `createBuffers` の後の `disposeBuffers`、`start` の後の `stop`、さらに `disposeBuffers` の全段を失わない。元レートの `setSampleRate` とoverload照会の失敗も、操作名とASIOエラー名・数値で報告する。正常コールバックの経路にはこの処理を追加しない。

## 4. 操作画面、設定、実機の接続

設定の読み込み・保存は `core::CriticalErrorScope` で囲み、リムーバブル媒体の重大エラーも呼び出しスレッド内のI/O失敗として扱う。保存はフラッシュ・置換・例外時の一時ファイル削除まで同じ範囲で行い、抜けると元のスレッドエラーモードへ戻す。

UI故障後は終端の専用描画に移り、`PlaybackController::closed()` のロック下の状態照会とrevision通知だけでCloseと解放完了を扱う。snapshotの文字列複製に再び失敗して再生が残らないよう、Close送信・失敗表示・Closeボタン・セッション終了の完了照会をすべてこの経路へそろえる。通常の表示は従来どおり再利用snapshotを使う。

実行ファイル隣の `settings.ini` のパスは `ui::executable_settings_path` で構成し、`PlayerOptions` からコントローラーへ渡す。制御STAで起動時に一度読み、`General.SelectedDevice`、対応する `Device.{CLSID}` の `Name`・`OutputPair`・`BufferSize`・`StartSilence`、`Window` の矩形と最大化状態を取得する。`SelectedDevice` が不在・空・ゼロGUIDなら未設定機器とし、`NO DEVICE` と設定案内を公開してPlayを拒否する。不正な設定や登録・初期化の失敗も既存の例外表示経路へ渡し、別の機器やペアへ代替しない。GUIは機器名（なければCLSID）と出力ペアを読み取り専用で表示する。

設定は最大1 MiB、UTF-8（BOM任意）、NULなし。ASCIIのsection/key名とCLSIDは大小を区別せず、空白・CRLF/LF・`;`/`#`コメントを許す。機器別数値は符号なし10進整数で、`OutputPair` は正の奇数、`BufferSize` は0以上、`StartSilence` は0～1000。既知キーの重複・範囲外・不正UTF-8・読み取り失敗を拒否する。UTF-8の判定と値の変換は一覧と同じ `core::utf8_to_wide` を使う（全体の判定は起動時と終了時に一度、最大2 MiBのUTF-16へ変換する）。未知キーと他機器は再生設定に使わず、原文として保持する。通常値は出力1/2・推奨幅・100 msで、試験ではパスや設定を注入し、実際の利用者設定を読まない。指定された値だけを `DeviceConfig::apply` で既定値へ重ね、結果を `DeviceSettings::validate` で確かめる。キー名を示す文のため、INIの範囲検査も残す。再生中の監視や再初期化時の再読は行わない。

`OutputPair` の先頭番号から0始まりの隣接出力を求め、レート確定後の `negotiate_output_format` とASIOバッファーの作成・作成後の検証で同じペアを使う。片側でも欠落・精度不足・非対応なら開始前に拒否し、1/2へ代替しない。Pause/Resume、異レートの再open、通知後の再初期化でも起動時のペアを保ち、現在のチャンネル数・出力型を再照会する。

ウィンドウ設定は制御側のスナップショットでUIへ渡す。`X/Y/Width/Height` は通常時の外枠を物理ピクセル・スクリーン座標で保持する。Windowの既知項目がすべて不在なら標準配置。1項目でも指定する場合は矩形の4キーが必要で、部分指定・`Maximized` 単独・重複・不正値は設定エラーとする。`Maximized` は0/1で、このキーのみ省略可能（既定0）。UIは現在のモニターの作業領域と最小寸法に合わせて復元矩形を補正し、初回表示にも `SetWindowPlacement` を使って起動元の初回ShowWindow指定による上書きを避ける。最小化時も通常矩形と最大化への復帰状態を使い、最小化自体を次回へ復元しない。Close時にこの値を制御側へ渡し、ドライバーを解放した後で保存する。UIとコールバックに設定I/Oを追加しない。

終了時の保存では最新原文を読み、`Window` の既知キーだけを更新する。読み取りと同じ走査で値の位置を原文から求め、空の値や `=` のない行の値も原文内の空の位置として扱う（書き換えの前に解析が空・代入なしの既知キーを拒否するので、到達するのは検証済みの値だけで、Debugビルドは値が原文内にあることを表明する）。起動後に手動編集された `SelectedDevice`・機器別設定・他機器・未知キー・コメント・BOMを保持し、この再読を再生設定へ適用しない。同じディレクトリに一時ファイルを書き、フラッシュ後に置換する。起動時の解析が正常に完了した場合だけ保存し、未設定機器でもウィンドウ状態は保存する。保存失敗は既存ファイルを保持して終了処理のエラーとして公開する。GUIは解放後に理由を表示し、再Closeで非ゼロ終了できる。

`asio::select_buffer_frames` は実レートで照会したrangeを検証し、設定0ならその時点のpreferred、正値なら指定値がmin/maxとgranularityに適合することを求める。granularityが−1なら2の冪、正ならminからの刻み、0なら追加の刻み制約なしとする。同じ規則を例外なしで返す `asio::accepts_buffer_frames` は選択関数が幅を返す場合だけ真で、両者はrangeの検査と指定値の判定を共有し、選択関数はrangeを1回だけ検査して、外れた指定値に詳細な拒否文（`asio::buffer_frames_refusal`。指定値・min・preferred・max・granularity）を作る。プレイリストはこの判定で該当曲だけを `next_track_rejected` とし、同じ拒否文に曲の整数レートを加えたもの（例：`requested=8, minimum=16, …, rate=96000`）を `track_error` に表示する。プレイリスト以外の要求は選択関数の詳細な例外で終える。createBuffers拒否時に他の幅へ再試行しない。Pause/Resume、異レート再open、通知後の再初期化を経たPlayでもrangeを再照会する。0は毎回そのpreferredを選び、正値は同じ指定値を再検証する。INIから0を起動時に読むことは、推奨幅を起動時の数値へ固定することを意味しない。ドライバーに残った推奨値は、以後の設定0での再生や同じドライバーの推奨値を使う後続アプリの幅・遅延に影響し得る。

`PlaybackDiagnostics::buffer_capabilities` は各区間の確定レートと検証済みrangeを幅選択の前に記録する。無効な手動幅やcreateBuffers拒否でもその記録を残し、照会失敗・矛盾したrangeでは記録しない。`BufferSetup` はcreateBuffers後の出力/レート/遅延検証に成功してから実幅を記録する。rangeは要求側の能力記録だけが保持し、SegmentRunnerやBufferSetupへの転送・重複保持を行わない。コントローラーは開始成功または要求終了の既存通知で両方を公開する。次のPlayと再初期化で両方を確保済みの領域のまま消し（`PlaybackDiagnostics::reset`）、任意数の異レート区間を可変長の診断列へ分けて記録する。GUIは `current_segment` の能力と確保を表示する。追加のドライバー照会・コールバック処理・表示タイマーは設けない。

GUIは要求値、検証済みの実幅とms、照会したrateとrangeを分けて表示し、停止後も残す実績には最後の確保と明記する。msは実幅と整数へ戻した検証済みレートから64 bit整数で求め、小数第3位で偶数丸めにする（旧 `{:.3f}` 表示と対応8レートで同じ文字列）。正の整数でないレートは表示時に丸めず、ms・Hzを省く。OutputをDeviceとは別のコントロールに置き、長いNameでも選択ペアを読めるようにする。Bufferは標準の読み取り専用複数行Editで、最小寸法や大きい能力値でも折返し・縦スクロールにより全文を確認・コピーできる。縦幅が足りない場合はラベルを各欄の横へ移し、Buffer/File/Errorへ高さを配分して、エラーの理由を画面内に残す。保存位置のない初回表示もモニターの作業領域に収める。

GUIはRIFF/RF64を共通に扱う。操作試験は48 kHzのPCM16/PCM24/PCM32・Float32と対応するExtensibleを対象とし、対応レート全8種は読み込み・再生経路の試験で確認する。`main.cpp` はCRTが起動時に分解したUnicodeの起動引数（`__wargv`。`wmain` の `argv` と同じMicrosoft C++の引用規則）を `parse_player_options` に渡す。`CommandLineToArgvW` とshell32は使わず、`wavplay.exe` はSHELL32.dllを取り込まない。プロセスへ読み込まれないことまでは保証しない（ole32とcomctl32 v6はSHELL32.dllを遅延読み込みで参照し、ASIOドライバーはプロセス内DLL）。引数なしでは `ui::executable_playlist_path` が解決した実行ファイル隣の `playlist.txt` を読む。`--wav path` は1曲の確認用入口として残し、未知・重複・不足した引数を拒否する。`--help` / `/?` は使い方を表示する。製品の `--driver` は廃止し、機器は設定ファイルだけで指定する。実機診断用コンソールのCLSID引数と、試験用の直接注入は独立した入口として残す。一覧・WAVの準備と機器の初期化はUIスレッドで行わない。

`PlayerControls` はソートしない単一選択の標準ListBoxで全パスを表示し、採用件数と除外件数をラベルへ出す。不変一覧が同じなら行を作り直さず、選択行だけを更新する。作り直すときは件数と文字列の総バイト数を `LB_INITSTORAGE` で先に確保してから行を追加する。水平スクロールの幅は実際の文字幅から求め、DPI・フォントが変われば測り直す。リサイズでリストと選択パス欄を広げ、選択パス欄は長いUnicodeパスも折返し・スクロール・選択コピーできる。クリックと上下キーは選択、ダブルクリックとリストにフォーカスがあるEnterは選択曲の先頭からの再生を要求する。起動時は未選択・停止で、未選択のPlay/Spaceは先頭曲を使う。空一覧は件数0・Play無効とし、不正UTF-8や読み取り失敗はエラー表示と区別する。

`player_window.cpp` はWin32の標準コントロールとシステムフォントを使う。状態欄は `PlaybackSnapshot::state` だけから更新し、保留操作は別欄に表示する。Play/Pauseは安定状態かつ `accepted == settled` のときだけ有効、Stopの重複は受付番号で抑止する。Space/Escapeは子コントロールへ渡す前にメッセージループで消費し、キーの自動反復を無視する。非アクティブ化（`WM_ACTIVATE` の `WA_INACTIVE`）ではフォーカス中の子コントロールを覚え、次の `WM_SETFOCUS` でまだ子・有効・表示中なら1回だけ戻す（DefDlgProcと同じ）。Alt+Tabの後もリストのEnterやStopのフォーカスが続く。それ以外の配送先は、有効なリスト、なければメインウィンドウとする。フォーカス中のコントロールを無効にしたとき（先に無効にしてから移すので、無効にするリスト自身は選ばない）と、アクティブなメインウィンドウにフォーカスがあるかどこにもないとき（起動時を含む）に使う。`refresh()` が毎回確かめるので、初期化後にリストが有効になればリストへ移る。最初のTab対象（読み取り専用のBuffer）やError欄へは移さない。読み取り専用のEditはキャレット点滅のタイマーを動かし、Enterを取り込むためである。メインウィンドウでもSpace/Escapeとアクセスキーは使える。生成順とTab順は変えない。Tabとアクセスキーは `IsDialogMessageW` で処理し、通常のウィンドウでもEnterが正しいボタンを実行できるよう `DM_GETDEFID` に応答する。修飾キー付きSpace/Escapeはトランスポート操作にしない。リサイズとDPI変更では文字と配置を更新する。

Closeは要求だけを送り、通常は通知先のHWNDを `closed` まで残す。その時点でドライバー・WAV・制御STAの解放が終わっており、ウィンドウを破棄してコントローラーをjoinする。Close中に初めて発生したエラーはClosed状態で表示し、再度Closeを受けて終了する。外部のWM_QUITでは解放後に確認待ちを残さず終了コードを返し、故障があれば1を優先する。初期化途中の例外と通常終了はWM_QUITを投稿しないため、同じUIスレッドを再利用しても終了通知が残らない。

ログオフ・シャットダウン・Restart Managerの終了では、`WM_ENDSESSION` から戻った後いつでもプロセスが終わり得る。`WM_QUERYENDSESSION` は副作用なくTRUEを返し（拒否しない）、wParamがFALSEの `WM_ENDSESSION`（終了の取消し）では何もしない。TRUEの `WM_ENDSESSION` は外部のWM_QUITと同じ扱い（終了コードは未設定なら0、確認待ちなし。表示中の終了処理エラーはその場で確認済みにする）で `request_close()` を通し、配置を取得してCloseを送る（配置の取得に失敗した場合はUI故障として配置なしのCloseを送り、待機は続ける）。まだ `closed` でなければ通知先のHWNDを外して以後の公開がイベントをセットするようにし（待機中は投稿メッセージを処理しないため）、`ShutdownBlockReasonCreate` で理由を示し、`closed` まで1つの単調時計の期限で最大約4秒待つ。`closed` の確認でsnapshotを複製できなければ未完了として期限まで待ち続け、通知先を外してから戻すまでの間は例外を出さない（通知先と理由は必ず戻る）。待つ間は送信メッセージだけを処理し（`MsgWaitForMultipleObjectsEx` の `QS_SENDMESSAGE` と `PeekMessageW` の `PM_NOREMOVE | PM_QS_SENDMESSAGE`）、ドライバーの `SendMessage` と相互に待たず、投稿・入力メッセージを再入させない。理由を外した後、ウィンドウが残っていれば通知先を戻して通知を1件出し、次に動くループ（モーダルループを含む）が `closed` を描画してウィンドウを破棄する。期限を過ぎたら解放の完了を待たずに戻る。終了処理のエラーは表示できず、終了コードだけに残る。

UIは `MsgWaitForMultipleObjectsEx` で状態イベントとWindowsメッセージを待つ。通知は未処理の先頭でウィンドウへ投稿するだけで、その投稿が残る間は後続を1件へ集約する。投稿は外側のループ（`QS_ALLINPUT` と `MWMO_INPUTAVAILABLE`）を起こし、Windowsの移動・サイズ変更・メニューなどのモーダルループでも処理されるので、1件の変更でUIを起こすのは1回になる（以前はイベントと投稿で2回起き、先に起きた側の描画の後に空の起床が残った）。イベントは、通知先のHWNDがない場合（破棄後とセッション終了の待機中）と投稿の失敗時、およびUI自身の強制更新（初回、Close要求、UI故障、破棄、最小化からの復帰）にだけ使う。revisionは毎回公開する。投稿の処理開始時に保留フラグをexchangeで解除するので、公開側のexchangeと同期してその後のrevisionの読み取りが順序付けられ、以後の変更は新しい通知になる。ウィンドウ破棄時は同期状態の再取得とイベント起床を強制し、破棄で消える投稿を補う。定期ポーリングは行わない。待機自体が失敗した場合だけ、すぐには再試行せず50 ms後に1回待ち直し、次に失敗しなければ元の待機へ戻る（1回の失敗で以後の50 ms周期に固定しない）。UI故障として示すのは最初の失敗だけで、失敗が続いても待ち直しのたびにError欄を描き直さず、2回目の故障を `closed` の確認として扱ってエラーを見せないままウィンドウを閉じることもない。1回の処理件数を制御STAと同じ `core::message_batch_limit` で制限し、連続した入力でも状態更新を妨げない。通知オブジェクトはコントローラーより長く所有し、先にUIが破棄された場合も解放完了までイベントを待つ。

最小化中は、進捗だけの公開（`progressed`）でrevisionだけを記録し、投稿もイベントもしない。毎秒の時間表示のためのUIの起床・描画は、誰も見ていない間は止まる。状態・故障・確定・`closed` の公開は最小化中も通知し、`closed` によるウィンドウ破棄と終了コードは変わらない。最小化の印はUIスレッドが `WM_SIZE`（`SIZE_MINIMIZED` で立て、それ以外で下ろす）で書くatomicで、状態のロック中に呼ばれる通知は `IsIconic` を呼ばずにこれを読む。最小化から戻る `WM_SIZE` では強制更新の印とイベントを立て、ループの次の `refresh()` がrevisionのヒントによらず同期されたsnapshotを1回読む（印を下ろしてからsnapshotをロック中に読むので、その後の進捗は通知され、その前の進捗はsnapshotに含まれる）。`refresh()` は配置の復元で `WM_SIZE` を起こし得るため、`WM_SIZE` から直接は呼ばない。最小化中でもUIスレッドが別の理由で起きれば、ループの終わりの `refresh()` がヒントで最新の秒を描く。隠れているだけで最小化していないウィンドウは従来どおり毎秒更新する。時間欄の幅は変えない。

`player_window.cpp` は通知、操作受付、確定状態の反映、非同期終了を担当する。故障かどうかは表示文字列ではなく `PlaybackSnapshot::faulted()`（例外、または曲単位の拒否を除く `playback::is_fault`）で判定し、終了コードに反映する。エラー欄は例外、なければ `track_error` を `ui::exception_message` で表示し、どちらもなければ結果の固定文（`ui::result_text`）を表示する。読み込み失敗（結果 `read_error`）では固定文（読めないことと再起動の案内）を残し、生産者の例外を改行と `Cause: ` の後に続ける。`std::throw_with_nested` で付いた原因も外側から順に改行と `Cause: ` で続け、最大4段まで示す。各段の `system_error` は `FormatMessageW` の文を使う。ボタンの有効状態と操作受付は `player_text.cpp` の同じ `available` の結果を使い、コントロールの表示状態から再生可否を推測しない。状態・結果・エラー欄（`error_text`）・Buffer・Output・Device・一覧ラベル・時間・保留操作の表示文字列も同じファイルの純粋関数で作り、HWNDなしで試験できる。終了の状態機械（Close要求・確認・破棄・UI故障のフラグ）は `player_window.cpp` に残す。ウィンドウ手続きとキー配送の両方で例外を捕捉し、UIを動かしたまま停止・解放する。再描画中の同期通知が同じ表示更新を再入しないようにする。現在曲の追従と選択、曲ごとの経過/総時間を表示する。再生中の表示は提出済みPCMに基づき、実機の聴取位置は推測しない。

同一曲の通常進捗では時間欄だけを更新する。`PlaybackSnapshot` は完全な複製と既存mutexを保ち、非進捗の公開で既存revisionを状態世代として保存する。UIは前回の描画が成功した状態世代と現在曲を照合するため、状態変更の直後に進捗が通知されても未描画の変更を失わない。曲境界・操作・故障・強制更新は全表示を更新し、最小化からの復帰とUI故障時のCloseを短縮しない。機器・出力・バッファー等の変わらない文字列の再整形とWin32照会を通常進捗から外すだけで、通知・表示頻度・音声経路は変えない。

時間表示は `PlaybackSnapshot` の `position_frames`・`frame_count`・`sample_rate` から整数除算で求め、レート0は未取得として扱う。再生中は `TrackPlayer` が現在曲の提出済みPCMを公開し、手動選択だけでは時間の対象を変えない。停止後は選択曲の情報を必要時だけ準備し、選択変更・次曲拒否で前の曲の時間を残さない。開始無音と末尾のゼロ埋めは経過に含めず、Pauseの保持位置、Stopの0、自然終端の総フレーム数を同じ表示へ渡す。曲ごとのレートを使うため、異レートの一覧でもフレーム数を足し合わせない。

通常の進捗通知は制御側の既存待機で曲番号または整数秒が変わった場合だけ公開する。待機の起床周期は区間ごとに1回、1秒から20 msと1ブロック時間を引いて求め（下限0.5秒）、進捗の間隔とコールバック監視期限の下限に共用する。待機は既定のタイマー刻み（15.625 ms）1つ分まで遅れて終わり、提出PCMはブロック単位で進む。ちょうど1秒の周期では標本の位置が少しずつ遅れ、ときどき整数秒を1つ飛ばすため、刻み1つ分の遅れとブロックの丸めを合わせても間隔が1秒を超えないようにした。それより長いスケジューリング遅延では飛びが残り得る。47 ms以下のブロック（44.1 kHzの2048フレームまで）では933～979 msとなり、監視期限の下限は従来の1秒よりわずかに短いが、4ブロック時間（最大188 ms）を大きく上回る。ブロックが長いほど下限は下がり、約196 msのブロック（44.1 kHzの8640フレーム）で4ブロック時間と一致する（784 ms、従来は1秒）。それより長いブロックの監視窓は4ブロック時間だけで決まり、3ブロック分の遅れを許す。従来も250 ms以上のブロックでは同じだった。通常は既存のコールバック監視の起床を使い、非常に大きいバッファー幅で4ブロック時間が起床周期を超える場合だけ、同じ待機へ起床周期の進捗期限を加える。コールバック不着の監視期限は独立して保持し、進捗表示のために延長しない。UIタイマー、追加の常駐スレッド、コールバックからの進捗通知やドライバー位置照会は追加しない。UIは秒単位の値が同じなら表示文字を更新せず、最小化中は進捗の公開で起きない。

`player_controls.cpp` は標準コントロール、システムフォント、配置、フォーカスを担当する。子コントロールのID（`control_id`）もこのビューが定義し、連続した範囲として表示文字のキャッシュの添字に使う（連続性は `static_assert` で固定）。読み取り専用のBuffer/File/Error欄にも独立ラベルとTab移動を用意し、値を選択してコピーできる。複数行EDITがTabを取り込む点だけを `SetWindowSubclass` で補い、他の文字操作は標準処理に任せる。値が同じならテキストを設定し直さず、選択・スクロールを保持する。表示文字のキャッシュは `SetWindowTextW` の成功後にだけ新しい値へ入れ替え、常に実際の表示と一致させる。UI故障時に例外なしで直接設定する `show_text` は該当欄のキャッシュを不明扱いにし、キャッシュと表示の不一致を残さない（次に `text` を呼べば必ず設定し直す）。UI故障は終端状態で、`player_window.cpp` は以後 `show_text` だけを使い、通常更新へ戻らない。故障後の表示（状態とStop/Close）も通常更新と同じく公開されたrevisionごとに1回だけ設定し、マウス移動などの無関係な起床では設定し直さない（故障の直後の1回は強制する）。計測用に `GetDC` で得たウィンドウのクライアント領域DCとフォント選択はRAII（`ScreenDc`）で例外時も戻して解放する。GDIフォントを所有し、DPI・システム設定・テーマ変更時は実際のLOGFONTとDPIを比較して、変化した場合だけ再生成・計測する。フォント単位の配置から最小ウィンドウ寸法も算出し、フォントまたはクライアント寸法が変わった場合だけ配置をまとめて更新し、子コントロールも再描画する。

`run_player_window` は専用のUIスレッドで最初のトップレベルウィンドウを作る前に呼ぶ。編集可能な入力を持たないため、製品側で `ImmDisableIME(0)` を呼び、このスレッドのIME初期化を避ける。ドライバーを所有する別のSTAには作用させない。試験用fixtureで無効化して製品の挙動を覆い隠さず、所有UIスレッド上で既定IMEウィンドウがないことと、別スレッドのIMEが保たれることを確かめる。同じUIスレッドは、`window.create()` の後（制御STAの開始後）から `run_player_window` が戻るまで `core::ThreadSpeedScope` で `ui_thread_speed`（現在はWindows既定）を要求し、戻る前にも既定へ戻す。試験がスレッドを再利用しても後のケースへ残さない。`run_player_window` はスレッドに名前を付けない（製品では `main.cpp` が主スレッドに付ける）。

起動引数と `PlayerOptions` は `player_options.cpp`、Unicode診断は `error_message.cpp` に分ける。Windowsエラーは `FormatMessageW` で取得し、起動失敗は1つのUnicodeダイアログで示す。使い方の説明は起動引数の解析を囲む独立した `try` の `std::invalid_argument` にだけ添え、後続の単一起動・設定パス・GUIの失敗には添えない。`ui` 層が直接取り込める製品層は `core`・`asio`・`control` で、`ui` のどのファイルからたどれる製品ヘッダーにも `asio/asio_sdk.h` を含めない（現在 `asio` から使うのはSDK非依存の `device_settings.h`・`driver_registry.h` だけ）。`src/app/main.cpp` は `ui` と `core` だけを取り込む。下位層から `ui` へ依存させない。

製品入口は有効な起動引数を解析した後、設定・一覧の読み込みと機器初期化より前に `ui::SingleInstance` で単一起動を判定する。同じユーザーSIDとWindowsセッションの `Local` 名前空間を使い、設置フォルダーによらず共有する。`acquire_or_activate` で新規GUIを作る権利を得た場合だけ起動を進め、`attach`・`detach` で前面化先のウィンドウの寿命を管理する。先行GUIの準備・終了中は最大5秒待ち、先行プロセスが終了・異常終了して占有が解ければ取得し直す。既存ウィンドウの無応答や期限超過ではエラーにし、別のGUI・機器を二重に開かない。後発起動の `--wav`、設定、リストは既存プロセスへ適用しない。

後発プロセスは対象PIDだけへ `AllowSetForegroundWindow` の権利を渡し、既存GUIは `handle_activation` で最小化から `SW_RESTORE` して `SetForegroundWindow` を呼ぶ。Windowsの制限で前面化できない場合はタスクバーを点滅させ、OSのフォーカス制限を変更しない。[前面化権限の移譲](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-allowsetforegroundwindow)・[前面化の条件](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-setforegroundwindow)。ヘルプと不正な引数は従来どおり機器を開かず処理する。

両実行ファイルに `build/app.manifest` を埋め込み、起動時のヘルプ・エラー画面を含めPerMonitorV2、Common Controls v6、通常ユーザー権限、長いパスへの対応を宣言する。実際の埋め込み内容も検証スクリプトで監査する。DPIのプロセス既定値をmanifestで指定する方針は [MicrosoftのDPI設定](https://learn.microsoft.com/en-us/windows/win32/hidpi/setting-the-default-dpi-awareness-for-a-process)、標準コントロールとTabの扱いは [Visual Styles](https://learn.microsoft.com/en-us/windows/win32/controls/cookbook-overview) と [IsDialogMessage](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-isdialogmessagew) に合わせる。

プレイリストと単曲は操作・設定・ウィンドウの経路を共有する。一覧の解析・ファイル準備・設定I/O・GUI表示文字列の生成はコールバックの外へ保つ。

実機では選択したドライバーが返す能力を使い、機種名や特定ドライバーのバージョンで分岐しない。音声経路の自動試験が成立した段階で、[実機試験の手順](test-specification.md#実機試験の手順)に従って確認する。

UIの通常描画では、1個の文字列作業領域を各formatterへ渡す。PlayerControls.text_reuseは表示が同じなら領域を保持し、更新成功後にキャッシュとswapして以前の確保領域を返す。画面更新の後に文字列確保を行わず、失敗時はキャッシュを進めない。snapshot取得、通知の集約、revision、最小化からの再取得は従来の契約を維持する。
