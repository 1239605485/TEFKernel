/*******************************************************************************
 * tefkernel - buff_manager
 *
 * 自定义 Buff（不占用原版 BuffID）
 *
 * 为什么不用原版 buffType：
 *   手机端原生 Buff 栏（GUIBuffs）会把 buffType 里 >= BuffID.Count 的项过滤掉，
 *   而且原版 AddBuff/UpdateBuffs 对未知 type 有一堆按 type 索引的表与清理逻辑
 *   （IsFedState、MountType、buffImmune...），既容易越界也会误删。
 *
 * 现在改成：
 *   · runtime_id 只是本注册表里的稳定句柄，不写进 buffType；
 *   · 内核自己维护「玩家 -> 活跃 buff -> 剩余帧数」，每帧推进并回调 update；
 *   · 图标与倒计时由内核 GUI 覆盖层绘制；
 *   · 不扩容任何原版数组，不挂钩 AddBuff / FindBuffIndex / DelBuff。
 *******************************************************************************/

#include "internal/terraria/buff_manager.h"

#include <stdbool.h>
#include <stdint.h>
#include "patchlib/il2cpp_api.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "internal/log.h"
#include "patchlib/field.h"
#include "patchlib/method.h"
#include "patchlib/property.h"
#include "patchlib/struct/array.h"
#include "patchlib/type.h"
#include "terraria/gui.h"
#include "terraria/texture2d.h"

#define BUFF_ACTIVE_MAX 32

tefstd_vector_t g_terraria_buff_registry;

static patch_handle_t f_player_buff_type = PATCH_NULL;
static patch_handle_t f_main_player = PATCH_NULL;
static patch_handle_t m_get_my_player = PATCH_NULL;
static patch_hook_id_t h_player_update = PATCH_HOOK_INVALID_ID;
static patch_hook_id_t h_buff_bar = PATCH_HOOK_INVALID_ID;
static patch_handle_t f_sprite_batch = PATCH_NULL;
static patch_handle_t f_main_my_player = PATCH_NULL;  /* PC 是 Main.myPlayer 字段 */
static patch_handle_t f_player_who_am_i = PATCH_NULL; /* Player.whoAmI */
/* 原版 Buff 栏布局：Buffs_Layout.ItemScale / NormalColour */
static patch_handle_t m_layout_instance = PATCH_NULL;
static patch_handle_t f_layout_item_scale = PATCH_NULL;
static patch_handle_t f_layout_normal_colour = PATCH_NULL;
static patch_handle_t f_layout_text_scale = PATCH_NULL;
static patch_handle_t f_layout_text_offset = PATCH_NULL;
static bool g_ids_assigned = false;
static int g_base_count = -1;

typedef struct {
    patch_handle_t player;   // 玩家对象（不持有；桌面版每次取值是新的 GCHandle，不能用于比较）
    int who;                 // whoAmI：跨平台稳定的玩家身份
    int buff_id;             // runtime_id
    int time;                // 剩余帧数
} active_buff_t;

/* 桌面版每次从这里拿到的对象句柄都不同，所以一律用 whoAmI 判断“同一个玩家”。 */
static int player_who(patch_handle_t player) {
    if (player == PATCH_NULL || f_player_who_am_i == PATCH_NULL) return -1;
    unsigned char buf[64];
    memset(buf, 0, sizeof(buf));
    patchlib_field_get_value(f_player_who_am_i, player, buf);
    int who = -1;
    memcpy(&who, buf, sizeof(who));
    return who;
}

static active_buff_t g_active[BUFF_ACTIVE_MAX];
static int g_active_count;

/* ------------------------------------------------------------------ */
/* 注册表                                                              */
/* ------------------------------------------------------------------ */

static int compare_buff(const void *a, const void *b) {
    const terraria_buff_handle_t *pa = *(const terraria_buff_handle_t **) a;
    const terraria_buff_handle_t *pb = *(const terraria_buff_handle_t **) b;

    int cmp = strcmp(pa->parent_modloader_id, pb->parent_modloader_id);
    if (cmp != 0) return cmp;
    cmp = strcmp(pa->parent_id, pb->parent_id);
    if (cmp != 0) return cmp;
    return strcmp(pa->internal_name, pb->internal_name);
}

