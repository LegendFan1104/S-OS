#define USER
#include "types.h"
#include "user.h"
#include "fs/stat.h"

#define STDOUT 1

#define handle_error(msg)                                                                                              \
    do {                                                                                                               \
        printf("%s\n", msg);                                                                                           \
        exit(-1);                                                                                                      \
    } while (0)

#define BUF_SIZE 4096

int main(int argc, char *argv[]) {
    int fd, nread;
    // char buf[BUF_SIZE];
    char *buf = (char *) malloc(BUF_SIZE);
    struct linux_dirent64 *d;
    int bpos;
    unsigned char d_type;

    // printf("hello ls; %d\n",argc);
    fd = openat(AT_FDCWD, argc > 1 ? argv[1] : ".", O_RDONLY | O_DIRECTORY, 0600);
    if (fd == -1) {
        handle_error("getdents");
    }

    for (;;) {
        nread = getdents64(fd, (struct linux_dirent64 *) buf, BUF_SIZE);
        if (nread == -1)
            // handle_error("getdents");
            break;

        if (nread == 0)
            break;

        // printf("--------------- nread=%d ---------------\n", nread);
        // printf("inode#    file type  d_reclen  d_off   d_name\n");
        int ctr = 0;
        for (bpos = 0; bpos < nread;) {
            d = (struct linux_dirent64 *) (buf + bpos);
            d_type = d->d_type;

            switch (d_type) {
                case T_DIR:
                    printf("\033[34;1m%s\t\033[0m", d->d_name);
                    break;
                case T_CHR:
                    printf("\033[38;5;214m%s\t\033[0m", d->d_name);
                    break;
                default:
                    printf("%s\t", d->d_name);
                    break;
            }

            bpos += d->d_reclen;
            if (++ctr == 6) {
                write(STDOUT, "\n", 1);
                ctr = 0;
            }

            // break;
        }
        write(STDOUT, "\n", 1);
        // write(1, argv[i], strlen(argv[i]));
    }
    free(buf);
    return 0;
    exit(0);
}
