#include <stdio.h>
#include <papi.h>
#include <stdlib.h>

int main(){
    int retval;

    retval = PAPI_hl_region_begin("computation");
    if(retval != PAPI_OK)
        exit(1);
    for(int i = 0; i < 10000; i++)
        printf("Value i: %d\n",i);
    
    retval = PAPI_hl_region_end("computation");
    if(retval != PAPI_OK)
        exit(1);
}
