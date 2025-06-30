//
// File-system system calls.
// Mostly argument checking, since we don't trust
// user code, and calls into file.c and fs.c.
//

#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "stat.h"
#include "spinlock.h"
#include "proc.h"
#include "fs.h"
#include "sleeplock.h"
#include "file.h"
#include "fcntl.h"

// Fetch the nth word-sized system call argument as a file descriptor
// and return both the descriptor and the corresponding struct file.
static int
argfd(int n, int *pfd, struct file **pf)
{
  int fd;
  struct file *f;

  argint(n, &fd);
  if(fd < 0 || fd >= NOFILE || (f=myproc()->ofile[fd]) == 0)
    return -1;
  if(pfd)
    *pfd = fd;
  if(pf)
    *pf = f;
  return 0;
}

// Allocate a file descriptor for the given file.
// Takes over file reference from caller on success.
static int
fdalloc(struct file *f)
{
  int fd;
  struct proc *p = myproc();

  for(fd = 0; fd < NOFILE; fd++){
    if(p->ofile[fd] == 0){
      p->ofile[fd] = f;
      return fd;
    }
  }
  return -1;
}

//在一个进程内部，dup（或 dup2) 会复制一个文件描述符。新复制的 fd 和原来的 fd 指向同一个 struct file 实例。
uint64
sys_dup(void)
{
  struct file *f;
  int fd;

  if(argfd(0, 0, &f) < 0)
    return -1;
  if((fd=fdalloc(f)) < 0)
    return -1;
  filedup(f);
  return fd;
}

uint64
sys_read(void)
{
  struct file *f;
  int n;
  uint64 p;

  argaddr(1, &p);
  argint(2, &n);
  if(argfd(0, 0, &f) < 0)
    return -1;
  return fileread(f, p, n);
}

uint64
sys_write(void)
{
  struct file *f;
  int n;
  uint64 p;
  
  argaddr(1, &p);
  argint(2, &n);
  if(argfd(0, 0, &f) < 0)
    return -1;

  return filewrite(f, p, n);
}

uint64
sys_close(void)
{
  int fd;
  struct file *f;

  if(argfd(0, &fd, &f) < 0)
    return -1;
  myproc()->ofile[fd] = 0;
  fileclose(f);
  return 0;
}

uint64
sys_fstat(void)
{
  struct file *f;
  uint64 st; // user pointer to struct stat

  argaddr(1, &st);
  if(argfd(0, 0, &f) < 0)
    return -1;
  return filestat(f, st);
}

// 创建硬链接：使新路径指向已存在文件的 inode
// 参数:
//   old - 已存在文件的路径
//   new - 要创建的硬链接路径
// 返回值:
//   成功返回 0，失败返回 -1
// - 目录实际上是一个特殊的文件，其内容是一个个的目录项（dirent），每个目录项包含一个文件名和inode号。
// - 创建硬链接就是在某个目录下添加一个目录项，该目录项的inode号指向源文件的inode。
// - 文件系统通过目录项中的inode号找到磁盘上的inode，进而访问文件内容。
uint64
sys_link(void)
{
  char name[DIRSIZ];      // 存储新链接的文件名（路径最后一部分）
  char new[MAXPATH];      // 存储新链接的完整路径
  char old[MAXPATH];      // 存储已存在文件的完整路径
  struct inode *dp, *ip;  // dp: 目标目录的 inode, ip: 源文件的 inode

  // 从用户空间获取路径参数
  if(argstr(0, old, MAXPATH) < 0 || argstr(1, new, MAXPATH) < 0)
    return -1;  // 路径无效或过长

  // 开始文件系统事务（确保操作原子性）
  begin_op();

  // 查找源文件的 inode
  if((ip = namei(old)) == 0){
    end_op();    // 结束事务
    return -1;   // 源文件不存在
  }

  // 锁定源文件 inode（防止并发修改）
  ilock(ip);
  
  // 禁止为目录创建硬链接（防止循环引用）
  if(ip->type == T_DIR){
    iunlockput(ip);  // 解锁并释放 inode
    end_op();        // 结束事务
    return -1;       // 目录不能硬链接
  }

  // 增加硬链接计数
  ip->nlink++;
  // 立即更新磁盘上的 inode（反映链接计数变化）
  iupdate(ip);
  // 解锁源文件 inode（此时链接计数已增加）
  iunlock(ip);

  // 解析新路径的父目录和文件名
  if((dp = nameiparent(new, name)) == 0)
    goto bad;  // 父目录不存在（跳转到错误处理）

  // 锁定目标目录 inode
  ilock(dp);
  
  // 检查设备一致性和创建目录项
  if(dp->dev != ip->dev || dirlink(dp, name, ip->inum) < 0){
    // 失败情况：跨设备或目录操作失败
    iunlockput(dp);  // 解锁并释放目录 inode
    goto bad;        // 跳转到错误处理
  }
  
  // 成功创建链接后的清理
  iunlockput(dp);  // 解锁并释放目录 inode
  iput(ip);        // 减少源文件 inode 的引用计数， 因为之前调用namei->namex->iget,会增加这个inode*的引用计数，不减1的话，内存无法回收

  // 成功结束事务
  end_op();
  return 0;

// 错误处理部分
bad:
  // 回滚之前增加的链接计数
  ilock(ip);       // 重新锁定源文件 inode
  ip->nlink--;     // 恢复链接计数
  iupdate(ip);     // 更新磁盘上的 inode
  iunlockput(ip);  // 解锁并释放 inode（若链接计数为0可能触发删除）
  end_op();        // 结束事务
  return -1;       // 返回错误
}

