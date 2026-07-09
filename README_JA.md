# MvSuite

[日本語](./README.md) | [English](./README_EN.md)

**MvSuite** は、動画・画像・音声素材を扱うための Windows 向けメディア作業支援ツール群です。

素材を集める、確認する、並べる、簡易再生する、外部ツールへ渡す、FFmpegで出力する、といった作業をできるだけ軽く行うことを目的にしています。

> Status: 開発中 / Experimental  
> Platform: Windows 11  
> Repository: https://github.com/cyfomix-ui/MvSuite

---

## 主な特徴

- 動画・画像・音声ファイルを中心に扱うメディア作業支援スイート
- Explorer やアプリ内リストからのプレビュー再生
- 複数メディアの同時プレビュー再生
- メディア素材を一時的に集めて管理する Stocker 系機能
- 画像・動画・音声をドラッグ＆ドロップで扱う軽量プレイヤー系機能
- FFmpeg を利用した動画出力・結合・変換支援
- DaVinci Resolve / Shotcut など外部編集ツールへ渡す前段階の素材整理
- 付箋・メモ・メディア貼り付け用途の MvSticky との連携を意識した設計
- 可能な範囲でイベント駆動を優先し、常時タイマー監視に依存しすぎない方針

---

## 含まれる主なアプリ / 機能

### MvStocker / Stocker

動画・画像・音声素材を集めて、順番確認や簡易再生を行うための機能です。

主な用途:

- 素材候補の一時保管
- 再生順の確認
- 簡易的な動画出力前の並び確認
- FFmpeg 出力処理への橋渡し

### MvHover / MvView 系プレビュー

メディアファイルを選択したときに、その場で小さなプレビューウィンドウを表示する機能です。

主な用途:

- Explorer 上での素早い動画・画像確認
- アプリ内リスト上でのメディア確認
- 複数ファイルの同時プレビュー
- 素材選別時の確認作業短縮

### DropMp3 / DropMp4

ファイルをドラッグ＆ドロップして再生する軽量プレイヤー系ツールです。

主な用途:

- 音声ファイルの素早い確認
- 動画ファイルの素早い確認
- プレイリスト的な簡易再生
- ローカルメディア確認用の小型プレイヤー

### MvSticky 連携

MvSticky は付箋・メモにメディアを貼り付ける用途の別アプリです。MvSuite では、メディアプレビューや素材確認の考え方を MvSticky と共有できるように設計しています。

---

## 必要環境

基本環境:

- Windows 11
- Python 3.x
- PowerShell 7 または Windows PowerShell
- FFmpeg

追加機能で必要になる可能性があるもの:

- mpv / libmpv runtime
- Visual Studio / Build Tools
- .NET SDK

FFmpeg を使用する機能では、`ffmpeg.exe` が PATH に通っているか、アプリ側で参照できる場所に配置されている必要があります。

---

## 使い方

Release から利用する場合:

1. GitHub の Releases から最新版をダウンロード
2. Zip を任意のフォルダへ展開
3. 起動用スクリプトまたは EXE を実行
4. 必要に応じて FFmpeg / mpv runtime を配置

ソースからビルドする場合:

```powershell
Set-ExecutionPolicy -Scope Process Bypass -Force
.\build.ps1 -Publish
```

管理者権限での実行は、必要な場合を除き推奨しません。

---

## フォルダ構成の考え方

現在のソース入口は `apps/` 配下を基準にしています。

```text
apps/
  launchers/   起動スクリプト
  shared/      共通コード
  tools/       補助処理

docs/
  設計メモ、作業記録、構成資料

version.xml
  アプリVersion管理

build.ps1
  ビルド / パッケージ作成
```

---

## Version 管理

Version 情報は `version.xml` を正として管理します。

表示対象:

- ウィンドウタイトル
- タスクトレイ Tooltip
- About 画面
- Splash 画面
- 起動ログ
- README
- ビルド出力名
- Zip / Release ファイル名

Version は `0.01` から開始し、更新ごとに `0.01` ずつ増やす方針です。

---

## ライセンスと注意

このプロジェクトは開発中の個人制作ツールです。

- 無保証です。
- 利用は自己責任でお願いします。
- メディアファイルの利用権利は利用者側で確認してください。
- FFmpeg / mpv / libmpv などを同梱する場合は、それぞれのライセンス表記に従ってください。

---

## 開発方針

MvSuite は、重い編集ソフトの代替ではなく、素材確認・素材整理・簡易出力・外部編集ツールへの橋渡しを軽く行うためのツール群です。

特に以下を重視しています。

- すぐ確認できること
- 小さく起動できること
- 複数メディアを同時に見比べられること
- Explorer や既存ワークフローを邪魔しないこと
- 設定やVersion管理を分かりやすく保つこと

---

## Repository

https://github.com/cyfomix-ui/MvSuite
