# 依存とライセンス

WAV Playerの公開物は自作のソース一式。ビルドに必要なSDKは各自が取得し、ビルド後の実行ファイルと再生用フォルダーはローカルで使う。

## 開発環境

| 場所 | 必要なもの |
|---|---|
| ビルドPC | Visual Studio 2026安定版、MSVC v145、Windows SDK 10.0.26100.0、Steinberg ASIO SDK 2.3.4。ツールチェーンとSDKはリポジトリには同梱しない。 |
| 再生PC | [動作環境](../README.md#必要な環境)を満たすWindowsと、使う機器のx64 ASIOドライバー。Visual StudioとSDKのインストールは不要。 |

## ASIO SDKの取得

リポジトリ直下で初回に `build.cmd -FetchSdk` を実行する。[取得スクリプト](../scripts/fetch-asio-sdk.ps1)が公式ZIPのSHA-256を照合し、`external/asiosdk/` へ展開する。SDKがすでにあれば `build.cmd` を使う。`-FetchSdk` は既存の展開先を上書きしない。[ビルド手順](../README.md#ビルド)

| 公式ZIP | バイト数 | SHA-256 |
|---|---:|---|
| [ASIO-SDK_2.3.4_2025-10-15.zip](https://download.steinberg.net/sdk_downloads/ASIO-SDK_2.3.4_2025-10-15.zip) | 8,910,208 | `D5EBF0C20DD2C5F43771FD0C1418F4B361BF52434EE670097CFA6B3A335E2ECA` |

手動配置では同じZIPのハッシュを確認し、`ASIOSDK/` の**中身**をコピーする。配置後に `external/asiosdk/common/iasiodrv.h` が見えることを確かめる。ZIPと展開先はGit管理外とし、SDK更新時は取得スクリプト、CI、記載した版とハッシュを同じ変更でそろえる。[SteinbergのSDK配布ページ](https://www.steinberg.net/developers/asiosdk-open/)

## ライセンスの範囲

| 対象 | 適用する条件 |
|---|---|
| 自作のソースコード・スクリプト・文書・設定・ビルド定義 | GNU GPL version 3 only（`GPL-3.0-only`）。[ライセンス本文](../LICENSE) |
| ASIO SDK | このプロジェクトはGPLv3の選択肢を使う。取得したSDKの `LICENSE.txt`、`common/LICENSE.txt` と各ファイルの個別表記を確認し、元の著作権・ライセンス・免責表示を保持する。ASIOはSteinbergの商標で、製品名はWAV Playerとする。 |
| Visual Studio、Windows SDK、Microsoftのランタイム、機器ドライバー | 各権利者の条件に従う。MSVC標準ライブラリのヘッダーなどに個別のライセンス表記があれば、その表記に従う。 |

## ソース公開とローカルビルド

`build.cmd` が作る `out/player/` は、利用者が自分のビルドPCから再生PCへコピーするためのもの。リポジトリやReleaseにSDK・CRT・実行ファイル・再生用フォルダーのZIPは含めない。CIも実行物を配布せず、詳細ログはrunner内に置く。公開ログの範囲と全検証・Release限定診断の違いは[検証](../README.md#検証)を参照する。

## バイナリを再配布する場合

Releaseは `/MT` で標準CRTを静的リンクする。EXEにはランタイム由来のコードが含まれるため、ソースの公開だけで個別のEXEの再配布許諾が確定するわけではない。バイナリを配布する場合は次の条件を分けて確認する。

1. **対応ソースの範囲。** アプリのソース、ビルドスクリプト、使用したASIO SDKヘッダーなど、配布するバイナリに対応するソースを特定する。System Libraryや汎用ビルドツールとして除外できるかは構成物ごとに判断する。[GPLv3 第1条](https://www.gnu.org/licenses/gpl-3.0.en.html#section1)
2. **ネットワークで提供する場所。** GPLv3第6条(d)の方法では、バイナリの配布場所から、対応ソースも追加料金なく同等の手段で入手できるようにする。別のサーバーを使う場合も、正確な版の入手先をバイナリに併記し、配布者が必要な期間の入手可能性を確保する。[GPLv3 第6条](https://www.gnu.org/licenses/gpl-3.0.en.html#section6)
3. **Microsoftの再配布条件。** 静的ランタイムの許諾はGPLのSystem Library例外とは別に、適用されるライセンス、再配布対象リスト、必要な表示・同意条件を確認する。自作部分についてGPLが与える再配布の権利は維持する。[Microsoftの再配布条件の確認方法](https://learn.microsoft.com/en-us/cpp/windows/redistributing-visual-cpp-files)