// Is the directory dp empty except for "." and ".." ?
static int
isdirempty(struct inode *dp)
{
  int off;
  struct dirent de;

  for(off=2*sizeof(de); off<dp->size; off+=sizeof(de)){
    if(readi(dp, 0, (uint64)&de, off, sizeof(de)) != sizeof(de))
      panic("isdirempty: readi");
    if(de.inum != 0)
      return 0;
  }
  return 1;
}

// 删除链接（移除一个目录项，可能删除文件本身）
uint64
sys_unlink(void)
{
  struct inode *ip, *dp;
  struct dirent de;
  char name[DIRSIZ], path[MAXPATH];
  uint off;

  if(argstr(0, path, MAXPATH) < 0)
    return -1;

  begin_op();
  printf("sys_unlink path:%s\n", path);
  if((dp = nameiparent(path, name)) == 0){
    end_op();
    return -1;
  }
  printf("sys_unlink name:%s\n", name);
  ilock(dp);

  // Cannot unlink "." or "..".
  if(namecmp(name, ".") == 0 || namecmp(name, "..") == 0)
    goto bad;

  if((ip = dirlookup(dp, name, &off)) == 0)
    goto bad;
  ilock(ip);

  if(ip->nlink < 1)
    panic("unlink: nlink < 1");
  if(ip->type == T_DIR && !isdirempty(ip)){
    iunlockput(ip);
    goto bad;
  }

  memset(&de, 0, sizeof(de));
  if(writei(dp, 0, (uint64)&de, off, sizeof(de)) != sizeof(de))
    panic("unlink: writei");
  if(ip->type == T_DIR){
    dp->nlink--;
    iupdate(dp);
  }
  iunlockput(dp);

  ip->nlink--;
  iupdate(ip);
  iunlockput(ip);

  end_op();

  return 0;

bad:
  iunlockput(dp);
  end_op();
  return -1;
}

// 为新分配的 inode 在文件系统中创建一个命名入口（目录项） 
// create：创建全新的 inode 并为其命名。
/**
 * 在文件系统中创建一个新文件/目录/设备文件，并返回其 inode
 * @param path 目标路径（将被创建的文件/目录的完整路径）
 * @param type 文件类型（T_FILE:普通文件, T_DIR:目录, T_DEVICE:设备文件）
 * @param major 主设备号（仅设备文件有效）
 * @param minor 次设备号（仅设备文件有效）
 * @return 新创建的 inode 指针，失败返回 0
 */
