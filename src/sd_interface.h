#pragma once
#ifdef SD_MMC_BUILD
  #include <SD_MMC.h>
  #define SD SD_MMC
#else
  #include <SD.h>
#endif
