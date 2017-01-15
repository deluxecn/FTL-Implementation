#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#define BLOCK_SIZE 512
#define BLOCK_ERASES 20
#include "746FlashSim.h"

static FILE *log_file_stream;
static char log_file_path[255];

int main(int argc, char *argv[]) {
    if (argc != 3) {
        printf("usage: test_2_4 <config_file_name> <log_file_path>\n");
        exit(EXIT_FAILURE);
    }
    int ret = 1;
    strcpy(log_file_path, argv[2]);
    log_file_stream = fopen(log_file_path, "w+");
    assert(log_file_stream != NULL);

    fprintf(log_file_stream, "------------------------------------------------------------\n");

    int r;
    FlashSimTest test(argv[1]);
    for (int i = 0; i < 2 + BLOCK_ERASES; i++) {
        for (ssize_t addr = BLOCK_SIZE - 1; addr >= 0; addr--) {
            uint64_t num_erases = test.TotalErasesPerformed();
            r = test.Write(log_file_stream, addr, (addr << i));
            if (r != 1) goto failed;
            if (i >= 2 && addr == BLOCK_SIZE - 1) {
                if (test.TotalErasesPerformed() - num_erases < 2 ||
                        test.TotalErasesPerformed() - num_erases > 3) {
                    fprintf(log_file_stream, "Too less or too many erases\n");
                    goto failed;
                }
            }
            TEST_PAGE_TYPE page_value;
            r = test.Read(log_file_stream, addr, &page_value);
            if (r != 1) goto failed;
            if (page_value != (addr << i)) {
                fprintf(log_file_stream, "Reading LBA %zu does not get the right value\n", addr);
                goto failed;
            }
        }
    }

    for (int i = 0; i < 2; i++) {
        r = test.Write(log_file_stream, 0, 0);
        if (r != 0) {
            fprintf(log_file_stream, "Breaking erasure limit\n");
            goto failed;
        }

        for (ssize_t addr = BLOCK_SIZE - 1; addr >= 0; addr--) {
            TEST_PAGE_TYPE page_value;
            r = test.Read(log_file_stream, addr, &page_value);
            if (r != 1) goto failed;
            if (page_value != (addr << (1 + BLOCK_ERASES))) {
                fprintf(log_file_stream, "Reading LBA %zu does not get the right value\n", addr);
                goto failed;
            }
        }
    }

    ret = 0;
    printf("SUCCESS ...Check %s for more details.\n", log_file_path);
    goto done;
failed:
    printf("FAILED ...Check %s for more details.\n", log_file_path);
done:
    fflush(log_file_stream);
    fclose(log_file_stream);
    return ret;
}
