/*******************************************************************************
 * tefkernel - npc_manager
 * Copyright (C) 2026 eternalfuture-e38299
 *
 * 自定义 NPC：注册 / ID 分配 / 贴图 / SetDefaults / AI 分发 / 生成。
 * 结构与 projectile_manager 同构。
 *******************************************************************************/

#include "internal/terraria/npc_manager.h"
#include "internal/log.h"

#include "patchlib/field.h"
#include "patchlib/method.h"
#include "patchlib/property.h"
#include "patchlib/struct/array.h"
#include "terraria/asset.h"
#include "terraria/texture2d.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

tefstd_vector_t g_terraria_npc_registry;

static patch_handle_t npc_class = PATCH_NULL;
static patch_handle_t f_type = PATCH_NULL;
static patch_handle_t npc_id_count = PATCH_NULL;       /* NPCID.Count (short) */
static patch_handle_t f_texture_npc = PATCH_NULL;      /* TextureAssets.Npc */
static patch_handle_t f_npc_frame_count = PATCH_NULL;  /* Main.npcFrameCount */
static patch_hook_id_t h_set_defaults = PATCH_HOOK_INVALID_ID;
static patch_hook_id_t h_ai = PATCH_HOOK_INVALID_ID;
static bool g_ids_assigned = false;

/* ------------------------------------------------------------------ */
/* 注册表                                                              */
/* ------------------------------------------------------------------ */

static int compare_npc(const void *a, const void *b) {
    const terraria_npc_handle_t *pa = *(const terraria_npc_handle_t **) a;
    const terraria_npc_handle_t *pb = *(const terraria_npc_handle_t **) b;

    int cmp = strcmp(pa->parent_modloader_id, pb->parent_modloader_id);
    if (cmp != 0) return cmp;
    cmp = strcmp(pa->parent_id, pb->parent_id);
    if (cmp != 0) return cmp;
    return strcmp(pa->internal_name, pb->internal_name);
}

bool terraria_npc_manager_register_npc(terraria_npc_handle_t *npc_handle) {
    if (!npc_handle || !npc_handle->parent_modloader_id ||
        !npc_handle->parent_id || !npc_handle->internal_name)
        return false;

    const size_t count = tefstd_vector_size(&g_terraria_npc_registry);
    for (size_t i = 0; i < count; ++i) {
        terraria_npc_handle_t **existing =
                tefstd_vector_at(&g_terraria_npc_registry, i);
        if (!existing || !*existing)
            continue;
        if (strcmp((*existing)->parent_modloader_id,
                   npc_handle->parent_modloader_id) == 0 &&
            strcmp((*existing)->parent_id, npc_handle->parent_id) == 0 &&
            strcmp((*existing)->internal_name, npc_handle->internal_name) == 0) {
            npc_handle->runtime_id = (*existing)->runtime_id;
            *existing = npc_handle;
            TEKLOG_INFO("npc re-registered: %s.%s id=%d",
                        npc_handle->parent_id, npc_handle->internal_name,
                        npc_handle->runtime_id);
            return true;
        }
    }

    if (g_ids_assigned)
        TEKLOG_WARN("npc registered after IDs assigned: %s.%s",
                    npc_handle->parent_id, npc_handle->internal_name);

    return tefstd_vector_push_back(&g_terraria_npc_registry, &npc_handle);
}

bool terraria_npc_manager_unregister_npc_by_id(const int runtime_id) {
    if (runtime_id < 0)
        return false;
    const size_t count = tefstd_vector_size(&g_terraria_npc_registry);
    for (size_t i = 0; i < count; ++i) {
        terraria_npc_handle_t **pp =
                tefstd_vector_at(&g_terraria_npc_registry, i);
        if (!pp || !*pp)
            continue;
        if ((*pp)->runtime_id == runtime_id) {
            const bool removed =
                    tefstd_vector_erase(&g_terraria_npc_registry, i, NULL);
            if (removed)
                terraria_npc_manager_assign_ids(terraria_npc_id_get_count());
            return removed;
        }
    }
    return false;
}

