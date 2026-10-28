/*******************************************************************************
 * tefkernel - item_save
 * Copyright (C) 2026 eternalfuture-e38299
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 *
 * Author: eternalfuture-e38299
 * GitHub: https://github.com/eternalfuture-e38299
 * Created: 2026/10/2
 *
 * 自定义物品存档（伴生文件，不改游戏存档格式）
 *
 * 游戏读档时会把 type >= ItemID.Count 的物品当作“未知物品”直接丢弃
 * （Player.Deserialize 里的 netDefaults(0)，且 ItemID.Count 是 const 无法放宽），
 * 所以背包里的 Mod 物品退出重进就没了。这里把自定义物品按稳定字符串键
 * (loader/mod/name) 另存到“伴生文件”，读档后按槽位塞回去；数字 ID 变化也不影响。
 *
 * 自定义 Buff 也写进玩家伴生文件：自定义 Buff 由内核自管理，runtime_id 不会
 * 写进 player.buffType（原版存档与绘制路径只会看到 0..BuffID.Count-1，因此
 * Main.buffNoSave / Player.buffImmune / TextureAssets.Buff / Lang 缓存等按
 * buff id 定长的原版数组不需要、也不应该为自定义 Buff 扩容），所以这里按稳定
 * 字符串键 (loader/mod/name) 存读；卸载 Mod 后存档里不会残留无效 ID。
 *
 * 文件位置与命名：
 *   - 玩家：<xxx.plr>.tef  （与玩家存档同目录，随其一起备份/迁移）
 *   - 世界：<xxx.wld>.tef  （覆盖世界内所有箱子 Chest.item）
 * 存档格式：v2 二进制（magic = "TEFSAVE2"，带 CRC32 校验）；
 *           检测到旧版 v1 纯文本存档会直接删除，下次保存写入 v2。
 * 写入采用 先写 <path>.tmp 再 rename 的原子替换，避免写坏。
 *******************************************************************************/

#include "internal/terraria/item_save.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "internal/log.h"
#include "internal/terraria/item_manager.h"
#include "terraria/buff_manager.h"
#include "patchlib/field.h"
#include "patchlib/method.h"
#include "patchlib/struct/array.h"
#include "patchlib/struct/string.h"

/* ------------------------------------------------------------------
 * 伴生存档 v2（二进制）
 *
 * v1 是纯文本 TSV：用户随手就能改，字符串重复、解析也慢。
 * v2 结构（所有整数小端）：
 *   magic[8]  "TEFSAVE2"
 *   u32 version (2)
 *   u32 kind    (1 = 玩家，2 = 世界)
 *   u32 item_count
 *   item:
 *     kind == 玩家: string tag
 *     kind == 世界: u32 chest
 *     u32 slot
 *     string loader / string mod / string name
 *     u32 stack / u32 prefix
 *   u32 buff_count（世界文件恒为 0）
 *   buff:
 *     string loader / string mod / string name / u32 time
 *   u32 crc32（覆盖前面全部字节）
 *
 * 读取时兼容 v1 文本；写入一律 v2。
 * crc 只能挡“随手改”，不是加密，挡不住有意的逆向。
 * ------------------------------------------------------------------ */
#define TEF_SAVE_MAGIC "TEFSAVE2"
#define TEF_SAVE_MAGIC_LEN 8
#define TEF_SAVE_VERSION 2u
#define TEF_SAVE_KIND_PLAYER 1u
#define TEF_SAVE_KIND_WORLD 2u

typedef struct {
  unsigned char *data;
  size_t len;
  size_t cap;
  bool ok;
} tef_buf_t;

static bool tef_buf_raw(tef_buf_t *b, const void *p, const size_t n) {
  if (!b->ok)
    return false;
  if (n == 0)
    return true;
  if (b->len + n > b->cap) {
    size_t cap = b->cap ? b->cap : 256;
    while (cap < b->len + n)
      cap *= 2;
    unsigned char *grown = (unsigned char *) realloc(b->data, cap);
    if (!grown) {
      b->ok = false;
      return false;
    }
    b->data = grown;
    b->cap = cap;
  }
  memcpy(b->data + b->len, p, n);
  b->len += n;
  return true;
}

static bool tef_buf_u32(tef_buf_t *b, const uint32_t v) {
  unsigned char tmp[4];
  tmp[0] = (unsigned char) (v & 0xFF);
  tmp[1] = (unsigned char) ((v >> 8) & 0xFF);
  tmp[2] = (unsigned char) ((v >> 16) & 0xFF);
  tmp[3] = (unsigned char) ((v >> 24) & 0xFF);
  return tef_buf_raw(b, tmp, sizeof(tmp));
}

static bool tef_buf_u16(tef_buf_t *b, const uint16_t v) {
  unsigned char tmp[2];
  tmp[0] = (unsigned char) (v & 0xFF);
  tmp[1] = (unsigned char) ((v >> 8) & 0xFF);
  return tef_buf_raw(b, tmp, sizeof(tmp));
}

static bool tef_buf_str(tef_buf_t *b, const char *s) {
  size_t n = s ? strlen(s) : 0;
  if (n > 0xFFFF)
    n = 0xFFFF;
  return tef_buf_u16(b, (uint16_t) n) && tef_buf_raw(b, s, n);
}

static void tef_buf_patch_u32(tef_buf_t *b, const size_t off,
                              const uint32_t v) {
  if (!b->ok || off + 4 > b->len)
    return;
  b->data[off + 0] = (unsigned char) (v & 0xFF);
  b->data[off + 1] = (unsigned char) ((v >> 8) & 0xFF);
  b->data[off + 2] = (unsigned char) ((v >> 16) & 0xFF);
  b->data[off + 3] = (unsigned char) ((v >> 24) & 0xFF);
}

