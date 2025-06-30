#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "fs.h"
#include "buf.h"

// Simple logging that allows concurrent FS system calls.
//
// A log transaction contains the updates of multiple FS system
// calls. The logging system only commits when there are
// no FS system calls active. Thus there is never
// any reasoning required about whether a commit might
// write an uncommitted system call's updates to disk.
//
// A system call should call begin_op()/end_op() to mark
// its start and end. Usually begin_op() just increments
// the count of in-progress FS system calls and returns.
// But if it thinks the log is close to running out, it
// sleeps until the last outstanding end_op() commits.
//
// The log is a physical re-do log containing disk blocks.
// The on-disk log format:
//   header block, containing block #s for block A, B, C, ...
//   block A
//   block B
//   block C
//   ...
// Log appends are synchronous.

// Contents of the header block, used for both the on-disk header block
// and to keep track in memory of logged block# before commit.
struct logheader {
  int n;
  int block[LOGSIZE];
};

struct log {
  struct spinlock lock;
  int start;
  int size;
  int outstanding; // how many FS sys calls are executing.
  int committing;  // in commit(), please wait.
  int dev;
  struct logheader lh;
};
struct log log;

static void recover_from_log(void);
static void commit();

void
initlog(int dev, struct superblock *sb)
{
  if (sizeof(struct logheader) >= BSIZE)
    panic("initlog: too big logheader");

  initlock(&log.lock, "log");
  log.start = sb->logstart;
  log.size = sb->nlog;
  log.dev = dev;
  recover_from_log();  //关键恢复点
}

// Copy committed blocks from log to their home location
static void
install_trans(int recovering)
{
  int tail;

  for (tail = 0; tail < log.lh.n; tail++) {
    struct buf *lbuf = bread(log.dev, log.start+tail+1); // read log block  lbuf（log buffer）: 读取的是日志区域中的一个块。日志区域从磁盘上的log.start开始。注意，日志头（log header）占据第一个块（即log.start指向的块），而日志数据块从log.start+1开始。
    struct buf *dbuf = bread(log.dev, log.lh.block[tail]); // read dst
    memmove(dbuf->data, lbuf->data, BSIZE);  // copy block to dst 不是消除必要的内存复制，因为它是保证文件系统一致性的基础机制。与write_log中复制遥相呼应。
    bwrite(dbuf);  // write dst to disk
    if(recovering == 0)   //非恢复模式，正常模式，正常提交
      bunpin(dbuf); //减少缓冲区的引用计数，允许缓存区引用计数为0时被重用。这个操作与log中log_write中的bpin形成对称操作
    brelse(lbuf);
    brelse(dbuf);
  }
}

// Read the log header from disk into the in-memory log header
static void
read_head(void)
{
  struct buf *buf = bread(log.dev, log.start);  //缓存未命中，重新分配缓存区
  struct logheader *lh = (struct logheader *) (buf->data);
  int i;
  log.lh.n = lh->n;
  for (i = 0; i < log.lh.n; i++) {
    log.lh.block[i] = lh->block[i];
  }
  brelse(buf);
}

// Write in-memory log header to disk.
// This is the true point at which the
// current transaction commits.
static void
write_head(void)
{
  struct buf *buf = bread(log.dev, log.start);
  struct logheader *hb = (struct logheader *) (buf->data); //由于下面的修改，所以这种转换是可以的。
  int i;
  hb->n = log.lh.n;    //仅修改buf->data前4字节
  for (i = 0; i < log.lh.n; i++) {
    hb->block[i] = log.lh.block[i];   //仅修改buf->data后面的4*n字节
  }
  bwrite(buf);  // commit point
  brelse(buf);
}

static void
recover_from_log(void)
{
  read_head();  // 关键：只读取日志头，由于之前日志头和op的修改都已经写入磁盘，所以crash后可以恢复
  install_trans(1); // if committed, copy from log to disk
  log.lh.n = 0;
  write_head(); // clear the log
}

