# WAV Player

Windows 11 向けの、WAV を x64 ASIO ドライバーへ無損失で送るプレーヤーです。RIFF/WAVE・RF64、PCM16/24/32・Float32、モノ／ステレオに対応します。音量調整・リサンプリング・エフェクトは行いません。

このリポジトリはソースコードを公開します。実行ファイル、ASIO SDK、Windows SDK、機器ドライバー、音源は同梱しません。

## 必要な環境

- 再生PC：Windows 11 25H2 以降の x64、AVX2・BMI1/BMI2・LZCNT・MOVBE・FMA・F16CとOSによるXMM/YMM状態保存、利用する機器の x64 ASIO ドライバー。
- ビルドPC：上記CPU条件、Visual Studio 2026 安定版のC++デスクトップ開発環境。リポジトリの [.vsconfig](.vsconfig) でMSVC v145とWindows SDK 10.0.26100.0を用意します。
- ビルド時のみ、Steinberg ASIO SDK 2.3.4を取得します。[取得先・ハッシュ・ライセンス](docs/dependencies.md)

再生PCにVisual StudioやSDKは不要です。機器の音量を調整してから再生してください。

## ソースの取得

```powershell
git clone https://github.com/ponta2git/wav-player.git
cd wav-player
```

変更をコミットする場合は、各自の公開用の氏名・メールをこのcheckoutに設定します。下の値は自分のものへ置き換えてください。メールには、GitHubのアカウント設定で確認した自分のnoreplyメールを使えます。既存checkoutのlocal設定はcloneへ引き継がれず、Gitの認証アカウントとコミットに記録される氏名・メールも別です。

```powershell
git config --local user.name "YOUR_PUBLIC_NAME"
git config --local user.email "YOUR_PUBLIC_EMAIL"
git var GIT_AUTHOR_IDENT
git var GIT_COMMITTER_IDENT
```

両方の表示を確認してからコミットします。以下のビルド・検証コマンドはリポジトリ直下で実行してください。

## ビルドと検証

初回はビルドPCでリポジトリ直下の `build.cmd -FetchSdk` を実行します。`-FetchSdk` はASIO SDKを新規取得する指定です。

```powershell
.\build.cmd -FetchSdk
```

固定したASIO SDKを取得・検証し、Release x64をビルドして `out/player/` に再生用フォルダーを作ります。SDK配置済みでは `-FetchSdk` を付けず `build.cmd` を使います。既存の出力先は上書きしないため、再ビルド時は `build.cmd -OutputDirectory "新しい保存先"` を指定してください。ビルドは実機の初期化や再生を行いません。

開発時は [WAV Player.sln](WAV%20Player.sln) をVisual Studioで開き、Debug／Release x64をビルドできます。製品本体は静的CRTを使用します。

