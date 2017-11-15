#include "util.h" // _GNU_SOURCE

#include <ctype.h>
#include <fcntl.h>
#ifdef USE_PTHREAD
  #include <pthread.h>
#endif
#include <sys/stat.h>
#include <openssl/ssl.h>
#include <openssl/err.h>

#include "socket_handler.h"
#include "certs.h"
#include "logger.h"
 
// private data for socket_handler() use

  // HTTP 204 No Content for Google generate_204 URLs
  static const char http204[] =
  "HTTP/1.1 204 No Content\r\n"
  "Content-Length: 0\r\n"
  "Content-Type: text/html; charset=UTF-8\r\n"
  "\r\n";

  // HTML stats response pieces
  static const char httpstats1[] =
  "HTTP/1.1 200 OK\r\n"
  "Content-type: text/html\r\n"
  "Content-length: ";
  // total content length goes between these two strings
  static const char httpstats2[] =
  "\r\n"
  "Connection: keep-alive\r\n"
  "\r\n";
  // split here because we care about the length of what follows
  static const char httpstats3[] =
  "<!DOCTYPE html><html><head><title>pixelserv statistics</title><style>body {font-family:monospace;} table {min-width: 75%; border-collapse: collapse;} th { height:18px; } td {border: 1px solid #e0e0e0; background-color: #f9f9f9;} td:first-child {width: 7%;} td:nth-child(2) {width: 15%; background-color: #ebebeb; border: 1px solid #f9f9f9;}</style></head><body>";
  // stats text goes between these two strings
  static const char httpstats4[] =
  "</body></html>\r\n";

  // note: the -2 is to avoid counting the last line ending characters
  static const unsigned int statsbaselen = sizeof httpstats3 + sizeof httpstats4 - 2;

  // TXT stats response pieces
  static const char txtstats1[] =
  "HTTP/1.1 200 OK\r\n"
  "Content-type: text/plain\r\n"
  "Content-length: ";
  // total content length goes between these two strings
  static const char txtstats2[] =
  "\r\n"
  "Connection: keep-alive\r\n"
  "\r\n";
  // split here because we care about the length of what follows
  static const char txtstats3[] =
  "\r\n";

  static const char httpredirect[] =
  "HTTP/1.1 307 Temporary Redirect\r\n"
  "Location: %s\r\n"
  "Content-type: text/plain\r\n"
  "Content-length: 0\r\n"
  "Connection: keep-alive\r\n\r\n";

  static const char httpnullpixel[] =
  "HTTP/1.1 200 OK\r\n"
  "Content-type: image/gif\r\n"
  "Content-length: 42\r\n"
  "Connection: keep-alive\r\n"
  "\r\n"
  "GIF89a" // header
  "\1\0\1\0"  // little endian width, height
  "\x80"    // Global Colour Table flag
  "\0"    // background colour
  "\0"    // default pixel aspect ratio
  "\1\1\1"  // RGB
  "\0\0\0"  // RBG black
  "!\xf9"  // Graphical Control Extension
  "\4"  // 4 byte GCD data follow
  "\1"  // there is transparent background color
  "\0\0"  // delay for animation
  "\0"  // transparent colour
  "\0"  // end of GCE block
  ","  // image descriptor
  "\0\0\0\0"  // NW corner
  "\1\0\1\0"  // height * width
  "\0"  // no local color table
  "\2"  // start of image LZW size
  "\1"  // 1 byte of LZW encoded image data
  "D"    // image data
  "\0"  // end of image data
  ";";  // GIF file terminator

  static const char httpnulltext[] =
  "HTTP/1.1 200 OK\r\n"
  "Strict-Transport-Security: max-age=31536000; includeSubDomains\r\n" //experimental support for HSTS
  "Content-type: text/html\r\n"
  "Content-length: 0\r\n"
  "Connection: keep-alive\r\n"
  "\r\n";

  static const char http501[] =
  "HTTP/1.1 501 Method Not Implemented\r\n"
  "Connection: keep-alive\r\n"
  "\r\n";

  static const char httpnull_png[] =
  "HTTP/1.1 200 OK\r\n"
  "Content-type: image/png\r\n"
  "Content-length: 67\r\n"
  "Connection: keep-alive\r\n"
  "\r\n"
  "\x89"
  "PNG"
  "\r\n"
  "\x1a\n"  // EOF
  "\0\0\0\x0d" // 13 bytes length
  "IHDR"
  "\0\0\0\1\0\0\0\1"  // width x height
  "\x08"  // bit depth
  "\x06"  // Truecolour with alpha
  "\0\0\0"  // compression, filter, interlace
  "\x1f\x15\xc4\x89"  // CRC
  "\0\0\0\x0a"  // 10 bytes length
  "IDAT"
  "\x78\x9c\x63\0\1\0\0\5\0\1"
  "\x0d\x0a\x2d\xb4"  // CRC
  "\0\0\0\0"  // 0 length
  "IEND"
  "\xae\x42\x60\x82";  // CRC

  static const char httpnull_jpg[] =
  "HTTP/1.1 200 OK\r\n"
  "Content-type: image/jpeg\r\n"
  "Content-length: 159\r\n"
  "Connection: close\r\n"
  "\r\n"
  "\xff\xd8"  // SOI, Start Of Image
  "\xff\xe0"  // APP0
  "\x00\x10"  // length of section 16
  "JFIF\0"
  "\x01\x01"  // version 1.1
  "\x01"      // pixel per inch
  "\x00\x48"  // horizontal density 72
  "\x00\x48"  // vertical density 72
  "\x00\x00"  // size of thumbnail 0 x 0
  "\xff\xdb"  // DQT
  "\x00\x43"  // length of section 3+64
  "\x00"      // 0 QT 8 bit
  "\xff\xff\xff\xff\xff\xff\xff\xff"
  "\xff\xff\xff\xff\xff\xff\xff\xff"
  "\xff\xff\xff\xff\xff\xff\xff\xff"
  "\xff\xff\xff\xff\xff\xff\xff\xff"
  "\xff\xff\xff\xff\xff\xff\xff\xff"
  "\xff\xff\xff\xff\xff\xff\xff\xff"
  "\xff\xff\xff\xff\xff\xff\xff\xff"
  "\xff\xff\xff\xff\xff\xff\xff\xff"
  "\xff\xc0"  // SOF
  "\x00\x0b"  // length 11
  "\x08\x00\x01\x00\x01\x01\x01\x11\x00"
  "\xff\xc4"  // DHT Define Huffman Table
  "\x00\x14"  // length 20
  "\x00\x01"  // DC table 1
  "\x00\x00\x00\x00\x00\x00\x00\x00"
  "\x00\x00\x00\x00\x00\x00\x00\x03"
  "\xff\xc4"  // DHT
  "\x00\x14"  // length 20
  "\x10\x01"  // AC table 1
  "\x00\x00\x00\x00\x00\x00\x00\x00"
  "\x00\x00\x00\x00\x00\x00\x00\x00"
  "\xff\xda"  // SOS, Start of Scan
  "\x00\x08"  // length 8
  "\x01"    // 1 component
  "\x01\x00"
  "\x00\x3f\x00"  // Ss 0, Se 63, AhAl 0
  "\x37" // image
  "\xff\xd9";  // EOI, End Of image

  static const char httpnull_swf[] =
  "HTTP/1.1 200 OK\r\n"
  "Content-type: application/x-shockwave-flash\r\n"
  "Content-length: 25\r\n"
  "Connection: keep-alive\r\n"
  "\r\n"
  "FWS"
  "\x05"  // File version
  "\x19\x00\x00\x00"  // litle endian size 16+9=25
  "\x30\x0A\x00\xA0"  // Frame size 1 x 1
  "\x00\x01"  // frame rate 1 fps
  "\x01\x00"  // 1 frame
  "\x43\x02"  // tag type is 9 = SetBackgroundColor block 3 bytes long
  "\x00\x00\x00"  // black
  "\x40\x00"  // tag type 1 = show frame
  "\x00\x00";  // tag type 0 - end file

  static const char httpnull_ico[] =
  "HTTP/1.1 200 OK\r\n"
  "Content-type: image/x-icon\r\n"
  "Cache-Control: max-age=2592000\r\n"
  "Content-length: 70\r\n"
  "Connection: keep-alive\r\n"
  "\r\n"
  "\x00\x00" // reserved 0
  "\x01\x00" // ico
  "\x01\x00" // 1 image
  "\x01\x01\x00" // 1 x 1 x >8bpp colour
  "\x00" // reserved 0
  "\x01\x00" // 1 colour plane
  "\x20\x00" // 32 bits per pixel
  "\x30\x00\x00\x00" // size 48 bytes
  "\x16\x00\x00\x00" // start of image 22 bytes in
  "\x28\x00\x00\x00" // size of DIB header 40 bytes
  "\x01\x00\x00\x00" // width
  "\x02\x00\x00\x00" // height
  "\x01\x00" // colour planes
  "\x20\x00" // bits per pixel
  "\x00\x00\x00\x00" // no compression
  "\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00"
  "\x00\x00\x00\x00" // end of header
  "\x00\x00\x00\x00" // Colour table
  "\x00\x00\x00\x00" // XOR B G R
  "\x80\xF8\x9C\x41"; // AND ?

  static const char httpoptions[] =
  "HTTP/1.1 200 OK\r\n"
  "Content-type: text/html\r\n"
  "Content-length: 11\r\n"
  "Allow: GET,OPTIONS\r\n"
  "Connection: keep-alive\r\n"
  "\r\n"
  "GET,OPTIONS";

