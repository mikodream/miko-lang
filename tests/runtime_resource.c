#include "miko_runtime.h"

#include <stdio.h>
#include <string.h>

int main(void) {
    const char* path = "runtime-resource-test.tmp";
    MikoFile* file = miko_file_open(path, "wb");
    if (file == NULL || !miko_file_is_open(file)) {
        return 1;
    }

    const char payload[] = "miko resource lifecycle\n";
    if (miko_file_write(file, payload, sizeof(payload) - 1) != sizeof(payload) - 1) {
        return 2;
    }
    if (miko_file_close(file) != 1 || miko_file_is_open(file)) {
        return 3;
    }
    if (miko_file_close(file) != 0) {
        return 4;
    }

    file = miko_file_open(path, "rb");
    if (file == NULL) {
        return 5;
    }
    char buffer[64] = {0};
    size_t count = miko_file_read(file, buffer, sizeof(buffer) - 1);
    if (count != sizeof(payload) - 1 || strcmp(buffer, payload) != 0) {
        return 6;
    }
    if (miko_file_close(file) != 1) {
        return 7;
    }

    if (miko_file_open("this-file-does-not-exist", "rb") != NULL) {
        return 8;
    }
    remove(path);
    puts("runtime-resource: pass");
    return 0;
}
