# CLAUDE.md — m5-usblink

M5Stack（ESP32-S3）と他の機器（PC、ラズパイ、Arduino）を USB でつなぐ層の共通ライブラリ。マイコン側（`arduino/`、C++）とパソコン側（`host/`、Python）の組。使い方は `README.md`、これから足すものは `TODO.md`。

## 決まり

- **プロジェクトのことを知らない。** 持つのは通信と手順（フレーム、シリアル、キーボード、ホスト／デバイス）だけ。アプリの API の口、基板の見分けの表、何を打ち込むか、どの操作の前に断るかはアプリ側に残す。
- **UL1 の手順と 4KB ごとの受け取り確認（ack）を崩さない。** ack が無いと、ESP32-S3 の HWCDC は黙ってデータを落とす。
- host 側の許可リスト（既定では localhost だけ）を保つ。
- 版は git のタグで出す。アプリは `lib_deps` に git の URL とタグを書いて固定し、同時に直す間だけ `symlink://` に差し替える。
- 前提: CoreS3、PlatformIO espressif32@6.9.0（Arduino 2.0.17）、C++11、ArduinoJson 7。host は Python 3.11 以上、uv。
