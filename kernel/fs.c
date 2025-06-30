// File system implementation.  Five layers:
//   + Blocks: allocator for raw disk blocks.
//   + Log: crash recovery for multi-step updates.
//   + Files: inode allocator, reading, writing, metadata.
//   + Directories: inode with special contents (list of other inodes!)
//   + Names: paths like /usr/rtm/xv6/fs.c for convenient naming.
//
// This file contains the low-level file system manipulation
// routines.  The (higher-level) system call implementations
// are in sysfile.c.

#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "stat.h"
#include "spinlock.h"
#include "proc.h"
#include "sleeplock.h"
#include "fs.h"
#include "buf.h"
#include "file.h"

#define min(a, b) ((a) < (b) ? (a) : (b))
// there should be one superblock per disk device, but we run with
// only one device
struct superblock sb; 

// Read the super block.
static void
readsb(int dev, struct superblock *sb)
{
  struct buf *bp;

  bp = bread(dev, 1);
  memmove(sb, bp->data, sizeof(*sb));
  brelse(bp);
}

// Init fs
void
fsinit(int dev) {
  readsb(dev, &sb);
  if(sb.magic != FSMAGIC)
    panic("invalid file system");
  initlog(dev, &sb); //恢复log中的数据
}

// Zero a block.
static void
bzero(int dev, int bno)
{
  struct buf *bp;

  bp = bread(dev, bno);
  memset(bp->data, 0, BSIZE);
  log_write(bp);
  brelse(bp);
}

// Blocks.

// 分配一个清零的磁盘块
// 参数:
//   dev - 设备号（标识哪个磁盘）
// 返回值:
//   成功: 分配的磁盘块号
//   失败: 0 (磁盘空间不足)
static uint
balloc(uint dev)
{
  int b, bi, m;            // b: 当前块组起始块号, bi: 块组内块索引, m: 位掩码
  struct buf *bp;           // 位图块缓冲区指针

  bp = 0;
  // 遍历所有块组 (每组包含BPB个块)
  for(b = 0; b < sb.size; b += BPB) {
    // 读取当前块组的位图块:
    // BBLOCK(b, sb) 计算管理块b的位图块号
    // 位图块位于: 引导块 + 超级块 + 日志块 + inode块 + 之前的位图块
    bp = bread(dev, BBLOCK(b, sb));
    
    // 遍历当前块组内的所有块 (最多BPB个块)
    for(bi = 0; bi < BPB && b + bi < sb.size; bi++) {
      m = 1 << (bi % 8);   // 创建位掩码 (0-7位对应字节内的位)
      
      // 检查块是否空闲 (位值为0表示空闲)
      if((bp->data[bi/8] & m) == 0) {
        // 标记块为已使用: 将对应位置1
        bp->data[bi/8] |= m;
        
        // 将位图块的修改写入日志 (确保原子性)
        log_write(bp);
        
        // 释放位图块缓冲区 (位图修改已持久化，不再需要缓存)
        brelse(bp);
        
        // 将新分配的磁盘块内容清零 (块号 = 块组起始b + 组内偏移bi)
        bzero(dev, b + bi);
        
        // 返回分配的块号
        return b + bi;
      }
    }
    // 当前位图块无空闲块，释放缓冲区
    brelse(bp);
  }
  
  // 所有块组遍历完毕仍未找到空闲块
  printf("balloc: out of blocks\n");
  return 0;
}

// Free a disk block.
static void
bfree(int dev, uint b)
{
  struct buf *bp;
  int bi, m;

  bp = bread(dev, BBLOCK(b, sb));
  bi = b % BPB;
  m = 1 << (bi % 8);
  if((bp->data[bi/8] & m) == 0)
    panic("freeing free block");
  bp->data[bi/8] &= ~m;  //清0位图块中的数据块对应的位图
  log_write(bp);
  brelse(bp);
}

