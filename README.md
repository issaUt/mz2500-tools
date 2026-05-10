# mz2500-tools

MZ-2500用の小さな補助ツール集です。

## mzd88

MZ-2500用2DD D88イメージを作成・編集するツールです。

MZ-2500の2DDで使われる負論理のセクタデータ、ディレクトリ、ビットマップ、BSD/BRD/BTX/OBJファイル追加に対応しています。

このリポジトリでは、Ruby版とC版を併設しています。

- `mzd88.rb`: Ruby版。参照実装として読みやすく、動作確認や仕様調整に向いています。
- `mzd88.c`: C版。単体バイナリ化して配布する用途に向いています。

## Ruby版

Ruby標準ライブラリのみを使用し、追加gemは不要です。

```sh
ruby mzd88.rb -blank disk.d88
ruby mzd88.rb -list disk.d88 --free
ruby mzd88.rb -add disk.d88 file1.bsd file2.brd
ruby mzd88.rb -add disk.d88 program.obj --load-addr 0x1200 --exec-addr 0x1200
ruby mzd88.rb -extract disk.d88 FILENAME output.bin
ruby mzd88.rb -rename disk.d88 OLD_NAME NEW_NAME
ruby mzd88.rb -delete disk.d88 NAME1 NAME2
ruby mzd88.rb -delete disk.d88 --all
```

## C版

C99対応コンパイラでビルドできます。

Linux/WSL/macOSなど:

```sh
gcc -std=c99 -O2 -Wall -Wextra -o mzd88 mzd88.c
```

Windows + MinGW-w64/MSYS2の場合:

```sh
gcc -std=c99 -O2 -Wall -Wextra -o mzd88.exe mzd88.c
```

Windows + Visual Studio 2022の場合:

```bat
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
cl /nologo /W4 /O2 /Fe:mzd88.exe mzd88.c
```

ビルド確認済み:

- WSL gcc
- Visual Studio 2022 MSVC (`cl /W4`)

### VSCodeでMINGW64ターミナルを使う

MSYS2のMINGW64環境をVSCodeのターミナルから使う場合は、VSCodeの `settings.json` にプロファイルを追加します。

```json
{
  "terminal.integrated.profiles.windows": {
    "MINGW64": {
      "path": "C:\\msys64\\usr\\bin\\bash.exe",
      "args": ["--login", "-i"],
      "env": {
        "MSYSTEM": "MINGW64",
        "CHERE_INVOKING": "1"
      }
    }
  },
  "terminal.integrated.defaultProfile.windows": "MINGW64"
}
```

UCRT64を使う場合は `MSYSTEM` を `UCRT64` にします。

```json
"MSYSTEM": "UCRT64"
```

MINGW64環境にgccが入っていない場合は、MSYS2のMINGW64ターミナルで以下を実行します。

```sh
pacman -Syu
pacman -S mingw-w64-x86_64-gcc
```

UCRT64環境の場合:

```sh
pacman -Syu
pacman -S mingw-w64-ucrt-x86_64-gcc
```

使い方はRuby版と同じです。

```sh
./mzd88 -blank disk.d88
./mzd88 -list disk.d88 --free
./mzd88 -add disk.d88 file1.bsd file2.brd
./mzd88 -add disk.d88 program.obj --load-addr 0x1200 --exec-addr 0x1200
./mzd88 -extract disk.d88 FILENAME output.bin
./mzd88 -rename disk.d88 OLD_NAME NEW_NAME
./mzd88 -delete disk.d88 NAME1 NAME2
./mzd88 -delete disk.d88 --all
```

## 主な機能

- ブランク2DD D88イメージ生成
- ファイル一覧表示、空き容量表示
- BSD/BRD/BTX/OBJファイル追加
- 複数ファイル追加
- ファイル抽出
- ファイル削除、複数削除、全削除
- ファイル名変更
- OBJのロードアドレス/実行アドレス指定

## ライセンス

MITライセンス