/* ECDHE-RSA-AES128-GCM-SHA256 :
   Android >= 4.4.2; Chrome >= 51; Firefox >= 49;
   IE 11 Win 10; Edge >= 13; Safari >= 9; Apple ATS 9 iOS 9
   ECDHE-RSA-AES128-SHA :
   IE 11 Win 7,8.1; IE 11 Winphone 8.1; Opera >= 17; Safar 7 iOS 7.1 */
#define PIXELSERV_CIPHER_LIST \
  "ECDHE-ECDSA-AES128-GCM-SHA256:" \
  "ECDHE-RSA-AES128-GCM-SHA256:" \
  "ECDHE-RSA-AES128-SHA:"

// private functions for socket_handler() use
#ifdef HEX_DUMP
// from http://sws.dett.de/mini/hexdump-c/
static void hex_dump(void *data, int size)
{
  /* dumps size bytes of *data to stdout. Looks like:
   * [0000] 75 6E 6B 6E 6F 77 6E 20   30 FF 00 00 00 00 39 00 unknown 0.....9.
   * (in a single line of course)
   */

  char *p = data;
  char c;
  int n;
  char bytestr[4] = {0};
  char addrstr[10] = {0};
  char hexstr[16*3 + 5] = {0};
  char charstr[16*1 + 5] = {0};
  for (n = 1; n <= size; n++) {
    if (n%16 == 1) {
      // store address for this line
      snprintf(addrstr, sizeof addrstr, "%.4x",
         ((unsigned int)p-(unsigned int)data) );
    }

    c = *p;
    if (isprint(c) == 0) {
      c = '.';
    }

    // store hex str (for left side)
    snprintf(bytestr, sizeof bytestr, "%02X ", *p);
    strncat(hexstr, bytestr, sizeof hexstr - strlen(hexstr) - 1);

    // store char str (for right side)
    snprintf(bytestr, sizeof bytestr, "%c", c);
    strncat(charstr, bytestr, sizeof charstr - strlen(charstr) - 1);

    if (n%16 == 0) {
      // line completed
      printf("[%4.4s]   %-50.50s  %s\n", addrstr, hexstr, charstr);
      hexstr[0] = 0;
      charstr[0] = 0;
    } else if (n%8 == 0) {
      // half line: add whitespaces
      strncat(hexstr, "  ", sizeof hexstr - strlen(hexstr) - 1);
      strncat(charstr, " ", sizeof charstr - strlen(charstr) - 1);
    }

    p++; // next byte
  }

  if (strlen(hexstr) > 0) {
    // print rest of buffer if not empty
    printf("[%4.4s]   %-50.50s  %s\n", addrstr, hexstr, charstr);
  }
}
#endif // HEX_DUMP

