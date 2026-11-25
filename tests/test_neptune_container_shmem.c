/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <assert.h>
#include <dirent.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include "anon_file.h"
int main(int argc,char **argv) {
    assert(argc==2);
    assert(setenv("WINIOS_NEPTUNE_SHM_DIR",argv[1],1)==0);
    int fd=os_create_anonymous_file(16384,"proxy-ctx");
    assert(fd>=0 && (fcntl(fd,F_GETFD)&FD_CLOEXEC));
    struct stat st;
    assert(fstat(fd,&st)==0 && st.st_size==16384 && st.st_nlink==0);
    uint32_t *a=mmap(NULL,16384,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);
    uint32_t *b=mmap(NULL,16384,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);
    assert(a!=MAP_FAILED && b!=MAP_FAILED);
    a[0]=0xabcdef01u;
    assert(b[0]==0xabcdef01u);
    assert(munmap(a,16384)==0 && munmap(b,16384)==0 && close(fd)==0);
    assert(setenv("WINIOS_NEPTUNE_SHM_DIR","relative-not-allowed",1)==0);
    assert(os_create_anonymous_file(16384,"proxy-ctx")<0);
    return 0;
}
