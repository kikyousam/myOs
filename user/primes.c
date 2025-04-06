#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

void help(int vals[35], int num)
{
    if(num == 0)
    {
        return;
    }
    fprintf(1, "prime %d\n", vals[0]);
    int p[2];
    pipe(p);
    if(fork() == 0)
    {   
        close(p[1]);
        int sieveNum;
        read(p[0], &sieveNum, 4);
        int passVal[35] = {0};
        int passNum = 0;
        int val;
        while(read(p[0], &val, 4))
        {
            if(val % sieveNum != 0)
            {
                passVal[passNum] = val;
                passNum ++;
            }
            else
            {
                fprintf(1, "sieveNum %d pass %d\n", sieveNum, val);
            }
        }
        fprintf(1, "passNum[%d]\n", passNum);
        for(int i = 0; i < passNum; ++i)
        {
            fprintf(1, "passVal[%d] %d\n",i, passVal[i]);
        }
        help(passVal, passNum);
        exit(0);
    }
    else
    {
        close(p[0]);
        for(int i = 0; i < num; ++i)
        {
            write(p[1], &vals[i], 4);
        }
        close(p[1]);
        wait(0);
        exit(0);
    }
}

int
main(int argc, char* argv[])
{
    int vals[35];
    int num = 34;
    for(int i = 0; i < num; ++i)
    {
        vals[i] = i + 2;
    }
    help(vals, num);
    exit(0);
}