/*
 * xuehanz Xuehan Zhou
 * Starter code for proxy lab.
 * Feel free to modify this code in whatever way you wish.
 */

/* Some useful includes to help you get started */

#include "csapp.h"
#include "http_parser.h"

#include <assert.h>
#include <ctype.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/types.h>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>

/*
 * Debug macros, which can be enabled by adding -DDEBUG in the Makefile
 * Use these if you find them useful, or delete them if not
 */
#ifdef DEBUG
#define dbg_assert(...) assert(__VA_ARGS__)
#define dbg_printf(...) fprintf(stderr, __VA_ARGS__)
#else
#define dbg_assert(...)
#define dbg_printf(...)
#endif

/*
 * Max cache and object sizes
 * You might want to move these to the file containing your cache implementation
 */
#define MAX_CACHE_SIZE (1024 * 1024)
#define MAX_OBJECT_SIZE (100 * 1024)

typedef struct sockaddr SA;
typedef struct {
    struct sockaddr_in addr; // Socket address
    socklen_t addrlen;       // Socket address length
    int connfd;              // Client connection file descriptor
    char host[MAXLINE];      // Client host
    char serv[MAXLINE];      // Client service (port)
} client_info;

/*
 * String to use for the User-Agent header.
 * Don't forget to terminate with \r\n
 */
static const char *header_user_agent =
    "User Agent: Mozilla/5.0 (X11; Linux x86_64; rv:3.10.0) Gecko/20230411 "
    "Firefox/63.0.1\r\n";
//"User Agent:Mozilla/5.0 (X11; Linux x86_64; rv:3.10.0) Gecko/20230411
// Firefox/63.0.1\r\n";

// I looked at tiny.c and used some of it
void proxy_clienterror(int fd, const char *errnum, const char *shortmsg,
                       const char *longmsg) {
    char buf[MAXLINE];
    char body[MAXBUF];
    size_t buflen;
    size_t bodylen;

    /* Build the HTTP response body */
    bodylen = snprintf(body, MAXBUF,
                       "<!DOCTYPE html>\r\n"
                       "<html>\r\n"
                       "<head><title>Proxy Error</title></head>\r\n"
                       "<body bgcolor=\"ffffff\">\r\n"
                       "<h1>%s: %s</h1>\r\n"
                       "<p>%s</p>\r\n"
                       "<hr /><em>The Proxy Web server</em>\r\n"
                       "</body></html>\r\n",
                       errnum, shortmsg, longmsg);
    if (bodylen >= MAXBUF) {
        return; // Overflow!
    }

    /* Build the HTTP response headers */
    buflen = snprintf(buf, MAXLINE,
                      "HTTP/1.0 %s %s\r\n"
                      "Content-Type: text/html\r\n"
                      "Content-Length: %zu\r\n%s\r\n",
                      errnum, shortmsg, bodylen, header_user_agent);
    if (buflen >= MAXLINE) {
        return; // Overflow!
    }

    /* Write the headers */
    if (rio_writen(fd, buf, buflen) < 0) {
        fprintf(stderr, "Error writing error response headers to client\n");
        return;
    }

    /* Write the body */
    if (rio_writen(fd, body, bodylen) < 0) {
        fprintf(stderr, "Error writing error response body to client\n");
        return;
    }
}

// handle sigpipe due to pdf
void sigpipe_handler(int sig) {
    return;
}

int parse_hostpath(char *buf, char *state, char *host, char *path) {
    int i = 0;
    for (; i < strlen(buf); i++) {
        if (buf[i] == ' ')
            break;
    }
    memcpy(state, buf, i);
    // sio_printf("%s\n",state);
    if (!strcmp(state, "GET") && !strcmp(state, "POST")) {
        sio_printf("Wrong state in parseline.\n");
        return 0;
    }
    char rub[MAXLINE];
    i++;
    buf = buf + i;
    memcpy(rub, buf, 7);
    // sio_printf("%s\n",rub);
    if (strcmp(rub, "http://") != 0) {
        sio_printf("Wrong rub in parseline.\n");
        return 0;
    }
    buf = buf + 7;
    i = 0;
    for (; i < strlen(buf); i++) {
        if (buf[i] == '/')
            break;
    }
    memcpy(host, buf, i);
    // sio_printf("%s\n",host);
    buf = buf + i;
    // sio_printf("%s\n",buf);
    i = 0;
    for (; i < strlen(buf); i++) {
        if (buf[i] == ' ')
            break;
    }
    memcpy(path, buf, i);
    // sio_printf("%s\n",path);
    buf = buf + i + 1;
    return 1;
}

