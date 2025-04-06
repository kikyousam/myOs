#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/param.h"
#include "user/user.h"

#define MAX_LINE_LENGTH 512

int main(int argc, char* argv[]) {
    if (argc < 2) {
        fprintf(2, "Usage: xargs <command> [args...]\n");
        exit(1);
    }

    char line[MAX_LINE_LENGTH];
    char* args[MAXARG];
    int arg_idx = 0;

    // 复制原始参数（跳过 "xargs"）
    for (int i = 1; i < argc; ++i) {
        args[arg_idx++] = argv[i];
    }

    // 读取标准输入并解析参数
    int n;
    while ((n = read(0, line, sizeof(line)))) {
        if (n < 0) {
            fprintf(2, "xargs: read error\n");
            exit(1);
        }

        line[n] = '\0'; // 确保字符串终止
        char* p = line;

        // 解析每行参数
        while (*p != '\0') {
            // 跳过空白字符
            while (*p == ' ' || *p == '\n') p++;
            if (*p == '\0') break;

            // 定位参数结束位置
            char* arg_start = p;
            while (*p != ' ' && *p != '\n' && *p != '\0') p++;
            
            // 复制参数到新内存
            char* arg = malloc(p - arg_start + 1);
            if (!arg) {
                fprintf(2, "xargs: memory allocation failed\n");
                exit(1);
            }
            memmove(arg, arg_start, p - arg_start);
            arg[p - arg_start] = '\0';

            // 检查参数数量是否超限
            if (arg_idx >= MAXARG - 1) {
                fprintf(2, "xargs: too many arguments\n");
                exit(1);
            }
            args[arg_idx++] = arg;
        }
    }

    // 设置exec的参数结尾NULL
    args[arg_idx] = 0;

    // 执行命令
    if (fork() == 0) {
        exec(args[0], args);
        fprintf(2, "xargs: exec %s failed\n", args[0]);
        exit(1);
    } else {
        wait(0);
        exit(0);
    }
}