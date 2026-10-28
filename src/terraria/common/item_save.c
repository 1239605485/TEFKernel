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
 * 写入采用 先写 <path>.tmp 再 rename 的原子替换，避免写坏。
 *******************************************************************************/

#include "internal/terraria/item_save.h"

#include <stdio.h>
#include <string.h>

#include "internal/log.h"
#include "internal/terraria/item_manager.h"
#include "terraria/buff_manager.h"
#include "patchlib/field.h"
#include "patchlib/method.h"
#include "patchlib/struct/array.h"
#include "patchlib/struct/string.h"

// 伴生文件头（读取时跳过以 '#' 开头的行）
static const char *TEF_HEADER = "#tefkernel-item v1";

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

static FILE *tef_open_tmp(const char *final_path, char *tmp_path, const size_t n) {
  if (snprintf(tmp_path, n, "%s.tmp", final_path) >= (int)n)
    return NULL;
  return fopen(tmp_path, "w");
}

static void tef_commit(FILE *f, const char *tmp_path, const char *final_path) {
  if (f)
    fclose(f);
  remove(final_path); // Windows 上 rename 不能覆盖已存在文件
  if (rename(tmp_path, final_path) != 0)
    TEKLOG_WARN("item_save: rename '%s' -> '%s' failed", tmp_path, final_path);
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

// 当某个槽位里的物品是“自定义物品”时，把它的稳定键写出一行；返回是否写出。
static bool write_custom_item_line(FILE *f, const size_t a, const size_t b,
                                   patch_handle_t item) {
  const int base = terraria_item_id_get_count();
  const size_t custom = tefstd_vector_size(&g_terraria_item_registry);

  int type = 0;
  patchlib_field_get_value(g_ops.f_type, item, &type);
  const int nid = type - base;
  if (nid < 0 || nid >= (int)custom)
    return false;

  terraria_item_handle_t **pp = tefstd_vector_at(&g_terraria_item_registry, nid);
  if (!pp || !*pp)
    return false;

  int stack = 0;
  patchlib_field_get_value(g_ops.f_stack, item, &stack);
  if (stack <= 0)
    return false;

  unsigned char prefix = 0;
  if (patchlib_is_valid(g_ops.f_item_prefix))
    patchlib_field_get_value(g_ops.f_item_prefix, item, &prefix);

  fprintf(f, "%zu\t%zu\t%s\t%s\t%s\t%d\t%d\n", a, b,
          (*pp)->parent_modloader_id ? (*pp)->parent_modloader_id : "?",
          (*pp)->parent_id ? (*pp)->parent_id : "?",
          (*pp)->internal_name ? (*pp)->internal_name : "?", stack,
          (int)prefix);
  return true;
}

// 把玩家某个容器里的自定义物品写进文件，返回写入条数
static int save_one_container(FILE *f, const char *tag, patch_handle_t arr,
                              bool is_chest) {
  if (!patchlib_is_valid(arr))
    return 0;

  const size_t len = patchlib_array_length(arr);
  int saved = 0;

  for (size_t i = 0; i < len; ++i) {
    patch_handle_t item = container_get_item(arr, i, is_chest);
    if (!patchlib_is_valid(item))
      continue;

    // 行格式：tag slot loader mod name stack prefix
    // 先借用一个临时缓冲判断是否是自定义物品，再按带 tag 的格式输出。
    const int base = terraria_item_id_get_count();
    const size_t custom = tefstd_vector_size(&g_terraria_item_registry);
    int type = 0;
    patchlib_field_get_value(g_ops.f_type, item, &type);
    const int nid = type - base;
    if (nid < 0 || nid >= (int)custom)
      continue;

    terraria_item_handle_t **pp =
        tefstd_vector_at(&g_terraria_item_registry, nid);
    if (!pp || !*pp)
      continue;

    int stack = 0;
    patchlib_field_get_value(g_ops.f_stack, item, &stack);
    if (stack <= 0)
      continue;

    unsigned char prefix = 0;
    if (patchlib_is_valid(g_ops.f_item_prefix))
      patchlib_field_get_value(g_ops.f_item_prefix, item, &prefix);

    fprintf(f, "%s\t%zu\t%s\t%s\t%s\t%d\t%d\n", tag, i,
            (*pp)->parent_modloader_id ? (*pp)->parent_modloader_id : "?",
            (*pp)->parent_id ? (*pp)->parent_id : "?",
            (*pp)->internal_name ? (*pp)->internal_name : "?", stack,
            (int)prefix);
    saved++;
  }
  return saved;
}

// ------------------------------------------------------------------
// 自定义 Buff（伴生存档）
// ------------------------------------------------------------------

struct buff_save_ctx {
  FILE *f;
  int count;
};

static void save_buff_line(void *user, terraria_buff_handle_t *buff, int time) {
  struct buff_save_ctx *ctx = (struct buff_save_ctx *) user;
  if (!ctx || !ctx->f || !buff || time <= 0) return;
  fprintf(ctx->f, "buff\t%s\t%s\t%s\t%d\n",
          buff->parent_modloader_id ? buff->parent_modloader_id : "?",
          buff->parent_id ? buff->parent_id : "?",
          buff->internal_name ? buff->internal_name : "?", time);
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

  char tmp_path[1400];
  FILE *f = tef_open_tmp(final_path, tmp_path, sizeof(tmp_path));
  if (!f) {
    TEKLOG_WARN("item_save: cannot write %s", final_path);
    return;
  }

  fprintf(f, "%s\n", TEF_HEADER);

  int saved = 0;
  for (size_t t = 0;
       t < sizeof(g_container_tags) / sizeof(g_container_tags[0]); ++t) {
    bool is_chest = false;
    patch_handle_t arr =
        resolve_container_array(g_container_tags[t], player, &is_chest);
    if (patchlib_is_valid(arr)) {
      saved += save_one_container(f, g_container_tags[t], arr, is_chest);
      patchlib_free(arr);
    }
  }

  struct buff_save_ctx bctx = {f, 0};
  terraria_buff_manager_visit_active(player, save_buff_line, &bctx);

  tef_commit(f, tmp_path, final_path);
  TEKLOG_DEBUG("item_save: saved %d custom item(s), %d buff(s) to %s", saved,
               bctx.count, final_path);
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

  FILE *f = fopen(final_path, "r");
  if (!f)
    return;

  char line[700];
  int restored = 0;
  while (fgets(line, sizeof(line), f)) {
    if (line[0] == '#')
      continue;

    if (strncmp(line, "buff\t", 5) == 0) {
      char bloader[160] = {0}, bmod[160] = {0}, bname[160] = {0};
      int btime = 0;
      if (sscanf(line, "buff\t%159[^\t]\t%159[^\t]\t%159[^\t]\t%d",
                 bloader, bmod, bname, &btime) == 4 && btime > 0) {
        terraria_buff_handle_t *bh = terraria_buff_manager_find_buff(bloader, bmod, bname);
        if (bh) {
          terraria_buff_manager_add(bh, player, btime);
          restored++;
        } else {
          TEKLOG_WARN("item_save: buff %s.%s.%s not found (mod unloaded?)",
                      bloader, bmod, bname);
        }
      }
      continue;
    }

    char tag[32] = {0};
    size_t slot = 0;
    char loader[160] = {0}, mod[160] = {0}, item_name[160] = {0};
    int stack = 0, prefix = 0;
    if (sscanf(line, "%31[^\t]\t%zu\t%159[^\t]\t%159[^\t]\t%159[^\t]\t%d\t%d",
               tag, &slot, loader, mod, item_name, &stack, &prefix) != 7)
      continue;

    bool is_chest = false;
    patch_handle_t arr = resolve_container_array(tag, player, &is_chest);
    if (!patchlib_is_valid(arr))
      continue;

    terraria_item_handle_t *h =
        terraria_item_manager_find_item(loader, mod, item_name);
    if (!h || h->runtime_id < 0) {
      // Mod 已被卸载：用内核预留的“未知物品”占位，保留槽位而不是直接丢掉
      TEKLOG_WARN("item_save: mod item %s.%s.%s not found, using placeholder",
                  loader, mod, item_name);
      h = terraria_item_manager_unknown_item();
    }
    if (!h || h->runtime_id < 0) {
      patchlib_free(arr);
      continue;
    }

    // 槽位越界保护（存档被手动修改或容器长度变化时会越界）
    if (slot >= patchlib_array_length(arr)) {
      patchlib_free(arr);
      continue;
    }

    // netDefaults/createItem 会触发 set_defaults_postfix，套用自定义默认值
    container_set_custom(arr, slot, is_chest, h->runtime_id, stack, prefix);
    restored++;
    patchlib_free(arr);
  }

  fclose(f);
  TEKLOG_DEBUG("item_save: restored %d custom item(s) from %s", restored,
               final_path);
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

  char tmp_path[1400];
  FILE *f = tef_open_tmp(final_path, tmp_path, sizeof(tmp_path));
  if (!f) {
    TEKLOG_WARN("item_save: cannot write %s", final_path);
    return;
  }

  fprintf(f, "%s\n", TEF_HEADER);

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
        if (max_items > 0 && (size_t)max_items < lim)
          lim = (size_t)max_items;
        for (size_t si = 0; si < lim; ++si) {
          patch_handle_t item = container_get_item(items, si, true);
          if (!patchlib_is_valid(item))
            continue;
          if (write_custom_item_line(f, ci, si, item))
            saved++;
        }
        patchlib_free(items);
      }
      patchlib_free(chest);
    }
    patchlib_free(chests);
  }

  tef_commit(f, tmp_path, final_path);
  TEKLOG_DEBUG("item_save: saved %d custom item(s) in world chests to %s", saved,
               final_path);
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

  FILE *f = fopen(final_path, "r");
  if (!f)
    return;

  patch_handle_t chests = PATCH_NULL;
  patchlib_field_get_value(f_main_chest, PATCH_NULL, &chests);
  if (!patchlib_is_valid(chests)) {
    fclose(f);
    return;
  }

  char line[700];
  int restored = 0;
  while (fgets(line, sizeof(line), f)) {
    if (line[0] == '#')
      continue;

    size_t chest_idx = 0, slot = 0;
    char loader[160] = {0}, mod[160] = {0}, item_name[160] = {0};
    int stack = 0, prefix = 0;
    if (sscanf(line, "%zu\t%zu\t%159[^\t]\t%159[^\t]\t%159[^\t]\t%d\t%d",
               &chest_idx, &slot, loader, mod, item_name, &stack, &prefix) != 7)
      continue;

    if (chest_idx >= patchlib_array_length(chests))
      continue;

    patch_handle_t chest = PATCH_NULL;
    if (!patchlib_array_at(chests, chest_idx, &chest) || !patchlib_is_valid(chest))
      continue;

    patch_handle_t items = PATCH_NULL;
    patchlib_field_get_value(f_chest_item, chest, &items);
    if (!patchlib_is_valid(items) || slot >= patchlib_array_length(items)) {
      if (patchlib_is_valid(items)) patchlib_free(items);
      patchlib_free(chest);
      continue;
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
      continue;
    }

    container_set_custom(items, slot, true, h->runtime_id, stack, prefix);
    restored++;

    patchlib_free(items);
    patchlib_free(chest);
  }

  patchlib_free(chests);
  fclose(f);
  TEKLOG_DEBUG("item_save: restored %d custom item(s) in world chests from %s",
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
