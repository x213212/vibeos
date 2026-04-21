CC = riscv64-unknown-elf-gcc
THIRD_PARTY_DIR = ./third_party
MBEDTLS_DIR = $(THIRD_PARTY_DIR)/mbedtls
MBEDTLS_BUILD_DIR = /tmp/mbedtls-rv-build
MBEDTLS_LIBS = $(MBEDTLS_BUILD_DIR)/library/libmbedtls.a \
               $(MBEDTLS_BUILD_DIR)/library/libmbedcrypto.a \
               $(MBEDTLS_BUILD_DIR)/library/libmbedx509.a \
               $(MBEDTLS_BUILD_DIR)/tf-psa-crypto/core/libtfpsacrypto.a

LIBSSH2_DIR = $(THIRD_PARTY_DIR)/libssh2
NSFB_DIR = $(THIRD_PARTY_DIR)/libnsfb
WCAP_DIR = $(THIRD_PARTY_DIR)/libwapcaplet
PUTIL_DIR = $(THIRD_PARTY_DIR)/libparserutils
HUB_DIR = $(THIRD_PARTY_DIR)/libhubbub
TCC_DIR = /root/trade_new/os/mini-riscv-os/tcc-riscv32

CFLAGS = -nostdlib -fno-builtin -mcmodel=medany -march=rv32imac -mabi=ilp32 -DLWIP_NO_CTYPE_H -DWITHOUT_ICONV_FILTER -g -Wall -w \
         -mno-relax \
         -I./ -I./lwip/src/include -I./lwip/src/core -I./lwip/src/netif \
         -I./gbemu_wasm/include \
         -I$(LIBSSH2_DIR)/include -I$(LIBSSH2_DIR)/src \
         -I$(NSFB_DIR)/include -I$(NSFB_DIR)/src \
         -I$(WCAP_DIR)/include \
         -I$(PUTIL_DIR)/include -I$(PUTIL_DIR)/src \
         -I$(HUB_DIR)/include -I$(HUB_DIR)/src \
         -I$(TCC_DIR) -DTCC_TARGET_RISCV32 -DONE_SOURCE=1 -DCONFIG_TCC_STATIC -DCONFIG_TCC_PREDEFS=1 \
         -include netsurf_port.h \
         -Dprintf=lib_printf \
         -I$(MBEDTLS_DIR)/include -I$(MBEDTLS_DIR)/tf-psa-crypto/include -I$(MBEDTLS_DIR)/tf-psa-crypto/core -I$(MBEDTLS_DIR)/tf-psa-crypto/drivers/builtin/include -I$(MBEDTLS_DIR)/tf-psa-crypto/drivers/builtin/src -I$(MBEDTLS_DIR)/tf-psa-crypto/utilities \
         -DMBEDTLS_CONFIG_FILE=\"$(MBEDTLS_DIR)/configs/config-suite-b.h\" \
         -DMBEDTLS_USER_CONFIG_FILE=\"mbedtls_os_config.h\" \
         -DTF_PSA_CRYPTO_CONFIG_FILE=\"$(MBEDTLS_DIR)/configs/crypto-config-suite-b.h\" \
         -DTF_PSA_CRYPTO_USER_CONFIG_FILE=\"tf_psa_crypto_os_config.h\" \
         -DMBEDTLS_MD_C -DMBEDTLS_MD_LIGHT -DMBEDTLS_CIPHER_C -DMBEDTLS_RSA_C \
         -DHAVE_CONFIG_H \
         -DHOST_BUILD_YEAR=$(shell date +%Y) -DHOST_BUILD_MONTH=$(shell date +%-m) -DHOST_BUILD_DAY=$(shell date +%-d) \
         -DHOST_BUILD_HOUR=$(shell date +%-H) -DHOST_BUILD_MIN=$(shell date +%-M) -DHOST_BUILD_SEC=$(shell date +%-S)

OPT_FLAGS = -O2 -funroll-loops -ffast-math -ftree-vectorize -ffunction-sections -fdata-sections
GDB = riscv64-unknown-elf-gdb
BUILD_DIR = .build

