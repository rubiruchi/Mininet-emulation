#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/if_tun.h>
#include <net/if.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/select.h>
#include <sys/time.h>
#include <termios.h>
#include <unistd.h>


/* buffer for reading from tun/tap interface, must be >= 1500 */
#define BUFSIZE 2000
#define CLIENT 0
#define SERVER 1
#define PORT 55555
#define DUMMY_FLAG 65500

int debug;
char *progname;
pthread_t tapTonet_id, netTotap_id;

struct fds {
  int* tap;
  int* net;
};

/**************************************************************************
 * do_debug: prints debugging stuff (doh!)                                *
 **************************************************************************/
void do_debug(char *msg, ...){
  
  va_list argp;
  
  if(debug) {
    va_start(argp, msg);
    vfprintf(stderr, msg, argp);
    va_end(argp);
  }
}

/**************************************************************************
 * tun_alloc: allocates or reconnects to a tun/tap device. The caller     *
 *            must reserve enough space in *dev.                          *
 **************************************************************************/
int tun_alloc(char *dev, int flags) {

  struct ifreq ifr;
  int fd, err;
  char *clonedev = "/dev/net/tun";

  if( (fd = open(clonedev , O_RDWR)) < 0 ) {
    perror("Opening /dev/net/tun");
    return fd;
  }

  memset(&ifr, 0, sizeof(ifr));

  ifr.ifr_flags = flags;

  if (*dev) {
    strncpy(ifr.ifr_name, dev, IFNAMSIZ);
  }

  if( (err = ioctl(fd, TUNSETIFF, (void *)&ifr)) < 0 ) {
    perror("ioctl(TUNSETIFF)");
    close(fd);
    return err;
  }

  strcpy(dev, ifr.ifr_name);

  return fd;
}

/**************************************************************************
 * cread: read routine that checks for errors and exits if an error is    *
 *        returned.                                                       *
 **************************************************************************/
int cread(int fd, char *buf, int n){
  
  int nread;

  if((nread=read(fd, buf, n)) < 0){
    perror("Reading data");
    exit(1);
  }
  return nread;
}

/**************************************************************************
 * crecv: "non-blocking" recv routine for continuously receiving data     *
 *        from interfaces.                                                *
 **************************************************************************/
int crecv(int fd, char *buf, int n){
  
  int nrecv;

  /* keep reading the data from the given interface */
  /* enforce returning 0 if no incoming data */
  /* otherwise, return the size of the incoming data */
  nrecv=read(fd, buf, n);
  if (nrecv == -1) return 0;
  else return nrecv;
}

/**************************************************************************
 * cwrite: write routine that checks for errors and exits if an error is  *
 *         returned.                                                      *
 **************************************************************************/
int cwrite(int fd, char *buf, int n){
  
  int nwrite;

  if((nwrite=write(fd, buf, n)) < 0){
    perror("Writing data");
    exit(1);
  }
  return nwrite;
}

/**************************************************************************
 * read_n: ensures we read exactly n bytes, and puts them into "buf".     *
 *         (unless EOF, of course)                                        *
 **************************************************************************/
int read_n(int fd, char *buf, int n) {

  int nread, left = n;

  while(left > 0) {
    if ((nread = cread(fd, buf, left)) == 0){
      return 0 ;      
    }else {
      left -= nread;
      buf += nread;
    }
  }
  return n;  
}

/**************************************************************************
 * signal_pipe: netTotap receives signal SIGPIPE when tapTonet breaks     *
 *              and netTotap sends SIGQUIT signal to tapTonet.            *
 **************************************************************************/
void signal_pipe(int signo)
{
  pthread_kill(tapTonet_id,SIGQUIT);
  pthread_join(tapTonet_id,NULL);
  
  perror("tapTonet pthread out \n");
  
  pthread_exit(0);
}

/**************************************************************************
 * pthread_out: tapTonet exits when receives signal.                      *
 **************************************************************************/
void pthread_out(int signo)
{
  pthread_exit(0);
}

/**************************************************************************
 * tapTonet_c: Tap interface to Net interface pthread for client.         *
 **************************************************************************/