全検証は次のコマンドで行います。ドライバーの登録や機器の接続は不要です。

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\verify.ps1 -Analyze -IncludeASan
```

プロジェクト構成・設定の監査、Debug／Releaseの静的解析付きビルド、ReleaseとDebug-ASanの模擬ASIO試験、Release成果物の監査を実行します。GitHub Actionsは `scripts/ci.ps1` から公開対象の監査、PowerShellの回帰試験、SDK取得、同じ全検証を実行します。手動実行の `release_only`（既定はfalse）は、runner上の失敗を再現するためにReleaseだけをビルド・試験する診断モードです。静的解析とASanを省くため、全検証の代わりにはなりません。

詳細ログはrunner内の一時領域に保持し、公開ログには合否、試験名、失敗したケース名・ソースのファイル名と行番号・数値の期待値と実測値を出します。既知の例外型と、固定したGUI試験操作の識別子・メッセージ番号・Win32エラー番号も表示します。例外の詳細やローカルパスは出しません。実行ファイルのartifactやReleaseは公開しません。[試験仕様](docs/test-specification.md)

## 再生PCで使う

1. ビルドPCの `out/player/` を、再生PCの書き込み可能なフォルダーへフォルダーごとコピーします。
2. 再生PCで `setup.cmd` を開き、登録済みASIOドライバーと音源フォルダーを指定します。設定とUTF-8のプレイリストを作り、プレーヤーを停止状態で起動します。
3. 表示された機器・出力・曲一覧を確認し、Playを押します。

同じPCでビルドした場合も `out/player/setup.cmd` を使います。既存の設定を扱う場合の確認や更新方法は、再生用フォルダーの [README](packaging/player/README.txt) を参照してください。

`settings.ini` と `playlist.txt` は実行ファイルの隣に置き、起動時に読みます。手動編集する場合はアプリを終了し、[設定例](settings.example.ini) と [設定の仕様](docs/requirements.md#設定と診断) を参照してください。プレイリストは1行1ファイルのローカル絶対パスです。

| 操作 | 動作 |
|---|---|
| Play/Pause・Space | 再生、一時停止、再開 |
| Stop・Escape | 再生を打ち切り、位置を先頭へ戻す |
| 曲のダブルクリック・リスト上でEnter | 選択曲の先頭から再生 |

同一レートで無損失に出力できる曲は連続再生します。異レートの曲間には切替のための無音があります。停止・再開時の波形の不連続によるクリックは許容する仕様です。シーク、リピート、シャッフル、GUIでの機器選択はありません。

入力形式を無損失で保持できないASIO出力では再生を拒否します。サンプル一致の保証範囲はWAVの値からASIO出力バッファーまでで、DACの物理出力や音質を保証するものではありません。[対応形式と制限](docs/requirements.md#入力と出力の一致)

## 開発文書

- [製品要件](docs/requirements.md)：対応範囲と受け入れ条件
- [アーキテクチャ](docs/architecture.md)：責務、依存方向、スレッド、再生経路
- [ASIO契約](docs/asio-contract.md)：SDKの規定とホスト側の方針
- [試験仕様](docs/test-specification.md)：自動検証と実機の確認手順
- [依存とライセンス](docs/dependencies.md)：SDKの取得、公開範囲、第三者条件

通常の開発は作業ブランチとPRで進めます。最新の `main` に追従したPRで `Full verification`（静的解析・Release・ASanを含む全検証）が成功してから、所有者が差分を確認してマージします。承認レビュー数は0です。手動の `Release diagnostics` は失敗を調べるための診断です。

文書の訂正など軽微な変更は、所有者本人の判断で `main` に直接pushできます。その場合は変更が先に反映され、続いて `Full verification` が実行されるため、所有者が結果を確認します。この例外はアカウント単位で、軽微かどうかは所有者が判断します。エージェントによる変更はPRを経由します。`main` の削除禁止・force push禁止・直線履歴の要件は所有者にも適用されます。

外部からのPRとIssuesは受け付けていますが、採用・対応・対応期限は約束していません。

`docs/` には現行の仕様・設計・試験手順を置きます。実験メモは `private/notes/`、途中の実験仕様は `private/experiments/`、実機記録は `private/results/`、公開準備の監査記録は `private/publication/`、生成物は `out/` に置きます。`private/` と `out/` はGitの追跡対象外です。

コミット前には `git status`、`git diff`、`git diff --cached` で変更と追加対象を確認し、次を実行します。ファイルの追加・削除では [公開対象一覧](build/public-files.txt) とVisual Studio登録を同時に更新してください。

```powershell
git diff --check
git diff --cached --check
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\check-public-source.ps1 -Tracked
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\check-project-layout.ps1
```

`.gitignore` と公開対象一覧は追跡するファイルを管理します。本文・コメント・コミット情報・過去の履歴にある個人情報や秘密情報を網羅的に検出するものではありません。push前には、新たに送るコミットの差分・氏名・メール・メッセージも確認してください。実験の実測値や作業経緯はコメントにも残さず `private/` に保存し、公開する説明は現行の契約・設計意図・検証手順に絞ります。

## バージョンとタグ

公開版の区切りには、注釈付きタグ `vメジャー.マイナー.パッチ` を付けます。`0.x` は互換性をまだ固定しない初期開発段階です。この間は互換性を保つ不具合修正でパッチを、機能追加や互換性を変える変更でマイナーを上げ、互換性への影響をPRで説明します。設定形式・起動引数・基本的な利用方法の互換性を維持すると判断した時点で `1.0.0` に進めます。以後も互換性を保つ不具合修正はパッチ、機能追加はマイナーとし、互換性を壊す変更でメジャーを上げます。

Windowsの製品バージョンは `メジャー.マイナー.パッチ.0` とし、[リソース](src/app/wavplay.rc) と [マニフェスト](build/app.manifest) を同じPRで更新します。例えばタグ `v0.1.0` は製品バージョン `0.1.0.0` に対応します。各コミットへのタグ付けは不要で、文書だけの変更も通常は新しい公開版にしません。

公開版を作る手順は次のとおりです。

1. 版番号と変更説明を含むPRの `Full verification` が成功してから、所有者がレビューして `main` へマージします。
2. タグ対象をレビュー済みの `main` のコミットに固定し、そのSHAに対する `push` の `Full verification` 成功を確認します。PRの検証結果や手動の `Release diagnostics` だけでは代用しません。現在のCIはタグpushでは起動しません。
3. 所有者が公開する版を決め、対象ソースの版番号・公開対象・既存タグとの重複を確認します。作成前に `git var GIT_COMMITTER_IDENT` でタグに記録する公開用の氏名・メールを確認し、対象の完全なコミットSHAを指定して注釈付きタグを作ります。注釈は版と公開する変更点に絞ります。
4. 作成したタグ1つだけをpushし、リモートのタグが意図したコミットを指すことを確認します。

`v*` タグは所有者だけが作成できるよう保護し、更新・削除には例外を設けません。この保護はタグ対象の選定やCI成功の確認を代行しないため、上記の手順で照合します。公開済みタグは移動・削除・再利用せず、修正は新しい版にします。

公開物はソース・文書・スクリプトです。GitHub Releaseは必須にせず、作る場合も変更説明とGitHubのソースアーカイブだけを使い、SDK・完成バイナリ・実機記録は添付しません。

## ライセンス

このリポジトリの自作ファイル（ソースコード、スクリプト、文書、設定・ビルド定義）は **GNU GPL version 3 only（SPDX: GPL-3.0-only）** で提供します。[ライセンス本文](LICENSE)。第三者のSDK・ツールチェーン・ランタイムには、それぞれの権利者の条件が適用されます。

公開物はソースのみです。ローカルで生成したバイナリを再配布する場合は、GPLの対応ソース提供義務と、組み込むランタイム等の再配布条件を確認してください。この説明はGPLが認める利用・改変・再配布の権利に追加制限を設けるものではありません。
