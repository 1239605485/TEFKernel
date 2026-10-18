/*******************************************************************************
 * tefkernel - item_manager
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
 * Created: 2026/8/26
 *******************************************************************************/

#include "internal/terraria/item_manager.h"
#include "terraria/language_manager.h"
#include "terraria/recipe_manager.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "internal/log.h"
#include "patchlib/field.h"
#include "patchlib/method.h"
#include "patchlib/struct/array.h"
#include "patchlib/struct/dictionary.h"
#include "patchlib/struct/list.h"
#include "patchlib/struct/string.h"
#include "terraria/asset.h"
#include "terraria/texture2d.h"

tefstd_vector_t g_terraria_item_registry; // 存储 terraria_item_handle_t*
static bool g_ids_assigned = false;

static patch_handle_t item_id_count;
static terraria_item_handle_t unknown_item;

static const char *unknown_item_text(language_culture_t l) {
    if (l == LANGUAGE_ENGLISH)
        return "{"
                "\"item_name_tefkernel_tefkernel\": {"
                "\"unknown\": \"英文\""
                "}"
                "}";


    return "{"
            "\"item_name_tefkernel_tefkernel\": {"
            "\"unknown\": \"不知道什么语言所以UNKNOWN\""
            "},"
            "\"item_tooltip_tefkernel_tefkernel\": {"
            "\"unknown\": \"傻逼泰拉代码\""
            "}"
            "}";
}

static void unknown_item_recipe() {
    terraria_recipe_set_result(unknown_item.runtime_id, 9999);
    terraria_recipe_set_material((int[2]){9, 1}, 1);
    terraria_recipe_add();
}

static void unknown_item_early_init(terraria_item_handle_t *current) {
    terraria_recipe_manager_register_callback(unknown_item_recipe);
    terraria_language_manager_register_callback(unknown_item_text);
}

// 未知物品的属性设置
static void unknown_item_set_defaults(terraria_item_handle_t *current,
                                      patch_handle_t instance) {
    patch_handle_t item_class = patchlib_type_get_type("Terraria", "Item");
    if (patchlib_is_valid(item_class)) {
        patch_handle_t f_name_override =
                patchlib_type_get_field(item_class, "_nameOverride");
        patch_handle_t f_bestiary_notes =
                patchlib_type_get_field(item_class, "BestiaryNotes");
        if (patchlib_is_valid(f_name_override)) {
            patch_handle_t name = patchlib_string_create("UNKNOWN");
            patchlib_field_set_value(f_name_override, instance, &name);
            patchlib_field_set_value(f_bestiary_notes, instance, &name);

            patchlib_free(name);
            patchlib_free(f_name_override);
            patchlib_free(f_bestiary_notes);
        }
        patchlib_free(item_class);
    }
}

// 生成未知物品纹理
static patch_handle_t
unknown_item_get_texture(terraria_item_handle_t *current) {
    const int SIZE = 64;
    const size_t PIXEL_COUNT = SIZE * SIZE;
    const size_t DATA_SIZE = PIXEL_COUNT * 4;

    uint8_t *pixels = malloc(DATA_SIZE);
    if (!pixels) {
        return 0;
    }

    // 预定义颜色值（直接使用 32-bit 整数，减少 memcpy 开销）
    const uint32_t COLOR_DARK_PURPLE = 0xFFA45067; // RGBA: 103,80,164,255
    const uint32_t COLOR_BLACK = 0xFF000000;
    const uint32_t DIVIDER_COLOR = 0xFF323232;

    const int CELL_SIZE = SIZE / 2;
    const int ROW_SIZE = SIZE * 4;

#if defined(__ANDROID__) || defined(ANDROID)
    // Android: Y 轴翻转
    for (int y = 0; y < SIZE; y++) {
        int src_y = SIZE - 1 - y;
        uint32_t *row = (uint32_t *) (pixels + y * ROW_SIZE);

        for (int x = 0; x < SIZE; x++) {
            if (x == CELL_SIZE || src_y == CELL_SIZE) {
                row[x] = DIVIDER_COLOR;
            } else {
                const bool isLeft = x < CELL_SIZE;
                const bool isTop = src_y < CELL_SIZE;
                row[x] = (isTop && isLeft) || (!isTop && !isLeft)
                             ? COLOR_BLACK
                             : COLOR_DARK_PURPLE;
            }
        }
    }
#else
    // 桌面: 正常坐标
    for (int y = 0; y < SIZE; y++) {
        uint32_t *row = (uint32_t *) (pixels + y * ROW_SIZE);

        for (int x = 0; x < SIZE; x++) {
            if (x == CELL_SIZE || y == CELL_SIZE) {
                row[x] = DIVIDER_COLOR;
            } else {
                const bool isLeft = x < CELL_SIZE;
                const bool isTop = y < CELL_SIZE;
                row[x] = (isTop && isLeft) || (!isTop && !isLeft)
                             ? COLOR_BLACK
                             : COLOR_DARK_PURPLE;
            }
        }
    }
#endif

    patch_handle_t texture = terraria_texture2d_create(
        SIZE, SIZE, TEXTURE_FORMAT_RGBA32, pixels, DATA_SIZE);

    free(pixels);

    patch_handle_t texture_type = terraria_texture2d_get_class();
    patch_handle_t asset = terraria_asset_create(texture_type, texture);

    patchlib_free(texture_type);
    patchlib_free(texture);

    return asset;
}

// 创建未知物品实例
static terraria_item_handle_t unknown_item = {
    .parent_modloader_id = "tefkernel",
    .parent_id = "tefkernel",
    .internal_name = "unknown",
    .has_tooltip = false,
    .runtime_id = -1,
    .item_ops = {
        .early_init = unknown_item_early_init /*unknown_item_early_init*/,
        .init_static = NULL,
        .set_defaults = unknown_item_set_defaults,
        .can_use = NULL,
        .get_texture = unknown_item_get_texture
    }
};

static patch_handle_t f_type = PATCH_NULL;
static patch_handle_t f_stack = PATCH_NULL;
static patch_handle_t m_reset_stats = PATCH_NULL;
static patch_handle_t m_rebuild_tooltip = PATCH_NULL; // void RebuildTooltip()

static patch_handle_t f_texture_item = PATCH_NULL;
static patch_handle_t f_texture_item_flame = PATCH_NULL;

static patch_handle_t f_item_name_cache = PATCH_NULL;
static patch_handle_t f_item_tooltip_cache = PATCH_NULL;
static patch_handle_t f_item_sorting_whitelists = PATCH_NULL; // ItemSorting._layerWhiteLists

static patch_handle_t m_from_language_key = PATCH_NULL; // static ItemTooltip FromLanguageKey(short id, string key)

// 自定义物品通用字段/方法句柄（通过 terraria_item_manager_get_item_ops 提供给存档等模块）
static patch_handle_t f_item_prefix = PATCH_NULL; // Item.prefix (byte)
static patch_handle_t m_item_net_defaults = PATCH_NULL; // Item.netDefaults(int)
static patch_handle_t m_item_prefix = PATCH_NULL; // Item.Prefix(int)

/* ------------------------------------------------------------------
 * 自定义物品的"可重铸类别"
 *
 * 哥布林工匠能不能重铸，只看 Item.GetRollablePrefixes()：
 *   PrefixLegacy.ItemSets.{SwordsHammersAxesPicks, SpearsMacesChainsawsDrillsPunchCannon,
 *                         GunsBows, Magic, Summon, BoomerangsChakrams,
 *                         ItemsThatCanHaveLegendary2}[type] == true
 *   饰品走 IsAPrefixableAccessory() → ItemID.Sets.CanGetPrefixes[type]
 *
 * 这几张表我们早就按 ItemID.Count 扩过容（否则会越界），但新槽位是 false，
 * 于是自定义物品永远"不可重铸"。这里按物品自身属性自动归类并置 true，
 * Mod 不需要额外声明。
 * ------------------------------------------------------------------ */
static patch_handle_t f_pref_swords = PATCH_NULL;
static patch_handle_t f_pref_spears = PATCH_NULL;
static patch_handle_t f_pref_guns = PATCH_NULL;
static patch_handle_t f_pref_magic = PATCH_NULL;
static patch_handle_t f_pref_summon = PATCH_NULL;
static patch_handle_t f_pref_boomerang = PATCH_NULL;
static patch_handle_t f_pref_legendary2 = PATCH_NULL;
static patch_handle_t f_can_get_prefixes = PATCH_NULL;
static int g_prefix_sets_state = -1; // -1 未解析 / 0 失败 / 1 就绪

/* Item 的伤害类别字段（1.4.5 已无 thrown，投掷并入 ranged） */
static patch_handle_t f_i_accessory = PATCH_NULL;
static patch_handle_t f_i_vanity = PATCH_NULL;
static patch_handle_t f_i_damage = PATCH_NULL;
static patch_handle_t f_i_melee = PATCH_NULL;
static patch_handle_t f_i_magic = PATCH_NULL;
static patch_handle_t f_i_ranged = PATCH_NULL;
static patch_handle_t f_i_summon = PATCH_NULL;

static int resolve_prefix_sets(void) {
  if (g_prefix_sets_state >= 0)
    return g_prefix_sets_state;
  g_prefix_sets_state = 0;

  patch_handle_t pl =
      patchlib_type_get_type("Terraria.GameContent.Prefixes", "PrefixLegacy");
  if (!patchlib_is_valid(pl)) {
    TEKLOG_WARN("prefix: PrefixLegacy not found");
    return 0;
  }
  patch_handle_t sets = patchlib_type_get_inner_type(pl, "ItemSets");
  patchlib_free(pl);
  if (!patchlib_is_valid(sets)) {
    TEKLOG_WARN("prefix: PrefixLegacy+ItemSets not found");
    return 0;
  }
  f_pref_swords = patchlib_type_get_field(sets, "SwordsHammersAxesPicks");
  f_pref_spears =
      patchlib_type_get_field(sets, "SpearsMacesChainsawsDrillsPunchCannon");
  f_pref_guns = patchlib_type_get_field(sets, "GunsBows");
  f_pref_magic = patchlib_type_get_field(sets, "Magic");
  f_pref_summon = patchlib_type_get_field(sets, "Summon");
  f_pref_boomerang = patchlib_type_get_field(sets, "BoomerangsChakrams");
  f_pref_legendary2 =
      patchlib_type_get_field(sets, "ItemsThatCanHaveLegendary2");
  patchlib_free(sets);

  patch_handle_t itemid = patchlib_type_get_type("Terraria.ID", "ItemID");
  if (patchlib_is_valid(itemid)) {
    patch_handle_t idsets = patchlib_type_get_inner_type(itemid, "Sets");
    patchlib_free(itemid);
    if (patchlib_is_valid(idsets)) {
      f_can_get_prefixes =
          patchlib_type_get_field(idsets, "CanGetPrefixes");
      patchlib_free(idsets);
    }
  }

  if (!patchlib_is_valid(f_pref_swords) ||
      !patchlib_is_valid(f_can_get_prefixes)) {
    TEKLOG_WARN("prefix: cannot resolve prefix sets "
                "(swords=%p canGetPrefixes=%p)",
                (void *)f_pref_swords, (void *)f_can_get_prefixes);
    return 0;
  }
  g_prefix_sets_state = 1;
  TEKLOG_INFO("prefix: rollable-prefix sets resolved");
  return 1;
}

