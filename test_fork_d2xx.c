/* Test D2XX after fork */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>
#include "ftd2xx.h"

int main(void)
{
    pid_t pid = fork();
    
    if (pid < 0) {
        perror("fork");
        return 1;
    }
    
    if (pid == 0) {
        /* Child process */
        printf("[Child] Testing FT_Open(0, ...)\n");
        
        FT_STATUS ftStatus;
        FT_HANDLE ftHandle = NULL;
        
        ftStatus = FT_Open(0, &ftHandle);
        if (ftStatus != FT_OK) {
            printf("[Child] FT_Open failed: %d\n", (int)ftStatus);
            exit(1);
        }
        
        printf("[Child] FT_Open succeeded!\n");
        
        DWORD driverVersion;
        ftStatus = FT_GetDriverVersion(ftHandle, &driverVersion);
        if (ftStatus == FT_OK) {
            printf("[Child] D2XX version: %x.%x.%x\n", 
                   (unsigned int)((driverVersion & 0x00FF0000) >> 16),
                   (unsigned int)((driverVersion & 0x0000FF00) >> 8),
                   (unsigned int)(driverVersion & 0x000000FF));
        }
        
        FT_Close(ftHandle);
        printf("[Child] Test PASSED\n");
        exit(0);
    } else {
        /* Parent process */
        int status;
        waitpid(pid, &status, 0);
        
        if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
            printf("[Parent] Child test PASSED\n");
            return 0;
        } else {
            printf("[Parent] Child test FAILED\n");
            return 1;
        }
    }
}
