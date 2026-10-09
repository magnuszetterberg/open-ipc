// The repo's test keys (keys/drone.key and keys/gs.key, from the password "change-me").
#pragma once
#include <stdint.h>
#include <stdio.h>

static inline bool read_key(const char *name, uint8_t *key, size_t size)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", KEYS_DIR, name);
    FILE *fp = fopen(path, "rb");
    if (fp == NULL)
    {
        return false;
    }
    bool ok = fread(key, size, 1, fp) == 1;
    fclose(fp);
    return ok;
}
