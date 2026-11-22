# ビルド設定・成果物とソースの監査

Windows x64の開発者とCIが[全検証](test-specification.md#ローカルとciの全検証)で行う、ソース登録・ビルド設定・生成物の判定条件を示す。以下で「木の複製」や別ビルドを指定する否定確認は、担当者が監査規則を検証する際の入力例。通常の全検証が毎回作る変異入力は[監査の自己検査](#監査の自己検査)に示す。

## ソース登録とビルド設定

`scripts/check-project-layout.ps1` は登録・依存・文書を調べる。`scripts/verification/build-settings.ps1` は共有ライブラリ・製品・試験の各プロジェクトで、指定した構成のコンパイル設定を評価する。

`scripts/tests/project-layout.tests.ps1` は公開対象の複製へ「構成の整合」「製品の文字列の整形」「文書の見出しへのリンク」の違反を1つずつ加え、`check-project-layout.ps1` がその理由で拒否すること、コメントだけの言及と外部リンクを合格にすることをCIで確かめる。

**構成の整合**

- 入力・操作　共有ライブラリのソース登録、両EXEからのプロジェクト参照と専用ソース登録、直接の層間include、`playback`・`control` の公開ヘッダー、SDK境界、`ui`・`main.cpp`・`playback_controller.h` の推移的なSDK非依存、下位層の試験群の直接の依存、試験ファイルと `suites.h` の表、Solution Items。
- 合格条件　`check-project-layout.ps1` が共有一覧の読み込み漏れ・EXEへの二重読み込み・共有オブジェクトへの参照漏れ、ソースの登録漏れ・重複・存在しない項目・禁止依存・他層の内部ヘッダー・SDKへの推移的到達・下位層試験から再生エンジンやGUIへの直接の依存・表にない試験ファイル・試験ファイルのない表項目を拒否する。

**製品の文字列の整形**

- 入力・操作　`src` の全ソース。否定確認は、木の複製へ `#include <format>`・`<sstream>`、`std::to_chars`・`std::format` の呼び出し、`//` のコメントだけでの言及を1つずつ加えたもの。
- 合格条件

  - `check-project-layout.ps1` が `//` のコメントを除いた行で、`<format>`・`<print>`・iostream・文字列ストリーム・`<iomanip>`・`<locale>` のinclude、`std::format` 系・`std::print`・`std::to_chars` を拒否し、ファイルと行を示して失敗する。`std::from_chars` のための `<charconv>` は許す。
  - コメントだけの言及は合格。ツールセットに依存しない。

**文書の見出しへのリンク**

- 入力・操作　README.mdとdocs/*.mdの断片付きの相対リンク。否定確認は、木の複製で見出しを改名したもの、存在しないファイル・断片へのリンクを加えたもの。
- 合格条件　`check-project-layout.ps1` がGitHubの規則（小文字、文字・数字・`_`・`-`・空白だけを残し、空白を `-`、重複に `-1`・`-2`）で求めた見出しの断片へ全リンクが解決することを確かめ、ないファイル・見出しを示して失敗する。

**ビルドの設定**

- ツールセット　`Directory.Build.props` でMSVCを `14.52.37025` に固定する。製品・共有ライブラリ・試験で同じ版を選び、ビルド前に `cl.exe`、`link.exe`、`dumpbin.exe` と、ASan構成で使うランタイムの存在を確認する。固定版がない環境や別の版を指定したビルドでは、コンパイルに入る前に失敗する。
- 入力・操作　全検証では3つのプロジェクトのDebug・Release・Debug-ASanを `MSBuild /getItem:ClCompile` で評価し、`planar_conversion` 群をコンパイルする。否定確認は木の複製の `build/Common.props`・`build/WavplaySources.props`・試験プロジェクトを1か所ずつ変えて行う。
- 各翻訳単位　`LanguageStandard=stdcpp23`。CRTはReleaseで `/MT`、Debug・Debug-ASanで `/MTd`。`EnableASAN` はDebug-ASanの全翻訳単位で有効にする。追加オプションは `/utf-8 /Zc:__cplusplus /arch:AVX2 /Zc:throwingNew /Zc:preprocessor /Brepro` の順に各1つ。起動ガードの `src/app/startup.cpp` と `src/wavplay/core/cpu_features.cpp` では同位置が `/arch:SSE4.2`。clは最後の競合指定を採るため、MSBuildが項目の設定の後に渡す `AdditionalOptions` の追加・重複（`/arch:AVX512`・`/Od`・`/GL-`・`-std:c++latest`・`/Zc:preprocessor-` 等）を拒否する。
- 最適化　全体最適化はReleaseだけにあり、起動ガードの2翻訳単位は全構成で除外する。Releaseの製品コードは `src/wavplay/ui`・`settings`・`playlist` 配下（下位ディレクトリを含む）が `MinSpace`、それ以外は `MaxSpeed`。違反は翻訳単位と値を示す。
- コンパイル時　`_MSVC_LANG==202302L`・`_HAS_CXX26==0`・`__cplusplus==_MSVC_LANG`・`_MSVC_TRADITIONAL==0`・`NTDDI_VERSION==NTDDI_WIN11_GE`、`_WIN32_WINNT` と `WINVER` が `_WIN32_WINNT_WIN10`、`TCHAR` が `wchar_t` と同じ大きさであることを確かめる。SDK 10.0.26100では既定値が同じため、`NTDDI_VERSION` の固定を外してもこの検査では検出しない。

## マニフェスト・版情報・ReleaseのPE

`scripts/verification/release-image.ps1` がバイナリに埋め込んだ値を検査する。マニフェストは全構成の両EXE、その他は各項目に示した成果物を使う。

**EXEに埋め込まれたマニフェスト**

全構成の製品とテストからresource #1を抽出し、PerMonitorV2、Common Controls v6、Windows 10/11互換、longPathAware、asInvoker/uiAccess=falseを各1件確認する。

**Releaseの版情報**

- 入力・操作　Releaseの `wavplay.exe` と埋め込みマニフェスト。否定確認は、木の複製の `build/app.manifest` の版を1.0.0.1にしたビルドと、VERSIONINFOのない `wavplay_tests.exe`。
- 合格条件　`verify.ps1` が両実行ファイルのマニフェストに `Wavplay.WindowsHost` の `assemblyIdentity` が1つあることを確かめる。`wavplay.exe` の製品名・説明が `WAV Player` でない、またはファイル・製品の版（文字列と固定値）がマニフェストの版と一致しなければ失敗する。エクスプローラーのプロパティ画面は見ない。

**Releaseのプロセス全体の緩和策**

- 入力・操作　Releaseの `wavplay.exe`。否定確認は `out\` 以外へ出力した `/guard:cf`・`/CETCOMPAT` 付きの検証用ビルド。
- 合格条件　`verify.ps1` がPEヘッダーのフラグだけを読み、`DllCharacteristics` のGUARD_CF（0x4000）または `CET compatible` の表示があれば[設計の判断](architecture.md#命令セットの前提)を示して失敗する。ツールセットのコード生成・インライン展開に依存しない。

**Releaseのスワップからの実行**

- 入力・操作　Releaseの `wavplay.exe`。否定確認は、その複製のファイルヘッダーの特性を0x22・0x422・0x822に書き換えた入力。
- 合格条件　`verify.ps1` がファイルヘッダーの特性だけを読み、`/SWAPRUN:CD,NET` の2つのフラグ（リムーバブル媒体0x400・ネットワーク0x800）のどちらかがなければ[メモリ配置](architecture.md#チューニングの入口)を示して失敗する。製品の `wWinMain` が `core::lock_process_image` を呼ぶことは自動では確かめず、製品の照会で確かめる。

**Releaseの取り込みDLL**

- 入力・操作　Releaseの `wavplay.exe`。否定確認は `out\` 以外へ出力した、WINMMを通常・遅延読み込みで、`.dll` 以外の名前のWINSPOOL.DRVを通常で追加した検証用ビルドと、許可リストからDLLを除いた・加えた場合。
- 合格条件　`verify.ps1` が `dumpbin /dependents` から通常と遅延読み込みの取り込みモジュール名だけを読み、同スクリプトの許可リスト1つと大小文字を区別せず完全に一致しなければ、リスト外のモジュール、または取り込まれなくなったDLLの名前を示して失敗する。C/C++ランタイムのDLLはリストにないので、動的CRTへの依存もリスト外として拒否する。

**Releaseのデバッグディレクトリ**

- 入力・操作　Releaseの `wavplay.exe`。否定確認は、木の複製でリンクの `/Brepro /PDBALTPATH:%_PDB%` を除いた、または `/GL`・`/LTCG` を除いたReleaseのビルド。
- 合格条件　`verify.ps1` が `dumpbin /headers` のデバッグディレクトリを読み、`coffgrp` の `(LTCG)` の印と `repro` の項目がそれぞれ1つ、`cv` が1つで `wavplay.pdb` をパスなしで示すことを確かめ、なければ `build/Common.props` の該当するスイッチを示して失敗する。PEヘッダーのデータだけを読む。

**Release成果物の識別**

- 入力・操作　Releaseのビルド直後。
- 合格条件　`verify.ps1` がMSVC（`VCToolsInstallDir` の版）・Windows SDKの版と、`wavplay.exe`・`wavplay_tests.exe` のSHA-256を表示する。ツールセットが違えば正当に変わるので、ハッシュの一致は判定しない。

## コールバックと起動時の生成コード

`scripts/verification/realtime-code.ps1` と `startup-code.ps1` を使う。Releaseは起動ガードの2翻訳単位を除きLTCGでビルドする。その `.obj` は中間表現なので、出荷するEXEの逆アセンブルまたはリンク時の `/FAcs` 一覧で生成コードを確認する。[最適化の方針](architecture.md#最適化の方針)

起動時は、ガードおよび製品側のCRT/TLS初期化、CPU要件を満たさなかった後の終了経路をSSE4.2の範囲に制限する。ガード通過後のコールバックには整数AVX2/BMIを含む別の[起動契約](architecture.md#命令セットの前提)を適用する。

`dumpbin` が宛先を `シンボル名 (RVA)` と表示する場合、イメージベースを加えたアドレスと関数入口の名前を照合する。起動時の `atexit` に登録されるデストラクターは、従来の絶対アドレス表記とこの表記を読み、リンクマップを確認して到達経路を監査する。

**Releaseのコールバック経路の機械語**

- 入力・操作

  - Releaseの `wavplay.exe` と隣のPDB。否定確認は、木の複製の `SegmentRenderer::render` に `OutputDebugStringW`・256 bit命令を生成する組み込み関数・AVX512の組み込み関数・可変長の `memcpy` を加え、または `silence` を `memset` に戻して `out\` 以外へ出力した検証用ビルドと、実際の逆アセンブルを書き換えた入力。
  - いずれも `[Management.Automation.Language.Parser]::ParseFile` で `scripts/verify.ps1` から `Get-CheckedOutput` と `Assert-Rejected` の定義を取り出し、`scripts/verification/` の `disassembly.ps1`・`realtime-code.ps1`・`linked-image.ps1`・`startup-code.ps1` の関数定義を読み込む。
  - `Assert-RealTimeCode` に、PDBを隣に置いた実行ファイルと `dumpbin.exe` のパスを渡して監査する。書き換えた入力は、`/disasm:nobytes` の呼び出しにだけ用意した出力を返す `Get-CheckedOutput` を後から定義して与える。呼び出し先と入口を `wavplay::ui`・`wavplay::settings`・`wavplay::playlist` の装飾名・非装飾名へ改名した入力も与える。

- 合格条件

  - `verify.ps1` が `dumpbin /disasm:nobytes` を大小文字を区別して読み、`switch_buffer`・`sample_rate_changed`・`asio_message`（コールバック表の項目）と、関数として残れば `is_streaming`（表の項目ではなく、`switch_buffer` の下で呼ばれる）から直接の呼び出し・ジャンプを推移的にたどる。
  - 到達した製品の関数はインライン展開の形によらず許すが、サイズ優先でビルドする `wavplay::ui`・`wavplay::settings`・`wavplay::playlist` の関数（装飾名・非装飾名とも）に到達すれば、その関数を示して失敗する。取り込み関数は `SetEvent` だけを許す。整数AVX2/BMIとLZCNT・MOVBE等は起動契約と以下の命令規則の範囲で許す。
  - 呼び出し先　ほかの取り込み関数、間接呼び出し、CRTの `memcpy`・`memset` を含む製品外の関数、`__chkstk`・GSクッキー・`__guard_*`・`_CxxThrowException`・`operator new/delete` は違反。
  - 命令　浮動小数点の算術・変換・比較（VEX形式を含む）、許可した整数・移動命令以外のVEX命令、AVX512/EVEX、zmm・マスクレジスターのオペランド、AVX512のマスク命令 `k*` は違反。CRTのCPU判定（`__isa_available`・`__isa_enabled`・`__isa_inverted`・`__favor`）の読み取りも拒否する。
  - 幅　YMMレジスターまたは `ymmword` オペランドを使う命令を拒否する。オペランドのない `vzeroupper` と `vzeroall` も拒否する。
  - 診断　違反やint3の詰め物以外の解読できない行は関数と命令を示す。ラベル欠落はPDB、入口欠落は関数名を示して失敗する。
  - 間接ジャンプは、製品関数の中で連続する4命令 `lea B,[イメージベース]`・`mov E,dword ptr [B+I*4+D]`・`add R,B`（RはEの64ビットでBと別）・`jmp R` からなり、表（イメージベース+D）が `jmp` より後ろの同じ関数内にあるMSVCのswitchの分岐だけを許して表示し、その表から先をデータとして読まない。間に別の命令が入る形（`lea` の巻き上げを含む）は間接ジャンプとして失敗する。
  - 製品の関数・命令・ロック付き命令の数、取り込み関数の呼び出し、4 KiBのコードページを表示する。

**Releaseの補充の定常経路の機械語**

- 入力・操作　コールバック監査と同じ逆アセンブルを使う。否定確認は、木の複製で変換カーネル1つに256 bit命令を生成する組み込み関数を加え、`out\` 以外へReleaseをビルドして監査する。
- 合格条件

  - `PrefetchRing::Impl::fill_slots` と `wavplay::audio` の無名名前空間にある全関数を入口とする。変換カーネルは間接呼び出しなので、関数名から拾う。`fill_slots` が見つからない場合と、カーネルが1つも見つからない場合は失敗する。
  - 製品の関数への直接の呼び出し・ジャンプをたどり、曲境界の `GaplessRun::advance_track` の手前で止める。到達した各関数にはコールバックと同じ幅の規則だけを適用する。取り込み関数、CRT、例外の送出はこの監査で許す。
  - 補充を準備する `PrefetchRing::Impl::produce` と `GaplessRun::advance_track` は、YMMオペランドを使う命令と `vzeroupper` の件数を関数ごとに表示する。件数は参考値とする。

**ReleaseのCRTの表明**

- 入力・操作　Releaseの `wavplay.exe` と隣のPDB。否定確認は、PDB付きのDebugの `wavplay.exe`、木の複製で `build/Common.props` のReleaseの `NDEBUG` を除いたRelease、規則が何も返さない複製。
- 合格条件

  - `build/Common.props` はReleaseだけに `NDEBUG` を定義し、製品の表明は標準の `assert` のままDebugだけで評価する。`verify.ps1` が機械語監査と同じ `dumpbin /disasm:nobytes` の出力に `_wassert`（ラベルまたは呼び出し先）があれば、Releaseが `NDEBUG` を定義し、製品のソースがそれを取り消さないことを求めて失敗する。
  - `switch_buffer` に `call _wassert` を挿入した複製を拒否することも確かめる。名前だけを読み、インライン展開の形に依存しない。

**ReleaseのCRTのstdioと変換のブロック関数**

- 入力・操作　Releaseの `wavplay.exe` と隣のPDB。否定確認は、木の複製で `DriverSession` の報告に `std::fputs` を加えたRelease、モノPCM16→Int16の `core::copy_block` を `std::memcpy` に戻したRelease、規則の片方を何も返さないようにした関数。
- 合格条件

  - `verify.ps1` が機械語監査と同じ逆アセンブルで、CRTのstdioの出力関数（`fputs`・`fwrite`・`fputc`・`puts`・`__stdio_common_vf(w)printf` 系など）の名前がラベルか操作対象にあれば失敗し、`wavplay::audio` の関数（引数の型ではなく関数自身の名前空間で判定する）から `memcpy`・`memmove`・`memset` への直接の `call`・`jmp` があれば関数と命令を示して失敗する。
  - `wavplay::audio` の関数がなければPDBを示して失敗する。`switch_buffer` へ挿入した `call fputs` と、最初の `wavplay::audio` の関数へ挿入した `call memcpy`（名前のみ、名前とRVAの両形式）を拒否することも確かめる。CRTの終了処理の `_fflush_nolock` などは対象にしない。名前だけを読み、インライン展開の形に依存しない。

## 監査の自己検査

実際の `dumpbin /headers`・`/dependents` の出力と逆アセンブルの、メモリ上の複製。各複製に違反を1つ加える。

実際の監査が合格した後、`verify.ps1` が各複製を同じ判定に通し、加えた違反の理由で拒否されなければ失敗する。挿入は監査が見つけた入口に行うので、特定のコードの形を前提にしない。規則を1つずつ無効にした監査スクリプトの複製では、その規則の件で失敗する。

- PEヘッダー・取り込みDLL　コンソールの副系統、GUARD_CF、`CET compatible`、ネットワーク媒体のスワップの印の欠落、LTCGの印の欠落、PDBのパス、`repro` の欠落、許可リスト外のDLL、取り込まれなくなったDLLの9件。
- 逆アセンブルはコールバック19件と補充3件の計22件を拒否することを確かめる。コールバックからたどった呼び出し先が関数として残れば、その関数を `wavplay::ui` に改名する1件を加える。

  - コールバック　`switch_buffer` の最初の命令の直後に、`OutputDebugStringW` の取り込み呼び出し、`call rax`、`jmp` より前の表を読むswitchの形、`addsd`、`__chkstk`、`operator new`、zmmのオペランド、YMMオペランドを使うAVX512の `vmovdqu64`、`vmovdqu ymm0,ymmword ptr [rcx]`、`vzeroupper`、`vaddsd`、`__isa_available` の読み取り、`memcpy`、存在しない呼び出し先、名前とRVAの不一致、データの行をそれぞれ挿入する。入口の改名、入口の `wavplay::ui` への改名、ラベルの削除も試す。名前とRVAが一致する呼び出しの合格も確認する。
  - 補充　`fill_slots` と最初の変換カーネルに、それぞれYMM命令を挿入する。コールバックのYMM命令・`vzeroupper` と合わせた4件は幅の規則を理由に拒否する。`fill_slots` には一覧に存在しないRVAを伴う呼び出しも挿入する。

CRTの表明・stdio・ブロック関数は前節の挿入例を拒否する。`scripts/verification/startup-code.ps1` は実際の起動ガードでの `vmovdqu ymm0,ymmword ptr [rcx]` と `call rax`、初期化関数での `vzeroupper` とCPU確認前の `run_application` 呼び出しをそれぞれ拒否する。デストラクター登録は絶対アドレスと名前＋RVAの両形式の合格、名前とRVAの不一致と範囲外のRVAの拒否を確認する。ビルド設定・マニフェスト・版情報の監査には同じ自動自己検査がない。

## 実装レビューと手動での再現性確認

**コールバックからの到達範囲**

- 担当者は `playback/segment_renderer.cpp`、`stream::PrefetchRing::Consumer` の `pop`・`read_failed`、`PrefetchRing::pop_block`、`PlaybackSignal` と `core/block_copy.h` のインライン操作を変更時に読み、確保・待機・ディスクI/O・ログ・GUI呼び出しがないことを確認する。スピン待ちや処理の意味の変化は機械語の自動監査では分からない。ASanでメモリ境界、出力バッファーの照合で形式と左右順を確認する。[スレッドとリアルタイム境界](architecture.md#スレッドとリアルタイム境界)
- 各公開再生入口で最終位置の確定まで排他を保つことをコードで確認する。並行再生要求をドライバー照会前に拒否する挙動は既存の自動試験で確認する。
- ファイルのopen・reopen・読み取りと一覧読み込みの最中に、必要なスレッドのエラーモードが立つことをコードで確認する。媒体のないカードリーダーまたは光学ドライブがある場合は、システムのダイアログを出さず、除外または読み取りエラーになることを手動でも確認する。

**同一条件からのRelease再ビルド**

両EXEは `/Brepro`（cl・link）と `/PDBALTPATH:%_PDB%` でビルドし、時刻とPDBのGUIDを内容のハッシュに、デバッグディレクトリのPDBをファイル名だけにする。`/Brepro` は文書化されていないため、再現性は再ビルドで確認する。

1. 同じコミット・ツールセット・SDK・ソース配置から、既存出力のない2つの一時ディレクトリへ `MSBuild src\app\wavplay.vcxproj /t:Rebuild /p:Configuration=Release /p:Platform=x64 /p:BaseOutputPath=<一時ディレクトリ>\` を実行し、`wavplay.exe` のSHA-256を比べる。
2. `dumpbin /headers` のデバッグディレクトリは `cv` に `wavplay.pdb` だけを示し、`repro` の項目を持つ（`verify.ps1` の監査対象）。
3. 通常のビルドで既存PDBへ再リンクすると、非増分リンクでもPDBの世代が進みハッシュが変わることがあるため、比べるのは再ビルドどうしとする。製品EXEの不一致は調査し、原因を説明せず再現性を認定しない。試験コードは `std::format` を使い、試験EXEの一致は製品EXEとは別に扱う。LTCGでは配置も生成コードに影響し得るため、異なるソース配置間のバイト一致をこの確認の条件にしない。

### Release 2版の配置比較

基準版と候補版のReleaseフォルダーに、それぞれ同じビルドの `wavplay.exe` と `wavplay.map` を用意する。対応する `wavplay.pdb` があれば同じ場所に置く。製品を起動せず、Windows PowerShell 5.1から次のように実行する。[配置比較コマンド](../scripts/compare-release-layout.ps1)

```powershell
.\scripts\compare-release-layout.ps1 -BaselineDir .\out\baseline -CandidateDir .\out\candidate -ReportPath .\out\layout.json
```

画面には主要関数と共有状態の移動量、JSONには両EXEのSHA-256、対応したシンボル、移動量が連続する範囲、命令とセクションの差を出す。位置は各イメージの先頭からの相対アドレス（RVA）、移動量は候補版から基準版を引いたバイト数。定数の集計はリンクマップの `__xmm@`・`__real@` シンボルを対象とし、詳細には所属オブジェクトも記録する。

64バイト境界をまたぐ命令は、静的にたどれた範囲で関数内の位置を示す。再配置に伴う参照先の変位、命令や即値の変更、確定できない差を区別する。固定版の `dumpbin` がない環境では命令欄を `Unavailable` とし、配置とセクションの比較結果を保存する。セクションのバイト差は両版にある長さで数え、追加・削除されたバイト数を別に示す。EXEとMAPの取り違えなどの入力異常はエラーになる。

### 先読みの実測比較

初期充填には `--measure-prefetch`、再生中の補充を模擬するには `--measure-prefetch-stream` を使う。どちらも試験用WAVを生成し、ASIO機器を必要としない。たとえば24/32入力の全件検査を含む条件は、リポジトリ直下で次のように測る。

```powershell
.\out\x64\Release\wavplay_tests.exe --measure-prefetch ext24in32
.\out\x64\Release\wavplay_tests.exe --measure-prefetch-stream ext24in32
```

入力は `pcm16`（既定）、`pcm24`、`pcm32`、`ext24in32`、`float32`、`extfloat32`。第2引数で無損失変換できるASIO出力型を指定できる。たとえば `--measure-prefetch-stream pcm24 ASIOSTInt32LSB` はステレオPCM24から32 bit整数出力への補充を測る。初期充填の比較には `--measure-prefetch` の予熱後の中央値と読み取り回数を使う。

リング1周当たりの補充頻度を比較する場合は、出力型に続けて `2`、`4`、`8` のいずれかを指定する。指定すると同じ約30秒の音源を使い、読み取り量と変換バッチを既定のまま、リング生成時に平均の通知間隔を決める。通知は製品と同じく短い間隔と長い間隔を交互に使う。PCM16・PCM24のステレオランプは、別の実行で同じ6条件の全フレームを32 bit整数出力の左右の期待値・終端と照合できる。

```powershell
.\out\x64\Release\wavplay_tests.exe --check-prefetch-pcm pcm24 2
.\out\x64\Release\wavplay_tests.exe --measure-prefetch-stream pcm24 ASIOSTInt32LSB 2
```

補充は1回の実行で次の6条件を順に測る。44.1 kHz・幅16の行は幅と量の両方の基準に使う。幅の単位は1ブロック当たりのフレーム数。

| 系列 | サンプルレートと幅 | 比較のしかた |
|---|---|---|
| 幅 | 44.1 kHzで8、16、64 | 1起床の元データ量がほぼ等しい条件で、ブロック幅による変換・格納経路の差を見る。 |
| 量 | 幅16で44.1、88.2、176.4 kHz | 元データ量がほぼ1倍・2倍・4倍になる3点で、CPU時間を「起床当たりの固定費＋元データのバイト数×バイト当たりの費用」に当てはめる。 |
| 継続 | 96 kHz・幅256 | 過去と同じレート・幅で再測定する。旧出力のCPU時間はハーネスを含むプロセス全体の合計なので、今回の生産者1起床の値と直接比較できない。 |

各行には、初期充填後の起床数、1起床の読み取り回数と生産者のCPU時間（µs）の中央値・最小・最大、元データのバイト数の中央値を表示する。頻度を省略すると、製品と同じ既定の8回／周で約13秒の音源を測り、従来の13列を出力する。ハーネスは `Sleep(10)` から戻るたびにプロセスの読み取り回数と転送バイト数を採る。読み取りが続いた周期を1起床にまとめ、最後の読み取りの後に行う変換も含めるため、次の読み取りのない周期までの生産者サイクル差を採る。末尾の短い補充も集計に含む。回数とバイト数はキャッシュされた試験用WAVへのI/Oを表す。

頻度を指定した場合は、1起床の元バイト数の最大値、初期充填後から最終フレームを取り出した直後までの生産者の総CPU時間（µs）、その総CPU時間を有効フレーム数から求めた再生秒数で割った値（µs／再生秒）を追加する。

起動時に時刻カウンター（TSC）を `QueryPerformanceCounter` で校正し、その係数で `QueryThreadCycleTime` の差分をµsに換算する。CPU時間は実行コアと周波数に依存する概算で、用途はこのPCでの補充費用の比較に限る。条件ごとに20回以上の起床を観測し、空ブロックや読み取り失敗がなく、全有効フレームを末尾まで取り出してEOFに達したことを確認する。

各構成を少なくとも3回測り、実行順を往路と復路で反転する。各実行の起床中央値を取り、その反復間の中央値を条件の代表値、最小・最大を反復範囲とする。比較する2条件について、反復範囲の重なりを記録する。

読み取り1回の費用は、入力形式・出力型・レート・幅をそろえ、元データ量がほぼ等しく読み取り回数が異なる2条件で、CPU時間の差を回数の差で割って見積もる。3種類の読み取り量をまとめて比較する場合は、入力形式・出力型ごとに、幅16の3レート×3種類の9点に「CPU時間＝起床当たりの固定費＋バイト単価×元バイト数＋読み取り単価×読み取り回数」を最小二乗法で当てはめる。読み取り単価には、読み取りとともに回数が変わる変換準備やバッチ公開の費用も含む。

同じPCのReleaseビルドを使い、AC電源・電源プランをそろえ、測定するコンソールを前面に保つ。測定中は他のビルド・試験・測定を止める。生の出力とコミット、ツールセット、SDK、最適化、ソース配置、電源条件、実行順は `private/results/` の日付付きフォルダーに保存する。
