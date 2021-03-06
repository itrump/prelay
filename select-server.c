/*
 * reference:
 *  - http://pubs.opengroup.org/onlinepubs/9699919799/functions/accept.html 
 *  - https://www.ibm.com/support/knowledgecenter/en/SSLTBW_2.1.0/com.ibm.zos.v2r1.hala001/orgblockasyn.htm
 *  - http://www.scs.stanford.edu/nyu/03sp/lab/tcpproxy.html
 *  - http://www.partow.net/programming/tcpproxy/index.html
 **/
// TODO
//  - resolve problems on mobile devices

#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include <sys/time.h>
#include <time.h>
#include <signal.h>

#define BUF_LEN (1024 * 1024)
#define SERV_PORT 8993
#define FD_SIZE 100
#define TIME_FORMAT "%Y-%m-%d %H:%M:%S"

#define LOGI(format, ...)                                                        \
{                                                                         \
       time_t now = time(NULL);                                             \
       struct timeval t_now;                                         \
       gettimeofday(&t_now, NULL);                                     \
       char timestr[32];                                                    \
       strftime(timestr, 32, TIME_FORMAT, localtime(&now));                  \
       printf(" %s.%ld [prelay] INFO: " format "\n", timestr, t_now.tv_usec, ## __VA_ARGS__); \
}

#define print(format, ...)   \
    printf(format, __VA_ARGS__); \
/*
 * Notes
 *  An fd_set is a fixed size buffer. 
 * Executing FD_CLR() or FD_SET() with a value of fd that  
 * is negative or is equal to or larger than
 * FD_SETSIZE will result in undefined behavior.
 * Moreover, POSIX requires fd to be a valid file descriptor.
 */

// conf of obfs local
const static int obfs_local_port = 8992;
const static char* obfs_local_ip = "127.0.0.1";
const static int CTX_SIZE = 32;
const char* LOG_FILE = "prelay.log";

int build_obfs_conn() {
    struct sockaddr_in obfs_serv_addr;
    int obfs_connfd;
    // connect to obfs local
    if ((obfs_connfd = socket(AF_INET, SOCK_STREAM, 0)) == -1) {
        LOGI( "Create obfs socket Error : %d\n", errno );
        return -1;
    }
    bzero(&obfs_serv_addr, sizeof(obfs_serv_addr));
    obfs_serv_addr.sin_family = AF_INET;
    obfs_serv_addr.sin_port = htons(obfs_local_port);
    inet_pton(AF_INET, obfs_local_ip, &obfs_serv_addr.sin_addr);

    if (connect(obfs_connfd, (struct sockaddr*)&obfs_serv_addr, 
            sizeof(obfs_serv_addr)) < 0) {
        LOGI( "connect to obfs local Error : %d,%m\n", errno);
        return -1;
    }
    LOGI("build obfs sock[%d]\n", obfs_connfd);
    return obfs_connfd;
}
void install_signal() {
    signal(SIGPIPE, SIG_IGN);
    signal(SIGABRT, SIG_IGN);
}

struct port_relay_ctx_t {
    int obfs_sock; // obfs sock fd
    int client_sock; // client sock fd
    int status;      // mark idle or not, 0 is idle, 1 busy
};

int init_ctx(struct port_relay_ctx_t* p_ctx, int size) {
    int i = 0, tsock = -1;
    for (i = 0 ; i < size; ++i) {
        tsock = build_obfs_conn();
        if (tsock < 0) {
            LOGI("get socket[%d] error!\n", tsock);
            return -1;
        }
        p_ctx->obfs_sock = tsock;
        p_ctx->client_sock = -1;
        p_ctx->status = 0;
        p_ctx++;
    }
    return 0;
}

int init_port_relay(struct port_relay_ctx_t* p_ctx, int size) {
    // ignore signals
    install_signal();
    if (init_ctx(p_ctx, size) < 0) {
        return -1;
    }

    return 0;
}

int serve_prepare(fd_set* fset, int* maxfd, struct port_relay_ctx_t* p_ctx,
        int size) {
    int i = 0;
    for (i = 0; i < size; ++i) {
        if (*maxfd < p_ctx->obfs_sock) {
            *maxfd = p_ctx->obfs_sock;
        }
        // add to set
        FD_SET(p_ctx->obfs_sock, fset);
        p_ctx++;
    }
    LOGI("server prepared ok.\n");
    return 0;
}

int rebuild_obfs_sock(fd_set* fdset,
        int obfs_sock, struct port_relay_ctx_t* p_ctx, int size) {
    int i = 0;
    int new_sock = -1;
    struct port_relay_ctx_t* ptr = p_ctx;
    for (i = 0; i < size; ++i) {
        LOGI("get obfs sock[%d] ctx sock[%d]\n",
                obfs_sock, ptr->obfs_sock);
        if (obfs_sock != ptr->obfs_sock) { 
            // be careful not forget
            ptr++;
            continue;
        }
        // release obfs sock
        FD_CLR(obfs_sock, fdset);
        close(obfs_sock);
        ptr->client_sock = -1;
        ptr->status = 0;
        ptr->obfs_sock = build_obfs_conn();
        if (ptr->obfs_sock < 0) {
            LOGI("build obfs sock[%d] failed.", ptr->obfs_sock);
            return -1;
        }
        new_sock = ptr->obfs_sock;
        FD_SET(ptr->obfs_sock, fdset);
        // update max sockfd
        // be careful not forget
        ptr++;
        // build new obfs sock success
        break;
    }
    if (i >= size) {
        LOGI("[%d] not found!\n", obfs_sock);
        return -1;
    }
    LOGI("[%d] rebuild sock[%d] ok.", obfs_sock, new_sock);
    return 0;
}

int write_to_obfs(int client_sock, struct port_relay_ctx_t* p_ctx, 
        int size, char* buf, ssize_t buf_size) {
    int i = 0, write_size = -1, j = 0;
   //LOGI("begin write to obfs, client sock %d.", client_sock);
    struct port_relay_ctx_t* ptr_ctx = p_ctx;
    for (i = 0; i < size; ++i) {
        // exists session
        if (ptr_ctx->client_sock == client_sock) {
            LOGI("old session, begin write to obfs sock\n");
            write_size = write(ptr_ctx->obfs_sock, buf, buf_size);
            LOGI("old sesion, client sock[%d] obfs sock[%d] write size[%d] errno[%m]\n", 
                    client_sock, ptr_ctx->obfs_sock, write_size);
            break;
        } 
        ptr_ctx++;
    }
    // first time, relate two socket.
    if (i >= size) {
        ptr_ctx = p_ctx;
        for (j = 0; j < size; ++j) {
            // find an idle obfs socket
            LOGI("get ptr[%p]\t", ptr_ctx);
            if (ptr_ctx->client_sock < 0 && ptr_ctx->status == 0) {
                write_size = write(ptr_ctx->obfs_sock, buf, buf_size);
                LOGI("first time, client sock[%d] obfs sock[%d] write size[%d] errno[%m]\n", 
                        client_sock, ptr_ctx->obfs_sock, write_size);
                // relate two socket and set status to busy
                ptr_ctx->client_sock = client_sock;
                ptr_ctx->status = 1;
                break;
            }
            ptr_ctx++;
        }
        LOGI("\n");
        // after traverse, no idel one
        if (j >= size) {
            LOGI("j[%d] size[%d] no idel socket!\n", j, size);
            return -1;
        }
    }
    return 0;
}

int destroy_client_sock(fd_set* aset, int obfs_sock,
        struct port_relay_ctx_t * p_ctx, int size) {
    int i = 0, tsock = -1, csock = -1;
    struct port_relay_ctx_t* ptr = p_ctx;
    for (int i = 0; i < size; ++i, ptr++) {
        tsock = ptr->obfs_sock;
        csock = ptr->client_sock;
        if (tsock != obfs_sock) {
            continue;
        }
        // release close client sock
        if (csock >= 0) {
            LOGI("clear and close client sock[%d]", csock);
            FD_CLR(ptr->client_sock, aset);
            close(ptr->client_sock);
            ptr->status = 0;
        }
        break;
    }
    return 0;
}

int read_obfs_data(fd_set* rset, fd_set* allset, 
        struct port_relay_ctx_t * p_ctx, int size, 
        ssize_t* obfs_max_read_size) {
    int i = 0, tsock = -1, csock = -1;
    char         obfs_buf[BUF_LEN];               
    ssize_t read_len = 0;
    int write_size = -1;
    LOGI("enter read obfs data\n");
    struct port_relay_ctx_t * ptr = p_ctx;
    for (i = 0; i < size; ++i) {
        tsock = ptr->obfs_sock;
        csock = ptr->client_sock;
        LOGI("traverse obfs sock[%d] client sock[%d]\t", tsock, csock);
        if (FD_ISSET(tsock, rset)) {
            LOGI("get a ready obfs sock[%d]\n", tsock);
            memset(obfs_buf, 0, sizeof(obfs_buf));
            read_len = read(tsock, obfs_buf, BUF_LEN);
            LOGI("get data from obfs server, read len[%d]\n", (int)read_len);
            // obfs sock disconnected
            if (read_len == 0 || read_len < 0) {
                LOGI("[%d] get [%lu] bytes from obfs, Data size is 0, maybe closed or error.\n", 
                        tsock, read_len);
                if (rebuild_obfs_sock(allset, tsock, ptr, CTX_SIZE) < 0) {
                    LOGI("rebuild obfs sock failed.");
                    return -1;
                }
            // get data succ
            } else {
                // TODO check csock valid
                if (csock < 0) {
                    LOGI("get csock[%d] unexpected, quit write obfs to client, size[%lu]", 
                            csock, read_len);
                    continue;
                }
                write_size = write(csock, obfs_buf, read_len);
                LOGI("get from obfs, write to client sock[%d] data size:%d, errno:%m\n", 
                        csock, write_size);
                // write fail
                if (write_size < 0) {
                    if (destroy_client_sock(allset, tsock, ptr, CTX_SIZE) < 0) {
                        LOGI("write size < 0, rebuild obfs sock failed.");
                        return -1;
                    }
                    errno = 0;
                    LOGI("reset errno to %d", errno);
                }
                if (read_len > *obfs_max_read_size) {
                    *obfs_max_read_size = read_len;
                }
            }
        }
        ptr++;
    }
    return 0;
}

int destroy_client_session(struct port_relay_ctx_t* p_ctx, 
        int size, int client_sock) {
    int i = 0, csock = -1;
    struct port_relay_ctx_t* ptr_ctx = p_ctx;
    for (i = 0; i < size; ++i, ptr_ctx++) {
        csock = ptr_ctx->client_sock;
        LOGI("traverse client sock[%d], try find [%d]\n", 
                csock, client_sock);
        if (csock != client_sock) {
            continue;
        }
        // release this client sock and idle it
        LOGI("remove ctx client sock[%d], target sock[%d], and set idle\n", 
                ptr_ctx->client_sock, client_sock);
        ptr_ctx->client_sock = -1;
        ptr_ctx->status = 0;
        break;
    }
    return 0;
}

int update_maxfd(struct port_relay_ctx_t* p_ctx, int size, int* maxfd) {
    int i = 0;
    struct port_relay_ctx_t* ptr = p_ctx;
    for (i = 0; i < size; ++i) {
        if (ptr->obfs_sock > *maxfd) {
            LOGI("update maxfd from[%d] to [%d]",
                    *maxfd, ptr->obfs_sock);
            *maxfd = ptr->obfs_sock;
        }
        if (ptr->client_sock > *maxfd) {
            LOGI("update maxfd from[%d] to [%d]",
                    *maxfd, ptr->client_sock);
            *maxfd = ptr->client_sock;
        }
        ptr++;
    }
    return 0;
}

// In blocking mode, the recv, send, 
// connect (TCP only) and accept (TCP only) 
// socket API calls will block indefinitely until 
// the requested action has been performed. 
// In non-blocking mode, these functions return immediately

int set_fd_nonblocking(int fd) {
    int flags;
    flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) {
        LOGI("get flags[%d], change to 0", flags);
        flags = 0;
    }
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

int main( int argc, char ** argv )
{
    struct port_relay_ctx_t ctx[CTX_SIZE];
    int             listenfd, connfd, sockfd, maxfd, maxi, i, j;
    int             nready = 0, client[FD_SIZE];        //!> 接收select返回值、保存客户端套接字
    struct timeval  client_tv[FD_SIZE]; // record active timestamp
    int             lens;
    ssize_t     n, max_read_size = 0, obfs_max_read_size = 0;                //!> read字节数
    fd_set        rset, allset;    //!> 不要理解成就只能保存一个，其实fd_set有点像封装的数组
    char         buf[BUF_LEN];               
    socklen_t    clilen;
    struct sockaddr_in servaddr;
    struct sockaddr chiaddr;
    int opt = 1;
    int client_sock = -1;
    int write_size = 0;
    int loop_count = 0;

    LOGI("server begin init.");
    if (init_port_relay(ctx, CTX_SIZE) < 0) {
        LOGI("init failed.\n");
        return -1;
    }

   
    LOGI("server begin listen.");
    if( ( listenfd = socket( AF_INET, SOCK_STREAM, 0 ) ) == -1 )
    {
        LOGI( "Create socket Error : %d\n", errno );
        exit( EXIT_FAILURE );
    }
    if (setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, 
            (char *)&opt, sizeof(opt)) < 0) {
        LOGI( "setsocketopt failed.: %d\n", errno );
        exit( EXIT_FAILURE );
    }

    set_fd_nonblocking(listenfd);
   
    //!>
    //!> 下面是接口信息
    bzero( &servaddr, sizeof( servaddr ) );
    servaddr.sin_family = AF_INET;
    servaddr.sin_addr.s_addr  =htonl( INADDR_ANY );
    servaddr.sin_port = htons( SERV_PORT );
   
    //!>
    //!> 绑定
    if( bind( listenfd, ( struct sockaddr * )&servaddr, sizeof( servaddr ) ) == -1 )
    {   
        LOGI("Bind Error : %d\n", errno);
        exit(EXIT_FAILURE  );
    }
   
    //!>
    //!> 监听
    if( listen( listenfd, SOMAXCONN) == -1 )
    {
        LOGI("Listen Error : %d\n", errno );
        exit( EXIT_FAILURE );
    }
   
    //!> 当前最大的感兴趣的套接字fd
    maxfd = listenfd;    
    //!> 当前可通知的最大的fd
    maxi = -1;            //!> 仅仅是为了client数组的好处理
   
    for( i = 0; i < FD_SIZE; i++ )    //!> 首先置为全-1
    {
        client[i] = -1;        //!> 首先client的等待队列中是没有的，所以全部置为-1
        client_tv[i].tv_sec = 0;
        client_tv[i].tv_usec = 0;
    }
   
    FD_ZERO(&allset);        //!> 先将其置为0
    FD_SET(listenfd, &allset);
                    //!> 说明当前我对此套接字有兴趣，下次select的时候通知我！
    //LOGI("server begin prepare.");
    if (serve_prepare(&allset, &maxfd, ctx, CTX_SIZE) < 0) {
        LOGI("serve prepare failed.\n");
        return -1;
    }
    //LOGI("server begin loop.");
    while( 1 )
    {
        loop_count++;
        if (loop_count > 20) {
            // break;
        }
        // update_maxfd(ctx, CTX_SIZE, &maxfd);
        LOGI("max fd[%d] max read size[%lu] obfs max read size[%lu]\n", 
                maxfd, max_read_size, obfs_max_read_size);
        //LOGI("server loop %d.", loop_count);
        LOGI("server loop %d, select return %d\n", loop_count, nready);
        rset = allset;//!> 由于allset可能每次一个循环之后都有变化，所以每次都赋值一次
        //LOGI("server begin select.");
        if( (nready = select( maxfd + 1, &rset, NULL, NULL, NULL )) == -1)
        {                    //!> if 存在关注
            LOGI("Select Erorr : %d\n", errno );
            //exit( EXIT_FAILURE );
            return -1;
        }
       
        if( nready <= 0 )            //!> if 所有的感兴趣的没有就接着回去select
        {
            continue;
        }
        //LOGI("select return %d.", nready);
        print("select return %d.\n", nready);
       

        //LOGI("server read obfs data.");
        // read obfs data
        if (read_obfs_data(&rset, &allset, ctx, CTX_SIZE, &obfs_max_read_size) < 0) {
            LOGI("read obfs data error.\n");
            break;
        }
       
       
        //LOGI("read client sock data.");
        LOGI("read client sock data.\n");
        if( FD_ISSET( listenfd, &rset ) )            //!> if 是监听接口上的“来电”
        {                                            //!>
            //!> LOGI("server listen ...\n");
            clilen = sizeof( chiaddr );
           
            LOGI("Start doing... \n");
           
               if( ( connfd  = accept( listenfd, (struct sockaddr *)&chiaddr, &clilen ) ) == -1 )
               {                                        //!> accept 返回的还是套接字
                   LOGI( "Accept Error : %d\n", errno );
                   continue;
               }
               //LOGI("client connect fd:%d.", connfd);
               char ip_str[INET6_ADDRSTRLEN];
               // ipv4 ok, while ipv6 differs
               struct sockaddr_in* addr_in = (struct sockaddr_in*)&chiaddr;
               LOGI("get connect from client[%s:%u]\n", 
                       inet_ntoa(addr_in->sin_addr), 
                       ntohs(addr_in->sin_port));
              
               // remove timeout client sock
               for (j = 0; j < FD_SIZE; j++) {
                   struct timeval time_now;
                   int time_out_sec = 10;
                   gettimeofday(&time_now, NULL);
                   LOGI("processing client sock[%d]\t", client[j]);
                   if (client[j] >= 0 && time_now.tv_sec > client_tv[j].tv_sec + time_out_sec) {
                       LOGI("get a time out client sock[%d]=%d\n", j, client[j]);
                       close( client[j]);
                       FD_CLR( client[j], &allset );
                       client[j] = -1;
                       LOGI("close client sock[%d]=%d\n", j, client[j]);
                   }
               }
               LOGI("\n");
              
               for( i = 0; i < FD_SIZE; i++ )    //!> 注意此处必须是循环，刚开始我认
                                                   //!> 为可以直接设置一个end_i来直接处
                                                   //!> 理,实质是不可以的！因为每个套接
               {                                    //!> 字的退出时间是不一样的，后面的
                   if( client[i] < 0 )                //!> 可能先退出，那么就乱了，所以只
                   {                                //!> 有这样了！
                       LOGI("set client[%d]=%d\n", i, connfd);
                       client[i] = connfd;            //!> 将client的请求连接保存
                       // record active time
                       gettimeofday(&(client_tv[i]), NULL);
                       break;
                   }
               }
              
               if( i == FD_SIZE )                //!> The last one
               {
                   LOGI( "Too many client connect... close current connfd[%d]\n", connfd);
                   close( connfd );            //!> if 满了那么就不连接你了，关闭吧
                continue;                    //!> 返回
               }
                                            //!> listen的作用就是向数组中加入套接字！
            LOGI("put connfd[%d] to allset\n", connfd);
            FD_SET( connfd, &allset );    //!> 说明现在对于这个连接也是感兴趣的！
                                            //!> 所以加入allset的阵容
            if( connfd > maxfd )            //!> 这个还是为了解决乱七八糟的数组模型
                                            //!> 的处理
            {
                maxfd = connfd;
            }
           
            if( i > maxi )                    //!> 同上
            {
                maxi = i;
            }
        }

        // process user in fd
        //!> 下面就是处理数据函数
        for( i = 0; i <= maxi; i++ )        //!> 对所有的连接请求的处理
        {
            if( ( sockfd = client[i] ) > 0 )    //!> 还是为了不规整的数组
            {            //!> 也就说client数组不是连续的全正数或者-1，可能是锯齿状的
                if( FD_ISSET( sockfd, &rset ) )    //!> if 当前这个数据套接字有要读的
                 {
                     LOGI("get an active fd[%d]\n", sockfd);
                     memset( buf, 0, sizeof( buf ) );    //!> 此步重要，不要有时候出错
                
                     n = read( sockfd, buf, BUF_LEN);
                     if( n < 0 )
                     {
                         LOGI("Error!\n");
                         close( sockfd );            //!> 说明在这个请求端口上出错了！
                        FD_CLR( sockfd, &allset );
                        LOGI("read size < 0, close client[%d] fd[%d]\n", i, sockfd);
                        client[i] = -1;
                        continue;
                     }
                    if( n == 0 )
                    {
                        LOGI("client sock[%d] no data. closed.\n", sockfd);
                        close( sockfd );            //!> 说明在这个请求端口上读完了！
                        destroy_client_session(ctx, CTX_SIZE, sockfd);
                        FD_CLR( sockfd, &allset );
                        LOGI("no data. close client[%d] fd[%d]\n", i, sockfd);
                        client[i] = -1;
                        continue;
                    }
                    if (n > max_read_size) {
                        max_read_size = n;
                    }
                   
                    // LOGI("Server Recv: %s\n", buf);
                    LOGI("Server Recv data size: %lu, from fd:%d\n", 
                            n, sockfd);
                   
                    if( strcmp( buf, "q" ) == 0 )                //!> 客户端输入“q”退出标志
                    {
                        close( sockfd );
                        FD_CLR( sockfd, &allset );
                        client[i] = -1;
                        continue;
                    }
                   
                    LOGI("Server receive ss data size: %lu\n", n);
                    // write( sockfd, buf, n );        //!> 读出来的写进去

                    // obfs process
                    write_to_obfs(sockfd, ctx, CTX_SIZE, buf, n);
                }
            }
        }
       
    }
    //LOGI("close conn and exit...");
    close(listenfd);
    // TODO close all connect fd
   
    return 0;
}