static uint32_t tef_crc32(const unsigned char *p, const size_t n) {
  uint32_t crc = 0xFFFFFFFFu;
  for (size_t i = 0; i < n; ++i) {
    crc ^= p[i];
    for (int k = 0; k < 8; ++k)
      crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t) -(int32_t) (crc & 1u));
  }
  return ~crc;
}

typedef struct {
  const unsigned char *p;
  size_t len;
  size_t off;
} tef_rd_t;

static bool tef_rd_raw(tef_rd_t *r, void *out, const size_t n) {
  if (r->off + n > r->len)
    return false;
  if (out)
    memcpy(out, r->p + r->off, n);
  r->off += n;
  return true;
}

static bool tef_rd_u32(tef_rd_t *r, uint32_t *out) {
  unsigned char tmp[4];
  if (!tef_rd_raw(r, tmp, sizeof(tmp)))
    return false;
  *out = (uint32_t) tmp[0] | ((uint32_t) tmp[1] << 8) |
         ((uint32_t) tmp[2] << 16) | ((uint32_t) tmp[3] << 24);
  return true;
}

static bool tef_rd_u16(tef_rd_t *r, uint16_t *out) {
  unsigned char tmp[2];
  if (!tef_rd_raw(r, tmp, sizeof(tmp)))
    return false;
  *out = (uint16_t) ((uint16_t) tmp[0] | ((uint16_t) tmp[1] << 8));
  return true;
}

static bool tef_rd_str(tef_rd_t *r, char *out, const size_t cap) {
  uint16_t n = 0;
  if (!tef_rd_u16(r, &n))
    return false;
  if (r->off + n > r->len)
    return false;
  const size_t copy = (cap > 0 && n >= cap) ? cap - 1 : n;
  if (copy > 0)
    memcpy(out, r->p + r->off, copy);
  out[copy] = '\0';
  r->off += n;
  return true;
}

static unsigned char *tef_read_all(const char *path, size_t *out_len) {
  *out_len = 0;
  FILE *f = fopen(path, "rb");
  if (!f)
    return NULL;
  if (fseek(f, 0, SEEK_END) != 0) {
    fclose(f);
    return NULL;
  }
  const long size = ftell(f);
  if (size < 0) {
    fclose(f);
    return NULL;
  }
  rewind(f);
  unsigned char *data = (unsigned char *) malloc((size_t) size + 1);
  if (!data) {
    fclose(f);
    return NULL;
  }
  const size_t got = fread(data, 1, (size_t) size, f);
  fclose(f);
  data[got] = '\0';
  *out_len = got;
  return data;
}

static bool tef_write_all(const char *final_path, const void *data,
                          const size_t len) {
  char tmp_path[1400];
  if (snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", final_path) >=
      (int) sizeof(tmp_path))
    return false;
  FILE *f = fopen(tmp_path, "wb");
  if (!f) {
    TEKLOG_WARN("item_save: cannot write %s", tmp_path);
    return false;
  }
  const bool ok = fwrite(data, 1, len, f) == len;
  if (fclose(f) != 0 || !ok) {
    remove(tmp_path);
    TEKLOG_WARN("item_save: write %s failed", tmp_path);
    return false;
  }
  remove(final_path);
  if (rename(tmp_path, final_path) != 0) {
    TEKLOG_WARN("item_save: rename '%s' -> '%s' failed", tmp_path, final_path);
    remove(tmp_path);
    return false;
  }
  return true;
}

// ---- 自定义物品存档所需句柄 ----
static terraria_item_handles_t g_ops = {0};               // Item 字段/方法（来自 item_manager）
static patch_handle_t f_player_name = PATCH_NULL;     // Player.name (string)
static patch_handle_t f_player_inventory = PATCH_NULL; // Player.inventory (Item[])
static patch_handle_t f_player_misc_equips = PATCH_NULL; // Player.miscEquips (Item[])
static patch_handle_t f_player_misc_dyes = PATCH_NULL;   // Player.miscDyes (Item[])
static patch_handle_t f_player_armor = PATCH_NULL;       // Player.armor (Item[])
static patch_handle_t f_player_dye = PATCH_NULL;         // Player.dye (Item[])
static patch_handle_t f_player_bank = PATCH_NULL;        // Player.bank (Chest)
static patch_handle_t f_player_bank2 = PATCH_NULL;       // Player.bank2 (Chest)
static patch_handle_t f_player_bank3 = PATCH_NULL;       // Player.bank3 (Chest)
static patch_handle_t f_player_bank4 = PATCH_NULL;       // Player.bank4 (Chest)
static patch_handle_t f_chest_item = PATCH_NULL;         // Chest.item (Item[])
static patch_handle_t f_chest_max_items = PATCH_NULL;    // Chest.maxItems (int)
static patch_handle_t m_pfd_get_player = PATCH_NULL;  // PlayerFileData.get_Player()
static patch_handle_t m_file_get_path = PATCH_NULL;   // FileData.get_Path()（Player/World 通用）
static patch_handle_t f_main_chest = PATCH_NULL;      // Main.chest (Chest[])
static patch_handle_t f_main_active_world = PATCH_NULL; // Main.ActiveWorldFileData (WorldFileData)
// 移动端 Chest.item 是 ChestItem[]，用以下句柄把槽位与 Item 互转
static patch_handle_t m_item_ctor = PATCH_NULL;          // Item(int, ItemVariant)
static patch_handle_t m_chestitem_expand = PATCH_NULL;   // ChestItem.ExpandItem() -> Item
static patch_handle_t m_chestitem_set_to_item = PATCH_NULL; // ChestItem.SetToItem(Item)

// ------------------------------------------------------------------
// 路径与文件
// ------------------------------------------------------------------

