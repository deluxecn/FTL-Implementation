#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "746FlashSim.h"

static FILE *log_file_stream;
static char log_file_path[255];

int main(int argc, char *argv[]) {
    if (argc != 3) {
        printf("usage: test_1_5 <config_file_name> <log_file_path>\n");
        exit(EXIT_FAILURE);
    }
    int ret = 1;
    strcpy(log_file_path, argv[2]);
    log_file_stream = fopen(log_file_path, "w+");
    assert(log_file_stream != NULL);

    fprintf(log_file_stream, "------------------------------------------------------------\n");

    int r;
    FlashSimTest test(argv[1]);
    TEST_PAGE_TYPE ignored;
    r = test.Read(log_file_stream, 0, &ignored);
    if (r != 0) goto failed;

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