// Inodes.
//
// An inode describes a single unnamed file.
// The inode disk structure holds metadata: the file's type,
// its size, the number of links referring to it, and the
// list of blocks holding the file's content.
//
// The inodes are laid out sequentially on disk at block
// sb.inodestart. Each inode has a number, indicating its
// position on the disk.
//
// The kernel keeps a table of in-use inodes in memory
// to provide a place for synchronizing access
// to inodes used by multiple processes. The in-memory
// inodes include book-keeping information that is
// not stored on disk: ip->ref and ip->valid.
//
// An inode and its in-memory representation go through a
// sequence of states before they can be used by the
// rest of the file system code.
//
// * Allocation: an inode is allocated if its type (on disk)
//   is non-zero. ialloc() allocates, and iput() frees if
//   the reference and link counts have fallen to zero.
//
// * Referencing in table: an entry in the inode table
//   is free if ip->ref is zero. Otherwise ip->ref tracks
//   the number of in-memory pointers to the entry (open
//   files and current directories). iget() finds or
//   creates a table entry and increments its ref; iput()
//   decrements ref.
//
// * Valid: the information (type, size, &c) in an inode
//   table entry is only correct when ip->valid is 1.
//   ilock() reads the inode from
//   the disk and sets ip->valid, while iput() clears
//   ip->valid if ip->ref has fallen to zero.
//
// * Locked: file system code may only examine and modify
//   the information in an inode and its content if it
//   has first locked the inode.
//
// Thus a typical sequence is:
//   ip = iget(dev, inum)
//   ilock(ip)
//   ... examine and modify ip->xxx ...
//   iunlock(ip)
//   iput(ip)
//
// ilock() is separate from iget() so that system calls can
// get a long-term reference to an inode (as for an open file)
// and only lock it for short periods (e.g., in read()).
// The separation also helps avoid deadlock and races during
// pathname lookup. iget() increments ip->ref so that the inode
// stays in the table and pointers to it remain valid.
//
// Many internal file system functions expect the caller to
// have locked the inodes involved; this lets callers create
// multi-step atomic operations.
//
// The itable.lock spin-lock protects the allocation of itable
// entries. Since ip->ref indicates whether an entry is free,
// and ip->dev and ip->inum indicate which i-node an entry
// holds, one must hold itable.lock while using any of those fields.
//
// An ip->lock sleep-lock protects all ip-> fields other than ref,
// dev, and inum.  One must hold ip->lock in order to
// read or write that inode's ip->valid, ip->size, ip->type, &c.

struct {
  struct spinlock lock;
  struct inode inode[NINODE];  //最多50个inode
} itable;       //inode(索引结点)保存在内核中的表格

void
iinit()
{
  int i = 0;
  
  initlock(&itable.lock, "itable");
  for(i = 0; i < NINODE; i++) {
    initsleeplock(&itable.inode[i].lock, "inode");
  }
}

static struct inode* iget(uint dev, uint inum);

// Allocate an inode on device dev.
// Mark it as allocated by  giving it type type.
// Returns an unlocked but allocated and referenced inode,
// or NULL if there is no free inode.
struct inode*
ialloc(uint dev, short type)
{
  int inum;
  struct buf *bp;
  struct dinode *dip;

  for(inum = 1; inum < sb.ninodes; inum++){
    bp = bread(dev, IBLOCK(inum, sb));    //包含inode的缓冲区，从磁盘中读inode的缓存，mkfs 在文件系统创建时完成对inode磁盘区域的初始化
    dip = (struct dinode*)bp->data + inum%IPB;   //Disk Inode Pointer
    if(dip->type == 0){  // a free inode
      memset(dip, 0, sizeof(*dip));
      dip->type = type;
      log_write(bp);   // mark it allocated on the disk
      brelse(bp);
      printf("ialloc: inum=%d type=%d\n", inum, type);
      return iget(dev, inum);
    }
    brelse(bp);
  }
  printf("ialloc: no inodes\n");
  return 0;
}

// Copy a modified in-memory inode to disk.
// Must be called after every change to an ip->xxx field
// that lives on disk.
// Caller must hold ip->lock.
// 只要你修改了 inode 的内存字段（比如 nlink、size、addrs），你就要调用 iupdate() 把修改同步写回磁盘，保持一致性。
void
iupdate(struct inode *ip)
{
  struct buf *bp;
  struct dinode *dip;

  bp = bread(ip->dev, IBLOCK(ip->inum, sb));
  dip = (struct dinode*)bp->data + ip->inum%IPB;
  dip->type = ip->type;
  dip->major = ip->major;
  dip->minor = ip->minor;
  dip->nlink = ip->nlink;
  dip->size = ip->size;
  memmove(dip->addrs, ip->addrs, sizeof(ip->addrs));
  log_write(bp);
  brelse(bp);
}

