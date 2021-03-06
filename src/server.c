#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/tcp.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "common.h"
#include "server.h"

#define MAX_WRITE 104857600  // 100MB

/* Global variables */
int serverPort;
char flowbuf[MAX_WRITE];
int reverse_dir;
char tcp_congestion_name[80];
char pers_tput_log[80];

int main (int argc, char *argv[]) {
  int listenfd;
  socklen_t len;
  struct sockaddr_in servaddr;
  struct sockaddr_in cliaddr;
  int sock_opt = 1;
  
  /* read command line arguments */
  read_args(argc, argv);
  
  /* initialize socket */
  listenfd = socket(AF_INET, SOCK_STREAM, 0);
  if (listenfd < 0)
    error("ERROR opening socket");

  if (setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &sock_opt, sizeof(sock_opt)) < 0)
    error("ERROR setting SO_REUSERADDR option");
  if (setsockopt(listenfd, IPPROTO_TCP, TCP_NODELAY, &sock_opt, sizeof(sock_opt)) < 0)
    error("ERROR setting TCP_NODELAY option");
  if (setsockopt(listenfd, IPPROTO_TCP, TCP_CONGESTION, tcp_congestion_name, 80) < 0)
    error("ERROR setting TCP_CONGESTION option");

  memset(&servaddr, 0, sizeof(servaddr));
  servaddr.sin_family = AF_INET;
  servaddr.sin_addr.s_addr = htonl(INADDR_ANY);
  servaddr.sin_port = htons(serverPort);
  
  if (bind(listenfd, (struct sockaddr *)&servaddr, sizeof(servaddr)) < 0)
    error("ERROR on bind");
  
  if (listen(listenfd, 20) < 0)
    error("ERROR on listen");
  
  printf("Dynamic traffic generator application server started...\n");
  printf("Listening port: %d\n", serverPort);

  while(1) {
    /* wait for connections */ 
    int sockfd;
    len = sizeof(cliaddr);
    sockfd = accept(listenfd, (struct sockaddr *) &cliaddr, &len);
    if (sockfd < 0)
      error("ERROR on accept");

    pid_t pid = fork();
    if (pid < 0)
      error("ERROR on fork");

    if (pid == 0) {
      /* child process */
      if (close(listenfd) < 0)
	error("child: ERROR on close");
      handle_connection(sockfd, (const struct sockaddr_in *) &cliaddr);
      break;
    }
    else {
      /* parent process */
      if (close(sockfd) < 0)
	error("parent: ERROR on close");
    }
  }

  return 0;
}

/* 
 * Handles requests for an established connection. Each request is initiated
 * by the client with a small message providing meta-data for the request, 
 * specifically, a flow index and size. The server echoes the meta-data, and
 * subsequently sends a flow of the requested size to the client.
 */ 
void handle_connection(int sockfd, const struct sockaddr_in *cliaddr) {
  uint f_index;
  uint f_size;
  int total;
  int n;
  uint meta_data_size = 2 * sizeof(uint);
  char buf[16]; /* buffer to hold meta data */
  char clistr[INET_ADDRSTRLEN];
  char readbuf[READBUF_SIZE];

  if (inet_ntop(AF_INET, &(cliaddr->sin_addr), clistr, INET_ADDRSTRLEN) == NULL)
    error("ERROR on inet_ntop");

  printf("Connection established to %s (sockfd = %d)!\n", clistr, sockfd);

  while(1) {
    /* read meta-data */
    if (read_exact(sockfd, buf, meta_data_size, 16, false) 
	!= meta_data_size)
      break;

    /* extract flow index and size */
    memcpy(&f_index, buf, sizeof(uint));
    memcpy(&f_size, buf + sizeof(uint), sizeof(uint));

#ifdef DEBUG
    printf("Flow request: index: %u size: %d\n", f_index, f_size);
#endif
    
    /* echo meta-data (f_index and f_size) */
    if (write_exact(sockfd, buf, meta_data_size, MAX_WRITE, false)
	!= meta_data_size)
      break;

    if (! reverse_dir) {
      /* send flow of f_size bytes */
      if (f_size == 0) {
        /* This is the client's way of signaling a persistently backlogged
           socket. Write data as fast as possible */
        int n = write_forever(sockfd, flowbuf, MAX_WRITE, pers_tput_log);
        if (n < 0) {
          printf("Server: error in writing to persistently backlogged "
                 "socket\n");
          exit(EXIT_FAILURE);
        }
      } else {
        if (write_exact(sockfd, flowbuf, f_size, MAX_WRITE, true)
            != f_size)
          break;
#ifdef DEBUG
        else
          printf("Sent %d bytes to client\n", f_size);
#endif
      }
    } else {
      /* receive flow of size f_size bytes */
      total = f_size;
      do {
        int readsize = total;
        if (readsize > READBUF_SIZE)
          readsize = READBUF_SIZE;

        n = read(sockfd, readbuf, readsize);
 #ifdef DEBUG
        printf("Partial receive %d bytes from client; total %d\n", n, total);
 #endif

        total -= n;

      } while (total > 0 && n > 0);

      if (total > 0) {
        printf("failed to read: %d\n", total);
        break;
      }
#ifdef DEBUG
      else {
        printf("Received %d bytes from client\n", f_size);
      }
#endif
      /* Read finished. */
    }
  }

  /* close_connection */
  close(sockfd);
  printf("Connection to %s closed (sockfd = %d)!\n", clistr, sockfd);
}

/*
 * Read command line arguments. 
 */
void read_args(int argc, char*argv[]) {
  /* default values */
  serverPort = 5000;
  reverse_dir = 0;
  strcpy(tcp_congestion_name, "reno");
  strcpy(pers_tput_log, "server.log");

  int i = 1;
  while (i < argc) {
    if (strcmp(argv[i], "-p") == 0) {
      serverPort = atoi(argv[i+1]);
      i += 2;
    } else if (strcmp(argv[i], "-h") == 0) {
      print_usage();
      exit(EXIT_FAILURE);
    } else if (strcmp(argv[i], "-r") == 0) {
      reverse_dir = 1;
      i += 1;
    } else if (strcmp(argv[i], "-t") == 0) {
      strcpy(tcp_congestion_name, argv[i+1]);
      i += 2;
    } else if (strcmp(argv[i], "-l") == 0) {
      strcpy(pers_tput_log, argv[i+1]);
      i += 2;
    } else {
      printf("invalid option: %s\n", argv[i]);
      print_usage();
      exit(EXIT_FAILURE);
    }
  }
}

/*
 * Print usage.
 */
void print_usage() {
  printf("usage: server [options]\n");
  printf("options:\n");
  printf("-p <value>                 port number (default 5000)\n");
  printf("-r                         transfer data client->server\n");
  printf("-t <string>                tcp congestion control algorithm (default reno)\n");
  printf("-l                         throughput log file for backlogged transfers\n");
  printf("-h                         display usage information and quit\n");
}
