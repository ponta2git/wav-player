# 依存とライセンス

## 開発環境

ビルドにはVisual Studio 2026 安定版、MSVC v145、Windows SDK 10.0.26100.0を使う。アプリが直接利用する外部音声APIはSteinberg ASIO SDK 2.3.4。SDKとツールチェーンはリポジトリへ同梱せず、ビルドPCで取得する。再生PCにはVisual StudioとSDKを置かない。[ビルド手順](../README.md#ビルドと検証)

## ASIO SDKの取得

初回の `build.cmd -FetchSdk` は [取得スクリプト](../scripts/fetch-asio-sdk.ps1) を通じて、次の公式ZIPを取得し、SHA-256を検査して `external/asiosdk/` へ配置する。取得は明示的な `-FetchSdk` 指定時だけ行い、SDK配置済みでは `build.cmd` を使う。既存SDKを `-FetchSdk` で上書きしない。

| 配布ファイル | バイト数 | SHA-256 |
|---|---:|---|
| `ASIO-SDK_2.3.4_2025-10-15.zip` | 8,910,208 | `D5EBF0C20DD2C5F43771FD0C1418F4B361BF52434EE670097CFA6B3A335E2ECA` |

[公式ZIP](https://download.steinberg.net/sdk_downloads/ASIO-SDK_2.3.4_2025-10-15.zip)・[SDKの配布ページ](https://www.steinberg.net/developers/asiosdk-open/)

手動で配置する場合も同じZIPのハッシュを確認し、ZIP内の `ASIOSDK/` の中身を `external/asiosdk/` へコピーする。`external/asiosdk/common/iasiodrv.h` が存在する構成にする。SDKの展開先と取得ZIPはGitの追跡対象外。SDKを更新するときは、取得スクリプト、CI、版・ハッシュの記載を同じ変更で更新する。

## ライセンスの範囲

このリポジトリの自作ファイル（ソースコード、スクリプト、文書、設定・ビルド定義）は **GNU GPL version 3 only（GPL-3.0-only）** で提供する。[本文](../LICENSE)

ASIO SDKの利用にはGPLv3の選択肢を用いる。取得したSDKの `LICENSE.txt`、`common/LICENSE.txt` と各ファイルの個別表記を確認し、元の著作権・ライセンス・免責表示を保持する。ASIOはSteinbergの商標であり、本製品の名称はWAV Playerとする。

Visual Studio、Windows SDK、Microsoftのランタイム、各機器のASIOドライバーは、自作部分のGPL表明の対象に含めない。これらは各権利者の条件に従って取得・利用する。MSVCの標準ライブラリヘッダーなど、個別のライセンス表記があるファイルもその表記に従う。

## ソース公開とローカルビルド

このリポジトリではソースのみを公開し、SDK、CRTのファイル、実行ファイル、再生用フォルダーのZIPを配布しない。`build.cmd` が作る `out/player/` は、利用者が自分のビルドPCから再生PCへコピーして使うためのローカル生成物である。CIは既定で全ビルド・静的解析・ReleaseとASanの試験を実行し、手動実行時だけRelease限定の診断モードを選べる。詳細ログはrunner内の一時領域に保持し、公開ログには合否と試験・失敗箇所の識別情報、数値の比較、既知の例外型と固定した試験操作・エラー番号を出す。例外の詳細やローカルパス、実行物をartifactやReleaseへアップロードしない。

Releaseは `/MT` で標準CRTを静的リンクする。DLLを別に同梱しなくても、生成したEXEにはランタイム由来のコードが含まれる。ソースのみの公開は、このEXEの再配布許諾が確認済みであることを意味しない。

## バイナリを再配布する場合

GPLv3のバイナリをネットワーク経由で配布する場合、対応するソースを追加料金なく同等の方法で取得できるようにする。対象はアプリのソース、ビルドスクリプト、使用したASIO SDKヘッダー等である。GPLv3のSystem Libraryや汎用ビルドツールの除外は、個々の構成物について判定する。[GPLv3 第1条](https://www.gnu.org/licenses/gpl-3.0.en.html#section1)

GPLv3第6条(d)では、対応ソースの一部を第三者のサーバーから取得させることもできる。その場合も、バイナリの配布場所に正確な版の入手先を明示し、同等の取得手段と必要な期間の入手可能性を配布者が維持する。SDKへの一般的な案内だけで責任を果たしたことにはしない。[GPLv3 第6条](https://www.gnu.org/licenses/gpl-3.0.en.html#section6)

GPL側のSystem Library例外と、Microsoft側の静的ランタイム再配布許諾は別に確認する。再配布する構成物、適用するライセンス・再配布対象リスト、必要な表示や同意条件を確認し、自作部分のGPL上の権利を制限しない形で整える。このリポジトリのソース公開は、利用者によるGPL上の再配布を追加禁止するものではない。