// Find the inode with number inum on device dev
// and return the in-memory copy. Does not lock
// the inode and does not read it from disk.
static struct inode*
iget(uint dev, uint inum)
{
  struct inode *ip, *empty;

  acquire(&itable.lock);

  // Is the inode already in the table?
  empty = 0;
  for(ip = &itable.inode[0]; ip < &itable.inode[NINODE]; ip++){
    if(ip->ref > 0 && ip->dev == dev && ip->inum == inum){
      ip->ref++;  //增加此inode的inum
      release(&itable.lock);
      return ip;
    }
    if(empty == 0 && ip->ref == 0)    // Remember empty slot.
      empty = ip;
  }

  // Recycle an inode entry.
  if(empty == 0)
    panic("iget: no inodes");

  ip = empty;
  ip->dev = dev;
  ip->inum = inum;
  ip->ref = 1;
  ip->valid = 0;
  release(&itable.lock); // 缓存未命中时，返回空闲slot

  return ip;
}

// Increment reference count for ip.
// Returns ip to enable ip = idup(ip1) idiom.
struct inode*
idup(struct inode *ip)
{
  acquire(&itable.lock);
  ip->ref++;
  release(&itable.lock);
  return ip;
}

// Lock the given inode.
// Reads the inode from disk if necessary.
void
ilock(struct inode *ip)
{
  struct buf *bp;
  struct dinode *dip;

  if(ip == 0 || ip->ref < 1)
    panic("ilock");

  acquiresleep(&ip->lock);  //保证内存 inode 字段的原子访问,保护磁盘加载过程
  
  if(ip->valid == 0){  // 数据未加载,将磁盘存储的dinode搬到内存上的inode
    bp = bread(ip->dev, IBLOCK(ip->inum, sb));
    dip = (struct dinode*)bp->data + ip->inum%IPB;
    ip->type = dip->type;
    ip->major = dip->major;
    ip->minor = dip->minor;
    ip->nlink = dip->nlink;
    ip->size = dip->size;
    memmove(ip->addrs, dip->addrs, sizeof(ip->addrs));
    brelse(bp);
    ip->valid = 1;
    if(ip->type == 0)
      panic("ilock: no type");
  }
}

// Unlock the given inode.
void
iunlock(struct inode *ip)
{
  if(ip == 0 || !holdingsleep(&ip->lock) || ip->ref < 1)
    panic("iunlock");

  releasesleep(&ip->lock);
}

// Drop a reference to an in-memory inode.
// If that was the last reference, the inode table entry can
// be recycled.
// If that was the last reference and the inode has no links
// to it, free the inode (and its content) on disk.
// All calls to iput() must be inside a transaction in
// case it has to free the inode.
// 减少inode的引用计数，如果计数为0，则将inode从itable中移除
void
iput(struct inode *ip)
{
  printf("iput: inum=%d type=%d nlink=%d ref=%d caller=%p\n",
         ip->inum, ip->type, ip->nlink, ip->ref, __builtin_return_address(0));
  acquire(&itable.lock);

  if(ip->ref == 1 && ip->valid && ip->nlink == 0){
    // inode has no links and no other references: truncate and free.

    // ip->ref == 1 means no other process can have ip locked,
    // so this acquiresleep() won't block (or deadlock).
    acquiresleep(&ip->lock);

    release(&itable.lock);

    itrunc(ip);
    ip->type = 0;
    iupdate(ip);
    ip->valid = 0;

    releasesleep(&ip->lock);

    acquire(&itable.lock);
    printf("iput free: inum=%d\n", ip->inum);
  }else if(ip->ref == 0){
    // 添加安全保护：发现引用计数为0但未释放的inode
    panic("iput: ref=0 but inode not freed");
  }

  ip->ref--;
  release(&itable.lock);
}

// Common idiom: unlock, then put.
void
iunlockput(struct inode *ip)
{
  iunlock(ip);
  iput(ip);
}