// redirect utility functions
char* strstr_last(const char* const str1, const char* const str2) {
  char *strp;
  int len1, len2;
  len2 = strlen(str2);
  if (len2==0) {
    return (char *) str1;
  }
  len1 = strlen(str1);
  if (len1 - len2 <= 0) {
    return 0;
  }
  strp = (char *)(str1 + len1 - len2);
  while (strp != str1) {
    if (*strp == *str2 && strncmp(strp, str2, len2) == 0) {
      return strp;
    }
    strp--;
  }
  return 0;
}

char from_hex(const char ch) {
  return isdigit(ch) ? ch - '0' : tolower(ch) - 'a' + 10;
}

void urldecode(char* const decoded, char* const encoded) {
  char* pstr = encoded;
  char* pbuf = decoded;

  while (*pstr) {
    if (*pstr == '%') {
      if (pstr[1] && pstr[2]) {
        *pbuf++ = from_hex(pstr[1]) << 4 | from_hex(pstr[2]);
        pstr += 2;
      }
    } else {
      *pbuf++ = *pstr;
    }
    pstr++;
  }
  *pbuf = '\0';
}

double elapsed_time_msec(const struct timespec start_time) {
  struct timespec current_time = {0, 0};
  struct timespec diff_time = {0, 0};

  if (!start_time.tv_sec &&
      !start_time.tv_nsec) {
    log_msg(LGG_DEBUG, "check_time(): returning because start_time not set");
    return -1.0;
  }

  get_time(&current_time);

  diff_time.tv_sec = difftime(current_time.tv_sec, start_time.tv_sec) + 0.5;
  diff_time.tv_nsec = current_time.tv_nsec - start_time.tv_nsec;
  if (diff_time.tv_nsec < 0) {
    // normalize nanoseconds
    diff_time.tv_sec  -= 1;
    diff_time.tv_nsec += 1000000000;
  }

  return diff_time.tv_sec * 1000 + ((double)diff_time.tv_nsec / 1000000);
}

#ifdef DEBUG
void child_signal_handler(int sig)
{
  if (sig != SIGTERM
   && sig != SIGUSR2) {
    log_msg(LGG_DEBUG, "Thread or child process ignoring unsupported signal number: %d", sig);
    return;
  }

  if (sig == SIGTERM) {
    // Ignore this signal while we are quitting
    signal(SIGTERM, SIG_IGN);
  }

  log_msg(LGG_DEBUG, "Thread or child process caught signal %d near line number %lu of file %s", sig, LINE_NUMBER, __FILE__);

  if (sig == SIGTERM) {
    // exit program on SIGTERM
    log_msg(LGG_DEBUG, "Thread or child process exit on SIGTERM");
    exit(EXIT_SUCCESS);
  }

  return;
}

#define TIME_CHECK(x) {\
  if (do_warning) {\
    do_warning = 0;\
    time_msec = elapsed_time_msec(start_time);\
    if (time_msec > warning_time) {\
      log_msg(LGG_DEBUG, "Elapsed time %f msec exceeded warning_time=%d msec following operation: %s", time_msec, warning_time, x);\
    }\
  }\
}

#else
# define TIME_CHECK(x,y...)
#endif //DEBUG

extern const char *tls_pem;
extern int tls_ports[];
extern int num_tls_ports;
extern STACK_OF(X509_INFO) *cachain;
extern struct Global *g;