/* 静态 bool[] 字段的第 type 位置 true（每次现取数组对象：
 * 扩容会替换数组实例，缓存句柄会失效） */
static void prefix_set_entry(patch_handle_t field, const int type,
                             const char *name) {
  if (!patchlib_is_valid(field) || type < 0)
    return;
  patch_handle_t arr = PATCH_NULL;
  patchlib_field_get_value(field, NULL, &arr);
  if (!patchlib_is_valid(arr)) {
    /* 静态数组尚未初始化（C# 静态字段在类型首次访问时才创建）。
     * 等游戏自己用到它们，再由下面的自愈逻辑补长。 */
    static int warned_null = 0;
    if (warned_null < 4) {
      warned_null++;
      TEKLOG_DEBUG("prefix: %s is null (not initialized yet) - will retry", name);
    }
    return;
  }

  size_t len = patchlib_array_length(arr);
  const int need = terraria_item_id_get_count() +
                   (int)tefstd_vector_size(&g_terraria_item_registry);
  if ((int)len < need) {
    /* 自愈：这些静态数组在扩容时还不存在（或被初始化成原版长度），
     * 首次用到时按需要补长。新槽位填 0(false)。 */
    patch_handle_t bigger = patchlib_array_resize(arr, (size_t)need, NULL);
    if (patchlib_is_valid(bigger)) {
      patchlib_field_set_value(field, NULL, &bigger);
      TEKLOG_INFO("prefix: %s grown %zu -> %d (lazy)", name, len, need);
      patchlib_free(arr);
      arr = bigger;
      len = patchlib_array_length(arr);
    } else {
      TEKLOG_WARN("prefix: %s grow failed (len=%zu need=%d)", name, len, need);
      patchlib_free(arr);
      return;
    }
  }

  if ((size_t)type >= len) {
    TEKLOG_WARN("prefix: %s still too short (len=%zu type=%d)", name, len, type);
    patchlib_free(arr);
    return;
  }

  unsigned char one = 1;
  if (!patchlib_array_set(arr, (size_t)type, &one)) {
    TEKLOG_WARN("prefix: %s set failed (type=%d)", name, type);
  } else {
    static unsigned char logged[8192];
    if (type < (int)sizeof(logged) && !logged[type]) {
      logged[type] = 1;
      TEKLOG_INFO("prefix: %s[%d] = true (len=%zu)", name, type, len);
    }
  }
  patchlib_free(arr);
}

static bool item_flag(patch_handle_t item, patch_handle_t field) {
  if (!patchlib_is_valid(field))
    return false;
  unsigned char v = 0;
  patchlib_field_get_value(field, item, &v);
  return v != 0;
}

/* 每个类型只打一次归类日志（set_defaults 会被频繁调用） */
static unsigned char g_prefix_logged[8192];

/* 按物品属性决定它能掷出哪一类词缀；对自定义物品幂等（重复调用无害） */
static void apply_rollable_prefix(patch_handle_t item, const int type) {
  if (resolve_prefix_sets() != 1)
    return;

  /* 注意：1.4.5 起武器可堆叠，不能用 maxStack>1 判断"不可重铸"；
   * 原版的唯一判据就是 PrefixLegacy.ItemSets / CanGetPrefixes 的成员资格。 */
  const char *cls = NULL;

  /* 探针：每个类型打一次原始判定值，便于定位"为什么没归类" */
  {
    static unsigned char probed[8192];
    if (type >= 0 && type < (int)sizeof(probed) && !probed[type]) {
      probed[type] = 1;
      int dmg = 0;
      if (patchlib_is_valid(f_i_damage))
        patchlib_field_get_value(f_i_damage, item, &dmg);
      TEKLOG_DEBUG("prefix probe: type=%d acc=%d vanity=%d dmg=%d melee=%d "
                  "magic=%d ranged=%d summon=%d (fields: dmg=%p acc=%p)",
                  type, item_flag(item, f_i_accessory) ? 1 : 0,
                  item_flag(item, f_i_vanity) ? 1 : 0, dmg,
                  item_flag(item, f_i_melee) ? 1 : 0,
                  item_flag(item, f_i_magic) ? 1 : 0,
                  item_flag(item, f_i_ranged) ? 1 : 0,
                  item_flag(item, f_i_summon) ? 1 : 0,
                  (void *)f_i_damage, (void *)f_i_accessory);
    }
  }

  if (item_flag(item, f_i_accessory)) {
    /* 饰品：只要不是时装，就能掷饰品词缀（护佑/幸运…） */
    if (!item_flag(item, f_i_vanity)) {
      prefix_set_entry(f_can_get_prefixes, type, "CanGetPrefixes");
      cls = "accessory";
    } else {
      cls = "vanity(no prefix)";
    }
    goto done;
  }

  int damage = 0;
  if (patchlib_is_valid(f_i_damage))
    patchlib_field_get_value(f_i_damage, item, &damage);
  if (damage <= 0)
    return; /* 材料/消耗品：本来就不该有词缀 */

  if (item_flag(item, f_i_magic)) {
    prefix_set_entry(f_pref_magic, type, "Magic");
    cls = "magic";
  } else if (item_flag(item, f_i_summon)) {
    prefix_set_entry(f_pref_summon, type, "Summon");
    cls = "summon";
  } else if (item_flag(item, f_i_ranged)) {
    prefix_set_entry(f_pref_guns, type, "GunsBows");
    cls = "guns/bows";
  } else {
    /* 近战，以及未标类别的有伤物品：都能掷剑类词缀（传奇/超神…） */
    prefix_set_entry(f_pref_swords, type, "Swords");
    cls = "swords/tools";
  }

done:
  if (cls && type >= 0 && type < (int)sizeof(g_prefix_logged) &&
      !g_prefix_logged[type]) {
    g_prefix_logged[type] = 1;
    TEKLOG_INFO("prefix: custom item type=%d -> %s", type, cls);
  }
}

/* 按需补归类：重铸/掷前缀时游戏会调 Item.GetRollablePrefixes()，
 * 那一刻手里是真实实例、且静态数组早已扩容完成 → 在这里补写最可靠。 */
static bool get_rollable_prefixes_prefix(patch_handle_t instance, void **args,
                                        const patch_method_signature_t *sig_info,
                                        void *result);

static void set_defaults_postfix(patch_handle_t this, void **args, void *result,
                                 const patch_method_signature_t *sig_info) {
    // 检查 args 是否有效
    if (!args || !args[0] || !this) {
        return; // 非原版物品不打印日志
    }

    const int args_type = *(int *) args[0];
    const int base_count = terraria_item_id_get_count();
    const size_t custom_count = tefstd_vector_size(&g_terraria_item_registry);

    // 只处理自定义物品（超出原版范围的 ID）
    const int nid = args_type - base_count;
    if (nid < 0 || nid >= (int) custom_count) {
        return; // 非原版物品不打印日志
    }

    TEKLOG_INFO("set_defaults_postfix: custom nid=%d, applying defaults", nid);

    // ⭐ 关键：强制恢复 type（SetDefaults 内部可能把它清空了）
    patchlib_field_set_value(f_type, this, args[0]);

    // 设置堆叠数
    int stack = 1;
    patchlib_field_set_value(f_stack, this, &stack);

    // 调用自定义物品的 set_defaults
    terraria_item_handle_t **item_handle_ptr =
            tefstd_vector_at(&g_terraria_item_registry, nid);
    if (item_handle_ptr && *item_handle_ptr) {
        terraria_item_handle_t *item_handle = *item_handle_ptr;
        if (item_handle->item_ops.set_defaults) {
            item_handle->item_ops.set_defaults(item_handle, this);
        }
    }

    /* 让自定义物品在哥布林工匠处可重铸（按属性自动归类） */
    apply_rollable_prefix(this, args_type);

    if (patchlib_is_valid(m_rebuild_tooltip))
        patchlib_method_invoke_args(m_rebuild_tooltip, this, PATCH_NULL, NULL);
}

/* GetRollablePrefixes 钩子：自定义物品按需补归类（数组此时已扩容） */
static bool get_rollable_prefixes_prefix(patch_handle_t instance, void **args,
                                        const patch_method_signature_t *sig_info,
                                        void *result) {
  (void)args; (void)sig_info; (void)result;
  if (!instance || !patchlib_is_valid(f_type))
    return false;

  int type = 0;
  patchlib_field_get_value(f_type, instance, &type);
  if (type >= terraria_item_id_get_count()) {
    static unsigned char hit[8192];
    if (type < (int)sizeof(hit) && !hit[type]) {
      hit[type] = 1;
      TEKLOG_INFO("prefix: GetRollablePrefixes hook hit (type=%d)", type);
    }
    apply_rollable_prefix(instance, type);
  }

  return false; /* 继续执行原方法 */
}


/*
static bool
item_check_check_can_use_inner_prefix(patch_handle_t this, void **args,
                                      const patch_method_signature_t *sig_info,
                                      void *result) {

  if (!args || !args[0] || !this) {
    return false; // 非原版物品不打印日志
  }

  bool ignore_cursed = false;
  if (args[1]) {
    ignore_cursed = *(bool *)args[1];
  }

  patch_handle_t args_item = *(patch_handle_t *)args[0];
  int current_item_type = 0;
  patchlib_field_get_value(f_type, args_item, &current_item_type);

  const int base_count = terraria_item_id_get_count();
  const size_t custom_count = tefstd_vector_size(&g_terraria_item_registry);

  // 只处理自定义物品（超出原版范围的 ID）
  const int nid = current_item_type - base_count;
  if (nid < 0 || nid >= (int)custom_count) {
    return false; // 非原版物品不打印日志
  }

  // 调用自定义物品的 can_use
  terraria_item_handle_t **item_handle_ptr =
      tefstd_vector_at(&g_terraria_item_registry, nid);
  if (item_handle_ptr && *item_handle_ptr) {
    terraria_item_handle_t *item_handle = *item_handle_ptr;
    if (item_handle->item_ops.can_use) {
      *(bool *)result = item_handle->item_ops.can_use(item_handle, this,
                                                      args_item, ignore_cursed);
      return true;
    }
  }

  return false;
}
*/

static void get_item_name_postfix(patch_handle_t this, void **args,
                                  void *result,
                                  const patch_method_signature_t *sig_info) {
    const int current_item_type = *(int *) args[0];
    const int base_count = terraria_item_id_get_count();
    const size_t custom_count = tefstd_vector_size(&g_terraria_item_registry);

    const int nid = current_item_type - base_count;
    if (nid < 0 || nid >= (int) custom_count) {
        return;
    }

    patch_handle_t array_item_name_cache = PATCH_NULL;
    patchlib_field_get_value(f_item_name_cache, NULL, &array_item_name_cache);

    // 仅当缓存中确实存在条目时才覆盖原版返回值；否则保留原版
    // GetItemName 返回的 LocalizedText.Empty，避免把结果改写成 null
    // 造成后续 GetItemNameValue / UI 的 NullReferenceException。
    patch_handle_t cached_name = PATCH_NULL;
    if (patchlib_array_at(array_item_name_cache, current_item_type, &cached_name) &&
        patchlib_is_valid(cached_name) && result) {
        *(patch_handle_t *) result = cached_name;
    }

    patchlib_free(array_item_name_cache);
}

