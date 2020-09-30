#include "lib/common.h"

#define MAX_LINE 1024
#define FD_INIT_SIZE 128

char rot13_char(char c) {
  if ((c >= 'a' && c <= 'm') || (c >= 'A' && c <= 'M'))
    return c + 13;
  else if ((c >= 'n' && c <= 'z') || (c >= 'N' && c <= 'Z'))
    return c - 13;
  else
    return c;
}

//数据缓冲区
struct Buffer {
  int connect_fd;  //连接字
  char buffer[MAX_LINE];  //实际缓冲
  size_t writeIndex;    //缓冲写入位置
  size_t readIndex;     //缓冲读取位置
  int readable;       //是否可以读
};


//分配一个Buffer对象，初始化writeIdnex和readIndex等
struct Buffer *alloc_Buffer() {
  struct Buffer *buffer = malloc(sizeof(struct Buffer));
  if (!buffer)
    return NULL;
  buffer->connect_fd = 0;
  buffer->writeIndex = buffer->readIndex = buffer->readable = 0;
  return buffer;
}

//释放Buffer对象
void free_Buffer(struct Buffer *buffer) {
  free(buffer);
}

//这里从fd套接字读取数据，数据先读取到本地buf数组中，再逐个拷贝到buffer对象缓冲中
int onSocketRead(int fd, struct Buffer *buffer) {
  char buf[1024];
  int i;
  ssize_t result;
  while (1) {
    result = recv(fd, buf, sizeof(buf), 0);
    if (result <= 0)
      break;

    //按char对每个字节进行拷贝，每个字节都会先调用rot13_char来完成编码，之后拷贝到buffer对象的缓冲中，
    //其中writeIndex标志了缓冲中写的位置
    for (i = 0; i < result; ++i) {
      if (buffer->writeIndex < sizeof(buffer->buffer))
        buffer->buffer[buffer->writeIndex++] = rot13_char(buf[i]);
      //如果读取了回车符，则认为client端发送结束，此时可以把编码后的数据回送给客户端
      if (buf[i] == '\n') {
        buffer->readable = 1;  //缓冲区可以读
      }
    }
  }

  if (result == 0) {
    return 1;
  } else if (result < 0) {
    if (errno == EAGAIN)
      return 0;
    return -1;
  }

  return 0;
}

//从buffer对象的readIndex开始读，一直读到writeIndex的位置，这段区间是有效数据
int onSocketWrite(int fd, struct Buffer *buffer) {
  while (buffer->readIndex < buffer->writeIndex) {
    ssize_t result = send(fd, buffer->buffer + buffer->readIndex, buffer->writeIndex - buffer->readIndex, 0);
    if (result < 0) {
      if (errno == EAGAIN)
        return 0;
      return -1;
    }

    buffer->readIndex += result;
  }

  //readindex已经追上writeIndex，说明有效发送区间已经全部读完，将readIndex和writeIndex设置为0，复用这段缓冲
  if (buffer->readIndex == buffer->writeIndex)
    buffer->readIndex = buffer->writeIndex = 0;

  //缓冲数据已经全部读完，不需要再读
  buffer->readable = 0;

  return 0;
}


int main(int argc, char **argv) {
  int listen_fd;
  int i, maxfd;

  // buffer list初始化
  // 每个fd 关联一个 buffer，可以复用
  // buffer: writeidx表明有多少数据要写，这个是读的时候更新
  // buffer: readidx表明已经写了多少数据。需要追上readidx
  // buffer: readable表明是否读取完成，这个重要。之前强调过，tcp的分包需要处理
  struct Buffer *buffer[FD_INIT_SIZE];
  for (i = 0; i < FD_INIT_SIZE; ++i) {
    buffer[i] = alloc_Buffer();
  }

  // listener设置nonblocking
  listen_fd = tcp_nonblocking_server_listen(SERV_PORT);

  fd_set readset, writeset, exset;
  FD_ZERO(&readset);
  FD_ZERO(&writeset);
  FD_ZERO(&exset);

  while (1) {
    maxfd = listen_fd;

    FD_ZERO(&readset);
    FD_ZERO(&writeset);
    FD_ZERO(&exset);

    // listener加入readset
    FD_SET(listen_fd, &readset);

    // AddFdToPeer
    for (i = 0; i < FD_INIT_SIZE; ++i) {
      if (buffer[i]->connect_fd > 0) {

        // 更新maxfd
        if (buffer[i]->connect_fd > maxfd)
          maxfd = buffer[i]->connect_fd;

        // Add conn_fd to select
        FD_SET(buffer[i]->connect_fd, &readset);

        // 对于读取完成/业务处理完成的fd
        // 可以进行写操作
        // Add conn_fd to select
        // 写事件触发的原因：fd对应的写缓冲区为空，写事件触发(可写)，之前这里的理解一直有误
        if (buffer[i]->readable) {
          FD_SET(buffer[i]->connect_fd, &writeset);
        }
      }
    }

    if (select(maxfd + 1, &readset, &writeset, &exset, NULL) < 0) {
      error(1, errno, "select error");
    }

    // handle listener
    if (FD_ISSET(listen_fd, &readset)) {
      printf("listening socket readable\n");
      sleep(5);
      struct sockaddr_storage ss;
      socklen_t slen = sizeof(ss);
      int fd = accept(listen_fd, (struct sockaddr *) &ss, &slen);
      if (fd < 0) {
        error(1, errno, "accept failed");
      } else if (fd > FD_INIT_SIZE) {
        error(1, 0, "too many connections");
        close(fd);
      } else {
        // 这里原本应该是AddFdToSelect
        // 执行了一些更新缓冲区的操作
        make_nonblocking(fd);
        if (buffer[fd]->connect_fd == 0) {
          buffer[fd]->connect_fd = fd;
        } else {
          error(1, 0, "too many connections");
        }
      }
    }

    // handle peer
    for (i = 0; i < maxfd + 1; ++i) {
      int r = 0;
      if (i == listen_fd)
        continue;

      if (FD_ISSET(i, &readset)) {
        // handle read events
        // 由于存在tcp分包的情形,但是本次的实现没有这么做。因为是echo，读多少写多少就行。只是不阻塞而已
        r = onSocketRead(i, buffer[i]);
      }

      // r == 0, 读取部分，但是可以写
      if (r == 0 && FD_ISSET(i, &writeset)) {
        r = onSocketWrite(i, buffer[i]);
      }

      // r == 1, 读取完成，本次什么都没读到。也不用写，关闭就行
      if (r) {
        buffer[i]->connect_fd = 0;
        close(i);
      }
    }
  }
}