// 调用一个返回 string 的无参方法(get_Path/get_Player 等)，转成 C 字符串。
// 注意：返回的 cstr 由 loader 用 AllocHGlobal 分配，内核侧不释放（释放语义跨平台不一致）。
static char *invoke_get_str(patch_handle_t method, patch_handle_t instance) {
  if (!patchlib_is_valid(method) || !patchlib_is_valid(instance))
    return NULL;
  patch_handle_t s = PATCH_NULL;
  patchlib_method_invoke_args(method, instance, &s, NULL);
  if (!patchlib_is_valid(s))
    return NULL;
  char *c = patchlib_string_cstr(s);
  patchlib_free(s);
  return c;
}

// 由原文件路径构造伴生文件路径：<path>.tef
static bool make_tef_path(const char *base, char *buf, const size_t n) {
  if (!base || !base[0])
    return false;
  if (snprintf(buf, n, "%s.tef", base) >= (int)n)
    return false;
  return true;
}

// ------------------------------------------------------------------
// 玩家容器
// ------------------------------------------------------------------

// 自定义物品存档覆盖的玩家容器标签。前三个在原版读档时会被直接丢弃（必需），
// 其余（装备/染料/猪猪存储）原版按数字 ID 存读，这里额外兜底以便在 Mod
// 集合变化时仍能恢复正确物品或占位。
static const char *g_container_tags[] = {
    "inv",   "misc", "miscdye", "armor", "dye",
    "bank",  "bank2", "bank3",  "bank4"};

// 按标签解析出对应的容器数组（bank* 需要先取 Chest 实例再取 Chest.item）。
// out_is_chest 输出该容器是否为 Chest（移动端其 .item 是 ChestItem[]）。
static patch_handle_t resolve_container_array(const char *tag,
                                              patch_handle_t player,
                                              bool *out_is_chest) {
  patch_handle_t field = PATCH_NULL;
  const bool is_chest = strcmp(tag, "bank") == 0 || strcmp(tag, "bank2") == 0 ||
                        strcmp(tag, "bank3") == 0 || strcmp(tag, "bank4") == 0;
  if (out_is_chest)
    *out_is_chest = is_chest;

  if (strcmp(tag, "inv") == 0)
    field = f_player_inventory;
  else if (strcmp(tag, "misc") == 0)
    field = f_player_misc_equips;
  else if (strcmp(tag, "miscdye") == 0)
    field = f_player_misc_dyes;
  else if (strcmp(tag, "armor") == 0)
    field = f_player_armor;
  else if (strcmp(tag, "dye") == 0)
    field = f_player_dye;
  else if (strcmp(tag, "bank") == 0)
    field = f_player_bank;
  else if (strcmp(tag, "bank2") == 0)
    field = f_player_bank2;
  else if (strcmp(tag, "bank3") == 0)
    field = f_player_bank3;
  else if (strcmp(tag, "bank4") == 0)
    field = f_player_bank4;
  else
    return PATCH_NULL;

  if (!patchlib_is_valid(field))
    return PATCH_NULL;

  patch_handle_t arr = PATCH_NULL;
  if (is_chest) {
    if (!patchlib_is_valid(f_chest_item))
      return PATCH_NULL;
    patch_handle_t chest = PATCH_NULL;
    patchlib_field_get_value(field, player, &chest);
    if (patchlib_is_valid(chest)) {
      patchlib_field_get_value(f_chest_item, chest, &arr);
      patchlib_free(chest);
    }
  } else {
    patchlib_field_get_value(field, player, &arr);
  }
  return arr;
}

// 取某槽位的 Item（非拥有引用，勿释放）。
// 移动端 Chest 的槽位是 ChestItem，用 ExpandItem() 转成 Item；其余平台/容器直接读 Item。
static patch_handle_t container_get_item(patch_handle_t arr, size_t i,
                                         bool is_chest) {
#if defined(__ANDROID__)
  if (is_chest && patchlib_is_valid(m_chestitem_expand)) {
    unsigned char buf[256];
    memset(buf, 0, sizeof(buf));
    if (!patchlib_array_at(arr, i, buf))
      return PATCH_NULL;
    patch_handle_t item = PATCH_NULL;
    patchlib_method_invoke_args(m_chestitem_expand, buf, &item, NULL);
    return item;
  }
#else
  (void)is_chest;
#endif
  patch_handle_t item = PATCH_NULL;
  if (!patchlib_array_at(arr, i, &item))
    return PATCH_NULL;
  return item;
}

// 把某槽位设为自定义物品（移动端 Chest 槽位需 SetToItem 回写）。
static void container_set_custom(patch_handle_t arr, size_t slot, bool is_chest,
                                 int id, int stack, int prefix) {
#if defined(__ANDROID__)
  if (is_chest) {
    if (!patchlib_is_valid(m_item_ctor) ||
        !patchlib_is_valid(m_chestitem_set_to_item))
      return;
    patch_handle_t item = PATCH_NULL;
    patch_handle_t variant = PATCH_NULL;
    void *ca[2] = {&id, &variant};
    patchlib_constructor_invoke(m_item_ctor, &item, ca);
    if (!patchlib_is_valid(item))
      return;
    if (stack > 0)
      patchlib_field_set_value(g_ops.f_stack, item, &stack);
    if (prefix > 0 && patchlib_is_valid(g_ops.m_prefix)) {
      void *pa[1] = {&prefix};
      patchlib_method_invoke_args(g_ops.m_prefix, item, NULL, pa);
    }
    unsigned char buf[256];
    memset(buf, 0, sizeof(buf));
    if (!patchlib_array_at(arr, slot, buf))
      return;
    void *sa[1] = {&item};
    patchlib_method_invoke_args(m_chestitem_set_to_item, buf, NULL, sa);
    patchlib_array_set(arr, slot, buf);
    return;
  }
#else
  (void)is_chest;
#endif
  patch_handle_t item = PATCH_NULL;
  if (!patchlib_array_at(arr, slot, &item) || !patchlib_is_valid(item))
    return;
  int iid = id;
  void *a1[1] = {&iid};
  patchlib_method_invoke_args(g_ops.m_net_defaults, item, NULL, a1);
  if (stack > 0)
    patchlib_field_set_value(g_ops.f_stack, item, &stack);
  if (prefix > 0 && patchlib_is_valid(g_ops.m_prefix)) {
    void *pa[1] = {&prefix};
    patchlib_method_invoke_args(g_ops.m_prefix, item, NULL, pa);
  }
}

