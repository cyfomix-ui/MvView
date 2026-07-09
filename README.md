# MvView v0.20 First Plot

MvView は、MvSuite の MvExpHover / MvHover を C++ / Win32 の単独常駐アプリへ切り出すためのファーストプロットです。

## 今回の範囲

- Windows 11 / C++ / Win32 常駐アプリ
- タスクトレイ常駐
- Explorer が前面のときだけ監視
- Explorer で選択された画像・動画・音声をプレビュー
- 監視の主駆動は `SetWinEventHook` による Windows イベント
- タイマーは選択イベント後の短い debounce と、プレビュー表示中のカーソル距離チェックのみ
- プレビューは `WS_EX_NOACTIVATE | WS_EX_TOPMOST | WS_EX_TOOLWINDOW`
- 設定・ログは `%APPDATA%\MvView` 配下
- レジストリは使わない
- `--open "path"` で MvSticky 等から直接呼び出し可能
- アイコンは明るい黄色系の MvView アイコン


## v0.20 の変更

- シングル選択後、待機時間中のマウス移動イベントがクリック/選択イベントを上書きして再生されない問題を修正。
- 複数プレビュー再生中に別メディアを選択した場合、旧プレビューを即停止して新しい選択の待機に入るよう修正。

## v0.20 の変更

- Windows 11 Explorer のタブ表示で、1つ目のタブの選択ファイルが別タブへ持ち越される問題を軽減。
  - `IShellWindows` の候補を即 `break` せず、同じ Explorer ルート HWND に紐づく候補を全走査。
  - Active Explorer のタイトルと `LocationName` を照合して、現在表示中のタブ候補を優先。
  - `FocusedItem` は引き続き診断ログ専用。
- クリック選択後、待機時間中にマウスを少し動かすとプレビューが開始されない問題を修正。
  - クリック/選択イベント由来のプレビューは、カーソルが Explorer 内にある限り開始許可。
  - マウス移動イベント由来の再開だけ距離制限を残し、古い選択の暴発を抑制。
- ExplorerReader の候補ログを強化。
  - foreground title
  - candidate index / LocationName / selected count / score
  - active candidate index

設定ダイアログとプレビュー操作系は引き続き以下を持っています。

- トレイメニューの「設定」で設定ダイアログを表示
- Preview 解像度: `720p / 480p / 360p / 240p / 180p`
- 待機時間: `0.0 ～ 5.0秒`、デフォルト `1.2秒`
- 起動時音声: デフォルトOFF
- プレビュー枠色: 明るい黄色、アクア、グリーン、オレンジ、ピンク、ホワイト、パープル
- プレビュー右上に音声ありメディアの `♫` マークを表示
- 下部に再生バー、左に現在位置、右にメディア長を表示
- 右下の音量マークから縦スライダーで音量変更
- プレビュー画面クリックで一時停止/再生
- ホイールで再生位置を前後
- Ctrl+ホイールで一時的に解像度変更
- `loop-file=no` に変更し、ループ再生しない
- Explorer で複数選択されている場合、最大9件まで同時プレビュー

## 依存関係

ビルド時依存:

- Visual Studio 2022
- Desktop development with C++ workload
- Windows SDK

実行時依存:

- `mpv-2.dll` または `libmpv-2.dll`
- 上記 DLL が必要とする依存 DLL 群

このファーストプロットは libmpv を動的ロードします。つまり、ビルド時に mpv のヘッダや `.lib` は不要です。
v0.20 では `mpv-2.dll` が無くても MvView 自体はトレイ起動し、Tooltip に `MvView v0.20 / mpv missing` を出します。

また、Explorer の選択イベントだけでは拾えないクリックがあるため、v0.20 では低レベルマウスフック（`WH_MOUSE_LL`）を併用し、Explorer 上のクリック後に選択状態を再確認します。トレイ Tooltip は `NIF_SHOWTIP` を付けて、Windows 11 でも標準ツールチップが出やすいようにしています。
探索パスと `LoadLibrary` の失敗理由は `%APPDATA%\MvView\logs\MvView.log` に出力します。

探索対象:

```text
MvView.exe と同じフォルダの mpv-2.dll
MvView.exe と同じフォルダの libmpv-2.dll
MvView.exe と同じフォルダの runtime\mpv-2.dll
MvView.exe と同じフォルダの runtime\libmpv-2.dll
PATH 上の mpv-2.dll / libmpv-2.dll
```

## ビルド

PowerShell:

```powershell
Set-ExecutionPolicy -Scope Process Bypass -Force
.\build.ps1 -Configuration Release
```

出力:

```text
bin\x64\Release\MvView.exe
```

## 起動

```powershell
.\bin\x64\Release\MvView.exe
```

起動するとトレイに常駐します。
Explorer で `.mp4`, `.mkv`, `.jpg`, `.png`, `.mp3`, `.flac` などを選択すると、少し待ってからカーソル近くにプレビューが出ます。

## 設定ファイル

初回起動時に以下が作成されます。

```text
%APPDATA%\MvView\settings.json
```

主な設定:

```json
{
  "enabled": true,
  "previewDelayMs": 1200,
  "previewResolutionP": 360,
  "cursorFarClosePx": 220,
  "closeWhenForegroundLost": true,
  "closeWhenSelectionEmpty": true,
  "closeOnNonMedia": true,
  "audioOnStart": false,
  "volumePercent": 50,
  "borderColorIndex": 0,
  "registerEscapeWhilePreviewVisible": true
}
```

## MvSticky から呼ぶ想定

```powershell
MvView.exe --open "D:\media\sample.mp4"
```

既に MvView が常駐している場合、2つ目のプロセスは既存 MvView へ `WM_COPYDATA` でパスを渡して終了します。

## 現時点の注意点

- 設定ダイアログを追加していますが、詳細設定は引き続き JSON でも編集できます。
- Explorer の「未選択のカーソル直下アイテム」取得は今回入れていません。第1段階は選択中ファイル方式です。
- Explorer 側が選択イベントを出さない状況では、トレイ左クリックで再チェックできます。
- `mpv-2.dll` / `libmpv-2.dll` が見つからない場合でも、アプリはトレイ常駐します。Tooltip とログで状態を確認してください。
- DLL が存在しても依存DLL不足なら `LoadLibrary failed ... error=126` などがログに出ます。mpv runtime の DLL 群を同じ場所にコピーしてください。
- グローバル ESC はプレビュー表示中だけ登録します。不要なら `registerEscapeWhilePreviewVisible` を false にしてください。

## ログ

```text
%APPDATA%\MvView\logs\MvView.log
```


## Version management

- Current version: `v0.20`
- Increment policy: increase by `0.01` per source/release (`v0.01`, `v0.02`, `v0.03`, `v0.04`, `v0.05`, `v0.06`, `v0.07`, `v0.08`, `v0.09`, `v0.10`, `v0.11`, `v0.12`, `v0.13`, `v0.14`, `v0.15`, `v0.16`, `v0.17`, `v0.18`, `v0.19`, `v0.20`, ...).
- Tray tooltip shows `MvView v0.20 / mpv ok` or `MvView v0.20 / mpv missing`, matching the MvSticky style.
- Version should be aligned in `resource.h`, `MvView.rc`, tray tooltip/About text, README, and package name.


## Visual Studio toolset note

`build.ps1` auto-detects the installed C++ PlatformToolset such as `v143` or `v180`. You can override it with `-PlatformToolset v143` or `-PlatformToolset v180` when needed.
