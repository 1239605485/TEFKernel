/*******************************************************************************
 * tefkernel - projectile_manager
 *
 * 自定义弹幕（Projectile）：注册 / ID 分配 / 贴图 / SetDefaults / AI 分发。
 * 结构与 item_manager 同构。
 *
 * 设计要点：
 *   · ID：base = ProjectileID.Count（原版数量），runtime_id = base + 注册表下标；
 *     注册表按 (loader, mod, name) 排序后分配，保证客户端/服务端一致。
 *   · 扩容：TextureAssets.Projectile[] 与 ProjectileID.Sets.*（按 type 索引的静态数组）
 *     必须一起扩容，否则绘制/逻辑越界。
 *   · SetDefaults：postfix，给自定义类型的实例套用 Mod 默认值。
 *   · AI：**prefix**（void 方法，桌面加载器不会读返回缓冲），只对自定义类型接管，
 *     返回 true 跳过原版 AI；其余类型立即返回 false 走原版。
 *******************************************************************************/

#include "internal/terraria/projectile_manager.h"

#include "internal/log.h"
#include "patchlib/field.h"
#include "patchlib/method.h"
#include "patchlib/struct/array.h"
#include "patchlib/type.h"
#include "terraria/asset.h"
#include "terraria/texture2d.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

tefstd_vector_t g_terraria_projectile_registry;

static patch_handle_t projectile_class = PATCH_NULL;
static patch_handle_t f_type = PATCH_NULL;       // Projectile.type (int)
static patch_handle_t f_active = PATCH_NULL;     // Projectile.active (bool)

static patch_handle_t projectile_id_count = PATCH_NULL;   // ProjectileID.Count (short)
static patch_handle_t f_texture_projectile = PATCH_NULL;  // TextureAssets.Projectile

static patch_hook_id_t h_set_defaults = PATCH_HOOK_INVALID_ID;
static patch_hook_id_t h_ai = PATCH_HOOK_INVALID_ID;
static bool g_ids_assigned = false;

/* Player.UpdateProjectileCaches 会做 ownedProjectileCounts[projectile.type]++，
 * 而游戏进入世界时会替换本地玩家实例（拿到的是原版长度的数组）。
 * 生成自定义弹幕时顺手把玩家数组补齐，避免每帧越界（异常会被
 * Main.cs 的 player[i].Update(i) try/catch 吞掉，表现就是"玩家被定格"）。 */
static patch_handle_t f_main_players = PATCH_NULL;  // Main.player (Player[])
static patch_handle_t f_owned_counts = PATCH_NULL;  // Player.ownedProjectileCounts (int[])
static patch_handle_t f_proj_frames = PATCH_NULL;   // Main.projFrames (int[])

static void ensure_player_owned_counts(const int new_size) {
    if (!patchlib_is_valid(f_main_players) || !patchlib_is_valid(f_owned_counts))
        return;
    patch_handle_t players = PATCH_NULL;
    patchlib_field_get_value(f_main_players, NULL, &players);
    if (!patchlib_is_valid(players))
        return;

    const size_t n = patchlib_array_length(players);
    int patched = 0;
    for (size_t i = 0; i < n; ++i) {
        patch_handle_t player = PATCH_NULL;
        if (!patchlib_array_at(players, i, &player) || !patchlib_is_valid(player))
            continue;

        patch_handle_t counts = PATCH_NULL;
        patchlib_field_get_value(f_owned_counts, player, &counts);
        if (patchlib_is_valid(counts) &&
            patchlib_array_length(counts) < (size_t) new_size) {
            patch_handle_t bigger =
                    patchlib_array_resize(counts, (size_t) new_size, NULL);
            if (patchlib_is_valid(bigger)) {
                patchlib_field_set_value(f_owned_counts, player, &bigger);
                patched++;
                patchlib_free(bigger);
            } else {
                TEKLOG_ERROR("projectile: ownedProjectileCounts resize 失败 (player %zu)", i);
            }
        }
        if (patchlib_is_valid(counts))
            patchlib_free(counts);
        patchlib_free(player);
    }
    patchlib_free(players);

    if (patched > 0)
        TEKLOG_INFO("projectile: ownedProjectileCounts 补齐 %d 个玩家 -> %d",
                    patched, new_size);
}