bool terraria_buff_manager_register_buff(terraria_buff_handle_t *buff_handle) {
    if (!buff_handle || !buff_handle->parent_modloader_id ||
        !buff_handle->parent_id || !buff_handle->internal_name)
        return false;

    const size_t count = tefstd_vector_size(&g_terraria_buff_registry);
    for (size_t i = 0; i < count; ++i) {
        terraria_buff_handle_t **existing =
                tefstd_vector_at(&g_terraria_buff_registry, i);
        if (!existing || !*existing)
            continue;
        if (strcmp((*existing)->parent_modloader_id,
                   buff_handle->parent_modloader_id) == 0 &&
            strcmp((*existing)->parent_id, buff_handle->parent_id) == 0 &&
            strcmp((*existing)->internal_name,
                   buff_handle->internal_name) == 0) {
            buff_handle->runtime_id = (*existing)->runtime_id;
            buff_handle->texture = (*existing)->texture;
            *existing = buff_handle;
            TEKLOG_INFO("buff re-registered (hot reload): %s.%s id=%d",
                        buff_handle->parent_id, buff_handle->internal_name,
                        buff_handle->runtime_id);
            return true;
        }
    }

    if (g_ids_assigned)
        TEKLOG_WARN("buff registered after IDs assigned: %s.%s", buff_handle->parent_id,
                    buff_handle->internal_name);

    return tefstd_vector_push_back(&g_terraria_buff_registry, &buff_handle);
}

static void clear_active_of(const int runtime_id) {
    for (int i = 0; i < g_active_count;) {
        if (g_active[i].buff_id == runtime_id) {
            g_active[i] = g_active[g_active_count - 1];
            g_active_count--;
        } else {
            i++;
        }
    }
}

bool terraria_buff_manager_unregister_buff_by_id(const int runtime_id) {
    if (runtime_id < 0)
        return false;
    const size_t count = tefstd_vector_size(&g_terraria_buff_registry);
    for (size_t i = 0; i < count; ++i) {
        terraria_buff_handle_t **pp =
                tefstd_vector_at(&g_terraria_buff_registry, i);
        if (!pp || !*pp)
            continue;
        if ((*pp)->runtime_id == runtime_id) {
            clear_active_of(runtime_id);
            const bool removed =
                    tefstd_vector_erase(&g_terraria_buff_registry, i, NULL);
            if (removed)
                terraria_buff_manager_assign_ids(terraria_buff_id_get_count());
            return removed;
        }
    }
    return false;
}

bool terraria_buff_manager_assign_ids(const int base_id) {
    const size_t count = tefstd_vector_size(&g_terraria_buff_registry);
    if (count == 0) {
        g_ids_assigned = true;
        return true;
    }
    terraria_buff_handle_t **items = g_terraria_buff_registry.data;
    qsort(items, count, sizeof(terraria_buff_handle_t *), compare_buff);
    for (size_t i = 0; i < count; ++i)
        items[i]->runtime_id = base_id + (int) i;
    g_ids_assigned = true;
    return true;
}

int terraria_buff_id_get_count(void) {
    return 401;
}

terraria_buff_handle_t *terraria_buff_manager_get_buff(const int runtime_id) {
    const int base = terraria_buff_id_get_count();
    const int nid = runtime_id - base;
    const size_t count = tefstd_vector_size(&g_terraria_buff_registry);
    if (nid < 0 || nid >= (int) count)
        return NULL;
    terraria_buff_handle_t **pp =
            tefstd_vector_at(&g_terraria_buff_registry, (size_t) nid);
    return pp ? *pp : NULL;
}

tefstd_vector_t *terraria_buff_manager_get_buffs(void) {
    return &g_terraria_buff_registry;
}

terraria_buff_handle_t *terraria_buff_manager_find_buff(const char *loader, const char *mod,
                                                        const char *name) {
    if (!loader || !mod || !name)
        return NULL;
    const size_t count = tefstd_vector_size(&g_terraria_buff_registry);
    for (size_t i = 0; i < count; ++i) {
        terraria_buff_handle_t **pp = tefstd_vector_at(&g_terraria_buff_registry, i);
        if (!pp || !*pp)
            continue;
        if (strcmp((*pp)->parent_modloader_id, loader) == 0 &&
            strcmp((*pp)->parent_id, mod) == 0 &&
            strcmp((*pp)->internal_name, name) == 0)
            return *pp;
    }
    return NULL;
}