bool terraria_npc_manager_assign_ids(const int base_id) {
    const size_t count = tefstd_vector_size(&g_terraria_npc_registry);
    if (count == 0) {
        g_ids_assigned = true;
        return true;
    }
    terraria_npc_handle_t **items = g_terraria_npc_registry.data;
    qsort(items, count, sizeof(terraria_npc_handle_t *), compare_npc);
    for (size_t i = 0; i < count; ++i)
        items[i]->runtime_id = base_id + (int) i;
    g_ids_assigned = true;
    return true;
}

int terraria_npc_id_get_count(void) {
    short count = 0;
    if (patchlib_is_valid(npc_id_count))
        patchlib_field_get_value(npc_id_count, NULL, &count);
    return count;
}

terraria_npc_handle_t *terraria_npc_manager_get_npc(const int runtime_id) {
    const int base = terraria_npc_id_get_count();
    const int nid = runtime_id - base;
    const size_t count = tefstd_vector_size(&g_terraria_npc_registry);
    if (nid < 0 || nid >= (int) count)
        return NULL;
    terraria_npc_handle_t **pp =
            tefstd_vector_at(&g_terraria_npc_registry, (size_t) nid);
    return pp ? *pp : NULL;
}

tefstd_vector_t *terraria_npc_manager_get_npcs(void) {
    return &g_terraria_npc_registry;
}

