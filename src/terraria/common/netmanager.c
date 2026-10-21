/*******************************************************************************
 * tefkernel - netmanager
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
 * Created: 2026/7/24
 *******************************************************************************/

#include <string.h>
#include <stdlib.h>

#include "internal/terraria/netmanager.h"

#include <stdint.h>
#include <stdio.h>

#include "internal/log.h"
#include "patchlib/field.h"
#include "patchlib/method.h"
#include "patchlib/struct/array.h"
#include "patchlib/struct/string.h"
#include "terraria/main.h"

#include "../../tefpackage/lz4.h"
#include "internal/mod_core.h"
#include "internal/modloader/modloader_core_imp.h"
#include "internal/module/module_core_imp.h"

// 前向声明
connection_type_t terraria_netmanager_client_connections[256];
error_type_t terraria_netmanager_client_errors[256];
char terraria_netmanager_error_details[256][1024];

static patch_handle_t network_text_from_literal = PATCH_NULL;
#if !defined(__ANDROID__)
static patch_handle_t read_buffer = PATCH_NULL;
static patch_handle_t read_buffer_max = PATCH_NULL;
#endif
static patch_handle_t who_am_i = PATCH_NULL;

static bool send_data_hook(patch_handle_t instance, void **args,
                           const patch_method_signature_t *sig_info, void *result);

static bool send_hook(patch_handle_t instance, void **args,
                      const patch_method_signature_t *sig_info, void *result);

static bool get_data_hook(patch_handle_t instance, void **args,
                          const patch_method_signature_t *sig_info, void *result);

/**
 * @brief 获取所有模块、ModLoader和Mod的ID和版本列表
 * @param out_data 输出的序列化数据（调用者需释放）
 * @param out_size 输出数据大小
 * @return 成功返回true
 */
static bool terraria_netmanager_get_all_versions(uint8_t **out_data, uint32_t *out_size) {
    if (!out_data || !out_size) return false;

    // 使用动态缓冲区
    size_t buffer_size = 4096;
    uint8_t *buffer = malloc(buffer_size);
    if (!buffer) return false;

    size_t offset = 0;

    // 写入版本号标识 (用于快速检查)
    buffer[offset++] = 0x01; // 协议版本

    // ===== 写入Module列表 =====
    const size_t module_count = tefkernel_get_module_count();

    // 写入数量 (2字节)
    if (offset + 2 > buffer_size) {
        buffer_size += 1024;
        buffer = realloc(buffer, buffer_size); // NOLINT(*-suspicious-realloc-usage)
    }
    buffer[offset++] = (module_count >> 8) & 0xFF;
    buffer[offset++] = module_count & 0xFF;

    for (size_t i = 0; i < module_count; i++) {
        module_handle_t *mod = tefkernel_get_module_by_index(i);
        if (!mod) continue;

        const module_info_t *info = tefkernel_get_module_info(mod);
        if (!info) continue;

        // 写入pkg_id (长度 + 字符串)
        const size_t id_len = strlen(info->pkg_id);
        if (offset + 1 + id_len + 4 > buffer_size) {
            buffer_size += id_len + 1024;
            buffer = realloc(buffer, buffer_size); // NOLINT(*-suspicious-realloc-usage)
        }
        buffer[offset++] = (uint8_t) id_len;
        memcpy(buffer + offset, info->pkg_id, id_len);
        offset += id_len;

        // 写入version_code (4字节)
        buffer[offset++] = (info->version_code >> 24) & 0xFF;
        buffer[offset++] = (info->version_code >> 16) & 0xFF;
        buffer[offset++] = (info->version_code >> 8) & 0xFF;
        buffer[offset++] = info->version_code & 0xFF;
    }

    // ===== 写入ModLoader列表 =====
    const size_t ml_count = tefkernel_get_ml_count();
    if (offset + 2 > buffer_size) {
        buffer_size += 1024;
        buffer = realloc(buffer, buffer_size); // NOLINT(*-suspicious-realloc-usage)
    }
    buffer[offset++] = (ml_count >> 8) & 0xFF;
    buffer[offset++] = ml_count & 0xFF;

    for (size_t i = 0; i < ml_count; i++) {
        ml_handle_t *ml = tefkernel_get_ml_by_index(i);
        if (!ml) continue;

        const ml_info_t *info = ml->ml_entry->info;
        if (!info) continue;

        const size_t id_len = strlen(info->pkg_id);
        if (offset + 1 + id_len + 4 > buffer_size) {
            buffer_size += id_len + 1024;
            buffer = realloc(buffer, buffer_size); // NOLINT(*-suspicious-realloc-usage)
        }
        buffer[offset++] = (uint8_t) id_len;
        memcpy(buffer + offset, info->pkg_id, id_len);
        offset += id_len;

        buffer[offset++] = (info->version_code >> 24) & 0xFF;
        buffer[offset++] = (info->version_code >> 16) & 0xFF;
        buffer[offset++] = (info->version_code >> 8) & 0xFF;
        buffer[offset++] = info->version_code & 0xFF;
    }

    // ===== 写入Mod列表 =====
    const size_t mod_count = tefkernel_get_mod_count();
    if (offset + 2 > buffer_size) {
        buffer_size += 1024;
        buffer = realloc(buffer, buffer_size); // NOLINT(*-suspicious-realloc-usage)
    }
    buffer[offset++] = (mod_count >> 8) & 0xFF;
    buffer[offset++] = mod_count & 0xFF;

    for (size_t i = 0; i < mod_count; i++) {
        mod_handle_t *mod = tefkernel_get_mod_by_index(i);
        if (!mod) continue;

        if (!mod->manifest || !mod->mod_id) continue;

        const size_t id_len = strlen(mod->mod_id);
        if (offset + 1 + id_len + 4 > buffer_size) {
            buffer_size += id_len + 1024;
            buffer = realloc(buffer, buffer_size); // NOLINT(*-suspicious-realloc-usage)
        }
        buffer[offset++] = (uint8_t) id_len;
        memcpy(buffer + offset, mod->mod_id, id_len);
        offset += id_len;

        // 写入mod的version_code (从manifest获取)
        const multiplayer_mod_info_t *mod_info = mod->owner_ml->ml_entry->ops->get_multiplayer_info(mod->manifest);
        const int version_code = mod_info->version_code ? mod_info->version_code : 0;
        buffer[offset++] = (version_code >> 24) & 0xFF;
        buffer[offset++] = (version_code >> 16) & 0xFF;
        buffer[offset++] = (version_code >> 8) & 0xFF;
        buffer[offset++] = version_code & 0xFF;
    }

    // ===== LZ4压缩 =====
    // 计算最大压缩大小
    const int max_compressed_size = LZ4_compressBound((int) offset);
    uint8_t *compressed = malloc(max_compressed_size + 4); // +4用于存储原始大小
    if (!compressed) {
        free(buffer);
        return false;
    }

    // 存储原始大小 (4字节)
    compressed[0] = (offset >> 24) & 0xFF;
    compressed[1] = (offset >> 16) & 0xFF;
    compressed[2] = (offset >> 8) & 0xFF;
    compressed[3] = offset & 0xFF;

    // 压缩
    const int compressed_size = LZ4_compress_default((char *) buffer, (char *) (compressed + 4), (int) offset,
                                                     max_compressed_size);
    if (compressed_size <= 0) {
        free(buffer);
        free(compressed);
        return false;
    }

    free(buffer);

    *out_data = compressed;
    *out_size = compressed_size + 4; // 包含原始大小头

    TEKLOG_INFO("Serialized data: %zu bytes, compressed to: %d bytes", offset, compressed_size);
    return true;
}