/* 把箱子里“超出当前已注册自定义范围”的物品换成内核占位物品。
 * 这些物品通常来自已卸载的 Mod（或 .tef 伴生文件缺失/滞后）。
 * 不清理的话，UI / 绘制按 type 索引原版数组会越界，表现为箱子打不开。 */
static int sanitize_chest(patch_handle_t chest, const int max_valid) {
  if (!patchlib_is_valid(chest) || !patchlib_is_valid(f_chest_item) ||
      !patchlib_is_valid(g_ops.f_type))
    return 0;

  terraria_item_handle_t *placeholder = terraria_item_manager_unknown_item();
  const int placeholder_id = placeholder ? placeholder->runtime_id : -1;
  if (placeholder_id < 0)
    return 0;

  int max_items = 0;
  if (patchlib_is_valid(f_chest_max_items))
    patchlib_field_get_value(f_chest_max_items, chest, &max_items);

  patch_handle_t items = PATCH_NULL;
  patchlib_field_get_value(f_chest_item, chest, &items);
  if (!patchlib_is_valid(items))
    return 0;

  size_t lim = patchlib_array_length(items);
  if (max_items > 0 && (size_t)max_items < lim)
    lim = (size_t)max_items;

  int changed = 0;
  for (size_t si = 0; si < lim; ++si) {
    patch_handle_t item = container_get_item(items, si, true);
    if (!patchlib_is_valid(item))
      continue;

    int type = 0;
    patchlib_field_get_value(g_ops.f_type, item, &type);
    if (type >= max_valid) {
      container_set_custom(items, si, true, placeholder_id, 1, 0);
      changed++;
    }
    patchlib_free(item);
  }

  patchlib_free(items);
  return changed;
}

static void sanitize_world_chests(void) {
  if (!patchlib_is_valid(f_main_chest))
    return;

  patch_handle_t chests = PATCH_NULL;
  patchlib_field_get_value(f_main_chest, PATCH_NULL, &chests);
  if (!patchlib_is_valid(chests))
    return;

  const int max_valid = terraria_item_manager_get_count();
  int changed = 0;
  const size_t count = patchlib_array_length(chests);
  for (size_t i = 0; i < count; ++i) {
    patch_handle_t chest = PATCH_NULL;
    if (!patchlib_array_at(chests, i, &chest) || !patchlib_is_valid(chest))
      continue;
    changed += sanitize_chest(chest, max_valid);
    patchlib_free(chest);
  }
  patchlib_free(chests);

  if (changed > 0)
    TEKLOG_INFO("item_save: replaced %d stale custom item(s) in world chests",
                changed);
}

static void sanitize_player_banks(patch_handle_t player) {
  if (!patchlib_is_valid(player))
    return;

  patch_handle_t banks[4] = {f_player_bank, f_player_bank2, f_player_bank3,
                             f_player_bank4};
  const int max_valid = terraria_item_manager_get_count();
  int changed = 0;
  for (int i = 0; i < 4; ++i) {
    if (!patchlib_is_valid(banks[i]))
      continue;
    patch_handle_t chest = PATCH_NULL;
    patchlib_field_get_value(banks[i], player, &chest);
    if (!patchlib_is_valid(chest))
      continue;
    changed += sanitize_chest(chest, max_valid);
    patchlib_free(chest);
  }

  if (changed > 0)
    TEKLOG_INFO("item_save: replaced %d stale custom item(s) in banks", changed);
}

/* 把某个槽位的自定义物品追加成一条二进制记录；返回是否写出。 */
static bool append_item_record(tef_buf_t *b, const bool world, const char *tag,
                               const size_t a, const size_t slot,
                               patch_handle_t item) {
  const int base = terraria_item_id_get_count();
  const size_t custom = tefstd_vector_size(&g_terraria_item_registry);

  int type = 0;
  patchlib_field_get_value(g_ops.f_type, item, &type);
  const int nid = type - base;
  if (nid < 0 || nid >= (int) custom)
    return false;

  terraria_item_handle_t **pp =
      tefstd_vector_at(&g_terraria_item_registry, nid);
  if (!pp || !*pp)
    return false;

  int stack = 0;
  patchlib_field_get_value(g_ops.f_stack, item, &stack);
  if (stack <= 0)
    return false;

  unsigned char prefix = 0;
  if (patchlib_is_valid(g_ops.f_item_prefix))
    patchlib_field_get_value(g_ops.f_item_prefix, item, &prefix);

  const char *loader =
      (*pp)->parent_modloader_id ? (*pp)->parent_modloader_id : "?";
  const char *mod = (*pp)->parent_id ? (*pp)->parent_id : "?";
  const char *name = (*pp)->internal_name ? (*pp)->internal_name : "?";

  if (world) {
    if (!tef_buf_u32(b, (uint32_t) a))
      return false;
  } else {
    if (!tef_buf_str(b, tag))
      return false;
  }
  return tef_buf_u32(b, (uint32_t) slot) && tef_buf_str(b, loader) &&
         tef_buf_str(b, mod) && tef_buf_str(b, name) &&
         tef_buf_u32(b, (uint32_t) stack) && tef_buf_u32(b, (uint32_t) prefix);
}

