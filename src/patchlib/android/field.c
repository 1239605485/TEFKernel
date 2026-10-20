/*******************************************************************************
 * tefkernel - field
 * Copyright (C) 2025 eternalfuture-e38299
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
 * Created: 2025/11/23
 *******************************************************************************/

#include "patchlib/field.h"

#include <string.h>

#include "internal/log.h"
#include "../il2cpp_api.h"

bool patchlib_field_is_thread_static(patch_handle_t field) {
    if (!patchlib_is_valid(field)) {
        TEKLOG_WARN("Invalid field handle");
        return false;
    }

    return il2cpp_field_get_offset(field) == -1;
}

bool patchlib_field_is_static(patch_handle_t field) {
    if (!patchlib_is_valid(field)) {
        TEKLOG_WARN("Invalid field handle");
        return false;
    }

    return (*(int*)((uintptr_t)il2cpp_field_get_type(field) + sizeof(void*)) & 0x0010) != 0;
}

bool patchlib_field_is_const(patch_handle_t field) {
    if (!patchlib_is_valid(field)) {
        TEKLOG_WARN("Invalid field handle");
        return false;
    }

    return (*(int*)((uintptr_t)il2cpp_field_get_type(field) + sizeof(void*)) & 0x0040) != 0;
}

void patchlib_field_get_value(patch_handle_t field, patch_handle_t instance, void *value) {
    if (!patchlib_is_valid(field)) {
        TEKLOG_ERROR("Invalid field handle");
        return;
    }

    if (!value) {
        TEKLOG_ERROR("Value pointer is NULL");
        return;
    }

    if (patchlib_field_is_const(field) || patchlib_field_is_thread_static(field)) {
        il2cpp_field_static_get_value(field, value);
        return;
    }

    const size_t field_size = patchlib_field_get_size(field);
    void* field_pointer = patchlib_field_get_pointer(field, instance);

    if (!field_pointer) {
        TEKLOG_ERROR("Failed to get field pointer");
        return;
    }

    memcpy(value, field_pointer, field_size);
}

void patchlib_field_set_value(patch_handle_t field, patch_handle_t instance, void *value) {
    if (!patchlib_is_valid(field)) {
        TEKLOG_ERROR("Invalid field handle");
        return;
    }

    if (!value) {
        TEKLOG_ERROR("Value pointer is NULL");
        return;
    }

    const size_t field_size = patchlib_field_get_size(field);
    void* field_pointer = patchlib_field_get_pointer(field, instance);

    if (!field_pointer) {
        TEKLOG_ERROR("Failed to get field pointer");
        return;
    }

    memcpy(field_pointer, value, field_size);
}

void * patchlib_field_get_pointer(patch_handle_t field, void *instance) {
    if (!patchlib_is_valid(field)) {
        TEKLOG_ERROR("Invalid field handle");
        return NULL;
    }

    if (patchlib_field_is_const(field)) {
        TEKLOG_WARN("Cannot get pointer to const field");
        return NULL;
    }

    if (patchlib_field_is_thread_static(field)) {
        TEKLOG_WARN("Thread static fields not supported");
        return NULL;
    }

    const size_t offset = il2cpp_field_get_offset(field);
    const void* p_class = il2cpp_field_get_parent(field);

    if (patchlib_field_is_static(field)) {
        void* staticData = il2cpp_class_get_static_field_data(p_class);
        if (!staticData) {
            TEKLOG_ERROR("Failed to get static field data");
            return NULL;
        }

        return (void*)((uintptr_t)staticData + offset);
    }

    /* 实例字段必须给 instance。否则下面会算出 NULL + offset（例如 0x12f），
     * 调用方再 memcpy 就是 SIGSEGV，而且故障地址看起来像随机数。
     * 这里直接返回 NULL，由 patchlib_field_get_value/set_value 统一处理。 */
    if (!instance) {
        const char *fname = patchlib_field_get_name(field);
        TEKLOG_ERROR("Instance is NULL for non-static field '%s'", fname ? fname : "?");
        return NULL;
    }

    /* 旧代码把 il2cpp_field_get_parent() 返回的 Il2CppClass* 直接传给
     * il2cpp_type_get_type()，读的是类对象里的 gc_desc 字节；约 1/256 的类
     * 会随机命中 IL2CPP_TYPE_VALUETYPE，导致该类的所有实例字段地址偏移 -16。
     * 内核拿到的 instance 都是类对象指针（不是装箱结构体），这里不做修正。 */
    return (void*)((uintptr_t)instance + offset);
}