static terraria_buff_handle_t *find_by_runtime_id(const int runtime_id) {
    const size_t count = tefstd_vector_size(&g_terraria_buff_registry);
    for (size_t i = 0; i < count; ++i) {
        terraria_buff_handle_t **pp = tefstd_vector_at(&g_terraria_buff_registry, i);
        if (pp && *pp && (*pp)->runtime_id == runtime_id)
            return *pp;
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* 活跃 Buff                                                           */
/* ------------------------------------------------------------------ */

static active_buff_t *find_active(patch_handle_t player, const int runtime_id) {
    const int who = player_who(player);
    for (int i = 0; i < g_active_count; ++i)
        if (g_active[i].who == who && g_active[i].buff_id == runtime_id)
            return &g_active[i];
    return NULL;
}

int terraria_buff_manager_add(terraria_buff_handle_t *h, patch_handle_t player, int time) {
    if (!h || h->runtime_id < 0 || player == PATCH_NULL || time <= 0)
        return -1;

    active_buff_t *a = find_active(player, h->runtime_id);
    if (!a) {
        if (g_active_count >= BUFF_ACTIVE_MAX) {
            TEKLOG_WARN("buff: active 列表已满 (%d)", BUFF_ACTIVE_MAX);
            return -1;
        }
        a = &g_active[g_active_count++];
        a->player = PATCH_NULL;   /* 桌面版句柄是临时的，身份只用 who */
        a->who = player_who(player);
        a->buff_id = h->runtime_id;
        a->time = 0;
    }
    if (time > a->time) a->time = time;
    if (h->buff_ops.apply) h->buff_ops.apply(h, player, a->time);
    return a->time;
}

int terraria_buff_manager_time(terraria_buff_handle_t *h, patch_handle_t player) {
    if (!h || player == PATCH_NULL) return 0;
    active_buff_t *a = find_active(player, h->runtime_id);
    return a ? a->time : 0;
}

void terraria_buff_manager_remove(terraria_buff_handle_t *h, patch_handle_t player) {
    if (!h || player == PATCH_NULL) return;
    for (int i = 0; i < g_active_count;) {
        if (g_active[i].who == player_who(player) && g_active[i].buff_id == h->runtime_id) {
            g_active[i] = g_active[g_active_count - 1];
            g_active_count--;
        } else {
            i++;
        }
    }
}

/* 每帧：推进这个玩家身上的自定义 Buff */
static void player_tick(patch_handle_t this) {
    if (!this || g_active_count == 0)
        return;
    for (int i = 0; i < g_active_count;) {
        active_buff_t *a = &g_active[i];
        if (a->who != player_who(this)) {
            i++;
            continue;
        }
        terraria_buff_handle_t *h = find_by_runtime_id(a->buff_id);
        if (!h) {
            g_active[i] = g_active[g_active_count - 1];
            g_active_count--;
            continue;
        }
        if (a->time > 0) a->time--;
        if (a->time <= 0) {
            TEKLOG_DEBUG("buff: %s 到期", h->internal_name);
            g_active[i] = g_active[g_active_count - 1];
            g_active_count--;
            continue;
        }
        if (h->buff_ops.update)
            h->buff_ops.update(h, this, i);
        i++;
    }
}

static void update_buffs_postfix(patch_handle_t this, void **args, void *result,
                                 const patch_method_signature_t *sig) {
    (void) args; (void) result; (void) sig;
    player_tick(this);
}

/* ------------------------------------------------------------------ */
/* 图标                                                                */
/* ------------------------------------------------------------------ */

void terraria_buff_manager_init_texture2d(void) {
    const size_t count = tefstd_vector_size(&g_terraria_buff_registry);
    int done = 0;
    for (size_t i = 0; i < count; ++i) {
        terraria_buff_handle_t **pp = tefstd_vector_at(&g_terraria_buff_registry, i);
        if (!pp || !*pp)
            continue;
        terraria_buff_handle_t *h = *pp;
        if (h->texture == PATCH_NULL && h->buff_ops.get_texture)
            h->texture = h->buff_ops.get_texture(h);
#if defined(__ANDROID__)
        /* C 结构体里的引用不算 GC root，必须用 GCHandle 钉住，否则贴图会被回收 */
        if (h->texture != PATCH_NULL && h->texture_pin == NULL && il2cpp_gchandle_new)
            h->texture_pin = il2cpp_gchandle_new(h->texture, false);
        TEKLOG_INFO("buff: pin id=%d gchandle_new=%p texture=%p pin=%p",
                    h->runtime_id, (void *) il2cpp_gchandle_new,
                    (void *) h->texture, (void *) h->texture_pin);
#endif
        if (h->texture != PATCH_NULL) done++;
    }
    TEKLOG_INFO("buff textures: %d/%zu set", done, count);
}

/* ------------------------------------------------------------------ */
/* 覆盖层：图标 + 倒计时                                                */
/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */
/* 原生 Buff 栏接管                                                     */
/*                                                                     */
/* GUIBuffs.Draw 每帧:                                                  */
/*   numActiveBuffs = 0;                                               */
/*   扫 buffType: 1..400 -> activeBuffs[n++]；>=401 -> buffType[i] = 0 */
/*   GUIItemGrid.Draw(grid, itemScale, itemOver, itemDraw, n, tex, 0)  */
/*   -> 逐项回调 GUIBuffs.ItemDraw(index, position, scale)              */
/*                                                                     */
/* 我们在 GUIItemGrid.Draw 前把计数 +N，再在 ItemDraw 里接管多出来的     */
/* index：位置/缩放/批次/颜色都由原版网格算好，天然与原生 Buff 栏一致。   */
/* ------------------------------------------------------------------ */

static int g_in_bar;          /* 正在 GUIBuffs.Draw 内 */
static int g_bar_base = -1;   /* 原版数量（多出来的 index 从这里开始） */
static unsigned g_bar_epoch = 0;       /* GUIBuffs.Draw 每帧自增 */
static unsigned g_bar_mask_epoch = 0;  /* 已画掩码所属帧 */
static unsigned long long g_bar_mask = 0;

static int main_my_player(void) {
    unsigned char buf[64];
    memset(buf, 0, sizeof(buf));
    if (m_get_my_player != PATCH_NULL) {
        patchlib_method_invoke_args(m_get_my_player, NULL, buf, NULL);
        int my = 0;
        memcpy(&my, buf, sizeof(my));
        return my;
    }
    if (f_main_my_player != PATCH_NULL) {
        patchlib_field_get_value(f_main_my_player, NULL, buf);
        int my = 0;
        memcpy(&my, buf, sizeof(my));
        return my;
    }
    return -1;
}

static patch_handle_t local_player_obj(void) {
    if (f_main_player == PATCH_NULL) return PATCH_NULL;
    const int my = main_my_player();
    if (my < 0) return PATCH_NULL;
    patch_handle_t players = PATCH_NULL;
    patchlib_field_get_value(f_main_player, NULL, &players);
    if (players == PATCH_NULL) return PATCH_NULL;
    patch_handle_t local = PATCH_NULL;
    patchlib_array_at(players, (size_t) my, &local);
    patchlib_free(players);
    return local;
}

static void *main_sprite_batch(void) {
    if (f_sprite_batch == PATCH_NULL) return NULL;
    unsigned char sbbuf[64];
    memset(sbbuf, 0, sizeof(sbbuf));
    patchlib_field_get_value(f_sprite_batch, NULL, sbbuf);
    void *sb = NULL;
    memcpy(&sb, sbbuf, sizeof(sb));
    return sb;
}

static int local_custom_count(void) {
    if (f_main_player == PATCH_NULL) return 0;
    const int my = main_my_player();
    if (my < 0) return 0;
    patch_handle_t players = PATCH_NULL;
    patchlib_field_get_value(f_main_player, NULL, &players);
    if (players == PATCH_NULL) return 0;
    patch_handle_t local = PATCH_NULL;
    patchlib_array_at(players, (size_t) my, &local);
    patchlib_free(players);
    if (local == PATCH_NULL) return 0;
    int n = 0;
    const int my_who = player_who(local);
    patchlib_free(local);   /* 桌面版 array_at 每次分配一个 GCHandle */
    for (int i = 0; i < g_active_count; ++i)
        if (g_active[i].who == my_who && g_active[i].time > 0) n++;
    return n;
}

static patch_handle_t resolve_getter_like(patch_handle_t type, const char *name) {
    patch_handle_t prop = patchlib_type_get_property(type, name);
    if (!prop || !patchlib_is_valid(prop)) return PATCH_NULL;
    patch_handle_t getter = patchlib_property_get_get_method(prop);
    patchlib_free(prop);
    return getter;
}

static active_buff_t *local_nth_custom(int n) {
    if (f_main_player == PATCH_NULL) return NULL;
    const int my = main_my_player();
    if (my < 0) return NULL;
    patch_handle_t players = PATCH_NULL;
    patchlib_field_get_value(f_main_player, NULL, &players);
    if (players == PATCH_NULL) return NULL;
    patch_handle_t local = PATCH_NULL;
    patchlib_array_at(players, (size_t) my, &local);
    patchlib_free(players);
    if (local == PATCH_NULL) return NULL;
    const int who = player_who(local);
    patchlib_free(local);
    for (int i = 0; i < g_active_count; ++i) {
        if (g_active[i].who != who || g_active[i].time <= 0) continue;
        if (n-- == 0) return &g_active[i];
    }
    return NULL;
}

static bool buffs_draw_prefix(patch_handle_t this, void **args,
                              const patch_method_signature_t *sig, void *result) {
    (void) this; (void) args; (void) sig; (void) result;
    g_in_bar = 1;
    g_bar_base = -1;
    g_bar_epoch++;
    return false;
}

static void buffs_draw_postfix(patch_handle_t this, void **args, void *result,
                               const patch_method_signature_t *sig) {
    (void) this; (void) args; (void) result; (void) sig;
    g_in_bar = 0;
    g_bar_base = -1;
}

static void buff_item_draw_postfix(patch_handle_t this, void **args, void *result,
                                   const patch_method_signature_t *sig) {
    (void) this; (void) sig; (void) result;
    if (!g_in_bar || g_bar_base < 0 || !args || !args[1]) return;
    const int index = *(int *) args[1];
    if (index >= g_bar_base) {
        static int dbg;
        if (dbg < 3) {
            dbg++;
            TEKLOG_DEBUG("buff bar: postfix ran for index=%d", index);
        }
    }
}

/* GUIItemGrid.Draw(grid, itemScale, itemOver, itemDraw, count, texture, flag) */
static bool item_grid_draw_prefix(patch_handle_t this, void **args,
                                  const patch_method_signature_t *sig, void *result) {
    (void) this; (void) sig; (void) result;
    if (!g_in_bar || !args || !args[4]) return false;
    /* 同一帧里这个网格可能被画多次（重排/重绘），只扩展一次；
     * 否则 g_bar_base 会一帧内不断上移，导致我们的图标被重复画、
     * 而原版又拿到越界的 activeBuffs[index] 画出别的 Buff 和随机时长。 */
    if (g_bar_base >= 0) {
        static int repeat_logged;
        if (repeat_logged < 3) {
            repeat_logged++;
            TEKLOG_WARN("buff bar: GUIItemGrid.Draw 同一帧被调用多次（count=%d, base=%d）",
                        *(int *) args[4], g_bar_base);
        }
        return false;
    }
    const int extra = local_custom_count();
    if (extra <= 0) return false;
    int *count = (int *) args[4];
    g_bar_base = *count;
    *count += extra;
    TEKLOG_DEBUG("buff bar: extend %d -> %d", g_bar_base, *count);
    return false;
}

/* GUIBuffs.ItemDraw(gridLayout, index, position, scale) */
static bool buff_item_draw_prefix(patch_handle_t this, void **args,
                                  const patch_method_signature_t *sig, void *result) {
    (void) this; (void) sig; (void) result;
    if (!g_in_bar || g_bar_base < 0 || !args || !args[1] || !args[2] || !args[3])
        return false;
    const int index = *(int *) args[1];
    if (index < g_bar_base) return false;   /* 原版项交给原版画 */

    const int slot = index - g_bar_base;
    if (slot >= 0 && slot < 64) {
        /* 同一帧同一格只画一次，避免重复绘制叠加亮度 */
        if (g_bar_mask_epoch != g_bar_epoch) {
            g_bar_mask_epoch = g_bar_epoch;
            g_bar_mask = 0;
        }
        const unsigned long long bit = 1ull << slot;
        if (g_bar_mask & bit) return true;
        g_bar_mask |= bit;
    }

    active_buff_t *a = local_nth_custom(slot);
    if (!a) return true;
    terraria_buff_handle_t *h = find_by_runtime_id(a->buff_id);
    if (!h) return true;

    const float *pos = (const float *) args[2];
    /* 原版：scale = Buffs_Layout.ItemScale（未选中时），贴图 32x32 按它缩放 */
    float item_scale = 1.0f;
    /* 原版: Lerp(NormalColour, SelectedColour, buffAlpha)；激活态 buffAlpha 高，
     * 等价于 SelectedColour(白)。NormalColour 是 40% 透明灰，直接用会压暗图标。 */
    /* 图标略微透明：原版图标是 Lerp(NormalColour, SelectedColour, buffAlpha)，
     * 不是纯白不透明；纯白会明显比旁边的原版图标亮。 */
    unsigned tint = 0x80FFFFFFu;   /* alpha = 50% */
    if (m_layout_instance != PATCH_NULL) {
        patch_handle_t inst = PATCH_NULL;
        unsigned char ibuf[64];
        memset(ibuf, 0, sizeof(ibuf));
        patchlib_method_invoke_args(m_layout_instance, NULL, ibuf, NULL);
        memcpy(&inst, ibuf, sizeof(inst));
        if (inst != PATCH_NULL) {
            if (f_layout_item_scale != PATCH_NULL) {
                unsigned char fbuf[64];
                memset(fbuf, 0, sizeof(fbuf));
                patchlib_field_get_value(f_layout_item_scale, inst, fbuf);
                memcpy(&item_scale, fbuf, sizeof(item_scale));
            }
            patchlib_free(inst);
        }
    }
    if (!(item_scale > 0.0f)) item_scale = 1.0f;
    const float size = 32.0f * item_scale;

    void *sb = NULL;
    if (f_sprite_batch != PATCH_NULL) {
        unsigned char sbbuf[64];
        memset(sbbuf, 0, sizeof(sbbuf));
        patchlib_field_get_value(f_sprite_batch, NULL, sbbuf);
        memcpy(&sb, sbbuf, sizeof(sb));
    }
    if (!sb) return true;
    /* 游戏 UI 批次已经开着：借用它，坐标/混合/字体都与原版一致 */
    terraria_gui_begin_external(sb);

    if (h->texture != PATCH_NULL)
        terraria_gui_image_raw(h->texture, pos[0], pos[1], size, size, tint);
    else
        terraria_gui_rect(pos[0], pos[1], size, size, 0xFF00FF00u);

    static int dbg;
    if (dbg < 3) {
        dbg++;
        TEKLOG_INFO("buff bar: override index=%d pos=(%.1f,%.1f) scale=%.2f size=%.1f tint=%08x",
                    index, pos[0], pos[1], item_scale, size, tint);
    }

    if (!h->no_time_display) {
        /* 与原版 GUIBuffs.ItemDraw 一致：<60s 用秒，<60min 用分，再往上用小时，
         * 数字四舍五入；带单位后缀。 */
        char label[24];
        if (a->time < 3600) {
            int v = (a->time + 30) / 60;
            if (v < 1) v = 1;
            snprintf(label, sizeof(label), "%d s", v);
        } else if (a->time < 216000) {
            snprintf(label, sizeof(label), "%d m", (a->time + 1800) / 3600);
        } else {
            snprintf(label, sizeof(label), "%d h", (a->time + 108000) / 216000);
        }
        {
            static unsigned last_log;
            static int last_frames = -1;
            if ((unsigned) (a->time / 60) != last_log) {
                last_log = (unsigned) (a->time / 60);
                if (a->time != last_frames) {
                    last_frames = a->time;
                    TEKLOG_INFO("buff bar: time id=%d frames=%d sec=%d",
                                a->buff_id, a->time, (a->time + 59) / 60);
                }
            }
        }
        /* 原版：位置 = 图标位置 + TextOffset * ItemScale，
         *       字号 = ItemScale * TextScale，
         *       用 SpriteBatch.DrawString（无描边），颜色同样吃 50% 透明 */
        float text_scale = 0.8f;
        float off_x = size * 0.15f, off_y = size * 0.95f;
        patch_handle_t inst2 = PATCH_NULL;
        if (m_layout_instance != PATCH_NULL) {
            unsigned char ibuf2[64];
            memset(ibuf2, 0, sizeof(ibuf2));
            patchlib_method_invoke_args(m_layout_instance, NULL, ibuf2, NULL);
            memcpy(&inst2, ibuf2, sizeof(inst2));
        }
        if (inst2 != PATCH_NULL) {
            if (f_layout_text_scale != PATCH_NULL) {
                unsigned char tbuf[64];
                memset(tbuf, 0, sizeof(tbuf));
                patchlib_field_get_value(f_layout_text_scale, inst2, tbuf);
                float v = 0.0f;
                memcpy(&v, tbuf, sizeof(v));
                if (v > 0.0f) text_scale = v;
            }
            if (f_layout_text_offset != PATCH_NULL) {
                unsigned char obuf[64];
                memset(obuf, 0, sizeof(obuf));
                patchlib_field_get_value(f_layout_text_offset, inst2, obuf);
                float ox = 0.0f, oy = 0.0f;
                memcpy(&ox, obuf, sizeof(ox));
                memcpy(&oy, obuf + 4, sizeof(oy));
                off_x = ox * item_scale;
                off_y = oy * item_scale;
            }
            patchlib_free(inst2);
        }
        const float glyph_scale = item_scale * text_scale;
        terraria_gui_text(label, pos[0] + off_x, pos[1] + off_y, glyph_scale, tint);
    }
    terraria_gui_end_external();
    return true;   /* 跳过原版（它会把 activeBuffs 越界） */
}

/* ------------------------------------------------------------------ */
/* PC（桌面）原生 Buff 栏                                               */
/* ------------------------------------------------------------------ */
#if !defined(__ANDROID__)
/* 记录原版这一帧画 Buff 图标用的坐标，直接以它为基准续画，不猜布局 */
static int g_pc_icon_count;
static int g_pc_icon_x, g_pc_icon_y;                 /* 最后一个图标 */
static int g_pc_icon_prev_x, g_pc_icon_prev_y;       /* 倒数第二个 */
static int g_pc_first_x = -1, g_pc_first_y = -1;      /* 第一个图标 */

static bool pc_draw_buff_icon_prefix(patch_handle_t this, void **args,
                                     const patch_method_signature_t *sig, void *result) {
    (void) this; (void) sig; (void) result;
    if (!args || !args[2] || !args[3]) return false;
    const int x = *(int *) args[2];
    const int y = *(int *) args[3];
    if (g_pc_icon_count == 0) {
        g_pc_first_x = x;
        g_pc_first_y = y;
    } else {
        g_pc_icon_prev_x = g_pc_icon_x;
        g_pc_icon_prev_y = g_pc_icon_y;
    }
    g_pc_icon_x = x;
    g_pc_icon_y = y;
    g_pc_icon_count++;
    return false;
}

static bool pc_buffs_draw_prefix(patch_handle_t this, void **args,
                                 const patch_method_signature_t *sig, void *result) {
    (void) this; (void) args; (void) sig; (void) result;
    g_pc_icon_count = 0;
    g_pc_icon_x = g_pc_icon_y = -1;
    g_pc_icon_prev_x = g_pc_icon_prev_y = -1;
    g_pc_first_x = g_pc_first_y = -1;
    return false;
}

/* 原版布局（Terraria 1.4.5.8 Main.DrawInterface_Resources_Buffs）：
 *   x = 32 + n*38, y = 76 + (n/11)*50，图标 32x32、scale 1
 *   计时文字 FontAssets.ItemStack、scale 0.8、位置 (x, y+32) */
static void pc_buffs_draw_postfix(patch_handle_t this, void **args, void *result,
                                  const patch_method_signature_t *sig) {
    (void) this; (void) args; (void) result; (void) sig;
    if (g_active_count <= 0 || f_player_buff_type == PATCH_NULL) return;
    patch_handle_t local = local_player_obj();
    if (local == PATCH_NULL) return;
    patch_handle_t types = PATCH_NULL;
    patchlib_field_get_value(f_player_buff_type, local, &types);
    if (types == PATCH_NULL) return;
    const size_t len = patchlib_array_length(types);
    int n = 0;
    for (size_t i = 0; i < len; ++i) {
        int t = 0;
        if (patchlib_array_at(types, i, &t) && t > 0) n++;
    }
    patchlib_free(types);
    const int local_who = local ? player_who(local) : -1;

    /* 基准：优先用原版这一帧第一个图标的真实坐标（比抄常量可靠） */
    int base_x = 32, base_y = 76, step = 38;
    if (g_pc_icon_count > 0 && g_pc_first_x >= 0) {
        base_x = g_pc_first_x;
        base_y = g_pc_first_y;
        if (g_pc_icon_count > 1) {
            const int dx = g_pc_icon_x - g_pc_icon_prev_x;
            const int dy = g_pc_icon_y - g_pc_icon_prev_y;
            if (dx > 0 && dy == 0) step = dx;
        }
    }
    {
        static int logged;
        if (logged < 3) {
            logged++;
            TEKLOG_INFO("pc buff bar: vanilla=%d recorded=%d base=(%d,%d) step=%d",
                        n, g_pc_icon_count, base_x, base_y, step);
        }
    }

    void *sb = main_sprite_batch();
    if (!sb) return;
    terraria_gui_begin_external(sb);
    const unsigned tint = 0x80FFFFFFu;   /* 50%，与原版 buffAlpha 观感接近 */
    for (int i = 0; i < g_active_count; ++i) {
        active_buff_t *a = &g_active[i];
        if (a->who != local_who || a->time <= 0) continue;
        terraria_buff_handle_t *h = find_by_runtime_id(a->buff_id);
        if (!h) continue;
        const float x = (float) (base_x + (n % 11) * step);
        const float y = (float) (base_y + (n / 11) * 50);
        if (h->texture != PATCH_NULL)
            terraria_gui_image_raw(h->texture, x, y, 32.0f, 32.0f, tint);
        else
            terraria_gui_rect(x, y, 32.0f, 32.0f, 0xFF00FF00u);
        if (!h->no_time_display) {
            char label[24];
            if (a->time < 3600) {
                int v = (a->time + 30) / 60;
                if (v < 1) v = 1;
                snprintf(label, sizeof(label), "%d s", v);
            } else if (a->time < 216000) {
                snprintf(label, sizeof(label), "%d m", (a->time + 1800) / 3600);
            } else {
                snprintf(label, sizeof(label), "%d h", (a->time + 108000) / 216000);
            }
            terraria_gui_text(label, x, y + 32.0f, 0.8f, tint);
        }
        n++;
    }
    terraria_gui_end_external();
    if (local) patchlib_free(local);
}
#endif

/* ------------------------------------------------------------------ */
/* 生命周期                                                            */
/* ------------------------------------------------------------------ */

void terraria_buff_manager_init(void) {
    tefstd_vector_init(&g_terraria_buff_registry, sizeof(terraria_buff_handle_t *));

    patch_handle_t main_cls = patchlib_type_get_type("Terraria", "Main");
    if (patchlib_is_valid(main_cls)) {
        f_main_player = patchlib_type_get_field(main_cls, "player");
        f_sprite_batch = patchlib_type_get_field(main_cls, "spriteBatch");
        f_main_my_player = patchlib_type_get_field(main_cls, "myPlayer");
        m_get_my_player =
                patchlib_type_get_method_by_param_count(main_cls, "get_myPlayer", 0);
        if (!patchlib_is_valid(m_get_my_player))
            m_get_my_player = patchlib_type_get_method_by_param_count(main_cls, "get_myPlayer", 0);
#if !defined(__ANDROID__)
        /* PC 没有 GUIBuffs/GUIItemGrid，Buff 栏由 DrawInterface_Resources_Buffs 画 */
        {
            patch_handle_t d = patchlib_type_get_method_by_param_count(
                    main_cls, "DrawInterface_Resources_Buffs", 0);
            if (patchlib_is_valid(d)) {
                h_buff_bar = patchlib_install_prepost_hook(d, pc_buffs_draw_prefix,
                                                           pc_buffs_draw_postfix);
                TEKLOG_INFO("buff: hooked Main.DrawInterface_Resources_Buffs (%d)",
                            (int) h_buff_bar);
            } else {
                TEKLOG_WARN("buff: 未找到 Main.DrawInterface_Resources_Buffs（PC 图标不会显示）");
            }
            /* 记录原版图标坐标（静态方法 4 参：drawBuffText, slot, x, y） */
            patch_handle_t icon = patchlib_type_get_method_by_param_count(
                    main_cls, "DrawBuffIcon", 4);
            if (patchlib_is_valid(icon)) {
                patchlib_install_prepost_hook(icon, pc_draw_buff_icon_prefix, NULL);
                TEKLOG_INFO("buff: hooked Main.DrawBuffIcon (基准坐标)");
            } else {
                TEKLOG_WARN("buff: 未找到 Main.DrawBuffIcon（图标位置退回常量）");
            }
        }
#endif
        patchlib_free(main_cls);
    }

    patch_handle_t player_cls = patchlib_type_get_type("Terraria", "Player");
    if (patchlib_is_valid(player_cls)) {
        f_player_buff_type = patchlib_type_get_field(player_cls, "buffType");
        f_player_who_am_i = patchlib_type_get_field(player_cls, "whoAmI");
        patch_handle_t upd = patchlib_type_get_method_by_param_count(player_cls, "UpdateBuffs", 1);
        if (patchlib_is_valid(upd)) {
            h_player_update = patchlib_install_prepost_hook(upd, NULL, update_buffs_postfix);
            TEKLOG_INFO("buff: hooked Player.UpdateBuffs (%d)", (int) h_player_update);
        }
        patchlib_free(player_cls);
    }

    /* 原版 Buff 栏布局（缩放与配色） */
    {
        patch_handle_t lay = patchlib_type_get_type("", "Buffs_Layout");
        if (patchlib_is_valid(lay)) {
            m_layout_instance = resolve_getter_like(lay, "Instance");
            f_layout_item_scale = patchlib_type_get_field(lay, "ItemScale");
            f_layout_normal_colour = patchlib_type_get_field(lay, "NormalColour");
            f_layout_text_scale = patchlib_type_get_field(lay, "TextScale");
            if (!patchlib_is_valid(f_layout_text_scale))
                f_layout_text_scale = patchlib_type_get_field(lay, "ItemTextScale");
            f_layout_text_offset = patchlib_type_get_field(lay, "TextOffset");
            patchlib_free(lay);
        }
    }

    /* 原生 Buff 栏接管 */
    {
        patch_handle_t gui = patchlib_type_get_type("", "GUIBuffs");
        if (patchlib_is_valid(gui)) {
            patch_handle_t d = patchlib_type_get_method_by_param_count(gui, "Draw", 0);
            if (patchlib_is_valid(d)) {
                h_buff_bar = patchlib_install_prepost_hook(d, buffs_draw_prefix,
                                                           buffs_draw_postfix);
                TEKLOG_INFO("buff: hooked GUIBuffs.Draw (%d)", (int) h_buff_bar);
            }
            patch_handle_t idraw = patchlib_type_get_method_by_param_count(gui, "ItemDraw", 4);
            if (patchlib_is_valid(idraw)) {
                patch_hook_id_t hid = patchlib_install_prepost_hook(
                        idraw, buff_item_draw_prefix, buff_item_draw_postfix);
                TEKLOG_INFO("buff: hooked GUIBuffs.ItemDraw (%d)", (int) hid);
            }
            patchlib_free(gui);
        }
        patch_handle_t grid = patchlib_type_get_type("", "GUIItemGrid");
        if (patchlib_is_valid(grid)) {
            patch_handle_t gd = patchlib_type_get_method_by_param_count(grid, "Draw", 6);
            if (patchlib_is_valid(gd)) {
                patch_hook_id_t hid =
                        patchlib_install_prepost_hook(gd, item_grid_draw_prefix, NULL);
                TEKLOG_INFO("buff: hooked GUIItemGrid.Draw (%d)", (int) hid);
            }
            patchlib_free(grid);
        }
    }
    TEKLOG_INFO("buff: init done (custom=%zu, 不占用原版 BuffID)",
                tefstd_vector_size(&g_terraria_buff_registry));
}

void terraria_buff_manager_destroy(void) {
#if defined(__ANDROID__)
    const size_t n = tefstd_vector_size(&g_terraria_buff_registry);
    for (size_t i = 0; i < n; ++i) {
        terraria_buff_handle_t **pp = tefstd_vector_at(&g_terraria_buff_registry, i);
        if (pp && *pp && (*pp)->texture_pin && il2cpp_gchandle_free) {
            il2cpp_gchandle_free((*pp)->texture_pin);
            (*pp)->texture_pin = NULL;
        }
    }
#endif
    if (h_player_update != PATCH_HOOK_INVALID_ID) {
        patchlib_uninstall_hook(h_player_update);
        h_player_update = PATCH_HOOK_INVALID_ID;
    }
    tefstd_vector_destroy(&g_terraria_buff_registry);
    g_active_count = 0;
}