// 把玩家某个容器里的自定义物品写进二进制缓冲，返回写入条数
static int save_one_container(tef_buf_t *b, const char *tag, patch_handle_t arr,
                              bool is_chest) {
  if (!patchlib_is_valid(arr))
    return 0;

  const size_t len = patchlib_array_length(arr);
  int saved = 0;

  for (size_t i = 0; i < len; ++i) {
    patch_handle_t item = container_get_item(arr, i, is_chest);
    if (!patchlib_is_valid(item))
      continue;
    if (append_item_record(b, false, tag, 0, i, item))
      saved++;
    patchlib_free(item);
  }
  return saved;
}

// ------------------------------------------------------------------
// 自定义 Buff（伴生存档）
// ------------------------------------------------------------------

struct buff_save_ctx {
  tef_buf_t *b;
  int count;
};

static void save_buff_line(void *user, terraria_buff_handle_t *buff, int time) {
  struct buff_save_ctx *ctx = (struct buff_save_ctx *) user;
  if (!ctx || !ctx->b || !buff || time <= 0) return;
  const char *loader =
      buff->parent_modloader_id ? buff->parent_modloader_id : "?";
  const char *mod = buff->parent_id ? buff->parent_id : "?";
  const char *name = buff->internal_name ? buff->internal_name : "?";
  if (tef_buf_str(ctx->b, loader) && tef_buf_str(ctx->b, mod) &&
      tef_buf_str(ctx->b, name) &&
      tef_buf_u32(ctx->b, (uint32_t) time))
    ctx->count++;
}

static void save_player_items(patch_handle_t player, patch_handle_t pfd) {
  if (!patchlib_is_valid(player))
    return;

  char *plr = invoke_get_str(m_file_get_path, pfd);
  if (!plr) {
    TEKLOG_WARN("item_save: cannot resolve player file path; skip");
    return;
  }

  char final_path[1400];
  if (!make_tef_path(plr, final_path, sizeof(final_path)))
    return;

  tef_buf_t b = {0};
  b.ok = true;

  tef_buf_raw(&b, TEF_SAVE_MAGIC, TEF_SAVE_MAGIC_LEN);
  tef_buf_u32(&b, TEF_SAVE_VERSION);
  tef_buf_u32(&b, TEF_SAVE_KIND_PLAYER);
  const size_t item_count_pos = b.len;
  tef_buf_u32(&b, 0);

  int saved = 0;
  for (size_t t = 0;
       t < sizeof(g_container_tags) / sizeof(g_container_tags[0]); ++t) {
    bool is_chest = false;
    patch_handle_t arr =
        resolve_container_array(g_container_tags[t], player, &is_chest);
    if (patchlib_is_valid(arr)) {
      saved += save_one_container(&b, g_container_tags[t], arr, is_chest);
      patchlib_free(arr);
    }
  }
  tef_buf_patch_u32(&b, item_count_pos, (uint32_t) saved);

  const size_t buff_count_pos = b.len;
  tef_buf_u32(&b, 0);
  struct buff_save_ctx bctx = {&b, 0};
  terraria_buff_manager_visit_active(player, save_buff_line, &bctx);
  tef_buf_patch_u32(&b, buff_count_pos, (uint32_t) bctx.count);

  const uint32_t crc = b.ok ? tef_crc32(b.data, b.len) : 0;
  tef_buf_u32(&b, crc);

  if (b.ok && tef_write_all(final_path, b.data, b.len))
    TEKLOG_DEBUG("item_save: saved %d item(s), %d buff(s) to %s", saved,
                 bctx.count, final_path);
  free(b.data);
}

/* 把一条玩家物品记录恢复到对应容器；未注册的 Mod 用 UNKNOWN 占位。 */
static void apply_player_item(patch_handle_t player, const char *tag,
                              const size_t slot, const char *loader,
                              const char *mod, const char *item_name,
                              const int stack, const int prefix) {
  bool is_chest = false;
  patch_handle_t arr = resolve_container_array(tag, player, &is_chest);
  if (!patchlib_is_valid(arr))
    return;

  terraria_item_handle_t *h =
      terraria_item_manager_find_item(loader, mod, item_name);
  if (!h || h->runtime_id < 0) {
    TEKLOG_WARN("item_save: mod item %s.%s.%s not found, using placeholder",
                loader, mod, item_name);
    h = terraria_item_manager_unknown_item();
  }
  if (!h || h->runtime_id < 0) {
    patchlib_free(arr);
    return;
  }
  if (slot >= patchlib_array_length(arr)) {
    patchlib_free(arr);
    return;
  }

  container_set_custom(arr, slot, is_chest, h->runtime_id, stack, prefix);
  patchlib_free(arr);
}