// called at the start of each FS system call.
void
begin_op(void)
{
  acquire(&log.lock);
  while(1){
    if(log.committing){  //如果日志正在提交（`log.committing`为真），则睡眠等待。日志提交是临界操作，必须独占完成
      sleep(&log, &log.lock);
    } else if(log.lh.n + (log.outstanding+1)*MAXOPBLOCKS > LOGSIZE){ //否则，如果当前操作（包括正在等待的操作）可能耗尽日志空间，则睡眠等待。当前日志占用 + (当前操作数+1)*单操作最大占用 > 日志总容量
      // this op might exhaust log space; wait for commit.
      sleep(&log, &log.lock);
    } else {  //否则，增加`log.outstanding`（表示当前未完成的操作数），释放锁并跳出循环。
      log.outstanding += 1;
      release(&log.lock);
      break;
    }
  }
}

// called at the end of each FS system call.
// commits if this was the last outstanding operation.
void
end_op(void)
{
  int do_commit = 0;

  acquire(&log.lock);
  log.outstanding -= 1;
  if(log.committing)   
    panic("log.committing");
  if(log.outstanding == 0){ //立即提交释放大量空间
    do_commit = 1;
    log.committing = 1;
  } else {
    // begin_op() may be waiting for log space,
    // and decrementing log.outstanding has decreased
    // the amount of reserved space.
    wakeup(&log);  //log.outstanding -= 1 减少了未完成操作计数,可以使得begin_op中进程唤醒，避免死锁。提高并发性
  }
  release(&log.lock);

  if(do_commit){
    // call commit w/o holding locks, since not allowed
    // to sleep with locks.
    commit();
    acquire(&log.lock);
    log.committing = 0;
    wakeup(&log);  //立即提交，唤醒进程，可以唤醒begin_op的两个分支的睡眠
    release(&log.lock);
  }
}

// Copy modified blocks from cache to log.
static void
write_log(void)
{
  int tail;

  for (tail = 0; tail < log.lh.n; tail++) {
    struct buf *to = bread(log.dev, log.start+tail+1); // log block
    struct buf *from = bread(log.dev, log.lh.block[tail]); // cache block log.lh.block[tail]表示磁盘对应的块，可以看log_write函数负责维护。
    memmove(to->data, from->data, BSIZE);
    bwrite(to);  // write the log
    brelse(from);
    brelse(to);
  }
}

static void
commit()
{
  if (log.lh.n > 0) {
    write_log();     // Write modified blocks from cache to log
    write_head();    // Write header to disk -- the real commit 标记提交，原子提交事务
    install_trans(0); // Now install writes to home locations
    log.lh.n = 0;   // 事务清理
    write_head();    // Erase the transaction from the log 标记事务已经完成
  }
}

// Caller has modified b->data and is done with the buffer.
// Record the block number and pin in the cache by increasing refcnt.
// commit()/write_log() will do the disk write.
//
// log_write() replaces bwrite(); a typical use is:
//   bp = bread(...)
//   modify bp->data[]
//   log_write(bp)
//   brelse(bp)
void
log_write(struct buf *b)
{
  int i;

  acquire(&log.lock);
  if (log.lh.n >= LOGSIZE || log.lh.n >= log.size - 1)
    panic("too big a transaction");
  if (log.outstanding < 1)  //检查当前是否有进行中的事务（log.outstanding>=1），否则panic（因为log_write应该在事务中调用）
    panic("log_write outside of trans");

  for (i = 0; i < log.lh.n; i++) {
    if (log.lh.block[i] == b->blockno)   // log absorption,避免重复记录同一个块的多次修改
      break;
  }
  log.lh.block[i] = b->blockno;
  if (i == log.lh.n) {  // Add new block to log?
    bpin(b);   //固定缓存区
    log.lh.n++;  //扩展日志
  }
  release(&log.lock);
}

