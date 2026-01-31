/* Minimal D2XX test */
#include <stdio.h>
#include <stdlib.h>
#include "ftd2xx.h"

int main(void)
{
    FT_STATUS ftStatus;
    FT_HANDLE ftHandle = NULL;
    
    printf("Testing FT_Open(0, ...)\n");
    
    ftStatus = FT_Open(0, &ftHandle);
    if (ftStatus != FT_OK) {
        printf("FT_Open failed: %d\n", (int)ftStatus);
        return 1;
    }
    
    printf("FT_Open succeeded!\n");
    
    DWORD driverVersion;
    ftStatus = FT_GetDriverVersion(ftHandle, &driverVersion);
    if (ftStatus == FT_OK) {
        printf("D2XX version: %x.%x.%x\n", 
               (unsigned int)((driverVersion & 0x00FF0000) >> 16),
               (unsigned int)((driverVersion & 0x0000FF00) >> 8),
               (unsigned int)(driverVersion & 0x000000FF));
    }
    
    FT_Close(ftHandle);
    printf("Test PASSED\n");
    return 0;
}