static void load_player_items(patch_handle_t player, patch_handle_t pfd) {
  if (!patchlib_is_valid(player))
    return;

  char *plr = invoke_get_str(m_file_get_path, pfd);
  if (!plr)
    return;

  char final_path[1400];
  if (!make_tef_path(plr, final_path, sizeof(final_path)))
    return;

  size_t len = 0;
  unsigned char *data = tef_read_all(final_path, &len);
  if (!data)
    return;

  if (len < 20 || memcmp(data, TEF_SAVE_MAGIC, TEF_SAVE_MAGIC_LEN) != 0) {
    /* 旧版纯文本存档：不读取，直接删掉（下次保存会写 v2） */
    TEKLOG_INFO("item_save: %s 是旧版文本存档，已删除", final_path);
    free(data);
    remove(final_path);
    return;
  }

  const size_t body = len - 4;
  const uint32_t stored =
      (uint32_t) data[body] | ((uint32_t) data[body + 1] << 8) |
      ((uint32_t) data[body + 2] << 16) | ((uint32_t) data[body + 3] << 24);
  if (tef_crc32(data, body) != stored) {
    TEKLOG_WARN("item_save: %s CRC 校验失败，已忽略", final_path);
    free(data);
    return;
  }

  tef_rd_t r = {data, body, TEF_SAVE_MAGIC_LEN};
  uint32_t version = 0, kind = 0, item_count = 0, buff_count = 0;
  if (!tef_rd_u32(&r, &version) || !tef_rd_u32(&r, &kind) ||
      !tef_rd_u32(&r, &item_count) || version != TEF_SAVE_VERSION ||
      kind != TEF_SAVE_KIND_PLAYER) {
    TEKLOG_WARN("item_save: %s 头部无效，已忽略", final_path);
    free(data);
    return;
  }

  int restored = 0;
  for (uint32_t i = 0; i < item_count; ++i) {
    char tag[32] = {0};
    char loader[160] = {0}, mod[160] = {0}, item_name[160] = {0};
    uint32_t slot = 0, stack = 0, prefix = 0;
    if (!tef_rd_str(&r, tag, sizeof(tag)) || !tef_rd_u32(&r, &slot) ||
        !tef_rd_str(&r, loader, sizeof(loader)) ||
        !tef_rd_str(&r, mod, sizeof(mod)) ||
        !tef_rd_str(&r, item_name, sizeof(item_name)) ||
        !tef_rd_u32(&r, &stack) || !tef_rd_u32(&r, &prefix))
      break;
    if (tag[0] && stack > 0) {
      apply_player_item(player, tag, (size_t) slot, loader, mod, item_name,
                        (int) stack, (int) prefix);
      restored++;
    }
  }

  if (!tef_rd_u32(&r, &buff_count))
    buff_count = 0;
  int buffs = 0;
  for (uint32_t i = 0; i < buff_count; ++i) {
    char loader[160] = {0}, mod[160] = {0}, name[160] = {0};
    uint32_t time = 0;
    if (!tef_rd_str(&r, loader, sizeof(loader)) ||
        !tef_rd_str(&r, mod, sizeof(mod)) ||
        !tef_rd_str(&r, name, sizeof(name)) || !tef_rd_u32(&r, &time))
      break;
    if (time == 0)
      continue;
    terraria_buff_handle_t *bh =
        terraria_buff_manager_find_buff(loader, mod, name);
    if (bh) {
      terraria_buff_manager_add(bh, player, (int) time);
      buffs++;
    } else {
      TEKLOG_WARN("item_save: buff %s.%s.%s not found (mod unloaded?)",
                  loader, mod, name);
    }
  }

  free(data);
  TEKLOG_DEBUG("item_save: restored %d item(s), %d buff(s) from %s", restored,
               buffs, final_path);
}

static void player_save_postfix(patch_handle_t this, void **args, void *result,
                                const patch_method_signature_t *sig) {
  (void)this;
  (void)result;
  (void)sig;
  if (!args || !args[0])
    return;
  patch_handle_t pfd = *(patch_handle_t *)args[0];
  if (!patchlib_is_valid(pfd) || !patchlib_is_valid(m_pfd_get_player))
    return;
  patch_handle_t player = PATCH_NULL;
  patchlib_method_invoke_args(m_pfd_get_player, pfd, &player, NULL);
  save_player_items(player, pfd);
}

static void player_load_postfix(patch_handle_t this, void **args, void *result,
                                const patch_method_signature_t *sig) {
  (void)this;
  (void)args;
  (void)sig;
  if (!result)
    return;
  patch_handle_t pfd = *(patch_handle_t *)result;
  if (!patchlib_is_valid(pfd) || !patchlib_is_valid(m_pfd_get_player))
    return;
  patch_handle_t player = PATCH_NULL;
  patchlib_method_invoke_args(m_pfd_get_player, pfd, &player, NULL);
  load_player_items(player, pfd);
  sanitize_player_banks(player);
}

// ------------------------------------------------------------------
// 世界箱子（Main.chest）
// ------------------------------------------------------------------

static char *world_tef_base() {
  if (!patchlib_is_valid(f_main_active_world))
    return NULL;
  patch_handle_t wfd = PATCH_NULL;
  patchlib_field_get_value(f_main_active_world, PATCH_NULL, &wfd);
  if (!patchlib_is_valid(wfd))
    return NULL;
  char *p = invoke_get_str(m_file_get_path, wfd);
  patchlib_free(wfd);
  return p;
}