void* tapTonet_c(void* input)
{
  do_debug("tap to net thread is up!\n");
  int tap_fd = ((struct fds*)input)->tap;
  int net_fd = ((struct fds*)input)->net;
  uint16_t nread, nwrite, plength;
  char buffer[BUFSIZE];
  char dummy_packet[BUFSIZE];

  struct timespec ts;
  ts.tv_nsec = 500000;
  ts.tv_sec = 0;

  /* construct the dummy packet */
  for (int i=0;i<1000;++i) dummy_packet[i] = 'F';

  /* register SIGQUIT signal */
  signal(SIGQUIT,pthread_out);

  while(1)
  {
    /* "non-blocking" read the data from tap interface */
    nread = crecv(tap_fd, buffer, BUFSIZE);
    do_debug("TAP2NET: Read %d bytes from the tap interface\n", nread);

    if (nread == 0) {
      /* no incoming data, send dummy packet instead */
      plength = htons(DUMMY_FLAG);
      nwrite = cwrite(net_fd, (char *)&plength, sizeof(plength));
      nwrite = cwrite(net_fd, dummy_packet, 1000);
      do_debug("TAP2NET dummy: Written %d bytes to the network\n", 1000);
      /* send 1000 bytes dummy packet for every 1ms => 10Mbps sending rate */
      nanosleep(&ts, NULL);
    } else if (nread > 0) {
      /* send real data to network */
      plength = htons(nread);
      nwrite = cwrite(net_fd, (char *)&plength, sizeof(plength));
      nwrite = cwrite(net_fd, buffer, nread);
      do_debug("TAP2NET: Written %d bytes to the network\n", nwrite);
    }
  }
}

/**************************************************************************
 * tapTonet_s: Tap interface to Net interface pthread for server.         *
 **************************************************************************/
void* tapTonet_s(void* input)
{
  do_debug("tap to net thread is up!\n");
  int tap_fd = ((struct fds*)input)->tap;
  int net_fd = ((struct fds*)input)->net;
  uint16_t nread, nwrite, plength;
  char buffer[BUFSIZE];

  /* construct the dummy packet */
  signal(SIGQUIT,pthread_out);

  while(1)
  {
    nread = cread(tap_fd, buffer, BUFSIZE);
    
    do_debug("TAP2NET: Read %d bytes from the tap interface\n", nread);

    /* write length + packet */
    plength = htons(nread);
    nwrite = cwrite(net_fd, (char *)&plength, sizeof(plength));
    nwrite = cwrite(net_fd, buffer, nread);
    
    do_debug("TAP2NET: Written %d bytes to the network\n", nwrite);
  }
}

/**************************************************************************
 * netTotap: Net interface to Tap interface pthread.                      *
 **************************************************************************/
void* netTotap(void* input)
{
  do_debug("net to tap thread is up!\n");
  int tap_fd = ((struct fds*)input)->tap;
  int net_fd = ((struct fds*)input)->net;
  uint16_t nread, nwrite, plength;
  char buffer[BUFSIZE];
  
  /* register SIGPIPE signal */
  signal(SIGPIPE,signal_pipe);

  while(1)
  {
    /* keep reading the data from net interface */
    nread = read_n(net_fd, (char *)&plength, sizeof(plength));
    do_debug("Data length: %d\n", ntohs(plength));

    if (nread == 0) {
       /* ctrl-c at the other end */
      break;
    }

    if (ntohs(plength) == DUMMY_FLAG) {
      /* read dummy packet and dump it immidiately*/
      nread = read_n(net_fd, buffer, 1000);
      do_debug("NET2TAP dummy: Read %d bytes from the network and dump it!\n", nread);
    } else {
      /* read packet */
      nread = read_n(net_fd, buffer, ntohs(plength));
      do_debug("NET2TAP: Read %d bytes from the network\n", nread);
      /* now buffer[] contains a full packet or frame, write it into the tun/tap interface */
      nwrite = cwrite(tap_fd, buffer, nread);
      do_debug("NET2TAP: Written %d bytes to the tap interface\n", nwrite);
    }
  }
}

/**************************************************************************
 * my_err: prints custom error messages on stderr.                        *
 **************************************************************************/
void my_err(char *msg, ...) {

  va_list argp;
  
  va_start(argp, msg);
  vfprintf(stderr, msg, argp);
  va_end(argp);
}

/**************************************************************************
 * usage: prints usage and exits.                                         *
 **************************************************************************/
void usage(void) {
  fprintf(stderr, "Usage:\n");
  fprintf(stderr, "%s -i <ifacename> [-s|-c <serverIP>] [-p <port>] [-u|-a] [-d]\n", progname);
  fprintf(stderr, "%s -h\n", progname);
  fprintf(stderr, "\n");
  fprintf(stderr, "-i <ifacename>: Name of interface to use (mandatory)\n");
  fprintf(stderr, "-s|-c <serverIP>: run in server mode (-s), or specify server address (-c <serverIP>) (mandatory)\n");
  fprintf(stderr, "-p <port>: port to listen on (if run in server mode) or to connect to (in client mode), default 55555\n");
  fprintf(stderr, "-u|-a: use TUN (-u, default) or TAP (-a)\n");
  fprintf(stderr, "-d: outputs debug information while running\n");
  fprintf(stderr, "-h: prints this help text\n");
  exit(1);
}

