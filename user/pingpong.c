#include "kernel/types.h"
#include "user/user.h"

int main(int argc, char *argv[]) {
    int p_parent[2], p_child[2];
    char c[1];

    pipe(p_parent);
    pipe(p_child);

    if (fork() == 0) {
        // child
        close(p_child[0]);
        close(p_parent[1]);

        read(p_parent[0], c, 1);
        close(p_parent[0]);
        printf("%d: received ping\n", getpid());

        write(p_child[1], "p", 1);
        close(p_child[1]);
    } else {
        // parent
        close(p_child[1]);
        close(p_parent[0]);

        write(p_parent[1], "p", 1);
        read(p_child[0], c, 1);

        close(p_child[0]);
        close(p_parent[1]);
        wait(0);
        printf("%d: received pong\n", getpid());
    }

    exit(0);
}
