#include "kernel/types.h"
#include "kernel/fcntl.h"
#include "user/user.h"

char buf[512];

// void
// cat(int fd)
// {
//   int n;

//   while((n = read(fd, buf, sizeof(buf))) > 0) {
//     if (write(1, buf, n) != n) {
//       fprintf(2, "cat: write error\n");
//       exit(1);
//     }
//   }
//   if(n < 0){
//     fprintf(2, "cat: read error\n");
//     exit(1);
//   }
// }


/*Hint from lab instructions*/
int
readline(int fd, char *buf, int maxlen)
{
  int n;
  char c;
  int i = 0;

  /* Read one character at a time from fd */
  while((n = read(fd, &c, 1)) > 0){
    /* Only store the character if there is room for it and a null terminator */
    if (i < maxlen - 1) {
      buf[i++] = c;
      if (c == '\n')
        break;
    } else {
      /* We can't recover, so just print a message and exit */
      fprintf(2, "readline() - line too long\n");
      exit(-1);
    }
  }
  /* This is a little tricky. If read() returns 0 AND we didn't
     read previous characters for this line, then we want to return 0.
     Also, if read returns a value less than 0, we want to return this
     error condition. */
  if(((n == 0) && (i == 0)) || (n < 0))
    return n;

  /* Add the null terminator to the end for the string buffer */
  buf[i] = '\0';
  return i;
}

void
print_lineno(int n)
{
  int digits = 0;
  int tmp = n;

  if (tmp == 0)
    digits = 1;
  else {
    while (tmp > 0) {
      digits++;
      tmp /= 10;
    }
  }

  for (int i = 0; i < 6 - digits; i++)
    printf(" ");

  printf("%d  ", n);   
}


void
cat(int fd, int n_option, int *line_no)
{
  int n;

  while((n = readline(fd, buf, 512)) > 0){
    if(n_option){
      print_lineno(*line_no);
      (*line_no)++;
    }
    if(write(1, buf, n) != n){
      fprintf(2, "cat: write error\n");
      exit(1);
    }
  }
  if(n < 0){
    fprintf(2, "cat: read error\n");
    exit(1);
  }
}

int
main(int argc, char *argv[])
{
  int fd;
  int i = 1;
  int n_option = 0;
  int line_no = 1;

  if (argc <= 1) {
    cat(0, n_option, &line_no);
    exit(0);
  }
  if(argc > 1 && strcmp(argv[1], "-n") == 0){
    n_option = 1;
    i = 2;
  }

  if(argc == 2 && n_option == 1){
    cat(0, n_option, &line_no);  
    exit(0);
  }

  for(; i < argc; i++){
    if((fd = open(argv[i], O_RDONLY)) < 0){
      fprintf(2, "cat: cannot open %s\n", argv[i]);
      exit(1);
    }
    cat(fd, n_option, &line_no);
    close(fd);
  }
  exit(0);
}