static void save_world_chests() {
  if (!patchlib_is_valid(f_main_chest) || !patchlib_is_valid(f_chest_item))
    return;

  char *wld = world_tef_base();
  if (!wld || !wld[0]) {
    TEKLOG_WARN("item_save: cannot resolve world path; skip chest save");
    return;
  }

  char final_path[1400];
  if (!make_tef_path(wld, final_path, sizeof(final_path)))
    return;

  tef_buf_t b = {0};
  b.ok = true;

  tef_buf_raw(&b, TEF_SAVE_MAGIC, TEF_SAVE_MAGIC_LEN);
  tef_buf_u32(&b, TEF_SAVE_VERSION);
  tef_buf_u32(&b, TEF_SAVE_KIND_WORLD);
  const size_t item_count_pos = b.len;
  tef_buf_u32(&b, 0);

  int saved = 0;
  patch_handle_t chests = PATCH_NULL;
  patchlib_field_get_value(f_main_chest, PATCH_NULL, &chests);
  if (patchlib_is_valid(chests)) {
    const size_t chest_count = patchlib_array_length(chests);
    for (size_t ci = 0; ci < chest_count; ++ci) {
      patch_handle_t chest = PATCH_NULL;
      if (!patchlib_array_at(chests, ci, &chest) || !patchlib_is_valid(chest))
        continue;

      int max_items = 0;
      if (patchlib_is_valid(f_chest_max_items))
        patchlib_field_get_value(f_chest_max_items, chest, &max_items);

      patch_handle_t items = PATCH_NULL;
      patchlib_field_get_value(f_chest_item, chest, &items);
      if (patchlib_is_valid(items)) {
        size_t lim = patchlib_array_length(items);
        if (max_items > 0 && (size_t) max_items < lim)
          lim = (size_t) max_items;
        for (size_t si = 0; si < lim; ++si) {
          patch_handle_t item = container_get_item(items, si, true);
          if (!patchlib_is_valid(item))
            continue;
          if (append_item_record(&b, true, NULL, ci, si, item))
            saved++;
          patchlib_free(item);
        }
        patchlib_free(items);
      }
      patchlib_free(chest);
    }
    patchlib_free(chests);
  }
  tef_buf_patch_u32(&b, item_count_pos, (uint32_t) saved);

  /* 世界文件没有 buff 段，但保留计数字段让格式一致 */
  tef_buf_u32(&b, 0);

  const uint32_t crc = b.ok ? tef_crc32(b.data, b.len) : 0;
  tef_buf_u32(&b, crc);

  if (b.ok && tef_write_all(final_path, b.data, b.len))
    TEKLOG_DEBUG("item_save: saved %d item(s) in world chests to %s", saved,
                 final_path);
  free(b.data);
}

/* 把一条箱子物品记录恢复到 Main.chest；未注册的 Mod 用 UNKNOWN 占位。 */
static void apply_world_item(patch_handle_t chests, const size_t chest_idx,
                             const size_t slot, const char *loader,
                             const char *mod, const char *item_name,
                             const int stack, const int prefix) {
  if (chest_idx >= patchlib_array_length(chests))
    return;

  patch_handle_t chest = PATCH_NULL;
  if (!patchlib_array_at(chests, chest_idx, &chest) || !patchlib_is_valid(chest))
    return;

  patch_handle_t items = PATCH_NULL;
  patchlib_field_get_value(f_chest_item, chest, &items);
  if (!patchlib_is_valid(items) || slot >= patchlib_array_length(items)) {
    if (patchlib_is_valid(items))
      patchlib_free(items);
    patchlib_free(chest);
    return;
  }

  terraria_item_handle_t *h =
      terraria_item_manager_find_item(loader, mod, item_name);
  if (!h || h->runtime_id < 0) {
    TEKLOG_WARN("item_save: chest mod item %s.%s.%s not found, using placeholder",
                loader, mod, item_name);
    h = terraria_item_manager_unknown_item();
  }
  if (!h || h->runtime_id < 0) {
    patchlib_free(items);
    patchlib_free(chest);
    return;
  }

  container_set_custom(items, slot, true, h->runtime_id, stack, prefix);
  patchlib_free(items);
  patchlib_free(chest);
}

static void load_world_chests() {
  if (!patchlib_is_valid(f_main_chest) || !patchlib_is_valid(f_chest_item))
    return;

  char *wld = world_tef_base();
  if (!wld || !wld[0])
    return;

  char final_path[1400];
  if (!make_tef_path(wld, final_path, sizeof(final_path)))
    return;

  size_t len = 0;
  unsigned char *data = tef_read_all(final_path, &len);
  if (!data)
    return;

  if (len < 20 || memcmp(data, TEF_SAVE_MAGIC, TEF_SAVE_MAGIC_LEN) != 0) {
    TEKLOG_INFO("item_save: %s 是旧版文本存档，已删除", final_path);
    free(data);
    remove(final_path);
    return;
  }

  const size_t body = len - 4;
  const uint32_t stored =
      (uint32_t) data[body] | ((uint32_t) data[body + 1] << 8) |
      ((uint32_t) data[body + 2] << 16) | ((uint32_t) data[body + 3] << 24);
  if (tef_crc32(data, body) != stored) {
    TEKLOG_WARN("item_save: %s CRC 校验失败，已忽略", final_path);
    free(data);
    return;
  }

  tef_rd_t r = {data, body, TEF_SAVE_MAGIC_LEN};
  uint32_t version = 0, kind = 0, item_count = 0;
  if (!tef_rd_u32(&r, &version) || !tef_rd_u32(&r, &kind) ||
      !tef_rd_u32(&r, &item_count) || version != TEF_SAVE_VERSION ||
      kind != TEF_SAVE_KIND_WORLD) {
    TEKLOG_WARN("item_save: %s 头部无效，已忽略", final_path);
    free(data);
    return;
  }

  patch_handle_t chests = PATCH_NULL;
  patchlib_field_get_value(f_main_chest, PATCH_NULL, &chests);
  if (!patchlib_is_valid(chests)) {
    free(data);
    return;
  }

  int restored = 0;
  for (uint32_t i = 0; i < item_count; ++i) {
    char loader[160] = {0}, mod[160] = {0}, item_name[160] = {0};
    uint32_t chest_idx = 0, slot = 0, stack = 0, prefix = 0;
    if (!tef_rd_u32(&r, &chest_idx) || !tef_rd_u32(&r, &slot) ||
        !tef_rd_str(&r, loader, sizeof(loader)) ||
        !tef_rd_str(&r, mod, sizeof(mod)) ||
        !tef_rd_str(&r, item_name, sizeof(item_name)) ||
        !tef_rd_u32(&r, &stack) || !tef_rd_u32(&r, &prefix))
      break;
    if (stack == 0)
      continue;
    apply_world_item(chests, (size_t) chest_idx, (size_t) slot, loader, mod,
                     item_name, (int) stack, (int) prefix);
    restored++;
  }

  patchlib_free(chests);
  free(data);
  TEKLOG_DEBUG("item_save: restored %d item(s) in world chests from %s",
               restored, final_path);
}