# NetSurf Core Libraries Sources
NSFB_SOURCES = $(NSFB_DIR)/src/libnsfb.c $(NSFB_DIR)/src/surface/surface.c $(NSFB_DIR)/src/surface/ram.c \
               $(NSFB_DIR)/src/plot/api.c $(NSFB_DIR)/src/plot/8bpp.c $(NSFB_DIR)/src/plot/16bpp.c \
               $(NSFB_DIR)/src/plot/32bpp-xbgr8888.c $(NSFB_DIR)/src/plot/32bpp-xrgb8888.c \
               $(NSFB_DIR)/src/plot/generic.c $(NSFB_DIR)/src/plot/util.c \
               $(NSFB_DIR)/src/palette.c $(NSFB_DIR)/src/cursor.c

PUTIL_SOURCES = $(WCAP_DIR)/src/libwapcaplet.c \
                $(PUTIL_DIR)/src/charset/aliases.c \
                $(PUTIL_DIR)/src/input/inputstream.c $(PUTIL_DIR)/src/input/filter.c \
                 $(PUTIL_DIR)/src/charset/codec.c \
                $(PUTIL_DIR)/src/charset/codecs/codec_utf8.c \
                $(PUTIL_DIR)/src/charset/codecs/codec_utf16.c \
                $(PUTIL_DIR)/src/charset/codecs/codec_8859.c \
                $(PUTIL_DIR)/src/charset/codecs/codec_ascii.c \
                $(PUTIL_DIR)/src/charset/codecs/codec_ext8.c \
                $(PUTIL_DIR)/src/charset/encodings/utf8.c \
                $(PUTIL_DIR)/src/charset/encodings/utf16.c \
                $(PUTIL_DIR)/src/utils/stack.c $(PUTIL_DIR)/src/utils/vector.c \
                $(PUTIL_DIR)/src/utils/buffer.c $(PUTIL_DIR)/src/utils/errors.c

HUB_SOURCES = $(HUB_DIR)/src/parser.c $(HUB_DIR)/src/tokeniser/tokeniser.c $(HUB_DIR)/src/tokeniser/entities.c \
              $(HUB_DIR)/src/utils/string.c \
              $(HUB_DIR)/src/charset/detect.c \
              $(HUB_DIR)/src/treebuilder/treebuilder.c $(HUB_DIR)/src/treebuilder/initial.c \
              $(HUB_DIR)/src/treebuilder/before_html.c $(HUB_DIR)/src/treebuilder/before_head.c \
              $(HUB_DIR)/src/treebuilder/in_head.c $(HUB_DIR)/src/treebuilder/in_head_noscript.c \
              $(HUB_DIR)/src/treebuilder/after_head.c $(HUB_DIR)/src/treebuilder/in_body.c \
              $(HUB_DIR)/src/treebuilder/after_body.c $(HUB_DIR)/src/treebuilder/after_after_body.c \
              $(HUB_DIR)/src/treebuilder/in_table.c $(HUB_DIR)/src/treebuilder/in_caption.c \
              $(HUB_DIR)/src/treebuilder/in_column_group.c $(HUB_DIR)/src/treebuilder/in_table_body.c \
              $(HUB_DIR)/src/treebuilder/in_row.c $(HUB_DIR)/src/treebuilder/in_cell.c \
              $(HUB_DIR)/src/treebuilder/in_select.c $(HUB_DIR)/src/treebuilder/in_select_in_table.c \
              $(HUB_DIR)/src/treebuilder/in_frameset.c $(HUB_DIR)/src/treebuilder/after_frameset.c \
              $(HUB_DIR)/src/treebuilder/after_after_frameset.c $(HUB_DIR)/src/treebuilder/in_foreign_content.c \
              $(HUB_DIR)/src/treebuilder/generic_rcdata.c $(HUB_DIR)/src/treebuilder/element-type.c

