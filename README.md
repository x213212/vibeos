# vibe-os

`vibe-os` 是一個 RISC-V 32-bit QEMU browser OS 整合版。

Lineage:

- Original project: [cccriscv/mini-riscv-os](https://github.com/cccriscv/mini-riscv-os)
- Referenced fork/base: [x213212/mini-riscv-os](https://github.com/x213212/mini-riscv-os)
- This integration: `vibe-os`, based on `08-BlockDeviceDriver`, adding GUI networking, WRP, and browser support.

本專案從 `mini-riscv-os` 的 `08-BlockDeviceDriver` 階段延伸，加入 GUI、網路、NetSurf-style browser frontend、WRP server 整合與現代網站瀏覽能力。

## Quick Start

最短流程是在 host shell 下載、進入專案、啟動 QEMU：

```sh
# Host shell
git clone https://github.com/x213212/mini-riscv-os.git
cd mini-riscv-os/08-BlockDeviceDriver
make ENABLE_AUDIO=1 qemu
```

如果你要使用本整合版的 WRP browser，還需要先在 host 啟動 tap0 和 WRP server；完整指令分在下面。

## Commands

### Host Shell

這些指令是在 QEMU 外面的 Linux shell 執行。

設定 QEMU tap 網路：

```sh
# Host shell
cd /root/trade_new/os/mini-riscv-os/08-BlockDeviceDriver

sudo ip tuntap add dev tap0 mode tap user "$USER" 2>/dev/null || true
sudo ip addr flush dev tap0
sudo ip addr add 192.168.123.100/24 dev tap0
sudo ip link set tap0 up
```

編譯 mbedTLS static libs，第一次建置或 mbedTLS source 更新時才需要：

```sh
# Host shell
cd /root/trade_new/os/mini-riscv-os/08-BlockDeviceDriver
rm -rf /tmp/mbedtls-rv-build

cmake -S /root/trade_new/os/mini-riscv-os/08-BlockDeviceDriver/third_party/mbedtls \
  -B /tmp/mbedtls-rv-build \
  -G "Unix Makefiles" \
  -DCMAKE_SYSTEM_NAME=Generic \
  -DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY \
  -DCMAKE_C_COMPILER=/usr/bin/riscv64-unknown-elf-gcc \
  -DCMAKE_AR=/usr/bin/riscv64-unknown-elf-ar \
  -DCMAKE_RANLIB=/usr/bin/riscv64-unknown-elf-ranlib \
  -DCMAKE_C_FLAGS="-march=rv32imac -mabi=ilp32 -I/root/trade_new/os/mini-riscv-os/08-BlockDeviceDriver" \
  -DENABLE_PROGRAMS=OFF \
  -DENABLE_TESTING=OFF \
  -DGEN_FILES=ON \
  -DUSE_STATIC_MBEDTLS_LIBRARY=ON \
  -DUSE_SHARED_MBEDTLS_LIBRARY=OFF \
  -DUSE_STATIC_TF_PSA_CRYPTO_LIBRARY=ON \
  -DUSE_SHARED_TF_PSA_CRYPTO_LIBRARY=OFF \
  -DMBEDTLS_CONFIG_FILE=/root/trade_new/os/mini-riscv-os/08-BlockDeviceDriver/third_party/mbedtls/configs/config-suite-b.h \
  -DMBEDTLS_USER_CONFIG_FILE=/root/trade_new/os/mini-riscv-os/08-BlockDeviceDriver/mbedtls_os_config.h \
  -DMBEDTLS_FATAL_WARNINGS=OFF \
  -DMBEDTLS_AS_SUBPROJECT=OFF \
  -DMBEDTLS_TARGET_PREFIX=rv_

cmake --build /tmp/mbedtls-rv-build -j2
```

編譯 OS：

```sh
# Host shell
cd /root/trade_new/os/mini-riscv-os/08-BlockDeviceDriver
make -j2 os.elf
```

修改 header 或第三方 source 後，建議強制重編：

```sh
# Host shell
cd /root/trade_new/os/mini-riscv-os/08-BlockDeviceDriver
make -B -j2 os.elf
```

編譯 WRP server：

```sh
# Host shell
cd /root/trade_new/os/mini-riscv-os/08-BlockDeviceDriver/wrp
GOPATH=/tmp/gopath GOMODCACHE=/tmp/gopath/pkg/mod GOCACHE=/tmp/go-build go build -o wrp ./...
```

啟動 WRP server。這個 shell 要保持開著：

```sh
# Host shell, terminal 1
cd /root/trade_new/os/mini-riscv-os/08-BlockDeviceDriver/wrp
./wrp -l :9999 -b /opt/google/chrome/chrome -ua "" -s 1s
```

啟動 QEMU。這是在另一個 host shell：

```sh
# Host shell, terminal 2
cd /root/trade_new/os/mini-riscv-os/08-BlockDeviceDriver
make ENABLE_AUDIO=1 qemu
```

### QEMU Guest Shell

這些指令是在 QEMU 視窗裡的 OS terminal 執行，不是在 host Linux shell。

查看 WRP server URL：

```text
# QEMU guest shell
wrp status
```

設定 WRP server URL：

```text
# QEMU guest shell
wrp set http://192.168.123.100:9999
```

開啟 WRP browser：

```text
# QEMU guest shell
wrp open
```

直接開網站：

```text
# QEMU guest shell
netsurf https://news.ycombinator.com
netsurf https://duckduckgo.com
netsurf http://68k.news
```

下載檔案：

```text
# QEMU guest shell
wget http://192.168.123.100:9999/ index.html
```

SSH：

```text
# QEMU guest shell
ssh status
ssh set root@192.168.123.100:2221
ssh auth <password>
ssh exec <command>
```

## Protocol / Network Stack

`vibe-os` 的網路與遠端功能不是從零手刻完整協議，而是整合既有 open source stack：

- TCP/IP: 使用 [lwIP](https://github.com/lwip-tcpip/lwip) 作為 guest OS 內的 TCP/IP stack。
- Network device: QEMU `virtio-net-device`，host 端透過 `tap0` 接到 `192.168.123.100/24`。
- HTTP client: OS 內建 `user_wget.c`，跑在 lwIP TCP socket 上，用來下載 WRP HTML、GIP/GIF image、一般 HTTP 檔案。
- SSH protocol: 使用 [libssh2](https://github.com/libssh2/libssh2) 實作 SSH-2 client protocol。
- SSH crypto backend: libssh2 使用 [mbedTLS](https://github.com/Mbed-TLS/mbedtls) backend，也就是 `third_party/libssh2/src/mbedtls.c`。
- TLS / crypto primitives: mbedTLS 提供 RSA、AES、SHA、HMAC、CTR-DRBG、X.509、PK parsing 等 crypto primitives。
- Browser remoting: 現代網站由 host 上的 [WRP](https://github.com/tenox7/wrp) + Chrome render，guest OS 只處理 WRP 回傳的 HTML、ISMAP、GIP/GIF image。

重要邊界：

- `vibe-os` guest 端不是直接執行現代 JavaScript/CSS。
- WRP server 在 host 端執行 Chrome，guest 透過 HTTP/ISMAP 操作它。
- SSH 指令是 guest OS 透過 lwIP TCP 連線到遠端 SSH server，再由 libssh2 處理 SSH-2 protocol。

## Upstream Projects

核心來源：

- Original OS: [cccriscv/mini-riscv-os](https://github.com/cccriscv/mini-riscv-os)
- Referenced OS fork/base: [x213212/mini-riscv-os](https://github.com/x213212/mini-riscv-os)
- WRP Web Rendering Proxy: [tenox7/wrp](https://github.com/tenox7/wrp)
- GIP fast GIF encoder: [tenox7/gip](https://github.com/tenox7/gip)
- lwIP TCP/IP stack: [lwip-tcpip/lwip](https://github.com/lwip-tcpip/lwip)
- mbedTLS: [Mbed-TLS/mbedtls](https://github.com/Mbed-TLS/mbedtls)
- libssh2: [libssh2/libssh2](https://github.com/libssh2/libssh2)
- RISC-V GNU toolchain: [riscv-collab/riscv-gnu-toolchain](https://github.com/riscv-collab/riscv-gnu-toolchain)
- QEMU: [qemu-project/qemu](https://gitlab.com/qemu-project/qemu)
- Chromium / Chrome rendering engine: [chromium/src](https://chromium.googlesource.com/chromium/src)

NetSurf libraries：

- libnsfb: [git.netsurf-browser.org/libnsfb.git](https://git.netsurf-browser.org/libnsfb.git)
- libwapcaplet: [git.netsurf-browser.org/libwapcaplet.git](https://git.netsurf-browser.org/libwapcaplet.git)
- libparserutils: [git.netsurf-browser.org/libparserutils.git](https://git.netsurf-browser.org/libparserutils.git)
- libhubbub: [git.netsurf-browser.org/libhubbub.git](https://git.netsurf-browser.org/libhubbub.git)

WRP Go dependencies：

- chromedp: [chromedp/chromedp](https://github.com/chromedp/chromedp)
- cdproto: [chromedp/cdproto](https://github.com/chromedp/cdproto)
- halfgone: [MaxHalford/halfgone](https://github.com/MaxHalford/halfgone)
- goquery: [PuerkitoBio/goquery](https://github.com/PuerkitoBio/goquery)
- rootcerts: [breml/rootcerts](https://github.com/breml/rootcerts)
- go-quantize: [ericpauley/go-quantize](https://github.com/ericpauley/go-quantize)
- shortuuid: [lithammer/shortuuid](https://github.com/lithammer/shortuuid)
- resize: [nfnt/resize](https://github.com/nfnt/resize)
- oksvg: [srwiley/oksvg](https://github.com/srwiley/oksvg)
- rasterx: [srwiley/rasterx](https://github.com/srwiley/rasterx)
- Go image: [golang/image](https://github.com/golang/image)
- Go net: [golang/net](https://github.com/golang/net)
- Go sys: [golang/sys](https://github.com/golang/sys)
- Go text: [golang/text](https://github.com/golang/text)

已整合：

- VGA GUI / window manager
- virtio block / net / keyboard / tablet
- lwIP TCP/IP
- 簡易 shell、檔案系統、文字編輯器
- NetSurf-style browser frontend
- WRP Web Rendering Proxy 整合，用主機上的 Chrome 幫 OS 顯示現代網站
- WRP GIP fast GIF 路徑
- ISMAP click / keyboard / wheel queue，同步 WRP `.map` ID

目前主要工作目標是讓 QEMU 裡的自製 OS 可以透過主機上的 WRP server 瀏覽現代網站。

Base project:

- Original GitHub: [https://github.com/cccriscv/mini-riscv-os](https://github.com/cccriscv/mini-riscv-os)
- Referenced fork/base GitHub: [https://github.com/x213212/mini-riscv-os](https://github.com/x213212/mini-riscv-os)
- Base stage: `08-BlockDeviceDriver`

## 目錄

```text
RISC-V WRP Browser OS repo root/
  Makefile              OS build / QEMU 啟動設定
  os.elf                編譯後的 RISC-V kernel / userspace image
  hdd.dsk               QEMU virtio block disk image
  user.c                shell、GUI、window、terminal command
  user_netsurf.c        NetSurf/WRP frontend、滑鼠/鍵盤/滾輪 queue
  user_utils.c          URL normalize、WRP URL 產生、GIF/GIP decoder
  user_wget.c/.h        HTTP client / WGET queue
  ssh_client.c/.h       SSH 設定與 WRP URL 設定儲存
  virtio_net.c          virtio-net + lwIP netif
  lwip/                 lwIP source tree
  gbemu_wasm/           Game Boy emulator core
  wrp/                  修改版 WRP server
```

## Host / Guest 網路配置

QEMU 使用 tap 裝置：

```make
-netdev tap,id=net0,ifname=tap0,script=no,downscript=no
```

目前 IP 規劃：

| 端點 | IP / port | 說明 |
| --- | --- | --- |
| QEMU guest OS | `192.168.123.1/24` | 寫在 `virtio_net.c` |
| Host tap0 | `192.168.123.100/24` | WRP server 所在主機 |
| WRP default URL | `http://192.168.123.100:9999` | 寫在 `ssh_client.c` |

Guest 端目前不是直接上網；現代網站主要透過 host 上的 WRP / Chrome 處理。Guest 只需要能連到：

```text
192.168.123.100:9999
```

### tap0 設定

```sh
sudo ip tuntap add dev tap0 mode tap user "$USER" 2>/dev/null || true
sudo ip addr flush dev tap0
sudo ip addr add 192.168.123.100/24 dev tap0
sudo ip link set tap0 up
ip addr show tap0
```

如果有防火牆，放行 host 的 `9999/tcp`：

```sh
sudo ufw allow in on tap0 to any port 9999 proto tcp
```

如果沒有用 ufw，依你的 nftables / iptables 規則放行即可。

### OS 端 WRP URL

預設值在 `ssh_client.c`：

```c
#define SSH_DEFAULT_WRP_URL "http://192.168.123.100:9999"
```

在 OS terminal 裡可以改：

```text
wrp status
wrp set http://192.168.123.100:9999
wrp open
```

## 編譯 OS

需要工具：

- `riscv64-unknown-elf-gcc`
- `qemu-system-riscv32`
- GNU make
- 已建好的 mbedTLS RISC-V static libs，預設在 `/tmp/mbedtls-rv-build`

## Source Tree 來源

本整合版引用並延伸：

- [cccriscv/mini-riscv-os](https://github.com/cccriscv/mini-riscv-os)
- [x213212/mini-riscv-os](https://github.com/x213212/mini-riscv-os)

目前工作目錄仍位於原本教學階段路徑：

```text
/root/trade_new/os/mini-riscv-os/08-BlockDeviceDriver
```

文件中的 shell 指令使用這個本機路徑；如果你把專案 clone 到其他地方，請把路徑換成自己的 repo root。

## 第三方 source tree 位置

第三方 source code 已放進本專案目錄：

```text
/root/trade_new/os/mini-riscv-os/08-BlockDeviceDriver/
  third_party/
    mbedtls/
    libssh2/
    libnsfb/
    libwapcaplet/
    libparserutils/
    libhubbub/
```

`Makefile` 目前使用：

```make
THIRD_PARTY_DIR = ./third_party
MBEDTLS_DIR = $(THIRD_PARTY_DIR)/mbedtls
LIBSSH2_DIR = $(THIRD_PARTY_DIR)/libssh2
NSFB_DIR = $(THIRD_PARTY_DIR)/libnsfb
WCAP_DIR = $(THIRD_PARTY_DIR)/libwapcaplet
PUTIL_DIR = $(THIRD_PARTY_DIR)/libparserutils
HUB_DIR = $(THIRD_PARTY_DIR)/libhubbub
```

如果是乾淨機器，需要先把這些 source tree 放到 `third_party/` 下。來源可用 upstream：

```sh
cd /root/trade_new/os/mini-riscv-os/08-BlockDeviceDriver
mkdir -p third_party
cd third_party

git clone https://github.com/Mbed-TLS/mbedtls.git
git clone https://github.com/libssh2/libssh2.git
git clone https://git.netsurf-browser.org/libnsfb.git
git clone https://git.netsurf-browser.org/libwapcaplet.git
git clone https://git.netsurf-browser.org/libparserutils.git
git clone https://git.netsurf-browser.org/libhubbub.git
```

如果你用的是已經打過補丁的本地版本，保留本地版本，不要直接覆蓋。

## 編譯 mbedTLS 給 RISC-V

本專案的 OS link 會用到這些 static libs：

```text
/tmp/mbedtls-rv-build/library/libmbedtls.a
/tmp/mbedtls-rv-build/library/libmbedcrypto.a
/tmp/mbedtls-rv-build/library/libmbedx509.a
/tmp/mbedtls-rv-build/tf-psa-crypto/core/libtfpsacrypto.a
```

這些路徑寫在 `Makefile`：

```make
THIRD_PARTY_DIR = ./third_party
MBEDTLS_DIR = $(THIRD_PARTY_DIR)/mbedtls
MBEDTLS_BUILD_DIR = /tmp/mbedtls-rv-build
```

建議用獨立 build dir，不要在 source tree 內 build：

```sh
cd /root/trade_new/os/mini-riscv-os/08-BlockDeviceDriver
rm -rf /tmp/mbedtls-rv-build

cmake -S /root/trade_new/os/mini-riscv-os/08-BlockDeviceDriver/third_party/mbedtls \
  -B /tmp/mbedtls-rv-build \
  -G "Unix Makefiles" \
  -DCMAKE_SYSTEM_NAME=Generic \
  -DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY \
  -DCMAKE_C_COMPILER=/usr/bin/riscv64-unknown-elf-gcc \
  -DCMAKE_AR=/usr/bin/riscv64-unknown-elf-ar \
  -DCMAKE_RANLIB=/usr/bin/riscv64-unknown-elf-ranlib \
  -DCMAKE_C_FLAGS="-march=rv32imac -mabi=ilp32 -I/root/trade_new/os/mini-riscv-os/08-BlockDeviceDriver" \
  -DENABLE_PROGRAMS=OFF \
  -DENABLE_TESTING=OFF \
  -DGEN_FILES=ON \
  -DUSE_STATIC_MBEDTLS_LIBRARY=ON \
  -DUSE_SHARED_MBEDTLS_LIBRARY=OFF \
  -DUSE_STATIC_TF_PSA_CRYPTO_LIBRARY=ON \
  -DUSE_SHARED_TF_PSA_CRYPTO_LIBRARY=OFF \
  -DMBEDTLS_CONFIG_FILE=/root/trade_new/os/mini-riscv-os/08-BlockDeviceDriver/third_party/mbedtls/configs/config-suite-b.h \
  -DMBEDTLS_USER_CONFIG_FILE=/root/trade_new/os/mini-riscv-os/08-BlockDeviceDriver/mbedtls_os_config.h \
  -DMBEDTLS_FATAL_WARNINGS=OFF \
  -DMBEDTLS_AS_SUBPROJECT=OFF \
  -DMBEDTLS_TARGET_PREFIX=rv_

cmake --build /tmp/mbedtls-rv-build -j2
```

確認輸出：

```sh
ls -l \
  /tmp/mbedtls-rv-build/library/libmbedtls.a \
  /tmp/mbedtls-rv-build/library/libmbedcrypto.a \
  /tmp/mbedtls-rv-build/library/libmbedx509.a \
  /tmp/mbedtls-rv-build/tf-psa-crypto/core/libtfpsacrypto.a
```

目前 `CMakeCache.txt` 的關鍵設定應該類似：

```text
CMAKE_C_COMPILER=/usr/bin/riscv64-unknown-elf-gcc
CMAKE_SYSTEM_NAME=Generic
CMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY
CMAKE_C_FLAGS=-march=rv32imac -mabi=ilp32 -I/root/trade_new/os/mini-riscv-os/08-BlockDeviceDriver
ENABLE_PROGRAMS=OFF
ENABLE_TESTING=OFF
USE_STATIC_MBEDTLS_LIBRARY=ON
USE_STATIC_TF_PSA_CRYPTO_LIBRARY=ON
MBEDTLS_CONFIG_FILE=/root/trade_new/os/mini-riscv-os/08-BlockDeviceDriver/third_party/mbedtls/configs/config-suite-b.h
MBEDTLS_USER_CONFIG_FILE=/root/trade_new/os/mini-riscv-os/08-BlockDeviceDriver/mbedtls_os_config.h
```

注意：

- `mbedtls_os_config.h` 是本 OS 的 mbedTLS user config。
- `tf_psa_crypto_os_config.h` 會在 OS build 時透過 Makefile 傳給 compiler。
- 不要開 mbedTLS programs/tests；裸機 RISC-V OS 不需要 host 測試程式。
- 如果 mbedTLS source 更新過，建議刪掉 `/tmp/mbedtls-rv-build` 重跑 CMake。

## libssh2 怎麼編譯

本專案沒有先把 libssh2 編成獨立的 `libssh2.a`。OS Makefile 直接把 libssh2 source 編進 `os.elf`。

Makefile 相關 include：

```make
-I$(LIBSSH2_DIR)/include
-I$(LIBSSH2_DIR)/src
```

Makefile 直接編入的 libssh2 source 包含：

```text
third_party/libssh2/src/agent.c
third_party/libssh2/src/bcrypt_pbkdf.c
third_party/libssh2/src/chacha.c
third_party/libssh2/src/channel.c
third_party/libssh2/src/cipher-chachapoly.c
third_party/libssh2/src/comp.c
third_party/libssh2/src/crypt.c
third_party/libssh2/src/global.c
third_party/libssh2/src/hostkey.c
third_party/libssh2/src/keepalive.c
third_party/libssh2/src/kex.c
third_party/libssh2/src/knownhost.c
third_party/libssh2/src/mac.c
third_party/libssh2/src/mbedtls.c
third_party/libssh2/src/misc.c
third_party/libssh2/src/packet.c
third_party/libssh2/src/pem.c
third_party/libssh2/src/poly1305.c
third_party/libssh2/src/publickey.c
third_party/libssh2/src/scp.c
third_party/libssh2/src/session.c
third_party/libssh2/src/sftp.c
third_party/libssh2/src/transport.c
third_party/libssh2/src/userauth.c
third_party/libssh2/src/userauth_kbd_packet.c
third_party/libssh2/src/version.c
```

也就是：

```sh
cd /root/trade_new/os/mini-riscv-os/08-BlockDeviceDriver
make -j2 os.elf
```

就會同時編譯 libssh2。

這裡使用的是 libssh2 的 mbedTLS backend：

```text
third_party/libssh2/src/mbedtls.c
```

所以順序是：

1. 先把 mbedTLS static libs 建到 `/tmp/mbedtls-rv-build`
2. 再 `make os.elf`
3. `os.elf` link 時同時帶入 libssh2 source object 與 mbedTLS static libs

不建議在這個 OS 專案裡使用 libssh2 upstream 的 `cmake --build` 產生 host library，因為這裡需要的是裸機 RISC-V、無 libc/無 POSIX 的整合路徑。要改 SSH 功能時，改：

```text
ssh_client.c
ssh_client.h
Makefile 的 libssh2 source list / CFLAGS
```

然後重新：

```sh
make -B -j2 os.elf
```

## NetSurf 相關 libraries 怎麼編譯

這些 library 也不是先獨立建 `.a`，而是 Makefile 直接編 source。

### libnsfb

路徑：

```text
third_party/libnsfb
```

Makefile 編入：

```text
src/libnsfb.c
src/surface/surface.c
src/surface/ram.c
src/plot/api.c
src/plot/8bpp.c
src/plot/16bpp.c
src/plot/32bpp-xbgr8888.c
src/plot/32bpp-xrgb8888.c
src/plot/generic.c
src/plot/util.c
src/palette.c
src/cursor.c
```

### libwapcaplet / libparserutils

路徑：

```text
third_party/libwapcaplet
third_party/libparserutils
```

Makefile 編入 charset、inputstream、codec、buffer、vector、stack 等 source。

### libhubbub

路徑：

```text
third_party/libhubbub
```

Makefile 編入 tokeniser、parser、treebuilder 與 HTML parsing 相關 source。

### 編譯方式

不需要進各 library 目錄各自 make。直接：

```sh
cd /root/trade_new/os/mini-riscv-os/08-BlockDeviceDriver
make -j2 os.elf
```

Makefile 會用同一組 RISC-V CFLAGS 編進 `os.elf`。

這樣做的原因：

- 裸機 OS 沒有完整 libc / POSIX runtime。
- 需要統一使用 `netsurf_port.h` 做 portability shim。
- linker script 是本專案的 `os.ld`。
- 分開建 upstream `.a` 容易混入 host ABI 或不相容設定。

一般編譯：

```sh
cd /root/trade_new/os/mini-riscv-os/08-BlockDeviceDriver
make -j2 os.elf
```

完整重建：

```sh
make clean
make -j2 os.elf
```

如果只改 header，而 Makefile 沒有追到 dependency，可以強制重建：

```sh
make -B -j2 os.elf
```

啟動 QEMU：

```sh
make qemu
```

啟動 QEMU 並開 AC97 audio：

```sh
make ENABLE_AUDIO=1 qemu
```

`Makefile` 目前的 QEMU 重點設定：

- `qemu-system-riscv32`
- `-machine virt`
- `-bios none`
- `-kernel os.elf`
- virtio block: `hdd.dsk`
- virtio net: `tap0`
- virtio keyboard
- virtio tablet
- VGA
- monitor: `telnet:localhost:4321`

## 編譯 WRP

WRP 位於：

```sh
/root/trade_new/os/mini-riscv-os/08-BlockDeviceDriver/wrp
```

一般 Go build：

```sh
cd /root/trade_new/os/mini-riscv-os/08-BlockDeviceDriver/wrp
go build -o wrp ./...
```

如果環境不能寫入 `$HOME/go` 或 `$HOME/.cache`，用 `/tmp`：

```sh
GOPATH=/tmp/gopath \
GOMODCACHE=/tmp/gopath/pkg/mod \
GOCACHE=/tmp/go-build \
go build -o wrp ./...
```

啟動：

```sh
./wrp -l :9999 -b /opt/google/chrome/chrome -ua "" -s 1s
```

參數說明：

- `-l :9999`: listen 在所有 host interface 的 `9999`
- `-b /opt/google/chrome/chrome`: 指定 Chrome binary
- `-ua ""`: 不覆寫 UA，避免強制 HeadlessChrome UA
- `-s 1s`: render 後等待 1 秒再截圖

確認 WRP 正在 listen：

```sh
ss -ltnp '( sport = :9999 )'
```

如果 port 被舊 server 佔住：

```sh
ss -ltnp '( sport = :9999 )'
kill <pid>
./wrp -l :9999 -b /opt/google/chrome/chrome -ua "" -s 1s
```

## WRP 整合設計

Guest OS 不直接執行 JavaScript 或現代 CSS。流程是：

1. OS terminal 執行 `netsurf <url>` 或 `wrp open`
2. OS 把目標 URL 包成 WRP request：

   ```text
   http://192.168.123.100:9999/?m=ismap&t=gip&w=<width>&h=<height>&url=<encoded-url>
   ```

3. Host WRP 用 Chrome 開頁面。
4. WRP 截圖，輸出 GIP/GIF image。
5. WRP 回傳 HTML：

   ```html
   <A HREF="/map/<id>.map">
     <IMG SRC="/img/<id>.gif" ISMAP>
   </A>
   ```

6. OS 解析 HTML，下載 `/img/<id>.gif`。
7. OS GIF decoder 解成 RGB565，畫到 VGA GUI。
8. 點擊、鍵盤、滾輪透過 `/map/<id>.map?...` 回送給 WRP。

## 這份 fork 對 WRP / NetSurf 做過的主要改寫

### 1. WRP URL default

`ssh_client.c` 增加預設：

```c
#define SSH_DEFAULT_WRP_URL "http://192.168.123.100:9999"
```

所以 OS 裡不用先 `wrp set` 也可以打開 WRP。

### 2. document navigation 與 ISMAP navigation 分離

`user_netsurf.c` 中分離：

- document navigation: 開新網站
- ISMAP navigation: 點擊 / 鍵盤 / 滾輪送到 `/map/...`

避免按鍵或點擊把 address bar 的目標 URL 改成內部 `/map/...`。

### 3. 操作 queue 與 ID locking

WRP 每次處理 click/key/scroll 都會產生新的：

```text
/map/<new-id>.map
/img/<new-id>.gif
```

舊 map 會被 WRP cache 清掉。為避免連續操作拿舊 ID，OS 端加入：

- `waiting_for_id`
- action queue
- 新 map ID 未回來前不送下一筆
- 新 image 未下載/解碼前也不送下一筆

這解決：

- `Unable to find map`
- 連續打字 ID race
- 滾輪第二次開始失效
- 點擊和滾輪互相打斷

### 4. 滾輪 queue 化

滾輪不再直接呼叫 navigation，而是進 action queue：

```text
PageDown
PageUp
```

正常 log 應該是：

```text
[NET] ENQUEUE SCROLL: PageDown
[NET] Q-EXEC win=1 target='http://192.168.123.100:9999/map/<id>.map?0,0&k=PageDown'
[NET] image_queue ...
[NET] image win=1 bytes=...
```

下一筆 scroll 必須等新 image 解碼完才會送。

### 5. WRP scroll 修正

`wrp/ismap.go` 的 scroll key 改成直接調整 root scroller：

- `document.scrollingElement`
- fallback `document.documentElement`
- fallback `document.body`

並避免 scroll 後截圖前重新 `SetDeviceMetricsOverride`，因為這會重置 Chrome viewport / scroll state。

WRP log 會輸出：

```text
Sending Keys: "PageDown"
Scroll PageDown y=<scrollTop> err=<nil>
Encoded GIP image ...
```

### 6. GIP fast GIF

OS WRP request 使用：

```text
t=gip
```

WRP 的 GIP 實際輸出仍是 `.gif`，但用 `github.com/tenox7/gip` 的快速 parallel GIF encoder。

OS 端 `user_utils.c` 的 GIF decoder 加了 GIP 相容：

- 修正 GIP header palette size 判斷
- LZW prefix/suffix/stack 改成 static buffer，避免每張圖 malloc/free
- 非 interlace GIF 快路徑避免每 pixel 做除法與取餘數

### 7. HTTP header buffer

`user_wget.h` 的 HTTP header buffer 加大，避免 WRP 回圖時：

```text
[wget] finish success=0 msg=HDR TOO BIG
```

這類錯誤會導致圖片下載失敗、頁面空白。

### 8. Favorites

NetSurf 視窗有 `[Fav]` 選單，包含：

- 68k News
- Hacker News
- DuckDuckGo
- FrogFind
- Lite Wiki
- Low-tech Mag
- Wttr.in

Favorites click 使用專用路徑，避免點選 sidebar 時同時觸發底層網頁 click。

## OS 內常用指令

WRP：

```text
wrp status
wrp set http://192.168.123.100:9999
wrp open
```

NetSurf / browser：

```text
netsurf https://news.ycombinator.com
netsurf https://duckduckgo.com
netsurf http://68k.news
```

下載：

```text
wget http://192.168.123.100:9999/ file.html
```

SSH：

```text
ssh status
ssh set root@192.168.123.100:2221
ssh auth <password>
ssh exec <command>
```

其他 shell：

```text
ls
cat <file>
write <file> <text>
vim <file>
gbemu <rom.gb>
demo3d
```

## Debug / troubleshooting

### WRP server 是否活著

Host：

```sh
ss -ltnp '( sport = :9999 )'
curl 'http://192.168.123.100:9999/'
```

OS：

```text
wrp status
wrp open
```

### 頁面空白

看 OS log：

```text
[NET] image_ref ...
[NET] image_queue ...
[NET] image win=1 bytes=...
```

如果看到：

```text
HDR TOO BIG
```

代表 QEMU 還在跑舊 `os.elf`，需要重新啟動 QEMU。

如果看到：

```text
image decode failed
```

代表 WRP 回的 image 格式 OS decoder 吃不下。此 fork 預設使用 `t=gip`，且已修 GIP GIF decoder 相容。

### 滾輪失效

正常 OS log：

```text
[NET] ENQUEUE SCROLL: PageDown
[NET] Q-EXEC win=1 target='.../map/<id>.map?0,0&k=PageDown'
[NET] image_queue ...
[NET] image win=1 bytes=...
```

正常 WRP log：

```text
ISMAP Request for /map/<id>.map [0,0&k=PageDown]
Sending Keys: "PageDown"
Scroll PageDown y=<value> err=<nil>
Encoded GIP image ...
```

如果只有 `ENQUEUE SCROLL` 沒有 `Q-EXEC`，代表 queue 還在等前一張圖或前一個 ID。

如果 `Q-EXEC` 一直使用同一個舊 map id，代表新 HTML / image 沒完成，或 QEMU 還在跑舊版。

如果 WRP 回：

```text
Unable to find map
```

代表 map cache 被下一次 capture 清掉，通常是 queue lock 沒生效或 QEMU 還是舊版。

### WRP port 被佔用

```sh
ss -ltnp '( sport = :9999 )'
kill <pid>
cd /root/trade_new/os/mini-riscv-os/08-BlockDeviceDriver/wrp
./wrp -l :9999 -b /opt/google/chrome/chrome -ua "" -s 1s
```

### QEMU 沒網路

確認 tap0：

```sh
ip addr show tap0
```

應該看到：

```text
192.168.123.100/24
```

確認 WRP listen address 包含：

```text
192.168.123.100:9999
```

## 重要檔案

| 檔案 | 用途 |
| --- | --- |
| `Makefile` | OS build / QEMU flags |
| `virtio_net.c` | guest IP / lwIP netif / virtio-net |
| `user.c` | shell、window manager、`netsurf` / `wrp` command |
| `user_netsurf.c` | browser UI、HTML parser、WRP queue、click/key/wheel |
| `user_utils.c` | URL wrapper、GIP/GIF decoder、WRP request |
| `user_wget.c` | HTTP client、image/document queue |
| `user_wget.h` | WGET buffer size |
| `ssh_client.c` | SSH config 與 WRP default URL |
| `wrp/ismap.go` | WRP ISMAP、click/key/scroll、screenshot、GIP encode |
| `wrp/wrp.go` | WRP server flags / request parsing |
| `wrp/util.go` | WRP palette / helper |

## 使用到的 open source

這個工作樹把多個 open source 元件整合在一起。重新散佈前請檢查各自 upstream license。

### OS / emulator / toolchain

- [cccriscv/mini-riscv-os](https://github.com/cccriscv/mini-riscv-os)：原版 RISC-V toy OS。
- [x213212/mini-riscv-os](https://github.com/x213212/mini-riscv-os)：本整合版參考的 fork/base，本專案基於其 `08-BlockDeviceDriver` 階段延伸。
- QEMU：RISC-V virt machine 模擬器。
- RISC-V GNU toolchain：`riscv64-unknown-elf-gcc`。

### Network / crypto / SSH

- lwIP：TCP/IP stack。
- mbedTLS：TLS / crypto library。
- libssh2：SSH client library。

### NetSurf 相關 libraries

Makefile 直接編入以下 NetSurf project libraries：

- libnsfb：framebuffer abstraction。
- libwapcaplet：string intern / atom library。
- libparserutils：charset / parser utilities。
- libhubbub：HTML5 parser。

### WRP server

- WRP：`github.com/tenox7/wrp`，Web Rendering Proxy。
- GIP：`github.com/tenox7/gip`，快速 parallel GIF encoder。
- chromedp：Go Chrome DevTools Protocol control。
- cdproto：Chrome DevTools Protocol bindings。
- Go standard library：HTTP server、image encode/decode 等。
- Google Chrome / Chromium：實際 rendering engine。

### WRP Go modules

`wrp/go.mod` 目前直接使用：

- `github.com/MaxHalford/halfgone`
- `github.com/PuerkitoBio/goquery`
- `github.com/breml/rootcerts`
- `github.com/chromedp/cdproto`
- `github.com/chromedp/chromedp`
- `github.com/ericpauley/go-quantize`
- `github.com/lithammer/shortuuid/v4`
- `github.com/nfnt/resize`
- `github.com/srwiley/oksvg`
- `github.com/srwiley/rasterx`
- `github.com/tenox7/gip`
- `golang.org/x/image`
- `golang.org/x/net`

間接依賴：

- `github.com/andybalholm/cascadia`
- `github.com/chromedp/sysutil`
- `github.com/go-json-experiment/json`
- `github.com/gobwas/httphead`
- `github.com/gobwas/pool`
- `github.com/gobwas/ws`
- `github.com/google/uuid`
- `golang.org/x/sys`
- `golang.org/x/text`

### 其他整合

- `gbemu_wasm/`：Game Boy emulator core。
- VGA / AC97 / virtio 裝置程式碼：本專案內整合實作。

## 開發注意事項

- 修改 `.h` 後，Makefile 不一定會自動重建，必要時用 `make -B -j2 os.elf`。
- WRP 修改後要重新 `go build -o wrp ./...`，而且要重啟 server。
- OS 修改後要重新啟動 QEMU，正在跑的 QEMU 不會自動載入新的 `os.elf`。
- WRP 的 map/image cache 對 ID 很敏感，連續操作一定要經過 OS action queue。
- 不要讓 scroll / key 在新 image 還沒下載完成前送出，否則 WRP 可能清掉上一輪 map。

## 目前建議啟動順序

```sh
# 1. host network
cd /root/trade_new/os/mini-riscv-os/08-BlockDeviceDriver
sudo ip tuntap add dev tap0 mode tap user "$USER" 2>/dev/null || true
sudo ip addr flush dev tap0
sudo ip addr add 192.168.123.100/24 dev tap0
sudo ip link set tap0 up

# 2. WRP server
cd /root/trade_new/os/mini-riscv-os/08-BlockDeviceDriver/wrp
./wrp -l :9999 -b /opt/google/chrome/chrome -ua "" -s 1s

# 3. QEMU, in another shell
cd /root/trade_new/os/mini-riscv-os/08-BlockDeviceDriver
make ENABLE_AUDIO=1 qemu
```

OS terminal：

```text
wrp open
```

或：

```text
netsurf https://news.ycombinator.com
```