int main(int argc, char *argv[]) {
  
  int tap_fd, option;
  int flags = IFF_TUN;
  char if_name[IFNAMSIZ] = "";
  struct sockaddr_in local, remote;
  char remote_ip[16] = "";            /* dotted quad IP string */
  unsigned short int port = PORT;
  int sock_fd, net_fd, optval = 1;
  socklen_t remotelen;
  int cliserv = -1;    /* must be specified on cmd line */

  progname = argv[0];
  
  /* Check command line options */
  while ((option = getopt(argc, argv, "i:sc:p:uahd")) > 0) {
    switch(option) {
      case 'd':
        debug = 1;
        break;
      case 'h':
        usage();
        break;
      case 'i':
        strncpy(if_name, optarg, IFNAMSIZ-1);
        break;
      case 's':
        cliserv = SERVER;
        break;
      case 'c':
        cliserv = CLIENT;
        strncpy(remote_ip, optarg, 15);
        break;
      case 'p':
        port = atoi(optarg);
        break;
      case 'u':
        flags = IFF_TUN;
        break;
      case 'a':
        flags = IFF_TAP;
        break;
      default:
        my_err("Unknown option %c\n", option);
        usage();
    }
  }

  argv += optind;
  argc -= optind;

  if (argc > 0) {
    my_err("Too many options!\n");
    usage();
  }

  if (*if_name == '\0') {
    my_err("Must specify interface name!\n");
    usage();
  } else if(cliserv < 0) {
    my_err("Must specify client or server mode!\n");
    usage();
  } else if((cliserv == CLIENT)&&(*remote_ip == '\0')) {
    my_err("Must specify server address!\n");
    usage();
  }

  /* initialize tun/tap interface */
  if ((tap_fd = tun_alloc(if_name, flags | IFF_NO_PI)) < 0 ) {
    my_err("Error connecting to tun/tap interface %s!\n", if_name);
    exit(1);
  }

  if (cliserv == CLIENT) {
    /* make the tap_fd non-blocking */
    flags = fcntl(tap_fd, F_GETFL, 0);
    fcntl(tap_fd, F_SETFL, flags | O_NONBLOCK);
  }


  do_debug("Successfully connected to interface %s\n", if_name);

  if ((sock_fd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
    perror("socket()");
    exit(1);
  }

  if (cliserv == CLIENT) {
    /* Client, try to connect to server */

    /* assign the destination address */
    memset(&remote, 0, sizeof(remote));
    remote.sin_family = AF_INET;
    remote.sin_addr.s_addr = inet_addr(remote_ip);
    remote.sin_port = htons(port);

    /* connection request */
    if (connect(sock_fd, (struct sockaddr*) &remote, sizeof(remote)) < 0) {
      perror("connect()");
      exit(1);
    }
    net_fd = sock_fd;

    do_debug("CLIENT: Connected to server %s\n", inet_ntoa(remote.sin_addr));
  } else {
    /* Server, wait for connections */

    /* avoid EADDRINUSE error on bind() */
    if (setsockopt(sock_fd, SOL_SOCKET, SO_REUSEADDR, (char *)&optval, sizeof(optval)) < 0) {
      perror("setsockopt()");
      exit(1);
    } 
    
    memset(&local, 0, sizeof(local));
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    local.sin_port = htons(port);
    if (bind(sock_fd, (struct sockaddr*) &local, sizeof(local)) < 0) {
      perror("bind()");
      exit(1);
    }
    
    if (listen(sock_fd, 5) < 0) {
      perror("listen()");
      exit(1);
    }
    
    /* wait for connection request */
    remotelen = sizeof(remote);
    memset(&remote, 0, remotelen);
    if ((net_fd = accept(sock_fd, (struct sockaddr*)&remote, &remotelen)) < 0) {
      perror("accept()");
      exit(1);
    }

    do_debug("SERVER: Client connected from %s\n", inet_ntoa(remote.sin_addr));
  }

  struct fds *fd = (struct fds *)malloc(sizeof(struct fds));
  fd->tap = tap_fd;
  fd->net = net_fd;

  /* create the tap to net pthread */
  if (cliserv == CLIENT) {
    if (pthread_create(&tapTonet_id, NULL, tapTonet_c, (void *)fd))
    {
      perror("CLIENT: pthread_create tap to net err\n");
    }
  } else {
    if (pthread_create(&tapTonet_id, NULL, tapTonet_s, (void *)fd))
    {
      perror("SERVER: pthread_create tap to net err\n");
    }
  }

  /* create the net to tap pthread */
  if (pthread_create(&netTotap_id, NULL, netTotap, (void *)fd))
  {
    printf("pthread_create net to tap err\n");
  }
  
  /* block the threads at this point until netTotap exits */
  pthread_join(netTotap_id,NULL);
  printf("write pthread out \n");
  
  return(0);
}