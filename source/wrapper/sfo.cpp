#include <cstring>
#include "sfo.h"

static constexpr uint32_t SFO_MAGIC = 0x46535000;

static bool RangeInside(size_t size, uint64_t offset, uint64_t length)
{
    return offset <= size && length <= size - offset;
}

static const char* BoundedCString(const char* buffer, size_t size, uint64_t offset)
{
    if (!RangeInside(size, offset, 1))
        return nullptr;

    const char* value = buffer + offset;
    for (size_t i = offset; i < size; i++)
    {
        if (buffer[i] == '\0')
            return value;
    }

    return nullptr;
}

namespace SFO {
    const char* GetString(const char* buffer, size_t size, const char *name)
    {
        if (size < sizeof(SfoHeader))
            return nullptr;

        const SfoHeader* header = reinterpret_cast<const SfoHeader*>(buffer);
        const SfoEntry* entries =
                reinterpret_cast<const SfoEntry*>(buffer + sizeof(SfoHeader));

        if (header->magic != SFO_MAGIC)
            return nullptr;

        if (size < sizeof(SfoHeader) + header->count * sizeof(SfoEntry))
            return nullptr;

        if (!RangeInside(size, header->keyofs, 1) || !RangeInside(size, header->valofs, 1))
            return nullptr;

        for (uint32_t i = 0; i < header->count; i++) {
            const char* key = BoundedCString(buffer, size, (uint64_t)header->keyofs + entries[i].nameofs);
            if (key == nullptr)
                continue;

            if (strcmp(key, name) == 0) {
                const char* value = BoundedCString(buffer, size, (uint64_t)header->valofs + entries[i].dataofs);
                return value;
            }
        }
        
        return {};
    }

    std::map<std::string, std::string> GetParams(const char* buffer, size_t size)
    {
        std::map<std::string, std::string> out;

        if (size < sizeof(SfoHeader))
            return out;

        const SfoHeader* header = reinterpret_cast<const SfoHeader*>(buffer);
        const SfoEntry* entries =
                reinterpret_cast<const SfoEntry*>(buffer + sizeof(SfoHeader));

        if (header->magic != SFO_MAGIC)
            return out;

        if (size < sizeof(SfoHeader) + header->count * sizeof(SfoEntry))
            return out;

        if (!RangeInside(size, header->keyofs, 1) || !RangeInside(size, header->valofs, 1))
            return out;

        for (uint32_t i = 0; i < header->count; i++) {
            const char* key = BoundedCString(buffer, size, (uint64_t)header->keyofs + entries[i].nameofs);
            if (key == nullptr)
                continue;

            if (entries[i].type == 2)
            {
                const char* value = BoundedCString(buffer, size, (uint64_t)header->valofs + entries[i].dataofs);
                if (value == nullptr)
                    continue;
                out.insert(std::make_pair(key, value));
            }
            else
            {
                uint64_t value_offset = (uint64_t)header->valofs + entries[i].dataofs;
                if (!RangeInside(size, value_offset, sizeof(uint32_t)))
                    continue;

                uint32_t value;
                memcpy(&value, buffer + value_offset, sizeof(value));
                out.insert(std::make_pair(key, std::to_string(value)));
            }
        }

        return out;
    }
}