terraria_npc_handle_t *terraria_npc_manager_find_npc(
        const char *loader, const char *mod, const char *name) {
    if (!loader || !mod || !name)
        return NULL;
    const size_t count = tefstd_vector_size(&g_terraria_npc_registry);
    for (size_t i = 0; i < count; ++i) {
        terraria_npc_handle_t **pp =
                tefstd_vector_at(&g_terraria_npc_registry, i);
        if (!pp || !*pp)
            continue;
        if (strcmp((*pp)->parent_modloader_id, loader) == 0 &&
            strcmp((*pp)->parent_id, mod) == 0 &&
            strcmp((*pp)->internal_name, name) == 0)
            return *pp;
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* 数组扩容                                                            */
/* ------------------------------------------------------------------ */

/* 用原数组里出现次数最多的元素填充新槽位（NPCID.Sets 里很多集合
 * 的默认值不是 0，例如 TrailCacheLength = 10）。 */
static void fill_new_slots_with_modal(patch_handle_t old_array,
                                      patch_handle_t new_array,
                                      const size_t old_len,
                                      const size_t new_len) {
    enum { MAX_UNIQ = 64, EBUF = 64 };
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
    for (int j = 1; j < nu; ++j)
        if (counts[j] > counts[best])
            best = j;
    for (size_t i = old_len; i < new_len; ++i)
        if (!patchlib_array_set(new_array, i, vals[best]))
            break;
}

static bool field_is_array(patch_handle_t field) {
    patch_handle_t t = patchlib_field_get_type_handle(field);
    if (!patchlib_is_valid(t))
        return false;
    const char *name = patchlib_type_get_name(t);
    bool is_array = false;
    if (name) {
        const size_t len = strlen(name);
        is_array = len >= 2 && strcmp(name + len - 2, "[]") == 0;
    }
    patchlib_free(t);
    return is_array;
}

/* 把按 NPC type 索引的静态数组扩到 new_size；只处理长度正好等于 base 的数组 */
static void resize_static_array(patch_handle_t field, const char *name,
                                const int base, const int new_size,
                                const bool modal, const int int_fill,
                                const bool fill_with_int) {
    if (!patchlib_is_valid(field)) {
        TEKLOG_WARN("npc resize: field %s not found", name ? name : "?");
        return;
    }
    patch_handle_t array = PATCH_NULL;
    patchlib_field_get_value(field, NULL, &array);
    if (!patchlib_is_valid(array))
        return;

    const size_t len = patchlib_array_length(array);
    if (len != (size_t) base) {
        TEKLOG_DEBUG("npc resize: %s len=%zu != base=%d, skip",
                     name ? name : "?", len, base);
        patchlib_free(array);
        return;
    }

    patch_handle_t bigger = PATCH_NULL;
    if (fill_with_int) {
        int fill = int_fill;
        bigger = patchlib_array_resize(array, (size_t) new_size, &fill);
    } else {
        bigger = patchlib_array_resize(array, (size_t) new_size, NULL);
    }
    if (patchlib_is_valid(bigger)) {
        if (modal)
            fill_new_slots_with_modal(array, bigger, len, (size_t) new_size);
        patchlib_field_set_value(field, NULL, &bigger);
        TEKLOG_DEBUG("npc resize: %s %zu -> %d", name ? name : "?", len, new_size);
        patchlib_free(bigger);
    } else {
        TEKLOG_ERROR("npc resize: %s resize failed", name ? name : "?");
    }
    patchlib_free(array);
}

/* 处理 Sets 里的所有数组成员 */
static int resize_sets_arrays(const char *ns, const char *cls, const char *inner,
                              const int base, const int new_size,
                              const bool exclude_town_groups) {
    patch_handle_t cls_h = patchlib_type_get_type(ns, cls);
    if (!patchlib_is_valid(cls_h))
        return 0;
    patch_handle_t sets = patchlib_type_get_inner_type(cls_h, inner);
    patchlib_free(cls_h);
    if (!patchlib_is_valid(sets))
        return 0;

    tefstd_vector_t fields;
    tefstd_vector_init(&fields, sizeof(patch_handle_t));
    patchlib_type_get_fields(sets, false, &fields);
    int resized = 0;
    for (size_t i = 0; i < fields.size; ++i) {
        patch_handle_t field = *(patch_handle_t *) tefstd_vector_at(&fields, i);
        if (!patchlib_is_valid(field))
            continue;
        const char *name = patchlib_field_get_name(field);
        if (!patchlib_field_is_static(field) || patchlib_field_is_const(field) ||
            !field_is_array(field) || !name) {
            patchlib_free(field);
            continue;
        }
        if (strcmp(name, "Factory") == 0 || strcmp(name, "Count") == 0 ||
            (exclude_town_groups && strcmp(name, "TownNPCsFramingGroups") == 0)) {
            patchlib_free(field);
            continue;
        }

        patch_handle_t array = PATCH_NULL;
        patchlib_field_get_value(field, NULL, &array);
        if (!array) {
            patchlib_free(field);
            continue;
        }
        const size_t len = patchlib_array_length(array);
        if (len == (size_t) base) {
            patch_handle_t bigger =
                    patchlib_array_resize(array, (size_t) new_size, NULL);
            if (patchlib_is_valid(bigger)) {
                fill_new_slots_with_modal(array, bigger, len, (size_t) new_size);
                patchlib_field_set_value(field, NULL, &bigger);
                resized++;
                patchlib_free(bigger);
            } else {
                TEKLOG_ERROR("npc resize: %s.%s.%s failed",
                             cls, inner, name);
            }
        }
        patchlib_free(array);
        patchlib_free(field);
    }
    tefstd_vector_destroy(&fields);
    patchlib_free(sets);
    return resized;
}

/* 扩容某个类型上的实例数组（如 Player.npcTypeNoAggro） */
static void resize_player_arrays(const int base, const int new_size) {
    patch_handle_t main_cls = patchlib_type_get_type("Terraria", "Main");
    patch_handle_t player_cls = patchlib_type_get_type("Terraria", "Player");
    if (!patchlib_is_valid(main_cls) || !patchlib_is_valid(player_cls))
        return;
    patch_handle_t f_players = patchlib_type_get_field(main_cls, "player");
    patch_handle_t f_no_aggro =
            patchlib_type_get_field(player_cls, "npcTypeNoAggro");

    if (patchlib_is_valid(f_players) && patchlib_is_valid(f_no_aggro)) {
        patch_handle_t players = PATCH_NULL;
        patchlib_field_get_value(f_players, NULL, &players);
        if (patchlib_is_valid(players)) {
            const size_t n = patchlib_array_length(players);
            int patched = 0;
            for (size_t i = 0; i < n; ++i) {
                patch_handle_t player = PATCH_NULL;
                if (!patchlib_array_at(players, i, &player) || !player)
                    continue;
                patch_handle_t arr = PATCH_NULL;
                patchlib_field_get_value(f_no_aggro, player, &arr);
                if (arr && patchlib_array_length(arr) == (size_t) base) {
                    patch_handle_t bigger =
                            patchlib_array_resize(arr, (size_t) new_size, NULL);
                    if (patchlib_is_valid(bigger)) {
                        patchlib_field_set_value(f_no_aggro, player, &bigger);
                        patched++;
                        patchlib_free(bigger);
                    }
                }
                if (arr) patchlib_free(arr);
                patchlib_free(player);
            }
            TEKLOG_INFO("npc resize: npcTypeNoAggro %d/%zu players", patched, n);
        }
        if (patchlib_is_valid(players)) patchlib_free(players);
    }
    if (patchlib_is_valid(f_players)) patchlib_free(f_players);
    if (patchlib_is_valid(f_no_aggro)) patchlib_free(f_no_aggro);
    patchlib_free(main_cls);
    patchlib_free(player_cls);
}

/* Lang._npcNameCache (LocalizedText[]) 按 NPCID.Count 定长 */
static void resize_npc_name_cache(const int base, const int new_size) {
    patch_handle_t lang_cls = patchlib_type_get_type("Terraria", "Lang");
    if (!patchlib_is_valid(lang_cls))
        return;
    patch_handle_t f = patchlib_type_get_field(lang_cls, "_npcNameCache");
    patchlib_free(lang_cls);
    if (!patchlib_is_valid(f))
        return;

    patch_handle_t arr = PATCH_NULL;
    patchlib_field_get_value(f, NULL, &arr);
    if (patchlib_is_valid(arr) && patchlib_array_length(arr) == (size_t) base) {
        patch_handle_t first = PATCH_NULL;
        if (patchlib_array_at(arr, 0, &first) && first) {
            patch_handle_t bigger =
                    patchlib_array_resize(arr, (size_t) new_size, &first);
            if (patchlib_is_valid(bigger)) {
                patchlib_field_set_value(f, NULL, &bigger);
                TEKLOG_INFO("npc resize: Lang._npcNameCache %d -> %d", base,
                            new_size);
                patchlib_free(bigger);
            }
        }
        patchlib_free(first);
    }
    if (patchlib_is_valid(arr)) patchlib_free(arr);
    patchlib_free(f);
}

void terraria_npc_manager_resize(void) {
    const size_t custom = tefstd_vector_size(&g_terraria_npc_registry);
    if (custom == 0)
        return;
    const int base = terraria_npc_id_get_count();
    if (base <= 0)
        return;
    const int new_size = base + (int) custom;

    /* 1) TextureAssets.Npc[] */
    resize_static_array(f_texture_npc, "TextureAssets.Npc", base, new_size,
                        false, 0, false);

    /* 2) Main.npcFrameCount[]（新槽位必须 >= 1，否则绘制时除以 0） */
    resize_static_array(f_npc_frame_count, "Main.npcFrameCount", base, new_size,
                        false, 1, true);

    /* 3) NPCID.Sets.* */
    int resized = resize_sets_arrays("Terraria.ID", "NPCID", "Sets", base,
                                     new_size, true);
    TEKLOG_INFO("npc resize: NPCID.Sets %d array(s) resized (base=%d -> %d)",
                resized, base, new_size);

    /* 4) Main 上按 NPC type 索引的数组 */
    patch_handle_t main_cls = patchlib_type_get_type("Terraria", "Main");
    if (patchlib_is_valid(main_cls)) {
        static const char *main_fields[] = {"townNPCCanSpawn", "slimeRainNPC",
                                            "npcCatchable"};
        for (size_t i = 0; i < sizeof(main_fields) / sizeof(main_fields[0]); ++i) {
            patch_handle_t f = patchlib_type_get_field(main_cls, main_fields[i]);
            if (!patchlib_is_valid(f))
                continue;
            resize_static_array(f, main_fields[i], base, new_size, false, 0,
                                false);
            patchlib_free(f);
        }

        /* NPC.ShimmeredTownNPCs / npcsFoundForCheckActive 是 NPC 类上的静态表 */
        patch_handle_t npc_cls = patchlib_type_get_type("Terraria", "NPC");
        if (patchlib_is_valid(npc_cls)) {
            static const char *npc_fields[] = {"ShimmeredTownNPCs",
                                               "npcsFoundForCheckActive"};
            for (size_t i = 0; i < sizeof(npc_fields) / sizeof(npc_fields[0]); ++i) {
                patch_handle_t f = patchlib_type_get_field(npc_cls, npc_fields[i]);
                if (!patchlib_is_valid(f))
                    continue;
                resize_static_array(f, npc_fields[i], base, new_size, false, 0,
                                    false);
                patchlib_free(f);
            }
            patchlib_free(npc_cls);
        }
        patchlib_free(main_cls);
    }

    /* 5) Player.npcTypeNoAggro（每个玩家一份） */
    resize_player_arrays(base, new_size);

    /* 6) Lang._npcNameCache */
    resize_npc_name_cache(base, new_size);
}

/* ------------------------------------------------------------------ */
/* 贴图                                                                */
/* ------------------------------------------------------------------ */

static patch_handle_t make_placeholder_texture(void) {
    static unsigned char px[16 * 16 * 4];
    for (int i = 0; i < 16 * 16; ++i) {
        px[i * 4 + 0] = 255;
        px[i * 4 + 1] = 0;
        px[i * 4 + 2] = 255;
        px[i * 4 + 3] = 255;
    }
    patch_handle_t tex = terraria_texture2d_create(16, 16, TEXTURE_FORMAT_RGBA32,
                                                   px, sizeof(px));
    if (!patchlib_is_valid(tex))
        return PATCH_NULL;
    patch_handle_t cls = terraria_texture2d_get_class();
    patch_handle_t asset = terraria_asset_create(cls, tex);
    if (patchlib_is_valid(cls)) patchlib_free(cls);
    if (patchlib_is_valid(tex)) patchlib_free(tex);
    return asset;
}

void terraria_npc_manager_init_texture2d(void) {
    const size_t custom = tefstd_vector_size(&g_terraria_npc_registry);
    if (custom == 0 || !patchlib_is_valid(f_texture_npc))
        return;

    patch_handle_t array = PATCH_NULL;
    patchlib_field_get_value(f_texture_npc, NULL, &array);
    if (!patchlib_is_valid(array))
        return;

    const size_t len = patchlib_array_length(array);
    patch_handle_t placeholder = PATCH_NULL;
    int done = 0;
    for (size_t i = 0; i < custom; ++i) {
        terraria_npc_handle_t **pp =
                tefstd_vector_at(&g_terraria_npc_registry, i);
        if (!pp || !*pp)
            continue;
        terraria_npc_handle_t *h = *pp;
        if (h->runtime_id < 0 || (size_t) h->runtime_id >= len)
            continue;

        patch_handle_t tex = PATCH_NULL;
        if (h->npc_ops.get_texture)
            tex = h->npc_ops.get_texture(h);
        if (!patchlib_is_valid(tex)) {
            if (!patchlib_is_valid(placeholder)) {
                placeholder = make_placeholder_texture();
                if (!patchlib_is_valid(placeholder))
                    break;
            }
            tex = placeholder;
        }
        patchlib_array_set(array, (size_t) h->runtime_id, &tex);

        if (patchlib_is_valid(f_npc_frame_count)) {
            patch_handle_t frames = PATCH_NULL;
            patchlib_field_get_value(f_npc_frame_count, NULL, &frames);
            if (patchlib_is_valid(frames)) {
                int n = h->frames > 0 ? h->frames : 1;
                patchlib_array_set(frames, (size_t) h->runtime_id, &n);
                patchlib_free(frames);
            }
        }
        done++;
    }
    patchlib_free(array);
    TEKLOG_INFO("npc textures: %d set", done);
}

/* ------------------------------------------------------------------ */
/* 钩子                                                                */
/* ------------------------------------------------------------------ */

static terraria_npc_handle_t *custom_from_instance(patch_handle_t inst) {
    if (!inst || !patchlib_is_valid(f_type))
        return NULL;
    int type = 0;
    patchlib_field_get_value(f_type, inst, &type);
    const int nid = type - terraria_npc_id_get_count();
    const size_t count = tefstd_vector_size(&g_terraria_npc_registry);
    if (nid < 0 || nid >= (int) count)
        return NULL;
    terraria_npc_handle_t **pp =
            tefstd_vector_at(&g_terraria_npc_registry, (size_t) nid);
    return (pp && *pp) ? *pp : NULL;
}

/* NPC.SetDefaults(int Type, NPCSpawnParams) 后置 */
static void set_defaults_postfix(patch_handle_t this, void **args, void *result,
                                 const patch_method_signature_t *sig_info) {
    (void) result;
    (void) sig_info;
    if (!this || !args || !args[0] || !patchlib_is_valid(f_type))
        return;

    const int type = *(int *) args[0];
    const int nid = type - terraria_npc_id_get_count();
    const size_t count = tefstd_vector_size(&g_terraria_npc_registry);
    if (nid < 0 || nid >= (int) count)
        return;

    terraria_npc_handle_t **pp =
            tefstd_vector_at(&g_terraria_npc_registry, (size_t) nid);
    if (!pp || !*pp)
        return;
    terraria_npc_handle_t *h = *pp;
    if (h->npc_ops.set_defaults)
        h->npc_ops.set_defaults(h, this);
}

/* NPC.AI() 前置：自定义 NPC 默认跳过原版 AI */
static bool ai_prefix(patch_handle_t this, void **args,
                      const patch_method_signature_t *sig_info, void *result) {
    (void) args;
    (void) sig_info;
    (void) result;
    terraria_npc_handle_t *h = custom_from_instance(this);
    if (!h || !h->npc_ops.ai)
        return false;
    if (h->ai_after_vanilla)
        return false;

    h->npc_ops.ai(h, this);
    return true;
}

static void ai_postfix(patch_handle_t this, void **args, void *result,
                       const patch_method_signature_t *sig_info) {
    (void) args;
    (void) result;
    (void) sig_info;
    terraria_npc_handle_t *h = custom_from_instance(this);
    if (!h || !h->npc_ops.ai || !h->ai_after_vanilla)
        return;
    h->npc_ops.ai(h, this);
}

/* ------------------------------------------------------------------ */
/* 生成                                                                */
/* ------------------------------------------------------------------ */

int terraria_npc_manager_spawn(const int runtime_id, const float x,
                               const float y) {
    if (runtime_id < 0)
        return -1;

    static patch_handle_t new_npc = PATCH_NULL;
    static patch_handle_t source_cls = PATCH_NULL;
    if (!patchlib_is_valid(new_npc)) {
        patch_handle_t cls = patchlib_type_get_type("Terraria", "NPC");
        if (patchlib_is_valid(cls)) {
            new_npc = patchlib_type_get_method_by_param_count(cls, "NewNPC", 10);
            patchlib_free(cls);
        }
        if (!patchlib_is_valid(new_npc)) {
            TEKLOG_ERROR("npc spawn: NPC.NewNPC(10) not found");
            return -1;
        }
    }
    if (!patchlib_is_valid(source_cls)) {
        source_cls = patchlib_type_get_type("Terraria.DataStructures",
                                            "EntitySource_SpawnNPC");
        if (!patchlib_is_valid(source_cls)) {
            TEKLOG_ERROR("npc spawn: EntitySource_SpawnNPC not found");
            return -1;
        }
    }

    patch_handle_t source = patchlib_type_new_instance(source_cls);
    int xi = (int) x;
    int yi = (int) y;
    int type = runtime_id;
    int start = 0;
    float ai0 = 0.0f, ai1 = 0.0f, ai2 = 0.0f, ai3 = 0.0f;
    int target = 255;
    void *args[10] = {&source, &xi, &yi, &type, &start,
                      &ai0, &ai1, &ai2, &ai3, &target};
    int result = -1;
    const bool ok = patchlib_method_invoke_args(new_npc, PATCH_NULL, &result, args);
    if (patchlib_is_valid(source)) patchlib_free(source);
    if (!ok)
        return -1;
    return result;
}

/* ------------------------------------------------------------------ */
/* 生命周期                                                            */
/* ------------------------------------------------------------------ */

void terraria_npc_manager_init(void) {
    tefstd_vector_init(&g_terraria_npc_registry, sizeof(terraria_npc_handle_t *));

    npc_class = patchlib_type_get_type("Terraria", "NPC");
    if (!patchlib_is_valid(npc_class)) {
        TEKLOG_ERROR("npc: Terraria.NPC not found");
        return;
    }

    f_type = patchlib_type_get_field(npc_class, "type");

    patch_handle_t id_cls = patchlib_type_get_type("Terraria.ID", "NPCID");
    if (patchlib_is_valid(id_cls)) {
        npc_id_count = patchlib_type_get_field(id_cls, "Count");
        patchlib_free(id_cls);
    }

    patch_handle_t tex_cls =
            patchlib_type_get_type("Terraria.GameContent", "TextureAssets");
    if (patchlib_is_valid(tex_cls)) {
        f_texture_npc = patchlib_type_get_field(tex_cls, "Npc");
        patchlib_free(tex_cls);
    }

    patch_handle_t main_cls = patchlib_type_get_type("Terraria", "Main");
    if (patchlib_is_valid(main_cls)) {
        f_npc_frame_count = patchlib_type_get_field(main_cls, "npcFrameCount");
        patchlib_free(main_cls);
    }

    patch_handle_t set_defaults = patchlib_type_get_method_by_param_count(
            npc_class, "SetDefaults", 2);
    if (patchlib_is_valid(set_defaults)) {
        h_set_defaults = patchlib_install_prepost_hook(set_defaults, NULL,
                                                       set_defaults_postfix);
        TEKLOG_INFO("npc: hooked NPC.SetDefaults (%d)", (int) h_set_defaults);
    } else {
        TEKLOG_ERROR("npc: NPC.SetDefaults(int,NPCSpawnParams) not found");
    }

    patch_handle_t ai = patchlib_type_get_method_by_param_count(npc_class, "AI", 0);
    if (patchlib_is_valid(ai)) {
        h_ai = patchlib_install_prepost_hook(ai, ai_prefix, ai_postfix);
        TEKLOG_INFO("npc: hooked NPC.AI (%d)", (int) h_ai);
    }

    TEKLOG_INFO("npc: init done (base=%d, custom=%zu)",
                terraria_npc_id_get_count(),
                tefstd_vector_size(&g_terraria_npc_registry));
}

void terraria_npc_manager_destroy(void) {
    if (h_set_defaults != PATCH_HOOK_INVALID_ID) {
        patchlib_uninstall_hook(h_set_defaults);
        h_set_defaults = PATCH_HOOK_INVALID_ID;
    }
    if (h_ai != PATCH_HOOK_INVALID_ID) {
        patchlib_uninstall_hook(h_ai);
        h_ai = PATCH_HOOK_INVALID_ID;
    }
    tefstd_vector_destroy(&g_terraria_npc_registry);
}