static void content_samples_initialize_postfix(patch_handle_t this, void **args,
                                               void *result,
                                               const patch_method_signature_t *sig_info) {
    (void)this; (void)args; (void)result; (void)sig_info;

    patch_handle_t item_type = patchlib_type_get_type("Terraria", "Item");
    patch_handle_t cs_type =
            patchlib_type_get_type("Terraria.ID", "ContentSamples");
    if (!patchlib_is_valid(item_type) || !patchlib_is_valid(cs_type)) {
        TEKLOG_ERROR("ContentSamples/Item type not found");
        if (patchlib_is_valid(item_type)) patchlib_free(item_type);
        if (patchlib_is_valid(cs_type))   patchlib_free(cs_type);
        return;
    }

    /* ---------- 取三个字典实例 ---------- */
    patch_handle_t dict_by_type = PATCH_NULL;
    patch_handle_t dict_net_ids = PATCH_NULL;
    patch_handle_t dict_persist_ids = PATCH_NULL;
    // 字典的闭合泛型类型句柄（用于 set_Item，避免走 il2cpp_object_get_class）
    patch_handle_t t_by_type = PATCH_NULL;
    patch_handle_t t_net_ids = PATCH_NULL;
    patch_handle_t t_persist_ids = PATCH_NULL;

    patch_handle_t f1 = patchlib_type_get_field(cs_type, "ItemsByType");
    if (patchlib_is_valid(f1)) {
        patchlib_field_get_value(f1, PATCH_NULL, &dict_by_type);
        t_by_type = patchlib_field_get_type_handle(f1);
    }

    patch_handle_t f2 =
            patchlib_type_get_field(cs_type, "ItemNetIdsByPersistentIds");
    if (patchlib_is_valid(f2)) {
        patchlib_field_get_value(f2, PATCH_NULL, &dict_net_ids);
        t_net_ids = patchlib_field_get_type_handle(f2);
    }

    patch_handle_t f3 =
            patchlib_type_get_field(cs_type, "ItemPersistentIdsByNetIds");
    if (patchlib_is_valid(f3)) {
        patchlib_field_get_value(f3, PATCH_NULL, &dict_persist_ids);
        t_persist_ids = patchlib_field_get_type_handle(f3);
    }

    /* ---------- 取构造函数 Item(int, ItemVariant) ---------- */
    patch_handle_t m_ctor =
            patchlib_type_get_method_by_param_count(item_type, ".ctor", 2);

    /* ---------- ItemID.Search（ReLogic.Reflection.IdDictionary）-------
     * 不调用 IdDictionary.Add：移动端用 libffi 调它会在 libil2cpp 崩溃。
     * 改为直接写它的两个内部字典 _nameToId / _idToName（set_Item 路径已验证可用）。*/
    patch_handle_t item_id_cls = patchlib_type_get_type("Terraria.ID", "ItemID");
    patch_handle_t f_search = PATCH_NULL;
    patch_handle_t search_dict = PATCH_NULL;
    patch_handle_t f_name_to_id = PATCH_NULL;
    patch_handle_t f_id_to_name = PATCH_NULL;
    patch_handle_t dict_name_to_id = PATCH_NULL; // Dictionary<string,int>
    patch_handle_t dict_id_to_name = PATCH_NULL; // Dictionary<int,string>
    patch_handle_t t_name_to_id = PATCH_NULL;
    patch_handle_t t_id_to_name = PATCH_NULL;
    if (patchlib_is_valid(item_id_cls)) {
        f_search = patchlib_type_get_field(item_id_cls, "Search");
        if (patchlib_is_valid(f_search))
            patchlib_field_get_value(f_search, PATCH_NULL, &search_dict);

        patch_handle_t id_dict_cls =
                patchlib_type_get_type("ReLogic.Reflection", "IdDictionary");
        if (patchlib_is_valid(id_dict_cls)) {
            f_name_to_id = patchlib_type_get_field(id_dict_cls, "_nameToId");
            f_id_to_name = patchlib_type_get_field(id_dict_cls, "_idToName");
            if (patchlib_is_valid(f_name_to_id))
                t_name_to_id = patchlib_field_get_type_handle(f_name_to_id);
            if (patchlib_is_valid(f_id_to_name))
                t_id_to_name = patchlib_field_get_type_handle(f_id_to_name);
            patchlib_free(id_dict_cls);
        }
        if (patchlib_is_valid(search_dict)) {
            if (patchlib_is_valid(f_name_to_id))
                patchlib_field_get_value(f_name_to_id, search_dict, &dict_name_to_id);
            if (patchlib_is_valid(f_id_to_name))
                patchlib_field_get_value(f_id_to_name, search_dict, &dict_id_to_name);
        }
    }

    /* ---------- 遍历所有自定义物品 ---------- */
    const size_t custom_count = tefstd_vector_size(&g_terraria_item_registry);
    int registered = 0;

    for (size_t i = 0; i < custom_count; ++i) {
        terraria_item_handle_t **pp =
                tefstd_vector_at(&g_terraria_item_registry, i);
        if (!pp || !*pp) continue;

        const terraria_item_handle_t *h = *pp;
        if (h->runtime_id < 0) continue;

        int item_id = h->runtime_id;

        /* ----- 创建该 id 的 Item 实例 ----- */
        patch_handle_t item = PATCH_NULL;
        patch_handle_t variant = PATCH_NULL;
        void *ctor_args[2] = { &item_id, &variant };
        patchlib_constructor_invoke(m_ctor, &item, ctor_args);

        /* ----- ItemsByType[item_id] = item ----- */
        if (!patchlib_dictionary_set_value_typed(dict_by_type, t_by_type,
                                                 &item_id, &item))
            TEKLOG_WARN("ItemsByType set failed: id=%d", item_id);

        /* ----- 动态构造持久化 id 字符串 ----- */
        const char *loader = h->parent_modloader_id ? h->parent_modloader_id : "?";
        const char *mod    = h->parent_id           ? h->parent_id           : "?";
        const char *name   = h->internal_name       ? h->internal_name       : "?";

        const size_t persist_len =
                strlen(loader) + 1 + strlen(mod) + 1 + strlen(name) + 1;
        char *persist_id = malloc(persist_len);
        if (!persist_id) {
            TEKLOG_WARN("malloc persist_id failed: id=%d", item_id);
            if (patchlib_is_valid(item)) patchlib_free(item);
            continue;
        }
        snprintf(persist_id, persist_len, "%s_%s_%s", loader, mod, name);

        patch_handle_t str_obj = patchlib_string_create(persist_id);
        free(persist_id);

        if (patchlib_is_valid(str_obj)) {
            /* ----- ItemNetIdsByPersistentIds[persist_id] = item_id ----- */
            if (!patchlib_dictionary_set_value_typed(dict_net_ids, t_net_ids,
                                                     &str_obj, &item_id))
                TEKLOG_WARN("NetIds set failed: id=%d", item_id);

            /* ----- ItemPersistentIdsByNetIds[item_id] = persist_id ----- */
            if (!patchlib_dictionary_set_value_typed(dict_persist_ids,
                                                     t_persist_ids, &item_id,
                                                     &str_obj))
                TEKLOG_WARN("PersistIds set failed: id=%d", item_id);

            /* ----- ItemID.Search 内部字典：name->id / id->name ----- */
            if (patchlib_is_valid(dict_name_to_id) &&
                patchlib_is_valid(t_name_to_id)) {
                if (!patchlib_dictionary_set_value_typed(dict_name_to_id,
                                                         t_name_to_id, &str_obj,
                                                         &item_id))
                    TEKLOG_WARN("Search nameToId set failed: id=%d", item_id);
            }
            if (patchlib_is_valid(dict_id_to_name) &&
                patchlib_is_valid(t_id_to_name)) {
                if (!patchlib_dictionary_set_value_typed(dict_id_to_name,
                                                         t_id_to_name, &item_id,
                                                         &str_obj))
                    TEKLOG_WARN("Search idToName set failed: id=%d", item_id);
            }
        }

        /* 释放本轮创建的临时句柄（字典已持有 item/str 的引用） */
        patchlib_free(str_obj);
        patchlib_free(item);

        registered++;
    }

    TEKLOG_INFO("registered %d custom items into ContentSamples", registered);

    /* ---------- 释放从类型/字段/方法 API 获得的句柄 ---------- */
    patchlib_free(f1);
    patchlib_free(f2);
    patchlib_free(f3);
    patchlib_free(t_by_type);
    patchlib_free(t_net_ids);
    patchlib_free(t_persist_ids);
    patchlib_free(m_ctor);
    patchlib_free(item_type);
    patchlib_free(cs_type);

    patchlib_free(dict_name_to_id);
    patchlib_free(dict_id_to_name);
    patchlib_free(t_name_to_id);
    patchlib_free(t_id_to_name);
    patchlib_free(search_dict);
    patchlib_free(f_name_to_id);
    patchlib_free(f_id_to_name);
    patchlib_free(f_search);
    patchlib_free(item_id_cls);

    patchlib_free(dict_by_type);
    patchlib_free(dict_net_ids);
    patchlib_free(dict_persist_ids);
}

static void creative_item_sacrifices_catalog_initialize_postfix(
        patch_handle_t this, void **args, void *result,
        const patch_method_signature_t *sig_info)
{
    (void)args; (void)result; (void)sig_info;

    if (!patchlib_is_valid(this)) {
        TEKLOG_ERROR("CreativeItemSacrificesCatalog.Initialize: this invalid");
        return;
    }

    patch_handle_t cls = patchlib_type_get_type(
        "Terraria.GameContent.Creative", "CreativeItemSacrificesCatalog");
    if (!patchlib_is_valid(cls)) {
        TEKLOG_ERROR("CreativeItemSacrificesCatalog type not found");
        return;
    }

    patch_handle_t field = patchlib_type_get_field(
        cls, "_sacrificeCountNeededByItemId");
    if (!patchlib_is_valid(field)) {
        TEKLOG_ERROR("field _sacrificeCountNeededByItemId not found");
        patchlib_free(cls);
        return;
    }

    patch_handle_t dict = PATCH_NULL;
    patchlib_field_get_value(field, this, &dict);
    if (!patchlib_is_valid(dict)) {
        TEKLOG_ERROR("failed to get sacrifice dict");
        patchlib_free(field);
        patchlib_free(cls);
        return;
    }

    // 字典的闭合泛型类型句柄（用于 set_Item，避免 il2cpp_object_get_class）
    patch_handle_t dict_type = patchlib_field_get_type_handle(field);

    /* ---------- 遍历所有自定义物品 ---------- */
    const size_t custom_count = tefstd_vector_size(&g_terraria_item_registry);
    int registered = 0;

    for (size_t i = 0; i < custom_count; ++i) {
        terraria_item_handle_t **pp =
            tefstd_vector_at(&g_terraria_item_registry, i);
        if (!pp || !*pp) continue;

        terraria_item_handle_t *h = *pp;
        if (h->runtime_id < 0) continue;

        int item_id = h->runtime_id;
        int sacrifice_count = h->sacrifice_count;

        /* 研究数量 <= 0 的物品不注册（不可研究） */
        if (sacrifice_count <= 0) {
            TEKLOG_DEBUG("skip sacrifice reg: id=%d count=%d",
                         item_id, sacrifice_count);
            continue;
        }

        if (patchlib_dictionary_set_value_typed(dict, dict_type, &item_id,
                                                &sacrifice_count))
            registered++;
        else
            TEKLOG_WARN("sacrifice set failed: id=%d", item_id);
    }

    TEKLOG_INFO("registered %d sacrifice entries", registered);

    /* ---------- 清理 ----------
       注意：dict 是从字段读出的引用，由游戏持有，不要 free */
    patchlib_free(dict_type);
    patchlib_free(field);
    patchlib_free(cls);
}