static int tls_servername_cb(SSL *cSSL, int *ad, void *arg)
{
  SSL_CTX *sslctx = NULL;
  int rv = SSL_TLSEXT_ERR_OK;
  tlsext_cb_arg_struct *tlsext_cb_arg = (tlsext_cb_arg_struct *)arg;
  const char* pem_dir = tlsext_cb_arg->tls_pem;
  char full_pem_path[PIXELSERV_MAX_PATH];
  char servername[PIXELSERV_MAX_SERVER_NAME+1];
  servername[PIXELSERV_MAX_SERVER_NAME] = '\0';
  if ((tlsext_cb_arg->servername = (char*)SSL_get_servername(cSSL, TLSEXT_NAMETYPE_host_name)) == NULL)
    //goto free_all;
    strncpy(servername, tlsext_cb_arg->server_ip, PIXELSERV_MAX_SERVER_NAME);
  else
    strncpy(servername, tlsext_cb_arg->servername, PIXELSERV_MAX_SERVER_NAME);
#ifdef DEBUG
  printf("https request for hostname: %s\n", servername);
#endif
  int dot_count=0;
  char *pem_file = strchr(servername, '.');
  char *tld = NULL;

  while(pem_file != NULL){
    dot_count++;
    tld = pem_file + 1;
    pem_file = strchr(tld, '.');
  }
  if (dot_count > 1 && !(dot_count == 3 && atoi(tld) >= 0)){
    pem_file = strchr(servername, '.');
    *(--pem_file) = '_';
  } else
    pem_file = servername;
#ifdef DEBUG
  printf("pem file name: %s\n", pem_file);
#endif
  strcpy(full_pem_path, pem_dir);
  strcat(full_pem_path, "/");
  strcat(full_pem_path, pem_file);
#ifdef DEBUG
  printf("full_pem_path: %s\n",full_pem_path);
#endif
  struct stat st;
  if(stat(full_pem_path, &st) != 0){
    log_msg(LGG_WARNING, "%s %s missing", tlsext_cb_arg->servername, pem_file);
    tlsext_cb_arg->status = SSL_MISS;
    int fd = open(PIXEL_CERT_PIPE, O_WRONLY);
    if(fd == -1)
      log_msg(LGG_ERR, "Failed to open %s: %s", PIXEL_CERT_PIPE, strerror(errno));
    else {
      strcat(pem_file,":");
      write(fd, pem_file, strlen(pem_file));
      close(fd);
    }
    rv = SSL_TLSEXT_ERR_ALERT_FATAL;
    goto free_all;
  }

  sslctx = SSL_CTX_new(TLSv1_2_server_method());
  SSL_CTX_set_options(sslctx,
      SSL_OP_SINGLE_DH_USE |
      SSL_MODE_RELEASE_BUFFERS |
      SSL_OP_NO_COMPRESSION |
      SSL_OP_CIPHER_SERVER_PREFERENCE);
  SSL_CTX_set_ecdh_auto(sslctx, 1);
  if (SSL_CTX_set_cipher_list(sslctx, PIXELSERV_CIPHER_LIST) <= 0)
    log_msg(LGG_DEBUG, "cipher_list cannot be set");

  if(SSL_CTX_use_certificate_file(sslctx, full_pem_path, SSL_FILETYPE_PEM) <= 0 ||
    SSL_CTX_use_PrivateKey_file(sslctx, full_pem_path, SSL_FILETYPE_PEM) <= 0) {
    log_msg(LGG_ERR, "Cannot use %s\n",full_pem_path);
    tlsext_cb_arg->status = SSL_ERR;
    rv = SSL_TLSEXT_ERR_ALERT_FATAL;
    goto free_all;
  }

  if (cachain) {
    X509_INFO *inf; int i;
    for (i=sk_X509_INFO_num(cachain)-1; i >= 0; i--)
    {
      if ((inf = sk_X509_INFO_value(cachain, i)) && inf->x509 &&
             !SSL_CTX_add_extra_chain_cert(sslctx, X509_dup(inf->x509))) //X509_ref_up requires >= v1.1
        log_msg(LGG_ERR, "Cannot add CA cert %d\n", i);
    }
  }

  SSL_set_SSL_CTX(cSSL, sslctx);
  tlsext_cb_arg->status = SSL_HIT;
  tlsext_cb_arg->sslctx = (void*)sslctx;

free_all:
#ifdef DEBUG
  printf("%s: sslctx %p\n", __FUNCTION__, (void*) sslctx);
#endif
  return rv;
}

static int read_socket(int fd, char **msg, SSL *cSSL) {
  if (*msg == NULL)
    *msg = malloc(CHAR_BUF_SIZE + 1);
  else
    *msg = realloc(*msg, CHAR_BUF_SIZE + 1);
  if (!(*msg)) {
    log_msg(LGG_ERR, "Out of memory. Cannot malloc receiver buffer.");
    return -1;
  }

  int i, rv, msg_len = 0;
  char *bufptr = *msg;

  for (i=1; i<=MAX_CHAR_BUF_LOTS;) { /* 128K max with CHAR_BUF_SIZE == 4K */
    if (!cSSL)
      rv = recv(fd, bufptr, CHAR_BUF_SIZE, 0);
    else {
      rv = SSL_read(cSSL, (char *)bufptr, CHAR_BUF_SIZE);
      TESTPRINT("SSL handshake. errno: %d rv: %d\n", SSL_get_error(cSSL, rv), rv);
    }
    msg_len += rv;
    if (rv < CHAR_BUF_SIZE)
      break;
    else {
      ++i;
      if (!(*msg = realloc(*msg, CHAR_BUF_SIZE * i + 1))) {
          log_msg(LGG_ERR, "Out of memory. Cannot realloc receiver buffer. Size: %d", CHAR_BUF_SIZE * i);
          return -1; /* start processing with whatever we received already */
      }
      log_msg(LGG_DEBUG, "Realloc receiver buffer. Size: %d", CHAR_BUF_SIZE * i);
      bufptr = *msg + CHAR_BUF_SIZE * (i - 1);
    }
  }
  return msg_len;
}