// Inode content
//
// The content (data) associated with each inode is stored
// in blocks on the disk. The first NDIRECT block numbers
// are listed in ip->addrs[].  The next NINDIRECT blocks are
// listed in block ip->addrs[NDIRECT].

// 返回 inode 中第 n 个逻辑块的磁盘物理地址
// 参数:
//   ip   - 目标 inode 指针
//   bn   - 文件内的逻辑块号 (从0开始编号)
// 返回值:
//   成功: 磁盘物理块地址
//   失败: 0 (磁盘空间不足)
// 功能说明:
//   1. 若逻辑块尚未分配物理块，则动态分配新块
//   2. 维护直接块和间接块映射关系
//   3. 更新 inode 元数据并确保修改持久化
static uint
bmap(struct inode *ip, uint bn)
{
  uint addr, *a;         // addr: 物理块地址, a: 间接块地址数组指针
  struct buf *bp;         // 缓冲区指针，用于读取间接块

  // 处理直接块 (bn 0~10)
  if(bn < NDIRECT){
    // 检查直接块是否已分配
    if((addr = ip->addrs[bn]) == 0){
      // 未分配则申请新磁盘块
      addr = balloc(ip->dev);
      if(addr == 0)
        return 0;         // 磁盘空间不足
      ip->addrs[bn] = addr; // 更新inode的直接块映射表
    }
    return addr;          // 返回物理块地址
  }
  
  // 处理一级间接块 (bn 11~266)
  bn -= NDIRECT;          // 调整bn为间接块内的索引 (0~255)
  if(bn < NINDIRECT){
    // 检查间接块指针是否已分配
    if((addr = ip->addrs[NDIRECT]) == 0){
      // 未分配则申请新磁盘块作为间接块
      addr = balloc(ip->dev);
      if(addr == 0)
        return 0;         // 磁盘空间不足
      ip->addrs[NDIRECT] = addr; // 更新inode的间接块指针
    }
    
    // 读取间接块内容到缓冲区
    bp = bread(ip->dev, addr);
    a = (uint*)bp->data;  // 将缓冲区转换为uint数组
    
    // 检查目标块是否已分配
    if((addr = a[bn]) == 0){
      // 未分配则申请新磁盘块
      addr = balloc(ip->dev);
      if(addr){
        a[bn] = addr;      // 更新间接块映射表
        log_write(bp);     // 记录日志确保持久化（写入间接块修改）
      }
    }
    brelse(bp);           // 释放间接块缓冲区
    return addr;          // 返回物理块地址
  }

  // 处理二级间接块 (bn 267~65802)
  struct buf *bp1, *bp2;         // 缓冲区指针，用于读取一级间接块bp1,二级间接块bp2
  uint *a1, *a2;                 // a1: 一级间接块数组指针， a2: 二级间接块数组指针
  bn -= NINDIRECT;
  if(bn < NINDIRECTDOUBLE){
    // 检查间接块指针是否已分配
    if((addr = ip->addrs[NDIRECT+1]) == 0){
      // 未分配则申请新磁盘块作为间接块
      addr = balloc(ip->dev);
      if(addr == 0)
        return 0;         // 磁盘空间不足
      ip->addrs[NDIRECT+1] = addr; // 更新inode的一级间接块指针
    }
    
    // 读取一级间接块内容到一级间接块缓冲区
    bp1 = bread(ip->dev, addr);
    a1 = (uint*)bp1->data;  // 将缓冲区转换为uint数组

    uint bn1 = bn / NINDIRECT;     //一级间接块inode索引
    if((addr = a1[bn1]) == 0){
      addr = balloc(ip->dev);
      if(addr){
        a1[bn1] = addr;     // 更新间接块映射表
        log_write(bp1);     // 记录日志确保持久化（写入间接块修改）
      }else{
        brelse(bp1); 
        return 0;
      }
    }

    brelse(bp1);           // 释放间接块缓冲区

    // 读取二级间接块内容到二级缓冲区
    bp2 = bread(ip->dev, addr);
    a2 = (uint*)bp2->data;  // 将缓冲区转换为uint数组

    uint bn2 = bn % NINDIRECT;    //二级间接块inode索引
    if((addr = a2[bn2]) == 0){
      addr = balloc(ip->dev);
      if(addr){
        a2[bn2] = addr;
        log_write(bp2);
      }
    }

    brelse(bp2);
    return addr;
  }
  // 逻辑块号超出最大范围 (NDIRECT + NINDIRECT + NINDIRECTDOUBLE)
  panic("bmap: out of range");
}