OBJ = start.s sys.s mem.s lib.c timer.c task.c os.c user.c user_utils.c user_netsurf.c tool_editor.c user_wget.c ssh_client.c fs.c 3d.c tool_asm.c app_runtime.c trap.c lock.c plic.c string.c vga.c font.c alloc.c tcc_glue.c $(TCC_DIR)/libtcc.c mbedtls_entropy_compat.c mbedtls_port.c \
      mbedtls_compat.c \
      compat_time.c virtio_disk.c virtio_net.c virtio_input.c ac97_audio.c gbemu_runtime.c gbemu_audio_stub.c \
      gbemu_wasm/lib/cart.c gbemu_wasm/lib/cpu.c gbemu_wasm/lib/cpu_fetch.c gbemu_wasm/lib/cpu_proc.c \
      gbemu_wasm/lib/cpu_util.c gbemu_wasm/lib/dbg.c gbemu_wasm/lib/dma.c gbemu_wasm/lib/gamepad.c \
      gbemu_wasm/lib/interrupts.c gbemu_wasm/lib/instructions.c gbemu_wasm/lib/io.c gbemu_wasm/lib/lcd.c \
      gbemu_wasm/lib/ppu.c gbemu_wasm/lib/ppu_pipeline.c gbemu_wasm/lib/ppu_sm.c gbemu_wasm/lib/ram.c \
      gbemu_wasm/lib/stack.c gbemu_wasm/lib/bus.c gbemu_wasm/lib/timer.c \
      $(NSFB_SOURCES) $(PUTIL_SOURCES) $(HUB_SOURCES) \
      $(LIBSSH2_DIR)/src/agent.c $(LIBSSH2_DIR)/src/bcrypt_pbkdf.c $(LIBSSH2_DIR)/src/chacha.c \
      $(LIBSSH2_DIR)/src/channel.c $(LIBSSH2_DIR)/src/cipher-chachapoly.c $(LIBSSH2_DIR)/src/comp.c \
      $(LIBSSH2_DIR)/src/crypt.c $(LIBSSH2_DIR)/src/global.c $(LIBSSH2_DIR)/src/hostkey.c \
      $(LIBSSH2_DIR)/src/keepalive.c $(LIBSSH2_DIR)/src/kex.c $(LIBSSH2_DIR)/src/knownhost.c \
      $(LIBSSH2_DIR)/src/mac.c $(LIBSSH2_DIR)/src/mbedtls.c $(LIBSSH2_DIR)/src/misc.c \
      $(LIBSSH2_DIR)/src/packet.c $(LIBSSH2_DIR)/src/pem.c $(LIBSSH2_DIR)/src/poly1305.c \
      $(LIBSSH2_DIR)/src/publickey.c $(LIBSSH2_DIR)/src/scp.c $(LIBSSH2_DIR)/src/session.c \
      $(LIBSSH2_DIR)/src/sftp.c $(LIBSSH2_DIR)/src/transport.c $(LIBSSH2_DIR)/src/userauth.c \
      $(LIBSSH2_DIR)/src/userauth_kbd_packet.c $(LIBSSH2_DIR)/src/version.c \
      $(MBEDTLS_DIR)/tf-psa-crypto/extras/md.c \
      $(MBEDTLS_DIR)/tf-psa-crypto/extras/pk.c \
      $(MBEDTLS_DIR)/tf-psa-crypto/extras/pk_wrap.c \
      $(MBEDTLS_DIR)/tf-psa-crypto/extras/pkparse.c \
      $(MBEDTLS_DIR)/tf-psa-crypto/extras/pk_rsa.c \
      $(MBEDTLS_DIR)/tf-psa-crypto/drivers/builtin/src/cipher.c \
      $(MBEDTLS_DIR)/tf-psa-crypto/drivers/builtin/src/cipher_wrap.c \
      $(MBEDTLS_DIR)/tf-psa-crypto/drivers/builtin/src/rsa_alt_helpers.c \
      $(MBEDTLS_DIR)/tf-psa-crypto/drivers/builtin/src/rsa.c \
      $(MBEDTLS_DIR)/tf-psa-crypto/drivers/builtin/src/aes.c \
      $(MBEDTLS_DIR)/tf-psa-crypto/drivers/builtin/src/block_cipher.c \
      $(MBEDTLS_DIR)/tf-psa-crypto/drivers/builtin/src/sha1.c \
      $(MBEDTLS_DIR)/tf-psa-crypto/drivers/builtin/src/sha256.c \
      $(MBEDTLS_DIR)/tf-psa-crypto/drivers/builtin/src/sha512.c \
      $(MBEDTLS_DIR)/tf-psa-crypto/drivers/builtin/src/md5.c \
      $(MBEDTLS_DIR)/tf-psa-crypto/drivers/builtin/src/bignum.c \
      $(MBEDTLS_DIR)/tf-psa-crypto/drivers/builtin/src/bignum_core.c \
      $(MBEDTLS_DIR)/tf-psa-crypto/drivers/builtin/src/psa_util_internal.c \
      $(MBEDTLS_DIR)/tf-psa-crypto/drivers/builtin/src/ctr_drbg.c \
      $(MBEDTLS_DIR)/tf-psa-crypto/drivers/builtin/src/hmac_drbg.c \
      $(MBEDTLS_DIR)/tf-psa-crypto/utilities/base64.c \
      $(MBEDTLS_DIR)/tf-psa-crypto/utilities/pem.c \
      $(MBEDTLS_DIR)/tf-psa-crypto/utilities/oid.c \
      lwip/src/core/def.c lwip/src/core/init.c lwip/src/core/mem.c lwip/src/core/memp.c \
      lwip/src/core/altcp.c lwip/src/core/altcp_alloc.c lwip/src/core/altcp_tcp.c \
      lwip/src/core/netif.c lwip/src/core/pbuf.c lwip/src/core/raw.c lwip/src/core/sys.c \
      lwip/src/core/tcp.c lwip/src/core/tcp_in.c lwip/src/core/tcp_out.c lwip/src/core/udp.c \
      lwip/src/core/ip.c lwip/src/core/dns.c lwip/src/netif/ethernet.c lwip/src/netif/lowpan6.c \
      lwip/src/core/timeouts.c lwip/src/core/inet_chksum.c \
      lwip/src/core/ipv4/ip4.c lwip/src/core/ipv4/acd.c lwip/src/core/ipv4/autoip.c \
      lwip/src/core/ipv4/igmp.c lwip/src/core/ipv4/ip4_addr.c lwip/src/core/ipv4/icmp.c \
      lwip/src/core/ipv4/dhcp.c lwip/src/core/ipv4/etharp.c lwip/src/core/ipv4/ip4_frag.c \
      lwip/src/apps/altcp_tls/altcp_tls_mbedtls.c lwip/src/apps/altcp_tls/altcp_tls_mbedtls_mem.c

