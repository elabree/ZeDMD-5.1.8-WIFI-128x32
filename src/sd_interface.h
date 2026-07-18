#ifndef SD_INTERFACE_H
#define SD_INTERFACE_H
#ifdef SD_MMC_BUILD
  #include <SD_MMC.h>
  #define SD SD_MMC
#else
  #include <SD.h>
#endif // SD_MMC_BUILD
#endif // SD_INTERFACE_H