/**
 * @brief 比较本地和远程的模块/ModLoader/Mod列表（严格模式）
 * @param remote_data 远程发送的数据
 * @param remote_size 数据大小
 * @param error_msg 输出的错误消息
 * @param error_msg_size 错误消息缓冲区大小
 * @return 完全相同返回true，任何差异返回false
 */
static bool terraria_netmanager_compare_versions(const uint8_t *remote_data, const uint32_t remote_size,
                                                 char *error_msg, const size_t error_msg_size) {
    if (!remote_data || remote_size < 4) {
        snprintf(error_msg, error_msg_size, "Invalid data");
        return false;
    }

    // 解压
    const uint32_t original_size = (remote_data[0] << 24) | (remote_data[1] << 16) |
                                   (remote_data[2] << 8) | remote_data[3];

    if (original_size > 1024 * 1024) {
        snprintf(error_msg, error_msg_size, "Data too large: %u", original_size);
        return false;
    }

    uint8_t *decompressed = malloc(original_size);
    if (!decompressed) {
        snprintf(error_msg, error_msg_size, "Memory allocation failed");
        return false;
    }

    // 注：data_len 所指的"消息长度"与实际负载数组之间可能存在 2 字节的长度前缀偏差，
    // 沿用 LZ4_decompress_safe 会因尾部垃圾字节报错。这里使用 partial 版本，
    // 解到 orig_size 即停止，对收包端的长度语义差异保持宽容。
    const int decompressed_size = LZ4_decompress_safe_partial((char *) (remote_data + 4), (char *) decompressed,
                                                              (int) remote_size - 4, (int) original_size,
                                                              (int) original_size);
    if (decompressed_size < 0) {
        free(decompressed);
        snprintf(error_msg, error_msg_size, "LZ4 failed: ret=%d orig_size=%u remote_size=%u (payload=%u)",
                 decompressed_size, original_size, remote_size, remote_size - 4);
        TEKLOG_WARN("compare_versions: %s", error_msg);
        return false;
    }

    size_t offset = 0;
    char mismatch_details[2048] = {0};
    bool has_mismatch = false;
    int total_errors = 0;
    const int MAX_DISPLAY = 10;

    typedef struct {
        char id[128];
        int version;
        bool matched;
    } version_entry_t;

    typedef struct {
        char id[128];
        int version;
        char ml_id[128];
        bool matched;
    } mod_entry_t;

    typedef struct {
        char ml_id[128];
        char errors[1024];
        int count;
    } ml_group_t;

    version_entry_t *local_modules = NULL, *remote_modules = NULL;
    version_entry_t *local_mls = NULL, *remote_mls = NULL;
    mod_entry_t *local_mods = NULL, *remote_mods = NULL;
    ml_group_t *groups = NULL;

    // 检查协议版本
    const uint8_t proto_version = decompressed[offset++];
    if (proto_version != 0x01) {
        snprintf(error_msg, error_msg_size, "Protocol version mismatch: local=1, remote=%d", proto_version);
        free(decompressed);
        return false;
    }

    // ============================================================
    // 1. 比较 Module
    // ============================================================
    const uint16_t remote_module_count = (decompressed[offset] << 8) | decompressed[offset + 1];
    offset += 2;
    const size_t local_module_count = tefkernel_get_module_count();

    local_modules = calloc(512, sizeof(version_entry_t));
    remote_modules = calloc(512, sizeof(version_entry_t));
    if (!local_modules || !remote_modules) {
        free(local_modules);
        free(remote_modules);
        free(decompressed);
        snprintf(error_msg, error_msg_size, "Memory allocation failed");
        return false;
    }
    size_t local_modules_found = 0, remote_modules_found = 0;

    for (size_t i = 0; i < local_module_count && local_modules_found < 512; i++) {
        module_handle_t *mod = tefkernel_get_module_by_index(i);
        if (!mod) continue;
        const module_info_t *info = tefkernel_get_module_info(mod);
        if (!info) continue;
        strncpy(local_modules[local_modules_found].id, info->pkg_id, 127);
        local_modules[local_modules_found].id[127] = '\0';
        local_modules[local_modules_found].version = info->version_code;
        local_modules[local_modules_found].matched = false;
        local_modules_found++;
    }

    for (uint16_t i = 0; i < remote_module_count && offset < decompressed_size && remote_modules_found < 512; i++) {
        uint8_t id_len = decompressed[offset++];
        if (offset + id_len + 4 > decompressed_size) break;
        char id[128];
        if (id_len >= 128) id_len = 127;
        memcpy(id, decompressed + offset, id_len);
        id[id_len] = '\0';
        offset += id_len;
        const int version = (decompressed[offset] << 24) | (decompressed[offset + 1] << 16) |
                            (decompressed[offset + 2] << 8) | decompressed[offset + 3];
        offset += 4;
        strncpy(remote_modules[remote_modules_found].id, id, 127);
        remote_modules[remote_modules_found].id[127] = '\0';
        remote_modules[remote_modules_found].version = version;
        remote_modules[remote_modules_found].matched = false;
        remote_modules_found++;
    }

    char module_errors[1024] = {0};
    int module_error_count = 0;

    if (local_modules_found != remote_modules_found) {
        snprintf(module_errors + strlen(module_errors), sizeof(module_errors) - strlen(module_errors),
                 "count: %zu vs %zu\n", local_modules_found, remote_modules_found);
        module_error_count++;
        total_errors++;
    }

    for (size_t i = 0; i < local_modules_found && module_error_count < MAX_DISPLAY; i++) {
        bool found = false;
        for (size_t j = 0; j < remote_modules_found; j++) {
            if (strcmp(local_modules[i].id, remote_modules[j].id) == 0) {
                found = true;
                remote_modules[j].matched = true;
                if (local_modules[i].version != remote_modules[j].version) {
                    snprintf(module_errors + strlen(module_errors), sizeof(module_errors) - strlen(module_errors),
                             "%s : v%d != v%d\n", local_modules[i].id,
                             local_modules[i].version, remote_modules[j].version);
                    module_error_count++;
                    total_errors++;
                }
                break;
            }
        }
        if (!found) {
            snprintf(module_errors + strlen(module_errors), sizeof(module_errors) - strlen(module_errors),
                     "%s : missing (v%d)\n", local_modules[i].id, local_modules[i].version);
            module_error_count++;
            total_errors++;
        }
    }

    for (size_t i = 0; i < remote_modules_found && module_error_count < MAX_DISPLAY; i++) {
        if (!remote_modules[i].matched) {
            snprintf(module_errors + strlen(module_errors), sizeof(module_errors) - strlen(module_errors),
                     "%s : extra (v%d)\n", remote_modules[i].id, remote_modules[i].version);
            module_error_count++;
            total_errors++;
        }
    }

    // ============================================================
    // 2. 比较 ModLoader
    // ============================================================
    char ml_errors[1024] = {0};

    if (offset + 2 <= decompressed_size) {
        int ml_error_count = 0;
        const uint16_t remote_ml_count = (decompressed[offset] << 8) | decompressed[offset + 1];
        offset += 2;
        const size_t local_ml_count = tefkernel_get_ml_count();

        local_mls = calloc(128, sizeof(version_entry_t));
        remote_mls = calloc(128, sizeof(version_entry_t));
        size_t local_ml_found = 0, remote_ml_found = 0;

        for (size_t i = 0; i < local_ml_count && local_ml_found < 128; i++) {
            ml_handle_t *ml = tefkernel_get_ml_by_index(i);
            if (!ml) continue;
            const ml_info_t *info = ml->ml_entry->info;
            if (!info) continue;
            strncpy(local_mls[local_ml_found].id, info->pkg_id, 127);
            local_mls[local_ml_found].id[127] = '\0';
            local_mls[local_ml_found].version = info->version_code;
            local_mls[local_ml_found].matched = false;
            local_ml_found++;
        }

        for (uint16_t i = 0; i < remote_ml_count && offset < decompressed_size && remote_ml_found < 128; i++) {
            if (offset + 1 > decompressed_size) break;
            uint8_t id_len = decompressed[offset++];
            if (offset + id_len + 4 > decompressed_size) break;
            char id[128];
            if (id_len >= 128) id_len = 127;
            memcpy(id, decompressed + offset, id_len);
            id[id_len] = '\0';
            offset += id_len;
            const int version = (decompressed[offset] << 24) | (decompressed[offset + 1] << 16) |
                                (decompressed[offset + 2] << 8) | decompressed[offset + 3];
            offset += 4;
            strncpy(remote_mls[remote_ml_found].id, id, 127);
            remote_mls[remote_ml_found].id[127] = '\0';
            remote_mls[remote_ml_found].version = version;
            remote_mls[remote_ml_found].matched = false;
            remote_ml_found++;
        }

        if (local_ml_found != remote_ml_found) {
            snprintf(ml_errors + strlen(ml_errors), sizeof(ml_errors) - strlen(ml_errors),
                     "count: %zu vs %zu\n", local_ml_found, remote_ml_found);
            ml_error_count++;
            total_errors++;
        }

        for (size_t i = 0; i < local_ml_found && ml_error_count < MAX_DISPLAY; i++) {
            bool found = false;
            for (size_t j = 0; j < remote_ml_found; j++) {
                if (strcmp(local_mls[i].id, remote_mls[j].id) == 0) {
                    found = true;
                    remote_mls[j].matched = true;
                    if (local_mls[i].version != remote_mls[j].version) {
                        snprintf(ml_errors + strlen(ml_errors), sizeof(ml_errors) - strlen(ml_errors),
                                 "%s : v%d != v%d\n", local_mls[i].id,
                                 local_mls[i].version, remote_mls[j].version);
                        ml_error_count++;
                        total_errors++;
                    }
                    break;
                }
            }
            if (!found) {
                snprintf(ml_errors + strlen(ml_errors), sizeof(ml_errors) - strlen(ml_errors),
                         "%s : missing (v%d)\n", local_mls[i].id, local_mls[i].version);
                ml_error_count++;
                total_errors++;
            }
        }

        for (size_t i = 0; i < remote_ml_found && ml_error_count < MAX_DISPLAY; i++) {
            if (!remote_mls[i].matched) {
                snprintf(ml_errors + strlen(ml_errors), sizeof(ml_errors) - strlen(ml_errors),
                         "%s : extra (v%d)\n", remote_mls[i].id, remote_mls[i].version);
                ml_error_count++;
                total_errors++;
            }
        }

        // ============================================================
        // 3. 比较 Mod（按 ModLoader 分组）
        // ============================================================
        if (offset + 2 <= decompressed_size) {
            const uint16_t remote_mod_count = (decompressed[offset] << 8) | decompressed[offset + 1];
            offset += 2;
            const size_t local_mod_count = tefkernel_get_mod_count();

            local_mods = calloc(512, sizeof(mod_entry_t));
            size_t local_mod_found = 0;

            for (size_t i = 0; i < local_mod_count && local_mod_found < 512; i++) {
                const mod_handle_t *mod = tefkernel_get_mod_by_index(i);
                if (!mod || !mod->mod_id || !mod->owner_ml) continue;
                strncpy(local_mods[local_mod_found].id, mod->mod_id, 127);
                local_mods[local_mod_found].id[127] = '\0';
                const multiplayer_mod_info_t *mod_info = mod->owner_ml->ml_entry->ops->get_multiplayer_info(mod->manifest);
                local_mods[local_mod_found].version = mod_info ? mod_info->version_code : 0;
                // 获取 ModLoader ID
                const ml_info_t *ml_info = mod->owner_ml->ml_entry->info;
                if (ml_info) {
                    strncpy(local_mods[local_mod_found].ml_id, ml_info->pkg_id, 127);
                } else {
                    strcpy(local_mods[local_mod_found].ml_id, "unknown");
                }
                local_mods[local_mod_found].ml_id[127] = '\0';
                local_mods[local_mod_found].matched = false;
                local_mod_found++;
            }

            remote_mods = calloc(512, sizeof(mod_entry_t));
            size_t remote_mod_found = 0;

            for (uint16_t i = 0; i < remote_mod_count && offset < decompressed_size && remote_mod_found < 512; i++) {
                if (offset + 1 > decompressed_size) break;
                uint8_t id_len = decompressed[offset++];
                if (offset + id_len + 4 > decompressed_size) break;
                char id[128];
                if (id_len >= 128) id_len = 127;
                memcpy(id, decompressed + offset, id_len);
                id[id_len] = '\0';
                offset += id_len;
                const int version = (decompressed[offset] << 24) | (decompressed[offset + 1] << 16) |
                                    (decompressed[offset + 2] << 8) | decompressed[offset + 3];
                offset += 4;
                strncpy(remote_mods[remote_mod_found].id, id, 127);
                remote_mods[remote_mod_found].id[127] = '\0';
                remote_mods[remote_mod_found].version = version;
                strcpy(remote_mods[remote_mod_found].ml_id, "?"); // 远程数据没有 ModLoader 信息
                remote_mods[remote_mod_found].matched = false;
                remote_mod_found++;
            }

            // 按 ModLoader 分组收集错误
            groups = calloc(64, sizeof(ml_group_t));
            int group_count = 0;
            if (!local_mls || !remote_mls || !local_mods || !remote_mods || !groups) {
                free(local_modules);
                free(remote_modules);
                free(local_mls);
                free(remote_mls);
                free(local_mods);
                free(remote_mods);
                free(groups);
    free(local_modules);
    free(remote_modules);
    free(local_mls);
    free(remote_mls);
    free(local_mods);
    free(remote_mods);
    free(groups);
    free(decompressed);
                snprintf(error_msg, error_msg_size, "Memory allocation failed");
                return false;
            }

            // 检查本地 Mod（缺失或版本不匹配）
            for (size_t i = 0; i < local_mod_found; i++) {
                bool found = false;
                for (size_t j = 0; j < remote_mod_found; j++) {
                    if (strcmp(local_mods[i].id, remote_mods[j].id) == 0) {
                        found = true;
                        remote_mods[j].matched = true;
                        if (local_mods[i].version != remote_mods[j].version) {
                            int gidx = -1;
                            for (int k = 0; k < group_count; k++) {
                                if (strcmp(groups[k].ml_id, local_mods[i].ml_id) == 0) {
                                    gidx = k;
                                    break;
                                }
                            }
                            if (gidx == -1 && group_count < 64) {
                                gidx = group_count;
                                strncpy(groups[gidx].ml_id, local_mods[i].ml_id, 127);
                                groups[gidx].ml_id[127] = '\0';
                                groups[gidx].errors[0] = '\0';
                                groups[gidx].count = 0;
                                group_count++;
                            }
                            if (gidx != -1 && groups[gidx].count < MAX_DISPLAY) {
                                snprintf(groups[gidx].errors + strlen(groups[gidx].errors),
                                         sizeof(groups[gidx].errors) - strlen(groups[gidx].errors),
                                         "%s : v%d != v%d\n", local_mods[i].id,
                                         local_mods[i].version, remote_mods[j].version);
                                groups[gidx].count++;
                                total_errors++;
                            }
                        }
                        break;
                    }
                }
                if (!found) {
                    int gidx = -1;
                    for (int k = 0; k < group_count; k++) {
                        if (strcmp(groups[k].ml_id, local_mods[i].ml_id) == 0) {
                            gidx = k;
                            break;
                        }
                    }
                    if (gidx == -1 && group_count < 64) {
                        gidx = group_count;
                        strncpy(groups[gidx].ml_id, local_mods[i].ml_id, 127);
                        groups[gidx].ml_id[127] = '\0';
                        groups[gidx].errors[0] = '\0';
                        groups[gidx].count = 0;
                        group_count++;
                    }
                    if (gidx != -1 && groups[gidx].count < MAX_DISPLAY) {
                        snprintf(groups[gidx].errors + strlen(groups[gidx].errors),
                                 sizeof(groups[gidx].errors) - strlen(groups[gidx].errors),
                                 "%s : missing (v%d)\n", local_mods[i].id, local_mods[i].version);
                        groups[gidx].count++;
                        total_errors++;
                    }
                }
            }

            // 检查远程多余的 Mod（放到 "?" 组）
            for (size_t i = 0; i < remote_mod_found; i++) {
                if (!remote_mods[i].matched) {
                    int gidx = -1;
                    for (int k = 0; k < group_count; k++) {
                        if (strcmp(groups[k].ml_id, "?") == 0) {
                            gidx = k;
                            break;
                        }
                    }
                    if (gidx == -1 && group_count < 64) {
                        gidx = group_count;
                        strcpy(groups[gidx].ml_id, "?");
                        groups[gidx].errors[0] = '\0';
                        groups[gidx].count = 0;
                        group_count++;
                    }
                    if (gidx != -1 && groups[gidx].count < MAX_DISPLAY) {
                        snprintf(groups[gidx].errors + strlen(groups[gidx].errors),
                                 sizeof(groups[gidx].errors) - strlen(groups[gidx].errors),
                                 "%s : extra (v%d)\n", remote_mods[i].id, remote_mods[i].version);
                        groups[gidx].count++;
                        total_errors++;
                    }
                }
            }

            // 构建最终错误信息
            snprintf(mismatch_details, sizeof(mismatch_details),
                     "%d error(s)\n", total_errors);

            if (module_error_count > 0) {
                strncat(mismatch_details, "[Module] ", sizeof(mismatch_details) - strlen(mismatch_details) - 1);
                strncat(mismatch_details, module_errors, sizeof(mismatch_details) - strlen(mismatch_details) - 1);
            }

            if (ml_error_count > 0) {
                strncat(mismatch_details, "[ModLoader] ", sizeof(mismatch_details) - strlen(mismatch_details) - 1);
                strncat(mismatch_details, ml_errors, sizeof(mismatch_details) - strlen(mismatch_details) - 1);
            }

            // 按顺序输出每个 ModLoader 的错误
            for (int i = 0; i < group_count; i++) {
                if (groups[i].count > 0) {
                    char header[256];
                    snprintf(header, sizeof(header), "[%s] ", groups[i].ml_id);
                    strncat(mismatch_details, header, sizeof(mismatch_details) - strlen(mismatch_details) - 1);
                    strncat(mismatch_details, groups[i].errors, sizeof(mismatch_details) - strlen(mismatch_details) - 1);
                }
            }

            has_mismatch = (total_errors > 0);
        }
    }

    free(decompressed);

    if (has_mismatch) {
        snprintf(error_msg, error_msg_size, "%s", mismatch_details);
        return false;
    }

    snprintf(error_msg, error_msg_size, "OK");
    return true;
}

