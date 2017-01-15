#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "746FlashSim.h"

static FILE *log_file_stream;
static char log_file_path[255];

int main(int argc, char *argv[]) {
    if (argc != 3) {
        printf("usage: test_1_2 <config_file_name> <log_file_path>\n");
        exit(EXIT_FAILURE);
    }
    int ret = 1;
    strcpy(log_file_path, argv[2]);
    log_file_stream = fopen(log_file_path, "w+");
    assert(log_file_stream != NULL);

    fprintf(log_file_stream, "------------------------------------------------------------\n");

    int r;
    FlashSimTest test(argv[1]);
    const TEST_PAGE_TYPE page_value1 = 15712;
    const TEST_PAGE_TYPE page_value2 = 18712;
    TEST_PAGE_TYPE page_value;

    r = test.Write(log_file_stream, 0, page_value1);
    if (r != 1) goto failed;
    r = test.Read(log_file_stream, 0, &page_value);
    if (r != 1) goto failed;

    if (page_value != page_value1) {
        fprintf(log_file_stream, "Reading LBA 0 does not get the right value\n");
        goto failed;
    }

    r = test.Write(log_file_stream, 1, page_value2);
    if (r != 1) goto failed;
    r = test.Read(log_file_stream, 1, &page_value);
    if (r != 1) goto failed;

    if (page_value != page_value2) {
        fprintf(log_file_stream, "Reading LBA 1 does not get the right value\n");
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