void* conn_handler( void *ptr )
{
  int argc = GLOBAL(g, argc);
  char **argv = GLOBAL(g, argv);
  const int new_fd = CONN_TLSTOR(ptr, new_fd);
  const int pipefd = GLOBAL(g, pipefd);
  const char* const stats_url = GLOBAL(g, stats_url);
  const char* const stats_text_url = GLOBAL(g, stats_text_url);
  const int do_204 = GLOBAL(g, do_204);
  const int do_redirect = GLOBAL(g, do_redirect);
#ifdef DEBUG
  const int warning_time = GLOBAL(g, warning_time);
#endif

  // NOTES:
  // - from here on, all exit points should be counted or at least logged
  // - exit() should not be called from the child process
  response_struct pipedata = {0};
  struct timeval timeout = {GLOBAL(g, select_timeout), 0};
  int rv = 0;
  char *buf = NULL, *bufptr = NULL;
  char *url = NULL;
  char* aspbuf = NULL;
  const char* response = httpnulltext;
  int rsize = sizeof httpnulltext - 1;
  char* version_string = NULL;
  char* stat_string = NULL;
  struct timespec start_time = {0, 0};
  int ssl = 0;
  int num_req = 0; // number of requests processed by this thread
  char *req_url = malloc(1024); // initial size only
  int req_len = 1023; // size of req_url less one
  #define HOST_LEN_MAX 80
  char host[HOST_LEN_MAX];
  char *post_buf = NULL;
  int post_buf_len = 0;

#ifdef DEBUG
  double time_msec = 0.0;
  int do_warning = (warning_time > 0);

  SET_LINE_NUMBER(__LINE__);

  // set up signal handling
  {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = child_signal_handler;
    sigemptyset(&sa.sa_mask);

    // set signal handler for termination
    if (sigaction(SIGTERM, &sa, NULL)) {
      log_msg(LGG_DEBUG, "sigaction(SIGTERM) reported error: %m");
    }

    // set signal handler for info
    sa.sa_flags = SA_RESTART; // prevent EINTR from interrupted library calls
    if (sigaction(SIGUSR2, &sa, NULL)) {
      log_msg(LGG_DEBUG, "sigaction(SIGUSR2) reported error: %m");
    }
  }

  SET_LINE_NUMBER(__LINE__);
#ifdef USE_PTHREAD
    printf("%s: tid = %d\n", __FUNCTION__, (int)pthread_self());
#endif
#endif

  // note the time
  get_time(&start_time);

  // the socket is connected, but we need to perform a check for incoming data
  // since we're using blocking checks, we first want to set a timeout
  if (setsockopt(new_fd, SOL_SOCKET, SO_RCVTIMEO, (char*)&timeout, sizeof(struct timeval)) < 0) {
    log_msg(LGG_DEBUG, "setsockopt(timeout) reported error: %m");
  }

  // select() is used because we want to give up after a specified timeout
  //  period, in case the client is messing with us

  SET_LINE_NUMBER(__LINE__);
  
  // determine https port or not
  char server_ip[INET6_ADDRSTRLEN] = {'\0'};
  {
    struct sockaddr_storage sin_addr;
    socklen_t sin_addr_len = sizeof(sin_addr);
    char port[NI_MAXSERV] = {'\0'};

    getsockname(new_fd, (struct sockaddr*)&sin_addr, &sin_addr_len);
    if(getnameinfo((struct sockaddr *)&sin_addr, sin_addr_len,
                   server_ip, sizeof server_ip,
                   port, sizeof port,
                   NI_NUMERICHOST | NI_NUMERICSERV) != 0)
      perror("getnameinfo");

    int i;
    for(i=0; i<num_tls_ports; i++)
      if(atoi(port) == tls_ports[i]) ssl = 1;
#ifdef DEBUG    
    printf("socket handler port number %s\n", port);

    char client_ip[INET6_ADDRSTRLEN]= {'\0'};
    getpeername(new_fd, (struct sockaddr*)&sin_addr, &sin_addr_len);
    if(getnameinfo((struct sockaddr *)&sin_addr, sin_addr_len, client_ip, \
            sizeof client_ip, NULL, 0, NI_NUMERICHOST) != 0)
      perror("getnameinfo");

    printf("socket handler Connection from %s\n", client_ip);
#endif
  }

  SSL_CTX *sslctx = NULL;
  SSL *cSSL = NULL;
  tlsext_cb_arg_struct tlsext_cb_arg = { tls_pem, NULL, server_ip, SSL_UNKNOWN, NULL };

  if(ssl){
    int ssl_err;
    sslctx = SSL_CTX_new(TLSv1_2_server_method());
    SSL_CTX_set_options(sslctx,
        SSL_MODE_RELEASE_BUFFERS |
        SSL_OP_NO_COMPRESSION |
        SSL_OP_CIPHER_SERVER_PREFERENCE);
    if (SSL_CTX_set_cipher_list(sslctx, PIXELSERV_CIPHER_LIST) <= 0)
      log_msg(LGG_DEBUG, "cipher_list cannot be set");
    SSL_CTX_set_tlsext_servername_callback(sslctx, tls_servername_cb);
    SSL_CTX_set_tlsext_servername_arg(sslctx, &tlsext_cb_arg);

    cSSL = SSL_new(sslctx);
    SSL_set_fd(cSSL, new_fd );
    ssl_err = SSL_accept(cSSL);
    pipedata.ssl = tlsext_cb_arg.status;
    if (ssl_err <= 0)
      goto event_loop; // will send default reply there
  }
  TIME_CHECK("SSL setup");

event_loop:
  pipedata.run_time = elapsed_time_msec(start_time);

  // enter event loop
  while(1) {
    response = httpnulltext;
    rsize = sizeof httpnulltext - 1;
    int wait_cnt = GLOBAL(g, http_keepalive) / GLOBAL(g, select_timeout);
    if (wait_cnt < 1) wait_cnt = 1;

    while (wait_cnt >=1) {
      errno = 0;
      rv = read_socket(new_fd, &buf, cSSL);
      if (rv > 0)
        break;
      if (rv == 0 && ssl) // client disconnects without sending any data
        pipedata.ssl = SSL_HIT_CLS;
      if (rv == 0 || (rv < 0 && (errno == ECONNRESET || errno == ETIMEDOUT)) || wait_cnt == 1) { 
        log_msg(LGG_DEBUG, "Exit recv loop socket:%d rv:%d errno:%d wait_cnt:%d num_req:%d\n",
             new_fd, rv, errno, wait_cnt, num_req);
        goto done_with_this_thread;
      }
      --wait_cnt;
    }
    get_time(&start_time);

    if (rv <= 0) {
      if (errno == ECONNRESET || rv == 0) {
        log_msg(LGG_DEBUG, "recv() ECONNRESET: %m");
        pipedata.status = FAIL_CLOSED;
      } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
        log_msg(LGG_DEBUG, "recv() EAGAIN: %m");
        pipedata.status = FAIL_TIMEOUT;
      } else {
        log_msg(LGG_DEBUG, "recv() error: %m");
        pipedata.status = FAIL_GENERAL;
      }
    } else {                    // got some data
      TIME_CHECK("initial recv()");
      buf[rv] = '\0';
      TESTPRINT("\nreceived %d bytes\n'%s'\n", rv, buf);
      pipedata.rx_total = rv;

#ifdef HEX_DUMP
      hex_dump(buf, rv);
#endif
      char *body = strstr(buf, "\r\n\r\n");
      int body_len = (body) ? (rv + buf - body) : 0;
      char *req = strtok_r(buf, "\r\n", &bufptr);
      if (log_get_verb() >= LGG_INFO) {
        req_url[0] = '\0';
        host[0] = '\0';
        if (req) {
          if (strlen(req) > req_len) {
            req_len = strlen(req);
            req_url = realloc(req_url, req_len + 1);
          }
          strcpy(req_url, req);

          // locate and copy Host
          char *tmpHost = strstr(bufptr, "Host: "); // e.g. "Host: abc.com"
          if (tmpHost) {
            tmpHost += strlen("Host: ");
            tmpHost = strtok(tmpHost, "\r\n");
            TESTPRINT("host log:%s socket:%d\n", tmpHost, new_fd);
            if (strlen(tmpHost) < HOST_LEN_MAX)
               strcpy(host, tmpHost);
            else
               strcpy(host, tmpHost + (strlen(tmpHost) - HOST_LEN_MAX) + 1);
            *(tmpHost + strlen(tmpHost)) = '\r';
          }
        }
      }
      char *method = strtok(req, " ");

      if (method == NULL) {
        log_msg(LGG_DEBUG, "client did not specify method");
      } else {
        TESTPRINT("method: '%s'\n", method);
        if (!strcmp(method, "OPTIONS")) {
          pipedata.status = SEND_OPTIONS;
          response = httpoptions;
          rsize = sizeof httpoptions - 1;
        } else if (!strcmp(method, "POST")) {
            int recv_len = 0;
            int length = 0;
            int post_buf_size = 0;
            int wait_cnt = 0;
            char *h = strstr(bufptr, "Content-Length: ");

            h += strlen("Content-Length: ");
            length = atoi(strtok(h, "\r\n"));

            log_msg(LGG_DEBUG, "POST socket: %d Content-Length: %d", new_fd, length);

            if (length < MAX_HTTP_POST_LEN)
              post_buf_size = length;
            else
              post_buf_size = MAX_HTTP_POST_LEN;
            post_buf = malloc(post_buf_size + 1);
            if (!post_buf) {
              log_msg(LGG_ERR, "Out of memory. Cannot malloc receiver buffer.");
              goto end_post;
            }
            post_buf[post_buf_size] = '\0';

            /* body points to "\r\n\r\n" */
            if (body && body_len > 4) {
              recv_len = body_len - 4;
              memcpy(post_buf, body + 4, recv_len);
              length -= recv_len;
              post_buf_size -= recv_len;
            }

            log_msg(LGG_DEBUG, "POST socket: %d expect length: %d\n", new_fd, length);

            wait_cnt = MAX_HTTP_POST_WAIT / GLOBAL(g, select_timeout);
            if (wait_cnt < 1) wait_cnt = 1;

            /* caputre POST content */
            for (; length > 0 && wait_cnt > 0;) {
              errno = 0;
              if (ssl) {
                rv = SSL_read(cSSL, (char *)(post_buf + recv_len), post_buf_size);
                TESTPRINT("SSL handshake. errno: %d rv: %d\n", SSL_get_error(cSSL, rv), rv);
              }else
                rv = recv(new_fd, post_buf + recv_len, post_buf_size, MSG_WAITALL);

              log_msg(LGG_DEBUG, "POST socket:%d recv length:%d; errno:%d", new_fd, rv, errno);

              if (rv > 0) {
                pipedata.rx_total += rv;
                length -= rv;
                if ((recv_len + rv) < MAX_HTTP_POST_LEN) {
                  recv_len += rv;
                  post_buf_size -= rv;
                  post_buf[recv_len] = '\0';
                } else {
                  if (length > CHAR_BUF_SIZE) {
                    /* discard bytes from 'MAX_HTTP_POST_LEN - CHAR_BUF_SIZE'
                    to 'Content-Length - CHAR_BUF_SIZE' */
                    recv_len += rv - CHAR_BUF_SIZE;
                    post_buf_size = CHAR_BUF_SIZE;
                  } else {
                      recv_len += rv - length;
                      post_buf_size = length;
                  }
                }
              } else
                --wait_cnt;
            }

          end_post:
            post_buf_len = recv_len;
            pipedata.status = SEND_POST;
            response = http204;
            rsize = sizeof http204 - 1;
        } else if (!strcmp(method, "GET")) {
          // send default from here, no matter what happens
          pipedata.status = DEFAULT_REPLY;
          // trim up to non path chars
          char *path = strtok(NULL, " ");//, " ?#;=");     // "?;#:*<>[]='\"\\,|!~()"
          if (path == NULL) {
            pipedata.status = SEND_NO_URL;
            log_msg(LGG_DEBUG, "client did not specify URL for GET request");
          } else if (!strncmp(path, "/log=", strlen("/log="))) {
            int v = atoi(path + strlen("/log="));
            if (v > LGG_DEBUG || v < 0)
              pipedata.status = SEND_BAD;
            else {
              pipedata.status = ACTION_LOG_VERB;
              pipedata.verb = v;
            }
          } else if (!strcmp(path, stats_url)) {
            pipedata.status = SEND_STATS;
            version_string = get_version(argc, argv);
            stat_string = get_stats(1, 0);
            rsize = asprintf(&aspbuf,
                             "%s%u%s%s%s<br>%s%s",
                             httpstats1,
                             (unsigned int)(statsbaselen + strlen(version_string) + 4 + strlen(stat_string)),
                             httpstats2,
                             httpstats3,
                             version_string,
                             stat_string,
                             httpstats4);
            free(version_string);
            free(stat_string);
            response = aspbuf;
          } else if (!strcmp(path, stats_text_url)) {
            pipedata.status = SEND_STATSTEXT;
            version_string = get_version(argc, argv);
            stat_string = get_stats(0, 1);
            rsize = asprintf(&aspbuf,
                             "%s%u%s%s\n%s%s",
                             txtstats1,
                             (unsigned int)(strlen(version_string) + 1 + strlen(stat_string) + 2),
                             txtstats2,
                             version_string,
                             stat_string,
                             txtstats3);
            free(version_string);
            free(stat_string);
            response = aspbuf;
          } else if (do_204 && !strcasecmp(path, "/generate_204")) {
            pipedata.status = SEND_204;
            response = http204;
            rsize = sizeof http204 - 1;
          } else {
            // pick out encoded urls (usually advert redirects)
            if (do_redirect && strcasestr(path, "=http")) {
              char *decoded = malloc(strlen(path)+1);
              urldecode(decoded, path);

              SET_LINE_NUMBER(__LINE__);

              // double decode
              urldecode(path, decoded);

              SET_LINE_NUMBER(__LINE__);

              free(decoded);
              url = strstr_last(path, "http://");
              if (url == NULL) {
                url = strstr_last(path, "https://");
              }
              // WORKAROUND: google analytics block - request bomb on pages with conversion callbacks (see in chrome)
              if (url) {
                char *tok = NULL;

                SET_LINE_NUMBER(__LINE__);

                for (tok = strtok_r(NULL, "\r\n", &bufptr); tok; tok = strtok_r(NULL, "\r\n", &bufptr)) {
                  char *hkey = strtok(tok, ":");
                  char *hvalue = strtok(NULL, "\r\n");
                  if (strstr(hkey, "Referer") && strstr(hvalue, url)) {
                    url = NULL;
                    TESTPRINT("Not redirecting likely callback URL: %s:%s\n", hkey, hvalue);
                    break;
                  }
                }

                SET_LINE_NUMBER(__LINE__);

              }
            }
            if (do_redirect && url) {
              pipedata.status = SEND_REDIRECT;
              rsize = asprintf(&aspbuf, httpredirect, url);
              response = aspbuf;
              TESTPRINT("Sending redirect: %s\n", url);
              url = NULL;
            } else {
              char *file = strrchr(strtok(path, "?#;="), '/');
              if (file == NULL) {
                pipedata.status = SEND_BAD_PATH;
                log_msg(LGG_DEBUG, "URL contains invalid file path %s", path);
              } else {
                TESTPRINT("file: '%s'\n", file);
                char *ext = strrchr(file, '.');
                if (ext == NULL) {
                  pipedata.status = SEND_NO_EXT;
                  log_msg(LGG_DEBUG, "no file extension %s from path %s", file, path);
                } else {
                  TESTPRINT("ext: '%s'\n", ext);
                  if (!strcasecmp(ext, ".gif")) {
                    TESTPRINT("Sending gif response\n");
                    pipedata.status = SEND_GIF;
                    response = httpnullpixel;
                    rsize = sizeof httpnullpixel - 1;
                  } else if (!strcasecmp(ext, ".png")) {
                    TESTPRINT("Sending png response\n");
                    pipedata.status = SEND_PNG;
                    response = httpnull_png;
                    rsize = sizeof httpnull_png - 1;
                  } else if (!strncasecmp(ext, ".jp", 3)) {
                    TESTPRINT("Sending jpg response\n");
                    pipedata.status = SEND_JPG;
                    response = httpnull_jpg;
                    rsize = sizeof httpnull_jpg - 1;
                  } else if (!strcasecmp(ext, ".swf")) {
                    TESTPRINT("Sending swf response\n");
                    pipedata.status = SEND_SWF;
                    response = httpnull_swf;
                    rsize = sizeof httpnull_swf - 1;
                  } else if (!strcasecmp(ext, ".ico")) {
                    TESTPRINT("Sending ico response\n");
                    pipedata.status = SEND_ICO;
                    response = httpnull_ico;
                    rsize = sizeof httpnull_ico - 1;
                  } else if (!strncasecmp(ext, ".js", 3)) {  // .jsx ?
                    pipedata.status = SEND_TXT;
                    TESTPRINT("Sending txt response\n");
                    response = httpnulltext;
                    rsize = sizeof httpnulltext - 1;
                  } else {
                    TESTPRINT("Sending ufe response\n");
                    pipedata.status = SEND_UNK_EXT;
                    log_msg(LOG_DEBUG, "unrecognized file extension %s from path %s", ext, path);
                  }
                }
              }
            }
          }
          // end of GET
        } else {
          if (!strcmp(method, "HEAD")) {
            // HEAD (TODO: send header of what the actual response type would be?)
            pipedata.status = SEND_HEAD;
          } else {
            // something else, possibly even non-HTTP
            log_msg(LGG_DEBUG, "Sending HTTP 501 response for unknown HTTP method: %s", method);
            pipedata.status = SEND_BAD;
          }
          TESTPRINT("Sending 501 response\n");
          response = http501;
          rsize = sizeof http501 - 1;
        }
      }
    }

    num_req++;

#ifdef DEBUG
    if (pipedata.status != FAIL_TIMEOUT)
      TIME_CHECK("response selection");
#endif

    // done processing socket connection; now handle selected result action
    if (pipedata.status == FAIL_GENERAL) {
      log_msg(LGG_DEBUG, "Client request processing completed with FAIL_GENERAL status");
    } else if (pipedata.status != FAIL_TIMEOUT && pipedata.status != FAIL_CLOSED) {
      SET_LINE_NUMBER(__LINE__);

      // only attempt to send response if we've chosen a valid response type
      if (ssl)
        rv = SSL_write(cSSL, response, rsize);
      else
        // this is currently a blocking call, so zero should not be returned
        rv = send(new_fd, response, rsize, MSG_NOSIGNAL);

      if (rv < 0) { // check for error message, but don't bother checking that all bytes sent
        if (errno == EPIPE || errno == ECONNRESET) {
          // client closed socket sometime after initial check
          log_msg(LGG_DEBUG, "attempt to send response for status=%d resulted in send() error: %m", pipedata.status);
          pipedata.status = FAIL_REPLY;
        } else {
          // some other error
          log_msg(LGG_ERR, "attempt to send response for status=%d resulted in send() error: %m", pipedata.status);
          pipedata.status = FAIL_GENERAL;
        }
      } else if (rv != rsize) {
        log_msg(LGG_ERR, "send() reported only %d of %d bytes sent; status=%d", rv, rsize, pipedata.status);
      }

      free(aspbuf);  // free memory allocated by asprintf() if any
      aspbuf = NULL;
      response = httpnullpixel;
    
      if (log_get_verb() >= LGG_INFO) {
        struct sockaddr_storage sin_addr;
        socklen_t sin_addr_len = sizeof(sin_addr);
        char client_ip[INET6_ADDRSTRLEN]= {'\0'};    

        getpeername(new_fd, (struct sockaddr*)&sin_addr, &sin_addr_len);
        if(getnameinfo((struct sockaddr *)&sin_addr,
                       sin_addr_len,
                       client_ip,
                       sizeof client_ip,
                       NULL, 0, NI_NUMERICHOST) != 0)
          perror("getnameinfo");
        log_xcs(LGG_INFO, client_ip, host, (tlsext_cb_arg.servername != NULL), req_url, post_buf, post_buf_len);
      }
      free(buf);
      buf = NULL;
      free(post_buf);
      post_buf = NULL;
      post_buf_len = 0;
    }

    /*** NOTE: pipedata.status should not be altered after this point ***/

    TIME_CHECK("response send()");

    // store time delta in milliseconds
    pipedata.run_time += elapsed_time_msec(start_time);

    SET_LINE_NUMBER(__LINE__);

    // write pipedata to pipe
    // note that the parent must not perform a blocking pipe read without checking
    // for available data, or else it may deadlock when we don't write anything
    rv = write(pipefd, &pipedata, sizeof(pipedata));
    pipedata.run_time = 0.0;
    if (rv < 0) {
      log_msg(LGG_ERR, "write() to pipe reported error: %m");
    } else if (rv == 0) {
      log_msg(LGG_ERR, "write() to pipe reported no data written and no error");
    } else if (rv != sizeof(pipedata)) {
      log_msg(LGG_ERR, "write() to pipe reported writing only %d bytes of expected %u", rv, (unsigned int)sizeof(pipedata));
    }

    TIME_CHECK("pipe write()");
    SET_LINE_NUMBER(__LINE__);
  } // event loop

