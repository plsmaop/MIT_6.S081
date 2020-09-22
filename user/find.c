#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"
#include "kernel/fs.h"

#define STD_ERR 2

void find(char *dir_tree, char *filename) {
  char buf[512], *p;
  int fd;
  struct dirent de;
  struct stat st;

  if ((fd = open(dir_tree, 0)) < 0){
    fprintf(STD_ERR, "find: cannot open %s\n", dir_tree);
    exit(1);
  }

  if (fstat(fd, &st) < 0){
    fprintf(STD_ERR, "find: cannot stat %s\n", dir_tree);
    close(fd);
    exit(1);
  }

  switch (st.type) {
  case T_FILE:
    fprintf(STD_ERR, "find: directory tree: %s is not a directory\n", dir_tree);
    exit(1);

  case T_DIR:
    if(strlen(dir_tree) + 1 + DIRSIZ + 1 > sizeof buf) {
      printf("find: path too long\n");
      break;
    }

    memset(buf, 0, 512);
    strcpy(buf, dir_tree);
    p = buf + strlen(buf);
    *p++ = '/';

    char tmp[512];
    while (read(fd, &de, sizeof(de)) == sizeof(de)) {
      if (de.inum == 0 || strcmp(".", de.name) == 0 || strcmp("..", de.name) == 0)
        continue;

      if (strcmp(de.name, filename) == 0) {
        printf("%s%s\n", buf, filename);
        continue;
      }

      memset(tmp, 0, 512);
      strcpy(tmp, buf);
      p = tmp + strlen(tmp);
      memmove(p, de.name, DIRSIZ);
      p[DIRSIZ] = 0;

      if (stat(tmp, &st) < 0) {
        printf("find: cannot stat: %s\n", tmp);
        continue;
      }

      if (st.type != T_DIR) {
        continue;
      }

      find(tmp, filename);
    }

    break;
  }

  close(fd);
}

int
main(int argc, char *argv[])
{
  if (argc < 3){
    fprintf(STD_ERR, "Usage: find directory_tree filename\n");
    exit(1);
  }


  find(argv[1], argv[2]);
  exit(0);
}