static struct inode*
create(char *path, short type, short major, short minor)
{
  struct inode *ip, *dp;  // ip:新文件inode, dp:父目录inode
  char name[DIRSIZ];      // 存储路径中的文件名部分

  // 步骤1：解析父目录和文件名
  // 使用 nameiparent 解析路径，获取父目录 inode (dp) 和文件名 (name)
  if((dp = nameiparent(path, name)) == 0)
    return 0;  // 路径解析失败

  // 锁定父目录 inode（后续需要修改目录内容）
  ilock(dp);

  // 步骤2：检查文件是否已存在
  // 在父目录中查找同名文件（0 表示不存储偏移量）
  if((ip = dirlookup(dp, name, 0)) != 0) {
    // 文件已存在时的处理：
    iunlockput(dp);  // 释放父目录锁和引用
    
    // 锁定已存在的文件 inode（检查其类型）
    ilock(ip);
    
    /* 特殊处理：当创建普通文件且目标已存在普通文件/设备文件时
     * 符合 open(O_CREAT) 语义：文件存在不是错误 */
    if(type == T_FILE && (ip->type == T_FILE || ip->type == T_DEVICE))
      return ip;  // 返回已存在的 inode
    
    // 其他情况（目录/设备文件已存在，或类型不匹配）：
    iunlockput(ip);  // 释放文件锁和引用
    return 0;        // 返回错误
  }

  // 步骤3：分配新 inode
  // 在父目录所在设备上分配指定类型的新 inode
  if((ip = ialloc(dp->dev, type)) == 0) {
    iunlockput(dp);  // 分配失败，释放父目录
    return 0;
  }

  // 步骤4：初始化新 inode
  ilock(ip);          // 锁定新 inode（防止并发修改）
  ip->major = major;  // 设置主设备号（设备文件有效）
  ip->minor = minor;  // 设置次设备号（设备文件有效）
  ip->nlink = 1;      // 初始链接计数=1（文件自身）
  iupdate(ip);        // 立即写回磁盘

  // 步骤5：特殊处理目录类型
  if(type == T_DIR) {
    // 创建目录自引用 "." 和父目录引用 ".."
    if(dirlink(ip, ".", ip->inum) < 0 ||   // . -> 当前目录
       dirlink(ip, "..", dp->inum) < 0) {  // .. -> 父目录
      goto fail;  // 创建失败跳转清理
    }
    // 注意：此时不增加 ip->nlink，避免循环引用
    // （"." 已计入初始的 nlink=1）
  }

  // 步骤6：将新文件链接到父目录
  // 在父目录 dp 中添加指向新 inode 的条目
  if(dirlink(dp, name, ip->inum) < 0)
    goto fail;  // 链接失败

  // 步骤7：更新父目录链接计数（仅目录类型）
  if(type == T_DIR) {
    // 此时操作已确保成功，更新父目录链接计数
    dp->nlink++;  // 因新目录的 ".." 指向父目录
    iupdate(dp);  // 更新父目录磁盘数据
  }

  // 步骤8：清理资源并返回
  iunlockput(dp);  // 释放父目录锁和引用
  return ip;       // 返回新 inode

// 错误处理标签
 fail:
  // 清理已分配的资源：
  ip->nlink = 0;     // 标记 inode 链接数为0（可被回收）
  iupdate(ip);       // 更新 inode 磁盘数据
  iunlockput(ip);    // 释放新文件锁和引用（会触发回收）
  iunlockput(dp);    // 释放父目录锁和引用
  return 0;          // 返回错误
}

#define MAX_SYMLINK_DEPTH 10  // 最大符号链接深度

/**
 * 递归解析符号链接
 * @param ip 已锁定的符号链接 inode
 * @return 成功时返回锁定状态的目标 inode，失败返回0
 */
struct inode* 
resolve_symlink(struct inode* ip)
{
    char target[MAXPATH];
    struct inode *next;
    int depth = 0;

    // 保持当前 inode 锁定状态进入循环
    while(ip->type == T_SYMLINK) {
        // 检查递归深度
        if(++depth > MAX_SYMLINK_DEPTH) {
            iunlockput(ip);  // 解锁并释放
            return 0;
        }

        // 读取符号链接目标
        memset(target, 0, MAXPATH);
        int len = readi(ip, 0, (uint64)target, 0, MAXPATH);
        if(len <= 0) {
            iunlockput(ip);  // 解锁并释放
            return 0;
        }

        // 解锁当前符号链接（但不立即释放，因为可能需要事务）
        iunlock(ip);
        
        // 查找目标 inode
        if((next = namei(target)) == 0) {
            iput(ip);  // 释放原符号链接
            return 0;
        }

        // 锁定新 inode
        ilock(next);
        
        // 释放原符号链接
        iput(ip);
        
        // 继续处理新 inode
        ip = next;
    }
    
    // 返回时 ip 处于锁定状态
    return ip;
}