void parse_hostport(char *host, char *hostname, char *hostport) {
    int i = 0;
    for (; i < strlen(host); i++) {
        if (host[i] == ':')
            break;
    }
    memcpy(hostname, host, i);
    if (i == strlen(host) - 1) {
        strcpy(hostport, "80");
        return;
    }
    host = host + i + 1;
    memcpy(hostport, host, strlen(host));
    return;
}

// looked at tiny.c and used some of it
void serve_proxy(int connfd) {
    /*int res = getnameinfo(
            (SA *) &client->addr, client->addrlen,
            client->host, sizeof(client->host),
            client->serv, sizeof(client->serv),
            0);
    if (res == 0) {
        sio_printf("Accepted connection from %s:%s\n", client->host,
    client->serv);
    }
    else {
        fprintf(stderr, "getnameinfo failed: %s\n", gai_strerror(res));
    }*/
    rio_t rio;
    rio_readinitb(&rio, connfd);
    char buf[MAXLINE];
    memset(buf, '\0', MAXLINE);
    if (rio_readlineb(&rio, buf, MAXLINE) <= 0) {
        return;
    }

    // sio_printf("%s", buf);
    char state[MAXLINE];
    char host[MAXLINE];
    char path[MAXLINE];
    if (!parse_hostpath(buf, state, host, path)) {
        // sio_printf("Wrong parseline!\n");
        proxy_clienterror(connfd, "400", "Bad Request",
                          "Proxy could not parse request headers");
        // sio_printf("Wrong parseline!\n");
        return;
    }
    if (strcmp(state, "POST") == 0) {
        proxy_clienterror(connfd, "501", "Not Implemented",
                          "Proxy does not implement this POST method");
        return;
    }
    char hostname[MAXLINE];
    char hostport[MAXLINE];
    parse_hostport(host, hostname, hostport);
    // sio_printf("%s\n",hostname);
    // sio_printf("%s\n",hostport);
    int proxy_clientfd;
    proxy_clientfd = open_clientfd(hostname, hostport);
    if (proxy_clientfd < 0) {
        // sio_printf("connection failed\n");
        // rio_writen(connfd,header_user_agent,strlen(header_user_agent));
        proxy_clienterror(connfd, "503", "Service Unavailable",
                          "Proxy does not implement this POST method");
        close(proxy_clientfd);
        return;
    }
    rio_readinitb(&rio, proxy_clientfd);
    char rbuf[MAXLINE];
    memset(rbuf, '\0', MAXLINE);
    size_t buflen;
    buflen = snprintf(rbuf, MAXLINE,
                      "GET %s HTTP/1.0\r\nHost:%s:%s\r\nConnection: "
                      "close\r\nProxy-Connection: close\r\n%s\r\n",
                      path, hostname, hostport, header_user_agent);
    if (buflen >= MAXLINE)
        return; // Overflow
    // sio_printf("%s\n",rbuf);
    rio_writen(proxy_clientfd, rbuf, strlen(rbuf));
    ssize_t n;
    // ssize_t count=0;
    memset(buf, '\0', MAXLINE);
    // rio_writen(connfd,header_user_agent,strlen(header_user_agent));
    /*while((n=rio_readlineb(&rio, buf, 1)) >= 0){
            if(n>0){
    rio_writen(connfd,buf,n);
    rio_writen(connfd,header_user_agent,strlen(header_user_agent));
            break;}
    }*/

    /*while(1){
    n=rio_readnb(&rio, buf, MAXLINE);
    if(strcmp(buf,"\r\n")!=0){
             rio_writen(connfd,buf,n);
             break;
    }
    rio_writen(connfd,header_user_agent,strlen(header_user_agent));
    rio_writen(connfd,buf,n);
    }*/
    // int flag=0;

    while ((n = rio_readnb(&rio, buf, MAXLINE)) >= 0) {
        n = rio_writen(connfd, buf, n);
        // if(strcmp(buf,"HTTP/1.0 400 Bad request\r\n")==0)
        // rio_writen(connfd,header_user_agent,strlen(header_user_agent));
	if(n<0)
	{
		return;
		proxy_clienterror(connfd, "400", "Bad Request",
                          "Proxy could not parse request headers");
	}
        memset(buf, '\0', MAXLINE);
    }

    // rio_writen(connfd,buf,count);
    // if(count==0){
    //	    sio_printf("count=0\n");
    // rio_writen(connfd,header_user_agent,strlen(header_user_agent));
    // proxy_clienterror(connfd,"503",  "Service Unavailable",
    //		  "The server is not ready to handle the request");
    //  }
    // shutdown(proxy_clientfd,0);
    close(proxy_clientfd);
    /*char state[MAXLINE];
    char host[MAXLINE];
    char path[MAXLINE];
    if(!parse_hostpath(buf,state,host,path)){
        sio_printf("Wrong parseline!\n");
        return;
    }
    rio_writen(client->connfd, header_user_agent, strlen(header_user_agent));*/

    // sio_printf("%s\n",state);
    // sio_printf("%d\n",client);
    return;
}

