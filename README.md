# mz2500-tools

MZ-2500用の小さな補助ツール集です。

## mdz88.rb

`mdz88.rb` は、MZ-2500用2DD D88イメージを作成・編集するRubyスクリプトです。
Ruby標準ライブラリのみを使用し、追加gemは不要です。

主な機能:

- ブランク2DD D88イメージ生成
- ファイル一覧表示、空き容量表示
- BSD/BRD/BTX/OBJファイル追加
- 複数ファイル追加
- ファイル抽出
- ファイル削除、複数削除、全削除
- ファイル名変更
- OBJのロードアドレス/実行アドレス指定

MZ-2500の2DDで使われる負論理のセクタデータ、ディレクトリ、ビットマップに対応しています。

### Usage

```sh
ruby mdz88.rb -blank disk.d88
ruby mdz88.rb -list disk.d88 --free
ruby mdz88.rb -add disk.d88 file1.bsd file2.brd
ruby mdz88.rb -add disk.d88 program.obj --load-addr 0x1200 --exec-addr 0x1200
ruby mdz88.rb -extract disk.d88 FILENAME output.bin
ruby mdz88.rb -rename disk.d88 OLD_NAME NEW_NAME
ruby mdz88.rb -delete disk.d88 NAME1 NAME2
ruby mdz88.rb -delete disk.d88 --all
```
