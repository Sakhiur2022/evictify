#include "types.h"
#include "user.h"

// Usage: bcbench [workload] [key=value ...]
//   workload: toy | seq | hotscan | wset | pollute | mixed | safety
//   keys:     scan=N (1-400)  hot=N (1-64)  interval=N  passes=N (1-20)
//             rounds=N (1-100)  trace=N (print first N requests; default 0 = off)
// Legacy form still accepted:  bcbench <scan> <rounds>   (= hotscan)
// NBUF is a compile-time kernel setting; use make NBUF=60.

static char *names[] = { "toy", "seq", "hotscan", "wset", "pollute", "mixed", "safety" };

static void
usage(void)
{
  printf(2, "Usage: bcbench [toy|seq|hotscan|wset|pollute|mixed|safety]\n");
  printf(2, "         [scan=N] [hot=N] [interval=N] [passes=N] [rounds=N] [trace=N]\n");
  printf(2, "       bcbench <scan> <rounds>      (legacy: hotscan)\n");
  exit();
}

int
main(int argc, char *argv[])
{
  int w = 2, scan = 120, hot = 4, interval = 40, passes = 3, rounds = 5, trace = 0;
  int i, j;

  if(argc > 1 && argv[1][0] >= '0' && argv[1][0] <= '9'){
    scan = atoi(argv[1]);                     // legacy positional form
    if(argc > 2) rounds = atoi(argv[2]);
    if(argc > 3) usage();
  } else {
    for(i = 1; i < argc; i++){
      char *eq = 0;
      for(j = 0; argv[i][j]; j++)
        if(argv[i][j] == '='){ eq = &argv[i][j]; break; }
      if(!eq){
        for(j = 0; j < 7; j++)
          if(strcmp(argv[i], names[j]) == 0) break;
        if(j == 7) usage();
        w = j;
        continue;
      }
      *eq = 0;
      if(strcmp(argv[i], "scan") == 0) scan = atoi(eq + 1);
      else if(strcmp(argv[i], "hot") == 0) hot = atoi(eq + 1);
      else if(strcmp(argv[i], "interval") == 0) interval = atoi(eq + 1);
      else if(strcmp(argv[i], "passes") == 0) passes = atoi(eq + 1);
      else if(strcmp(argv[i], "rounds") == 0) rounds = atoi(eq + 1);
      else if(strcmp(argv[i], "trace") == 0) trace = atoi(eq + 1);
      else usage();
    }
  }

  if(bcachebench(w, scan, hot, interval, passes, rounds, trace) < 0){
    printf(2, "bcbench: invalid parameters (or too many requests)\n");
    exit();
  }
  exit();
}
