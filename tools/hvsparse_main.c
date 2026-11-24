/* See host/hvsparse.h for the sparse disk format. */
#include <stdio.h>
#include <string.h>

#include "hvsparse.h"

int main(int argc, char **argv) {
    if (argc == 4 && !strcmp(argv[1], "pack")) return hv_sparse_pack(argv[2], argv[3]) ? 1 : 0;
    if (argc == 4 && !strcmp(argv[1], "unpack")) return hv_sparse_unpack(argv[2], argv[3], 0, NULL, NULL) ? 1 : 0;
    if (argc == 4 && !strcmp(argv[1], "copy")) return hv_sparse_copy_raw(argv[2], argv[3]) ? 1 : 0;
    if (argc == 3 && !strcmp(argv[1], "validate")) return hv_sparse_validate(argv[2], 0) ? 1 : 0;
    fprintf(stderr, "usage: hvsparse pack|unpack|copy SRC DST, or hvsparse validate SRC\n");
    return 2;
}