/**
 * 系统调用 open 的实现函数
 * @return 成功返回文件描述符 fd，失败返回 -1
 */
uint64
sys_open(void)
{
  char path[MAXPATH];      // 存储文件路径
  int fd, omode;           // fd:文件描述符, omode:打开模式
  struct file *f;          // 文件结构指针
  struct inode *ip;        // 文件 inode 指针
  int n;                   // 临时变量

  // 步骤1：获取系统调用参数
  argint(1, &omode);                             // 获取打开模式参数
  if((n = argstr(0, path, MAXPATH)) < 0)         // 获取文件路径参数
    return -1;

  printf("sys_open[%d]: path='%s' mode=%d\n", myproc()->pid, path, omode);
  // 步骤2：开始文件系统事务
  begin_op();

  // 步骤3：处理文件创建/打开
  if(omode & O_CREATE) {  // 需要创建文件
    // 调用 create 创建普通文件 (T_FILE)
    ip = create(path, T_FILE, 0, 0);
    if(ip == 0) {         // 创建失败
      end_op();
      return -1;
    }
  } else {                // 不需要创建文件
    // 查找路径对应的 inode
    if((ip = namei(path)) == 0) {  // 文件不存在
      end_op();
      return -1;
    }
    // 锁定 inode 并检查类型
    ilock(ip);
    // 禁止以非只读模式打开目录
    if(ip->type == T_DIR && omode != O_RDONLY) {
      iunlockput(ip);
      end_op();
      return -1;
    }
  }

  // 步骤4：设备文件有效性检查
  if(ip->type == T_DEVICE && (ip->major < 0 || ip->major >= NDEV)) {
    iunlockput(ip);
    end_op();
    return -1;  // 无效设备号
  }

  // 处理符号链接（仅在非 O_NOFOLLOW 时）
if(ip->type == T_SYMLINK && !(omode & O_NOFOLLOW)) {
    // 解析符号链接链
    struct inode *resolved = resolve_symlink(ip);
    if(!resolved) {
        end_op();
        return -1;
    }
    ip = resolved;  // 使用解析后的 inode
    // 注意：此时 ip 仍处于锁定状态!
}

  // 步骤5：分配文件结构和文件描述符
  // 创建新的 struct file 实例
  if((f = filealloc()) == 0 || (fd = fdalloc(f)) < 0) {
    // 分配失败时的清理
    if(f)
      fileclose(f);       // 释放文件结构
    iunlockput(ip);       // 解锁并释放 inode
    end_op();
    return -1;
  }

  // 步骤6：初始化文件结构
  if(ip->type == T_DEVICE) {  // 设备文件
    f->type = FD_DEVICE;      // 标记为设备类型
    f->major = ip->major;     // 设置主设备号
  } else {                    // 普通文件或目录
    f->type = FD_INODE;       // 标记为 inode 类型
    f->off = 0;               // 初始化读写偏移量
  }
  f->ip = ip;                 // 关联 inode
  // 设置读写权限标志
  f->readable = !(omode & O_WRONLY);  // 非只写模式可读
  f->writable = (omode & O_WRONLY) || (omode & O_RDWR);  // 只写或读写模式可写

  // 步骤7：处理截断选项
  if((omode & O_TRUNC) && ip->type == T_FILE) {
    itrunc(ip);  // 截断文件（清空内容）
  }

  // 步骤8：清理资源并返回
  iunlock(ip);   // 解锁 inode（注意：此时文件结构持有引用）
  end_op();      // 提交文件系统事务
  return fd;     // 返回文件描述符
}

uint64
sys_mkdir(void)
{
  char path[MAXPATH];
  struct inode *ip;

  begin_op();
  if(argstr(0, path, MAXPATH) < 0 || (ip = create(path, T_DIR, 0, 0)) == 0){
    end_op();
    return -1;
  }
  iunlockput(ip);
  end_op();
  return 0;
}

uint64
sys_mknod(void)
{
  struct inode *ip;
  char path[MAXPATH];
  int major, minor;

  begin_op();
  argint(1, &major);
  argint(2, &minor);
  if((argstr(0, path, MAXPATH)) < 0 ||
     (ip = create(path, T_DEVICE, major, minor)) == 0){
    end_op();
    return -1;
  }
  iunlockput(ip);
  end_op();
  return 0;
}