/* ------------------------------------------------------------------ */
/* 注册表                                                              */
/* ------------------------------------------------------------------ */

static int compare_projectile(const void *a, const void *b) {
    const terraria_projectile_handle_t *pa =
            *(const terraria_projectile_handle_t **) a;
    const terraria_projectile_handle_t *pb =
            *(const terraria_projectile_handle_t **) b;

    int cmp = strcmp(pa->parent_modloader_id, pb->parent_modloader_id);
    if (cmp != 0) return cmp;
    cmp = strcmp(pa->parent_id, pb->parent_id);
    if (cmp != 0) return cmp;
    return strcmp(pa->internal_name, pb->internal_name);
}

bool terraria_projectile_manager_register_projectile(
        terraria_projectile_handle_t *projectile_handle) {
    if (!projectile_handle || !projectile_handle->parent_modloader_id ||
        !projectile_handle->parent_id || !projectile_handle->internal_name)
        return false;

    const size_t count = tefstd_vector_size(&g_terraria_projectile_registry);
    for (size_t i = 0; i < count; ++i) {
        terraria_projectile_handle_t **existing =
                tefstd_vector_at(&g_terraria_projectile_registry, i);
        if (!existing || !*existing)
            continue;
        if (strcmp((*existing)->parent_modloader_id,
                   projectile_handle->parent_modloader_id) == 0 &&
            strcmp((*existing)->parent_id, projectile_handle->parent_id) == 0 &&
            strcmp((*existing)->internal_name,
                   projectile_handle->internal_name) == 0) {
            /* 热重载：替换句柄，保留 runtime_id */
            projectile_handle->runtime_id = (*existing)->runtime_id;
            *existing = projectile_handle;
            TEKLOG_INFO("projectile re-registered (hot reload): %s.%s id=%d",
                        projectile_handle->parent_id,
                        projectile_handle->internal_name,
                        projectile_handle->runtime_id);
            return true;
        }
    }

    if (g_ids_assigned) {
        TEKLOG_WARN("projectile registered after IDs assigned: %s.%s "
                    "(runtime_id stays -1 until re-init)",
                    projectile_handle->parent_id,
                    projectile_handle->internal_name);
    }

    return tefstd_vector_push_back(&g_terraria_projectile_registry,
                                   &projectile_handle);
}

bool terraria_projectile_manager_unregister_projectile_by_id(
        const int runtime_id) {
    if (runtime_id < 0)
        return false;
    const size_t count = tefstd_vector_size(&g_terraria_projectile_registry);
    for (size_t i = 0; i < count; ++i) {
        terraria_projectile_handle_t **pp =
                tefstd_vector_at(&g_terraria_projectile_registry, i);
        if (!pp || !*pp)
            continue;
        if ((*pp)->runtime_id == runtime_id) {
            const bool removed =
                    tefstd_vector_erase(&g_terraria_projectile_registry, i, NULL);
            if (removed)
                terraria_projectile_manager_assign_ids(
                        terraria_projectile_id_get_count());
            return removed;
        }
    }
    return false;
}

bool terraria_projectile_manager_assign_ids(const int base_id) {
    const size_t count = tefstd_vector_size(&g_terraria_projectile_registry);
    if (count == 0) {
        g_ids_assigned = true;
        return true;
    }
    terraria_projectile_handle_t **items = g_terraria_projectile_registry.data;
    qsort(items, count, sizeof(terraria_projectile_handle_t *),
          compare_projectile);
    for (size_t i = 0; i < count; ++i)
        items[i]->runtime_id = base_id + (int) i;
    g_ids_assigned = true;
    return true;
}

