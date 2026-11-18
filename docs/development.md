# WAV Player の開発手順

この文書は変更の検証、PRの流れ、コミット前の確認、版とタグの手順を定める。ビルド環境と再生までの手順は [README](../README.md)、層の責務は[再生設計](architecture.md)、検証項目と合否の基準は[試験仕様](test-specification.md)を参照する。

## リポジトリの構成

| パス | 内容 | Git |
|---|---|---|
| `src/app/` | 製品の入口（`wavplay.exe`） | 追跡 |
| `src/wavplay/<層>/` | 層ごとの製品コード。製品と試験が同じオブジェクトをリンクする | 追跡 |
| `tests/wavplay_tests/` | 自動試験と模擬ASIO（`wavplay_tests.exe`） | 追跡 |
| `scripts/`、`build/` | ビルド・検証のスクリプト、共通のビルド設定、公開対象一覧 | 追跡 |
| `packaging/player/` | 再生用フォルダーに入れるセットアップ一式 | 追跡 |
| `docs/` | 現行の要件・設計・契約・試験手順・開発手順 | 追跡 |
| `external/asiosdk/` | 取得したASIO SDK | 対象外 |
| `out/` | ビルドの生成物と再生用フォルダー | 対象外 |
| `private/` | 実験と実機の記録。`notes/`（実験メモ）、`experiments/`（途中の実験仕様）、`results/`（実機記録）、`publication/`（公開準備の監査記録） | 対象外 |

Visual Studioでは [WAV Player.sln](../WAV%20Player.sln) を開き、Debug／Release x64をビルドする。

実験の実測値や作業の経緯は、コメントを含む公開ファイルに書かず `private/` に置く。公開する説明は、現行の契約・設計意図・検証手順に絞る。

## 検証

全検証はリポジトリ直下で次のコマンドを実行する。ドライバーの登録や機器の接続は不要。

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\verify.ps1 -Analyze -IncludeASan
```

| 段階 | 内容 |
|---|---|
| 構成の監査 | ソースの登録、層の依存、文書のリンク、評価済みのビルド設定 |
| ビルド | Debug・Release（静的解析付き）、Debug-ASan |
| 成果物の監査 | マニフェスト、版情報、ReleaseのPEと機械語 |
| 試験 | ReleaseとDebug-ASanで、模擬ASIOを使う全試験群 |

試験群を個別に実行するには、`out\x64\Release\wavplay_tests.exe --list` で名前を調べ、`--suite 群名` を指定する。

GitHub ActionsはPRと `main` へのpushで全検証を実行し、結果を `Full verification` に示す。Release側では公開対象の監査、PowerShellスクリプトの回帰試験、製品EXEの起動確認も行う。手動実行の `release_only` は、静的解析とASanを省いてReleaseだけを調べる `Release diagnostics` で、失敗の調査に使う。

CIの公開ログは合否と失敗箇所に絞るため、詳細は同じコマンドをローカルで実行して調べる。ランナーの分担と公開ログの範囲は[試験仕様](test-specification.md#ローカルとciの全検証)を参照する。

## 変更の流れ

| 変更 | 経路 | 反映の条件 |
|---|---|---|
| 通常の変更（エージェントによる変更を含む） | 作業ブランチからPR | 最新の `main` に追従したPRで `Full verification` が成功し、所有者が差分を確認してマージする |
| 所有者が軽微と判断した変更（文書の訂正など） | 所有者が `main` へ直接push | pushの後に実行される `Full verification` の結果を所有者が確認する |

`main` の削除とforce pushは禁止で、直線履歴を保つ。この規則は所有者にも適用する。承認レビューは必須にしない。

外部からのPRとIssueを受け付ける。採否と対応時期は所有者が判断する。

## コミット前の確認

コミットに記録する氏名とメールは公開される。cloneごとに公開用の値を設定し、表示を確認してからコミットする。メールにはGitHubのnoreplyアドレスを使える。

```powershell
git config --local user.name "YOUR_PUBLIC_NAME"
git config --local user.email "YOUR_PUBLIC_EMAIL"
git var GIT_AUTHOR_IDENT
git var GIT_COMMITTER_IDENT
```

`git status`、`git diff`、`git diff --cached` で変更と追加対象を確認し、次を実行する。

```powershell
git diff --check
git diff --cached --check
# Gitが追跡するファイルと公開対象一覧の一致
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\check-public-source.ps1 -Tracked
# Visual Studioへの登録、層の依存、文書のリンク
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\check-project-layout.ps1
```

- ファイルを追加・移動・削除するときは、[公開対象一覧](../build/public-files.txt)とVisual Studioの登録を同じ変更で更新する。
- スクリプトが確認するのはファイルの構成である。本文とコメントの内容、コミットの氏名・メール・メッセージは、コミット前とpush前に自分で確認する。

## バージョンとタグ

公開版の区切りには、注釈付きタグ `vメジャー.マイナー.パッチ` を付ける。

| 段階 | パッチ | マイナー | メジャー |
|---|---|---|---|
| `0.x`（互換性を固定しない初期開発） | 互換性を保つ不具合修正 | 機能追加、互換性を変える変更 | — |
| `1.0.0` 以降 | 互換性を保つ不具合修正 | 機能追加 | 互換性を壊す変更 |

- `0.x` の間は、互換性への影響をPRで説明する。設定形式・起動引数・基本的な利用方法の互換性を維持すると判断した時点で、`1.0.0` に進める。
- Windowsの製品バージョンは `メジャー.マイナー.パッチ.0` とする（タグ `v0.1.0` は `0.1.0.0`）。[リソース](../src/app/wavplay.rc)と[マニフェスト](../build/app.manifest)を同じPRで更新する。
- 文書だけの変更では、通常は版を上げない。

公開版は次の手順で作る。

1. 版番号の更新と変更説明を含むPRを、`Full verification` の成功後に所有者がレビューして `main` へマージする。
2. レビュー済みの `main` のコミットをタグの対象に決め、PR上の結果ではなく、そのSHAに対する `push` の `Full verification` の成功を確認する。CIはタグのpushでは起動しないため、この結果がタグ対象の検証結果になる。
3. 所有者が公開する版を決め、対象ソースの版番号、公開対象、既存タグとの重複を確認する。`git var GIT_COMMITTER_IDENT` でタグに記録する氏名・メールを確かめ、対象コミットの完全なSHAを指定して注釈付きタグを作る。注釈には版と公開する変更点だけを書く。
4. 作成したタグ1つだけをpushし、リモートのタグが対象のコミットを指すことを確認する。

`v*` タグを作成できるのは所有者だけで、更新と削除は例外なく禁止する。公開済みのタグは移動・再利用せず、修正は新しい版として公開する。

GitHub Releaseの作成は任意。作る場合も、変更説明とGitHubのソースアーカイブだけを使い、SDK・バイナリ・実機記録は添付しない。
