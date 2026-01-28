#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/fcntl.h"
#include "user/user.h"

char buf[512];

void
wc(int fd, char *name, int show_lines, int show_words, int show_chars)
{
  int i, n;
  int l, w, c, inword;

  l = w = c = 0;
  inword = 0;
  while((n = read(fd, buf, sizeof(buf))) > 0){
    for(i=0; i<n; i++){
      c++;
      if(buf[i] == '\n')
        l++;
      if(strchr(" \r\t\n\v", buf[i]))
        inword = 0;
      else if(!inword){
        w++;
        inword = 1;
      }
    }
  }
  if(n < 0){
    printf("wc: read error\n");
    exit(1);
  }
  if(show_lines)
    printf("%d ", l);
  if(show_words)
    printf("%d ", w);
  if(show_chars)
    printf("%d ", c);

  printf("%s\n", name);
}

int
main(int argc, char *argv[])
{
  int fd, i;
  int show_lines = 0;
  int show_words = 0;
  int show_chars = 0;
  int first_file = 1;


  for(i = 1; i < argc && argv[i][0] == '-'; i++){
    char *opt = argv[i];
    int j;
    for(j = 1; opt[j] != '\0'; j++){
      if(opt[j] == 'l')
        show_lines = 1;
      else if(opt[j] == 'w')
        show_words = 1;
      else if(opt[j] == 'c')
        show_chars = 1;
      else {
        printf("wc: invalid option -%c\n", opt[j]);
        exit(1);
      }
    }
    first_file++;
  }
  if(!show_chars && !show_lines && !show_words){
    show_lines = 1;
    show_words = 1;
    show_chars = 1;
  }

    //KEY FIX: No files → read from stdin
  if(first_file >= argc){
    wc(0, "", show_lines, show_words, show_chars);
    exit(0);
  }




  for(i = first_file; i < argc; i++){
    if((fd = open(argv[i], O_RDONLY)) < 0){
      printf("wc: cannot open %s\n", argv[i]);
      exit(1);
    }
    wc(fd, argv[i], show_lines, show_words, show_chars);
    close(fd);
  }
  exit(0);
}