QEMU = qemu-system-riscv32
QFLAGS = -smp 1 -machine virt -m 1G -bios none \
         -drive if=none,format=raw,file=hdd.dsk,id=x0 \
         -device virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0 \
         -device virtio-net-device,netdev=net0,mac=52:54:00:12:34:56,bus=virtio-mmio-bus.1,csum=off,guest_csum=off,gso=off,guest_tso4=off,guest_tso6=off,guest_ecn=off,guest_ufo=off,host_tso4=off,host_tso6=off,host_ecn=off,host_ufo=off,mrg_rxbuf=off,rx_queue_size=256,tx_queue_size=256 \
         -device virtio-keyboard-device,bus=virtio-mmio-bus.2 \
         -device virtio-tablet-device,bus=virtio-mmio-bus.3 \
         -netdev tap,id=net0,ifname=tap0,script=no,downscript=no \
         -d guest_errors \
         -device VGA \
         -monitor telnet:localhost:4321,server,nowait

ENABLE_AUDIO ?= 0
ifeq ($(ENABLE_AUDIO),1)
QFLAGS += -audiodev pa,id=snd0,server=unix:/mnt/wslg/PulseServer -device AC97,audiodev=snd0
endif

all: os.elf hdd.dsk qemu

test: os.elf qemu

rebuild: clean os.elf

ALL_SRCS = $(OBJ)
OBJS = $(patsubst %.s,$(BUILD_DIR)/%.o,$(patsubst %.c,$(BUILD_DIR)/%.o,$(ALL_SRCS)))
DEPS = $(OBJS:.o=.d)

os.elf: $(OBJS) $(MBEDTLS_LIBS)
	$(CC) $(CFLAGS) $(OPT_FLAGS) -Wl,--gc-sections -T os.ld -o os.elf $(OBJS) $(MBEDTLS_LIBS) -lgcc

$(BUILD_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(OPT_FLAGS) -MMD -MP -c $< -o $@

$(BUILD_DIR)/%.o: %.s
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(OPT_FLAGS) -MMD -MP -c $< -o $@

-include $(DEPS)

qemu: os.elf hdd.dsk
	$(QEMU) $(QFLAGS) -kernel os.elf

clean:
	rm -rf $(BUILD_DIR) *.elf *.img

hdd.dsk:
	dd if=/dev/urandom of=hdd.dsk bs=1M count=32
