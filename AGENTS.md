# AGENTS.md

## 実装と検証

- [要件](docs/requirements.md)と[設計](docs/architecture.md)の該当箇所を読み、依頼の範囲で層の責務を保つ簡潔な実装を選ぶ。挙動の変更を求められたら、その要求を受け入れ条件とし、実装・試験をそれに合わせる。
- 独立して進められる作業はサブエージェントと分担する。主担当は担当範囲と期待する結果を伝え、成果を統合する。
- [試験仕様](docs/test-specification.md)と受け入れ条件から必要な検証を選び、主担当が成功を確認する。変更による失敗は修正して再検証する。完了できない場合は失敗と未実行を区別し、理由を報告する。

## 公開範囲とファイル登録

- `docs/` は現行の要件・設計・契約・試験手順に限る。実験・試聴・作業記録は用途別に `private/`、生成物と移動用フォルダーは `out/`、SDKは `external/asiosdk/` に置き、Gitの対象外とする。利用者の設定もローカルに保つ。[記録の保存先](README.md#開発文書)
- ソースコメント・設定例を含む公開ファイルに、実機名・機器固有の識別子・個人パス・実験の実測値・実験記録へのリンクを載せない。
- 公開ファイルの追加・移動・改名・削除では、[公開対象一覧](build/public-files.txt)と該当するVisual Studioの登録を同じ変更で更新する。
  - 製品と試験で共有するソース・ヘッダーは `build/WavplaySources.props`
  - プロジェクト専用のファイルは各 `.vcxproj`
  - 開発・配布用の文書・設定・スクリプトは `WAV Player.sln` の Solution Items
- ソースの書き出しには `scripts/export-source.ps1` を使い、一覧のファイルだけを新しいディレクトリへ出力する。

## コミット・PR・タグ

- レビューしやすい単位でコミットする。コミット前に差分・追加対象を点検し、`scripts/check-public-source.ps1 -Tracked` を通す。`git var GIT_AUTHOR_IDENT` と `git var GIT_COMMITTER_IDENT` の氏名・メールを、このcheckoutの `git config --local` で指定した公開用の値と照合する。
- push前に、送る全コミットの差分・氏名・メール・メッセージを点検する。軽微な変更も作業ブランチからPRにし、最新の `main` に追従したPRで `Full verification` の成功を確認する。PRと検証結果をユーザーに渡す。`main` への直接push・マージはユーザー本人に限る。
- 版番号の更新とタグ公開は[READMEの版管理手順](README.md#バージョンとタグ)に従う。タグはユーザー指定の版に限り、レビュー済みの `main` に含まれる対象コミットの完全なSHAで `push` の `Full verification` が成功してから公開する。
