/*******************************************************************************
 * tefkernel - 运行时功能开关（仅用于排查问题，正常不依赖）
 *
 * 用法：/tmp/tefk_flags.txt 每行一个开关名，存在即禁用对应功能。
 *   例： printf 'no_resize\nno_item_hooks\n' > /tmp/tefk_flags.txt
 *******************************************************************************/
#ifndef TEFKERNEL_FLAGS_H
#define TEFKERNEL_FLAGS_H

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

static inline bool tef_flag(const char *name) {
  FILE *f = fopen("/tmp/tefk_flags.txt", "r");
  if (!f)
    return false;
  char line[96];
  bool hit = false;
  while (fgets(line, sizeof(line), f)) {
    line[strcspn(line, "\r\n")] = 0;
    if (strcmp(line, name) == 0) {
      hit = true;
      break;
    }
  }
  fclose(f);
  return hit;
}

#endif /* TEFKERNEL_FLAGS_H */