int terraria_projectile_id_get_count() {
    short count = 0;
    if (patchlib_is_valid(projectile_id_count))
        patchlib_field_get_value(projectile_id_count, NULL, &count);
    return count;
}

terraria_projectile_handle_t *terraria_projectile_manager_get_projectile(
        const int runtime_id) {
    const int base = terraria_projectile_id_get_count();
    const int nid = runtime_id - base;
    const size_t count = tefstd_vector_size(&g_terraria_projectile_registry);
    if (nid < 0 || nid >= (int) count)
        return NULL;
    terraria_projectile_handle_t **pp =
            tefstd_vector_at(&g_terraria_projectile_registry, (size_t) nid);
    return pp ? *pp : NULL;
}

tefstd_vector_t *terraria_projectile_manager_get_projectiles() {
    return &g_terraria_projectile_registry;
}

terraria_projectile_handle_t *terraria_projectile_manager_find_projectile(
        const char *loader, const char *mod, const char *name) {
    if (!loader || !mod || !name)
        return NULL;
    const size_t count = tefstd_vector_size(&g_terraria_projectile_registry);
    for (size_t i = 0; i < count; ++i) {
        terraria_projectile_handle_t **pp =
                tefstd_vector_at(&g_terraria_projectile_registry, i);
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
/* 扩容                                                                */
/* ------------------------------------------------------------------ */

/* 用原数组里出现次数最多的元素填充新槽位。
 * ProjectileID.Sets 里不少集合有非 0 默认值（TrailCacheLength=10、
 * TrailWidth、IDStaticNPCHitCooldown 等），array_resize 只会清零。 */
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

/* 把某个"按 ProjectileID 索引"的静态数组扩到 new_size（长度必须等于 base 才动） */
static void resize_static_array(patch_handle_t field, const char *name,
                                const int base, const int new_size) {
    if (!patchlib_is_valid(field)) {
        TEKLOG_WARN("projectile resize: field %s not found", name ? name : "?");
        return;
    }
    patch_handle_t array = PATCH_NULL;
    patchlib_field_get_value(field, NULL, &array);
    if (!patchlib_is_valid(array)) {
        TEKLOG_DEBUG("projectile resize: %s is null, skip", name ? name : "?");
        return;
    }
    const size_t len = patchlib_array_length(array);
    if (len != (size_t) base) {
        TEKLOG_DEBUG("projectile resize: %s len=%zu != base=%d, skip",
                     name ? name : "?", len, base);
        patchlib_free(array);
        return;
    }
    patch_handle_t bigger = patchlib_array_resize(array, (size_t) new_size, NULL);
    if (patchlib_is_valid(bigger)) {
        fill_new_slots_with_modal(array, bigger, len, (size_t) new_size);
        patchlib_field_set_value(field, NULL, &bigger);
        TEKLOG_INFO("projectile resize: %s %zu -> %d", name ? name : "?", len,
                    new_size);
        patchlib_free(bigger);
    } else {
        TEKLOG_ERROR("projectile resize: %s resize failed", name ? name : "?");
    }
    patchlib_free(array);
}

void terraria_projectile_manager_resize() {
    const size_t custom = tefstd_vector_size(&g_terraria_projectile_registry);
    if (custom == 0)
        return;
    const int base = terraria_projectile_id_get_count();
    if (base <= 0)
        return;
    const int new_size = base + (int) custom;

    /* 1) TextureAssets.Projectile[] */
    resize_static_array(f_texture_projectile, "TextureAssets.Projectile", base,
                        new_size);

    /* 2) ProjectileID.Sets.* */
    patch_handle_t id_cls = patchlib_type_get_type("Terraria.ID", "ProjectileID");
    if (!patchlib_is_valid(id_cls)) {
        TEKLOG_ERROR("projectile resize: ProjectileID not found");
        return;
    }
    patch_handle_t sets = patchlib_type_get_inner_type(id_cls, "Sets");
    patchlib_free(id_cls);
    if (!patchlib_is_valid(sets)) {
        TEKLOG_ERROR("projectile resize: ProjectileID+Sets not found");
        return;
    }

    tefstd_vector_t fields;
    tefstd_vector_init(&fields, sizeof(patch_handle_t));
    patchlib_type_get_fields(sets, false, &fields);
    int resized = 0;
    for (size_t i = 0; i < fields.size; ++i) {
        patch_handle_t field = *(patch_handle_t *) tefstd_vector_at(&fields, i);
        if (!patchlib_is_valid(field)) {
            continue;
        }
        const char *name = patchlib_field_get_name(field);

        // ProjectileID.Sets 里有 public const int（如 FixedDamageForRockets = 60）
        // 这种"字段"不是数组；直接当句柄用会读 60+偏移，fault 0x8c。
        if (!patchlib_field_is_static(field) || patchlib_field_is_const(field)) {
            TEKLOG_DEBUG("projectile resize: skip const/non-static %s",
                         name ? name : "?");
            patchlib_free(field);
            continue;
        }

        if (name && (strcmp(name, "Factory") == 0 ||
                     strcmp(name, "Count") == 0)) {
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
                TEKLOG_ERROR("projectile resize: %s (%zu -> %d) 失败",
                             name ? name : "?", len, new_size);
            }
        } else {
            TEKLOG_DEBUG("projectile resize: %s len=%zu != base=%d，跳过",
                         name ? name : "?", len, base);
        }
        patchlib_free(array);
        patchlib_free(field);
    }
    tefstd_vector_destroy(&fields);
    patchlib_free(sets);
    TEKLOG_INFO("projectile resize: %d set array(s) resized (base=%d -> %d)",
                resized, base, new_size);

    patch_handle_t main_cls = patchlib_type_get_type("Terraria", "Main");
    if (!patchlib_is_valid(main_cls)) {
        TEKLOG_ERROR("projectile resize: Main not found");
        return;
    }

    /* 3) Main 上按弹幕 type 索引的静态表
     *    Projectile.Draw 会直接读 Main.projPet[type] / Main.projFrames[type]，
     *    Item.CheckItemUse 会读 Main.projHook[shoot]，不扩容会越界。 */
    static const char *main_fields[] = {
            "projFrames", "projPet", "projHook", "projHostile"};
    for (size_t i = 0; i < sizeof(main_fields) / sizeof(main_fields[0]); ++i) {
        patch_handle_t field = patchlib_type_get_field(main_cls, main_fields[i]);
        if (!patchlib_is_valid(field))
            continue;
        if (i == 0 && !patchlib_is_valid(f_proj_frames))
            f_proj_frames = field;   /* 保留句柄给帧图写入用 */
        resize_static_array(field, main_fields[i], base, new_size);
        if (!(i == 0 && patchlib_is_valid(f_proj_frames) && f_proj_frames == field))
            patchlib_free(field);
    }

    /* 4) Player.ownedProjectileCounts（Player.Update 里按 projectile.type 自增） */
    patch_handle_t player_cls = patchlib_type_get_type("Terraria", "Player");
    patch_handle_t f_owned = PATCH_NULL;
    patch_handle_t f_players = PATCH_NULL;
    if (patchlib_is_valid(player_cls))
        f_owned = patchlib_type_get_field(player_cls, "ownedProjectileCounts");
    f_players = patchlib_type_get_field(main_cls, "player");

    if (patchlib_is_valid(f_owned) && patchlib_is_valid(f_players)) {
        patch_handle_t players = PATCH_NULL;
        patchlib_field_get_value(f_players, NULL, &players);
        if (patchlib_is_valid(players)) {
            const size_t n = patchlib_array_length(players);
            int patched = 0;
            for (size_t i = 0; i < n; ++i) {
                patch_handle_t player = PATCH_NULL;
                if (!patchlib_array_at(players, i, &player) || !player)
                    continue;
                patch_handle_t counts = PATCH_NULL;
                patchlib_field_get_value(f_owned, player, &counts);
                if (counts &&
                    patchlib_array_length(counts) == (size_t) base) {
                    patch_handle_t bigger = patchlib_array_resize(
                            counts, (size_t) new_size, NULL);
                    if (patchlib_is_valid(bigger)) {
                        patchlib_field_set_value(f_owned, player, &bigger);
                        patched++;
                    }
                }
                if (patchlib_is_valid(counts))
                    patchlib_free(counts);
                patchlib_free(player);
            }
            patchlib_free(players);
            TEKLOG_INFO("projectile resize: ownedProjectileCounts %d/%zu players",
                        patched, n);
        }
    } else {
        TEKLOG_WARN("projectile resize: ownedProjectileCounts handles unavailable");
    }
    if (patchlib_is_valid(f_owned))
        patchlib_free(f_owned);
    if (patchlib_is_valid(f_players))
        patchlib_free(f_players);
    if (patchlib_is_valid(player_cls))
        patchlib_free(player_cls);
    patchlib_free(main_cls);
}

/* ------------------------------------------------------------------ */
/* 贴图                                                                */
/* ------------------------------------------------------------------ */

static patch_handle_t make_placeholder_texture() {
    const int SIZE = 32;
    const size_t DATA = (size_t) SIZE * SIZE * 4;
    uint8_t *px = malloc(DATA);
    if (!px)
        return PATCH_NULL;
    for (int y = 0; y < SIZE; ++y) {
        for (int x = 0; x < SIZE; ++x) {
            uint8_t *p = px + ((size_t) y * SIZE + x) * 4;
            /* 紫色方块 + 简单描边，避免"什么都不画" */
            const bool border = (x < 2 || y < 2 || x >= SIZE - 2 || y >= SIZE - 2);
            p[0] = border ? 40 : 190;
            p[1] = border ? 20 : 90;
            p[2] = border ? 60 : 230;
            p[3] = 255;
        }
    }
    patch_handle_t tex = terraria_texture2d_create(
            SIZE, SIZE, TEXTURE_FORMAT_RGBA32, px, DATA);
    free(px);
    if (!patchlib_is_valid(tex))
        return PATCH_NULL;

    /* TextureAssets.Projectile[] 的元素是 Asset<Texture2D>，必须包一层，
     * 否则游戏读 .Value 时拿到的是 Texture2D 对象 → 画不出来（或崩）。 */
    patch_handle_t tex_type = terraria_texture2d_get_class();
    patch_handle_t asset = terraria_asset_create(tex_type, tex);
    if (patchlib_is_valid(tex_type))
        patchlib_free(tex_type);
    if (patchlib_is_valid(tex))
        patchlib_free(tex);
    return asset;
}

void terraria_projectile_manager_init_texture2d() {
    const size_t custom = tefstd_vector_size(&g_terraria_projectile_registry);
    if (custom == 0 || !patchlib_is_valid(f_texture_projectile))
        return;

    patch_handle_t array = PATCH_NULL;
    patchlib_field_get_value(f_texture_projectile, NULL, &array);
    if (!patchlib_is_valid(array))
        return;
    const size_t len = patchlib_array_length(array);
    const int base = terraria_projectile_id_get_count();

    patch_handle_t placeholder = PATCH_NULL;
    int done = 0;
    for (size_t i = 0; i < custom; ++i) {
        terraria_projectile_handle_t **pp =
                tefstd_vector_at(&g_terraria_projectile_registry, i);
        if (!pp || !*pp)
            continue;
        terraria_projectile_handle_t *h = *pp;
        if (h->runtime_id < 0 || (size_t) h->runtime_id >= len)
            continue;

        patch_handle_t tex = PATCH_NULL;
        if (h->projectile_ops.get_texture)
            tex = h->projectile_ops.get_texture(h);
        if (!patchlib_is_valid(tex)) {
            if (!patchlib_is_valid(placeholder)) {
                placeholder = make_placeholder_texture();
                if (!patchlib_is_valid(placeholder))
                    break;
            }
            tex = placeholder;
        }
        patchlib_array_set(array, (size_t) h->runtime_id, &tex);
        /* tex 写进引擎数组后不要再 free（桌面 GCHandle 语义） */
        done++;
    }
    patchlib_free(array);
    TEKLOG_INFO("projectile textures: %d set (base=%d)", done, base);
}

/* ------------------------------------------------------------------ */
/* 钩子                                                                */
/* ------------------------------------------------------------------ */

/**
 * 自定义弹幕的 ai / localAI 数组在部分平台（对象池/未初始化分配）上可能是空的。
 * 原版 AI 一进来就会读写 ai[]，空指针会直接抛异常，所以必须在 SetDefaults
 * 阶段就把数组补出来（与具体扩展无关，属于内核职责）。
 */
static void ensure_projectile_ai_arrays(patch_handle_t inst) {
    static patch_handle_t f_ai = PATCH_NULL;
    static patch_handle_t f_local_ai = PATCH_NULL;
    static patch_handle_t f_max_ai = PATCH_NULL;
    static bool resolved = false;

    if (!resolved) {
        resolved = true;
        patch_handle_t cls = patchlib_type_get_type("Terraria", "Projectile");
        if (patchlib_is_valid(cls)) {
            f_ai = patchlib_type_get_field(cls, "ai");
            f_local_ai = patchlib_type_get_field(cls, "localAI");
            f_max_ai = patchlib_type_get_field(cls, "maxAI");
            patchlib_free(cls);
        }
    }

    if (!patchlib_is_valid(f_ai) || !patchlib_is_valid(f_local_ai))
        return;

    /* PE 的 ai / localAI 是 12 字节内联结构（Float_FixedArray_3），
     * 不是 float[]；往里写托管数组指针会破坏 ai[0]/ai[1]。 */
    if (patchlib_field_get_type(f_ai) != PATCH_OBJECT)
        return;

    int n = 3;
    if (patchlib_is_valid(f_max_ai))
        patchlib_field_get_value(f_max_ai, NULL, &n);
    if (n < 1) n = 3;

    patch_handle_t float_type = patchlib_get_basic_type(PATCH_FLOAT);
    if (!patchlib_is_valid(float_type))
        return;

    patch_handle_t arr = PATCH_NULL;
    patchlib_field_get_value(f_ai, inst, &arr);
    if (!patchlib_is_valid(arr)) {
        arr = patchlib_array_create((size_t) n, float_type);
        if (patchlib_is_valid(arr))
            patchlib_field_set_value(f_ai, inst, &arr);
    }

    arr = PATCH_NULL;
    patchlib_field_get_value(f_local_ai, inst, &arr);
    if (!patchlib_is_valid(arr)) {
        arr = patchlib_array_create((size_t) n, float_type);
        if (patchlib_is_valid(arr))
            patchlib_field_set_value(f_local_ai, inst, &arr);
    }
}

static void set_defaults_postfix(patch_handle_t this, void **args, void *result,
                                 const patch_method_signature_t *sig_info) {
    (void) result;
    (void) sig_info;
    if (!this || !args || !args[0] || !patchlib_is_valid(f_type))
        return;
    const int type = *(int *) args[0];
    const int base = terraria_projectile_id_get_count();
    const int nid = type - base;
    const size_t count = tefstd_vector_size(&g_terraria_projectile_registry);
    if (nid < 0 || nid >= (int) count)
        return;

    /* SetDefaults 内部可能清空/改写 type，强制恢复 */
    int current_type = 0;
    patchlib_field_get_value(f_type, this, &current_type);
    if (current_type != type)
        TEKLOG_WARN("projectile SetDefaults 改写了 type：%d -> %d，已恢复",
                    type, current_type);
    patchlib_field_set_value(f_type, this, args[0]);
    TEKLOG_INFO("projectile set_defaults_postfix: custom nid=%d, applying defaults",
                nid);

    /* 原版 SetDefaults 末尾的 else 分支会把未知类型置为 active=false，
     * 自定义弹幕必须在这里重新激活，否则创建后立即失活（不更新、不绘制）。 */
    const bool active = true;
    patchlib_field_set_value(f_active, this, (void *) &active);

    /* ai / localAI 若为空，原版 AI 一进来就会空指针，这里提前补好 */
    ensure_projectile_ai_arrays(this);

    /* 帧图：声明了帧数就把 Main.projFrames[type] 写上（0 = 单帧整图不写）。
     * projFrames 是 int[] 静态数组：先取数组对象，再写第 type 个元素。 */
    {
        terraria_projectile_handle_t **fp =
                tefstd_vector_at(&g_terraria_projectile_registry, (size_t) nid);
        if (fp && *fp && (*fp)->frames > 0 && patchlib_is_valid(f_proj_frames)) {
            patch_handle_t arr = PATCH_NULL;
            patchlib_field_get_value(f_proj_frames, NULL, &arr);
            if (patchlib_is_valid(arr) &&
                (size_t) type < patchlib_array_length(arr)) {
                int frames = (*fp)->frames;
                patchlib_array_set(arr, (size_t) type, &frames);
            }
        }
    }

    /* 玩家侧缓存：ownedProjectileCounts 会被 Player.UpdateProjectileCaches
     * 按弹幕 type 索引，进入世界后玩家实例可能已换，这里按需补齐。 */
    ensure_player_owned_counts(base + (int) count);

    terraria_projectile_handle_t **pp =
            tefstd_vector_at(&g_terraria_projectile_registry, (size_t) nid);
    if (pp && *pp && (*pp)->projectile_ops.set_defaults)
        (*pp)->projectile_ops.set_defaults(*pp, this);

}

static bool ai_prefix(patch_handle_t this, void **args,
                      const patch_method_signature_t *sig_info, void *result) {
    (void) args;
    (void) sig_info;
    (void) result;
    if (!this || !patchlib_is_valid(f_type))
        return false;

    int type = 0;
    patchlib_field_get_value(f_type, this, &type);
    const int base = terraria_projectile_id_get_count();
    const int nid = type - base;
    const size_t count = tefstd_vector_size(&g_terraria_projectile_registry);

    if (nid < 0 || nid >= (int) count)
        return false; /* 原版弹幕：走原版 AI */

    terraria_projectile_handle_t **pp =
            tefstd_vector_at(&g_terraria_projectile_registry, (size_t) nid);
    if (!pp || !*pp)
        return false;
    terraria_projectile_handle_t *h = *pp;
    if (h->no_ai_hook || !h->projectile_ops.ai)
        return false; /* 沿用原版 aiStyle */

    if (h->ai_after_vanilla)
        return false; /* 先让原版 AI 跑完，ai_postfix 再调用扩展回调 */

    h->projectile_ops.ai(h, this);
    return true; /* 跳过原版 AI */
}

/**
 * 后置 AI 钩子：只处理 ai_after_vanilla 模式。
 * 原版 Projectile.AI()（aiStyle）已经执行完毕，这里再调扩展注册的回调，
 * 扩展只需补充 vanilla 没有的行为（拾取、Buff、额外重力等）。
 */
static void ai_postfix(patch_handle_t this, void **args, void *result,
                       const patch_method_signature_t *sig_info) {
    (void) args;
    (void) result;
    (void) sig_info;
    if (!this || !patchlib_is_valid(f_type))
        return;

    int type = 0;
    patchlib_field_get_value(f_type, this, &type);
    const int base = terraria_projectile_id_get_count();
    const int nid = type - base;
    const size_t count = tefstd_vector_size(&g_terraria_projectile_registry);

    if (nid < 0 || nid >= (int) count)
        return;

    terraria_projectile_handle_t **pp =
            tefstd_vector_at(&g_terraria_projectile_registry, (size_t) nid);
    if (!pp || !*pp)
        return;
    terraria_projectile_handle_t *h = *pp;
    if (h->no_ai_hook || !h->ai_after_vanilla || !h->projectile_ops.ai)
        return;

    h->projectile_ops.ai(h, this);
}

/* ------------------------------------------------------------------ */
/* 生命周期                                                            */
/* ------------------------------------------------------------------ */

void terraria_projectile_manager_init() {
    tefstd_vector_init(&g_terraria_projectile_registry,
                       sizeof(terraria_projectile_handle_t *));

    projectile_class = patchlib_type_get_type("Terraria", "Projectile");
    if (!patchlib_is_valid(projectile_class)) {
        TEKLOG_ERROR("projectile: Terraria.Projectile not found");
        return;
    }

    f_type = patchlib_type_get_field(projectile_class, "type");
    f_active = patchlib_type_get_field(projectile_class, "active");

    /* 玩家侧缓存字段（见 ensure_player_owned_counts） */
    patch_handle_t main_cls = patchlib_type_get_type("Terraria", "Main");
    if (patchlib_is_valid(main_cls)) {
        f_main_players = patchlib_type_get_field(main_cls, "player");
        patchlib_free(main_cls);
    }
    patch_handle_t player_cls = patchlib_type_get_type("Terraria", "Player");
    if (patchlib_is_valid(player_cls)) {
        f_owned_counts =
                patchlib_type_get_field(player_cls, "ownedProjectileCounts");
        patchlib_free(player_cls);
    }

    patch_handle_t id_cls = patchlib_type_get_type("Terraria.ID", "ProjectileID");
    if (patchlib_is_valid(id_cls)) {
        projectile_id_count = patchlib_type_get_field(id_cls, "Count");
        patchlib_free(id_cls);
    }

    patch_handle_t tex_cls =
            patchlib_type_get_type("Terraria.GameContent", "TextureAssets");
    if (patchlib_is_valid(tex_cls)) {
        f_texture_projectile = patchlib_type_get_field(tex_cls, "Projectile");
        patchlib_free(tex_cls);
    }

    patch_handle_t set_defaults =
            patchlib_type_get_method_by_param_count(projectile_class,
                                                    "SetDefaults", 1);
    if (patchlib_is_valid(set_defaults)) {
        h_set_defaults =
                patchlib_install_prepost_hook(set_defaults, NULL,
                                              set_defaults_postfix);
        TEKLOG_INFO("projectile: hooked Projectile.SetDefaults (%d)",
                    (int) h_set_defaults);
    }

    /* AI() 是 void 实例方法：prefix 安全（桌面加载器不会读返回缓冲） */
    patch_handle_t ai = patchlib_type_get_method_by_param_count(
            projectile_class, "AI", 0);
    if (patchlib_is_valid(ai)) {
        h_ai = patchlib_install_prepost_hook(ai, ai_prefix, ai_postfix);
        TEKLOG_INFO("projectile: hooked Projectile.AI (%d)", (int) h_ai);
    }

    TEKLOG_INFO("projectile: init done (base=%d, custom=%zu)",
                terraria_projectile_id_get_count(),
                tefstd_vector_size(&g_terraria_projectile_registry));
}

void terraria_projectile_manager_destroy() {
    if (h_set_defaults != PATCH_HOOK_INVALID_ID) {
        patchlib_uninstall_hook(h_set_defaults);
        h_set_defaults = PATCH_HOOK_INVALID_ID;
    }
    if (h_ai != PATCH_HOOK_INVALID_ID) {
        patchlib_uninstall_hook(h_ai);
        h_ai = PATCH_HOOK_INVALID_ID;
    }
    tefstd_vector_destroy(&g_terraria_projectile_registry);
}