// 比较函数（用于 qsort）
static int compare_item(const void *a, const void *b) {
    const terraria_item_handle_t *item_a = *(const terraria_item_handle_t **) a;
    const terraria_item_handle_t *item_b = *(const terraria_item_handle_t **) b;

    /* 先按 modloader 字典序 */
    int cmp = strcmp(item_a->parent_modloader_id, item_b->parent_modloader_id);
    if (cmp != 0)
        return cmp;

    /* 同一 modloader 内按 mod id 字典序 */
    cmp = strcmp(item_a->parent_id, item_b->parent_id);
    if (cmp != 0)
        return cmp;

    /* 同一 mod 内按 internal_name 字典序 */
    return strcmp(item_a->internal_name, item_b->internal_name);
}

// 初始化

static void resize_array(const char *ns, const char *cls, const char *fid,
                         patch_handle_t type, int nsize, void *dvalue);

#if !__ANDROID__
// PC端：AssetInitializer.LoadTextures 会按 TextureAssets.Item 的新长度遍历，
// 并读取 ItemID.Sets.TextureCopyLoad[i]（原版长度只有 ItemID.Count），
// 导致 IndexOutOfRangeException 中断整个贴图加载（表现为游戏无贴图）。
// 同时该函数会用原版 Images/Item_x 覆盖自定义物品贴图。
// 因此：1) resize 时同步扩充 TextureCopyLoad；2) LoadTextures
// 之后重新写入自定义贴图。
static void load_textures_postfix(patch_handle_t this, void **args,
                                  void *result,
                                  const patch_method_signature_t *sig_info) {
    TEKLOG_INFO("AssetInitializer.LoadTextures postfix: re-applying custom item "
        "textures");
    terraria_item_manager_init_texture2d();
}
#endif

// 一次性扩容 ItemID.Sets 中所有长度为 ItemID.Count 的静态数组。
// 游戏代码大量按 item.type
// 索引这些集合（ItemIconPulse、TrapSigned、TextureCopyLoad 等），
// 任何一个没扩容都会导致绘制自定义物品时
// IndexOutOfRangeException（表现为图标"透明"或崩溃）。 一次性扩容 ItemID.Sets
// 中所有长度为 ItemID.Count 的静态数组。 游戏代码大量按 item.type
// 索引这些集合（ItemIconPulse、TrapSigned、TextureCopyLoad 等），
// 任何一个没扩容都会导致绘制自定义物品时
// IndexOutOfRangeException（表现为图标"透明"或崩溃）。
// 扩容后把新槽位填成“该集合的默认值”。
// SetFactory 生成的集合大多有非 0 默认值（ToolTipDamageMultiplier=1、
// BonusMeleeSpeedMultiplier=1、SortingPriority*=-1 等），而 array_resize 只把
// 新槽清零，会导致自定义物品“伤害显示 0 / 攻击速度 0 / 不能排序”。
// 这里用原数组里出现次数最多的元素作为默认值填充（对 bool/int/float 集合即 factory 默认值）。
static void fill_new_slots_with_modal(patch_handle_t old_array,
                                      patch_handle_t new_array,
                                      const size_t old_len,
                                      const size_t new_len) {
    enum { MAX_UNIQ = 64, EBUF = 256 };
    if (!patchlib_is_valid(old_array) || !patchlib_is_valid(new_array) ||
        old_len == 0 || new_len <= old_len)
        return;

    uint64_t keys[MAX_UNIQ];
    int counts[MAX_UNIQ];
    uint8_t vals[MAX_UNIQ][EBUF];
    int nu = 0;
    uint8_t buf[EBUF];

    for (size_t i = 0; i < old_len; ++i) {
        memset(buf, 0, EBUF);
        if (!patchlib_array_at(old_array, i, buf))
            continue;
        uint64_t key = 0;
        memcpy(&key, buf, sizeof(key));
        int found = -1;
        for (int j = 0; j < nu; ++j) {
            if (keys[j] == key) {
                found = j;
                break;
            }
        }
        if (found >= 0) {
            counts[found]++;
        } else if (nu < MAX_UNIQ) {
            keys[nu] = key;
            memcpy(vals[nu], buf, EBUF);
            counts[nu] = 1;
            nu++;
        }
    }
    if (nu == 0)
        return;

    int best = 0;
    for (int j = 1; j < nu; ++j) {
        if (counts[j] > counts[best])
            best = j;
    }
    for (size_t i = old_len; i < new_len; ++i) {
        if (!patchlib_array_set(new_array, i, vals[best]))
            break; // 某些结构/引用元素无法写入，保持默认(0/null)即可
    }
}

static void resize_all_class_sets(const char *ns, const char *cls,
                                  const char *inner, const int base_count,
                                  const int new_size) {
    patch_handle_t sets_class = patchlib_type_get_type(ns, cls);
    if (inner) {
        patch_handle_t new_sets_class =
                patchlib_type_get_inner_type(sets_class, inner);
        patchlib_free(sets_class);
        sets_class = new_sets_class;
    }

    if (!patchlib_is_valid(sets_class)) {
        TEKLOG_ERROR("resize_all_class_sets: Failed to get %s.%s", ns, cls);
        return;
    }

    tefstd_vector_t fields;
    tefstd_vector_init(&fields, sizeof(patch_handle_t));
    patchlib_type_get_fields(sets_class, false, &fields);
    if (!fields.data || fields.size <= 0) {
        TEKLOG_ERROR("resize_all_class_sets: Failed to get fields of %s.%s", ns,
                     cls);
        patchlib_free(sets_class);
        tefstd_vector_destroy(&fields);
        return;
    }

    // ⭐ 需要排除的字段名称（非数组、List、长度不等于 ItemID.Count 的数组等）
    //
    // 注意：FoodParticleColors / DrinkParticleColors（Color[][]）以及若干可空数组
    // （NetUseSoundSync 等）也按 type 索引、长度等于 ItemID.Count，**必须一起扩容**；
    // 否则游戏用自定义 id 索引时会抛 IndexOutOfRangeException，被
    // Main.ignoreErrors 静默吞掉，导致 ItemCheck 半途中断（近战命中、射击等失效）。
    const char *EXCLUDED_FIELDS[] = {
        // 非数组
        "Factory", "DD2BannerEffect", "DefaultKillsForBannerNeeded", "Count",

        // List<T> 类型
        "ItemsThatAreProcessedAfterNormalContentSample", "NonColorfulDyeItems",

        // 长度不等于 ItemID.Count 的数组
        "ItemsForStuffCannon", "Workbenches"
    };

    int resized = 0;
    int skipped = 0;
    int error = 0;
    int skip_excluded = 0, skip_invalid = 0, skip_len = 0;
    char skip_sample[256];
    skip_sample[0] = 0;

    for (size_t i = 0; i < fields.size; i++) {
        patch_handle_t field = *(patch_handle_t *) tefstd_vector_at(&fields, i);
        if (!patchlib_is_valid(field)) {
            error++;
            continue;
        }

        const char *name = patchlib_field_get_name(field);

        // ⭐ 检查是否在排除列表中
        bool excluded = false;
        for (size_t j = 0; j < sizeof(EXCLUDED_FIELDS) / sizeof(EXCLUDED_FIELDS[0]);
             j++) {
            if (name && strcmp(name, EXCLUDED_FIELDS[j]) == 0) {
                excluded = true;
                break;
            }
        }

        if (excluded) {
            TEKLOG_DEBUG("resize_all_itemid_sets: skipping excluded field: %s",
                         name ? name : "?");
            patchlib_free(field);
            skipped++;
            skip_excluded++;
            if (strlen(skip_sample) < 200)
                snprintf(skip_sample + strlen(skip_sample), 200,
                         "%s(excl) ", name ? name : "?");
            continue;
        }

        // 获取字段值（数组实例）
        patch_handle_t array = PATCH_NULL;
        patchlib_field_get_value(field, NULL, &array);

        if (!patchlib_is_valid(array)) {
            TEKLOG_DEBUG("resize_all_itemid_sets: invalid array for %s",
                         name ? name : "?");
            patchlib_free(field);
            skipped++;
            skip_invalid++;
            if (strlen(skip_sample) < 200)
                snprintf(skip_sample + strlen(skip_sample), 200,
                         "%s(null) ", name ? name : "?");
            continue;
        }

        // 检查数组长度是否匹配 base_count
        const size_t len = patchlib_array_length(array);
        if (len != (size_t) base_count) {
            TEKLOG_DEBUG("resize_all_itemid_sets: skipping %s (len=%zu, expected=%d)",
                         name ? name : "?", len, base_count);
            patchlib_free(field);
            patchlib_free(array);
            skipped++;
            skip_len++;
            if (strlen(skip_sample) < 200)
                snprintf(skip_sample + strlen(skip_sample), 200,
                         "%s(len%zu) ", name ? name : "?", len);
            continue;
        }

        // ⭐ 执行扩容（传入默认值）
        patch_handle_t new_array = patchlib_array_resize(array, new_size, NULL);

        if (patchlib_is_valid(new_array)) {
            // 新槽按该集合默认值（原数组众数）填充，而不是 0
            fill_new_slots_with_modal(array, new_array, len, (size_t) new_size);
            patchlib_field_set_value(field, NULL, &new_array);
            TEKLOG_DEBUG(
                "resize_all_itemid_sets: ✅ resized ItemID.Sets.%s (%zu -> %d)",
                name ? name : "?", len, new_size);
            patchlib_free(new_array);
            resized++;
        } else {
            TEKLOG_ERROR("resize_all_itemid_sets: failed to resize %s",
                         name ? name : "?");
            error++;
        }

        patchlib_free(field);
        patchlib_free(array);
    }

    tefstd_vector_destroy(&fields);

    TEKLOG_INFO("resize_all_class_sets: %s.%s resized=%d skipped=%d "
                "(excluded=%d invalid=%d lenMismatch=%d base=%d) errors=%d",
                ns, cls, resized, skipped, skip_excluded, skip_invalid,
                skip_len, base_count, error);
    if (skipped && skip_sample[0])
        TEKLOG_DEBUG("  skip sample: %s", skip_sample);
    patchlib_free(sets_class);
}

