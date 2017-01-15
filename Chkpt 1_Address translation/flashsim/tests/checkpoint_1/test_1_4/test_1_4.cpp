#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#define SSD_SIZE 4
#define PACKAGE_SIZE 8
#define DIE_SIZE 2
#define PLANE_SIZE 10
#define BLOCK_SIZE 16
#define OVERPROVISIONING 0.05
#include "746FlashSim.h"

static FILE *log_file_stream;
static char log_file_path[255];

int main(int argc, char *argv[]) {
    if (argc != 3) {
        printf("usage: test_1_4 <config_file_name> <log_file_path>\n");
        exit(EXIT_FAILURE);
    }
    int ret = 1;
    strcpy(log_file_path, argv[2]);
    log_file_stream = fopen(log_file_path, "w+");
    assert(log_file_stream != NULL);

    fprintf(log_file_stream, "------------------------------------------------------------\n");
    const size_t hard_limit = SSD_SIZE * PACKAGE_SIZE * DIE_SIZE * PLANE_SIZE * BLOCK_SIZE;
    const size_t limit = hard_limit - (OVERPROVISIONING * hard_limit);
    const size_t start = 0;

    int r;
    FlashSimTest test(argv[1]);
    r = test.Write(log_file_stream, start + 0, rand() % 15213);
    if (r != 1) goto failed;
    r = test.Write(log_file_stream, limit - 1, rand() % 15213);
    if (r != 1) goto failed;

    r = test.Write(log_file_stream, hard_limit - 1, 0);
    if (r != 0) {
        if (r == 1) fprintf(log_file_stream, "NO overprovisioning ???\n");
        goto failed;
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
