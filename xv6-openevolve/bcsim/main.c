// Host driver: boots the buffer cache and runs the real bcache_bench().
// usage: bcsim workload scan hot interval passes rounds [trace]
#include <stdlib.h>
#include <stdio.h>
#include "types.h"
#include "defs.h"
int main(int argc, char **argv){
  int a[7] = {2, 120, 4, 40, 3, 5, 0};
  int i;
  for(i = 0; i < 7 && i + 1 < argc; i++) a[i] = atoi(argv[i+1]);
  binit();
  if(bcache_bench(a[0], a[1], a[2], a[3], a[4], a[5], a[6]) < 0){
    fprintf(stderr, "invalid parameters\n");
    return 1;
  }
  return 0;
}