// Truncate inode (discard contents).
// Caller must hold ip->lock.
void
itrunc(struct inode *ip)
{
  int i, j;
  struct buf *bp;
  uint *a;

  //释放直接块，处理文件的前 12 个数据块
  for(i = 0; i < NDIRECT; i++){
    if(ip->addrs[i]){
      bfree(ip->dev, ip->addrs[i]);
      ip->addrs[i] = 0;  //清除指针
    }
  }

  //释放一级间接块
  if(ip->addrs[NDIRECT]){
    //读取一级间接块
    bp = bread(ip->dev, ip->addrs[NDIRECT]);
    a = (uint*)bp->data;  //将buf*转换成uint*
    //释放所有一级间接块
    for(j = 0; j < NINDIRECT; j++){
      if(a[j])
        bfree(ip->dev, a[j]);  //bfree 释放物理磁盘块，释放块位图
    }
    brelse(bp);  // 释放磁盘块缓存
    bfree(ip->dev, ip->addrs[NDIRECT]);
    ip->addrs[NDIRECT] = 0;
  }

  //释放二级间接块
  struct buf *bp1, *bp2;         // 缓冲区指针，用于读取一级间接块bp1,二级间接块bp2
  uint *a1, *a2;                 // a1: 一级间接块数组指针， a2: 二级间接块数组指针

  if(ip->addrs[NDIRECT+1]){
    //读取一级间接块
    bp1 = bread(ip->dev, ip->addrs[NDIRECT+1]);
    a1 = (uint*)bp1->data;  //将buf*转换成uint*
    for(i = 0; i < NINDIRECT; ++i){
      if(a1[i]){
        //读取二级间接块
        bp2 = bread(ip->dev, a1[i]);
        a2 = (uint*)bp2->data;  //将buf*转换成uint*
        //释放所有二级间接块
        for(j = 0; j < NINDIRECT; j++){
          if(a2[j])
            bfree(ip->dev, a2[j]);  //bfree 释放物理磁盘块，释放块位图
        }
        brelse(bp2);  // 释放二级磁盘块缓存
        bfree(ip->dev, a1[i]);
      }
    }
    brelse(bp1);  // 释放一级磁盘块缓存
    ip->addrs[NDIRECT+1] = 0;
  }

  ip->size = 0;
  iupdate(ip); //更新磁盘inode
}

// 从 inode 复制文件状态信息到用户空间的 stat 结构体，
// 为文件系统调用（如 fstat, stat）提供核心数据支持。该函数是文件元数据查询的关键实现
void
stati(struct inode *ip, struct stat *st)
{
  st->dev = ip->dev;
  st->ino = ip->inum;
  st->type = ip->type;
  st->nlink = ip->nlink;
  st->size = ip->size;
}

//从 inode 对应的文件中读取数据，支持从内核空间或用户空间的目标地址读取。该函数是文件系统读取操作的核心实现。
// `ip`: 指向要读取的文件的 inode 的指针。
// `user_dst`: 标志位，如果为1，表示目标地址 `dst` 是用户空间的虚拟地址；否则为内核地址。
//  `dst`: 目标地址，数据将被复制到这个地址。根据 `user_dst` 的不同，可能是用户空间或内核空间的地址。
//  `off`: 文件内的偏移量（字节偏移），表示从文件的哪个位置开始读取。
// `n`: 要读取的字节数。
// 返回值：实际读取的字节数
int
readi(struct inode *ip, int user_dst, uint64 dst, uint off, uint n)
{
  uint tot, m;
  struct buf *bp;

  if(off > ip->size || off + n < off)  // 偏移越界或整数溢出
    return 0;
  if(off + n > ip->size)               // 读取范围超出文件大小
    n = ip->size - off;                // 修正为有效字节数

  for(tot=0; tot<n; tot+=m, off+=m, dst+=m) {  //此函数高效处理了跨块读取和边界对齐问题
    // 步骤1：获取磁盘块地址
    uint addr = bmap(ip, off/BSIZE);  // 计算逻辑块号
    
    // 步骤2：处理未分配块（文件空洞）
    if(addr == 0)
      break;  // 遇到未分配块立即停止读取
    
    // 步骤3：读取物理块到缓冲区
    bp = bread(ip->dev, addr);
    
    // 步骤4：计算本次读取量 m = min(剩余请求, 当前块可用数据)
    m = min(n - tot, BSIZE - off%BSIZE);  // 考虑块边界和剩余请求
    
    // 步骤5：数据复制到目标空间
    if(either_copyout(user_dst, dst, bp->data + (off % BSIZE), m) == -1) {
      brelse(bp);
      tot = -1;  // 复制失败标记
      break;
    }
    
    // 步骤6：释放缓冲区
    brelse(bp);
  }
  return tot;
}