// 一次性扩容所有按 ItemID.Count 定长、按物品 type 索引的静态集合类：
// - ItemID.Sets：116 个集合（ItemIconPulse、TrapSigned、TextureCopyLoad 等）
// - PrefixLegacy+ItemSets：7 个 bool[]（Item.Prefix 掷前缀路径，无边界检查，
//   合成产物 Prefix(-1) 时越界导致材料消耗但物品消失）
// - AmmoID+Sets：3 个 bool[]（弹药类型判断，无边界检查）
static void resize_all_itemid_sets(const int base_count, const int new_size) {
    resize_all_class_sets("Terraria.ID", "ItemID", "Sets", base_count, new_size);
    resize_all_class_sets("Terraria.GameContent.Prefixes", "PrefixLegacy",
                          "ItemSets", base_count, new_size);
    resize_all_class_sets("Terraria.ID", "AmmoID", "Sets", base_count, new_size);
}

// Main.itemAnimations 是 DrawAnimation[ItemID.Count]，
// 绘制自定义物品图标（ItemSlot.DrawItemIcon / Main.LoadItem /
// 更新动画等）时会按 type 索引，必须同步扩容。 新增区域为
// null，表示自定义物品没有动画帧，符合预期。
static void resize_item_animations(const int new_size) {
    patch_handle_t draw_animation_class =
            patchlib_type_get_type("Terraria.DataStructures", "DrawAnimation");
    if (!patchlib_is_valid(draw_animation_class)) {
        TEKLOG_ERROR("resize_item_animations: Failed to get DrawAnimation class");
        return;
    }

    patch_handle_t main_class = patchlib_type_get_type("Terraria", "Main");
    if (!patchlib_is_valid(main_class)) {
        TEKLOG_ERROR("resize_item_animations: Failed to get Main class");
        patchlib_free(draw_animation_class);
        return;
    }

    patch_handle_t field = patchlib_type_get_field(main_class, "itemAnimations");
    if (patchlib_is_valid(field)) {
        resize_array("Terraria", "Main", "itemAnimations", draw_animation_class,
                     new_size, NULL);
        patchlib_free(field);
    } else {
        TEKLOG_ERROR(
            "resize_item_animations: Failed to get Main.itemAnimations field");
    }

    patchlib_free(main_class);
    patchlib_free(draw_animation_class);
}

// Lang._itemTooltipCache / _itemNameCache 按 ItemID.Count 定长，且
// GetTooltip 无边界检查，自定义物品悬停取 tooltip 时直接越界。
// 扩容后 _itemTooltipCache 新槽位填充 ItemTooltip.None，避免返回 null 引发
// NRE。
static void resize_lang_caches(const int base_count, const int new_size) {
    patch_handle_t tooltip_class =
            patchlib_type_get_type("Terraria.UI", "ItemTooltip");
    patch_handle_t name_class =
            patchlib_type_get_type("Terraria.Localization", "LocalizedText");
    if (!patchlib_is_valid(tooltip_class) || !patchlib_is_valid(name_class)) {
        TEKLOG_ERROR(
            "resize_lang_caches: Failed to get ItemTooltip/LocalizedText class");
        if (patchlib_is_valid(tooltip_class))
            patchlib_free(tooltip_class);
        if (patchlib_is_valid(name_class))
            patchlib_free(name_class);
        return;
    }

    patch_handle_t f_none = patchlib_type_get_field(tooltip_class, "None");
    patch_handle_t none = PATCH_NULL;
    if (patchlib_is_valid(f_none))
        patchlib_field_get_value(f_none, NULL, &none);
    // 新槽位必须填充 ItemTooltip.None，否则 Lang.GetTooltip() 会返回 null，
    // 经 Item.RebuildTooltip() 写入 ToolTip 后在渲染提示时 NRE。
    resize_array("Terraria", "Lang", "_itemTooltipCache", tooltip_class,
                 new_size, patchlib_is_valid(none) ? &none : NULL);

    if (patchlib_is_valid(none))
        patchlib_free(none);
    if (patchlib_is_valid(f_none))
        patchlib_free(f_none);

    // _itemNameCache: LocalizedText[]，新槽位填 LocalizedText.Empty
    // （GetItemName 的钩子会读取该数组，null 会被写回到返回值）。
    patch_handle_t f_empty = patchlib_type_get_field(name_class, "Empty");
    patch_handle_t empty = PATCH_NULL;
    if (patchlib_is_valid(f_empty))
        patchlib_field_get_value(f_empty, NULL, &empty);
    resize_array("Terraria", "Lang", "_itemNameCache", name_class, new_size,
                 patchlib_is_valid(empty) ? &empty : NULL);
    if (patchlib_is_valid(empty))
        patchlib_free(empty);
    if (patchlib_is_valid(f_empty))
        patchlib_free(f_empty);

    // _layerIndexForItemType: int[]（GetSortingLayerIndex 无边界检查，
    // 物品进背包触发排序时按 type 索引，越界导致物品"消失"）
    patch_handle_t int32_type = patchlib_get_basic_type(PATCH_INT32);
    resize_array("Terraria.UI", "ItemSorting", "_layerIndexForItemType",
                 int32_type, new_size, NULL);
    patchlib_free(int32_type);

    patchlib_free(tooltip_class);
    patchlib_free(name_class);
}

// ArmorSetBonuses.SetsContaining 是锯齿数组 ArmorSetBonus[ItemID.Count][]，
// tooltip 生成直接按 item.type 索引且无边界检查。
// 扩容后新槽位填充 vanilla BuildLookup 使用的共享空数组（取 [0] 复用），否则
// foreach null 会 NRE。
static void resize_armor_set_bonuses(const int base_count, const int new_size) {
    patch_handle_t bonus_class =
            patchlib_type_get_type("Terraria.DataStructures", "ArmorSetBonuses");
    if (!patchlib_is_valid(bonus_class)) {
        TEKLOG_ERROR(
            "resize_armor_set_bonuses: Failed to get ArmorSetBonuses class");
        return;
    }

    patch_handle_t f_sets =
            patchlib_type_get_field(bonus_class, "SetsContaining");
    if (!patchlib_is_valid(f_sets)) {
        TEKLOG_ERROR(
            "resize_armor_set_bonuses: Failed to get SetsContaining field");
        patchlib_free(bonus_class);
        return;
    }

    patch_handle_t old_array = PATCH_NULL;
    patchlib_field_get_value(f_sets, NULL, &old_array);
    if (patchlib_is_valid(old_array)) {
        if ((size_t) base_count == patchlib_array_length(old_array)) {
            patch_handle_t empty = PATCH_NULL;
            if (patchlib_array_at(old_array, 0, &empty) && patchlib_is_valid(empty)) {
                patch_handle_t new_array =
                        patchlib_array_resize(old_array, (size_t) new_size, NULL);
                if (patchlib_is_valid(new_array)) {
                    for (int i = base_count; i < new_size; i++) {
                        patchlib_array_set(new_array, (size_t) i, &empty);
                    }
                    patchlib_field_set_value(f_sets, NULL, &new_array);
                    TEKLOG_INFO("resize_armor_set_bonuses: ✅ SetsContaining resized to "
                                "%d (empty slots filled)",
                                new_size);
                    patchlib_free(new_array);
                }
                patchlib_free(empty);
            } else {
                TEKLOG_ERROR(
                    "resize_armor_set_bonuses: Failed to read SetsContaining[0]");
            }
        }
        patchlib_free(old_array);
    }

    patchlib_free(f_sets);
    patchlib_free(bonus_class);
}

// ============================================================================
// 背包「整理」(ItemSorting) 兼容
//
// ItemSorting.SetupWhiteLists 只用原版物品构建 _layerWhiteLists；
// ItemSortingLayer.Validate 会按白名单过滤，自定义物品 type 不在任何白名单里
// 就会被所有分类层丢弃，变成“剩余项”。而 Sort 的放置循环按各分类层的计数
// (_sort_counts) 递减，剩余项不在计数内 —— 无处安放时 _sort_counts[0] 越界抛
// 异常，此时背包已被清空、物品还在缓存里，表现为“整理后物品消失”。
//
// 在每次 SetupWhiteLists 之后把自定义 type 追加进兜底层白名单即可。
static void inject_sorting_whitelists() {
    if (!patchlib_is_valid(f_item_sorting_whitelists))
        return;

    patch_handle_t dict = PATCH_NULL;
    patchlib_field_get_value(f_item_sorting_whitelists, PATCH_NULL, &dict);
    if (!patchlib_is_valid(dict))
        return;

    // 兜底两层：Not Trash（按稀有度）与 Trash（无条件收尾）
    static const char *names[] = {"Last - Not Trash", "Last - Trash"};

#if defined(__ANDROID__)
    // Android：用内核封装的字典/列表 API（内部走 il2cpp_object_get_class，手机端可用）
    for (size_t n = 0; n < sizeof(names) / sizeof(names[0]); ++n) {
        patch_handle_t key = patchlib_string_create(names[n]);
        if (!patchlib_is_valid(key))
            continue;
        patch_handle_t list = PATCH_NULL;
        if (patchlib_dictionary_get_value(dict, &key, &list) &&
            patchlib_is_valid(list)) {
            for (size_t i = 0;
                 i < tefstd_vector_size(&g_terraria_item_registry); ++i) {
                terraria_item_handle_t **pp =
                        tefstd_vector_at(&g_terraria_item_registry, i);
                if (!pp || !*pp || (*pp)->runtime_id < 0)
                    continue;
                const int id = (*pp)->runtime_id;
                patchlib_list_add(list, &id);
            }
            patchlib_free(list);
        }
        patchlib_free(key);
    }
#else
    // 桌面端：il2cpp_object_get_class 对泛型实例会返回坏句柄 → 崩。
    // 改用字段类型句柄取 get_Item，再用其返回类型句柄取 Add，自行 invoke。
    patch_handle_t dict_type =
            patchlib_field_get_type_handle(f_item_sorting_whitelists);
    if (!patchlib_is_valid(dict_type)) {
        patchlib_free(dict);
        return;
    }
    patch_handle_t m_get =
            patchlib_type_get_method_by_param_count(dict_type, "get_Item", 1);
    if (!patchlib_is_valid(m_get)) {
        patchlib_free(dict_type);
        patchlib_free(dict);
        return;
    }

    patch_method_signature_t sig;
    const bool sig_ok = patchlib_method_get_signature(m_get, &sig);
    patch_handle_t m_add = PATCH_NULL;
    if (sig_ok && patchlib_is_valid(sig.return_type_handle))
        m_add = patchlib_type_get_method_by_param_count(sig.return_type_handle,
                                                        "Add", 1);

    for (size_t n = 0; n < sizeof(names) / sizeof(names[0]); ++n) {
        patch_handle_t key = patchlib_string_create(names[n]);
        if (!patchlib_is_valid(key))
            continue;
        patch_handle_t list = PATCH_NULL;
        void *gargs[1] = {&key};
        const bool got = patchlib_method_invoke_args(m_get, dict, &list, gargs);
        if (got && patchlib_is_valid(list)) {
            if (patchlib_is_valid(m_add)) {
                for (size_t i = 0;
                     i < tefstd_vector_size(&g_terraria_item_registry); ++i) {
                    terraria_item_handle_t **pp =
                            tefstd_vector_at(&g_terraria_item_registry, i);
                    if (!pp || !*pp || (*pp)->runtime_id < 0)
                        continue;
                    const int id = (*pp)->runtime_id;
                    void *aargs[1] = {&id};
                    patchlib_method_invoke_args(m_add, list, PATCH_NULL, aargs);
                }
            }
            patchlib_free(list);
        }
        patchlib_free(key);
    }

    if (patchlib_is_valid(m_add))
        patchlib_free(m_add);
    if (sig_ok)
        patchlib_method_signature_free(&sig);
    patchlib_free(m_get);
    patchlib_free(dict_type);
#endif

    patchlib_free(dict);
}