static patch_handle_t terraria_netmanager_create_tefkernel_pack() {
    uint8_t *serialized_data = NULL;
    uint32_t serialized_size = 0;

    if (!terraria_netmanager_get_all_versions(&serialized_data, &serialized_size)) {
        TEKLOG_ERROR("Failed to get versions data");
        return NULL;
    }

    const int game_release = terraria_main_get_cur_release();
    const int str_len = strlen(TEFKERNEL_MAGIC_STRING);

    // 包结构: 总长度(2) + 类型(1) + 魔数字符串(长度+内容) + gameRelease(4) + versionCode(8) + 压缩数据
    const uint32_t total_length = 2 + 1 + 1 + str_len + 4 + 8 + serialized_size;

    TEKLOG_INFO("New TEFKernel packet: total=%u bytes (data=%u bytes)", total_length, serialized_size);

    patch_handle_t uint8_type = patchlib_get_basic_type(PATCH_UINT8);
    patch_handle_t new_array = patchlib_array_create(total_length, uint8_type);
    patchlib_free(uint8_type);

    if (!new_array) {
        TEKLOG_ERROR("Failed to create array");
        free(serialized_data);
        return NULL;
    }

    uint8_t *new_array_c = malloc(total_length);
    if (!new_array_c) {
        TEKLOG_ERROR("Failed to allocate memory");
        patchlib_free(new_array);
        free(serialized_data);
        return NULL;
    }

    size_t offset = 0;

    // 总长度 (小端序)
    new_array_c[offset++] = (uint8_t) (total_length & 0xFF);
    new_array_c[offset++] = (uint8_t) ((total_length >> 8) & 0xFF);

    // 包类型
    new_array_c[offset++] = 0x01;

    // 魔数字符串
    new_array_c[offset++] = (uint8_t) str_len;
    memcpy(&new_array_c[offset], TEFKERNEL_MAGIC_STRING, str_len);
    offset += str_len;

    // gameRelease
    for (int i = 0; i < 4; i++) {
        new_array_c[offset++] = (uint8_t) ((game_release >> (i * 8)) & 0xFF);
    }

    // versionCode (用于快速检查)
    for (int i = 0; i < 8; i++) {
        new_array_c[offset++] = (uint8_t) ((TEFKERNEL_VERSION_CODE >> (i * 8)) & 0xFF);
    }

    // 压缩数据
    memcpy(&new_array_c[offset], serialized_data, serialized_size);
    offset += serialized_size;

    free(serialized_data);

    patchlib_array_copy_from_c(new_array, new_array_c, total_length);
    free(new_array_c);

    TEKLOG_INFO("TEFKernel packet created successfully (%zu bytes)", offset);
    return new_array;
}

