# MvView v0.25

MvView は、Windows 11 Explorer のファイル一覧でメディア項目へカーソルを置くと、libmpv を使って画像・動画・音声を小窓プレビューする C++ / Win32 常駐アプリです。

## v0.25 の変更概要

- `ComPtr<T>` のムーブ代入演算子で、独自 `operator&()` が誤って呼ばれ `T**` になっていたコンパイルエラーを修正
- 自己代入判定に `std::addressof()` を使用し、COM出力引数用の `operator&()` と通常のアドレス取得を明確に分離
- ホバー起点プレビュー、Explorerタブ判定、UI Automation、複数選択確認など v0.22 の機能は変更せず維持
- Versionを `v0.25` へ更新
- MvFiler等の外部アプリ向けに、世代管理付きUTF-16 JSON `WM_COPYDATA` Hover IPCを追加
- 外部Hover状態をExplorer Hoverおよび従来の `--open` から分離

## v0.22 の変更概要

v0.21 までの「クリック／選択イベントを待って SelectedItems を再生する方式」を廃止し、Explorer 項目へのホバーを起点とする方式へ変更しました。

- Explorer が前面にあるときだけホバーを監視
- UI Automation の `ElementFromPoint` から `DataItem` / `ListItem` を取得
- 現在の Explorer タブの `IShellView` / `IFolderView2` / `IShellItem` と照合して完全なファイルパスを確定
- 表示名だけを現在フォルダーへ単純連結する処理は不使用
- パスを一意に確定できない場合は再生しない
- 詳細、一覧、大アイコン、特大アイコン表示を考慮
- ナビゲーションペイン、アドレスバー、フォルダー、余白、非メディアでは開始しない
- 同じ項目内でマウスが少し動いても待機時間をリセットしない
- ホバー項目が変わった場合だけ待機をキャンセルして再開始
- 対象項目から外れた場合、原則 100ms 以内に停止
- プレビューウィンドウは対象項目を覆いにくい位置へ配置
- Ctrl / Shift による複数選択では自動再生せず、確認画面を表示
- 複数確認後は従来どおり最大9件をタイル再生
- `allowMouseReturnStart_` による自動再開を廃止
- `--open "path"` は `DirectOpen` としてホバー条件から独立
- Version を `v0.22` へ更新

## 動作の流れ

### 単一メディア

1. Explorer のファイル一覧でメディア項目へカーソルを置きます。
2. 設定された「ホバー開始までの時間」だけ同じ項目内にとどまります。
3. 対象項目の完全なパスを UI Automation と Shell View の両方で確認します。
4. 一意に確定できた場合だけプレビューを開始します。
5. 対象項目から外れると停止します。
6. 「プレビュー上への移動を許可」が有効な場合は、プレビューウィンドウ上へカーソルを移動して操作できます。

クリック、選択変更、フォーカスイベントだけではプレビューを開始しません。

### 複数選択

1. Ctrl または Shift で複数のメディアを選択します。
2. 選択中メディアのいずれかへ設定時間だけカーソルを置きます。
3. 次の非モーダル確認画面を表示します。

```text
3個のメディアをプレビューしますか？
[再生] [キャンセル]
```

- ［再生］または Enter: 最大9件をタイル再生
- ［キャンセル］または Esc: 開始しない
- 選択内容が変わった場合: 確認画面と再生を終了
- 同じ選択内容では、項目から完全に離れるまで確認を繰り返さない
- 再生後に対象項目・確認画面・プレビューのどこにもカーソルがない場合は停止
- 同じ選択内容をマウス移動だけで自動再開しない

## 明示的な状態管理

ホバー処理は文字列 reason の部分一致ではなく、次の状態で管理します。

```cpp
enum class PreviewTrigger {
    None,
    HoverSingle,
    HoverMultiConfirmed,
    DirectOpen
};

enum class HoverState {
    Idle,
    WaitingSingle,
    WaitingMulti,
    ConfirmingMulti,
    PlayingSingle,
    PlayingMulti
};
```

保持する主な情報:

- 現在ホバー中の完全なファイルパス
- UI Automation 項目の画面矩形
- ホバー開始時刻と開始位置
- Explorer ルート HWND
- Shell View / タブ識別キー
- 複数選択の安定したキー
- 確認中・再生中のメディア一覧
- 確認抑止キー
- 再生開始直後の退出判定猶予時刻

## マウス監視と負荷対策

- `WH_MOUSE_LL` コールバックは座標とイベント種別を `PostMessage` するだけです。負座標を含むマルチモニターでも欠落しないよう、x64 の `LPARAM` に32bit座標を保持します。
- COM、UI Automation、Shell 列挙はフックコールバック内で実行しません。
- マウス移動通知は約50msで間引きます。
- 待機中・確認中・再生中だけ約50ms間隔でホバー状態を確認します。
- カーソルが現在の項目矩形内にある間は、UI Automation / Shell の再列挙をキャッシュで抑制します。
- Explorer タブの変化を検出するため、クリック／選択／フォーカスイベントでは再生を開始せず、キャッシュ無効化と既存プレビューのタブ・選択検証だけを行います。
- 項目から確認画面／プレビューへ移る短い経路は専用の移動回廊として扱い、「プレビュー上への移動を許可」を実用的にします。
- マウス移動ごとのログ出力は行いません。