static void setup_white_lists_postfix(patch_handle_t this, void **args,
                                      void *result,
                                      const patch_method_signature_t *sig_info) {
    (void) this;
    (void) args;
    (void) result;
    (void) sig_info;
    inject_sorting_whitelists();
}

void terraria_item_manager_init() {
    if (!tefstd_vector_init(&g_terraria_item_registry,
                            sizeof(terraria_item_handle_t *)))
        return;

    terraria_item_manager_register_item(&unknown_item);


    {
        patch_handle_t content_samples =
                patchlib_type_get_type("Terraria.ID", "ContentSamples");

        // 获取 Initialize 方法
        //    Initialize 通常是静态无参方法
        patch_handle_t init_method =
                patchlib_type_get_method_by_param_count(content_samples, "Initialize", 0);

        // 安装 Postfix Hook（prefix 传 NULL，只要 postfix）
        patchlib_install_prepost_hook(init_method,
                                      NULL, // 不需要 prefix
                                      content_samples_initialize_postfix); // postfix
    }

    patch_handle_t creative_item_sacrifices_catalog_cls = patchlib_type_get_type("Terraria.GameContent.Creative", "CreativeItemSacrificesCatalog");
    patch_handle_t creative_item_sacrifices_catalog_initialize = patchlib_type_get_method_by_param_count(creative_item_sacrifices_catalog_cls, "Initialize", 0);
    patchlib_install_prepost_hook(creative_item_sacrifices_catalog_initialize,
                                  NULL, // 不需要 prefix
                                  creative_item_sacrifices_catalog_initialize_postfix); // postfix

    patchlib_free(creative_item_sacrifices_catalog_cls);
    patchlib_free(creative_item_sacrifices_catalog_initialize);

    patch_handle_t item_id_class =
            patchlib_type_get_type("Terraria.ID", "ItemID");
    item_id_count = patchlib_type_get_field(item_id_class, "Count");

    patch_handle_t item_class = patchlib_type_get_type("Terraria", "Item");
    f_type = patchlib_type_get_field(item_class, "type");
    f_stack = patchlib_type_get_field(item_class, "stack");
    f_i_accessory = patchlib_type_get_field(item_class, "accessory");
    f_i_vanity = patchlib_type_get_field(item_class, "vanity");
    f_i_damage = patchlib_type_get_field(item_class, "damage");
    f_i_melee = patchlib_type_get_field(item_class, "melee");
    f_i_magic = patchlib_type_get_field(item_class, "magic");
    f_i_ranged = patchlib_type_get_field(item_class, "ranged");
    f_i_summon = patchlib_type_get_field(item_class, "summon");
    m_rebuild_tooltip = patchlib_type_get_method_by_param_count(item_class, "RebuildTooltip", 0);
    TEKLOG_INFO("item: RebuildTooltip method=%p", m_rebuild_tooltip);
    m_reset_stats =
            patchlib_type_get_method_by_param_count(item_class, "ResetStats", 1);
    patch_handle_t set_defaults =
            patchlib_type_get_method_by_param_count(item_class, "SetDefaults", 2);

    patchlib_install_prepost_hook(set_defaults, NULL, set_defaults_postfix);

    /* 按需补归类（数组扩容完成、且拿到真实实例时最可靠） */
    patch_handle_t get_rollable =
            patchlib_type_get_method_by_param_count(item_class, "GetRollablePrefixes", 0);
    if (patchlib_is_valid(get_rollable)) {
      patchlib_install_prepost_hook(get_rollable, get_rollable_prefixes_prefix, NULL);
      TEKLOG_INFO("item_manager: hooked Item.GetRollablePrefixes");
    } else {
      TEKLOG_WARN("item_manager: Item.GetRollablePrefixes not found");
    }

    patch_handle_t texture_asset_class =
            patchlib_type_get_type("Terraria.GameContent", "TextureAssets");

    f_texture_item = patchlib_type_get_field(texture_asset_class, "Item");
    f_texture_item_flame =
            patchlib_type_get_field(texture_asset_class, "ItemFlame");

    patch_handle_t lang_cls = patchlib_type_get_type("Terraria", "Lang");
    f_item_name_cache = patchlib_type_get_field(lang_cls, "_itemNameCache");
    f_item_tooltip_cache = patchlib_type_get_field(lang_cls, "_itemTooltipCache");
    patch_handle_t get_item_name =
            patchlib_type_get_method_by_param_count(lang_cls, "GetItemName", 1);

    patchlib_install_prepost_hook(get_item_name, NULL, get_item_name_postfix);

    // 背包整理：自定义物品 type 需注入排序白名单（SetupWhiteLists 仅含原版物品）
    patch_handle_t item_sorting_cls =
            patchlib_type_get_type("Terraria.UI", "ItemSorting");
    if (patchlib_is_valid(item_sorting_cls)) {
        f_item_sorting_whitelists =
                patchlib_type_get_field(item_sorting_cls, "_layerWhiteLists");
        patch_handle_t setup_white_lists = patchlib_type_get_method_by_param_count(
                item_sorting_cls, "SetupWhiteLists", 0);
        if (patchlib_is_valid(setup_white_lists)) {
            patchlib_install_prepost_hook(setup_white_lists, NULL,
                                          setup_white_lists_postfix);
            patchlib_free(setup_white_lists);
        }
        patchlib_free(item_sorting_cls);
    }

    patch_handle_t item_tooltip_cls = patchlib_type_get_type("Terraria.UI", "ItemTooltip");
    // 两端签名不同：
    //   桌面  FromLanguageKey(string key)              → 1 参
    //   手机  FromLanguageKey(short id, string key)    → 2 参（见 dump/ItemTooltip/FromLanguageKey.txt）
    m_from_language_key = patchlib_type_get_method_by_param_count(
            item_tooltip_cls, "FromLanguageKey",
#if __ANDROID__
            2
#else
            1
#endif
    );

    // ---- 自定义物品常用字段/方法（存档等功能模块通过 get_item_ops 取用）----
    f_item_prefix = patchlib_type_get_field(item_class, "prefix");
    m_item_net_defaults =
            patchlib_type_get_method_by_param_count(item_class, "netDefaults", 1);
    m_item_prefix =
            patchlib_type_get_method_by_param_count(item_class, "Prefix", 1);

    patchlib_free(item_tooltip_cls);
    patchlib_free(get_item_name);
    patchlib_free(lang_cls);
    patchlib_free(texture_asset_class);
    patchlib_free(item_class);
    patchlib_free(set_defaults);
    patchlib_free(item_id_class);

#if !__ANDROID__
    // PC端：在原版贴图加载完成后重新写入自定义物品贴图（原版会覆盖整个数组）
    patch_handle_t asset_initializer_class =
            patchlib_type_get_type("Terraria.Initializers", "AssetInitializer");
    if (patchlib_is_valid(asset_initializer_class)) {
        patch_handle_t load_textures = patchlib_type_get_method_by_param_count(
            asset_initializer_class, "LoadTextures", 1);
        if (patchlib_is_valid(load_textures)) {
            const patch_hook_id_t hook_id = patchlib_install_prepost_hook(
                load_textures, NULL, load_textures_postfix);
            if (hook_id != PATCH_HOOK_INVALID_ID) {
                TEKLOG_INFO(
                    "AssetInitializer.LoadTextures hook installed successfully");
            } else {
                TEKLOG_ERROR("Failed to install AssetInitializer.LoadTextures hook");
            }
            patchlib_free(load_textures);
        } else {
            TEKLOG_ERROR("Failed to get AssetInitializer.LoadTextures method");
        }
        patchlib_free(asset_initializer_class);
    } else {
        TEKLOG_ERROR("Failed to get AssetInitializer class");
    }
#endif
}

void terraria_item_manager_destroy() {
    // 注意：只释放 vector，不释放 item 本身（由调用者管理）
    tefstd_vector_destroy(&g_terraria_item_registry);
    g_ids_assigned = false;
}

bool terraria_item_manager_register_item(terraria_item_handle_t *item_handle) {
    if (!item_handle || !item_handle->parent_modloader_id ||
        !item_handle->parent_id || !item_handle->internal_name)
        return false;

    /* 检查是否已存在 */
    const size_t count = tefstd_vector_size(&g_terraria_item_registry);
    for (size_t i = 0; i < count; i++) {
        terraria_item_handle_t **existing =
                tefstd_vector_at(&g_terraria_item_registry, i);
        if (!existing || !*existing)
            continue;

        if (strcmp((*existing)->parent_modloader_id,
                   item_handle->parent_modloader_id) == 0 &&
            strcmp((*existing)->parent_id, item_handle->parent_id) == 0 &&
            strcmp((*existing)->internal_name, item_handle->internal_name) == 0) {
            // 热重载：同一 Mod 再次注册同名物品。用新句柄替换旧句柄，并保留
            // 已分配的 runtime_id。
            item_handle->runtime_id = (*existing)->runtime_id;
            *existing = item_handle;
            TEKLOG_INFO("item re-registered (hot reload): %s.%s id=%d",
                        item_handle->parent_id ? item_handle->parent_id : "?",
                        item_handle->internal_name ? item_handle->internal_name : "?",
                        item_handle->runtime_id);
            return true;
        }
    }

    if (g_ids_assigned) {
        // ID 已在 Main.Initialize 分配完成。此时新增物品不会获得有效
        // runtime_id，且按 type 索引的静态数组也不会再扩容，直接使用会越界。
        // 这里只告警；Mod 应在游戏初始化前完成注册。
        TEKLOG_WARN("item registered after IDs assigned: %s.%s.%s "
                    "(runtime_id will stay -1 until re-init)",
                    item_handle->parent_modloader_id ? item_handle->parent_modloader_id : "?",
                    item_handle->parent_id ? item_handle->parent_id : "?",
                    item_handle->internal_name ? item_handle->internal_name : "?");
    }

    /* 加入 registry */
    return tefstd_vector_push_back(&g_terraria_item_registry, &item_handle);
}

bool terraria_item_manager_unregister_item_by_id(const int runtime_id) {
    if (runtime_id < 0)
        return false;

    const size_t count = tefstd_vector_size(&g_terraria_item_registry);
    for (size_t i = 0; i < count; i++) {
        terraria_item_handle_t **pp =
                tefstd_vector_at(&g_terraria_item_registry, i);
        if (!pp || !*pp)
            continue;
        if ((*pp)->runtime_id == runtime_id) {
            const bool removed =
                    tefstd_vector_erase(&g_terraria_item_registry, i, NULL);
            // registry 按下标紧凑映射 runtime_id，删除后必须重新分配，
            // 否则 get_item / set_defaults_postfix / 存档的索引都会错位。
            if (removed)
                terraria_item_manager_assign_ids(terraria_item_id_get_count());
            return removed;
        }
    }
    return false;
}