static patch_handle_t terraria_netmanager_create_vanilla_connection_packet(int *out_size) {
    patch_handle_t uint8_type = patchlib_get_basic_type(PATCH_UINT8);
    const int game_release = terraria_main_get_cur_release();

    char vanilla_string[32];
    const int vanilla_str_len = snprintf(vanilla_string, sizeof(vanilla_string), "Terraria%d", game_release);

    if (vanilla_str_len > 255) {
        TEKLOG_ERROR("Vanilla string too long: %d", vanilla_str_len);
        patchlib_free(uint8_type);
        return NULL;
    }

    const int packet_length = 1 + 1 + vanilla_str_len;
    patch_handle_t new_array = patchlib_array_create(packet_length, uint8_type);
    patchlib_free(uint8_type);
    if (!new_array) return NULL;

    uint8_t *buffer = malloc(packet_length);
    if (!buffer) {
        patchlib_free(new_array);
        return NULL;
    }

    int offset = 0;
    buffer[offset++] = 1;
    buffer[offset++] = (uint8_t) vanilla_str_len;
    memcpy(&buffer[offset], vanilla_string, vanilla_str_len);
    offset += vanilla_str_len;

    patchlib_array_copy_from_c(new_array, buffer, packet_length);
    free(buffer);

    if (out_size) *out_size = packet_length;
    return new_array;
}

