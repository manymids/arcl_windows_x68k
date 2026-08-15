# px68k ARCL — Windows 向け X68000 エミュレータ / MCP サーバ

[English version](README.en.md)

本プロジェクトは [px68k-libretro](https://github.com/libretro/px68k-libretro) をベースに、Windows 上で動作する Sharp X68000 エミュレータ用フロントエンドを提供します。
通常の SDL2 GUI による対話操作に加え、[Model Context Protocol (MCP)](https://modelcontextprotocol.io/) の stdio サーバーとして起動できます。MCP 対応クライアントから、画面取得、入力、Human68k コンソール、デバッグ情報およびセーブステートを操作できます。

本リポジトリには、BIOS、CG-ROM、Human68k、ゲームその他のディスクイメージは一切含まれません。利用者自身が適法に取得・保有するファイルを使用してください。

## 対象環境

- Windows 10 または Windows 11（64-bit）
- MSYS2 の `MINGW64` 環境（ソースからビルドする場合）
- X68000 実機由来の BIOS/CG-ROM ダンプ
- 起動するディスクイメージ（`.XDF`、`.HDF`、`.D88` または `.DIM`）

このフロントエンドは Windows / MinGW-w64 向けです。他の OS 向けのビルド手順は提供していません。

## 事前に用意するファイル

### BIOS / CG-ROM

X68000 の BIOS と CG-ROM は Sharp の著作物です。実機など、利用する権利のある環境から取得した次のファイルを用意してください。

```text
px68k/
  system/
    keropi/
      iplrom.dat
      cgrom.dat
```

上記のディレクトリは Git では管理されないため、新規 clone 後は必要に応じて作成してください。

PowerShell では、次のように作成できます。

```powershell
New-Item -ItemType Directory -Force px68k\system\keropi
```

BIOS / CG-ROM、Human68k システムディスク、ゲームディスクを本リポジトリへ commit・再配布しないでください。

### ディスクイメージ

起動したいディスクイメージを任意の場所に配置します。以下では `C:\X68000\HUMAN302.XDF` を例として使用します。パスに空白が含まれる場合は、コマンドラインまたは MCP 設定で適切に引数として指定してください。

## ビルド

1. [MSYS2](https://www.msys2.org/) を導入します。
2. 「MSYS2 MINGW64」シェルを開きます。`MSYSTEM=MINGW64` であることを確認してください。
3. ビルド依存パッケージを導入します。

```bash
pacman -S --needed mingw-w64-x86_64-toolchain mingw-w64-x86_64-SDL2 mingw-w64-x86_64-cmake
```

4. リポジトリのルートで CMake を構成・ビルドします。

```bash
cmake -S windows -B windows/build -G "MinGW Makefiles"
cmake --build windows/build -j
```

成功すると `windows/build/arcl_windows_x68k.exe` が生成されます。SDL2 と zlib は静的リンクされる構成のため、生成された実行ファイルには通常、別途 SDL2 DLL や zlib DLL を同梱する必要はありません。

ビルド設定を最初から作り直したい場合は `windows/build/` を削除してから、上記の `cmake -S` を再実行してください。

## 通常モードで起動する

次のコマンドで SDL2 ウィンドウを開き、指定したディスクイメージを起動します。

```powershell
windows\build\arcl_windows_x68k.exe --system-dir px68k\system C:\X68000\HUMAN302.XDF
```

通常モードでは、GUI のイベント処理・描画・エミュレーションを同一スレッドで処理します。実行速度はホスト性能に依存し、実機相当の速度に固定されません。

| キー | 操作 |
|---|---|
| `Esc` | 終了 |
| `F5` | 一時停止 / 再開 |
| `F1` | ウィンドウタイトルにフレーム番号・状態を表示 / 非表示 |
| `F2` | 現在の画面を PNG ファイルとして保存 |

CPU クロックおよび RAM 容量は次のように指定できます。

```powershell
windows\build\arcl_windows_x68k.exe --system-dir px68k\system --clock 25 --ram 8 C:\X68000\HUMAN302.XDF
```

- `--clock`: `10`、`16`、`25`、`33`、`66`、`100` MHz（既定値: `10`）
- `--ram`: `1`〜`12` MB（既定値: `2`）

## MCP モードで起動する

MCP モードでは、実行ファイルが標準入力・標準出力で改行区切りの JSON-RPC 2.0 を処理します。標準出力は MCP プロトコル専用のため、端末上で直接操作する用途には向きません。MCP クライアントから stdio サーバーとして起動してください。

```powershell
windows\build\arcl_windows_x68k.exe --mcp --mcp-layers all --system-dir px68k\system C:\X68000\HUMAN302.XDF
```

MCP モードでも既定では SDL2 ウィンドウを表示します。AI に操作を任せながら画面を確認でき、`F5` により GUI 側から一時停止・再開できます。GUI ウィンドウを表示せずにバックグラウンド（ヘッドレス）で実行したい場合は `--no-window` オプションを指定してください。起動直後のエミュレータは停止状態です。クライアントから `arcl_run` または `arcl_resume` を呼び出すまで、エミュレーションは進みません。

### MCP クライアントの設定

`.mcp.json.example` と `.codex/config.toml.example` は、パスを含まない設定例です。使用するクライアントに合わせてコピーし、ディスクイメージのプレースホルダーを実際のパスに変更してください。

```powershell
Copy-Item .mcp.json.example .mcp.json
```

`.mcp.json` および `.codex/config.toml` はローカル環境固有の設定として Git 管理対象外です。実行ファイルや BIOS、ディスクイメージと同様に公開リポジトリへ追加しないでください。

### レイヤー指定

`--mcp-layers` には `l0`〜`l4` のカンマ区切り、または `all` を指定します。省略時は `l0,l1` です。

| レイヤー | 主な機能 |
|---|---|
| Control | 実行、一時停止、リセット、セーブ / ロード |
| L0 | 画面取得、キーボード、マウス、ジョイパッド、音声キャプチャ |
| L1 | Human68k コンソール、ディスク交換、ホストディレクトリ |
| L2 | レジスタ、メモリ、ブレークポイント、逆アセンブル |
| L3 | 映像、VRAM、パレット、スプライト、DMA、割り込み、OPM |
| L4 | 名前付きスナップショット、巻き戻し、実行速度計測 |

各 tool の正確な入力・出力形式は、MCP 接続後の `tools/list` が返す JSON Schema を参照してください。L2 以降にはエミュレート中の状態を書き換える機能が含まれるため、信頼できる MCP クライアントだけに接続してください。

## 既知の制約

- `arcl_type`、`arcl_command`、`arcl_mount` では `:`、`*`、`^`、`_`、`~` を入力できません。
- `arcl_console_read` は半角文字の認識を対象とします。全角文字・漢字は `?` として表示される場合があります。
- `arcl_step` による命令単位のステップ実行はサポートしません。代わりに `arcl_run(frames=1)`、または exec ブレークポイントと `arcl_run(until_break=true)` を使用してください。
- ブレークポイントおよび watchpoint の停止精度はフレーム単位です。
- `x68k_opm` はこのフロントエンド独自の追跡状態です。セーブステートをロードした直後は、ゲストが OPM レジスタを書き込むまで値が 0 と表示されることがあります。

## リポジトリ構成

| パス | 内容 |
|---|---|
| `windows/` | Windows フロントエンド、MCP サーバー、CMake 設定 |
| `px68k-libretro/` | px68k-libretro コアのソース（ARCL 用の最小フックを含む） |
| `arcl_common_spec.md` | ARCL tool の機種非依存コントラクト |
| `.mcp.json.example` | MCP クライアント向け設定例 |
| `.codex/config.toml.example` | Codex CLI 向け設定例 |

## ライセンス

`px68k-libretro` と、このコアをリンクして配布する本フロントエンドは GPLv2 の条件に従います。ライセンス本文は [px68k-libretro/COPYING](px68k-libretro/COPYING) を参照してください。

BIOS / CG-ROM およびディスクイメージの権利は、それぞれの権利者に帰属します。それらのファイルは本リポジトリのライセンス対象ではなく、同梱・再配布してはいけません。