bool terraria_item_manager_assign_ids(const int base_id) {
    const size_t count = tefstd_vector_size(&g_terraria_item_registry);
    if (count == 0)
        return true;

    // 获取底层数组指针进行排序
    terraria_item_handle_t **items = g_terraria_item_registry.data;

    // 排序
    qsort(items, count, sizeof(terraria_item_handle_t *), compare_item);

    // 分配 ID（从 base_id 开始紧凑分配）
    for (size_t i = 0; i < count; i++)
        items[i]->runtime_id = base_id + (int) i;

    g_ids_assigned = true;
    return true;
}

int terraria_item_id_get_count() {
    short count = 0;
    patchlib_field_get_value(item_id_count, NULL, &count);
    return count;
}

void terraria_item_manager_init_texture2d() {
    TEKLOG_DEBUG("terraria_item_manager_init_texture2d: starting");

    // 检查字段是否有效
    if (!patchlib_is_valid(f_texture_item) ||
        !patchlib_is_valid(f_texture_item_flame)) {
        TEKLOG_ERROR("Texture fields not initialized");
        return;
    }

    // 扩容数组
    patch_handle_t texture_type = terraria_texture2d_get_class();
    patch_handle_t asset_type = terraria_asset_get_generic_class(texture_type);

    resize_array("Terraria.GameContent", "TextureAssets", "Item", asset_type,
                 terraria_item_manager_get_count(), NULL);
    resize_array("Terraria.GameContent", "TextureAssets", "ItemFlame", asset_type,
                 terraria_item_manager_get_count(), NULL);

    patchlib_free(texture_type);
    patchlib_free(asset_type);

    // 初始化
    patch_handle_t texture_item_array = PATCH_NULL;
    patch_handle_t texture_item_flame_array = PATCH_NULL;
    patchlib_field_get_value(f_texture_item, NULL, &texture_item_array);
    patchlib_field_get_value(f_texture_item_flame, NULL,
                             &texture_item_flame_array);

    if (!patchlib_is_valid(texture_item_array) ||
        !patchlib_is_valid(texture_item_flame_array)) {
        TEKLOG_ERROR("Failed to get texture arrays");
        return;
    }

    int base_count = terraria_item_id_get_count();
    size_t custom_count = tefstd_vector_size(&g_terraria_item_registry);
    size_t array_len = patchlib_array_length(texture_item_array);

    TEKLOG_INFO("init_texture2d: array_len=%zu, base_count=%d, custom_count=%zu, "
                "start_index=%d",
                array_len, base_count, custom_count, base_count);

    // 创建默认纹理
    patch_handle_t unknow_texture = unknown_item_get_texture(NULL);
    if (!patchlib_is_valid(unknow_texture)) {
        TEKLOG_ERROR("Failed to create default texture");
        return;
    }
    TEKLOG_DEBUG("Created default texture: %p", unknow_texture);

    int success_count = 0;
    int fail_count = 0;

    for (size_t i = 0; i < custom_count; ++i) {
        const size_t index = (size_t) base_count + i;

        terraria_item_handle_t **item_handle_ptr =
                tefstd_vector_at(&g_terraria_item_registry, i);
        if (!item_handle_ptr || !*item_handle_ptr) {
            TEKLOG_WARN("init_texture2d: invalid item_handle at index %zu", i);
            fail_count++;
            continue;
        }

        terraria_item_handle_t *item_handle = *item_handle_ptr;
        TEKLOG_DEBUG("init_texture2d: processing item %zu: %s.%s (index=%zu)", i,
                     item_handle->parent_id, item_handle->internal_name, index);

        patch_handle_t itx = PATCH_NULL;
        if (item_handle->item_ops.get_texture) {
            itx = item_handle->item_ops.get_texture(item_handle);
            TEKLOG_DEBUG("init_texture2d: get_texture returned %p for %s.%s", itx,
                         item_handle->parent_id, item_handle->internal_name);
        }

        if (patchlib_is_valid(itx)) {
            if (patchlib_array_set(texture_item_array, index, &itx)) {
                patchlib_array_set(texture_item_flame_array, index, &itx);
                success_count++;
                TEKLOG_DEBUG(
                    "init_texture2d: set custom texture for %s.%s at index %zu",
                    item_handle->parent_id, item_handle->internal_name, index);
            } else {
                TEKLOG_ERROR("init_texture2d: failed to set texture at index %zu",
                             index);
                fail_count++;
            }
            patchlib_free(itx);
        } else {
            // 使用默认纹理
            if (patchlib_array_set(texture_item_array, index, &unknow_texture)) {
                patchlib_array_set(texture_item_flame_array, index, &unknow_texture);
                TEKLOG_DEBUG(
                    "init_texture2d: set default texture for %s.%s at index %zu",
                    item_handle->parent_id, item_handle->internal_name, index);
            } else {
                TEKLOG_ERROR(
                    "init_texture2d: failed to set default texture at index %zu",
                    index);
                fail_count++;
            }
        }
    }

    TEKLOG_INFO("init_texture2d: completed: %d success, %d failed, %zu total "
                "custom items",
                success_count, fail_count, custom_count);

    patchlib_free(unknow_texture);
    patchlib_free(texture_item_array);
    patchlib_free(texture_item_flame_array);
}

static void resize_array(const char *ns, const char *cls, const char *fid,
                         patch_handle_t type, const int nsize, void *dvalue) {
    // 获取类型和字段
    patch_handle_t class_handle = patchlib_type_get_type(ns, cls);
    if (!patchlib_is_valid(class_handle)) {
        TEKLOG_ERROR("resize_array: Failed to get class: %s.%s", ns, cls);
        return;
    }

    patch_handle_t field = patchlib_type_get_field(class_handle, fid);
    if (!patchlib_is_valid(field)) {
        TEKLOG_ERROR("resize_array: Failed to get field: %s.%s.%s", ns, cls, fid);
        patchlib_free(class_handle);
        return;
    }

    // 获取旧数组
    patch_handle_t array = PATCH_NULL;
    patchlib_field_get_value(field, NULL, &array);

    if (!patchlib_is_valid(array)) {
        TEKLOG_WARN(
            "resize_array: Array %s.%s.%s is NULL, creating new array of size %d",
            ns, cls, fid, nsize);
        patch_handle_t narray = patchlib_array_create(nsize, type);
        if (patchlib_is_valid(narray)) {
            if (dvalue) patchlib_array_fill(narray, dvalue);
            patchlib_field_set_value(field, NULL, &narray);
            TEKLOG_INFO("resize_array: Created new array %s.%s.%s of size %d", ns,
                        cls, fid, nsize);
            patchlib_free(narray);
        }
        patchlib_free(field);
        patchlib_free(class_handle);
        return;
    }

    const uint32_t old_len = patchlib_array_length(array);

    if (old_len >= (uint32_t) nsize) {
        TEKLOG_DEBUG("resize_array: %s.%s.%s already large enough: %u >= %d", ns,
                     cls, fid, old_len, nsize);
        patchlib_free(array);
        patchlib_free(field);
        patchlib_free(class_handle);
        return;
    }

    TEKLOG_DEBUG("resize_array: Resizing %s.%s.%s from %u to %d", ns, cls, fid,
                 old_len, nsize);

    // 创建新数组
    patch_handle_t narray = patchlib_array_create(nsize, type);
    if (!patchlib_is_valid(narray)) {
        TEKLOG_ERROR(
            "resize_array: Failed to create new array of size %d for %s.%s.%s",
            nsize, ns, cls, fid);
        patchlib_free(array);
        patchlib_free(field);
        patchlib_free(class_handle);
        return;
    }
    if (dvalue) patchlib_array_fill(narray, dvalue);

    // 复制所有旧数据到新数组
    if (old_len > 0) {
        if (!patchlib_array_copy(narray, array, old_len)) {
            TEKLOG_ERROR("resize_array: Failed to copy data for %s.%s.%s", ns, cls,
                         fid);
        }
    }

    // 更新字段
    patchlib_field_set_value(field, NULL, &narray);
    TEKLOG_INFO("resize_array: ✅ %s.%s.%s resized from %u to %d", ns, cls, fid,
                old_len, nsize);

    // 清理资源
    patchlib_free(narray);
    patchlib_free(array);
    patchlib_free(field);
    patchlib_free(class_handle);
}

// 强制把 ItemID.Sets 中若干“按物品 type 索引、运行时会访问”的集合扩容到
// new_size。与 resize_all_class_sets 不同：这里不要求旧长度恰好等于
// ItemID.Count，只要旧长度 < new_size 就扩容。
//
// 背景：Main.UpdateWorld_Items 对每个 WorldItem.UpdateItem 做 try/catch，
// 一旦自定义物品更新时抛 IndexOutOfRangeException，会被 Main.ignoreErrors
// 静默吞掉并把该槽位换成空 WorldItem（表现为“掉落物存在一帧后消失”）。
// WorldItem.UpdateItem 只会访问这些 ItemID.Sets 数组，因此在最终阶段再兜底
// 强制扩一次，即使前面的批量扩容因静态构造时序/失败被跳过也能兜住。
static void force_resize_itemid_set(const char *name, const int new_size) {
    if (!name || new_size <= 0)
        return;

    patch_handle_t item_id_class =
            patchlib_type_get_type("Terraria.ID", "ItemID");
    if (!patchlib_is_valid(item_id_class)) {
        TEKLOG_WARN("force_resize_itemid_set: ItemID not found");
        return;
    }
    patch_handle_t sets_class =
            patchlib_type_get_inner_type(item_id_class, "Sets");
    patchlib_free(item_id_class);
    if (!patchlib_is_valid(sets_class))
        return;

    patch_handle_t field = patchlib_type_get_field(sets_class, name);
    if (!patchlib_is_valid(field)) {
        patchlib_free(sets_class);
        return;
    }

    patch_handle_t array = PATCH_NULL;
    patchlib_field_get_value(field, NULL, &array);
    if (patchlib_is_valid(array)) {
        const size_t len = patchlib_array_length(array);
        if (len > 0 && len < (size_t) new_size) {
            patch_handle_t new_array =
                    patchlib_array_resize(array, (size_t) new_size, NULL);
            if (patchlib_is_valid(new_array)) {
                fill_new_slots_with_modal(array, new_array, len, (size_t) new_size);
                patchlib_field_set_value(field, NULL, &new_array);
                TEKLOG_INFO("force_resize_itemid_set: ✅ ItemID.Sets.%s (%zu -> %d)",
                            name, len, new_size);
                patchlib_free(new_array);
            } else {
                TEKLOG_ERROR("force_resize_itemid_set: failed to resize %s", name);
            }
        }
        patchlib_free(array);
    }

    patchlib_free(field);
    patchlib_free(sets_class);
}