static patch_handle_t terraria_netmanager_create_null_connection_packet() {
    patch_handle_t uint8_type = patchlib_get_basic_type(PATCH_UINT8);
    const uint16_t total_length = 5;
    const int packet_length = 5;

    patch_handle_t new_array = patchlib_array_create(packet_length, uint8_type);
    patchlib_free(uint8_type);
    if (!new_array) return NULL;

    uint8_t *buffer = malloc(packet_length);
    if (!buffer) {
        patchlib_free(new_array);
        return NULL;
    }

    int offset = 0;
    buffer[offset++] = (uint8_t) (total_length & 0xFF);
    buffer[offset++] = (uint8_t) ((total_length >> 8) & 0xFF);
    buffer[offset++] = 1;
    buffer[offset++] = 1;
    buffer[offset++] = 'a';

    patchlib_array_copy_from_c(new_array, buffer, packet_length);
    free(buffer);

    return new_array;
}

static connection_type_t terraria_netmanager_parse_tefconnection_packet(const uint8_t *data, const int data_len,
                                                                        const uint8_t client) {
    int offset = 1;
    if (offset + 1 >= data_len) {
        terraria_netmanager_client_errors[client] = ERROR_VERSION;
        snprintf(terraria_netmanager_error_details[client], sizeof(terraria_netmanager_error_details[client]),
                 "Invalid packet: too short");
        return CONNECTION_TYPE_VERSION_MISMATCH;
    }

    const uint8_t str_len = data[offset++];
    if (offset + str_len + 4 + 8 > data_len) {
        terraria_netmanager_client_errors[client] = ERROR_VERSION;
        snprintf(terraria_netmanager_error_details[client], sizeof(terraria_netmanager_error_details[client]),
                 "Invalid packet: incomplete data");
        return CONNECTION_TYPE_VERSION_MISMATCH;
    }

    // 检查魔数字符串
    if (str_len != strlen(TEFKERNEL_MAGIC_STRING) ||
        memcmp(&data[offset], TEFKERNEL_MAGIC_STRING, str_len) != 0) {
        terraria_netmanager_client_errors[client] = ERROR_VERSION;
        snprintf(terraria_netmanager_error_details[client], sizeof(terraria_netmanager_error_details[client]),
                 "Invalid magic string");
        return CONNECTION_TYPE_VERSION_MISMATCH;
    }
    offset += str_len;

    // 读取gameRelease (跳过)
    offset += 4;

    // 读取versionCode
    uint64_t version = 0;
    for (int i = 0; i < 8; i++) {
        version |= (uint64_t) data[offset++] << (i * 8);
    }

    if (version != TEFKERNEL_VERSION_CODE) {
        terraria_netmanager_client_errors[client] = ERROR_VERSION;
        snprintf(terraria_netmanager_error_details[client], sizeof(terraria_netmanager_error_details[client]),
                 "Version mismatch: local=%llu, remote=%llu",
                 (unsigned long long) TEFKERNEL_VERSION_CODE, (unsigned long long) version);
        return CONNECTION_TYPE_VERSION_MISMATCH;
    }

    // 解析压缩数据
    const uint32_t compressed_size = data_len - offset;
    if (compressed_size < 4) {
        terraria_netmanager_client_errors[client] = ERROR_VERSION;
        snprintf(terraria_netmanager_error_details[client], sizeof(terraria_netmanager_error_details[client]),
                 "Invalid compressed data");
        return CONNECTION_TYPE_VERSION_MISMATCH;
    }

    char error_msg[512] = {0};
    if (!terraria_netmanager_compare_versions(data + offset, compressed_size, error_msg, sizeof(error_msg))) {
        terraria_netmanager_client_errors[client] = ERROR_MOD_REQUIRED;
        snprintf(terraria_netmanager_error_details[client], sizeof(terraria_netmanager_error_details[client]),
                 "%s", error_msg);
        return CONNECTION_TYPE_BAD_HASH;
    }

    // 所有检查通过
    snprintf(terraria_netmanager_error_details[client], sizeof(terraria_netmanager_error_details[client]),
             "All modules/ModLoaders/Mods match");
    return CONNECTION_TYPE_TEFKERNEL;
}

static bool terraria_netmanager_is_vanilla_connection_packet(const uint8_t *data, const int data_len) {
    int offset = 1;
    if (offset + 1 >= data_len) return false;

    const uint8_t str_len = data[offset++];
    if (offset + str_len > data_len) return false;

    const char *str = (char *) &data[offset];
    const char *terraria_prefix = "Terraria";

    if (str_len < strlen(terraria_prefix)) return false;
    return strncmp(str, terraria_prefix, strlen(terraria_prefix)) == 0;
}

/* ---------------------------------------------------------------- */
/* 联机同步辅助（客户端请求 / 服务端权威执行）                        */
/* ---------------------------------------------------------------- */

static patch_handle_t m_net_send_data11 = PATCH_NULL;
static patch_handle_t m_net_send_tile_square = PATCH_NULL;
static patch_handle_t m_net_send_raw = PATCH_NULL;      /* SendPacket / Telepathy.Send */
static terraria_net_mod_message_cb g_mod_message_cb = NULL;
static patch_handle_t f_net_mode = PATCH_NULL;          /* Main.netMode */