done_with_this_thread:

  // signal the socket connection that we're done read-write
  if(ssl){
    SSL_set_shutdown(cSSL, SSL_SENT_SHUTDOWN | SSL_RECEIVED_SHUTDOWN);
#ifdef DEBUG
    printf("%s: sslctx %p\n", __FUNCTION__, (void*) tlsext_cb_arg.sslctx);
#endif
    SSL_free(cSSL);
    SSL_CTX_free((SSL_CTX*)tlsext_cb_arg.sslctx);
    SSL_CTX_free(sslctx);
  }
  
  if (shutdown(new_fd, SHUT_RDWR) < 0)
    log_msg(LGG_DEBUG, "shutdown() socket in thread or child process reported error: %m");
  if (close(new_fd) < 0)
    log_msg(LGG_DEBUG, "close() socket in thread or child process reported error: %m");

  TIME_CHECK("socket close()");
  
  // decrement number of service threads/processes by one before we exit
  // don't check for write errors
  memset(&pipedata, 0, sizeof(pipedata));
  pipedata.status = ACTION_DEC_KCC;
  pipedata.krq = num_req;
  rv = write(pipefd, &pipedata, sizeof(pipedata));

#ifndef USE_PTHREAD
  // child no longer needs write pipe, so close descriptor
  // this is probably redundant since we are about to exit() anyway
  if (close(pipefd) < 0)
    log_msg(LGG_DEBUG, "close() pipe in child process reported error: %m");

  TIME_CHECK("pipe close()");

  if (pipedata.status == FAIL_GENERAL)
    log_msg(LGG_ERR, "conn_handler exiting child process with FAIL_GENERAL status");
#endif

  free(buf);
  free(req_url);
  free(ptr);
  return NULL;
}