void *proxy_thread(void *connfd) {
    int cfd = *((int *)connfd);
    // client_info *ccfd=&cfd;
    // Free(connfd);
    pthread_detach(pthread_self());
    Free(connfd);
    Signal(SIGPIPE, sigpipe_handler);
    serve_proxy(cfd);
    close(cfd);
    return NULL;
}

int main(int argc, char **argv) {
    // printf("%s", header_user_agent);
    Signal(SIGPIPE, sigpipe_handler);
    int listenfd; //,connfd;
    int *connfd;
    // socklen_t clientlen;
    // struct sockaddr_storage clientaddr;
    // char client_hostname[MAXLINE], client_port[MAXLINE];
    // if (argc != 2) {
    //  sio_printf("argc wrong!\n");
    // exit(1);
    //}
    // client_info client_data;
    // client_info *client = &client_data;
    listenfd = open_listenfd(argv[1]);
    pthread_t tid;
    socklen_t clientlen;
    struct sockaddr_storage clientaddr;
    // sigset_t sig;
    // sigemptyset(&sig);
    // sigfillset(&sig);
    while (1) {
        clientlen = sizeof(clientaddr);
        connfd = Malloc(sizeof(int));
        *connfd = accept(listenfd, (SA *)&clientaddr, &clientlen);
        /*if (client->connfd < 0) {
            perror("accept");
            continue;
        }*/
        // serve_proxy(client);
        // close(client->connfd);

        // pthread_sigmask(SIG_BLOCK,&sig,NULL);

        // int res =
        // getnameinfo(
        //  (SA *) &client->addr, client->addrlen,
        // client->host, sizeof(client->host),
        // client->serv, sizeof(client->serv),
        // 0);
        /*if (res == 0) {
            sio_printf("Accepted connection from %s:%s\n", client->host,
        client->serv);
        }
        else {
            fprintf(stderr, "getnameinfo failed: %s\n", gai_strerror(res));
        }*/

        pthread_create(&tid, NULL, proxy_thread, connfd);
        // pthread_sigmask(SIG_UNBLOCK,&sig,NULL);
        /*clientlen=sizeof(struct sockaddr_storage);
        connfd=accept(listenfd, (SA*)&clientaddr, &clientlen);
        getnameinfo((SA*)&clientaddr,clientlen,client_hostname,MAXLINE,client_port,MAXLINE,0);
        sio_printf("Connected to (%s,%s)\n",client_hostname,client_port);
        serve_proxy(connfd);
        close(connfd);*/
    }
    return 0;
}
