# usblink

マイコン（ESP32）を USB でパソコンにつなぎ、マイコンが頼んだ HTTP をパソコンが代わりにする。Wi-Fi のない場所（5GHz しかない会場など）でも、パソコンがネットにつながっていれば、マイコンからネットの API を呼んだり、パソコンの上のサーバーを呼んだりできる。パソコンの時計ももらえる。

ログ（`Serial.print`）は同じ USB のシリアルにそのまま流してよい。usblink はログをそのまま表示し、打った文字はマイコンに送るので、シリアルモニタの代わりにもなる。

どのプロジェクトにも使えるように、プロジェクトのことは何も知らない。

- `arduino/`: マイコン側のライブラリ `UsbLink`（ESP32。ArduinoJson と ESP-IDF の mbedtls を使う）
- `host/`: パソコン側の `usblink` コマンド（Python、pyserial）

## 使い方

マイコン側（PlatformIO）:

```ini
; m5-usblink をプロジェクトの隣に clone した場合
lib_deps =
  symlink://../m5-usblink/arduino
```

```cpp
#include "UsbLink.h"

UsbLink link;

void setup() {
  Serial.setRxBufferSize(8192);  // 返事を受け取るので大きめに
  Serial.begin(115200);
  link.begin(Serial);
}

void loop() {
  link.poll();  // 毎回。あいさつ（3 秒ごと）と受け取り
  int c = link.read();  // Serial.read() の代わり（フレームでない文字だけが来る）
  if (link.connected()) {
    UsbLink::Response r;
    if (link.request("GET", "https://api.open-meteo.com/v1/forecast?...", nullptr, 0, nullptr, r)) {
      // r.status, r.body, r.length
      free(r.body);
    }
  }
}
```

パソコン側:

```sh
uv tool install ./m5-usblink/host  # 一度だけ（または uvx --from ./m5-usblink/host usblink ...）
usblink --allow api.open-meteo.com  # 許す先を足す（localhost は最初から）
```

- 代わりにする HTTP は、`--allow` で許したホストと `localhost` だけ。マイコンから何でも呼べるようにはしない。
- マイコンから見た `localhost` はパソコン（パソコンの上のサーバーを呼べる）。
- ポートを開くとマイコンが一度再起動することがある。マイコンが再起動してポートが消えても、つなぎ直す。

## 取り決め（版 1）

フレームは `0x1E` `UL1 ` で始まる一行の JSON。ログと同じシリアルに流れても見分けられる。

マイコンから:

```
0x1E UL1 {"v":1,"id":1,"op":"hello"}\n
0x1E UL1 {"v":1,"id":2,"op":"http","m":"POST","u":"http://localhost:8000/api/hello","h":{"content-type":"text/plain"},"b":"<本文の base64>"}\n
0x1E UL1 {"v":1,"id":2,"op":"ack","n":4096}\n
```

パソコンから:

```
0x1E UL1 {"v":1,"id":1,"op":"hello","t":1793900000,"s":200,"n":0,"c":4096}\n
0x1E UL1 {"v":1,"id":2,"op":"http","t":1793900000,"s":200,"h":{"content-type":"audio/wav"},"n":514018,"c":4096}\n<本文 n バイト>
```

- `hello`: マイコンがあいさつを送り続け（既定 3 秒ごと）、返事が間隔の 3 倍以内にあればつながっているとみる。返事の `t` はパソコンの時計（UNIX 時刻、秒）。
- `http`: `s` は HTTP の status（届かなければ 0、許していない先は 403）。
- 本文は `c` バイトずつ送る。マイコンは `c` バイト受け取るごと（と最後）に `ack` を返し、パソコンはそれを待ってから次を送る。ESP32-S3 の USB シリアル（arduino-esp32 の HWCDC）は、受け取りが追いつかないと黙って捨てるため（2026-11-05、514KB の WAV が一度に送ると届かなかった。ack ありで 3.3 秒）。
- 本文を送っているあいだ、パソコンはほかのフレームに返事をしない（返事が本文に混ざるので）。

## テスト

```sh
cd m5-usblink/host && uv run --group dev pytest
```

## ライセンス

MIT（`LICENSE`）。
