#ifndef CONFIG_H
#define CONFIG_H

#define TCC_VERSION "0.9.27"

// 核心環境不支援執行緒，關閉它以避免尋找 semaphore.h
#undef CONFIG_TCC_THREAD

// 核心環境通常不需要 backtrace 與 bcheck，關閉它們以減少依賴
#define CONFIG_TCC_BACKTRACE 0
#define CONFIG_TCC_BCHECK 0

// 設定 TCC 的預設搜尋路徑 (雖然我們用 JIT 模式，但 TCC 內部仍會初始化這些)
#define CONFIG_TCCDIR "/root/lib/tcc"
#define CONFIG_TCC_SYSINCLUDEPATHS "/root/include"
#define CONFIG_TCC_LIBPATHS "/root/lib"

#endif