// WorldItem.UpdateItem / 拾取 / 绘制路径上真正会被访问到的集合。
static void force_resize_critical_item_sets(const int new_size) {
    static const char *names[] = {
        // WorldItem.UpdateItem
        "ItemNoGravity",
        "RecoverableImportantItem",
        "IsLavaImmuneRegardlessOfRarity",
        "BossBag",
        "Torches",
        "WaterTorches",
        "CommonCoin",
        "CanPassivelyStackInWorldOverride",
        "OnlyNeedOneInInventoryOverride",
        // 拾取 / 背包
        "IsAPickup",
        "IgnoresEncumberingStone",
        "NebulaPickup",
        "ItemsThatShouldNotBeInInventory",
        // 绘制 / 贴图
        "IsFood",
        "AnimatesAsSoul",
        "TrapSigned",
        "DrawUnsafeIndicator",
        "ItemIconPulse",
        "TextureCopyLoad",
        // Item.NewItem / DropSelectedItem
        "IsAMaterial",
        "OverflowProtectionTimeOffset",
        "DropWithGrabDelayForAllPlayers",
        "Deprecated",
        // 曾被排除、但同样按物品 type 索引的集合
        "CanBeQuickusedOnGamepad",
        "ForceConsumption",
        "ForcesBreaksSleeping",
        "LockOnAimCompensation",
        "NetUseSoundSync",
    };

    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i)
        force_resize_itemid_set(names[i], new_size);
}

// Terraria.GameContent.QuickStacking.MatchingItemTypeDestinationList
// .firstEntryForType 是 int[ItemID.Count]，在 Tail/Add/Lookup 中直接按物品
// type 索引且无边界检查。快速堆叠到箱子/银行时会用它给自定义物品分类，
// 不扩容就会 IndexOutOfRangeException。该实例是 QuickStacking 的静态字段
// matchingItemTypeScratch。新槽位填 0（表示无条目）即可。
static void resize_quickstacking(const int new_size) {
    if (new_size <= 0)
        return;

    patch_handle_t qs_class =
            patchlib_type_get_type("Terraria.GameContent", "QuickStacking");
    if (!patchlib_is_valid(qs_class))
        return;

    patch_handle_t nested_class =
            patchlib_type_get_inner_type(qs_class, "MatchingItemTypeDestinationList");
    patch_handle_t f_scratch =
            patchlib_type_get_field(qs_class, "matchingItemTypeScratch");
    if (!patchlib_is_valid(nested_class) || !patchlib_is_valid(f_scratch)) {
        if (patchlib_is_valid(nested_class))
            patchlib_free(nested_class);
        if (patchlib_is_valid(f_scratch))
            patchlib_free(f_scratch);
        patchlib_free(qs_class);
        return;
    }

    patch_handle_t scratch = PATCH_NULL;
    patchlib_field_get_value(f_scratch, NULL, &scratch);
    if (!patchlib_is_valid(scratch)) {
        patchlib_free(f_scratch);
        patchlib_free(nested_class);
        patchlib_free(qs_class);
        return;
    }

    patch_handle_t f_first =
            patchlib_type_get_field(nested_class, "firstEntryForType");
    if (patchlib_is_valid(f_first)) {
        patch_handle_t array = PATCH_NULL;
        patchlib_field_get_value(f_first, scratch, &array);
        if (patchlib_is_valid(array)) {
            const size_t len = patchlib_array_length(array);
            if (len > 0 && len < (size_t) new_size) {
                patch_handle_t int_type = patchlib_get_basic_type(PATCH_INT32);
                patch_handle_t narray =
                        patchlib_array_create(new_size, int_type);
                patchlib_free(int_type);
                if (patchlib_is_valid(narray)) {
                    // 新数组默认全 0；复制旧数据
                    patchlib_array_copy(narray, array, len);
                    patchlib_field_set_value(f_first, scratch, &narray);
                    TEKLOG_INFO("resize_quickstacking: ✅ firstEntryForType (%zu -> %d)",
                                len, new_size);
                    patchlib_free(narray);
                } else {
                    TEKLOG_ERROR("resize_quickstacking: failed to create new array");
                }
            }
            patchlib_free(array);
        }
        patchlib_free(f_first);
    }

    patchlib_free(scratch);
    patchlib_free(f_scratch);
    patchlib_free(nested_class);
    patchlib_free(qs_class);
}

void terraria_item_manager_resize() {
    const int new_size = terraria_item_manager_get_count();

    patch_handle_t bool_type = patchlib_get_basic_type(PATCH_BOOL);
    patch_handle_t int_type = patchlib_get_basic_type(PATCH_INT32);

    static int d_cached_item_spawns_type = -1;

    // 注意：这些数组在 Terraria 中用于存储物品相关的数据
    resize_array("Terraria", "Item", "claw", bool_type, new_size, NULL);
    resize_array("Terraria", "Item", "staff", bool_type, new_size, NULL);
    resize_array("Terraria", "Item", "cachedItemSpawnsByType", int_type, new_size, &d_cached_item_spawns_type);

#if __ANDROID__
    resize_array("Terraria", "Player", "ItemUsesRightFire", bool_type, new_size, NULL);
    resize_array("", "VirtualControllerInputState", "ItemCategories",
                 int_type, new_size, NULL);
#endif
    // 一次性扩容 ItemID.Sets 所有按 ItemID.Count 定长的数组
    resize_all_itemid_sets(terraria_item_id_get_count(), new_size);
    // 绘制物品图标时按 type 索引 Main.itemAnimations，必须同步扩充
    resize_item_animations(new_size);
    // tooltip/名称缓存与物品排序索引同步扩容（无边界检查，越界会导致物品"消失"）
    resize_lang_caches(terraria_item_id_get_count(), new_size);
    // 护甲套装查询锯齿数组同步扩容（tooltip 生成路径）
    resize_armor_set_bonuses(terraria_item_id_get_count(), new_size);
    // 兜底：强制扩容运行时物品更新/拾取/绘制会用到的 ItemID.Sets 集合
    force_resize_critical_item_sets(new_size);
    // 快速堆叠按 type 索引的 firstEntryForType（无边界检查）
    resize_quickstacking(new_size);

    patchlib_free(bool_type);
    patchlib_free(int_type);
}

int terraria_item_manager_get_count() {
    return (int) tefstd_vector_size(&g_terraria_item_registry) +
           terraria_item_id_get_count();
}

terraria_item_handle_t *terraria_item_manager_get_item(int runtime_id) {
    terraria_item_handle_t **r = tefstd_vector_at(&g_terraria_item_registry, runtime_id - terraria_item_id_get_count());
    if (r && *r)
        return *r;

    return NULL;
}

tefstd_vector_t *terraria_item_manager_get_items() {
    return &g_terraria_item_registry;
}

void terraria_item_manager_get_item_ops(terraria_item_handles_t *out) {
    if (!out)
        return;
    out->f_type = f_type;
    out->f_stack = f_stack;
    out->f_item_prefix = f_item_prefix;
    out->m_net_defaults = m_item_net_defaults;
    out->m_prefix = m_item_prefix;
}

terraria_item_handle_t *terraria_item_manager_find_item(const char *loader,
                                                        const char *mod,
                                                        const char *name) {
    if (!loader || !mod || !name)
        return NULL;
    const size_t count = tefstd_vector_size(&g_terraria_item_registry);
    for (size_t i = 0; i < count; ++i) {
        terraria_item_handle_t **pp =
                tefstd_vector_at(&g_terraria_item_registry, i);
        if (!pp || !*pp)
            continue;
        terraria_item_handle_t *h = *pp;
        if (h->parent_modloader_id && h->parent_id && h->internal_name &&
            strcmp(h->parent_modloader_id, loader) == 0 &&
            strcmp(h->parent_id, mod) == 0 &&
            strcmp(h->internal_name, name) == 0)
            return h;
    }
    return NULL;
}

terraria_item_handle_t *terraria_item_manager_unknown_item() {
    return &unknown_item;
}

static char *generate_key(size_t key_size, const char *prefix, terraria_item_handle_t *current) {
    // 计算所需缓冲区大小
    if (current->parent_modloader_id)
        key_size += strlen(current->parent_modloader_id);
    else
        key_size += 1;

    if (current->parent_id)
        key_size += strlen(current->parent_id);
    else
        key_size += 1;

    if (current->internal_name)
        key_size += strlen(current->internal_name);
    else
        key_size += 1;

    // 分配并拼接键名
    char *key_buffer = malloc(key_size);
    snprintf(key_buffer, key_size, "%s_%s_%s.%s", prefix,
             current->parent_modloader_id ? current->parent_modloader_id : "?",
             current->parent_id ? current->parent_id : "?",
             current->internal_name ? current->internal_name : "?");

    return key_buffer;
}

void terraria_item_manager_init_localized_text() {
    patch_handle_t array_item_name_cache = PATCH_NULL;
    patch_handle_t array_item_tooltip_cache = PATCH_NULL;
    patchlib_field_get_value(f_item_name_cache, PATCH_NULL, &array_item_name_cache);
    patchlib_field_get_value(f_item_tooltip_cache, PATCH_NULL, &array_item_tooltip_cache);

    const int org_count = terraria_item_id_get_count();

    for (int i = 0; i < tefstd_vector_size(&g_terraria_item_registry); ++i) {
        // vector 存的是 terraria_item_handle_t*，所以先取指针，再解引用
        terraria_item_handle_t **pp_current =
                tefstd_vector_at(&g_terraria_item_registry,
                                 i);

        if (!pp_current || !*pp_current)
            continue;

        terraria_item_handle_t *current = *pp_current;

        // 分配并拼接键名
        char *item_name_key_buffer = generate_key(13, "item_name", current);
        int target_index = org_count + i;

        patch_handle_t item_name_key =
                terraria_language_manager_get_localized_text(item_name_key_buffer);

        patchlib_array_set(array_item_name_cache, target_index, &item_name_key);
        patchlib_free(item_name_key);
        free(item_name_key_buffer);

        if (current->has_tooltip && patchlib_is_valid(m_from_language_key)) {
            // 注意：generate_key 需要包含结尾 '\0'。格式 "%s_%s_%s.%s"
            // 的分隔符共 3 个，"item_tooltip" 长度为 12，故基数应为 12+3+1=16。
            char *item_tooltip_key_buffer = generate_key(16, "item_tooltip", current);

            // ItemTooltip.FromLanguageKey：桌面为 (string key)；手机为 (short id, string key)。
            patch_handle_t item_tooltip_key_str =
                    patchlib_string_create(item_tooltip_key_buffer);
            patch_handle_t item_tooltip = PATCH_NULL;

#if __ANDROID__
            int16_t tooltip_id = (int16_t) target_index;
            void *args[2] = {&tooltip_id, &item_tooltip_key_str};
#else
            void *args[1] = {&item_tooltip_key_str};
#endif
            patchlib_method_invoke_args(m_from_language_key, NULL, &item_tooltip, args);

            if (patchlib_is_valid(item_tooltip))
                patchlib_array_set(array_item_tooltip_cache, target_index, &item_tooltip);

            patchlib_free(item_tooltip);
            patchlib_free(item_tooltip_key_str);
            free(item_tooltip_key_buffer);
        }
    }

    patchlib_free(array_item_tooltip_cache);
    patchlib_free(array_item_name_cache);
    // 注意：不要释放 f_item_name_cache / f_item_tooltip_cache，
    // 它们是 item_manager 持有的全局字段句柄，get_item_name_postfix
    // 等钩子仍会继续使用。
}
