#include "kernel/param.h"
#include "kernel/types.h"
#include "user/user.h"

#define STD_IN 0
#define STD_ERR 2
#define BUF_SIZE 1024

void __exec(char *cmd, char *cmd_arg[]) {
    if (fork() == 0) {
        exec(cmd, cmd_arg);

        exit(0);
    }
}

void xargs(int argc, char *argv[]) {
    char c, *p, buf[BUF_SIZE], *cmd_arg[MAXARG];

    memset(buf, 0, BUF_SIZE);
    memset(cmd_arg, 0, MAXARG * sizeof(char *));

    for (int i = 1; i < argc; ++i) {
        // only copy pointer
        cmd_arg[i - 1] = argv[i];
    }

    p = buf;
    while (read(STD_IN, &c, 1)) {
        if (c == '\n') {
            cmd_arg[argc - 1] = buf;

            __exec(argv[1], cmd_arg);
            memset(buf, 0, BUF_SIZE);
            p = buf;
            continue;
        }

        *p++ = c;
    }

    while (wait((int *)0) != -1)
        ;
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(STD_ERR, "Usage: xargs command\n");
        exit(1);
    }

    xargs(argc, argv);
    exit(0);
}