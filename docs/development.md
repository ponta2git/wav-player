# WAV Player の開発手順

この文書は記録の保存先、変更の流れ、コミット前の確認、版とタグの手順を定める。ビルド環境と再生までの手順は [README](../README.md)、コードの配置は[再生設計](architecture.md#現在のコード配置)を参照する。

検証の方法は試験仕様を正本とする。全検証のコマンド、CIのジョブ構成、試験群の個別実行は[試験仕様](test-specification.md#ローカルとciの全検証)、変更した境界ごとの確認事項は[再生設計](architecture.md#品質特性と変更時の確認)を参照する。

## 記録の保存先

`docs/` には現行の要件・設計・契約・試験手順・開発手順だけを置く。次の場所はGitの対象外とする。

| パス | 内容 |
|---|---|
| `private/notes/` | 実験メモ |
| `private/experiments/` | 途中の実験仕様 |
| `private/results/` | 実機記録 |
| `private/publication/` | 公開準備の監査記録 |
| `out/` | ビルドの生成物と再生用フォルダー |
| `external/asiosdk/` | 取得したASIO SDK |

実験の実測値や作業の経緯は、コメントを含む公開ファイルに書かず `private/` に置く。公開する説明は、現行の契約・設計意図・検証手順に絞る。

## 変更の流れ

エージェントに実装・修正を依頼した場合は、変更内容とローカルでの検証結果の報告で作業が完了する。作業ブランチのpushやPR作成まで依頼する場合は、その旨を明示する。

変更を `main` に反映する際は、次の手順に従う。

| 変更 | 経路 | 反映の条件 |
|---|---|---|
| 通常の変更（エージェントによる変更を含む） | 作業ブランチからPR | 最新の `main` に追従したPRで `Full verification` が成功し、所有者が差分を確認してマージする |
| 所有者が軽微と判断した変更（文書の訂正など） | 所有者が `main` へ直接push | pushの後に実行される `Full verification` の結果を所有者が確認する |

`Full verification` は、GitHub Actionsが実行する全検証のチェック名である。

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

`git status`、`git diff`、`git diff --cached` で変更と追加対象を確認し、次を実行する。2つのスクリプトの判定条件は[ソース登録とビルド設定](test-build.md#ソース登録とビルド設定)を参照する。

```powershell
git diff --check
git diff --cached --check
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\check-public-source.ps1 -Tracked
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

GitHub Releaseの作成は任意。作る場合は変更説明とGitHubのソースアーカイブだけを使う。[公開物の範囲](dependencies.md#ソース公開とローカルビルド)