/* Mod 消息载体消息号。
 *
 * 不能用 250 这类“原版未使用”的号：PC ↔ Android 的传输层会在把包交给
 * MessageBuffer 之前丢掉未知消息号（实测手机端只收到 1~154）。
 * 这里挂到 RequestWorldData（6）：客户端→服务端、平时只在加入时发一次，
 * 且负载带 magic，内核在 get_data_hook 里命中才消费，不会影响真正的 6。 */
#define TEFK_MOD_MESSAGE_ID 6

void terraria_net_set_mod_message_handler(terraria_net_mod_message_cb cb) {
    g_mod_message_cb = cb;
}

static int net_mode_now(void) {
    if (!patchlib_is_valid(f_net_mode)) {
        patch_handle_t cls = patchlib_type_get_type("Terraria", "Main");
        if (patchlib_is_valid(cls)) {
            f_net_mode = patchlib_type_get_field(cls, "netMode");
            patchlib_free(cls);
        }
    }
    int mode = 0;
    if (patchlib_is_valid(f_net_mode))
        patchlib_field_get_value(f_net_mode, PATCH_NULL, &mode);
    return mode;
}

/* 把原始字节包发出去（客户端 -> 服务端）。
 * 桌面：NetMessage.SendPacket(byte[] data, int remoteClient)，服务端用 256；
 * Android：Telepathy.Client.Send(byte[] data, int length)。 */
static void net_send_raw_packet(const uint8_t *data, const int len) {
    if (!data || len <= 0)
        return;
    if (!patchlib_is_valid(m_net_send_raw)) {
        patch_handle_t cls = patchlib_type_get_type("Terraria", "NetMessage");
        if (patchlib_is_valid(cls)) {
#if defined(__ANDROID__)
            patch_handle_t t = patchlib_type_get_type("Telepathy", "Client");
            if (patchlib_is_valid(t)) {
                m_net_send_raw = patchlib_type_get_method_by_param_count(t, "Send", 2);
                patchlib_free(t);
            }
#else
            m_net_send_raw = patchlib_type_get_method_by_param_count(cls, "SendPacket", 2);
#endif
            patchlib_free(cls);
        }
    }
    if (!patchlib_is_valid(m_net_send_raw)) {
        TEKLOG_WARN("net: 原始发送方法未解析（SendPacket/Telepathy.Send），包未发出");
        return;
    }

    patch_handle_t uint8_type = patchlib_get_basic_type(PATCH_UINT8);
    if (!patchlib_is_valid(uint8_type))
        return;
    patch_handle_t arr = patchlib_array_create((size_t) len, uint8_type);
    patchlib_free(uint8_type);
    if (!patchlib_is_valid(arr))
        return;
    patchlib_array_copy_from_c(arr, data, (size_t) len);

#if defined(__ANDROID__)
    int length = len;
    void *args[2] = {&arr, &length};
#else
    int remote = 256; /* 256 = 发往服务端 */
    void *args[2] = {&arr, &remote};
#endif
    patchlib_method_invoke_args(m_net_send_raw, PATCH_NULL, NULL, args);
    patchlib_free(arr);
}

void terraria_net_send_mod_message(const char *mod_id, const char *channel,
                                   const void *data, int len) {
    if (!mod_id || !*mod_id || !channel || !*channel)
        return;
    if (net_mode_now() != 1) {
        TEKLOG_WARN("net: mod message 只能在客户端发送 (netMode=%d)", net_mode_now());
        return;
    }
    if (len < 0) len = 0;
    if (len > 8192) len = 8192;

    const size_t magic_len = strlen(TEFKERNEL_MAGIC_STRING);
    const size_t mod_len = strlen(mod_id);
    const size_t ch_len = strlen(channel);
    const size_t total = 2 + 1 + 1 + magic_len + 2 + mod_len + 2 + ch_len + 4 + (size_t) len;

    uint8_t *buf = malloc(total);
    if (!buf) return;
    size_t o = 0;
    buf[o++] = (uint8_t) (total & 0xFF);
    buf[o++] = (uint8_t) ((total >> 8) & 0xFF);
    buf[o++] = TEFK_MOD_MESSAGE_ID;
    buf[o++] = (uint8_t) magic_len;
    memcpy(buf + o, TEFKERNEL_MAGIC_STRING, magic_len); o += magic_len;
    buf[o++] = (uint8_t) (mod_len & 0xFF);
    buf[o++] = (uint8_t) ((mod_len >> 8) & 0xFF);
    memcpy(buf + o, mod_id, mod_len); o += mod_len;
    buf[o++] = (uint8_t) (ch_len & 0xFF);
    buf[o++] = (uint8_t) ((ch_len >> 8) & 0xFF);
    memcpy(buf + o, channel, ch_len); o += ch_len;
    buf[o++] = (uint8_t) (len & 0xFF);
    buf[o++] = (uint8_t) ((len >> 8) & 0xFF);
    buf[o++] = (uint8_t) ((len >> 16) & 0xFF);
    buf[o++] = (uint8_t) ((len >> 24) & 0xFF);
    if (len > 0) memcpy(buf + o, data, (size_t) len);

    TEKLOG_INFO("net: 发送 mod message mod=%s channel=%s len=%d total=%zu",
                mod_id, channel, len, total);
    net_send_raw_packet(buf, (int) total);
    free(buf);
}

static void resolve_net_sync_methods(void) {
    if (patchlib_is_valid(m_net_send_data11) &&
        patchlib_is_valid(m_net_send_tile_square))
        return;
    patch_handle_t cls = patchlib_type_get_type("Terraria", "NetMessage");
    if (!patchlib_is_valid(cls))
        return;
    if (!patchlib_is_valid(m_net_send_data11))
        m_net_send_data11 =
                patchlib_type_get_method_by_param_count(cls, "SendData", 11);
    if (!patchlib_is_valid(m_net_send_tile_square))
        m_net_send_tile_square =
                patchlib_type_get_method_by_param_count(cls, "SendTileSquare", 6);
    patchlib_free(cls);
}

void terraria_net_send_tile_action(int x, int y, int action) {
    resolve_net_sync_methods();
    if (!patchlib_is_valid(m_net_send_data11)) {
        TEKLOG_WARN("net: SendData(11) 未解析，图格请求未发送");
        return;
    }
    if (x < 0 || y < 0) return;

    /* 原版签名：
     * SendData(int msgType, int remoteClient, int ignoreClient, NetworkText text,
     *          int number, float number2, float number3, float number4,
     *          int number5, int number6, int number7) */
    int msg = 17, remote = -1, ignore = -1, number = action;
    patch_handle_t text = PATCH_NULL;
    float n2 = (float) x, n3 = (float) y, n4 = 0.0f;
    int n5 = 0, n6 = 0, n7 = 0;
    void *a[11] = {&msg, &remote, &ignore, &text, &number,
                   &n2, &n3, &n4, &n5, &n6, &n7};
    patchlib_method_invoke_args(m_net_send_data11, PATCH_NULL, NULL, a);
}