// 将数据写入 inode 表示的文件，支持向内核空间或用户空间的源地址读取数据。该函数是文件系统写入操作的核心实现，处理文件扩展、块分配和数据复制等关键操作。
// `ip`: 目标文件的 inode 指针。
// `user_src`: 标志，指示源数据地址 `src` 是用户空间地址（1）还是内核空间地址（0）。
// `src`: 源数据地址，可以是用户空间或内核空间的虚拟地址。
// `off`: 文件内的写入偏移量（字节）。
// `n`: 要写入的字节数。
// 返回值：成功写入的字节数，出错时返回 -1。
int
writei(struct inode *ip, int user_src, uint64 src, uint off, uint n)
{
  uint tot, m;
  struct buf *bp;

  // 检查1：偏移越界或整数溢出
  if(off > ip->size || off + n < off)
    return -1;
    
  // 检查2：写入范围超过最大文件限制
  if(off + n > MAXFILE*BSIZE)
    return -1;

  for(tot=0; tot<n; tot+=m, off+=m, src+=m) {
    // 步骤1：获取磁盘块地址
    uint addr = bmap(ip, off/BSIZE);
    if(addr == 0)
      break;  // 块分配失败时终止

    // 步骤2：读取目标块到缓冲区
    bp = bread(ip->dev, addr);
    
    // 步骤3：计算本次写入量
    m = min(n - tot, BSIZE - off%BSIZE);  // 块边界处理
    
    // 步骤4：数据复制到缓冲区
    if(either_copyin(bp->data + (off % BSIZE), user_src, src, m) == -1) {
      brelse(bp);
      break;  // 复制失败时终止
    }
    
    // 步骤5：日志写入（事务保护）
    log_write(bp);
    
    // 步骤6：释放缓冲区
    brelse(bp);
  }

  if(off > ip->size)
    ip->size = off;  // 更新inode中的文件大小

  // write the i-node back to disk even if the size didn't change
  // because the loop above might have called bmap() and added a new
  // block to ip->addrs[].
  iupdate(ip);

  return tot;
}

// Directories

int
namecmp(const char *s, const char *t)
{
  return strncmp(s, t, DIRSIZ);
}

// 在目录中查找指定名称的文件/子目录，返回对应的 inode。
// 这是文件系统路径解析的核心函数，用于实现 open、stat 等系统调用。
// - `dp`：指向目录 inode 的指针。该目录将被搜索。
// - `name`：要查找的文件名（字符串）。
// - `poff`：一个可选的输出参数，用于返回找到的目录项在目录文件中的偏移量。如果调用者不需要，可以为0。
// ### 返回值
// - 如果找到，返回对应文件的 inode 指针（未上锁）。
// - 如果没有找到，返回0。
struct inode*
dirlookup(struct inode *dp, char *name, uint *poff)
{
  uint off, inum;
  struct dirent de;

  if(dp->type != T_DIR)
    panic("dirlookup not DIR");  //输入 inode 必须是目录类型（T_DIR）

  for(off = 0; off < dp->size; off += sizeof(de)){  //遍历范围：整个目录文件（dp->size）
    // 读取目录项
    if(readi(dp, 0, (uint64)&de, off, sizeof(de)) != sizeof(de))
      panic("dirlookup read");

    // 跳过空闲条目
    if(de.inum == 0)
      continue;

    // 遍历范围：整个目录文件（dp->size）
    if(namecmp(name, de.name) == 0){
      if(poff)
        *poff = off;          // 返回条目偏移
      inum = de.inum;         // 提取 inode 编号
      return iget(dp->dev, inum); // 返回未锁定的 inode
    }
  }

  return 0;
}

