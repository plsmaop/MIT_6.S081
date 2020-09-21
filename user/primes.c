#include "kernel/types.h"
#include "user/user.h"

int main(int argc, char *argv[]) {
    int i = 2, ind = 0;
    int primes[] = {2, 3, 5, 7, 11, 13, 17, 19, 23, 29, 31};

    int src, p[2];
    pipe(p);

    src = dup(p[0]);
    close(p[0]);
    for (; ind < 11; ++ind) {
        int next_p[2];
        pipe(next_p);

        if (fork() == 0) {
            int prime = primes[ind];

            close(p[1]);
            close(next_p[0]);

            int num;
            while (read(src, (void *)(&num), sizeof(int)) != 0) {
                if (num == prime) {
                    printf("prime %d\n", num);
                }

                if (num % prime != 0) {
                    write(next_p[1], (void *)(&num), sizeof(int));
                }
            };

            close(src);
            close(next_p[1]);
            exit(0);
        } else {
            close(src);
            src = dup(next_p[0]);
            close(next_p[0]);
            close(next_p[1]);
        }
    }

    for (; i <= 35; ++i) {
        write(p[1], (void *)(&i), sizeof(int));
    }

    close(p[1]);
    while (wait((int *)0) != -1)
        ;

    exit(0);
}