void terraria_net_send_tile_square(int x, int y, int width, int height) {
    /* 直接发 MessageID 20（TileSquare）：
     *   SendData(20, remoteClient, ignoreClient, text,
     *            tileX, tileY, xSize, ySize, changeType)
     * 不走 SendTileSquare 的多重重载（by_param_count 在 IL2CPP 上可能匹配不到，
     * 失败还是静默的）。SendData(11 参数) 这条路径已被图格请求验证可用。 */
    resolve_net_sync_methods();
    if (!patchlib_is_valid(m_net_send_data11)) {
        TEKLOG_WARN("net: SendData(11) 未解析，TileSquare 未发送");
        return;
    }
    if (width <= 0 || height <= 0) return;

    {
        static int logged = 0;
        if (logged < 3) {
            logged++;
            TEKLOG_INFO("net: tile_square (%d,%d) %dx%d", x, y, width, height);
        }
    }

    int msg = 20, remote = -1, ignore = -1, number = x;
    patch_handle_t text = PATCH_NULL;
    float n2 = (float) y, n3 = (float) width, n4 = (float) height;
    int n5 = 0 /* TileChangeType.None */, n6 = 0, n7 = 0;
    void *a[11] = {&msg, &remote, &ignore, &text, &number,
                   &n2, &n3, &n4, &n5, &n6, &n7};
    patchlib_method_invoke_args(m_net_send_data11, PATCH_NULL, NULL, a);
}

void terraria_netmanager_init() {
    if (tefkernel_get_module_count() == 0 && tefkernel_get_ml_count() == 0) {
        TEKLOG_INFO("No modules or ModLoaders loaded - version isolation disabled, using vanilla networking logic\n"
                    "TEFKernel version isolation is skipped, all clients can connect without version checking");
        return;
    }

    // 初始化错误信息
    for (int i = 0; i < 256; i++)
        terraria_netmanager_error_details[i][0] = '\0';

    patch_handle_t message_buffer_class = patchlib_type_get_type("Terraria", "MessageBuffer");
    patch_handle_t net_message_class = patchlib_type_get_type("Terraria", "NetMessage");
    patch_handle_t network_text_class = patchlib_type_get_type("Terraria.Localization", "NetworkText");
    patch_handle_t send_data = patchlib_type_get_method(net_message_class, "SendData");
    patch_handle_t send = PATCH_NULL;
    patch_handle_t process_data = PATCH_NULL;
    network_text_from_literal = patchlib_type_get_method_by_param_count(network_text_class, "FromLiteral", 1);
    who_am_i = patchlib_type_get_field(message_buffer_class, "whoAmI");

#if defined(__ANDROID__)
    send = patchlib_type_get_method_by_param_count(patchlib_type_get_type("Telepathy", "Client"), "Send", 2);
    process_data = patchlib_type_get_method_by_param_count(message_buffer_class, "ProcessData", 3);
#else
    send = patchlib_type_get_method(net_message_class, "SendPacket");
    read_buffer = patchlib_type_get_field(message_buffer_class, "readBuffer");
    read_buffer_max = patchlib_type_get_field(message_buffer_class, "readBufferMax");
    process_data = patchlib_type_get_method(message_buffer_class, "GetData");
#endif

    patchlib_install_prepost_hook(send_data, send_data_hook, NULL);
    patchlib_install_prepost_hook(send, send_hook, NULL);
    patchlib_install_prepost_hook(process_data, get_data_hook, NULL);

    patchlib_free(message_buffer_class);
    patchlib_free(net_message_class);
    patchlib_free(send_data);
    patchlib_free(send);
    patchlib_free(process_data);
    patchlib_free(network_text_class);
}

#if __ANDROID__
typedef struct il2cpp_array_t {
    // ReSharper disable once CppDeclaratorNeverUsed
    void *m_class;
    // ReSharper disable once CppDeclaratorNeverUsed
    void *m_monitor;
    // ReSharper disable once CppDeclaratorNeverUsed
    void *m_bounds;
    // ReSharper disable once CppDeclaratorNeverUsed
    uint32_t m_length;
} il2cpp_array_t;
#endif

/* 解析并分发 Mod 消息；data[start] 是消息号，负载从 start+1 开始。
 * 返回 true 表示这是内核的 Mod 消息（已被消费，调用方应跳过原版处理）。 */
static bool handle_mod_message(const uint8_t *data, const int length,
                               const int start, patch_handle_t instance) {
    const uint8_t *p = data + start + 1;
    int remain = length - 1;
    char mod_id[128] = {0};
    char channel[128] = {0};

    if (remain < 1) return false;
    const uint8_t magic_len = *p++; remain -= 1;
    if (magic_len != strlen(TEFKERNEL_MAGIC_STRING)) return false;
    if (remain < (int) magic_len + 2 + 2 + 4) return false;
    if (memcmp(p, TEFKERNEL_MAGIC_STRING, magic_len) != 0) return false;
    p += magic_len; remain -= magic_len;

    const int mod_len = p[0] | (p[1] << 8); p += 2; remain -= 2;
    if (mod_len <= 0 || mod_len >= (int) sizeof(mod_id) || remain < mod_len) return true;
    memcpy(mod_id, p, (size_t) mod_len);
    mod_id[mod_len] = '\0';
    p += mod_len; remain -= mod_len;

    const int ch_len = p[0] | (p[1] << 8); p += 2; remain -= 2;
    if (ch_len <= 0 || ch_len >= (int) sizeof(channel) || remain < ch_len) return true;
    memcpy(channel, p, (size_t) ch_len);
    channel[ch_len] = '\0';
    p += ch_len; remain -= ch_len;

    uint32_t data_len = (uint32_t) p[0] | ((uint32_t) p[1] << 8) |
                        ((uint32_t) p[2] << 16) | ((uint32_t) p[3] << 24);
    p += 4; remain -= 4;
    if (data_len > (uint32_t) remain) data_len = (uint32_t) remain;

    int client = -1;
    if (patchlib_is_valid(who_am_i))
        patchlib_field_get_value(who_am_i, instance, &client);

    TEKLOG_INFO("net: mod message mod=%s channel=%s len=%u client=%d",
                mod_id, channel, data_len, client);
    if (g_mod_message_cb)
        g_mod_message_cb(mod_id, channel, p, (int) data_len, client);
    return true;
}