static void chest_save_postfix(patch_handle_t this, void **args, void *result,
                               const patch_method_signature_t *sig) {
  (void)this;
  (void)args;
  (void)result;
  (void)sig;
  save_world_chests();
}

static void chest_load_postfix(patch_handle_t this, void **args, void *result,
                               const patch_method_signature_t *sig) {
  (void)this;
  (void)args;
  (void)result;
  (void)sig;
  load_world_chests();
  sanitize_world_chests();
}

// ------------------------------------------------------------------
// 初始化
// ------------------------------------------------------------------

void terraria_item_save_init() {
  terraria_item_manager_get_item_ops(&g_ops);

  patch_handle_t player_class = patchlib_type_get_type("Terraria", "Player");
  if (!patchlib_is_valid(player_class)) {
    TEKLOG_WARN("item_save: Terraria.Player not found; item save disabled");
    return;
  }

  f_player_name = patchlib_type_get_field(player_class, "name");
  f_player_inventory = patchlib_type_get_field(player_class, "inventory");
  f_player_misc_equips = patchlib_type_get_field(player_class, "miscEquips");
  f_player_misc_dyes = patchlib_type_get_field(player_class, "miscDyes");
  f_player_armor = patchlib_type_get_field(player_class, "armor");
  f_player_dye = patchlib_type_get_field(player_class, "dye");
  f_player_bank = patchlib_type_get_field(player_class, "bank");
  f_player_bank2 = patchlib_type_get_field(player_class, "bank2");
  f_player_bank3 = patchlib_type_get_field(player_class, "bank3");
  f_player_bank4 = patchlib_type_get_field(player_class, "bank4");

  patch_handle_t chest_class = patchlib_type_get_type("Terraria", "Chest");
  if (patchlib_is_valid(chest_class)) {
    f_chest_item = patchlib_type_get_field(chest_class, "item");
    f_chest_max_items = patchlib_type_get_field(chest_class, "maxItems");
    patchlib_free(chest_class);
  }

  // 移动端：ChestItem <-> Item 转换 + Item 构造
  patch_handle_t item_class = patchlib_type_get_type("Terraria", "Item");
  if (patchlib_is_valid(item_class)) {
    m_item_ctor = patchlib_type_get_method_by_param_count(item_class, ".ctor", 2);
    patchlib_free(item_class);
  }
  patch_handle_t chestitem_class =
      patchlib_type_get_type("Terraria", "ChestItem");
  if (patchlib_is_valid(chestitem_class)) {
    m_chestitem_expand =
        patchlib_type_get_method_by_param_count(chestitem_class, "ExpandItem", 0);
    m_chestitem_set_to_item =
        patchlib_type_get_method_by_param_count(chestitem_class, "SetToItem", 1);
    patchlib_free(chestitem_class);
  }

  // FileData.get_Path()（PlayerFileData / WorldFileData 共用）
  patch_handle_t filedata_class = patchlib_type_get_type("Terraria.IO", "FileData");
  if (patchlib_is_valid(filedata_class)) {
    m_file_get_path =
        patchlib_type_get_method_by_param_count(filedata_class, "get_Path", 0);
    patchlib_free(filedata_class);
  }

  patch_handle_t pfd_class = patchlib_type_get_type("Terraria.IO", "PlayerFileData");
  if (patchlib_is_valid(pfd_class)) {
    m_pfd_get_player =
        patchlib_type_get_method_by_param_count(pfd_class, "get_Player", 0);
    patchlib_free(pfd_class);
  }

  // Main.chest / Main.ActiveWorldFileData
  patch_handle_t main_class = patchlib_type_get_type("Terraria", "Main");
  if (patchlib_is_valid(main_class)) {
    f_main_chest = patchlib_type_get_field(main_class, "chest");
    f_main_active_world =
        patchlib_type_get_field(main_class, "ActiveWorldFileData");
    patchlib_free(main_class);
  }

  // ---- 钩子：玩家存档 ----
  patch_handle_t save_player =
      patchlib_type_get_method_by_param_count(player_class, "SavePlayer", 3);
  if (patchlib_is_valid(save_player)) {
    patchlib_install_prepost_hook(save_player, NULL, player_save_postfix);
    TEKLOG_INFO("item_save: hooked Player.SavePlayer");
    patchlib_free(save_player);
  }

  patch_handle_t load_player =
      patchlib_type_get_method_by_param_count(player_class, "LoadPlayer", 2);
  if (patchlib_is_valid(load_player)) {
    patchlib_install_prepost_hook(load_player, NULL, player_load_postfix);
    TEKLOG_INFO("item_save: hooked Player.LoadPlayer");
    patchlib_free(load_player);
  }

  // ---- 钩子：世界箱子 ----
  patch_handle_t worldfile_class = patchlib_type_get_type("Terraria.IO", "WorldFile");
  if (patchlib_is_valid(worldfile_class)) {
    patch_handle_t save_chests = patchlib_type_get_method_by_param_count(
        worldfile_class, "SaveChests", 1);
    if (patchlib_is_valid(save_chests)) {
      patchlib_install_prepost_hook(save_chests, NULL, chest_save_postfix);
      TEKLOG_INFO("item_save: hooked WorldFile.SaveChests");
      patchlib_free(save_chests);
    }
    patch_handle_t load_chests = patchlib_type_get_method_by_param_count(
        worldfile_class, "LoadChests", 1);
    if (patchlib_is_valid(load_chests)) {
      patchlib_install_prepost_hook(load_chests, NULL, chest_load_postfix);
      TEKLOG_INFO("item_save: hooked WorldFile.LoadChests");
      patchlib_free(load_chests);
    }
    patchlib_free(worldfile_class);
  }

  patchlib_free(player_class);
}