// 在目录 dp 中创建新的目录项，将文件名 name 与 inode 编号 inum 关联，实现文件名到文件的映射。
// - 参数：
//   - `dp`：指向目标目录的 inode 的指针。
//   - `name`：要创建的文件名（字符串）。
//   - `inum`：要关联的 inode 号。
// - 返回值：成功返回0，失败返回-1。
int
dirlink(struct inode *dp, char *name, uint inum)
{
  int off;
  struct dirent de;
  struct inode *ip;

  // Check that name is not present.
  if((ip = dirlookup(dp, name, 0)) != 0){
    iput(ip);
    return -1;
  }

  // Look for an empty dirent. 寻找空闲目录槽位
  for(off = 0; off < dp->size; off += sizeof(de)){
    if(readi(dp, 0, (uint64)&de, off, sizeof(de)) != sizeof(de))
      panic("dirlink read");
    if(de.inum == 0)
      break;
  }

  //构造目录项
  strncpy(de.name, name, DIRSIZ);
  de.inum = inum;
  if(writei(dp, 0, (uint64)&de, off, sizeof(de)) != sizeof(de))  //持久化写入
    return -1;

  return 0;
}

// Paths

// Copy the next path element from path into name.
// Return a pointer to the element following the copied one.
// The returned path has no leading slashes,
// so the caller can check *path=='\0' to see if the name is the last one.
// If no name to remove, return 0.
//
// Examples:
//   skipelem("a/bb/c", name) = "bb/c", setting name = "a"
//   skipelem("///a//bb", name) = "bb", setting name = "a"
//   skipelem("a", name) = "", setting name = "a"
//   skipelem("", name) = skipelem("////", name) = 0
//
static char*
skipelem(char *path, char *name)
{
  char *s;
  int len;

  while(*path == '/')
    path++;
  if(*path == 0)
    return 0;
  s = path;
  while(*path != '/' && *path != 0)
    path++;
  len = path - s;
  if(len >= DIRSIZ)
    memmove(name, s, DIRSIZ);
  else {
    memmove(name, s, len);
    name[len] = 0;
  }
  while(*path == '/')
    path++;
  return path;
}

// 根据路径名查找对应的 inode（索引节点）。可选返回父目录的 inode 和路径的最后一个分量（文件名/目录名）。
// 此函数必须在事务中调用，因为它可能调用 iput() 释放 inode。
// 参数：
// - path：输入的路径字符串（如 /a/b/c）。
// - nameiparent：标志位。若为真：
//   返回父目录的 inode（而非目标 inode）。
//   将最后一个路径分量复制到 name 缓冲区。
// - name：指向缓冲区的指针，用于存储最后一个路径分量（需至少 DIRSIZ 字节空间）。
static struct inode*
namex(char *path, int nameiparent, char *name)
{
  struct inode *ip, *next;

  if(*path == '/')
    ip = iget(ROOTDEV, ROOTINO);  // 如果路径以 / 开头（绝对路径），则从根目录的 inode 开始查找
  else
    ip = idup(myproc()->cwd);     // 否则（相对路径），从当前进程的工作目录的 inode 开始查找。

  while((path = skipelem(path, name)) != 0){
    ilock(ip);
    if(ip->type != T_DIR){
      iunlockput(ip);
      return 0;
    }
    if (nameiparent && *path == '\0') {
        iunlock(ip);    // 解锁但不释放（父目录 inode 需返回）
        return ip;      // 返回父目录 inode
    }
    if((next = dirlookup(ip, name, 0)) == 0){
      iunlockput(ip);
      return 0;
    }
    iunlockput(ip);
    ip = next;
  }
  if (nameiparent) {
      iput(ip); // 释放 inode
      return 0; // 返回失败
  }
  return ip;
}

struct inode*
namei(char *path)
{
  char name[DIRSIZ];
  return namex(path, 0, name);
}

struct inode*
nameiparent(char *path, char *name)
{
  return namex(path, 1, name);
}