static bool get_data_hook(patch_handle_t instance, void **args,
                          const patch_method_signature_t *sig_info, void *result) {
    uint8_t msg_type = 0;
#if defined(__ANDROID__)
    uint8_t *buffer = *(patch_handle_t *) args[0] + sizeof(il2cpp_array_t);
    msg_type = buffer[0];
    const int start = 0;
    const int length = *(int *) args[1];
#else
    const int start = *(int *) args[0];
    const int length = *(int *) args[1];
    int buffer_max_size = -1;
    patch_handle_t read_buffer_array = PATCH_NULL;
    patchlib_field_get_value(read_buffer, instance, &read_buffer_array);
    patchlib_field_get_value(read_buffer_max, NULL, &buffer_max_size);
    patchlib_array_at(read_buffer_array, start, &msg_type);
#endif

    /* 诊断：每个消息号第一次出现时记一行，确认包是否到达 */
    {
        static uint8_t seen_ids[256] = {0};
        if (!seen_ids[msg_type]) {
            seen_ids[msg_type] = 1;
            TEKLOG_INFO("net: first packet msg=%u len=%d", (unsigned) msg_type, length);
        }
    }

    /* ---- Mod 消息（服务端权威 RPC）----
     * 载体是原版已知消息号，必须只在 magic 命中时消费；
     * 真正的原版同名消息要继续走原版处理。 */
    if (msg_type == TEFK_MOD_MESSAGE_ID) {
#if defined(__ANDROID__)
        if (handle_mod_message(buffer, length, start, instance))
            return true;
#else
        uint8_t *local_buf = malloc((size_t) (buffer_max_size > 0 ? buffer_max_size
                                                                 : length + 8));
        if (local_buf) {
            patchlib_array_copy_to_c(local_buf, read_buffer_array, buffer_max_size);
            const bool consumed = handle_mod_message(local_buf, length, start, instance);
            free(local_buf);
            if (consumed) return true;
        }
#endif
    }

    if (msg_type == 1) {
#if defined(__ANDROID__)
        {
            char hex[1024] = {0};
            const int n = length;
            for (int i = 0; i < n && (i + 1) * 3 < (int) sizeof(hex); ++i)
                snprintf(hex + i * 3, 4, "%02x ", buffer[start + i]);
            TEKLOG_INFO("ConnectPacket hex (full %d bytes): %s", n, hex);
        }
#endif
#if !defined(__ANDROID__)
        uint8_t *buffer = malloc(buffer_max_size);
        patchlib_array_copy_to_c(buffer, read_buffer_array, buffer_max_size);
#endif

        int client = -1;
        patchlib_field_get_value(who_am_i, instance, &client);

        if (client < 0 || client >= 256) return true;

        terraria_netmanager_client_connections[client] = CONNECTION_TYPE_NONE;
        terraria_netmanager_client_errors[client] = ERROR_NONE;
        terraria_netmanager_error_details[client][0] = '\0';

        terraria_netmanager_client_connections[client] =
                terraria_netmanager_parse_tefconnection_packet(buffer + start, length, client);
        TEKLOG_INFO("GetData msg=1: client=%d start=%d length=%d type=%d", client, start, length,
                    terraria_netmanager_client_connections[client]);

        if (terraria_netmanager_client_connections[client] != CONNECTION_TYPE_NONE) {
            TEKLOG_INFO("GetData: TEFKernel connection packet from client %d, converting to vanilla", client);
            if (terraria_netmanager_client_errors[client] != ERROR_NONE) {
                TEKLOG_WARN("Rejecting client %d: %s", client,
                             terraria_netmanager_error_details[client]);

                // 清理资源
                patchlib_free(read_buffer_array);
#if !defined(__ANDROID__)
                // buffer 仅在桌面端为 malloc 内存；Android 上指向 il2cpp GC 数组内部，不可 free
                free(buffer);
#endif

                // 返回 false 让原始方法继续执行
                // 这会导致连接包不被识别，客户端收到错误而断开
                return false;
            }

#if __ANDROID__
            patch_handle_t vanilla_array = terraria_netmanager_create_vanilla_connection_packet(args[1]);
            *(patch_handle_t *) args[0] = vanilla_array;
#else
            patch_handle_t vanilla_array = terraria_netmanager_create_vanilla_connection_packet(args[1]);
            for (int i = 0; i < *(int *) args[1]; ++i) {
                uint8_t byte = 0;
                patchlib_array_at(vanilla_array, i, &byte);
                patchlib_array_set(read_buffer_array, start + i, &byte);
            }
            patchlib_free(vanilla_array);
            patchlib_free(read_buffer_array);
#if !defined(__ANDROID__)
            free(buffer);
#endif
#endif
            return false;
        }

        if (terraria_netmanager_is_vanilla_connection_packet(buffer + start, length)) {
            terraria_netmanager_client_connections[client] = CONNECTION_TYPE_VANILLA;
            terraria_netmanager_client_errors[client] = ERROR_MOD_REQUIRED;
            snprintf(terraria_netmanager_error_details[client],
                     sizeof(terraria_netmanager_error_details[client]),
                     "Vanilla client detected - TEFKernel required");
            TEKLOG_INFO("Received vanilla connection packet from client %d", client);

            patch_handle_t array_null = terraria_netmanager_create_null_connection_packet();
#if __ANDROID__
            *(patch_handle_t *) args[0] = array_null;
            *(int *) args[1] = 5;
#else
            *(int *) args[1] = 5;
            for (int i = 0; i < 5; ++i) {
                uint8_t byte = 0;
                patchlib_array_at(array_null, i, &byte);
                patchlib_array_set(read_buffer_array, start + i, &byte);
            }
            patchlib_free(array_null);
            patchlib_free(read_buffer_array);
#if !defined(__ANDROID__)
            free(buffer);
#endif
#endif
            return false;
        }
    }
    return false;
}

static bool send_data_hook(patch_handle_t instance, void **args,
                           const patch_method_signature_t *sig_info, void *result) {
    if (!args[3]) return false;

    const int msg_type = *(int *) args[0];
    const int remote_client = *(int *) args[1];
    patch_handle_t error_text = *(patch_handle_t *) args[3];

    if (msg_type == 2 && remote_client >= 0 && remote_client < 256) {
        TEKLOG_DEBUG("SendData: msgType=2, g_error_message_id=%d, details=%s",
                     terraria_netmanager_client_errors[remote_client],
                     terraria_netmanager_error_details[remote_client]);

        if (terraria_netmanager_client_errors[remote_client] == ERROR_MOD_REQUIRED ||
            terraria_netmanager_client_errors[remote_client] == ERROR_VERSION) {

            /* ⚠ 不要 free 回调参数句柄：加载器在 CleanupContext 里会统一释放，
             * 再 free 一次就是 double free（桌面 GCHandle → mono_gchandle 崩溃）。 */

            char error_msg[512];
            const char *error_type = terraria_netmanager_client_errors[remote_client] == ERROR_VERSION
                                         ? "Version mismatch"
                                         : "Mod mismatch";
            snprintf(error_msg, sizeof(error_msg), "[TEFKernel] %s: %s",
                     error_type, terraria_netmanager_error_details[remote_client]);

            patch_handle_t error_text_str = patchlib_string_create(error_msg);
            void *iargs[1] = {&error_text_str};
            patchlib_method_invoke_args(network_text_from_literal, PATCH_NULL, &error_text, iargs);
        }

        if (error_text) {
            *(patch_handle_t *) args[3] = error_text;
        }
    }
    return false;
}

static bool send_hook(patch_handle_t instance, void **args,
                      const patch_method_signature_t *sig_info, void *result) {
    patch_handle_t array = *(patch_handle_t *) args[0];
    uint8_t pack_id = 0;
    patchlib_array_at(array, 2, &pack_id);

    if (pack_id == 1) {
        /* 同上：args[0] 是回调参数句柄，不在这里释放。 */
        patch_handle_t pack = terraria_netmanager_create_tefkernel_pack();
        *(patch_handle_t *) args[0] = pack;
#if defined(__ANDROID__)
        *(int *) args[1] = (int) patchlib_array_length(pack);
#endif
    }
    return false;
}