## 対応メディア

画像:

```text
.jpg .jpeg .png .webp .bmp .gif .tif .tiff
```

動画:

```text
.mp4 .mkv .mov .avi .webm .m4v .wmv .flv .ts .mts .m2ts .mpg .mpeg
```

音声:

```text
.mp3 .wav .flac .m4a .aac .ogg .opus .wma .aiff
```

## 設定

設定ファイル:

```text
%APPDATA%\MvView\settings.json
```

v0.22 で追加した設定:

```json
{
  "hoverPreviewEnabled": true,
  "hoverPreviewDelayMs": 800,
  "stopImmediatelyOnHoverLeave": true,
  "confirmMultipleSelection": true,
  "allowPointerOverPreview": true
}
```

- `hoverPreviewDelayMs` は `0～5000ms` に制限します。
- 旧設定に `previewDelayMs` だけがある場合、初回読み込み時に `hoverPreviewDelayMs` へ引き継ぎます。
- 既存の `previewResolutionP`、音声、音量、枠色、Escape 登録なども維持します。

設定ダイアログ項目:

- ホバーでプレビュー
- ホバー開始までの時間
- 対象から外れたらすぐ停止
- 複数選択時に確認する
- プレビュー上への移動を許可
- プレビュー開始時に音を出す
- Preview 解像度
- プレビュー枠色

## プレビュー操作

- クリック: 一時停止 / 再生
- ホイール: 再生位置を前後
- Ctrl + ホイール: 一時的に解像度変更
- 音量表示をクリック: 縦型音量スライダー
- Esc: プレビューを閉じる
- ループ再生: しない
- 複数プレビュー: 最大9件

## DirectOpen

MvSticky などから直接開く場合:

```powershell
MvView.exe --open "D:\media\sample.mp4"
```

`DirectOpen` で開いたプレビューは Explorer のホバー退出では閉じません。既に MvView が常駐している場合は、2つ目のプロセスから `WM_COPYDATA` で既存プロセスへパスを渡します。

## libmpv

実行時に次の順で動的ロードします。

```text
MvView.exe と同じフォルダーの mpv-2.dll
MvView.exe と同じフォルダーの libmpv-2.dll
runtime\mpv-2.dll
runtime\libmpv-2.dll
PATH 上の mpv-2.dll / libmpv-2.dll
```

DLL が無い場合でも MvView 自体はトレイ起動し、Tooltip とログへ状態を表示します。

## ビルド

必要環境:

- Windows 11
- Visual Studio / Build Tools
- Desktop development with C++
- Windows SDK

PowerShell:

```powershell
Set-ExecutionPolicy -Scope Process Bypass -Force
.\build.ps1 -Configuration Release -Publish
```

出力:

```text
bin\x64\Release\MvView.exe
publish\MvView_v0.25\
publish\MvView_v0.25_YYYYMMDD_HHMMSS.zip
```

`build.ps1` は `version.xml` を読み、`version_generated.h` を生成します。

## ログ

```text
%APPDATA%\MvView\logs\MvView.log
```

主なログ:

- `hover enter`
- `hover target changed`
- `hover wait started`
- `hover wait cancelled`
- `hover target left`
- `hover preview opened`
- `multi confirmation shown`
- `multi confirmation accepted`
- `multi confirmation cancelled`
- `preview closed`
- UI Automation / Shell 項目解決失敗
- Explorer タブ候補と選択された候補

## 既知の制限

- UI Automation 名と Shell 表示名から完全なパスを一意に確定できない項目は、安全のため再生しません。
- 拡張子非表示で同じ表示名になるファイル、検索結果内の同名ファイルなどは、複数候補になる場合があります。
- 仮想項目、クラウド上の未取得項目、ファイルシステムパスを持たない項目は対象外です。
- Windows 11 Explorer の内部 UI 実装変更により、特定の表示モードで追加調整が必要になる可能性があります。

## Version管理

- 現在のVersion: `v0.25`
- `version.xml` を唯一のVersion元とします。
- ソース／リリースごとに `0.01` ずつ増加します。
- タスクトレイTooltip、About、Splash、EXEリソース、配布名へ同じVersionを反映します。


## v0.25 changes

- 複数選択確認ダイアログのサイズ不足を修正
- タイトルバーを廃止し、メッセージと操作だけの軽い確認ウィンドウへ変更
- 確認ウィンドウのレイアウトと余白を調整


## External hover IPC (v0.25)

MvView keeps the existing `WM_COPYDATA` `dwData == 1` direct-open contract used by `MvView.exe --open <path>`. External hover clients use `dwData == 0x4D564831` (`MVH1`) with a null-terminated UTF-16 JSON document. Protocol `mvview-hover`, version `1`, supports `hover_open`, `hover_update`, `hover_move`, and `hover_close`. Requests carry `source_pid`, `request_id`, and monotonic `generation`; stale requests are ignored. Explorer hover and external hover are maintained as separate trigger states.