uint64
sys_chdir(void)
{
  char path[MAXPATH];
  struct inode *ip;
  struct proc *p = myproc();
  
  begin_op();
  if(argstr(0, path, MAXPATH) < 0 || (ip = namei(path)) == 0){
    end_op();
    return -1;
  }
  ilock(ip);
  if(ip->type != T_DIR){
    iunlockput(ip);
    end_op();
    return -1;
  }
  iunlock(ip);
  iput(p->cwd);
  end_op();
  p->cwd = ip;
  return 0;
}

uint64
sys_exec(void)
{
  char path[MAXPATH], *argv[MAXARG];
  int i;
  uint64 uargv, uarg;

  argaddr(1, &uargv);
  if(argstr(0, path, MAXPATH) < 0) {
    return -1;
  }
  memset(argv, 0, sizeof(argv));
  for(i=0;; i++){
    if(i >= NELEM(argv)){
      goto bad;
    }
    if(fetchaddr(uargv+sizeof(uint64)*i, (uint64*)&uarg) < 0){
      goto bad;
    }
    if(uarg == 0){
      argv[i] = 0;
      break;
    }
    argv[i] = kalloc();
    if(argv[i] == 0)
      goto bad;
    if(fetchstr(uarg, argv[i], PGSIZE) < 0)
      goto bad;
  }

  int ret = exec(path, argv);

  for(i = 0; i < NELEM(argv) && argv[i] != 0; i++)
    kfree(argv[i]);

  return ret;

 bad:
  for(i = 0; i < NELEM(argv) && argv[i] != 0; i++)
    kfree(argv[i]);
  return -1;
}

uint64 sys_pipe(void)
{
  uint64 fdarray; // 用户传入的指针，指向一个包含两个整数的数组，用于返回读写文件描述符
  struct file *rf, *wf; // 分别表示管道的读端和写端文件结构
  int fd0, fd1; // 文件描述符
  struct proc *p = myproc(); // 获取当前进程指针

  // 从系统调用参数中获取第 0 个参数，即 fdarray 的地址
  argaddr(0, &fdarray);

  // 创建管道，返回读端 rf 和写端 wf 的 struct file 指针
  // 如果失败（返回值 < 0），表示内核资源不足，直接返回错误
  if(pipealloc(&rf, &wf) < 0)
    return -1;

  fd0 = -1; // 初始化 fd0，作为失败时判断依据

  // 尝试为读端和写端分配两个文件描述符
  // 如果任意一个分配失败，进入错误处理分支
  if((fd0 = fdalloc(rf)) < 0 || (fd1 = fdalloc(wf)) < 0){
    // 如果读端分配成功而写端失败，需要释放已分配的资源
    if(fd0 >= 0)
      p->ofile[fd0] = 0; // 清除进程打开文件表中的引用
    fileclose(rf); // 关闭读端
    fileclose(wf); // 关闭写端
    return -1;
  }

  // 将内核分配好的文件描述符拷贝到用户空间 fdarray 数组中
  // 第一个是读端，第二个是写端
  if(copyout(p->pagetable, fdarray, (char*)&fd0, sizeof(fd0)) < 0 ||
     copyout(p->pagetable, fdarray+sizeof(fd0), (char *)&fd1, sizeof(fd1)) < 0){
    // 如果拷贝失败，需要回收资源并清除文件表项
    p->ofile[fd0] = 0;
    p->ofile[fd1] = 0;
    fileclose(rf);
    fileclose(wf);
    return -1;
  }

  // 一切顺利，返回 0 表示成功
  return 0;
}

uint64
sys_symlink(void)
{
  char target[MAXPATH], path[MAXPATH];
  struct inode *ip;

  // 读取参数
  if(argstr(0, target, MAXPATH) < 0 || argstr(1, path, MAXPATH) < 0)
    return -1;

  begin_op();
  
  // 创建符号链接 inode（注意：T_SYMLINK 需在 kernel/stat.h 中定义）
  ip = create(path, T_SYMLINK, 0, 0);
  if(ip == 0){
    end_op();
    return -1;
  }

  // 将目标路径写入符号链接 inode
  if(writei(ip, 0, (uint64)target, 0, strlen(target)) != strlen(target)) {
    iunlockput(ip);
    end_op();
    return -1;
  }

  iunlockput(ip);
  end_op();
  return 0;
}