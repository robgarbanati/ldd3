#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

struct mystruct {
    int i;
    char *data;
};


int main(void) {
    struct mystruct robs_struct = {15, NULL};
    printf("hello world\n");
    printf("robs_struct.i = %d\n", robs_struct.i);
    printf("(&robs_struct)->i = %d\n", (&robs_struct)->i);
}
    
